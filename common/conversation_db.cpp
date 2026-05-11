/*
 * This program source code file is part of Trace, a KiCad fork.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <conversation_db.h>
#include <kiplatform/environment.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/datetime.h>
#include <sqlite3.h>

// Use Boost UUID directly instead of KIID to avoid race condition with g_createNilUuids.
// KIID uses a global flag that can be set to generate nil UUIDs during footprint library
// loading (KIID_NIL_SET_RESET), which causes session/conversation IDs to become
// 00000000-0000-0000-0000-000000000000 if generated concurrently.
// This dedicated generator is thread-safe and always generates random UUIDs.
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <mutex>

CONVERSATION_DB& CONVERSATION_DB::Instance()
{
    static CONVERSATION_DB instance;
    return instance;
}


CONVERSATION_DB::CONVERSATION_DB() : m_db( nullptr )
{
}


CONVERSATION_DB::~CONVERSATION_DB()
{
    Close();
}


wxString CONVERSATION_DB::GetDatabasePath() const
{
    // Use GetUserDataPath() which returns ~/Library/Application Support/kicad on macOS
    wxString dataPath = KIPLATFORM::ENV::GetUserDataPath();
    wxFileName dbPath( dataPath, wxT( "conversations.db" ) );
    
    // Ensure directory exists
    if( !wxFileName::DirExists( dataPath ) )
    {
        wxFileName::Mkdir( dataPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );
    }
    
    return dbPath.GetFullPath();
}


bool CONVERSATION_DB::Initialize()
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( m_db )
        return true;  // Already initialized
    
    wxString dbPath = GetDatabasePath();
    
    int rc = sqlite3_open( dbPath.utf8_str(), &m_db );
    if( rc != SQLITE_OK )
    {
        wxLogError( wxT( "Failed to open conversation database: %s" ), 
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        m_db = nullptr;
        return false;
    }
    
    // Enable foreign keys
    execute( "PRAGMA foreign_keys = ON;" );
    
    // Create tables
    if( !createTables() )
    {
        wxLogError( wxT( "Failed to create conversation tables" ) );
        Close();
        return false;
    }

    // Run one-shot migrations. Any new migration must bump user_version and be idempotent.
    if( !runMigrations() )
    {
        wxLogWarning( wxT( "Conversation DB migrations failed (continuing)" ) );
    }

    // Cleanup old conversations (older than 90 days) to keep the local database
    // size reasonable. Supabase retains the canonical copy with no retention.
    (void) DeleteOldConversations( 90 );

    return true;
}


void CONVERSATION_DB::Close()
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( m_db )
    {
        sqlite3_close( m_db );
        m_db = nullptr;
    }
}


bool CONVERSATION_DB::createTables()
{
    // Conversations table - matches Supabase schema
    const char* createConversations = R"SQL(
        CREATE TABLE IF NOT EXISTS conversations (
            id TEXT PRIMARY KEY,
            user_id TEXT,
            project_file_path TEXT,
            session_id TEXT,
            title TEXT,
            summary TEXT,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL,
            is_synced INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id);
        CREATE INDEX IF NOT EXISTS idx_conversations_updated ON conversations(updated_at DESC);
        CREATE INDEX IF NOT EXISTS idx_conversations_session ON conversations(session_id);
    )SQL";
    
    // Messages table - matches Supabase schema
    const char* createMessages = R"SQL(
        CREATE TABLE IF NOT EXISTS messages (
            id TEXT PRIMARY KEY,
            conversation_id TEXT NOT NULL,
            role TEXT NOT NULL,
            content TEXT NOT NULL,
            created_at TEXT NOT NULL,
            metadata TEXT,
            is_synced INTEGER DEFAULT 0,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id);
        CREATE INDEX IF NOT EXISTS idx_messages_created ON messages(conversation_id, created_at);
    )SQL";
    
    // Open tabs table - for persisting tab state across app restarts
    const char* createOpenTabs = R"SQL(
        CREATE TABLE IF NOT EXISTS open_tabs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id TEXT NOT NULL,
            tab_order INTEGER NOT NULL,
            is_active INTEGER DEFAULT 0,
            project_file_path TEXT,
            created_at TEXT NOT NULL,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_open_tabs_order ON open_tabs(tab_order);
        CREATE INDEX IF NOT EXISTS idx_open_tabs_project ON open_tabs(project_file_path);
    )SQL";
    
    if( !execute( createConversations ) )
        return false;
    
    if( !execute( createMessages ) )
        return false;
    
    if( !execute( createOpenTabs ) )
        return false;
    
    // Todos table - for AI task tracking per conversation
    const char* createTodos = R"SQL(
        CREATE TABLE IF NOT EXISTS todos (
            id TEXT NOT NULL,
            conversation_id TEXT NOT NULL,
            content TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL,
            PRIMARY KEY (id, conversation_id),
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_todos_conversation ON todos(conversation_id);
    )SQL";
    
    if( !execute( createTodos ) )
        return false;
    
    return true;
}


bool CONVERSATION_DB::runMigrations()
{
    // Read current schema version. SQLite defaults user_version to 0 on a fresh DB.
    int currentVersion = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if( sqlite3_prepare_v2( m_db, "PRAGMA user_version;", -1, &stmt, nullptr ) == SQLITE_OK )
        {
            if( sqlite3_step( stmt ) == SQLITE_ROW )
                currentVersion = sqlite3_column_int( stmt, 0 );
            sqlite3_finalize( stmt );
        }
    }

    // Migration 1: drop legacy open_tabs rows with NULL or empty project_file_path.
    //
    // Before the cross-project-bleed fix, SaveOpenTabs() did `DELETE ... OR project_file_path
    // IS NULL` which meant every save from any project wiped NULL-path rows - but it only
    // wiped them AFTER writing the current save, so legacy rows could survive for a long
    // time. Now that SaveOpenTabs no longer touches NULL rows, those legacy rows would stick
    // around forever. Clean them up once here.
    if( currentVersion < 1 )
    {
        (void) execute( "DELETE FROM open_tabs WHERE project_file_path IS NULL "
                        "OR project_file_path = '';" );
        if( !execute( "PRAGMA user_version = 1;" ) )
            return false;
        currentVersion = 1;
    }

    return true;
}


bool CONVERSATION_DB::execute( const char* aSql )
{
    if( !m_db )
        return false;
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec( m_db, aSql, nullptr, nullptr, &errMsg );
    
    if( rc != SQLITE_OK )
    {
        wxLogError( wxT( "SQL error: %s" ), wxString::FromUTF8( errMsg ) );
        sqlite3_free( errMsg );
        return false;
    }
    
    return true;
}


// Thread-safe UUID generator for conversation/session IDs.
// Uses its own dedicated random generator to avoid the race condition with KIID's
// global g_createNilUuids flag (which can be set during footprint library loading).
wxString CONVERSATION_DB::GenerateUUID()
{
    // Static mutex and generator - initialized once, thread-safe access
    static std::mutex s_uuidMutex;
    static boost::uuids::random_generator s_uuidGenerator;
    
    std::lock_guard<std::mutex> lock( s_uuidMutex );
    boost::uuids::uuid uuid = s_uuidGenerator();
    return wxString::FromUTF8( boost::uuids::to_string( uuid ) );
}


wxString CONVERSATION_DB::GetCurrentTimestamp()
{
    return wxDateTime::Now().FormatISOCombined( 'T' ) + wxT( "Z" );
}


// =========================================================================
// Conversation operations
// =========================================================================

std::optional<CONVERSATION> CONVERSATION_DB::CreateConversation( const wxString& aUserId,
                                                                  const wxString& aProjectFilePath,
                                                                  const wxString& aSessionId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return std::nullopt;
    
    CONVERSATION conv;
    conv.id = GenerateUUID();
    conv.user_id = aUserId;
    // Unsaved projects have no file path. Give each such conversation a unique
    // placeholder so that unsaved-project chats don't collide into a single
    // "Untitled" bucket that every new unsaved project would inherit.
    conv.project_file_path = aProjectFilePath.IsEmpty()
        ? ( wxT( "Untitled:" ) + conv.id )
        : aProjectFilePath;
    conv.session_id = aSessionId;
    conv.title = wxEmptyString;
    conv.summary = wxEmptyString;
    conv.created_at = GetCurrentTimestamp();
    conv.updated_at = conv.created_at;
    conv.is_synced = false;
    
    const char* sql = "INSERT INTO conversations "
                      "(id, user_id, project_file_path, session_id, title, summary, created_at, updated_at, is_synced) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0);";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
    {
        wxLogError( wxT( "Failed to prepare statement: %s" ), 
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        return std::nullopt;
    }
    
    sqlite3_bind_text( stmt, 1, conv.id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, conv.user_id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 3, conv.project_file_path.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 4, conv.session_id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 5, conv.title.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 6, conv.summary.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 7, conv.created_at.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 8, conv.updated_at.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    if( rc != SQLITE_DONE )
    {
        wxLogError( wxT( "Failed to create conversation: %s" ), 
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        return std::nullopt;
    }
    
    return conv;
}


std::optional<CONVERSATION> CONVERSATION_DB::ImportConversation( const wxString& aConversationId,
                                                                 const wxString& aUserId,
                                                                 const wxString& aProjectFilePath,
                                                                 const wxString& aSessionId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    if( aConversationId.IsEmpty() )
        return std::nullopt;

    if( !m_db && !Initialize() )
        return std::nullopt;

    // If we already have this conversation locally, just return it. This makes the
    // sync path idempotent and prevents accidental duplication.
    auto existing = LoadConversation( aConversationId );
    if( existing.has_value() )
        return existing;

    CONVERSATION conv;
    conv.id = aConversationId;
    conv.user_id = aUserId;
    // Preserve empty paths as empty rather than minting an "Untitled:<id>" placeholder:
    // for imported rows we want to mirror the server's project_file_path verbatim.
    conv.project_file_path = aProjectFilePath;
    conv.session_id = aSessionId;
    conv.title = wxEmptyString;
    conv.summary = wxEmptyString;
    conv.created_at = GetCurrentTimestamp();
    conv.updated_at = conv.created_at;
    conv.is_synced = true;

    // INSERT OR IGNORE: if another writer raced us and inserted first, we still return
    // the existing row below.
    const char* sql = "INSERT OR IGNORE INTO conversations "
                      "(id, user_id, project_file_path, session_id, title, summary, "
                      "created_at, updated_at, is_synced) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1);";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return std::nullopt;

    sqlite3_bind_text( stmt, 1, conv.id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, conv.user_id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 3, conv.project_file_path.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 4, conv.session_id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 5, conv.title.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 6, conv.summary.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 7, conv.created_at.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 8, conv.updated_at.utf8_str(), -1, SQLITE_TRANSIENT );

    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );

    if( rc != SQLITE_DONE )
        return std::nullopt;

    // Re-read the canonical row back from the DB (handles the race where another
    // writer inserted first).
    return LoadConversation( conv.id );
}


std::optional<CONVERSATION> CONVERSATION_DB::LoadConversation( const wxString& aConversationId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return std::nullopt;
    
    const char* sql = "SELECT id, user_id, project_file_path, session_id, title, summary, "
                      "created_at, updated_at, is_synced FROM conversations WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return std::nullopt;
    
    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    if( rc != SQLITE_ROW )
    {
        sqlite3_finalize( stmt );
        return std::nullopt;
    }
    
    CONVERSATION conv;
    conv.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
    conv.user_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
    conv.project_file_path = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
    conv.session_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
    conv.title = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
    conv.summary = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
    conv.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 6 ) ) );
    conv.updated_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 7 ) ) );
    conv.is_synced = sqlite3_column_int( stmt, 8 ) != 0;
    
    sqlite3_finalize( stmt );
    return conv;
}


std::vector<CONVERSATION> CONVERSATION_DB::ListConversations( const wxString& aUserId, int aLimit,
                                                              const wxString& aProjectFilePath )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    std::vector<CONVERSATION> conversations;
    
    if( !m_db && !Initialize() )
        return conversations;
    
    std::string sql = "SELECT id, user_id, project_file_path, session_id, title, summary, "
                      "created_at, updated_at, is_synced FROM conversations";

    std::vector<std::string> clauses;
    if( !aUserId.IsEmpty() )
        clauses.push_back( "(user_id = ? OR user_id IS NULL OR user_id = '')" );
    if( !aProjectFilePath.IsEmpty() )
        clauses.push_back( "project_file_path = ?" );

    for( size_t i = 0; i < clauses.size(); i++ )
        sql += ( i == 0 ? " WHERE " : " AND " ) + clauses[i];

    sql += " ORDER BY updated_at DESC LIMIT ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql.c_str(), -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return conversations;
    
    int paramIndex = 1;
    if( !aUserId.IsEmpty() )
        sqlite3_bind_text( stmt, paramIndex++, aUserId.utf8_str(), -1, SQLITE_TRANSIENT );
    if( !aProjectFilePath.IsEmpty() )
        sqlite3_bind_text( stmt, paramIndex++, aProjectFilePath.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_int( stmt, paramIndex, aLimit );
    
    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        CONVERSATION conv;
        conv.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
        conv.user_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        conv.project_file_path = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
        conv.session_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
        conv.title = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        conv.summary = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        conv.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 6 ) ) );
        conv.updated_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 7 ) ) );
        conv.is_synced = sqlite3_column_int( stmt, 8 ) != 0;
        conversations.push_back( conv );
    }
    
    sqlite3_finalize( stmt );
    return conversations;
}


bool CONVERSATION_DB::UpdateConversationTitle( const wxString& aConversationId, const wxString& aTitle )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    const char* sql = "UPDATE conversations SET title = ?, updated_at = ?, is_synced = 0 WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    wxString now = GetCurrentTimestamp();
    sqlite3_bind_text( stmt, 1, aTitle.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, now.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 3, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


bool CONVERSATION_DB::UpdateConversationSummary( const wxString& aConversationId, const wxString& aSummary )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    const char* sql = "UPDATE conversations SET summary = ?, updated_at = ?, is_synced = 0 WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    wxString now = GetCurrentTimestamp();
    sqlite3_bind_text( stmt, 1, aSummary.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, now.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 3, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


bool CONVERSATION_DB::DeleteConversation( const wxString& aConversationId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    // Messages will be deleted automatically due to ON DELETE CASCADE
    const char* sql = "DELETE FROM conversations WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


// =========================================================================
// Message operations
// =========================================================================

std::optional<MESSAGE> CONVERSATION_DB::SaveMessage( const wxString& aConversationId,
                                                      const wxString& aRole,
                                                      const wxString& aContent,
                                                      const wxString& aMetadata,
                                                      const wxString& aMessageId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return std::nullopt;
    
    MESSAGE msg;
    msg.id = aMessageId.IsEmpty() ? GenerateUUID() : aMessageId;
    msg.conversation_id = aConversationId;
    msg.role = aRole;
    msg.content = aContent;
    msg.created_at = GetCurrentTimestamp();
    msg.metadata = aMetadata;
    msg.is_synced = false;
    
    const char* sql = "INSERT INTO messages "
                      "(id, conversation_id, role, content, created_at, metadata, is_synced) "
                      "VALUES (?, ?, ?, ?, ?, ?, 0);";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
    {
        wxLogError( wxT( "Failed to prepare statement: %s" ), 
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        return std::nullopt;
    }
    
    sqlite3_bind_text( stmt, 1, msg.id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, msg.conversation_id.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 3, msg.role.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 4, msg.content.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 5, msg.created_at.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 6, msg.metadata.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    if( rc != SQLITE_DONE )
    {
        wxLogError( wxT( "Failed to save message: %s" ), 
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        return std::nullopt;
    }
    
    // Update conversation's updated_at timestamp
    const char* updateSql = "UPDATE conversations SET updated_at = ?, is_synced = 0 WHERE id = ?;";
    sqlite3_stmt* updateStmt;
    rc = sqlite3_prepare_v2( m_db, updateSql, -1, &updateStmt, nullptr );
    if( rc == SQLITE_OK )
    {
        sqlite3_bind_text( updateStmt, 1, msg.created_at.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( updateStmt, 2, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_step( updateStmt );
        sqlite3_finalize( updateStmt );
    }
    
    return msg;
}


bool CONVERSATION_DB::UpdateMessageMetadata( const wxString& aMessageId, const wxString& aMetadata )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    if( !m_db && !Initialize() )
        return false;

    const char* sql = "UPDATE messages SET metadata = ? WHERE id = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;

    sqlite3_bind_text( stmt, 1, aMetadata.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, aMessageId.utf8_str(), -1, SQLITE_TRANSIENT );

    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );

    return rc == SQLITE_DONE;
}


wxString CONVERSATION_DB::GetMessageMetadata( const wxString& aMessageId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    if( !m_db && !Initialize() )
        return wxEmptyString;

    const char* sql = "SELECT metadata FROM messages WHERE id = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return wxEmptyString;

    sqlite3_bind_text( stmt, 1, aMessageId.utf8_str(), -1, SQLITE_TRANSIENT );

    wxString result;
    if( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        const char* text = reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) );
        if( text )
            result = wxString::FromUTF8( text );
    }
    sqlite3_finalize( stmt );
    return result;
}


bool CONVERSATION_DB::DeleteMessage( const wxString& aConversationId, const wxString& aMessageId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    if( !m_db && !Initialize() )
        return false;

    const char* sql = "DELETE FROM messages WHERE id = ? AND conversation_id = ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;

    sqlite3_bind_text( stmt, 1, aMessageId.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_text( stmt, 2, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );

    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );

    return rc == SQLITE_DONE;
}


std::vector<MESSAGE> CONVERSATION_DB::LoadMessages( const wxString& aConversationId, int aLimit )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    std::vector<MESSAGE> messages;

    if( !m_db && !Initialize() )
        return messages;

    const char* sql = "SELECT id, conversation_id, role, content, created_at, metadata, is_synced "
                      "FROM messages WHERE conversation_id = ? ORDER BY created_at ASC LIMIT ?;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return messages;

    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    sqlite3_bind_int( stmt, 2, aLimit );

    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        MESSAGE msg;
        msg.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
        msg.conversation_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        msg.role = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
        msg.content = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
        msg.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        msg.metadata = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        msg.is_synced = sqlite3_column_int( stmt, 6 ) != 0;
        messages.push_back( msg );
    }

    sqlite3_finalize( stmt );
    return messages;
}


std::optional<MESSAGE> CONVERSATION_DB::GetLastMessage( const wxString& aConversationId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return std::nullopt;
    
    const char* sql = "SELECT id, conversation_id, role, content, created_at, metadata, is_synced "
                      "FROM messages WHERE conversation_id = ? ORDER BY created_at DESC LIMIT 1;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return std::nullopt;
    
    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    if( rc != SQLITE_ROW )
    {
        sqlite3_finalize( stmt );
        return std::nullopt;
    }
    
    MESSAGE msg;
    msg.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
    msg.conversation_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
    msg.role = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
    msg.content = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
    msg.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
    msg.metadata = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
    msg.is_synced = sqlite3_column_int( stmt, 6 ) != 0;
    
    sqlite3_finalize( stmt );
    return msg;
}


// =========================================================================
// Sync operations
// =========================================================================

bool CONVERSATION_DB::MarkConversationSynced( const wxString& aConversationId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    const char* sql = "UPDATE conversations SET is_synced = 1 WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


bool CONVERSATION_DB::MarkMessageSynced( const wxString& aMessageId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    const char* sql = "UPDATE messages SET is_synced = 1 WHERE id = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    sqlite3_bind_text( stmt, 1, aMessageId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


std::vector<CONVERSATION> CONVERSATION_DB::GetUnsyncedConversations()
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    std::vector<CONVERSATION> conversations;
    
    if( !m_db && !Initialize() )
        return conversations;
    
    const char* sql = "SELECT id, user_id, project_file_path, session_id, title, summary, "
                      "created_at, updated_at, is_synced FROM conversations "
                      "WHERE is_synced = 0;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return conversations;
    
    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        CONVERSATION conv;
        conv.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
        conv.user_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        conv.project_file_path = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
        conv.session_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
        conv.title = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        conv.summary = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        conv.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 6 ) ) );
        conv.updated_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 7 ) ) );
        conv.is_synced = false;
        conversations.push_back( conv );
    }
    
    sqlite3_finalize( stmt );
    return conversations;
}


std::vector<MESSAGE> CONVERSATION_DB::GetUnsyncedMessages()
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    std::vector<MESSAGE> messages;
    
    if( !m_db && !Initialize() )
        return messages;
    
    const char* sql = "SELECT id, conversation_id, role, content, created_at, metadata, is_synced "
                      "FROM messages WHERE is_synced = 0;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return messages;
    
    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        MESSAGE msg;
        msg.id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
        msg.conversation_id = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        msg.role = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
        msg.content = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
        msg.created_at = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        msg.metadata = wxString::FromUTF8( reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        msg.is_synced = false;
        messages.push_back( msg );
    }
    
    sqlite3_finalize( stmt );
    return messages;
}


int CONVERSATION_DB::SetUserIdForLocalConversations( const wxString& aUserId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return 0;
    
    const char* sql = "UPDATE conversations SET user_id = ?, is_synced = 0 "
                      "WHERE user_id IS NULL OR user_id = '';";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return 0;
    
    sqlite3_bind_text( stmt, 1, aUserId.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    int changes = sqlite3_changes( m_db );
    sqlite3_finalize( stmt );
    
    return ( rc == SQLITE_DONE ) ? changes : 0;
}


int CONVERSATION_DB::DeleteOldConversations( int aDays )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return 0;
    
    // Calculate cutoff date (aDays ago from now)
    wxDateTime cutoff = wxDateTime::Now() - wxDateSpan::Days( aDays );
    wxString cutoffStr = cutoff.FormatISOCombined( 'T' ) + wxT( "Z" );
    
    // Delete old conversations (messages will cascade delete)
    const char* sql = "DELETE FROM conversations WHERE updated_at < ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
    {
        wxLogError( wxT( "Failed to prepare delete statement: %s" ),
                    wxString::FromUTF8( sqlite3_errmsg( m_db ) ) );
        return 0;
    }
    
    sqlite3_bind_text( stmt, 1, cutoffStr.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    int changes = sqlite3_changes( m_db );
    sqlite3_finalize( stmt );
    
    if( rc == SQLITE_DONE && changes > 0 )
    {
        // Deleted old conversations
    }
    
    return ( rc == SQLITE_DONE ) ? changes : 0;
}


// =========================================================================
// Open tab persistence operations
// =========================================================================

bool CONVERSATION_DB::SaveOpenTabs( const std::vector<OPEN_TAB>& aTabs, const wxString& aProjectFilePath )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    // Use a transaction for atomicity
    if( !execute( "BEGIN TRANSACTION;" ) )
        return false;
    
    // Clear existing tabs for this project.
    //
    // NOTE: Previously this also deleted rows with NULL project_file_path, which had the
    // nasty side effect that a save from ANY project would wipe legacy tab rows that
    // legitimately had no path. Scope the delete strictly to the current project. Legacy
    // NULL-path rows are cleaned up once by the user_version=1 migration in Initialize().
    const char* deleteSql = "DELETE FROM open_tabs WHERE project_file_path = ?;";
    sqlite3_stmt* deleteStmt;
    int rc = sqlite3_prepare_v2( m_db, deleteSql, -1, &deleteStmt, nullptr );
    if( rc != SQLITE_OK )
    {
        execute( "ROLLBACK;" );
        return false;
    }
    
    sqlite3_bind_text( deleteStmt, 1, aProjectFilePath.utf8_str(), -1, SQLITE_TRANSIENT );
    rc = sqlite3_step( deleteStmt );
    sqlite3_finalize( deleteStmt );
    
    if( rc != SQLITE_DONE )
    {
        execute( "ROLLBACK;" );
        return false;
    }
    
    // Insert new tabs
    const char* insertSql = "INSERT INTO open_tabs "
                            "(conversation_id, tab_order, is_active, project_file_path, created_at) "
                            "VALUES (?, ?, ?, ?, ?);";
    
    wxString now = GetCurrentTimestamp();
    
    for( const auto& tab : aTabs )
    {
        sqlite3_stmt* insertStmt;
        rc = sqlite3_prepare_v2( m_db, insertSql, -1, &insertStmt, nullptr );
        if( rc != SQLITE_OK )
        {
            execute( "ROLLBACK;" );
            return false;
        }
        
        sqlite3_bind_text( insertStmt, 1, tab.conversation_id.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_int( insertStmt, 2, tab.tab_order );
        sqlite3_bind_int( insertStmt, 3, tab.is_active ? 1 : 0 );
        sqlite3_bind_text( insertStmt, 4, aProjectFilePath.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( insertStmt, 5, now.utf8_str(), -1, SQLITE_TRANSIENT );
        
        rc = sqlite3_step( insertStmt );
        sqlite3_finalize( insertStmt );
        
        if( rc != SQLITE_DONE )
        {
            execute( "ROLLBACK;" );
            return false;
        }
    }
    
    if( !execute( "COMMIT;" ) )
    {
        execute( "ROLLBACK;" );
        return false;
    }
    
    return true;
}


std::vector<OPEN_TAB> CONVERSATION_DB::LoadOpenTabs( const wxString& aProjectFilePath )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    std::vector<OPEN_TAB> tabs;
    
    if( !m_db && !Initialize() )
        return tabs;
    
    const char* sql = "SELECT id, conversation_id, tab_order, is_active, project_file_path, created_at "
                      "FROM open_tabs WHERE project_file_path = ? ORDER BY tab_order ASC;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return tabs;
    
    sqlite3_bind_text( stmt, 1, aProjectFilePath.utf8_str(), -1, SQLITE_TRANSIENT );
    
    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        OPEN_TAB tab;
        tab.id = sqlite3_column_int( stmt, 0 );
        tab.conversation_id = wxString::FromUTF8( 
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        tab.tab_order = sqlite3_column_int( stmt, 2 );
        tab.is_active = sqlite3_column_int( stmt, 3 ) != 0;
        tab.project_file_path = wxString::FromUTF8( 
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        tab.created_at = wxString::FromUTF8( 
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        tabs.push_back( tab );
    }
    
    sqlite3_finalize( stmt );
    
    return tabs;
}


bool CONVERSATION_DB::ClearOpenTabs( const wxString& aProjectFilePath )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    
    if( !m_db && !Initialize() )
        return false;
    
    const char* sql = "DELETE FROM open_tabs WHERE project_file_path = ?;";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return false;
    
    sqlite3_bind_text( stmt, 1, aProjectFilePath.utf8_str(), -1, SQLITE_TRANSIENT );
    
    rc = sqlite3_step( stmt );
    sqlite3_finalize( stmt );
    
    return rc == SQLITE_DONE;
}


// =========================================================================
// Todo operations
// =========================================================================

bool CONVERSATION_DB::SaveOrUpdateTodos( const wxString& aConversationId,
                                         const std::vector<TODO_ITEM>& aTodos,
                                         bool aMerge )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    if( !m_db && !Initialize() )
        return false;

    if( !execute( "BEGIN TRANSACTION;" ) )
        return false;

    if( !aMerge )
    {
        const char* deleteSql = "DELETE FROM todos WHERE conversation_id = ?;";
        sqlite3_stmt* deleteStmt;
        int rc = sqlite3_prepare_v2( m_db, deleteSql, -1, &deleteStmt, nullptr );
        if( rc != SQLITE_OK )
        {
            execute( "ROLLBACK;" );
            return false;
        }
        sqlite3_bind_text( deleteStmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
        rc = sqlite3_step( deleteStmt );
        sqlite3_finalize( deleteStmt );

        if( rc != SQLITE_DONE )
        {
            execute( "ROLLBACK;" );
            return false;
        }
    }

    const char* upsertSql =
        "INSERT INTO todos (id, conversation_id, content, status, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id, conversation_id) DO UPDATE SET "
        "content = COALESCE(excluded.content, todos.content), "
        "status = COALESCE(excluded.status, todos.status), "
        "updated_at = excluded.updated_at;";

    wxString now = GetCurrentTimestamp();

    for( const auto& todo : aTodos )
    {
        sqlite3_stmt* upsertStmt;
        int rc = sqlite3_prepare_v2( m_db, upsertSql, -1, &upsertStmt, nullptr );
        if( rc != SQLITE_OK )
        {
            execute( "ROLLBACK;" );
            return false;
        }

        sqlite3_bind_text( upsertStmt, 1, todo.id.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( upsertStmt, 2, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( upsertStmt, 3, todo.content.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( upsertStmt, 4, todo.status.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( upsertStmt, 5, now.utf8_str(), -1, SQLITE_TRANSIENT );
        sqlite3_bind_text( upsertStmt, 6, now.utf8_str(), -1, SQLITE_TRANSIENT );

        rc = sqlite3_step( upsertStmt );
        sqlite3_finalize( upsertStmt );

        if( rc != SQLITE_DONE )
        {
            execute( "ROLLBACK;" );
            return false;
        }
    }

    if( !execute( "COMMIT;" ) )
    {
        execute( "ROLLBACK;" );
        return false;
    }

    return true;
}


std::vector<TODO_ITEM> CONVERSATION_DB::LoadTodos( const wxString& aConversationId )
{
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    std::vector<TODO_ITEM> todos;

    if( !m_db && !Initialize() )
        return todos;

    const char* sql = "SELECT id, conversation_id, content, status, created_at, updated_at "
                      "FROM todos WHERE conversation_id = ? ORDER BY created_at ASC;";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2( m_db, sql, -1, &stmt, nullptr );
    if( rc != SQLITE_OK )
        return todos;

    sqlite3_bind_text( stmt, 1, aConversationId.utf8_str(), -1, SQLITE_TRANSIENT );

    while( sqlite3_step( stmt ) == SQLITE_ROW )
    {
        TODO_ITEM item;
        item.id = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 0 ) ) );
        item.conversation_id = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 1 ) ) );
        item.content = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 2 ) ) );
        item.status = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 3 ) ) );
        item.created_at = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 4 ) ) );
        item.updated_at = wxString::FromUTF8(
            reinterpret_cast<const char*>( sqlite3_column_text( stmt, 5 ) ) );
        todos.push_back( item );
    }

    sqlite3_finalize( stmt );
    return todos;
}

