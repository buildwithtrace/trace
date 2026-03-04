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

#ifndef CONVERSATION_DB_H
#define CONVERSATION_DB_H

#include <wx/string.h>
#include <kicommon.h>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>

// Forward declaration
struct sqlite3;

/**
 * Represents a single conversation in the local database.
 * Schema matches Supabase for seamless cloud sync.
 */
struct CONVERSATION
{
    wxString id;               ///< UUID as string (primary key)
    wxString user_id;          ///< User ID (may be empty if signed out)
    wxString project_file_path; ///< Which file this conversation is about
    wxString session_id;       ///< Current session ID for tab grouping
    wxString title;            ///< Auto-generated or user-set title
    wxString summary;          ///< LLM-generated summary for sliding window
    wxString created_at;       ///< ISO timestamp
    wxString updated_at;       ///< ISO timestamp
    bool     is_synced;        ///< True if synced to Supabase
};

/**
 * Represents a single message in a conversation.
 * Schema matches Supabase for seamless cloud sync.
 */
struct MESSAGE
{
    wxString id;               ///< UUID as string (primary key)
    wxString conversation_id;  ///< Foreign key to conversation
    wxString role;             ///< 'user' or 'assistant' or 'system'
    wxString content;          ///< Message text
    wxString created_at;       ///< ISO timestamp
    wxString metadata;         ///< JSON string (for tool calls, etc)
    bool     is_synced;        ///< True if synced to Supabase
};

/**
 * Represents an open tab for persistence across app restarts.
 * Stores which conversation tabs were open and their order.
 */
struct OPEN_TAB
{
    int      id;                 ///< Auto-increment ID (primary key)
    wxString conversation_id;    ///< Foreign key to conversation
    int      tab_order;          ///< Order of the tab (0-indexed)
    bool     is_active;          ///< True if this tab was the active one
    wxString project_file_path;  ///< Project file path for scoping tabs
    wxString created_at;         ///< ISO timestamp
};

/**
 * Local SQLite database for caching AI conversations.
 * 
 * Provides offline-first storage with background sync to Supabase.
 * Schema matches Supabase exactly for seamless synchronization.
 * 
 * Database location:
 * - macOS/Linux: ~/.trace/conversations.db
 * - Windows: %APPDATA%/Trace/conversations.db
 */
class KICOMMON_API CONVERSATION_DB
{
public:
    /**
     * Get the singleton instance of the conversation database.
     */
    static CONVERSATION_DB& Instance();

    /**
     * Initialize the database (creates tables if not exist).
     * Called automatically on first access.
     * @return True if initialization succeeded.
     */
    bool Initialize();

    /**
     * Close the database connection.
     */
    void Close();

    /**
     * Check if the database is open and ready.
     */
    bool IsOpen() const { return m_db != nullptr; }

    // =========================================================================
    // Conversation operations
    // =========================================================================

    /**
     * Create a new conversation.
     * @param aUserId User ID (can be empty if not signed in)
     * @param aProjectFilePath Path to the schematic/pcb file
     * @param aSessionId Session ID for tab grouping
     * @return The created conversation, or nullopt on error.
     */
    std::optional<CONVERSATION> CreateConversation( const wxString& aUserId,
                                                     const wxString& aProjectFilePath,
                                                     const wxString& aSessionId );

    /**
     * Load a conversation by ID.
     * @param aConversationId The conversation ID.
     * @return The conversation if found, or nullopt.
     */
    std::optional<CONVERSATION> LoadConversation( const wxString& aConversationId );

    /**
     * List recent conversations for a user, ordered by updated_at descending.
     * @param aUserId User ID to filter by (empty for all local conversations)
     * @param aLimit Maximum number of conversations to return.
     * @return Vector of conversations.
     */
    std::vector<CONVERSATION> ListConversations( const wxString& aUserId = wxEmptyString,
                                                  int aLimit = 50 );

    /**
     * Update a conversation's title.
     * @param aConversationId The conversation ID.
     * @param aTitle The new title.
     * @return True if update succeeded.
     */
    bool UpdateConversationTitle( const wxString& aConversationId, const wxString& aTitle );

    /**
     * Update a conversation's summary.
     * @param aConversationId The conversation ID.
     * @param aSummary The new summary.
     * @return True if update succeeded.
     */
    bool UpdateConversationSummary( const wxString& aConversationId, const wxString& aSummary );

    /**
     * Delete a conversation and all its messages.
     * @param aConversationId The conversation ID.
     * @return True if deletion succeeded.
     */
    bool DeleteConversation( const wxString& aConversationId );

