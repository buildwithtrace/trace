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

#include <conversation_sync.h>
#include <conversation_db.h>
#include <auth/auth_manager.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <wx/log.h>
#include <json_common.h>
#include <amplitude_client.h>
#include <chrono>

CONVERSATION_SYNC& CONVERSATION_SYNC::Instance()
{
    static CONVERSATION_SYNC instance;
    return instance;
}


CONVERSATION_SYNC::CONVERSATION_SYNC() :
        m_running( false ),
        m_syncRequested( false ),
        m_supabaseUrl( wxEmptyString )
{
}


CONVERSATION_SYNC::~CONVERSATION_SYNC()
{
    Stop();
}


void CONVERSATION_SYNC::Start()
{
    if( m_running.load() )
        return;  // Already running

    m_running.store( true );
    m_thread = std::thread( &CONVERSATION_SYNC::syncLoop, this );
}


void CONVERSATION_SYNC::Stop()
{
    if( !m_running.load() )
        return;

    m_running.store( false );

    if( m_thread.joinable() )
        m_thread.join();
}


void CONVERSATION_SYNC::SyncNow()
{
    m_syncRequested.store( true );
}


void CONVERSATION_SYNC::syncLoop()
{
    while( m_running.load() )
    {
        // Wait for sync interval or immediate sync request
        for( int i = 0; i < SYNC_INTERVAL_SECONDS * 10 && m_running.load(); i++ )
        {
            if( m_syncRequested.load() )
            {
                m_syncRequested.store( false );
                break;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }

        if( !m_running.load() )
            break;

        // Only sync if authenticated
        if( AUTH_MANAGER::Instance().IsAuthenticated() )
        {
            doSync();
        }
    }
}


bool CONVERSATION_SYNC::doSync()
{
    std::lock_guard<std::mutex> lock( m_mutex );

    try
    {
        int convsSynced = pushConversations();
        int msgsSynced = pushMessages();

        if( convsSynced > 0 || msgsSynced > 0 )
        {
            wxLogDebug( wxT( "Synced %d conversations, %d messages to Supabase" ),
                        convsSynced, msgsSynced );

            AMPLITUDE_CLIENT::Instance().Track( "conversation_synced", {
                { "conversations_synced", convsSynced },
                { "messages_synced", msgsSynced },
            } );
        }

        return true;
    }
    catch( const std::exception& e )
    {
        wxLogWarning( wxT( "Conversation sync failed: %s" ), wxString::FromUTF8( e.what() ) );
        return false;
    }
}


int CONVERSATION_SYNC::pushConversations()
{
    if( m_supabaseUrl.IsEmpty() )
        return 0;

    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    std::vector<CONVERSATION> unsynced = db.GetUnsyncedConversations();

    if( unsynced.empty() )
        return 0;

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
        return 0;

    int synced = 0;

    for( const auto& conv : unsynced )
    {
        // Skip conversations without user_id (created while signed out)
        if( conv.user_id.IsEmpty() )
            continue;

        try
        {
            nlohmann::json payload;
            payload["id"] = conv.id.ToUTF8().data();
            payload["user_id"] = conv.user_id.ToUTF8().data();
            payload["project_file_path"] = conv.project_file_path.ToUTF8().data();
            payload["session_id"] = conv.session_id.ToUTF8().data();
            payload["title"] = conv.title.ToUTF8().data();
            payload["summary"] = conv.summary.ToUTF8().data();
            payload["created_at"] = conv.created_at.ToUTF8().data();
            payload["updated_at"] = conv.updated_at.ToUTF8().data();

            std::string url = m_supabaseUrl.ToStdString() + "/conversations";
            std::string body = payload.dump();

            KICAD_CURL_EASY curl;
            curl.SetURL( url );
            curl.SetPostFields( body );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "Authorization", ( "Bearer " + authToken ).ToStdString() );
            curl.SetHeader( "Prefer", "resolution=merge-duplicates" );

            int httpCode = curl.Perform();

            if( httpCode == 200 || httpCode == 201 )
            {
                db.MarkConversationSynced( conv.id );
                synced++;
            }
            else
            {
                wxLogWarning( wxT( "Failed to sync conversation %s: HTTP %d" ),
                              conv.id.Left( 8 ), httpCode );
            }
        }
        catch( const std::exception& e )
        {
            wxLogWarning( wxT( "Error syncing conversation %s: %s" ),
                          conv.id.Left( 8 ), wxString::FromUTF8( e.what() ) );
        }
    }

    return synced;
}