    // =========================================================================
    // Message operations
    // =========================================================================

    /**
     * Save a message to a conversation.
     * @param aConversationId The conversation ID.
     * @param aRole Message role ('user', 'assistant', 'system')
     * @param aContent Message content.
     * @param aMetadata Optional JSON metadata string.
     * @return The created message, or nullopt on error.
     */
    std::optional<MESSAGE> SaveMessage( const wxString& aConversationId,
                                         const wxString& aRole,
                                         const wxString& aContent,
                                         const wxString& aMetadata = wxEmptyString );

    /**
     * Load all messages for a conversation, ordered by created_at ascending.
     * @param aConversationId The conversation ID.
     * @param aLimit Maximum number of messages to return.
     * @return Vector of messages.
     */
    std::vector<MESSAGE> LoadMessages( const wxString& aConversationId, int aLimit = 500 );

    /**
     * Get the last message for a conversation (for preview).
     * @param aConversationId The conversation ID.
     * @return The last message if found, or nullopt.
     */
    std::optional<MESSAGE> GetLastMessage( const wxString& aConversationId );

    // =========================================================================
    // Sync operations
    // =========================================================================

    /**
     * Mark a conversation as synced to Supabase.
     * @param aConversationId The conversation ID.
     * @return True if update succeeded.
     */
    bool MarkConversationSynced( const wxString& aConversationId );

    /**
     * Mark a message as synced to Supabase.
     * @param aMessageId The message ID.
     * @return True if update succeeded.
     */
    bool MarkMessageSynced( const wxString& aMessageId );

    /**
     * Get all unsynced conversations.
     * @return Vector of conversations where is_synced = 0.
     */
    std::vector<CONVERSATION> GetUnsyncedConversations();

    /**
     * Get all unsynced messages.
     * @return Vector of messages where is_synced = 0.
     */
    std::vector<MESSAGE> GetUnsyncedMessages();

    /**
     * Update user_id for all local conversations (called after sign-in).
     * @param aUserId The user ID to set.
     * @return Number of conversations updated.
     */
    int SetUserIdForLocalConversations( const wxString& aUserId );

    // =========================================================================
    // Open tab persistence operations
    // =========================================================================

    /**
     * Save the current open tabs state.
     * Clears existing tabs and saves new state in a transaction.
     * @param aTabs Vector of open tabs to save.
     * @param aProjectFilePath Project file path for scoping.
     * @return True if save succeeded.
     */
    bool SaveOpenTabs( const std::vector<OPEN_TAB>& aTabs, const wxString& aProjectFilePath );

    /**
     * Load open tabs for a project.
     * @param aProjectFilePath Project file path to filter by.
     * @return Vector of open tabs ordered by tab_order.
     */
    std::vector<OPEN_TAB> LoadOpenTabs( const wxString& aProjectFilePath );

    /**
     * Clear all open tabs for a project.
     * @param aProjectFilePath Project file path to filter by.
     * @return True if clear succeeded.
     */
    bool ClearOpenTabs( const wxString& aProjectFilePath );

    // =========================================================================
    // Cleanup operations
    // =========================================================================

    /**
     * Delete conversations older than specified number of days.
     * This helps keep the local database clean and respects user privacy.
     * @param aDays Number of days to keep (conversations older than this are deleted).
     * @return Number of conversations deleted.
     */
    int DeleteOldConversations( int aDays = 7 );

    // =========================================================================
    // Utility
    // =========================================================================

    /**
     * Get the path to the database file.
     */
    wxString GetDatabasePath() const;

    /**
     * Generate a new UUID string.
     */
    static wxString GenerateUUID();

    /**
     * Get current ISO timestamp string.
     */
    static wxString GetCurrentTimestamp();

private:
    CONVERSATION_DB();
    ~CONVERSATION_DB();

    // Prevent copying
    CONVERSATION_DB( const CONVERSATION_DB& ) = delete;
    CONVERSATION_DB& operator=( const CONVERSATION_DB& ) = delete;

    /**
     * Create the database tables if they don't exist.
     */
    bool createTables();

    /**
     * Execute a SQL statement that doesn't return results.
     * @param aSql The SQL to execute.
     * @return True if execution succeeded.
     */
    bool execute( const char* aSql );

    sqlite3* m_db;                        ///< SQLite database handle
    mutable std::recursive_mutex m_mutex; ///< Thread safety for concurrent access (recursive for nested calls)
};

#endif // CONVERSATION_DB_H