int CONVERSATION_SYNC::pushMessages()
{
    if( m_supabaseUrl.IsEmpty() )
        return 0;

    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    std::vector<MESSAGE> unsynced = db.GetUnsyncedMessages();

    if( unsynced.empty() )
        return 0;

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
        return 0;

    int synced = 0;

    for( const auto& msg : unsynced )
    {
        try
        {
            nlohmann::json payload;
            payload["id"] = msg.id.ToUTF8().data();
            payload["conversation_id"] = msg.conversation_id.ToUTF8().data();
            payload["role"] = msg.role.ToUTF8().data();
            payload["content"] = msg.content.ToUTF8().data();
            payload["created_at"] = msg.created_at.ToUTF8().data();

            if( !msg.metadata.IsEmpty() )
            {
                try
                {
                    payload["metadata"] = nlohmann::json::parse( msg.metadata.ToStdString() );
                }
                catch( ... )
                {
                    payload["metadata"] = nlohmann::json::object();
                }
            }
            else
            {
                payload["metadata"] = nlohmann::json::object();
            }

            std::string url = m_supabaseUrl.ToStdString() + "/messages";
            std::string body = payload.dump();

            KICAD_CURL_EASY curl;
            curl.SetURL( url );
            curl.SetPostFields( body );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "Authorization", ( "Bearer " + authToken ).ToStdString() );
            curl.SetHeader( "Prefer", "resolution=merge-duplicates" );

            int httpCode = curl.Perform();

            if( httpCode == 200 || httpCode == 201 )
            {
                db.MarkMessageSynced( msg.id );
                synced++;
            }
            else
            {
                wxLogWarning( wxT( "Failed to sync message %s: HTTP %d" ),
                              msg.id.Left( 8 ), httpCode );
            }
        }
        catch( const std::exception& e )
        {
            wxLogWarning( wxT( "Error syncing message %s: %s" ),
                          msg.id.Left( 8 ), wxString::FromUTF8( e.what() ) );
        }
    }

    return synced;
}


int CONVERSATION_SYNC::FetchFromSupabase()
{
    if( m_supabaseUrl.IsEmpty() )
        return 0;

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
        return 0;

    try
    {
        std::string url = m_supabaseUrl.ToStdString() + "/conversations?order=updated_at.desc&limit=50";

        KICAD_CURL_EASY curl;
        curl.SetURL( url );
        curl.SetHeader( "Authorization", ( "Bearer " + authToken ).ToStdString() );
        curl.SetHeader( "Accept", "application/json" );

        int httpCode = curl.Perform();

        if( httpCode != 200 )
        {
            wxLogWarning( wxT( "Failed to fetch conversations from Supabase: HTTP %d" ), httpCode );
            return 0;
        }

        std::string response = curl.GetBuffer();
        nlohmann::json data = nlohmann::json::parse( response );

        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        int              fetched = 0;

        for( const auto& item : data )
        {
            wxString convId = wxString::FromUTF8( item.value( "id", "" ) );

            // Check if conversation already exists locally
            auto existing = db.LoadConversation( convId );
            if( existing.has_value() )
            {
                // Update local with Supabase data (Supabase is source of truth)
                wxString title = wxString::FromUTF8( item.value( "title", "" ) );
                wxString summary = wxString::FromUTF8( item.value( "summary", "" ) );

                if( !title.IsEmpty() )
                    db.UpdateConversationTitle( convId, title );
                if( !summary.IsEmpty() )
                    db.UpdateConversationSummary( convId, summary );

                db.MarkConversationSynced( convId );
            }
            else
            {
                // Create local copy with the Supabase-assigned UUID so server and client
                // agree on the conversation id. Using CreateConversation here (as we used
                // to) minted a fresh local UUID, which meant the row the client later sent
                // to the backend could never be matched against the server-side row.
                wxString userId = wxString::FromUTF8( item.value( "user_id", "" ) );
                wxString filePath = wxString::FromUTF8( item.value( "project_file_path", "" ) );
                wxString sessionId = wxString::FromUTF8( item.value( "session_id", "" ) );

                auto newConv = db.ImportConversation( convId, userId, filePath, sessionId );
                if( newConv.has_value() )
                {
                    wxString title = wxString::FromUTF8( item.value( "title", "" ) );
                    wxString summary = wxString::FromUTF8( item.value( "summary", "" ) );

                    if( !title.IsEmpty() )
                        db.UpdateConversationTitle( newConv->id, title );
                    if( !summary.IsEmpty() )
                        db.UpdateConversationSummary( newConv->id, summary );

                    db.MarkConversationSynced( newConv->id );
                    fetched++;
                }
            }
        }

        wxLogDebug( wxT( "Fetched %d conversations from Supabase" ), fetched );
        return fetched;
    }
    catch( const std::exception& e )
    {
        wxLogWarning( wxT( "Error fetching from Supabase: %s" ), wxString::FromUTF8( e.what() ) );
        return 0;
    }
}

