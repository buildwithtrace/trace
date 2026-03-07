/*
 * This program source code file is part of Trace, an AI-native PCB design application.
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

#include "ai_chat_panel_base.h"
#include <amplitude_client.h>
#include <widgets/chat_webview_panel.h>
#include <wx/sizer.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/ffile.h>
#include <wx/settings.h>
#include <wx/datetime.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <kiplatform/secrets.h>
#include <kiplatform/ui.h>

#include <paths.h>
#include <python_manager.h>
#include <kiway_player.h>
#include <wildcards_and_files_ext.h>
#include <json_common.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <functional>
#include <memory>
#include <future>
#include <algorithm>

#include <wx/utils.h>
#include <class_draw_panel_gal.h>
#include <eda_draw_frame.h>
#include <auth/auth_manager.h>
#include <confirm.h>
#include <ui_events.h>
#include <ai_backend_client.h>
#include <ai_tool_executor.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>
#include <conversation_db.h>
#include <conversation_sync.h>

// Tab management IDs
enum
{
    ID_TAB_NEW = wxID_HIGHEST + 1,
    ID_TAB_SELECT_BASE,
    ID_TAB_CLOSE_BASE = ID_TAB_SELECT_BASE + 100
};


// MODE_DROPDOWN_BUTTON removed - mode selection is now handled by React UI
// Mode state is tracked by AI_CHAT_PANEL_BASE::m_currentMode

// Native UI classes (STYLED_TOGGLE_BUTTON, TAB_CONTENT_PANEL, STYLED_MULTILINE_TEXTCTRL)
// removed - all UI is now handled by the webview (CHAT_WEBVIEW_PANEL + React)

// Forward declarations for static helpers used before their definitions
static nlohmann::json buildAssistantMetadataJson( const TAB_DATA* aTab,
                                                   const wxString& aMode,
                                                   const wxString& aAppType );
static bool isTimestampWithinMinutes( const wxString& aIsoTimestamp, int aMaxMinutes );


AI_CHAT_PANEL_BASE::AI_CHAT_PANEL_BASE( wxWindow* aParent, EDA_DRAW_FRAME* aFrame ) :
        wxPanel( aParent, wxID_ANY ),
        m_frame( aFrame ),
        m_currentMode( AI_MODE::AGENT ),
#ifdef TRACE_BACKEND_URL
        m_backendUrl( wxT( TRACE_BACKEND_URL ) ),  // Use build-time configured URL (includes /api/v2)
#else
        m_backendUrl( wxT( "http://localhost:8000/api/v2" ) ),  // Fallback
#endif
        m_requestInProgress( false ),
        m_sessionId( wxEmptyString ),
        m_conversationId( wxEmptyString ),
        m_streamingFlushTimer( nullptr ),
        m_aiEditInProgress( false ),
        m_aiEditStateCaptured( false ),
        m_reloadDebounceTimer( nullptr ),
        m_reloadPending( false ),
        m_reloadInProgress( false ),
        m_streamingBatchTimer( nullptr ),
        m_batchUpdatePending( false ),
        m_panelAlive( std::make_shared<std::atomic<bool>>( true ) ),
        m_isDestroying( false ),
        m_streamingTabIndex( -1 ),
        m_currentTabIndex( -1 ),
        m_expectedMode( AI_MODE::ASK ),
        m_awaitingModeConfirmation( false ),
        m_planModeExecuting( false )
{
    // Note: Backend clients are per-tab (created in createNewTab/loadPersistedTabs)

    // Create streaming flush timer
    m_streamingFlushTimer = new wxTimer( this, wxID_ANY );
    Bind( wxEVT_TIMER, &AI_CHAT_PANEL_BASE::onStreamingFlushTimer, this, m_streamingFlushTimer->GetId() );

    // Set up delete confirmation callback (will be applied to all tab tool executors)
    auto panelAlive = m_panelAlive;
    AI_CHAT_PANEL_BASE* self = this;  // Capture for CallAfter in lambda
    m_confirmationCallback = [self, panelAlive]( const std::string& filename ) -> std::future<bool>
        {
            auto promise = std::make_shared<std::promise<bool>>();
            std::future<bool> future = promise->get_future();

            self->CallAfter( [self, panelAlive, filename, promise]()
            {
            if( !panelAlive->load() || self->m_isDestroying.load() || !self->m_frame )
                {
                    promise->set_value( false );
                    return;
                }

                wxString message = wxString::Format(
                    _( "Are you sure you want to delete the file '%s'?\n\n"
                       "This will also delete the corresponding KiCad file." ),
                    filename );
                
                wxMessageDialog dlg( self->m_frame, message, _( "Confirm File Deletion" ),
                                    wxYES_NO | wxICON_QUESTION | wxYES_DEFAULT );
                
                promise->set_value( dlg.ShowModal() == wxID_YES );
            } );

            return future;
    };

    // Try to restore session from keychain before building UI
    // Skip if already authenticated (TryRestoreSession was called at app init)
    if( !AUTH_MANAGER::Instance().IsAuthenticated() )
        AUTH_MANAGER::Instance().TryRestoreSession();

    buildUI();

    // Listen for auth state changes to update UI (enable/disable controls)
    AUTH_MANAGER::Instance().Bind( EVT_AUTH_STATE_CHANGED, &AI_CHAT_PANEL_BASE::onAuthStateChanged, this );

    // Initialize local conversation database and load last conversation
    panelAlive = m_panelAlive;
    
    // Join any previous sync thread
    if( m_syncThread && m_syncThread->joinable() )
        m_syncThread->join();

    m_syncThread = std::make_unique<std::thread>( [panelAlive]() {
        try
        {
            CONVERSATION_DB::Instance().Initialize();
        }
        catch( const std::exception& e )
        {
            wxLogError( "[AI_CHAT] Exception initializing conversation DB: %s", e.what() );
        }
        catch( ... )
        {
            wxLogError( "[AI_CHAT] Unknown exception initializing conversation DB" );
        }
        
        // Note: loadLastConversation is now only called from the tab restoration logic
        // if loadPersistedTabs() returns false (no tabs to restore)
        
        // Silence unused variable warning
        (void)panelAlive;
    } );
}


AI_CHAT_PANEL_BASE::~AI_CHAT_PANEL_BASE()
{
    // Save open tabs state before destroying (for restoration on next launch)
    saveOpenTabs();
    
    // Stop and cleanup streaming flush timer
    if( m_streamingFlushTimer )
    {
        m_streamingFlushTimer->Stop();
        delete m_streamingFlushTimer;
        m_streamingFlushTimer = nullptr;
    }

    // Stop and cleanup reload debounce timer
    if( m_reloadDebounceTimer )
    {
        m_reloadDebounceTimer->Stop();
        delete m_reloadDebounceTimer;
        m_reloadDebounceTimer = nullptr;
    }

    // Stop and cleanup streaming batch timer
    if( m_streamingBatchTimer )
    {
        m_streamingBatchTimer->Stop();
        delete m_streamingBatchTimer;
        m_streamingBatchTimer = nullptr;
    }

    // Stop and cleanup per-tab idle status timers
    for( auto& tab : m_tabs )
    {
        if( tab.idleStatusTimer )
        {
            tab.idleStatusTimer->Stop();
            delete tab.idleStatusTimer;
            tab.idleStatusTimer = nullptr;
        }
    }

    // Unbind auth state change handler to prevent crashes
    AUTH_MANAGER::Instance().Unbind( EVT_AUTH_STATE_CHANGED, &AI_CHAT_PANEL_BASE::onAuthStateChanged, this );

    // CRITICAL: Set destruction flag FIRST to prevent CallAfter() callbacks from executing
    // This must happen before stopping clients to ensure callbacks see the flag
    m_isDestroying.store( true );

    // CRITICAL: Stop ALL per-tab backend clients and threads
    // Each tab has its own backend client for true parallel execution
    for( auto& tab : m_tabs )
    {
        tab.stopRequested.store( true );
        if( tab.backendClient )
            tab.backendClient->StopStream();
    }

    // Signal background threads that panel is being destroyed
    // Do this AFTER setting m_isDestroying and stopping clients to ensure proper shutdown order
    if( m_panelAlive )
        m_panelAlive->store( false );

    // Cleanup per-tab request threads (with timeout to prevent blocking)
    for( auto& tab : m_tabs )
    {
        if( tab.requestThread && tab.requestThread->joinable() )
            {
            // Give thread a brief chance to finish, then detach
            auto start = std::chrono::steady_clock::now();
            while( tab.isStreaming.load() &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start ).count() < 500 )
            {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
        
            // Detach to prevent blocking - thread will see panelAlive=false and exit
            if( tab.requestThread->joinable() )
                tab.requestThread->detach();
        }
    }

    // Join sync thread with timeout
    if( m_syncThread && m_syncThread->joinable() )
    {
        auto start = std::chrono::steady_clock::now();
        constexpr int timeoutMs = 2000; // 2 seconds for DB operations
        
        while( m_syncThread->joinable() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start ).count() < timeoutMs )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
        
        if( m_syncThread->joinable() )
            m_syncThread->join();
    }


    // Join conversation load thread with timeout
    if( m_conversationLoadThread && m_conversationLoadThread->joinable() )
    {
        auto start = std::chrono::steady_clock::now();
        constexpr int timeoutMs = 2000; // 2 seconds for DB operations
        
        while( m_conversationLoadThread->joinable() &&
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start ).count() < timeoutMs )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
        
        if( m_conversationLoadThread->joinable() )
            m_conversationLoadThread->join();
    }
}


bool AI_CHAT_PANEL_BASE::isAnyTabStreaming() const
{
    for( const auto& tab : m_tabs )
    {
        if( tab.isStreaming.load() )
            return true;
    }
    return false;
}


bool AI_CHAT_PANEL_BASE::claimFileOwnership( const wxString& aFilePath, int aTabIndex )
{
    std::lock_guard<std::mutex> lock( m_fileOwnershipMutex );
    
    // Soft ownership - just track which tab is working on the file
    // No blocking - the tool executor handles conflicts via file locks and hash checking
    auto it = m_fileOwnership.find( aFilePath );
    if( it != m_fileOwnership.end() && it->second != aTabIndex && it->second != -1 )
    {
        // Tab taking over file from another tab (soft ownership)
    }
    
    m_fileOwnership[aFilePath] = aTabIndex;
    return true;  // Always succeeds - soft ownership doesn't block
}


void AI_CHAT_PANEL_BASE::releaseFileOwnership( int aTabIndex )
{
    std::lock_guard<std::mutex> lock( m_fileOwnershipMutex );
    
    // Remove all file ownerships for this tab
    for( auto it = m_fileOwnership.begin(); it != m_fileOwnership.end(); )
    {
        if( it->second == aTabIndex )
        {
            it = m_fileOwnership.erase( it );
        }
        else
        {
            ++it;
        }
    }
}


int AI_CHAT_PANEL_BASE::getFileOwner( const wxString& aFilePath )
{
    std::lock_guard<std::mutex> lock( m_fileOwnershipMutex );
    
    auto it = m_fileOwnership.find( aFilePath );
    if( it != m_fileOwnership.end() )
        return it->second;
    
    return -1;  // No owner
}


void AI_CHAT_PANEL_BASE::markFileModifiedByTab( const wxString& aFilePath, int aTabIndex )
{
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    TAB_DATA& tab = m_tabs[aTabIndex];
    tab.fileModifiedDuringStream.store( true );
    tab.modifiedFiles.insert( aFilePath );
}


bool AI_CHAT_PANEL_BASE::isAnyTabStopRequested() const
{
    for( const auto& tab : m_tabs )
    {
        if( tab.stopRequested.load() )
            return true;
    }
    return false;
}


void AI_CHAT_PANEL_BASE::buildUI()
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Create the WebView-based chat panel (tabs + messages + input all in React)
    // Auth header, quota banner, and all other UI are now handled by React
    m_chatWebview = new CHAT_WEBVIEW_PANEL( this, wxID_ANY );
    m_chatWebview->SetMinSize( wxSize( 250, 400 ) );
    mainSizer->Add( m_chatWebview, 1, wxEXPAND | wxALL, 0 );

    // Initialize the chat UI
    m_chatWebview->InitializeChatUI();

    // Set up callbacks from React to C++
    m_chatWebview->SetSendMessageCallback( [this]( const wxString& content,
                                                     const std::vector<ChatAttachment>& attachments ) {
        // Send message directly without going through native input box
        sendMessageFromWebview( content, attachments );
    });

    m_chatWebview->SetStopRequestCallback( [this]( const wxString& tabId ) {
        int tabIndex = resolveTabIndex( tabId );
        onStopRequestForTab( tabIndex );
    });

    m_chatWebview->SetModeChangedCallback( [this]( const wxString& mode ) {
        if( mode == wxT( "ask" ) )
            m_currentMode = AI_MODE::ASK;
        else if( mode == wxT( "plan" ) )
            m_currentMode = AI_MODE::PLAN;
        else if( mode == wxT( "agent" ) )
            m_currentMode = AI_MODE::AGENT;
    });

    m_chatWebview->SetTabSelectedCallback( [this]( const wxString& tabId ) {
        // Find tab index by conversation ID
        for( int i = 0; i < static_cast<int>( m_tabs.size() ); i++ )
        {
            if( m_tabs[i].conversationId == tabId )
            {
                switchToTab( i );
                break;
            }
        }
    });

    m_chatWebview->SetTabCloseCallback( [this]( const wxString& tabId ) {
        // Find tab index by conversation ID
        for( int i = 0; i < static_cast<int>( m_tabs.size() ); i++ )
        {
            if( m_tabs[i].conversationId == tabId )
            {
                // Create synthetic event for existing handler
                wxCommandEvent evt( wxEVT_BUTTON, ID_TAB_CLOSE_BASE + i );
                evt.SetInt( i );  // Set the tab index
                onTabClose( evt );
                break;
            }
        }
    });

    m_chatWebview->SetNewTabCallback( [this]() {
        wxCommandEvent evt( wxEVT_BUTTON, ID_TAB_NEW );
        onNewTab( evt );
    });

    m_chatWebview->SetHistoryClickCallback( [this]() {
        wxCommandEvent evt;
        onHistorySelect( evt );
    });

    m_chatWebview->SetHistorySelectCallback( [this]( const wxString& conversationId ) {
        loadConversationToTabAsync( conversationId );
    });

    // Plan mode callbacks from webview (all now receive tabId for correct routing)
    m_chatWebview->SetPlanApproveCallback( [this]( const wxString& tabId ) {
        wxLogDebug( wxT( "Plan approve callback triggered from webview" ) );
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex >= 0 && tabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->HidePlanApproval();
            m_chatWebview->HidePhaseIndicator();
            sendPlanAction( "approve", "", tabIndex );
        }
    });

    m_chatWebview->SetPlanMoreResearchCallback( [this]( const wxString& feedback, const wxString& tabId ) {
        wxLogDebug( wxT( "Plan more research callback triggered from webview: %s" ), feedback );
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex >= 0 && tabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->HidePlanApproval();
            m_chatWebview->HidePhaseIndicator();
            sendPlanAction( "more_research", feedback.ToStdString(), tabIndex );
        }
    });

    m_chatWebview->SetPlanAdjustCallback( [this]( const wxString& feedback, const wxString& tabId ) {
        wxLogDebug( wxT( "Plan adjust callback triggered from webview: %s" ), feedback );
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex >= 0 && tabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->HidePlanApproval();
            m_chatWebview->HidePhaseIndicator();
            sendPlanAction( "adjust_plan", feedback.ToStdString(), tabIndex );
        }
    });

    m_chatWebview->SetPlanCancelCallback( [this]( const wxString& tabId ) {
        wxLogDebug( wxT( "Plan cancel callback triggered from webview" ) );
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex >= 0 && tabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->HidePlanApproval();
            m_chatWebview->HidePhaseIndicator();
            sendPlanAction( "cancel", "", tabIndex );
        }
    });

    m_chatWebview->SetPlanEditSaveCallback( [this]( const wxString& markdown, const wxString& tabId ) {
        wxLogDebug( wxT( "Plan edit+save callback triggered from webview" ) );
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex >= 0 && tabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->HidePlanApproval();
            m_chatWebview->HidePhaseIndicator();
            sendPlanAction( "approve", markdown.ToStdString(), tabIndex );
        }
    });

    m_chatWebview->SetPlanSaveToWorkspaceCallback( [this]( const wxString& markdown ) {
        wxString savedPath = savePlanToProjectFolder( markdown );
        if( !savedPath.IsEmpty() && m_chatWebview )
        {
            wxString statusMsg = wxString::Format( wxT( "Plan saved to %s" ), savedPath );
            m_chatWebview->ShowStatus( statusMsg );
        }
        else if( m_chatWebview )
        {
            m_chatWebview->ShowStatus( wxT( "Could not save plan. Is a project open?" ) );
        }
    });

    // Auth/payment callbacks from webview
    m_chatWebview->SetSignInClickCallback( [this]() {
        try
        {
            if( !AUTH_MANAGER::Instance().IsAuthenticated() )
            {
                AUTH_MANAGER::Instance().StartLogin();
            }
            updateAuthUI();
        }
        catch( ... )
        {
            if( m_chatWebview )
                m_chatWebview->ShowStatus( wxT( "Authentication error. Please try again." ) );
        }
    });

    m_chatWebview->SetUpgradeClickCallback( []() {
        wxLaunchDefaultBrowser( wxT( "https://buildwithtrace.com/dashboard/pricing" ) );
    });

    m_chatWebview->SetEditMessageCallback( [this]( const wxString& messageId, const wxString& newContent, const wxString& tabId ) {
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex < 0 || tabIndex >= static_cast<int>( m_tabs.size() ) )
            return;
        TAB_DATA& tab = m_tabs[tabIndex];
        if( tab.isStreaming.load() )
            return;

        wxString convId = tab.conversationId;
        if( convId.IsEmpty() )
            return;

        if( isOtherTabStreamingToCurrentFile() )
        {
            m_chatWebview->ShowStatus(
                wxT( "Cannot edit: another tab is actively editing this file. "
                     "Wait for it to finish." ) );
            return;
        }

        if( convId.StartsWith( wxT( "temp_" ) ) )
        {
            m_chatWebview->ClearMessages();
            sendMessageFromWebview( newContent );
            return;
        }

        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        auto messages = db.LoadMessages( convId );

        bool found = false;
        for( size_t i = 0; i < messages.size(); i++ )
        {
            if( found )
            {
                db.DeleteMessage( convId, messages[i].id );
            }
            else if( messages[i].id == messageId )
            {
                found = true;
                db.DeleteMessage( convId, messageId );
            }
        }

        if( tab.backendClient )
            tab.backendClient->SetEditMessageId( messageId.ToStdString() );

        if( found )
        {
            reloadMessagesToWebview( convId );
        }
        else
        {
            wxLogWarning( "Edit: message %s not found in SQLite, clearing webview", messageId );
            m_chatWebview->ClearMessages();
        }
        sendMessageFromWebview( newContent );
    });

    m_chatWebview->SetUndoToMessageCallback( [this]( const wxString& messageId, const wxString& tabId ) {
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex < 0 || tabIndex >= static_cast<int>( m_tabs.size() ) )
            return;
        TAB_DATA& tab = m_tabs[tabIndex];
        if( tab.isStreaming.load() )
            return;

        wxString convId = tab.conversationId;
        if( convId.IsEmpty() )
            return;

        if( isOtherTabStreamingToCurrentFile() )
        {
            m_chatWebview->ShowStatus(
                wxT( "Cannot undo: another tab is actively editing this file. "
                     "Wait for it to finish." ) );
            return;
        }

        if( convId.StartsWith( wxT( "temp_" ) ) )
        {
            m_chatWebview->ClearMessages();
            return;
        }

        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        auto messages = db.LoadMessages( convId );

        bool found = false;
        wxString versionToRestore;
        for( size_t i = 0; i < messages.size(); i++ )
        {
            if( messages[i].id == messageId )
            {
                found = true;
                for( int k = static_cast<int>( i ) - 1; k >= 0; k-- )
                {
                    if( messages[k].role == wxT( "assistant" ) && !messages[k].metadata.IsEmpty() )
                    {
                        try {
                            auto j = nlohmann::json::parse( messages[k].metadata.ToStdString() );
                            if( j.contains( "version_id" ) && j["version_id"].is_string() )
                            {
                                versionToRestore = wxString::FromUTF8( j["version_id"].get<std::string>() );
                                break;
                            }
                        } catch( ... ) {}
                    }
                }
                // Keep the user message itself so the user can edit and resend
                continue;
            }
            if( found )
                db.DeleteMessage( convId, messages[i].id );
        }

        if( !versionToRestore.IsEmpty() )
        {
            RestoreVersion( versionToRestore, [this, convId]( bool /*success*/ ) {
                reloadMessagesToWebview( convId );
            });
        }
        else
        {
            reloadMessagesToWebview( convId );
        }
    });

    m_chatWebview->SetRegenerateMessageCallback( [this]( const wxString& messageId, const wxString& tabId ) {
        int tabIndex = resolveTabIndex( tabId );
        if( tabIndex < 0 || tabIndex >= static_cast<int>( m_tabs.size() ) )
            return;
        TAB_DATA& tab = m_tabs[tabIndex];
        if( tab.isStreaming.load() )
            return;

        wxString convId = tab.conversationId;
        if( convId.IsEmpty() )
            return;

        // BUG FIX: check conflict BEFORE any DB mutations
        if( isOtherTabStreamingToCurrentFile() )
        {
            m_chatWebview->ShowStatus(
                wxT( "Cannot regenerate: another tab is actively editing this file. "
                     "Wait for it to finish." ) );
            return;
        }

        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        auto messages = db.LoadMessages( convId );

        wxString userContent;
        wxString versionToRestore;
        bool foundAssistant = false;
        for( size_t i = 0; i < messages.size(); i++ )
        {
            if( messages[i].id == messageId )
            {
                foundAssistant = true;
                for( int k = static_cast<int>( i ) - 1; k >= 0; k-- )
                {
                    if( messages[k].role == wxT( "assistant" ) && !messages[k].metadata.IsEmpty() )
                    {
                        try {
                            auto j = nlohmann::json::parse( messages[k].metadata.ToStdString() );
                            if( j.contains( "version_id" ) && j["version_id"].is_string() )
                            {
                                versionToRestore = wxString::FromUTF8( j["version_id"].get<std::string>() );
                                break;
                            }
                        } catch( ... ) {}
                    }
                }
                if( i > 0 && messages[i - 1].role == wxT( "user" ) )
                {
                    userContent = messages[i - 1].content;
                    db.DeleteMessage( convId, messages[i - 1].id );
                }
                db.DeleteMessage( convId, messages[i].id );
            }
            else if( foundAssistant )
            {
                db.DeleteMessage( convId, messages[i].id );
            }
        }

        if( !versionToRestore.IsEmpty() )
        {
            // BUG FIX: use completion callback so sendMessageFromWebview runs AFTER restore
            RestoreVersion( versionToRestore, [this, convId, userContent, foundAssistant]( bool /*success*/ ) {
                reloadMessagesToWebview( convId );

                if( !userContent.IsEmpty() )
                {
                    sendMessageFromWebview( userContent );
                }
                else if( foundAssistant )
                {
                    m_chatWebview->ShowStatus( wxT( "Response removed. No user message found to resend." ) );
                }
            });
        }
        else
        {
            reloadMessagesToWebview( convId );

            if( !userContent.IsEmpty() )
            {
                sendMessageFromWebview( userContent );
            }
            else if( foundAssistant )
            {
                m_chatWebview->ShowStatus( wxT( "Response removed. No user message found to resend." ) );
            }
        }
    });

    SetSizer( mainSizer );

    // Update auth state in React UI
    updateAuthUI();

    Layout();

    Bind( wxEVT_SYS_COLOUR_CHANGED, &AI_CHAT_PANEL_BASE::onThemeChanged, this );

    // Create initial tab immediately (required for UI to display)
    int initialTab = createNewTab();
    if( initialTab >= 0 )
        switchToTab( initialTab );
    
    // Defer tab restoration to after derived class is fully constructed
    // (GetCurrentFileName() is pure virtual and not available during base class construction)
    CallAfter( [this]() {
        if( m_isDestroying.load() )
            return;
        
        // Try to restore persisted tabs from the previous session
        // If restoration succeeds, the initial empty tab gets replaced
        // If it fails (no persisted tabs), we keep the initial empty tab
        if( loadPersistedTabs() )
        {
            // Restored persisted tabs
        }
        else
        {
            // No persisted tabs - keep the initial empty tab (Cursor-style: simple and predictable)
            
            // If we ended up with no tabs (restoration cleared but found nothing),
            // create a fresh tab
            if( m_tabs.empty() )
            {
                int newTab = createNewTab();
                if( newTab >= 0 )
                    switchToTab( newTab );
            }
        }
    } );
}


void AI_CHAT_PANEL_BASE::onThemeChanged( wxSysColourChangedEvent& aEvent )
{
    // Update webview theme
    if( m_chatWebview )
    {
        bool isDark = KIPLATFORM::UI::IsDarkTheme();
        m_chatWebview->SetTheme( isDark );
    }
    
    // Ensure button bitmap is updated for the new theme
    // Check if current tab is streaming for correct button state
    bool isStreaming = false;
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        isStreaming = m_tabs[m_currentTabIndex].isStreaming.load();
    }
    updateButtonState( isStreaming );

    // Propagate event to children
    aEvent.Skip();

    Refresh();
}


void AI_CHAT_PANEL_BASE::updateAuthUI()
{
    if( !m_chatWebview )
        return;

    try
    {
        bool isAuthenticated = AUTH_MANAGER::Instance().IsAuthenticated();

        // Update React UI with auth state (shows/hides auth header and tab bar)
        m_chatWebview->SetAuthState( isAuthenticated );

        if( isAuthenticated )
        {
            AUTH_USER user = AUTH_MANAGER::Instance().GetCurrentUser();
            m_chatWebview->SetUserInfo( user.fullName, user.email );

            // Fetch and show quota info banner on startup (trial time, usage)
            fetchAndShowQuotaInfo( true );  // true = startup, show banner once
        }
        else
        {
            // Hide banner when not authenticated
            m_chatWebview->HideBanner();
        }
    }
    catch( ... )
    {
        // Auth manager might not be initialized yet - show sign-in state
        m_chatWebview->SetAuthState( false );
    }
}


// onAuthButtonClick and onUpgradeButtonClick removed - handled by React UI via webview callbacks


void AI_CHAT_PANEL_BASE::showQuotaBanner( const wxString& aMessage, bool aShowUpgrade )
{
    if( !m_chatWebview )
        return;
    
    wxLogDebug( wxT( "[QUOTA_BANNER] Showing banner: %s (showUpgrade=%d)" ), aMessage, aShowUpgrade );
    
    m_chatWebview->ShowBanner( aMessage, aShowUpgrade );
}


void AI_CHAT_PANEL_BASE::hideQuotaBanner()
{
    if( !m_chatWebview )
        return;

    wxLogDebug( wxT( "[QUOTA_BANNER] Hiding banner" ) );
    m_chatWebview->HideBanner();
}


void AI_CHAT_PANEL_BASE::fetchAndShowQuotaInfo( bool aIsStartup )
{
    // Only fetch if authenticated
    if( !AUTH_MANAGER::Instance().IsAuthenticated() )
    {
        wxLogDebug( wxT( "[QUOTA_BANNER] Not authenticated, skipping quota fetch" ) );
        return;
    }

    // Get auth token
    std::string authToken = AUTH_MANAGER::Instance().GetAuthToken().ToStdString();
    if( authToken.empty() )
    {
        wxLogDebug( wxT( "[QUOTA_BANNER] No auth token, skipping quota fetch" ) );
        return;
    }

    wxLogDebug( wxT( "[QUOTA_BANNER] Fetching quota info (isStartup=%d)..." ), aIsStartup );

    // Capture backend URL and startup flag for thread
    std::string backendUrl = m_backendUrl.ToStdString();
    bool isStartup = aIsStartup;
    AI_CHAT_PANEL_BASE* self = this;  // Capture for CallAfter

    // Fetch quota in background thread to avoid blocking UI
    std::thread( [self, authToken, backendUrl, isStartup]() {
        try
        {
            // Create client and fetch quota
            AI_BACKEND_CLIENT client( backendUrl );
            USER_QUOTA_INFO quota = client.GetUserQuota( authToken );

            wxLogDebug( wxT( "[QUOTA_BANNER] API response: success=%d, plan=%s, code=%s, dailyCostUsed=%.4f, dailyCostCap=%.2f, isTrial=%d, trialHoursLeft=%d, creditsRemaining=%d" ),
                quota.success, wxString::FromUTF8( quota.plan.c_str() ), 
                wxString::FromUTF8( quota.code.c_str() ), quota.dailyCostUsed, quota.dailyCostCap,
                quota.isTrial, quota.trialHoursLeft, quota.creditsRemaining );

            if( !quota.success )
            {
                wxLogDebug( wxT( "[QUOTA_BANNER] Quota fetch failed, not showing banner" ) );
                return;
            }

            // Check if this is an on-demand (credit-based) plan
            bool isOnDemand = ( quota.plan.find( "on_demand" ) != std::string::npos );
            
            // Determine if quota is LOW (warning threshold) - now cost-based
            bool isLowQuota = false;
            
            if( quota.plan == "free" )
            {
                isLowQuota = true;  // Free users always see banner
            }
            else if( quota.isTrial )
            {
                // Trial: check cost usage and time remaining
                bool lowTime = ( quota.trialHoursLeft >= 0 && quota.trialHoursLeft <= QuotaConfig::TRIAL_LOW_HOURS_THRESHOLD );
                bool highUsage = false;
                if( quota.dailyCostCap > 0 )
                {
                    double usagePercent = ( quota.dailyCostUsed / quota.dailyCostCap ) * 100.0;
                    highUsage = ( usagePercent >= QuotaConfig::DAILY_USAGE_WARNING_PERCENT );
                }
                isLowQuota = lowTime || highUsage;
            }
            else if( isOnDemand )
            {
                isLowQuota = ( quota.creditsRemaining >= 0 && quota.creditsRemaining <= QuotaConfig::CREDITS_WARNING_THRESHOLD );
            }
            else if( quota.code == "SUBSCRIPTION_PAST_DUE" || quota.code == "SUBSCRIPTION_CANCELLED" )
            {
                isLowQuota = true;  // Always show warning for subscription issues
            }
            else if( quota.code == "DAILY_COST_LIMIT_REACHED" || quota.code == "MONTHLY_COST_LIMIT_REACHED" )
            {
                isLowQuota = true;  // Always show when cost limit reached
            }
            
            // Decide whether to show banner:
            // 1. On startup: always show (will be hidden after first message)
            // 2. After message: only show if quota is LOW (warning)
            // 3. Subscription plans (pro_*, ultra_*, internal): never show UNLESS past_due/cancelled/limit_reached
            
            bool isSubscriptionPlan = ( quota.plan != "trial" && quota.plan != "free" && !isOnDemand );
            bool hasSubscriptionIssue = ( quota.code == "SUBSCRIPTION_PAST_DUE" || quota.code == "SUBSCRIPTION_CANCELLED" 
                                          || quota.code == "DAILY_COST_LIMIT_REACHED" || quota.code == "MONTHLY_COST_LIMIT_REACHED" );
            
            if( isSubscriptionPlan && !hasSubscriptionIssue )
            {
                wxLogDebug( wxT( "[QUOTA_BANNER] Skipping banner for healthy subscription plan: %s" ), wxString::FromUTF8( quota.plan.c_str() ) );
                return;
            }
            
            // If not startup and not low quota, hide the banner (user sent a message)
            if( !isStartup && !isLowQuota )
            {
                wxLogDebug( wxT( "[QUOTA_BANNER] Not startup and quota not low, hiding banner" ) );
                self->CallAfter( [self]() {
                    self->hideQuotaBanner();
                } );
                return;
            }

            // Build banner message based on quota info - NOW COST-BASED
            wxString bannerMsg;
            bool isWarning = false;
            
            if( isOnDemand )
            {
                // On-demand (credit-based) user: show credits remaining
                if( quota.creditsRemaining >= 0 )
                {
                    if( quota.creditsRemaining <= QuotaConfig::CREDITS_CRITICAL_THRESHOLD )
                    {
                        bannerMsg = wxString::Format( 
                            wxT( "Low credits! Only %d remaining" ),
                            quota.creditsRemaining
                        );
                        isWarning = true;
                    }
                    else
                    {
                        bannerMsg = wxString::Format( 
                            wxT( "%d credits remaining" ),
                            quota.creditsRemaining
                        );
                    }
                }
                else
                {
                    // Credits not available from backend yet - show plan name
                    bannerMsg = wxT( "Credit-based plan active" );
                }
            }
            else if( quota.isTrial && quota.trialHoursLeft >= 0 )
            {
                int trialDays = quota.trialHoursLeft / 24;
                int trialHours = quota.trialHoursLeft % 24;

                if( quota.trialHoursLeft <= QuotaConfig::TRIAL_LOW_HOURS_THRESHOLD )
                {
                    if( trialDays > 0 )
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial ending soon! %dd %dh left" ),
                            trialDays, trialHours );
                    }
                    else
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial ending soon! %dh left" ),
                            quota.trialHoursLeft );
                    }
                    isWarning = true;
                }
                else if( quota.dailyCostCap > 0 )
                {
                    double usagePercent = ( quota.dailyCostUsed / quota.dailyCostCap ) * 100.0;
                    if( usagePercent >= QuotaConfig::DAILY_USAGE_WARNING_PERCENT )
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Daily limit almost reached! $%.2f/$%.2f | %dd left" ),
                            quota.dailyCostUsed, quota.dailyCostCap, trialDays );
                        isWarning = true;
                    }
                    else
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial: %dd left | $%.2f/$%.2f today" ),
                            trialDays, quota.dailyCostUsed, quota.dailyCostCap );
                    }
                }
                else
                {
                    bannerMsg = wxString::Format(
                        wxT( "Trial: %dd left" ), trialDays );
                }
            }
            else if( quota.code == "TRIAL_ACTIVE" )
            {
                int trialDays = ( quota.trialHoursLeft >= 0 )
                                    ? ( quota.trialHoursLeft / 24 ) : -1;

                if( trialDays >= 0 )
                {
                    if( quota.dailyCostCap > 0 )
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial: %dd left | $%.2f/$%.2f today" ),
                            trialDays, quota.dailyCostUsed, quota.dailyCostCap );
                    }
                    else
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial: %dd left" ), trialDays );
                    }
                }
                else
                {
                    if( quota.dailyCostCap > 0 )
                    {
                        bannerMsg = wxString::Format(
                            wxT( "Trial active | $%.2f/$%.2f today" ),
                            quota.dailyCostUsed, quota.dailyCostCap );
                    }
                    else
                    {
                        bannerMsg = wxT( "Trial active - unlimited usage" );
                    }
                }
            }
            else if( quota.code == "DAILY_COST_LIMIT_REACHED" )
            {
                // Daily cost limit reached
                bannerMsg = wxString::Format( 
                    wxT( "Daily limit reached ($%.2f). Try again tomorrow or upgrade." ),
                    quota.dailyCostCap > 0 ? quota.dailyCostCap : quota.dailyCostUsed
                );
                isWarning = true;
            }
            else if( quota.code == "MONTHLY_COST_LIMIT_REACHED" )
            {
                // Monthly cost limit reached
                bannerMsg = wxString::Format( 
                    wxT( "Monthly limit reached ($%.2f). Upgrade for more usage." ),
                    quota.monthlyCostCap > 0 ? quota.monthlyCostCap : quota.monthlyCostUsed
                );
                isWarning = true;
            }
            else if( quota.plan == "free" )
            {
                // Free user after trial - always show
                bannerMsg = wxT( "Free plan - Ask mode only. Upgrade for Agent & Plan modes." );
                isWarning = true;
            }
            else if( quota.code == "SUBSCRIPTION_PAST_DUE" )
            {
                // Payment failed - show warning but allow access (grace period)
                bannerMsg = wxT( "Payment failed. Please update your payment method to avoid service interruption." );
                isWarning = true;
            }
            else if( quota.code == "SUBSCRIPTION_CANCELLED" )
            {
                // Subscription cancelled - show warning
                bannerMsg = wxT( "Your subscription has been cancelled. Please renew to continue." );
                isWarning = true;
            }

            if( !bannerMsg.IsEmpty() )
            {
                wxLogDebug( wxT( "[QUOTA_BANNER] Scheduling banner display: %s (isWarning=%d)" ), 
                            bannerMsg, isWarning );
                // Show banner on main thread
                self->CallAfter( [self, bannerMsg, isWarning]() {
                    self->showQuotaBanner( bannerMsg, isWarning );
                } );
            }
            else
            {
                wxLogDebug( wxT( "[QUOTA_BANNER] No banner message to show (empty)" ) );
            }
        }
        catch( const std::exception& )
        {
            // Silently fail - quota display is informational
        }
    } ).detach();
}


void AI_CHAT_PANEL_BASE::onAuthStateChanged( wxCommandEvent& aEvent )
{
    // Defer UI update to the next event loop iteration so the window
    // is fully foregrounded and the webview can execute JavaScript
    CallAfter( [this]()
    {
        updateAuthUI();
        Layout();
        Refresh();

        if( AUTH_MANAGER::Instance().IsAuthenticated() )
        {
            AUTH_USER user = AUTH_MANAGER::Instance().GetCurrentUser();

            CONVERSATION_DB& db = CONVERSATION_DB::Instance();
            db.SetUserIdForLocalConversations( user.id );

            CONVERSATION_SYNC::Instance().Start();

            if( m_syncThread && m_syncThread->joinable() )
                m_syncThread->join();

            m_syncThread = std::make_unique<std::thread>( []() {
                CONVERSATION_SYNC::Instance().FetchFromSupabase();
            } );
        }
        else
        {
            CONVERSATION_SYNC::Instance().Stop();
        }
    } );
}


void AI_CHAT_PANEL_BASE::sendMessageFromWebview( const wxString& aMessage,
                                                   const std::vector<ChatAttachment>& aAttachments )
{
    wxString message = aMessage;
    message.Trim();

    if( message.IsEmpty() )
        return;

    // Update cached project path (safe access for destructor - avoids pure virtual call)
    m_cachedProjectPath = GetCurrentFileName();

    // Check if the CURRENT tab is streaming (per-tab check for parallel execution)
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        if( m_tabs[m_currentTabIndex].isStreaming.load() )
        {
            // This tab is already streaming - the send button should be a stop button
            // so this shouldn't normally happen, but guard against it
        return;
        }
    }

    // PROACTIVE TOKEN REFRESH: Check and refresh token BEFORE making any request
    // This prevents "Session expired" errors by ensuring we always have a valid token
    if( AUTH_MANAGER::Instance().IsAuthenticated() )
    {
        if( AUTH_MANAGER::Instance().IsTokenExpiringSoon() )
        {
            if( AUTH_MANAGER::Instance().RefreshAccessToken() )
            {
                // Proactive token refresh successful
            }
            else
            {
                // Proactive token refresh failed, but continuing with existing token
                // Don't block the request - the backend might still accept the token
                // If it truly fails, we'll get auth_error from the backend
            }
        }
    }

    // Create conversation in local DB if this is the first message
    // Check for temp ID or empty ID (new tabs start with temp_XXXXXXXX)
    bool needsNewConversation = m_conversationId.IsEmpty() || m_conversationId.StartsWith( wxT( "temp_" ) );
    if( needsNewConversation && m_currentTabIndex >= 0 )
    {
        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        
        // Ensure DB is initialized (in case async init hasn't completed yet)
        if( !db.IsOpen() )
        {
            if( !db.Initialize() )
            {
                wxLogError( wxT( "Failed to initialize conversation database" ) );
        return;
    }
        }
        
        wxString userId = wxEmptyString;
        if( AUTH_MANAGER::Instance().IsAuthenticated() )
        {
            AUTH_USER user = AUTH_MANAGER::Instance().GetCurrentUser();
            userId = user.id;
        }
        
        wxString oldTempId;
        if( m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            oldTempId = m_tabs[m_currentTabIndex].conversationId;
        }
        
        auto conv = db.CreateConversation( userId, GetCurrentFileName(), m_sessionId );
        if( conv.has_value() )
        {
            m_conversationId = conv->id;
            
            // Update tab data with real conversation ID
            if( m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
            {
                m_tabs[m_currentTabIndex].conversationId = m_conversationId;
                
                // Update webview tab ID (replace temp ID with real ID)
                // We need to remove the old tab and add a new one with the correct ID
                if( m_chatWebview && !oldTempId.IsEmpty() )
                {
                    m_chatWebview->ReplaceTabId( oldTempId, m_conversationId,
                                                  m_tabs[m_currentTabIndex].title );
                }
                
                // Save tab state now that we have a valid conversation ID
                saveOpenTabs();
            }
            }
            else
            {
            wxLogError( wxT( "Failed to create conversation in database" ) );
            return;
        }
    }

    // Generate a stable user message ID used by both SQLite and the webview.
    static int sUserMsgCounter = 0;
    wxString userMsgId = wxString::Format( wxT( "user_%ld_%d" ), wxGetLocalTime(), ++sUserMsgCounter );

    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
        m_tabs[m_currentTabIndex].pendingUserMsgId = userMsgId;

    // Save user message to local DB (only if we have a valid conversation)
    if( !m_conversationId.IsEmpty() )
    {
        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        
        auto msg = db.SaveMessage( m_conversationId, wxT( "user" ), message,
                                    wxString::Format( wxT( "{\"mode\":\"%s\",\"app_type\":\"%s\"}" ),
                                        m_currentMode == AI_MODE::PLAN ? wxT( "plan" )
                                            : m_currentMode == AI_MODE::ASK ? wxT( "ask" ) : wxT( "agent" ),
                                        GetAppType() ),
                                    userMsgId );
        if( !msg.has_value() )
        {
            wxLogError( wxT( "Failed to save message to database for conversation: %s" ), m_conversationId );
        }
        
        // Update conversation title from first message if not set
        if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            if( m_tabs[m_currentTabIndex].title == wxT( "New Chat" ) )
            {
                wxString title = message.Left( 30 );
                if( message.length() > 30 )
                    title += wxT( "..." );
                
                m_tabs[m_currentTabIndex].title = title;
                
                // Update webview tab title
                if( m_chatWebview )
                    m_chatWebview->SetTabTitle( m_conversationId, title );
                
                db.UpdateConversationTitle( m_conversationId, title );
            }
        }
    }

    // Clear the draft for this tab since message was sent
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
        {
        m_tabs[m_currentTabIndex].draftInput.Clear();
    }

    // Analytics: track chat message engagement
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        TAB_DATA& tab = m_tabs[m_currentTabIndex];
        tab.userMessageCount++;
        tab.totalUserCharsSent += static_cast<int>( message.length() );

        if( tab.userMessageCount == 1 )
            tab.conversationStartTime = std::chrono::steady_clock::now();

        std::string modeStr = "agent";
        if( m_currentMode == AI_MODE::PLAN )
            modeStr = "plan";
        else if( m_currentMode == AI_MODE::ASK )
            modeStr = "ask";

        AMPLITUDE_CLIENT::Instance().Track( "ai_chat_message_sent", {
            { "message_length", static_cast<int>( message.length() ) },
            { "mode", modeStr },
            { "conversation_depth", tab.userMessageCount },
            { "has_attachments", !aAttachments.empty() },
            { "attachment_count", static_cast<int>( aAttachments.size() ) },
            { "app_type", std::string( GetAppType().utf8_str() ) }
        });
    }
    
    sendToBackendAsync( message, aAttachments );
}


void AI_CHAT_PANEL_BASE::onSendMessage( wxCommandEvent& aEvent )
{
    // Legacy handler - send is now handled by React UI via webview callbacks
    (void) aEvent;
}


// Note: SendJsonRequest has been removed. Direct backend communication is now
// handled by AI_BACKEND_CLIENT via sendToBackendAsync().


// NOTE: HandleBackendEvent was removed - dead code
// All event handling now goes through handleBackendEventDirect with explicit tab index


void AI_CHAT_PANEL_BASE::SetDrcCallback( std::function<nlohmann::json()> aCallback )
        {
    // Store callback to set on all current AND future tabs
    m_drcCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
        {
        if( tab.toolExecutor )
            tab.toolExecutor->SetDrcCallback( aCallback );
    }
}


void AI_CHAT_PANEL_BASE::SetErcCallback( std::function<nlohmann::json()> aCallback )
    {
    // Store callback to set on all current AND future tabs
    m_ercCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetErcCallback( aCallback );
        }
    }


void AI_CHAT_PANEL_BASE::SetAnnotateCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_annotateCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetAnnotateCallback( aCallback );
            }
        }

void AI_CHAT_PANEL_BASE::SetGerberCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_gerberCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetGerberCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetDrillCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_drillCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetDrillCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetAutorouteCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_autorouteCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetAutorouteCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetHierarchyCallback( std::function<nlohmann::json()> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_hierarchyCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetHierarchyCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetSwitchSheetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_switchSheetCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetSwitchSheetCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetSwitchLayerCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_switchLayerCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetSwitchLayerCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetLayersCallback( std::function<nlohmann::json()> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_layersCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetLayersCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetLayerPresetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_layerPresetCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetLayerPresetCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetLayerVisibilityCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_layerVisibilityCallback = aCallback;
    
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetLayerVisibilityCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetFetchDimensionsCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
{
    // Store callback to set on all current AND future tabs
    m_fetchDimensionsCallback = aCallback;

    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
    {
        if( tab.toolExecutor )
            tab.toolExecutor->SetFetchDimensionsCallback( aCallback );
    }
}

void AI_CHAT_PANEL_BASE::SetSnapshotCallback( std::function<std::string()> aCallback )
    {
    // Store callback to set on all current AND future tabs
    m_snapshotCallback = aCallback;
            
    // Set on all existing tabs' tool executors
    for( auto& tab : m_tabs )
            {
        if( tab.toolExecutor )
            tab.toolExecutor->SetSnapshotCallback( aCallback );
            }
}


void AI_CHAT_PANEL_BASE::configureToolExecutor( AI_TOOL_EXECUTOR* aToolExecutor )
{
    if( !aToolExecutor )
        return;
    
    // Note: App type is set in sendToBackendAsync() since GetAppType() is pure virtual
    // and not available during base class construction
    
    // Apply all stored callbacks
    if( m_drcCallback )
        aToolExecutor->SetDrcCallback( m_drcCallback );
    if( m_ercCallback )
        aToolExecutor->SetErcCallback( m_ercCallback );
    if( m_annotateCallback )
        aToolExecutor->SetAnnotateCallback( m_annotateCallback );
    if( m_gerberCallback )
        aToolExecutor->SetGerberCallback( m_gerberCallback );
    if( m_drillCallback )
        aToolExecutor->SetDrillCallback( m_drillCallback );
    if( m_autorouteCallback )
        aToolExecutor->SetAutorouteCallback( m_autorouteCallback );
    if( m_hierarchyCallback )
        aToolExecutor->SetHierarchyCallback( m_hierarchyCallback );
    if( m_switchSheetCallback )
        aToolExecutor->SetSwitchSheetCallback( m_switchSheetCallback );
    // Layer callbacks (pcbnew)
    if( m_switchLayerCallback )
        aToolExecutor->SetSwitchLayerCallback( m_switchLayerCallback );
    if( m_layersCallback )
        aToolExecutor->SetLayersCallback( m_layersCallback );
    if( m_layerPresetCallback )
        aToolExecutor->SetLayerPresetCallback( m_layerPresetCallback );
    if( m_layerVisibilityCallback )
        aToolExecutor->SetLayerVisibilityCallback( m_layerVisibilityCallback );
    if( m_fetchDimensionsCallback )
        aToolExecutor->SetFetchDimensionsCallback( m_fetchDimensionsCallback );
    if( m_snapshotCallback )
        aToolExecutor->SetSnapshotCallback( m_snapshotCallback );
    if( m_confirmationCallback )
        aToolExecutor->SetConfirmationCallback( m_confirmationCallback );
}


void AI_CHAT_PANEL_BASE::handlePlanDocumentEvent( const nlohmann::json& aEvent, int aTabIndex )
{
    if( !aEvent.contains( "data" ) )
        return;

    const auto& data = aEvent["data"];
    
    // Extract plan document markdown - check both data["document"] and content
    std::string planMarkdown = data.value( "document", "" );
    if( planMarkdown.empty() && aEvent.contains( "content" ) )
    {
        planMarkdown = aEvent.value( "content", "" );
    }
    wxString planDoc = wxString::FromUTF8( planMarkdown );
    
    if( planDoc.IsEmpty() )
        return;
    
    // Check render_mode - "full" shows buttons, "summary" is display-only
    std::string renderMode = data.value( "render_mode", "full" );
    bool showApprovalButtons = ( renderMode == "full" );
    
    wxLogDebug( wxT( "handlePlanDocumentEvent: renderMode=%s, showApproval=%s" ),
                wxString::FromUTF8( renderMode ), showApprovalButtons ? wxT( "true" ) : wxT( "false" ) );
    
    // Save plan as a message to the local SQLite database immediately.
    // The plan markdown is stored directly in the content column so it
    // survives app restarts without depending on external files.
    if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        TAB_DATA& tab = m_tabs[aTabIndex];
        
        wxLogDebug( wxT( "handlePlanDocumentEvent: tab.conversationId=%s, aTabIndex=%d" ), 
                    tab.conversationId, aTabIndex );
        
        // Only save if we have a real conversation ID (not temp_*)
        if( !tab.conversationId.IsEmpty() && !tab.conversationId.StartsWith( wxT( "temp_" ) ) )
        {
            CONVERSATION_DB& db = CONVERSATION_DB::Instance();
            nlohmann::json metadata;
            metadata["type"] = "plan_document";
            metadata["render_mode"] = renderMode;
            
            wxLogDebug( wxT( "handlePlanDocumentEvent: Saving plan message with metadata: %s" ), 
                        wxString::FromUTF8( metadata.dump() ) );
            
            auto savedMsg = db.SaveMessage(
                tab.conversationId,
                wxT( "assistant" ),
                planDoc,
                wxString::FromUTF8( metadata.dump() )
            );
            
            if( savedMsg.has_value() )
            {
                wxLogDebug( wxT( "handlePlanDocumentEvent: Saved plan message to DB, id=%s" ), 
                            savedMsg->id );
            }
            else
            {
                wxLogWarning( wxT( "handlePlanDocumentEvent: Failed to save plan message to DB" ) );
            }
        }
        else
        {
            wxLogDebug( wxT( "handlePlanDocumentEvent: Skipping DB save - conversation ID is empty or temp: '%s'" ), 
                        tab.conversationId );
        }
        
        wxLogDebug( wxT( "handlePlanDocumentEvent: Plan persisted to SQLite" ) );
    }
    
    // Route to webview - create a NEW message for the plan document
    // This ensures the plan appears AFTER any user messages (like clarification responses)
    if( m_chatWebview && aTabIndex == m_currentTabIndex )
    {
        TAB_DATA& tab = m_tabs[aTabIndex];
        
        // First, finalize any existing streaming message to clear isStreaming state
        if( !tab.streamingMessageId.IsEmpty() )
        {
            wxLogDebug( wxT( "handlePlanDocumentEvent: Finalizing previous streaming message: %s" ), 
                        tab.streamingMessageId );
            m_chatWebview->FinalizeMessage( tab.streamingMessageId );
        }
        
        // Always create a NEW message ID for the plan document
        // This ensures the plan appears after any user messages that were added
        // since the last AI response (e.g., clarification responses like "1a, 2a, 3a")
        wxString planMsgId = wxString::Format( wxT( "plan_%lld" ), 
            static_cast<long long>( wxGetUTCTimeMillis().GetValue() ) );
        wxLogDebug( wxT( "handlePlanDocumentEvent: Creating new plan message: %s" ), planMsgId );
        
        // SetPlanDocument will create a new message with the plan document
        m_chatWebview->SetPlanDocument( planMsgId, planDoc, wxString::FromUTF8( renderMode ) );
        
        // Generate a new message ID for any subsequent text
        // This ensures text after the plan goes into a new message
        tab.streamingMessageId = wxString::Format( wxT( "msg_%lld" ), 
            static_cast<long long>( wxGetUTCTimeMillis().GetValue() ) );
        tab.isFirstStreamingFlush = true;  // Next text will create a new message
        
        wxLogDebug( wxT( "handlePlanDocumentEvent: Sent plan to webview, planMsgId=%s, new streamingId=%s" ), 
                    planMsgId, tab.streamingMessageId );
        
        // Show approval buttons if full mode
        if( showApprovalButtons )
        {
            m_chatWebview->ShowPlanApproval();
            wxLogDebug( wxT( "handlePlanDocumentEvent: Showing plan approval buttons" ) );
        }
    }
}


void AI_CHAT_PANEL_BASE::handlePhaseUpdateEvent( const nlohmann::json& aEvent, int aTabIndex )
{
    if( !aEvent.contains( "data" ) )
        return;

    const auto& data = aEvent["data"];
    std::string phase = data.value( "phase", "" );
    std::string phaseLabel = aEvent.value( "content", "" );
    bool readOnly = data.value( "read_only", false );
    
    // Capture research text when transitioning OUT of research phase
    if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        TAB_DATA& tab = m_tabs[aTabIndex];
        wxString newPhase = wxString::FromUTF8( phase );
        
        if( tab.currentPhase == wxT( "research" ) && newPhase != wxT( "research" ) )
        {
            if( !tab.pendingStreamingResponse.IsEmpty() )
                tab.researchSummary = tab.pendingStreamingResponse;
        }
        
        tab.currentPhase = newPhase;
    }
    
    // Build display text
    wxString displayText = wxString::FromUTF8( phaseLabel );
    wxString phaseColor;
    
    if( readOnly )
    {
        phaseColor = wxT( "#6b7280" ); // Gray -- blue phase styling in webview is enough
    }
    else if( phase == "executing" )
    {
        phaseColor = wxT( "#404040" ); // Dark gray
    }
    else
    {
        phaseColor = wxT( "#6b7280" ); // Gray
    }
    
    wxLogDebug( wxT( "handlePhaseUpdateEvent: phase=%s, label=%s, color=%s" ),
                wxString::FromUTF8( phase ), displayText, phaseColor );
    
    // Route to webview
    if( m_chatWebview && aTabIndex == m_currentTabIndex )
    {
        m_chatWebview->ShowPhaseIndicator( displayText, phaseColor );
    }
}


void AI_CHAT_PANEL_BASE::sendPlanApproval( const std::string& aApprovalType, int aTabIndex )
{
    // Call the new structured plan action endpoint
    sendPlanAction( aApprovalType, "", aTabIndex );
}


void AI_CHAT_PANEL_BASE::sendPlanAction( const std::string& aAction,
                                          const std::string& aFeedback,
                                          int aTabIndex )
{
    wxLogDebug( "[PLAN_ACTION] ENTRY: action=%s, tabIndex=%d, tabsSize=%zu", 
                aAction, aTabIndex, m_tabs.size() );
    
    // Validate tab index with extra safety
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
    {
        wxLogWarning( "[PLAN_ACTION] Invalid tab index: %d (size=%zu)", aTabIndex, m_tabs.size() );
        return;
    }
    
    // Check if panel is being destroyed
    if( m_isDestroying.load() )
    {
        wxLogWarning( "[PLAN_ACTION] Panel is being destroyed, aborting" );
        return;
    }
    
    wxLogDebug( "[PLAN_ACTION] Validation passed, proceeding..." );
    
    // Hide webview plan approval buttons and phase indicator
    if( m_chatWebview )
    {
        m_chatWebview->HidePlanApproval();
        m_chatWebview->HidePhaseIndicator();
    }
    
    m_awaitingModeConfirmation = true;
    
    // For approve action, expect agent mode transition
    if( aAction == "approve" )
    {
        m_planModeExecuting = true;
        m_expectedMode = AI_MODE::AGENT;  // Expect transition to agent mode
        wxLogDebug( "[PLAN_ACTION] Sent 'approve' - awaiting MODE_TRANSITION from backend" );
    }
    else if( aAction == "cancel" )
    {
        m_planModeExecuting = false;
        wxLogDebug( "[PLAN_ACTION] Sent 'cancel'" );
    }
    else
    {
        wxLogDebug( "[PLAN_ACTION] Sent '%s' with feedback", aAction );
    }
    
    wxLogDebug( "[PLAN_ACTION] Getting tab reference..." );
    TAB_DATA& tab = m_tabs[aTabIndex];
    
    wxLogDebug( "[PLAN_ACTION] Getting auth token..." );
    // Get auth token
    std::string authToken;
    std::string refreshToken;
    try
    {
        AUTH_MANAGER& authMgr = AUTH_MANAGER::Instance();
        authToken = authMgr.GetAuthToken().ToStdString();
        refreshToken = authMgr.GetRefreshToken().ToStdString();
        wxLogDebug( "[PLAN_ACTION] Got auth token (len=%zu)", authToken.length() );
    }
    catch( const std::exception& e )
    {
        wxLogWarning( "[PLAN_ACTION] Failed to get auth token: %s", e.what() );
        return;
    }
    catch( ... )
    {
        wxLogWarning( "[PLAN_ACTION] Failed to get auth token (unknown error)" );
        return;
    }
    
    std::string sessionId = m_sessionId.ToStdString();
    std::string conversationId = tab.conversationId.ToStdString();
    
    wxLogDebug( "[PLAN_ACTION] Ensuring backend client exists..." );
    // Ensure backend client exists for this tab
    if( !tab.backendClient )
    {
        std::string backendUrl = GetBackendUrl().ToStdString();
        wxLogDebug( "[PLAN_ACTION] Creating backend client with URL: %s", backendUrl );
        tab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( backendUrl );
    }
    
    AI_BACKEND_CLIENT* tabClient = tab.backendClient.get();
    if( !tabClient )
    {
        wxLogError( "[PLAN_ACTION] Failed to create backend client" );
        m_awaitingModeConfirmation = false;
        return;
    }
    wxLogDebug( "[PLAN_ACTION] Backend client ready" );
    
    // Set up event callback
    auto panelAlive = m_panelAlive;
    wxString convId = tab.conversationId;
    tabClient->SetEventCallback( [this, panelAlive, convId]( const AI_BACKEND_EVENT& aEvent )
    {
        if( !panelAlive->load() )
            return;

        safeCallAfter( [this, convId, aEvent]()
        {
            TAB_DATA* foundTab = findTabByConversationId( convId );
            if( !foundTab )
                return;
            
            int tabIndex = -1;
            for( size_t i = 0; i < m_tabs.size(); i++ )
            {
                if( &m_tabs[i] == foundTab )
                {
                    tabIndex = static_cast<int>( i );
                    break;
                }
            }
            
            if( tabIndex < 0 )
                return;
            
            handleBackendEventDirect( aEvent, tabIndex );
        } );
    } );
    
    // Mark tab as streaming and reset per-turn state
    tab.isStreaming.store( true );
    tab.stopRequested.store( false );
    tab.pendingStreamingResponse.Clear();
    tab.accumulatedThinking.Clear();
    tab.thinkingStartMs = 0;
    tab.thinkingDurationMs = 0;
    tab.activityMarkers.clear();
    tab.thinkingBlocks.clear();
    tab.lastThinkingBlockUsed = 0;
    tab.researchSummary.Clear();
    tab.currentPhase.Clear();
    
    // Switch Send button to Stop button while AI is responding
    updateButtonState( true );
    
    // Disable input while processing
    if( m_chatWebview )
    {
        m_chatWebview->SetInputEnabled( false );
    }
    
    // Ensure any previous thread is properly released
    // (detached threads should have been set to nullptr after detach)
    if( tab.requestThread )
    {
        if( tab.requestThread->joinable() )
        {
            // This shouldn't happen since we detach, but handle it just in case
            wxLogDebug( "[PLAN_ACTION] Previous thread still joinable - detaching" );
            tab.requestThread->detach();
        }
        tab.requestThread.reset();
    }
    
    // Capture values for thread
    std::string action = aAction;
    std::string feedback = aFeedback;
    
    // Get file paths for tool execution (like StreamChat does)
    wxString filePath = EnsureFileSavedForAI();
    wxString traceFilePath = filePath;
    wxString kicadFilePath = filePath;
    if( !filePath.IsEmpty() )
    {
        traceFilePath = ConvertToTraceFile( filePath );
    }
    std::string traceFilePathStr = traceFilePath.ToStdString();
    std::string kicadFilePathStr = kicadFilePath.ToStdString();
    
    wxLogDebug( "[PLAN_ACTION] Creating background thread for action=%s, file=%s", action, traceFilePathStr );
    
    // Capture self for CallAfter
    AI_CHAT_PANEL_BASE* self = this;
    
    // Start request in background thread with exception handling
    try
    {
        tab.requestThread = std::make_unique<std::thread>(
            [self, tabClient, panelAlive, action, sessionId, conversationId, feedback, authToken, convId, traceFilePathStr, kicadFilePathStr]()
            {
                try
                {
                    AI_STREAM_RESULT result = tabClient->PostPlanAction(
                        action, sessionId, conversationId, feedback, authToken,
                        traceFilePathStr, kicadFilePathStr );

                    if( !panelAlive->load() )
                        return;

                    self->CallAfter( [self, panelAlive, convId, result, action]()
                    {
                        if( !panelAlive->load() || self->m_isDestroying.load() )
                            return;

                        TAB_DATA* foundTab = self->findTabByConversationId( convId );
                        if( !foundTab )
                            return;

                        // Find tab index for flushing streaming buffer
                        int tabIndex = -1;
                        for( size_t i = 0; i < self->m_tabs.size(); i++ )
                        {
                            if( &self->m_tabs[i] == foundTab )
                            {
                                tabIndex = static_cast<int>( i );
                                break;
                            }
                        }
                        
                        // Flush any remaining buffered text before finalizing
                        if( tabIndex >= 0 )
                        {
                            self->flushStreamingBuffer( tabIndex );
                        }
                        
                        // Save AI response to local database (implementation response after plan approval)
                        wxString streamedResponse = foundTab->pendingStreamingResponse;
                        bool hasThinkingData = !foundTab->thinkingBlocks.empty();
                        bool hasActivityData = !foundTab->activityMarkers.empty();
                        if( !convId.IsEmpty()
                            && ( !streamedResponse.IsEmpty() || hasThinkingData || hasActivityData ) )
                        {
                            CONVERSATION_DB& db = CONVERSATION_DB::Instance();
                            
                            nlohmann::json metaJson = buildAssistantMetadataJson(
                                foundTab, wxT( "plan" ), self->GetAppType() );
                            wxString msgMeta = wxString::FromUTF8( metaJson.dump() );
                            auto msg = db.SaveMessage( convId, wxT( "assistant" ), streamedResponse, msgMeta );
                            if( !msg.has_value() )
                            {
                                wxLogError( wxT( "[PLAN_ACTION] Failed to save assistant message to database" ) );
                            }
                            else
                            {
                                foundTab->lastSavedDbMessageId = msg->id;
                                wxLogDebug( wxT( "[PLAN_ACTION] Saved implementation response with %zu markers, %zu thinking blocks" ),
                                            foundTab->activityMarkers.size(), foundTab->thinkingBlocks.size() );
                            }
                        }
                        
                        // Clear per-tab streaming response for next turn
                        foundTab->pendingStreamingResponse.Clear();
                        foundTab->accumulatedThinking.Clear();
                        foundTab->activityMarkers.clear();
                        foundTab->thinkingBlocks.clear();
                        foundTab->thinkingDurationMs = 0;
                        foundTab->lastThinkingBlockUsed = 0;
                        foundTab->researchSummary.Clear();
                        foundTab->currentPhase.Clear();

                        foundTab->isStreaming.store( false );
                        
                        // Re-enable dropdown based on result
                        if( result.status != "success" || action == "cancel" )
                        {
                            self->m_planModeExecuting = false;
                        }
                        
                        self->m_awaitingModeConfirmation = false;
                        
                        // Reset button state and re-enable input if no tabs are streaming
                        if( !self->isAnyTabStreaming() )
                        {
                            self->updateButtonState( false );
                        }
                        
                        if( self->m_chatWebview )
                        {
                            self->m_chatWebview->SetInputEnabled( true );
                        }
                    } );
                }
                catch( const std::exception& e )
                {
                    wxLogError( "[PLAN_ACTION] Thread exception: %s", e.what() );
                }
                catch( ... )
                {
                    wxLogError( "[PLAN_ACTION] Unknown thread exception" );
                }
            }
        );
        
        // Detach thread (it will complete independently)
        if( tab.requestThread && tab.requestThread->joinable() )
        {
            tab.requestThread->detach();
        }
    }
    catch( const std::exception& e )
    {
        wxLogError( "[PLAN_ACTION] Failed to create thread: %s", e.what() );
        tab.isStreaming.store( false );
        m_awaitingModeConfirmation = false;
        
        // Reset button state and re-enable input on error
        updateButtonState( false );
        if( m_chatWebview )
        {
            m_chatWebview->SetInputEnabled( true );
        }
    }
}


wxString AI_CHAT_PANEL_BASE::savePlanToProjectFolder( const wxString& aPlanDocument )
{
    if( !m_frame || aPlanDocument.IsEmpty() )
        return wxEmptyString;

    if( aPlanDocument == m_lastSavedPlanContent )
    {
        wxLogDebug( "Skipping duplicate plan save" );
        return m_lastSavedPlanPath;
    }

    wxString projPath = m_frame->Prj().GetProjectPath();
    if( projPath.IsEmpty() )
    {
        wxLogWarning( "Cannot save plan: no project is open" );
        return wxEmptyString;
    }

    wxFileName projectPath( projPath );
    wxString plansFolder = projectPath.GetPath() + wxFileName::GetPathSeparator() + "plans";
    
    // Create plans folder if it doesn't exist
    if( !wxFileName::DirExists( plansFolder ) )
    {
        wxFileName::Mkdir( plansFolder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );
    }
    
    // Generate filename with timestamp
    wxDateTime now = wxDateTime::Now();
    wxString timestamp = now.Format( "%Y%m%d_%H%M%S" );
    wxString filename = wxString::Format( "circuit_plan_%s.md", timestamp );
    wxString fullPath = plansFolder + wxFileName::GetPathSeparator() + filename;
    
    // Write file using wxFFile (recommended approach)
    wxFFile file( fullPath, "wb" );
    
    if( !file.IsOpened() )
    {
        wxLogWarning( "Could not auto-save plan to %s", fullPath );
        return wxEmptyString;
    }
    
    file.Write( aPlanDocument );
    file.Close();
    
    // Track this plan content and path to prevent duplicate saves
    m_lastSavedPlanContent = aPlanDocument;
    m_lastSavedPlanPath = fullPath;
    
    wxLogDebug( "Plan auto-saved to: %s", fullPath );
    return fullPath;
}


void AI_CHAT_PANEL_BASE::handleBackendEventDirect( const AI_BACKEND_EVENT& aEvent, int aTabIndex )
{
    // Critical: Don't process events if panel is being destroyed
    if( m_isDestroying.load() )
        return;
    
    // Validate tab index for parallel streaming safety
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    // DEBUG: Log every SSE event with timestamp for diagnosing streaming delays
    {
        wxString eventType;
        switch( aEvent.type )
        {
        case AI_EVENT_TYPE::TEXT_DELTA:  eventType = wxT( "TEXT_DELTA" ); break;
        case AI_EVENT_TYPE::STATUS:     eventType = wxT( "STATUS" ); break;
        case AI_EVENT_TYPE::TITLE_UPDATE: eventType = wxT( "TITLE_UPDATE" ); break;
        case AI_EVENT_TYPE::MODE_TRANSITION: eventType = wxT( "MODE_TRANSITION" ); break;
        case AI_EVENT_TYPE::THINKING:   eventType = wxT( "THINKING" ); break;
        case AI_EVENT_TYPE::TOOL_CALL:  eventType = wxT( "TOOL_CALL" ); break;
        case AI_EVENT_TYPE::FILE_EDIT:  eventType = wxT( "FILE_EDIT" ); break;
        case AI_EVENT_TYPE::DONE:       eventType = wxT( "DONE" ); break;
        case AI_EVENT_TYPE::EVENT_ERROR: eventType = wxT( "ERROR" ); break;
        case AI_EVENT_TYPE::PHASE_UPDATE: eventType = wxT( "PHASE_UPDATE" ); break;
        case AI_EVENT_TYPE::INTERRUPT:  eventType = wxT( "INTERRUPT" ); break;
        case AI_EVENT_TYPE::TODO_UPDATE: eventType = wxT( "TODO_UPDATE" ); break;
        case AI_EVENT_TYPE::PLAN_DOCUMENT: eventType = wxT( "PLAN_DOCUMENT" ); break;
        case AI_EVENT_TYPE::PROGRESS:   eventType = wxT( "PROGRESS" ); break;
        case AI_EVENT_TYPE::METRICS:    eventType = wxT( "METRICS" ); break;
        case AI_EVENT_TYPE::AUTH_ERROR: eventType = wxT( "AUTH_ERROR" ); break;
        case AI_EVENT_TYPE::QUOTA_EXCEEDED: eventType = wxT( "QUOTA_EXCEEDED" ); break;
        case AI_EVENT_TYPE::QUOTA_WARNING: eventType = wxT( "QUOTA_WARNING" ); break;
        case AI_EVENT_TYPE::PLAN_RESTRICTED: eventType = wxT( "PLAN_RESTRICTED" ); break;
        case AI_EVENT_TYPE::VERSIONS_LIST: eventType = wxT( "VERSIONS_LIST" ); break;
        case AI_EVENT_TYPE::VERSION_SAVED: eventType = wxT( "VERSION_SAVED" ); break;
        case AI_EVENT_TYPE::VERSION_RESTORED: eventType = wxT( "VERSION_RESTORED" ); break;
        default:                        eventType = wxString::Format( wxT( "UNKNOWN(%d)" ),
                                                        static_cast<int>( aEvent.type ) ); break;
        }
        wxString preview = wxString::FromUTF8( aEvent.content ).Left( 80 );
        preview.Replace( wxT( "\n" ), wxT( " " ) );
        wxLogDebug( wxT( "[SSE_EVENT] tab=%d type=%s content='%s'" ),
                    aTabIndex, eventType, preview );
    }

    // Reset idle status timer - any event means backend is responsive
    // This restarts the 2-second countdown before showing "Working..."
    resetIdleStatusTimer( aTabIndex );

    // Track last event type so idle timer knows whether to show "Working..." or keep current status
    if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        m_tabs[aTabIndex].lastEventType = aEvent.type;
    }
    
    // Flush any buffered text before processing non-text events
    // This ensures correct ordering (text appears before status/done messages)
    if( aEvent.type != AI_EVENT_TYPE::TEXT_DELTA )
    {
        flushStreamingBuffer( aTabIndex );
    }

    switch( aEvent.type )
    {
    case AI_EVENT_TYPE::TEXT_DELTA:
    {
        if( !aEvent.content.empty() )
        {
            if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() )
                && m_tabs[aTabIndex].thinkingActive )
            {
                long long blockDur = 0;
                if( m_tabs[aTabIndex].thinkingStartMs > 0 )
                {
                    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch() ).count();
                    blockDur = nowMs - m_tabs[aTabIndex].thinkingStartMs;
                    m_tabs[aTabIndex].thinkingDurationMs += blockDur;
                    m_tabs[aTabIndex].thinkingStartMs = 0;
                }
                if( !m_tabs[aTabIndex].thinkingBlocks.empty() )
                    m_tabs[aTabIndex].thinkingBlocks.back().durationMs = blockDur;

                m_tabs[aTabIndex].thinkingActive = false;
                if( m_chatWebview && aTabIndex == m_currentTabIndex )
                    m_chatWebview->HideThinking();
            }

            bufferStreamingText( wxString::FromUTF8( aEvent.content ), aTabIndex );
        }
        break;
    }
    case AI_EVENT_TYPE::STATUS:
    {
        wxString statusContent = wxString::FromUTF8( aEvent.content );
        if( !aEvent.agentId.IsEmpty() )
            statusContent = wxString::Format( wxT( "[%s] %s" ), aEvent.agentId, statusContent );

        wxString lower = statusContent.Lower();
        bool isGenericToolStatus = lower.StartsWith( wxT( "reading file" ) )
                       || lower.StartsWith( wxT( "writing file" ) )
                       || lower.StartsWith( wxT( "editing file" ) )
                       || lower == wxT( "making changes..." )
                       || lower == wxT( "searching in files..." )
                       || lower == wxT( "looking for matches..." )
                       || lower == wxT( "listing directory..." )
                       || lower == wxT( "removing file..." )
                       || lower == wxT( "deleting file..." )
                       || lower == wxT( "thinking" )
                       || lower == wxT( "working..." )
                       || lower == wxT( "processing..." )
                       || lower == wxT( "working on it..." )
                       || lower == wxT( "still working on it..." )
                       || lower == wxT( "one moment..." );

        if( !isGenericToolStatus
            && aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            TAB_DATA& tab = m_tabs[aTabIndex];
            tab.lastStatusMessage = statusContent;
            TAB_DATA::ActivityMarker marker;
            marker.charOffset = static_cast<int>( tab.pendingStreamingResponse.length() );
            marker.label = statusContent;
            marker.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch() ).count();
            if( !tab.thinkingBlocks.empty()
                && tab.thinkingBlocks.size() > tab.lastThinkingBlockUsed )
            {
                marker.thinkingContext = tab.thinkingBlocks.back().content;
                tab.lastThinkingBlockUsed = tab.thinkingBlocks.size();
            }
            tab.activityMarkers.push_back( marker );
        }
        if( !isGenericToolStatus )
            onStreamingStatus( statusContent, aTabIndex );
        break;
    }
    case AI_EVENT_TYPE::QUOTA_WARNING:
    {
        wxString level = "warning";
        if( aEvent.data.contains( "level" ) && aEvent.data["level"].is_string() )
            level = wxString::FromUTF8( aEvent.data["level"].get<std::string>() );

        nlohmann::json warningJson;
        warningJson["type"] = "quota_warning";
        warningJson["level"] = level.ToStdString();
        warningJson["content"] = aEvent.content;
        if( aEvent.data.contains( "monthly_cost_used" ) )
            warningJson["monthly_cost_used"] = aEvent.data["monthly_cost_used"];
        if( aEvent.data.contains( "monthly_cost_cap" ) )
            warningJson["monthly_cost_cap"] = aEvent.data["monthly_cost_cap"];

        wxString script = wxString::Format(
            wxT( "window.kichat && window.kichat.onQuotaWarning && "
                 "window.kichat.onQuotaWarning(%s)" ),
            wxString::FromUTF8( warningJson.dump() ) );
        m_chatWebview->RunScriptFireAndForget( script );

        onStreamingStatus( wxString::FromUTF8( aEvent.content ), aTabIndex );
        break;
    }
    case AI_EVENT_TYPE::TITLE_UPDATE:
    {
        wxString newTitle = wxString::FromUTF8( aEvent.content );
        if( !newTitle.IsEmpty() )
        {
            // Update the specific tab
            if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
            {
                m_tabs[aTabIndex].title = newTitle;
                
                // Update webview tab title
                if( m_chatWebview )
                    m_chatWebview->SetTabTitle( m_tabs[aTabIndex].conversationId, newTitle );
                
                // Also save to DB using the tab's conversation ID
                wxString convId = m_tabs[aTabIndex].conversationId;
                if( !convId.IsEmpty() )
                {
                    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
                    db.UpdateConversationTitle( convId, newTitle );
                }
            }
        }
        break;
    }
    case AI_EVENT_TYPE::MODE_TRANSITION:
    {
        // Handle mode transition - backend is authoritative for mode
        // This is part of the Plan Mode overhaul to ensure frontend-backend sync
        wxLogDebug( "[MODE_TRANSITION] Event received: from=%s to=%s reason=%s",
                    aEvent.fromMode, aEvent.toMode, aEvent.transitionReason );
        
        if( !aEvent.toMode.empty() )
        {
            AI_MODE targetMode = AI_MODE::ASK;  // Default
            
            if( aEvent.toMode == "agent" )
            {
                targetMode = AI_MODE::AGENT;
            }
            else if( aEvent.toMode == "ask" )
            {
                targetMode = AI_MODE::ASK;
            }
            else if( aEvent.toMode == "plan" )
            {
                targetMode = AI_MODE::PLAN;
            }
            
            // Verify if this matches our expected mode (for approval flow)
            if( m_awaitingModeConfirmation )
            {
                if( targetMode == m_expectedMode )
                {
                    wxLogDebug( "[MODE_TRANSITION] Backend confirmed expected mode: %s", aEvent.toMode );
                }
                else
                {
                    wxLogWarning( "[MODE_TRANSITION] Mode mismatch! Expected %d but got %s",
                                  static_cast<int>(m_expectedMode), aEvent.toMode );
                }
                m_awaitingModeConfirmation = false;
            }
            
            // Update mode state to backend's authoritative mode
            wxLogDebug( "[MODE_TRANSITION] Switching to %s mode", aEvent.toMode );
            m_currentMode = targetMode;
            
            // Hide plan approval buttons on ANY mode transition (not just to AGENT)
            // This ensures the panel is hidden when cancelling, adjusting, etc.
            // Update webview
            if( m_chatWebview )
            {
                m_chatWebview->HidePlanApproval();
                m_chatWebview->HidePhaseIndicator();
                
                // Update mode in webview
                wxString modeStr;
                if( targetMode == AI_MODE::AGENT )
                    modeStr = wxT( "agent" );
                else if( targetMode == AI_MODE::ASK )
                    modeStr = wxT( "ask" );
                else if( targetMode == AI_MODE::PLAN )
                    modeStr = wxT( "plan" );
                m_chatWebview->SetMode( modeStr );
            }
            
            // If switching to agent mode, plan execution is complete
            if( targetMode == AI_MODE::AGENT )
            {
                m_planModeExecuting = false;
            }
            else if( targetMode == AI_MODE::ASK || targetMode == AI_MODE::PLAN )
            {
            }
        }
        else
        {
            wxLogWarning( "[MODE_TRANSITION] Event received but toMode is empty" );
        }
        break;
    }
    case AI_EVENT_TYPE::PHASE_UPDATE:
    {
        // Call the phase update handler to update UI
        nlohmann::json eventJson;
        eventJson["type"] = "phase_update";
        eventJson["content"] = aEvent.content;
        eventJson["data"] = aEvent.data;
        handlePhaseUpdateEvent( eventJson, aTabIndex );
        break;
    }
    case AI_EVENT_TYPE::PLAN_DOCUMENT:
    {
        // Call the plan document handler to display plan and approval buttons
        nlohmann::json eventJson;
        eventJson["type"] = "plan_document";
        eventJson["content"] = aEvent.content;
        eventJson["data"] = aEvent.data;
        handlePlanDocumentEvent( eventJson, aTabIndex );
        break;
    }
    case AI_EVENT_TYPE::INTERRUPT:
    {
        std::string interruptType = "";
        if( aEvent.data.contains( "interrupt_type" ) && aEvent.data["interrupt_type"].is_string() )
            interruptType = aEvent.data["interrupt_type"].get<std::string>();
        
        wxLogDebug( "[INTERRUPT] Workflow paused for user input: type=%s", interruptType );
        
        // Hide any active status indicators — the interrupt replaces whatever was pending
        if( m_chatWebview )
        {
            m_chatWebview->HideStatus();
            m_chatWebview->HideTypingIndicator();
        }
        break;
    }
    case AI_EVENT_TYPE::TOOL_CALL:
    {
        // Capture state before file-modifying tools (search_replace, write)
        if( ( aEvent.toolName == "search_replace" || aEvent.toolName == "write" ) 
            && m_aiEditInProgress && !m_aiEditStateCaptured )
        {
            wxString filePath = GetCurrentFileName();
            if( m_frame && !filePath.IsEmpty() )
            {
                CaptureStateForAIEdit( filePath );
                m_aiEditStateCaptured = true;
            }
        }

        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_tabs[aTabIndex].toolCallCount++;
            if( aEvent.toolName == "search_replace" || aEvent.toolName == "write" )
                m_tabs[aTabIndex].fileEditCount++;

            TAB_DATA::ActivityMarker marker;
            marker.charOffset = static_cast<int>( m_tabs[aTabIndex].pendingStreamingResponse.length() );

            wxString tName = wxString::FromUTF8( aEvent.toolName );

            wxString fName;
            try
            {
                if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "file_path" )
                    && aEvent.toolArgs["file_path"].is_string() )
                {
                    wxString fp = wxString::FromUTF8( aEvent.toolArgs["file_path"].get<std::string>() );
                    wxFileName fn( fp );
                    fName = fn.GetFullName();
                    fName.Replace( wxT( "kicad_" ), wxT( "trace_" ) );
                }
            }
            catch( ... ) {}

            if( fName.IsEmpty() )
            {
                wxString cur = GetCurrentFileName();
                if( !cur.IsEmpty() )
                {
                    wxFileName fn( cur );
                    fName = fn.GetFullName();
                    fName.Replace( wxT( "kicad_" ), wxT( "trace_" ) );
                }
            }

            if( !fName.IsEmpty() )
            {
                if( tName == "read" || tName == "read_file" )
                    marker.label = wxString::Format( wxT( "Read %s" ), fName );
                else if( tName == "write" || tName == "write_file" )
                    marker.label = wxString::Format( wxT( "Wrote %s" ), fName );
                else if( tName == "search_replace" )
                    marker.label = wxString::Format( wxT( "Edited %s" ), fName );
                else
                    marker.label = wxString::Format( wxT( "Used %s" ), tName );
            }
            else
            {
                if( tName == "list_directory" || tName == "list_dir" )
                {
                    wxString dirLabel;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "path" )
                            && aEvent.toolArgs["path"].is_string() )
                        {
                            wxString dirPath = wxString::FromUTF8(
                                aEvent.toolArgs["path"].get<std::string>() );
                            wxFileName fn( dirPath );
                            dirLabel = fn.GetFullName();
                            if( dirLabel.IsEmpty() )
                                dirLabel = fn.GetDirs().IsEmpty() ? dirPath
                                    : fn.GetDirs().Last();
                        }
                    }
                    catch( ... ) {}
                    marker.label = dirLabel.IsEmpty()
                        ? wxString( wxT( "Browsed components" ) )
                        : wxString::Format( wxT( "Browsed %s" ), dirLabel );
                }
                else if( tName == "search" || tName == "grep" )
                {
                    wxString pattern;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "pattern" )
                            && aEvent.toolArgs["pattern"].is_string() )
                        {
                            pattern = wxString::FromUTF8(
                                aEvent.toolArgs["pattern"].get<std::string>() );
                            if( pattern.length() > 30 )
                                pattern = pattern.Left( 27 ) + wxT( "..." );
                        }
                    }
                    catch( ... ) {}
                    marker.label = pattern.IsEmpty()
                        ? wxString( wxT( "Searched" ) )
                        : wxString::Format( wxT( "Searched \"%s\"" ), pattern );
                }
                else if( tName == "search_footprints" )
                {
                    wxString query;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "queries" ) )
                        {
                            auto& q = aEvent.toolArgs["queries"];
                            if( q.is_string() )
                                query = wxString::FromUTF8( q.get<std::string>() );
                            else if( q.is_array() && !q.empty() && q[0].is_string() )
                                query = wxString::FromUTF8( q[0].get<std::string>() );
                        }
                    }
                    catch( ... ) {}
                    marker.label = query.IsEmpty()
                        ? wxString( wxT( "Searched footprints" ) )
                        : wxString::Format( wxT( "Found footprints \"%s\"" ), query );
                }
                else
                    marker.label = wxString::Format( wxT( "Used %s" ), tName );
            }

            marker.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch() ).count();

            // Only attach thinking context if a NEW thinking block has arrived since last capture
            if( !m_tabs[aTabIndex].thinkingBlocks.empty()
                && m_tabs[aTabIndex].thinkingBlocks.size() > m_tabs[aTabIndex].lastThinkingBlockUsed )
            {
                marker.thinkingContext = m_tabs[aTabIndex].thinkingBlocks.back().content;
                m_tabs[aTabIndex].lastThinkingBlockUsed = m_tabs[aTabIndex].thinkingBlocks.size();
            }

            m_tabs[aTabIndex].activityMarkers.push_back( marker );
        }

        AMPLITUDE_CLIENT::Instance().Track( "ai_tool_executed", {
            { "tool_name", aEvent.toolName },
            { "app_type", std::string( GetAppType().utf8_str() ) }
        });

        if( m_chatWebview && aTabIndex == m_currentTabIndex )
        {
            wxString toolName = wxString::FromUTF8( aEvent.toolName );

            wxString fileName;
            try
            {
                if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "file_path" )
                    && aEvent.toolArgs["file_path"].is_string() )
                {
                    wxString fullPath = wxString::FromUTF8( aEvent.toolArgs["file_path"].get<std::string>() );
                    wxFileName fn( fullPath );
                    fileName = fn.GetFullName();
                    fileName.Replace( wxT( "kicad_" ), wxT( "trace_" ) );
                }
            }
            catch( ... ) {}

            if( fileName.IsEmpty() )
            {
                wxString currentFile = GetCurrentFileName();
                if( !currentFile.IsEmpty() )
                {
                    wxFileName fn( currentFile );
                    fileName = fn.GetFullName();
                    fileName.Replace( wxT( "kicad_" ), wxT( "trace_" ) );
                }
            }

            wxString statusText;
            if( !fileName.IsEmpty() )
            {
                if( toolName == "read" || toolName == "read_file" )
                    statusText = wxString::Format( wxT( "Reading %s..." ), fileName );
                else if( toolName == "write" || toolName == "write_file" )
                    statusText = wxString::Format( wxT( "Writing %s..." ), fileName );
                else if( toolName == "search_replace" )
                    statusText = wxString::Format( wxT( "Editing %s..." ), fileName );
                else
                    statusText = wxString::Format( wxT( "Using %s..." ), toolName );
            }
            else
            {
                if( toolName == "list_directory" || toolName == "list_dir" )
                {
                    wxString dirLabel;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "path" )
                            && aEvent.toolArgs["path"].is_string() )
                        {
                            wxString dirPath = wxString::FromUTF8(
                                aEvent.toolArgs["path"].get<std::string>() );
                            wxFileName fn( dirPath );
                            dirLabel = fn.GetFullName();
                            if( dirLabel.IsEmpty() )
                                dirLabel = fn.GetDirs().IsEmpty() ? dirPath
                                    : fn.GetDirs().Last();
                        }
                    }
                    catch( ... ) {}
                    statusText = dirLabel.IsEmpty()
                        ? wxString( wxT( "Browsing components..." ) )
                        : wxString::Format( wxT( "Browsing %s..." ), dirLabel );
                }
                else if( toolName == "search" || toolName == "grep" )
                {
                    wxString pattern;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "pattern" )
                            && aEvent.toolArgs["pattern"].is_string() )
                        {
                            pattern = wxString::FromUTF8(
                                aEvent.toolArgs["pattern"].get<std::string>() );
                            if( pattern.length() > 30 )
                                pattern = pattern.Left( 27 ) + wxT( "..." );
                        }
                    }
                    catch( ... ) {}
                    statusText = pattern.IsEmpty()
                        ? wxString( wxT( "Searching..." ) )
                        : wxString::Format( wxT( "Searching \"%s\"..." ), pattern );
                }
                else if( toolName == "search_footprints" )
                {
                    wxString query;
                    try
                    {
                        if( aEvent.toolArgs.is_object() && aEvent.toolArgs.contains( "queries" ) )
                        {
                            auto& q = aEvent.toolArgs["queries"];
                            if( q.is_string() )
                                query = wxString::FromUTF8( q.get<std::string>() );
                            else if( q.is_array() && !q.empty() && q[0].is_string() )
                                query = wxString::FromUTF8( q[0].get<std::string>() );
                        }
                    }
                    catch( ... ) {}
                    statusText = query.IsEmpty()
                        ? wxString( wxT( "Searching footprints..." ) )
                        : wxString::Format( wxT( "Searching footprints \"%s\"..." ), query );
                }
                else if( toolName.IsEmpty() )
                    statusText = wxT( "Running tool..." );
                else
                    statusText = wxString::Format( wxT( "Using %s..." ), toolName );
            }

            m_chatWebview->ShowStatus( statusText );
        }
        break;
    }
    case AI_EVENT_TYPE::FILE_EDIT:
    {
        HandleFileEditEvent( aEvent, aTabIndex );

        // Send diff info to the React webview for search_replace edits
        if( m_chatWebview && aTabIndex == m_currentTabIndex
            && aEvent.toolName == "search_replace" )
        {
            wxString filename;
            wxString oldStr;
            wxString newStr;
            try
            {
                if( aEvent.toolArgs.is_object() )
                {
                    if( aEvent.toolArgs.contains( "file_path" )
                        && aEvent.toolArgs["file_path"].is_string() )
                    {
                        wxString fp = wxString::FromUTF8(
                            aEvent.toolArgs["file_path"].get<std::string>() );
                        wxFileName fn( fp );
                        filename = fn.GetFullName();
                        filename.Replace( wxT( "kicad_" ), wxT( "trace_" ) );
                    }
                    if( aEvent.toolArgs.contains( "old_str" )
                        && aEvent.toolArgs["old_str"].is_string() )
                    {
                        oldStr = wxString::FromUTF8(
                            aEvent.toolArgs["old_str"].get<std::string>() );
                    }
                    if( aEvent.toolArgs.contains( "new_str" )
                        && aEvent.toolArgs["new_str"].is_string() )
                    {
                        newStr = wxString::FromUTF8(
                            aEvent.toolArgs["new_str"].get<std::string>() );
                    }
                }
            }
            catch( ... ) {}

            if( !filename.IsEmpty() && ( !oldStr.IsEmpty() || !newStr.IsEmpty() ) )
                m_chatWebview->ShowFileEdit( filename, oldStr, newStr );
        }
        break;
    }
    case AI_EVENT_TYPE::TODO_UPDATE:
    {
        if( m_chatWebview && aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            m_chatWebview->SetTodos( wxString::FromUTF8( aEvent.content ),
                                      m_tabs[aTabIndex].conversationId );
        }
        break;
    }
    case AI_EVENT_TYPE::PROGRESS:
    {
        // Progress events with expandable sections are handled by the webview
        // Native panel expandable sections were removed with the native UI
        break;
    }
    case AI_EVENT_TYPE::METRICS:
    {
        break;
    }
    case AI_EVENT_TYPE::THINKING:
    {
        if( aEvent.content.empty() )
            break;

        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            if( m_tabs[aTabIndex].thinkingStartMs == 0 )
            {
                m_tabs[aTabIndex].thinkingStartMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch() ).count();
            }
            m_tabs[aTabIndex].thinkingActive = true;
            m_tabs[aTabIndex].accumulatedThinking += wxString::FromUTF8( aEvent.content );

            auto& blocks = m_tabs[aTabIndex].thinkingBlocks;
            int curOffset = static_cast<int>( m_tabs[aTabIndex].pendingStreamingResponse.length() );

            // Start a new block when: first block, previous block was finalized (durationMs > 0),
            // or agent_id changed (different sub-agent's thinking must not merge)
            wxString currentAgentId = aEvent.agentId;
            bool needNewBlock = blocks.empty()
                || blocks.back().durationMs > 0
                || blocks.back().agentId != currentAgentId;
            if( needNewBlock )
            {
                TAB_DATA::ThinkingBlock blk;
                blk.content = wxString::FromUTF8( aEvent.content );
                blk.charOffset = curOffset;
                blk.durationMs = 0;
                blk.agentId = currentAgentId;
                blocks.push_back( blk );
            }
            else
            {
                blocks.back().content += wxString::FromUTF8( aEvent.content );
            }
        }

        if( m_chatWebview && aTabIndex == m_currentTabIndex )
        {
            m_chatWebview->HideTypingIndicator();
            m_chatWebview->ShowThinking( wxString::FromUTF8( aEvent.content ), aEvent.agentId );
        }
        break;
    }
    case AI_EVENT_TYPE::EVENT_ERROR:
    {
        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            auto& tab = m_tabs[aTabIndex];
            if( tab.thinkingActive && tab.thinkingStartMs > 0
                && !tab.thinkingBlocks.empty() && tab.thinkingBlocks.back().durationMs == 0 )
            {
                long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch() ).count();
                tab.thinkingBlocks.back().durationMs = nowMs - tab.thinkingStartMs;
                tab.thinkingDurationMs += tab.thinkingBlocks.back().durationMs;
                tab.thinkingStartMs = 0;
            }
            tab.thinkingActive = false;
        }
        if( m_chatWebview )
            m_chatWebview->HideThinking();
        onBackendResponse( wxString::FromUTF8( aEvent.error ), false, aTabIndex );
        break;
    }
    case AI_EVENT_TYPE::AUTH_ERROR:
    {
        AUTH_MANAGER::Instance().SignOut();
        updateAuthUI();
        onBackendResponse( wxT( "Session expired. Please sign in again." ), false, aTabIndex, false );
        break;
    }
    case AI_EVENT_TYPE::QUOTA_EXCEEDED:
    {
        // Show upgrade message for quota exceeded (402)
        wxString upgradeMsg = wxT( "⚠️ **Plan Limit Reached**\n\n" );
        upgradeMsg += wxT( "You've used all your AI requests for this billing period.\n\n" );
        upgradeMsg += wxT( "**[Upgrade your plan](https://buildwithtrace.com/dashboard/pricing)** to continue using Trace AI." );
        onBackendResponse( upgradeMsg, false, aTabIndex, false );
        break;
    }
    case AI_EVENT_TYPE::PLAN_RESTRICTED:
    {
        // Show upgrade message for plan-restricted features (403)
        wxString upgradeMsg = wxT( "⚠️ **Paid Plan Required**\n\n" );
        upgradeMsg += wxT( "You're on the **Free plan**. Trace AI requires a paid subscription.\n\n" );
        upgradeMsg += wxT( "**[Upgrade to Pro](https://buildwithtrace.com/dashboard/pricing)** to unlock:\n" );
        upgradeMsg += wxT( "• AI-powered schematic design\n" );
        upgradeMsg += wxT( "• Automated PCB layout\n" );
        upgradeMsg += wxT( "• Component selection assistance\n" );
        upgradeMsg += wxT( "• And much more..." );
        onBackendResponse( upgradeMsg, false, aTabIndex, false );
        break;
    }
    case AI_EVENT_TYPE::DONE:
        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
        {
            auto& tab = m_tabs[aTabIndex];
            if( tab.thinkingActive && tab.thinkingStartMs > 0
                && !tab.thinkingBlocks.empty() && tab.thinkingBlocks.back().durationMs == 0 )
            {
                long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch() ).count();
                tab.thinkingBlocks.back().durationMs = nowMs - tab.thinkingStartMs;
                tab.thinkingDurationMs += tab.thinkingBlocks.back().durationMs;
                tab.thinkingStartMs = 0;
            }
            tab.thinkingActive = false;
        }
        if( m_chatWebview )
            m_chatWebview->HideThinking();
        break;
    case AI_EVENT_TYPE::VERSIONS_LIST:
    case AI_EVENT_TYPE::VERSION_SAVED:
    case AI_EVENT_TYPE::VERSION_RESTORED:
        // These are handled in the completion callback
        break;
    }
}


void AI_CHAT_PANEL_BASE::sendToBackendAsync( const wxString& aMessage,
                                               const std::vector<ChatAttachment>& aAttachments )
{
    // Per-tab streaming check for parallel execution
    int tabIndex = m_currentTabIndex;
    if( tabIndex < 0 || tabIndex >= static_cast<int>( m_tabs.size() ) )
    {
        wxLogError( wxT( "Invalid tab index for sending message" ) );
        return;
    }
    
    TAB_DATA& tab = m_tabs[tabIndex];
    
    // Check if THIS tab is already streaming (per-tab check for true parallelism)
    bool wasStreaming = tab.isStreaming.exchange( true );
    if( wasStreaming )
    {
        return;
    }

    // Reset per-tab streaming state
    tab.stopRequested.store( false );
    tab.pendingStreamingResponse.Clear();
    tab.accumulatedThinking.Clear();
    tab.thinkingStartMs = 0;
    tab.thinkingDurationMs = 0;
    tab.activityMarkers.clear();
    tab.thinkingBlocks.clear();
    tab.lastThinkingBlockUsed = 0;
    tab.researchSummary.Clear();
    tab.currentPhase.Clear();
    m_streamingTabIndex = tabIndex;  // Track which tab started this stream

    // Reset streaming buffer state for new request (per-tab)
    flushStreamingBuffer( tabIndex );  // Flush any remaining buffered content from previous request
    tab.streamingBuffer.Clear();
    tab.pendingDeltaCount = 0;
    tab.isFirstStreamingFlush = true;  // Next flush will be the first of this new stream
    if( m_streamingFlushTimer && m_streamingFlushTimer->IsRunning() )
    {
        m_streamingFlushTimer->Stop();
    }

    m_aiEditInProgress = true;
    m_aiEditStateCaptured = false;
    m_batchUpdatePending.store( false );        // Reset batch update flag for new stream
    
    // Reset per-tab file modification tracking for this stream
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        m_tabs[m_currentTabIndex].fileModifiedDuringStream.store( false );
        m_tabs[m_currentTabIndex].modifiedFiles.clear();
    }
    
    // Reset conversion tracking for this tab's new session
    if( tab.toolExecutor )
    {
        tab.toolExecutor->ResetConversionState();
    }
    
    // Track which file this tab is working on (for per-tab state management)
    // NOTE: We DON'T block if another tab is editing - instead we let the tool executor
    // handle conflicts gracefully using file hashing and fuzzy matching.
    // This gives Cursor-like seamless concurrent editing where:
    // - Edits are applied in order (via file locks and conversion queue)
    // - File changes are detected via hash comparison
    // - The AI can retry if content doesn't match (fuzzy matching helps)
    wxString currentFile = GetCurrentFileName();
    if( !currentFile.IsEmpty() )
    {
        // Soft ownership - just tracks which tab is working on what, no blocking
        claimFileOwnership( currentFile, m_currentTabIndex );
    }

    // Switch Send button to Stop button while AI is responding
    updateButtonState( true );

    // Update webview: add user message and show typing indicator
    if( m_chatWebview )
    {
        // Use the same ID that was saved to SQLite in sendMessageFromWebview
        wxString userMsgId = tab.pendingUserMsgId;
        if( userMsgId.IsEmpty() )
            userMsgId = wxString::Format( wxT( "user_%ld" ), wxGetLocalTime() );
        tab.pendingUserMsgId.Clear();
        m_chatWebview->AddMessage( userMsgId, wxT( "user" ), aMessage, aAttachments );
        
        // Show typing indicator and disable input
        m_chatWebview->ShowTypingIndicator();
        m_chatWebview->SetInputEnabled( false );
    }

    // Start idle status timer - will show "Working..." if no events for 2 seconds
    tab.lastEventType = AI_EVENT_TYPE::TEXT_DELTA;
    tab.lastStatusMessage.Clear();
    resetIdleStatusTimer( tabIndex );

    // Generate session ID if not set (GLOBAL - shared by all tabs in this app session)
    if( m_sessionId.IsEmpty() )
    {
        m_sessionId = CONVERSATION_DB::GenerateUUID();
    }

    // Proactively refresh token if expiring soon
    if( AUTH_MANAGER::Instance().IsAuthenticated() )
    {
        if( AUTH_MANAGER::Instance().IsTokenExpiringSoon() )
        {
            if( !AUTH_MANAGER::Instance().RefreshAccessToken() )
            {
                // Proactive token refresh failed - will sign out on 401
            }
        }
    }

    // Ensure schematic is saved before AI can read it
    wxString filePath = EnsureFileSavedForAI();
    
    // Get trace file path if kicad file path provided
    wxString traceFilePath = filePath;
    wxString kicadFilePath = filePath;
    if( !filePath.IsEmpty() )
    {
        traceFilePath = ConvertToTraceFile( filePath );
    }

    // Get auth tokens
    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    wxString refreshToken = AUTH_MANAGER::Instance().GetRefreshToken();

    // Create per-tab backend client AND tool executor for TRUE parallel execution
    // Each tab gets its own HTTP client and tool executor so streams don't block each other
    if( !tab.backendClient )
    {
        tab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( m_backendUrl.ToStdString() );
    }
    
    // Ensure tab has its own tool executor (prevents file access deadlocks between tabs!)
    if( !tab.toolExecutor )
    {
        tab.toolExecutor = std::make_unique<AI_TOOL_EXECUTOR>();
        configureToolExecutor( tab.toolExecutor.get() );  // Apply all callbacks
        tab.backendClient->SetToolExecutor( tab.toolExecutor.get() );
    }
    else
    {
        // Update app type in case it changed
        tab.toolExecutor->SetAppType( GetAppType().ToStdString() );
    }
    
    // Set allowed directories based on current project (security sandbox)
    if( !currentFile.IsEmpty() && tab.toolExecutor )
    {
        wxFileName fn( currentFile );
        wxString projectDir = fn.GetPath();
        if( !projectDir.IsEmpty() )
        {
            tab.toolExecutor->ClearAllowedProjectDirs();
            tab.toolExecutor->AddAllowedProjectDir( projectDir.ToStdString() );
        }
    }
    
    AI_BACKEND_CLIENT* tabClient = tab.backendClient.get();

    // Set up event callback for streaming (uses per-tab client)
    // NOTE: Use conversationId instead of tabIndex to avoid stale index issues during tab closure/shutdown
    auto panelAlive = m_panelAlive;
    wxString convId = tab.conversationId;
    tabClient->SetEventCallback( [this, panelAlive, convId]( const AI_BACKEND_EVENT& aEvent )
    {
        if( !panelAlive->load() )
            return;

        safeCallAfter( [this, convId, aEvent]()
        {
            // Find tab by conversation ID (indices may have shifted if tabs were closed)
            TAB_DATA* tab = findTabByConversationId( convId );
            if( !tab )
                return;  // Tab was closed while streaming
            
            // Check if this tab's stream was stopped
            if( tab->stopRequested.load() )
                return;
            
            // Find the tab index for handleBackendEventDirect
            int tabIndex = -1;
            for( size_t i = 0; i < m_tabs.size(); i++ )
            {
                if( &m_tabs[i] == tab )
                {
                    tabIndex = static_cast<int>( i );
                    break;
                }
            }
            
            if( tabIndex < 0 )
                return;  // Tab not found (shouldn't happen, but be safe)
            
            // Pass tabIndex directly for parallel streaming safety
            // No need to set global m_streamingTabIndex - each event knows its tab
            handleBackendEventDirect( aEvent, tabIndex );
        } );
    } );

    // Run streaming in background thread
    // IMPORTANT: Use per-tab session/conversation IDs for parallel execution safety
    std::string message = aMessage.ToStdString();
    std::string traceFile = traceFilePath.ToStdString();
    std::string kicadFile = kicadFilePath.ToStdString();
    std::string sessionId = m_sessionId.ToStdString();          // App-global session ID (shared by all tabs)
    std::string conversationId = tab.conversationId.ToStdString(); // Per-tab conversation ID
    std::string mode;
    switch( m_currentMode )
    {
    case AI_MODE::ASK:   mode = "ask";   break;
    case AI_MODE::PLAN:  mode = "plan";  break;
    case AI_MODE::AGENT: mode = "agent"; break;
    default:             mode = "ask";   break;
    }
    std::string appType = GetAppType().ToStdString();
    std::string auth = authToken.ToStdString();
    std::string refresh = refreshToken.ToStdString();

    // Copy attachments for thread capture
    std::vector<ChatAttachment> attachments = aAttachments;

    // Join previous request thread for this tab if still running (shouldn't happen, but be safe)
    if( tab.requestThread && tab.requestThread->joinable() )
    {
        tab.requestThread->detach(); // Detach old thread to prevent blocking UI
    }

    // Create per-tab request thread for TRUE parallel execution
    // Each tab uses its own backend client so multiple tabs can stream simultaneously
    // NOTE: Use conversationId instead of tabIndex to avoid stale index issues during tab closure/shutdown
    AI_CHAT_PANEL_BASE* self = this;  // Capture for CallAfter
    tab.requestThread = std::make_unique<std::thread>( [self, panelAlive, tabClient, message, traceFile, kicadFile, sessionId, conversationId,
                  mode, appType, auth, refresh, attachments]()
    {
        // Use per-tab backend client for parallel streaming
        AI_STREAM_RESULT result = tabClient->StreamChat(
                message, traceFile, kicadFile, sessionId, conversationId,
                mode, appType, auth, refresh, attachments );

        AMPLITUDE_CLIENT::Instance().Track( "ai_chat_completed", {
            { "mode", mode },
            { "app_type", appType },
            { "success", result.status == "success" },
            { "error_type", result.status != "success" ? result.status : "" },
            { "error_message", result.error },
        } );

        if( !panelAlive->load() )
            return;

        // Handle completion on UI thread
        // NOTE: Use conversationId to find tab, not tabIndex, because indices can shift if tabs are closed
        wxString convId = wxString::FromUTF8( conversationId );
        self->CallAfter( [self, panelAlive, convId, result, mode, appType]()
        {
            if( !panelAlive->load() || self->m_isDestroying.load() )
                return;

            // Find tab by conversation ID (indices may have shifted if tabs were closed)
            TAB_DATA* tab = self->findTabByConversationId( convId );
            if( !tab )
                return;  // Tab was closed while streaming

            // Find the tab index for functions that need it
            int tabIndex = -1;
            for( size_t i = 0; i < self->m_tabs.size(); i++ )
            {
                if( &self->m_tabs[i] == tab )
                {
                    tabIndex = static_cast<int>( i );
                    break;
                }
            }
            
            if( tabIndex < 0 )
                return;  // Tab not found (shouldn't happen, but be safe)

            // Flush any remaining buffered text before finalizing (for this tab)
            self->flushStreamingBuffer( tabIndex );

            // Finalize last thinking block duration if stream ended while still thinking
            if( tab->thinkingActive && !tab->thinkingBlocks.empty() )
            {
                long long blockDur = 0;
                if( tab->thinkingStartMs > 0 )
                {
                    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch() ).count();
                    blockDur = nowMs - tab->thinkingStartMs;
                    tab->thinkingDurationMs += blockDur;
                    tab->thinkingStartMs = 0;
                }
                tab->thinkingBlocks.back().durationMs = blockDur;
                tab->thinkingActive = false;
            }

            // Save AI response to local database using per-tab data
            wxString streamedResponse = tab->pendingStreamingResponse;
            bool hasThinkingData = !tab->thinkingBlocks.empty();
            bool hasActivityData = !tab->activityMarkers.empty();

            if( !convId.IsEmpty()
                && ( !streamedResponse.IsEmpty() || hasThinkingData || hasActivityData ) )
            {
                CONVERSATION_DB& db = CONVERSATION_DB::Instance();
                
                nlohmann::json metaJson = buildAssistantMetadataJson(
                    tab, wxString::FromUTF8( mode ), wxString::FromUTF8( appType ) );
                wxLogDebug( "SAVE: %zu activity_markers saved for conv %s",
                            tab->activityMarkers.size(), convId );
                wxString msgMeta = wxString::FromUTF8( metaJson.dump() );
                auto msg = db.SaveMessage( convId, wxT( "assistant" ), streamedResponse, msgMeta );
                if( msg.has_value() )
                {
                    tab->lastSavedDbMessageId = msg->id;
                }
                else
                {
                    wxLogError( wxT( "Failed to save assistant message to database for conversation: %s" ), convId );
                }
            }

            // Clear per-tab streaming response for next turn
            tab->pendingStreamingResponse.Clear();
            tab->accumulatedThinking.Clear();
            tab->activityMarkers.clear();
            tab->thinkingBlocks.clear();
            tab->thinkingDurationMs = 0;
            tab->lastThinkingBlockUsed = 0;
            tab->researchSummary.Clear();
            tab->currentPhase.Clear();

            bool success = ( result.status == "success" || result.status == "stopped" );
            wxString error;
            if( result.status == "error" )
                error = wxString::FromUTF8( result.error );
            
            // Check quota after successful request - only shows banner if quota is LOW
            if( success )
            {
                self->fetchAndShowQuotaInfo( false );  // false = not startup, only show if low quota
            }
            
            // CRITICAL: Reset per-tab streaming state BEFORE calling onBackendResponse
            // This allows isAnyTabStreaming() to return false so the button resets properly
            tab->isStreaming.store( false );
            tab->stopRequested.store( false );
            tab->streamingBuffer.Clear();
            tab->pendingDeltaCount = 0;
            
            // Stop idle status timer for this tab
            self->stopIdleStatusTimer( tabIndex );
            
            // Release file ownership for this tab (allows other tabs to edit these files)
            self->releaseFileOwnership( tabIndex );

            if( result.status == "auth_error" )
            {
                AUTH_MANAGER::Instance().SignOut();
                self->updateAuthUI();
                self->onBackendResponse( wxT( "Session expired. Please sign in again." ), false, tabIndex, false );
            }
            else if( result.status == "quota_exceeded" )
            {
                // Quota exhausted (402) - user hit daily/credit limit
                // Show banner with full info, no chat message needed
                wxString bannerMsg;
                if( !result.error.empty() )
                {
                    bannerMsg = wxString::FromUTF8( result.error );
                }
                else
                {
                    bannerMsg = wxT( "Request limit reached." );
                }
                bannerMsg += wxT( " Try Ask mode or upgrade." );
                
                self->showQuotaBanner( bannerMsg, true );  // true = show upgrade button
                // No chat message - banner has all the info
            }
            else if( result.status == "plan_restricted" )
            {
                // Plan restricted (403) - free user tried Agent/Plan mode
                // Auto-switch to Ask mode and show banner with full info
                wxString bannerMsg = wxT( "Free plan - Ask mode only. Upgrade for Agent & Plan." );
                
                self->showQuotaBanner( bannerMsg, true );  // true = show upgrade button
                
                // Auto-switch to Ask mode on the UI thread
                self->m_currentMode = AI_MODE::ASK;
                auto panelAliveInner = self->m_panelAlive;
                self->CallAfter( [self, panelAliveInner]() {
                    if( !panelAliveInner->load() || self->m_isDestroying.load() )
                        return;
                    if( self->m_chatWebview )
                        self->m_chatWebview->SetMode( wxT( "ask" ) );
                });
            }
            else
            {
                self->onBackendResponse( wxString::FromUTF8( result.response ), success, tabIndex, result.fileModified );
            }

            // Handle final cleanup after stream completes
            // Stop the batch update timer if running
            if( self->m_streamingBatchTimer && self->m_streamingBatchTimer->IsRunning() )
            {
                self->m_streamingBatchTimer->Stop();
            }

            // Clear any pending flags (they may have been consumed by batch timer)
            bool hadBatchPending = self->m_batchUpdatePending.exchange( false );
            bool hadFileModified = result.fileModified || tab->fileModifiedDuringStream.exchange( false );

            // ALWAYS flush pending conversions at stream end
            // This is critical because conversions are debounced and may not have happened yet
            bool conversionHappened = false;
            if( tab->toolExecutor )
            {
                conversionHappened = tab->toolExecutor->flushPendingConversion( true );
                wxLogDebug( wxT( "AI DEBUG [stream end]: flushPendingConversion returned %s" ), 
                           conversionHappened ? wxT( "true" ) : wxT( "false" ) );
            }
            
            // Reload if: conversion happened OR batch was pending OR file was modified
            bool needsReload = conversionHappened || hadBatchPending || hadFileModified;
            wxLogDebug( wxT( "AI DEBUG [stream end]: needsReload=%s (conversionHappened=%s, hadBatchPending=%s, hadFileModified=%s)" ),
                       needsReload ? wxT( "true" ) : wxT( "false" ),
                       conversionHappened ? wxT( "true" ) : wxT( "false" ),
                       hadBatchPending ? wxT( "true" ) : wxT( "false" ),
                       hadFileModified ? wxT( "true" ) : wxT( "false" ) );
            
            if( needsReload )
            {
                wxString filePath = self->GetCurrentFileName();
                wxLogDebug( wxT( "AI DEBUG [stream end]: Reloading from file: %s" ), filePath );
                
                if( !filePath.IsEmpty() )
                {
                    // Use mutex to prevent concurrent reload operations
                    std::lock_guard<std::mutex> lock( self->m_reloadMutex );

                    // Capture state if not already captured
                    if( self->m_aiEditInProgress && !self->m_aiEditStateCaptured )
                    {
                        self->CaptureStateForAIEdit( filePath );
                        self->m_aiEditStateCaptured = true;
                    }

                    // Final reload and create undo entries
                    if( self->ReloadFromFile( filePath ) )
                    {
                        wxLogDebug( wxT( "AI DEBUG [stream end]: ReloadFromFile succeeded" ) );
                        self->CompareAndCreateAIEditUndoEntries();
                        
                        // Autoplace fields for modified symbols
                        if( tab->toolExecutor )
                        {
                            std::set<std::string> modifiedUUIDs = tab->toolExecutor->GetModifiedSymbolUUIDs();
                            if( !modifiedUUIDs.empty() )
                            {
                                self->AutoplaceModifiedSymbols( modifiedUUIDs );
                                tab->toolExecutor->ClearModifiedSymbolUUIDs();
                            }
                        }
                        
                        // Auto-annotate all symbols after trace edits
                        self->AnnotateAllSymbols();
                        
                        // Only save document if conversion succeeded (or no conversion was needed)
                        // If conversion failed, saving would trigger kicad-to-trace conversion
                        // which would overwrite the AI's edit to the trace_sch file
                        bool conversionSucceeded = !conversionHappened || 
                            ( tab->toolExecutor && tab->toolExecutor->WasLastConversionSuccessful() );
                        
                        wxLogDebug( wxT( "AI DEBUG [stream end]: conversionSucceeded=%s (conversionHappened=%s, WasLastConversionSuccessful=%s)" ),
                                   conversionSucceeded ? wxT( "true" ) : wxT( "false" ),
                                   conversionHappened ? wxT( "true" ) : wxT( "false" ),
                                   ( tab->toolExecutor && tab->toolExecutor->WasLastConversionSuccessful() ) ? wxT( "true" ) : wxT( "false" ) );
                        
                        if( conversionSucceeded )
                        {
                            wxLogDebug( wxT( "AI DEBUG [stream end]: Calling SaveDocument()" ) );
                            // Save document to persist annotation changes
                            self->SaveDocument();
                            self->MarkDocumentAsSaved();
                        }
                        else
                        {
                            wxLogDebug( wxT( "AI DEBUG [stream end]: SKIPPING SaveDocument() due to conversion failure" ) );
                        }
                    }
                    else
                    {
                        wxLogDebug( wxT( "AI DEBUG [stream end]: ReloadFromFile FAILED" ) );
                    }
                }
            }

            // Update conversation ID if returned

            // Save schematic version after successful file modification
            if( hadFileModified )
            {
                self->saveVersionToDatabase( wxT( "AI edit" ), tabIndex );

                wxString versionId = self->GetLastSavedVersionId( tabIndex );
                if( !versionId.IsEmpty() && !tab->lastSavedDbMessageId.IsEmpty() )
                {
                    wxString existing = CONVERSATION_DB::Instance().GetMessageMetadata(
                        tab->lastSavedDbMessageId );
                    nlohmann::json mj;
                    if( !existing.IsEmpty() )
                    {
                        try { mj = nlohmann::json::parse( std::string( existing.utf8_str() ) ); }
                        catch( ... ) {}
                    }
                    mj["version_id"] = std::string( versionId.utf8_str() );
                    wxString merged = wxString::FromUTF8( mj.dump() );
                    CONVERSATION_DB::Instance().UpdateMessageMetadata(
                        tab->lastSavedDbMessageId, merged );
                }
            }
            
            // Note: Per-tab streaming state was already reset BEFORE onBackendResponse()
            // to ensure isAnyTabStreaming() returns false so the button resets properly
        } );
    } );
}


void AI_CHAT_PANEL_BASE::onStreamingText( const wxString& aText, bool aIsFirst, int aTabIndex )
{
    // Critical: Don't touch UI if panel is being destroyed (prevents crash from queued callbacks)
    if( m_isDestroying.load() )
        return;

    // Validate tab index (per-tab safety - uses passed index, not global)
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    TAB_DATA& tab = m_tabs[aTabIndex];
    
    // Safety check: Don't process if this tab's stream was stopped
    if( tab.stopRequested.load() )
        return;

    // Accumulate response in per-tab storage (not global - supports parallel streaming)
    if( aIsFirst )
    {
        tab.pendingStreamingResponse = aText;
        
        // Generate a NEW unique message ID for this streaming message
        // This prevents appending to old messages from previous streams
        tab.streamingMessageCounter++;
        tab.streamingMessageId = wxString::Format( wxT( "stream_%s_%d" ), 
                                                    tab.conversationId, 
                                                    tab.streamingMessageCounter );
    }
    else
    {
        tab.pendingStreamingResponse += aText;
    }
    
    tab.isStreaming.store( true );

    // Update webview (if this is the active tab)
    if( m_chatWebview && aTabIndex == m_currentTabIndex )
    {
        // Use the unique message ID stored in the tab
        wxString msgId = tab.streamingMessageId;
        
        // Always hide status when text arrives (status may have been shown by tool calls)
        m_chatWebview->HideStatus();
        
        if( aIsFirst )
        {
            m_chatWebview->HideTypingIndicator();
            m_chatWebview->AddMessage( msgId, wxT( "assistant" ), aText );
        }
        else
        {
            m_chatWebview->AppendToMessage( msgId, aText );
        }
    }
}


void AI_CHAT_PANEL_BASE::bufferStreamingText( const wxString& aText, int aTabIndex )
{
    if( aText.IsEmpty() )
        return;

    // Validate tab index (uses passed index, not global - for parallel streaming safety)
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    TAB_DATA& tab = m_tabs[aTabIndex];

    // Accumulate text in per-tab buffer
    // (isFirstStreamingFlush is set at stream start and cleared after first flush)
    tab.streamingBuffer += aText;
    tab.pendingDeltaCount++;

    // Start timer if not running (shared timer is ok - flushStreamingBuffer checks per-tab)
    if( m_streamingFlushTimer && !m_streamingFlushTimer->IsRunning() )
    {
        m_streamingFlushTimer->Start( STREAMING_FLUSH_INTERVAL_MS, wxTIMER_ONE_SHOT );
    }

    // Flush immediately if threshold reached
    if( tab.pendingDeltaCount >= STREAMING_FLUSH_DELTA_COUNT )
    {
        flushStreamingBuffer( aTabIndex );
    }
}


void AI_CHAT_PANEL_BASE::flushStreamingBuffer( int aTabIndex )
{
    // Critical: Don't access UI if panel is being destroyed
    if( m_isDestroying.load() )
        return;
    
    // Use explicit tab index for parallel streaming safety
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    TAB_DATA& tab = m_tabs[aTabIndex];
    
    if( tab.streamingBuffer.IsEmpty() )
        return;

    // Stop timer if running
    if( m_streamingFlushTimer && m_streamingFlushTimer->IsRunning() )
    {
        m_streamingFlushTimer->Stop();
    }

    // Flush buffered content to UI
    wxString textToFlush = tab.streamingBuffer;
    bool isFirst = tab.isFirstStreamingFlush;

    // Clear per-tab buffer and reset state
    tab.streamingBuffer.Clear();
    tab.pendingDeltaCount = 0;
    tab.isFirstStreamingFlush = false;

    // Update UI with accumulated text (pass tab index for parallel streaming safety)
    onStreamingText( textToFlush, isFirst, aTabIndex );
}

void AI_CHAT_PANEL_BASE::flushStreamingBuffer()
{
    // Default overload - flush current streaming tab
    flushStreamingBuffer( m_streamingTabIndex );
}


void AI_CHAT_PANEL_BASE::onStreamingFlushTimer( wxTimerEvent& aEvent )
{
    // For parallel streaming safety, flush ALL tabs that have buffered content
    for( size_t i = 0; i < m_tabs.size(); ++i )
    {
        if( !m_tabs[i].streamingBuffer.IsEmpty() )
        {
            flushStreamingBuffer( static_cast<int>( i ) );
        }
    }
}


void AI_CHAT_PANEL_BASE::onStreamingStatus( const wxString& aStatus, int aTabIndex )
{
    // Critical: Don't touch UI if panel is being destroyed (prevents crash from queued callbacks)
    if( m_isDestroying.load() )
        return;
    
    // Validate tab index (uses passed index, not global - for parallel streaming safety)
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    TAB_DATA& tab = m_tabs[aTabIndex];
    
    // Safety check: Don't process if this tab's stream was stopped
    if( tab.stopRequested.load() )
        return;

    if( !aStatus.IsEmpty() )
    {
        // Update webview (if this is the active tab)
        if( m_chatWebview && aTabIndex == m_currentTabIndex )
        {
            m_chatWebview->HideTypingIndicator();
            m_chatWebview->ShowStatus( aStatus );
        }
    }
}


void AI_CHAT_PANEL_BASE::onBackendResponse( const wxString& aResponse, bool aSuccess, int aTabIndex, bool aFileModified )
{
    // Critical: Don't touch UI if panel is being destroyed (prevents crash from queued callbacks)
    if( m_isDestroying.load() )
        return;
    
    // Validate tab index (uses passed index, not global - for parallel streaming safety)
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;
    
    // Get tab data
    TAB_DATA& tab = m_tabs[aTabIndex];

    // Kill idle timer immediately so it can't fire between HideStatus and SetStreaming(false)
    if( tab.idleStatusTimer )
        tab.idleStatusTimer->Stop();

    tab.aiMessageCount++;

    // Only track ai_response_completed for actual AI responses, not errors
    {
        double conversationDurationSec = 0.0;
        if( tab.userMessageCount > 0 )
        {
            auto now = std::chrono::steady_clock::now();
            conversationDurationSec = std::chrono::duration<double>( now - tab.conversationStartTime ).count();
        }

        std::string modeStr = "agent";
        if( m_currentMode == AI_MODE::PLAN )
            modeStr = "plan";
        else if( m_currentMode == AI_MODE::ASK )
            modeStr = "ask";

        if( aSuccess )
        {
            AMPLITUDE_CLIENT::Instance().Track( "ai_response_completed", {
                { "success", aSuccess },
                { "file_modified", aFileModified },
                { "mode", modeStr },
                { "user_message_count", tab.userMessageCount },
                { "ai_message_count", tab.aiMessageCount },
                { "tool_call_count", tab.toolCallCount },
                { "file_edit_count", tab.fileEditCount },
                { "total_user_chars_sent", tab.totalUserCharsSent },
                { "conversation_duration_seconds", conversationDurationSec },
                { "app_type", std::string( GetAppType().utf8_str() ) }
            });
        }
        else
        {
            AMPLITUDE_CLIENT::Instance().Track( "ai_request_failed", {
                { "mode", modeStr },
                { "user_message_count", tab.userMessageCount },
                { "conversation_duration_seconds", conversationDurationSec },
                { "app_type", std::string( GetAppType().utf8_str() ) }
            });
        }
    }
    
    // Update webview (if this is the active tab)
    if( m_chatWebview && aTabIndex == m_currentTabIndex )
    {
        m_chatWebview->HideStatus();
        m_chatWebview->HideTypingIndicator();
        m_chatWebview->HideThinking();
        
        // Finalize the streaming message using the unique ID stored in the tab
        if( !tab.streamingMessageId.IsEmpty() )
        {
            m_chatWebview->FinalizeMessage( tab.streamingMessageId );
        }
        
        // Re-enable input
        m_chatWebview->SetInputEnabled( true );
    }

    // Reset AI edit tracking after response completes
    if( !aFileModified || !m_aiEditInProgress )
    {
        m_aiEditInProgress = false;
        m_aiEditStateCaptured = false;
    }
    
    // Reset plan mode state if no longer executing
    // This handles cases where the backend didn't send a MODE_TRANSITION event
    // (e.g., error, timeout, or cancel)
    if( m_planModeExecuting && !aSuccess )
    {
        wxLogDebug( "[PLAN_MODE] Resetting plan mode state due to error/failure" );
        m_planModeExecuting = false;
        m_awaitingModeConfirmation = false;
    }

    // Switch Stop button back to Send button if authenticated
    bool isAuthenticated = false;
    try
    {
        isAuthenticated = AUTH_MANAGER::Instance().IsAuthenticated();
    }
    catch( ... )
    {
        // Auth manager error - leave disabled
    }

    if( isAuthenticated )
    {
        // Only switch button back to "Send" if NO tabs are streaming
        // (for parallel execution support)
        bool anyStreaming = isAnyTabStreaming();
        if( !anyStreaming )
        {
    updateButtonState( false );
        }
    
    // Re-enable input in webview
    if( m_chatWebview )
        m_chatWebview->SetInputEnabled( true );
    }

    // Note: Per-tab streaming state is reset in the completion handler
    // of sendToBackendAsync for true parallel execution support.
}


void AI_CHAT_PANEL_BASE::updateButtonState( bool aIsStopMode )
{
    if( m_chatWebview )
    {
        wxString tabId;
        if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
            tabId = m_tabs[m_currentTabIndex].conversationId;
        m_chatWebview->SetStreaming( aIsStopMode, tabId );
    }
}


void AI_CHAT_PANEL_BASE::onStopRequest( wxCommandEvent& aEvent )
{
    // Stop the current tab's stream (per-tab for true parallel execution)
    if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
    {
        onStopRequestForTab( m_currentTabIndex );
    }
}


void AI_CHAT_PANEL_BASE::onStopRequestForTab( int aTabIndex )
{
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];
    tab.stopRequested.store( true );
    tab.isStreaming.store( false );

    if( tab.backendClient )
        tab.backendClient->StopStream();

    stopIdleStatusTimer( aTabIndex );
    flushStreamingBuffer( aTabIndex );

    // Only change button to Send if NO tabs are streaming (supports parallel execution)
    if( !isAnyTabStreaming() )
    {
    updateButtonState( false );
    }
}


// =========================================================================
// Tab Management Methods
// =========================================================================

void AI_CHAT_PANEL_BASE::onTabSelected( wxCommandEvent& aEvent )
{
    int newIndex = aEvent.GetInt();
    if( newIndex == m_currentTabIndex || newIndex < 0 )
        return;

    // Note: We don't stop streaming when switching tabs - this allows parallel tasks
    // The streaming callbacks check m_streamingTabIndex to avoid updating wrong tab's UI

    // Switch to the new tab (shows its panel, hides others)
    switchToTab( newIndex );
}


void AI_CHAT_PANEL_BASE::onNewTab( wxCommandEvent& aEvent )
{
    // Note: We don't stop streaming - allows parallel tasks across tabs
    // No save needed - each tab has its own persistent panel
    
    // Create new tab and switch to it
    int newIndex = createNewTab();
    if( newIndex >= 0 )
    {
        switchToTab( newIndex );
    }
}


void AI_CHAT_PANEL_BASE::onTabClose( wxCommandEvent& aEvent )
{
    int tabIndex = aEvent.GetInt();
    if( tabIndex < 0 || tabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    // Don't allow closing the last tab
    if( m_tabs.size() <= 1 )
        return;

    TAB_DATA& tabToClose = m_tabs[tabIndex];

    // If this tab is actively streaming, stop it first
    // (we can't send updates to a deleted tab)
    if( tabToClose.isStreaming.load() )
    {
        tabToClose.stopRequested.store( true );
        
        // Stop the per-tab backend client
        if( tabToClose.backendClient )
            tabToClose.backendClient->StopStream();
        
        // Wait briefly for stream to stop (non-blocking check)
        for( int i = 0; i < 10; ++i )
        {
            if( !tabToClose.isStreaming.load() )
                break;
            wxMilliSleep( 10 );
        }
        
        // Force cleanup even if stream didn't stop cleanly
        tabToClose.isStreaming.store( false );
    }
    
    // CRITICAL: Always detach the request thread before destroying TAB_DATA
    // Even if streaming completed, the thread may still be joinable
    // std::thread destructor calls std::terminate() if joinable
    if( tabToClose.requestThread && tabToClose.requestThread->joinable() )
        tabToClose.requestThread->detach();
    
    // Note: With per-tab streaming, we don't need to update global streaming state
    // Each tab manages its own streaming independently
    
    // Update m_streamingTabIndex if needed (for event routing)
    if( m_streamingTabIndex > tabIndex )
        m_streamingTabIndex--;

    // Save conversation to local DB before closing
    if( !m_tabs[tabIndex].conversationId.IsEmpty() )
    {
        // Messages are already saved incrementally, just remove from UI
    }

    // Remove tab data
    wxString closedTabId = m_tabs[tabIndex].conversationId;
    m_tabs.erase( m_tabs.begin() + tabIndex );
    
    // Update webview tab bar
    if( m_chatWebview && !closedTabId.IsEmpty() )
        m_chatWebview->RemoveTab( closedTabId );

    // Adjust current index based on which tab was closed
    if( tabIndex < m_currentTabIndex )
    {
        // Closed a tab before the current one - shift index down
        m_currentTabIndex--;
    }
    else if( tabIndex == m_currentTabIndex )
    {
        // Closed the current tab - need to switch to another
        if( m_currentTabIndex >= static_cast<int>( m_tabs.size() ) )
            m_currentTabIndex = static_cast<int>( m_tabs.size() ) - 1;
    }
    // else: closed a tab after the current one - no adjustment needed

    if( m_currentTabIndex >= 0 )
        switchToTab( m_currentTabIndex );
    
    // Save tab state after closing a tab
    saveOpenTabs();
}


void AI_CHAT_PANEL_BASE::onHistorySelect( wxCommandEvent& aEvent )
{
    // Get conversation history from local DB
    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    
    wxString userId = wxEmptyString;
    if( AUTH_MANAGER::Instance().IsAuthenticated() )
    {
        AUTH_USER user = AUTH_MANAGER::Instance().GetCurrentUser();
        userId = user.id;
    }

    std::vector<CONVERSATION> conversations = db.ListConversations( userId, 20 );

    // Build JSON array for React history menu
    nlohmann::json conversationsJson = nlohmann::json::array();
    
    for( const auto& conv : conversations )
    {
        wxString title = conv.title.IsEmpty() ? wxString( wxT( "Untitled" ) ) : conv.title;
        
        // Truncate long titles
        if( title.length() > 50 )
            title = title.Left( 47 ) + wxT( "..." );

        // Format date hint
        wxString timestamp = conv.updated_at.Left( 10 ); // YYYY-MM-DD

        nlohmann::json convObj;
        convObj["id"] = conv.id.ToStdString();
        convObj["title"] = title.ToStdString();
        convObj["timestamp"] = timestamp.ToStdString();
        conversationsJson.push_back( convObj );
    }

    // Show React-based history menu
    if( m_chatWebview )
    {
        m_chatWebview->ShowHistoryMenu( wxString::FromUTF8( conversationsJson.dump() ) );
    }
}



void AI_CHAT_PANEL_BASE::switchToTab( int aTabIndex )
{
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];

    // Load messages if this tab hasn't been loaded yet (e.g., restored tabs)
    if( !tab.messagesLoaded && !tab.conversationId.IsEmpty() && !tab.conversationId.StartsWith( wxT( "temp_" ) ) )
    {
        loadMessagesForTab( aTabIndex );
    }
    else if( tab.messagesLoaded && m_chatWebview && !tab.isStreaming.load() )
    {
        // Only reload from DB when the tab is idle. If the tab is actively streaming,
        // React's TabState already holds the live turnSegments/activityItems.
        // Calling reloadMessagesToWebview here would ClearMessages and destroy them.
        reloadMessagesToWebview( tab.conversationId );
    }

    // Update webview tab selection
    if( m_chatWebview )
        m_chatWebview->SelectTab( tab.conversationId );

    // Update global conversation ID for the current tab (session ID is app-global, doesn't change)
    m_conversationId = tab.conversationId;
    m_currentTabIndex = aTabIndex;

    // Replay ephemeral state for this tab so the React UI reflects the correct state
    if( m_chatWebview )
    {
        // Only send SetStreaming(false) to correct React state; never replay SetStreaming(true)
        // because that clears live turnSegments. React preserves per-tab state on its own.
        if( !tab.isStreaming.load() )
            m_chatWebview->SetStreaming( false, tab.conversationId );

        m_chatWebview->SetInputEnabled( !tab.isStreaming.load() );
    }
}


TAB_DATA* AI_CHAT_PANEL_BASE::findTabByConversationId( const wxString& aConversationId )
{
    if( aConversationId.IsEmpty() )
        return nullptr;
    
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].conversationId == aConversationId )
        {
            return &m_tabs[i];
        }
    }
    
    return nullptr;
}


int AI_CHAT_PANEL_BASE::resolveTabIndex( const wxString& aTabId )
{
    if( !aTabId.IsEmpty() )
    {
        for( size_t i = 0; i < m_tabs.size(); i++ )
        {
            if( m_tabs[i].conversationId == aTabId )
                return static_cast<int>( i );
        }
    }
    return m_currentTabIndex;
}


/// Build the metadata JSON for an assistant message from per-tab streaming state.
/// Used by both the main StreamChat completion handler and the sendPlanAction completion handler.
static nlohmann::json buildAssistantMetadataJson( const TAB_DATA* aTab,
                                                   const wxString& aMode,
                                                   const wxString& aAppType )
{
    nlohmann::json metaJson;
    metaJson["mode"] = std::string( aMode.utf8_str() );
    metaJson["app_type"] = std::string( aAppType.utf8_str() );

    if( aTab->thinkingDurationMs > 0 )
        metaJson["thinking_duration_ms"] = static_cast<int>( aTab->thinkingDurationMs );

    if( !aTab->thinkingBlocks.empty() )
    {
        nlohmann::json blocksJson = nlohmann::json::array();
        for( const auto& blk : aTab->thinkingBlocks )
        {
            nlohmann::json bj = {
                { "content", std::string( blk.content.utf8_str() ) },
                { "offset", blk.charOffset },
                { "duration_ms", static_cast<int>( blk.durationMs ) }
            };
            if( !blk.agentId.IsEmpty() )
                bj["agent_id"] = std::string( blk.agentId.utf8_str() );
            blocksJson.push_back( bj );
        }
        metaJson["thinking_blocks"] = blocksJson;
    }

    if( !aTab->activityMarkers.empty() )
    {
        nlohmann::json markersJson = nlohmann::json::array();
        for( const auto& m : aTab->activityMarkers )
        {
            nlohmann::json mj = {
                { "offset", m.charOffset },
                { "label", std::string( m.label.utf8_str() ) },
                { "ts", m.timestampMs }
            };
            if( !m.thinkingContext.IsEmpty() )
                mj["thinking_context"] = std::string( m.thinkingContext.utf8_str() );
            markersJson.push_back( mj );
        }
        metaJson["activity_markers"] = markersJson;
    }

    if( !aTab->researchSummary.IsEmpty() )
        metaJson["research_summary"] = std::string( aTab->researchSummary.utf8_str() );

    return metaJson;
}


/// Check if an ISO 8601 timestamp is within `aMaxMinutes` of now.
/// Returns true if the timestamp is recent enough (plan approval is still valid).
static bool isTimestampWithinMinutes( const wxString& aIsoTimestamp, int aMaxMinutes )
{
    if( aIsoTimestamp.IsEmpty() )
        return false;

    wxDateTime dt;
    if( !dt.ParseISOCombined( aIsoTimestamp ) )
        return false;

    wxDateTime now = wxDateTime::Now();
    wxTimeSpan diff = now.Subtract( dt );
    return diff.GetMinutes() < aMaxMinutes;
}


/// Build a JS segments array string from message content + activity_markers + thinking_blocks JSON.
/// Returns e.g. ",segments:[{type:'text',content:'...'},{type:'activity',labels:[...],timestamps:[...]},...]"
/// or empty string if markers/blocks produce no useful segments.
static wxString buildSegmentsJs( const wxString& aContent, const nlohmann::json& aMarkers,
                                  const nlohmann::json& aThinkingBlocks = nlohmann::json() )
{
    bool hasMarkers = aMarkers.is_array() && !aMarkers.empty();
    bool hasThinking = aThinkingBlocks.is_array() && !aThinkingBlocks.empty();
    if( !hasMarkers && !hasThinking )
        return wxEmptyString;

    struct Marker { int offset; wxString label; long long ts; wxString thinkingContext; };
    std::vector<Marker> markers;

    if( hasMarkers )
    {
        markers.reserve( aMarkers.size() );
        for( const auto& m : aMarkers )
        {
            if( !m.contains( "offset" ) || !m["offset"].is_number_integer() )
                continue;
            if( !m.contains( "label" ) || !m["label"].is_string() )
                continue;
            Marker mk;
            mk.offset = m["offset"].get<int>();
            mk.label  = wxString::FromUTF8( m["label"].get<std::string>() );
            mk.ts     = ( m.contains( "ts" ) && m["ts"].is_number() ) ? m["ts"].get<long long>() : 0;
            mk.thinkingContext = ( m.contains( "thinking_context" ) && m["thinking_context"].is_string() )
                ? wxString::FromUTF8( m["thinking_context"].get<std::string>() ) : wxString();
            markers.push_back( mk );
        }
    }

    struct TBlock { int offset; wxString content; int durationMs; wxString agentId; };
    std::vector<TBlock> tblocks;

    if( hasThinking )
    {
        tblocks.reserve( aThinkingBlocks.size() );
        for( const auto& b : aThinkingBlocks )
        {
            if( !b.contains( "offset" ) || !b["offset"].is_number_integer() )
                continue;
            TBlock tb;
            tb.offset     = b["offset"].get<int>();
            tb.content    = ( b.contains( "content" ) && b["content"].is_string() )
                                ? wxString::FromUTF8( b["content"].get<std::string>() ) : wxString();
            tb.durationMs = ( b.contains( "duration_ms" ) && b["duration_ms"].is_number() )
                                ? b["duration_ms"].get<int>() : 0;
            tb.agentId    = ( b.contains( "agent_id" ) && b["agent_id"].is_string() )
                                ? wxString::FromUTF8( b["agent_id"].get<std::string>() ) : wxString();
            tblocks.push_back( tb );
        }
    }

    if( markers.empty() && tblocks.empty() )
        return wxEmptyString;

    // Build a unified list of interleave points sorted by offset
    struct InterleaveItem
    {
        int offset;
        enum { ACTIVITY, THINKING } kind;
        size_t index;
    };
    std::vector<InterleaveItem> items;
    for( size_t idx = 0; idx < markers.size(); idx++ )
        items.push_back( { markers[idx].offset, InterleaveItem::ACTIVITY, idx } );
    for( size_t idx = 0; idx < tblocks.size(); idx++ )
        items.push_back( { tblocks[idx].offset, InterleaveItem::THINKING, idx } );

    std::sort( items.begin(), items.end(),
               []( const InterleaveItem& a, const InterleaveItem& b )
               {
                   if( a.offset != b.offset ) return a.offset < b.offset;
                   return a.kind < b.kind; // THINKING before ACTIVITY at same offset
               } );

    std::string contentUtf8 = aContent.ToStdString( wxConvUTF8 );
    int contentLen = static_cast<int>( contentUtf8.length() );

    wxString js = wxT( ",segments:[" );
    bool firstSeg = true;

    auto appendText = [&]( int from, int to )
    {
        if( from >= to || from >= contentLen )
            return;
        int end = std::min( to, contentLen );
        wxString slice = wxString::FromUTF8( contentUtf8.c_str() + from, end - from );
        if( slice.IsEmpty() )
            return;
        if( !firstSeg ) js += wxT( "," );
        firstSeg = false;
        js += wxString::Format( wxT( "{type:'text',content:'%s'}" ),
                                CHAT_WEBVIEW_PANEL::EscapeForJS( slice ) );
    };

    int prevOffset = 0;
    size_t i = 0;
    while( i < items.size() )
    {
        int curOffset = items[i].offset;
        appendText( prevOffset, curOffset );

        // Emit thinking blocks at this offset, grouping sub-agent blocks
        std::vector<const TBlock*> subAgentBlocks;
        std::vector<const TBlock*> mainBlocks;
        size_t thinkScan = i;
        while( thinkScan < items.size() && items[thinkScan].offset == curOffset
               && items[thinkScan].kind == InterleaveItem::THINKING )
        {
            const TBlock& tb = tblocks[items[thinkScan].index];
            if( tb.agentId.StartsWith( wxT( "research_" ) ) )
                subAgentBlocks.push_back( &tb );
            else
                mainBlocks.push_back( &tb );
            thinkScan++;
        }
        i = thinkScan;

        for( const TBlock* tb : mainBlocks )
        {
            if( !firstSeg ) js += wxT( "," );
            firstSeg = false;
            if( tb->agentId.IsEmpty() )
            {
                js += wxString::Format( wxT( "{type:'thinking',content:'%s',durationMs:%d}" ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( tb->content ),
                                        tb->durationMs );
            }
            else
            {
                js += wxString::Format( wxT( "{type:'thinking',content:'%s',durationMs:%d,agentId:'%s'}" ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( tb->content ),
                                        tb->durationMs,
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( tb->agentId ) );
            }
        }

        if( !subAgentBlocks.empty() )
        {
            if( !firstSeg ) js += wxT( "," );
            firstSeg = false;
            int totalDur = 0;
            wxString subArr = wxT( "[" );
            bool firstSub = true;
            for( const TBlock* tb : subAgentBlocks )
            {
                totalDur += tb->durationMs;
                if( !firstSub ) subArr += wxT( "," );
                firstSub = false;
                subArr += wxString::Format(
                    wxT( "{agentId:'%s',content:'%s',durationMs:%d,label:'%s'}" ),
                    CHAT_WEBVIEW_PANEL::EscapeForJS( tb->agentId ),
                    CHAT_WEBVIEW_PANEL::EscapeForJS( tb->content ),
                    tb->durationMs,
                    CHAT_WEBVIEW_PANEL::EscapeForJS( tb->agentId ) );
            }
            subArr += wxT( "]" );
            js += wxString::Format( wxT( "{type:'thinking',content:'',durationMs:%d,subAgents:%s}" ),
                                    totalDur, subArr );
        }

        // Gather activity markers at this offset
        if( i < items.size() && items[i].offset == curOffset
            && items[i].kind == InterleaveItem::ACTIVITY )
        {
            wxString labelsArr = wxT( "[" );
            wxString tsArr = wxT( "[" );
            wxString tcArr = wxT( "[" );
            wxString durArr = wxT( "[" );
            bool firstLabel = true;
            std::set<wxString> seenLabels;
            // Collect all markers at this offset first, then compute durations
            std::vector<const Marker*> groupMarkers;
            size_t scanI = i;
            while( scanI < items.size() && items[scanI].offset == curOffset
                   && items[scanI].kind == InterleaveItem::ACTIVITY )
            {
                const Marker& mk = markers[items[scanI].index];
                if( seenLabels.find( mk.label ) == seenLabels.end() )
                {
                    seenLabels.insert( mk.label );
                    groupMarkers.push_back( &mk );
                }
                scanI++;
            }
            i = scanI;

            for( size_t gi = 0; gi < groupMarkers.size(); gi++ )
            {
                const Marker& mk = *groupMarkers[gi];
                if( !firstLabel ) { labelsArr += wxT( "," ); tsArr += wxT( "," ); tcArr += wxT( "," ); durArr += wxT( "," ); }
                firstLabel = false;
                labelsArr += wxString::Format( wxT( "'%s'" ),
                                               CHAT_WEBVIEW_PANEL::EscapeForJS( mk.label ) );
                tsArr += wxString::Format( wxT( "%lld" ), mk.ts );
                if( !mk.thinkingContext.IsEmpty() )
                    tcArr += wxString::Format( wxT( "'%s'" ),
                                               CHAT_WEBVIEW_PANEL::EscapeForJS( mk.thinkingContext ) );
                else
                    tcArr += wxT( "''" );

                long long durMs = 0;
                if( gi + 1 < groupMarkers.size() && mk.ts > 0 && groupMarkers[gi + 1]->ts > 0 )
                    durMs = groupMarkers[gi + 1]->ts - mk.ts;
                durArr += wxString::Format( wxT( "%lld" ), durMs );
            }
            labelsArr += wxT( "]" );
            tsArr += wxT( "]" );
            tcArr += wxT( "]" );
            durArr += wxT( "]" );

            if( !firstSeg ) js += wxT( "," );
            firstSeg = false;
            js += wxString::Format( wxT( "{type:'activity',labels:%s,timestamps:%s,thinkingContexts:%s,durationMs:%s}" ),
                                    labelsArr, tsArr, tcArr, durArr );
        }

        prevOffset = curOffset;
    }

    // Remaining text after the last item
    appendText( prevOffset, contentLen );

    js += wxT( "]" );
    return js;
}


void AI_CHAT_PANEL_BASE::reloadMessagesToWebview( const wxString& aConversationId )
{
    if( !m_chatWebview || aConversationId.IsEmpty()
        || aConversationId.StartsWith( wxT( "temp_" ) ) )
        return;

    m_chatWebview->ClearMessages( aConversationId );

    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    auto messages = db.LoadMessages( aConversationId );

    int msgIndex = 0;
    int totalMessages = static_cast<int>( messages.size() );

    // Deferred plan document: stored when we encounter a plan_document row,
    // attached to the next assistant message that gets emitted.
    wxString pendingPlanDoc;
    wxString pendingPlanRenderMode;
    bool     pendingPlanShowApproval = false;

    for( const auto& msg : messages )
    {
        bool isLastMessage = ( msgIndex == totalMessages - 1 );

        if( msg.role == wxT( "assistant" ) && !msg.metadata.IsEmpty() )
        {
            try
            {
                nlohmann::json metadataJson = nlohmann::json::parse( msg.metadata.ToStdString() );

                if( metadataJson.contains( "type" ) && metadataJson["type"] == "plan_document" )
                {
                    wxString planDoc = msg.content;

                    if( planDoc.IsEmpty() && metadataJson.contains( "plan_file_path" )
                        && metadataJson["plan_file_path"].is_string() )
                    {
                        wxString planFilePath = wxString::FromUTF8(
                            metadataJson["plan_file_path"].get<std::string>() );

                        if( wxFileName::FileExists( planFilePath ) )
                        {
                            wxFFile planFile( planFilePath, "r" );
                            if( planFile.IsOpened() )
                            {
                                planFile.ReadAll( &planDoc );
                                planFile.Close();
                            }
                        }
                    }

                    if( !planDoc.IsEmpty() )
                    {
                        pendingPlanDoc = planDoc;
                        pendingPlanRenderMode = wxT( "summary" );
                        pendingPlanShowApproval = false;

                        if( isLastMessage
                            && metadataJson.contains( "render_mode" )
                            && metadataJson["render_mode"] == "full"
                            && isTimestampWithinMinutes( msg.created_at, 55 ) )
                        {
                            pendingPlanRenderMode = wxT( "full" );
                            pendingPlanShowApproval = true;
                        }
                    }
                    msgIndex++;
                    continue;
                }

                // Build segments JS from activity_markers + thinking_blocks
                wxString segmentsJs;

                // Extract research_summary early so we can strip it from text segments
                wxString rsContent;
                if( metadataJson.contains( "research_summary" )
                    && metadataJson["research_summary"].is_string()
                    && !metadataJson["research_summary"].get<std::string>().empty() )
                {
                    rsContent = wxString::FromUTF8(
                        metadataJson["research_summary"].get<std::string>() );
                }

                // Strip research content from msg.content for text segments
                wxString textContent = msg.content;
                if( !rsContent.IsEmpty() )
                {
                    int pos = textContent.Find( rsContent );
                    if( pos != wxNOT_FOUND )
                    {
                        textContent = textContent.Mid( 0, pos )
                                    + textContent.Mid( pos + rsContent.length() );
                        // Trim leading whitespace from remainder
                        while( !textContent.IsEmpty()
                               && ( textContent[0] == '\n' || textContent[0] == '\r' ) )
                            textContent = textContent.Mid( 1 );
                    }
                }

                if( metadataJson.contains( "activity_markers" )
                    && metadataJson["activity_markers"].is_array()
                    && !metadataJson["activity_markers"].empty() )
                {
                    nlohmann::json tb;
                    if( metadataJson.contains( "thinking_blocks" )
                        && metadataJson["thinking_blocks"].is_array() )
                        tb = metadataJson["thinking_blocks"];
                    segmentsJs = buildSegmentsJs( textContent, metadataJson["activity_markers"], tb );
                }
                else if( metadataJson.contains( "thinking_blocks" )
                         && metadataJson["thinking_blocks"].is_array()
                         && !metadataJson["thinking_blocks"].empty() )
                {
                    segmentsJs = buildSegmentsJs( textContent, nlohmann::json(),
                                                   metadataJson["thinking_blocks"] );
                }

                // Inject research segment if research_summary was saved
                if( !rsContent.IsEmpty() )
                {
                    wxString rsSegment = wxString::Format(
                        wxT( "{type:'research',title:'Research Summary',content:'%s'}" ),
                        CHAT_WEBVIEW_PANEL::EscapeForJS( rsContent ) );

                    if( segmentsJs.IsEmpty() )
                    {
                        if( !textContent.Trim().IsEmpty() )
                        {
                            segmentsJs = wxString::Format(
                                wxT( ",segments:[%s,{type:'text',content:'%s'}]" ),
                                rsSegment,
                                CHAT_WEBVIEW_PANEL::EscapeForJS( textContent ) );
                        }
                        else
                        {
                            segmentsJs = wxString::Format(
                                wxT( ",segments:[%s]" ), rsSegment );
                        }
                    }
                    else
                    {
                        segmentsJs.Replace( wxT( ",segments:[" ),
                                            wxString::Format( wxT( ",segments:[%s," ), rsSegment ),
                                            false );
                    }
                }

                // Attach pending plan document to this message
                wxString planDocJs;
                if( !pendingPlanDoc.IsEmpty() )
                {
                    planDocJs = wxString::Format(
                        wxT( ",planDocument:{markdown:'%s',renderMode:'%s'}" ),
                        CHAT_WEBVIEW_PANEL::EscapeForJS( pendingPlanDoc ),
                        pendingPlanRenderMode );
                }

                if( !segmentsJs.IsEmpty() || !planDocJs.IsEmpty() )
                {
                    wxString msgId = wxString::Format( wxT( "reload_%d" ), msgIndex );
                    wxString script = wxString::Format(
                        wxT( "if(window.kichat){window.kichat.addMessage("
                             "{id:'%s',role:'assistant',content:'%s'%s%s});}" ),
                        CHAT_WEBVIEW_PANEL::EscapeForJS( msgId ),
                        CHAT_WEBVIEW_PANEL::EscapeForJS( msg.content ),
                        segmentsJs,
                        planDocJs );
                    if( m_chatWebview->IsBridgeReady() )
                        m_chatWebview->RunScriptFireAndForget( script );
                    else
                        m_chatWebview->QueuePendingCommand( script );

                    if( pendingPlanShowApproval )
                        m_chatWebview->ShowPlanApproval();

                    pendingPlanDoc.Clear();
                    pendingPlanRenderMode.Clear();
                    pendingPlanShowApproval = false;
                    msgIndex++;
                    continue;
                }
            }
            catch( const nlohmann::json::exception& )
            {
            }
        }

        wxString msgId = wxString::Format( wxT( "reload_%d" ), msgIndex );
        m_chatWebview->AddMessage( msgId, msg.role, msg.content );
        msgIndex++;
    }

    // If a plan document was at the very end with no following assistant message,
    // emit it standalone via SetPlanDocument as a fallback
    if( !pendingPlanDoc.IsEmpty() )
    {
        wxString planMsgId = wxString::Format( wxT( "reload_plan_%d" ), msgIndex );
        m_chatWebview->SetPlanDocument( planMsgId, pendingPlanDoc, pendingPlanRenderMode );
        if( pendingPlanShowApproval )
            m_chatWebview->ShowPlanApproval();
    }
}


void AI_CHAT_PANEL_BASE::loadMessagesForTab( int aTabIndex )
{
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];

    // Skip if already loaded
    if( tab.messagesLoaded )
        return;

    // Use atomic compare-exchange to prevent duplicate loads (clean race condition handling)
    bool expected = false;
    if( !tab.isLoadingMessages.compare_exchange_strong( expected, true ) )
    {
        // Another thread is already loading - skip
        return;
    }

    if( tab.conversationId.IsEmpty() )
    {
        tab.isLoadingMessages.store( false );  // Reset on early exit
        return;
    }

    // Load messages in background thread
    auto panelAlive = m_panelAlive;
    wxString convId = tab.conversationId;
    AI_CHAT_PANEL_BASE* self = this;

    std::thread( [self, panelAlive, convId]()
    {
        if( !panelAlive->load() )
            return;

        // Load messages from DB
        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        std::vector<MESSAGE> messages = db.LoadMessages( convId );

        // Update UI on main thread (use panel's CallAfter for reliable delivery)
        if( panelAlive->load() )
        {
            self->CallAfter( [self, panelAlive, convId, messages]()
            {
                if( !panelAlive->load() || self->m_isDestroying.load() )
                    return;

                // Find the tab by conversation ID (indices may have shifted)
                int tabIndex = -1;
                for( size_t i = 0; i < self->m_tabs.size(); i++ )
                {
                    if( self->m_tabs[i].conversationId == convId )
                    {
                        tabIndex = static_cast<int>( i );
                        break;
                    }
                }

                if( tabIndex < 0 )
                    return;  // Tab was closed while loading

                TAB_DATA& tab = self->m_tabs[tabIndex];

                // Clear webview messages for this tab (if it's the current tab)
                if( self->m_chatWebview && tabIndex == self->m_currentTabIndex )
                {
                    self->m_chatWebview->ClearMessages();
                }

                // Add messages to this tab's chat history (webview only)
                int msgIndex = 0;
                int totalMessages = static_cast<int>( messages.size() );
                wxLogDebug( wxT( "loadMessagesForTab: Loading %zu messages for conversation %s" ), 
                            messages.size(), convId );

                // Deferred plan document: stored when we encounter a plan_document row,
                // attached to the next assistant message that gets emitted.
                wxString pendingPlanDoc;
                wxString pendingPlanRenderMode;
                bool     pendingPlanShowApproval = false;

                for( const auto& msg : messages )
                {
                    bool isLastMessage = ( msgIndex == totalMessages - 1 );

                    wxLogDebug( wxT( "loadMessagesForTab: msg[%d] role=%s, metadata=%s" ), 
                                msgIndex, msg.role, msg.metadata.Left( 100 ) );
                    if( msg.role == wxT( "user" ) )
                    {
                        // Add to webview if this is the current tab
                        if( self->m_chatWebview && tabIndex == self->m_currentTabIndex )
                        {
                            wxString msgId = wxString::Format( wxT( "loaded_%d" ), msgIndex );
                            self->m_chatWebview->AddMessage( msgId, wxT( "user" ), msg.content );
                        }
                    }
                    else if( msg.role == wxT( "assistant" ) )
                    {
                        // Check if this is a plan_document type message (saved separately)
                        if( !msg.metadata.IsEmpty() )
                        {
                            try
                            {
                                nlohmann::json metadataJson = nlohmann::json::parse( msg.metadata.ToStdString() );
                                
                                if( metadataJson.contains( "type" ) && metadataJson["type"] == "plan_document" )
                                {
                                    if( self->m_chatWebview && tabIndex == self->m_currentTabIndex )
                                    {
                                        wxString planDoc = msg.content;
                                        
                                        if( planDoc.IsEmpty() && metadataJson.contains( "plan_file_path" )
                                            && metadataJson["plan_file_path"].is_string() )
                                        {
                                            wxString planFilePath = wxString::FromUTF8( 
                                                metadataJson["plan_file_path"].get<std::string>() );
                                            
                                            if( wxFileName::FileExists( planFilePath ) )
                                            {
                                                wxFFile planFile( planFilePath, "r" );
                                                if( planFile.IsOpened() )
                                                {
                                                    planFile.ReadAll( &planDoc );
                                                    planFile.Close();
                                                }
                                            }
                                        }
                                        
                                        if( !planDoc.IsEmpty() )
                                        {
                                            pendingPlanDoc = planDoc;
                                            pendingPlanRenderMode = wxT( "summary" );
                                            pendingPlanShowApproval = false;

                                            if( isLastMessage
                                                && metadataJson.contains( "render_mode" )
                                                && metadataJson["render_mode"] == "full"
                                                && isTimestampWithinMinutes( msg.created_at, 55 ) )
                                            {
                                                pendingPlanRenderMode = wxT( "full" );
                                                pendingPlanShowApproval = true;
                                            }
                                        }
                                        else
                                        {
                                            wxLogWarning( wxT( "Plan document has no content and no valid file path" ) );
                                        }
                                    }
                                    msgIndex++;
                                    continue;
                                }
                            }
                            catch( const nlohmann::json::exception& e )
                            {
                                wxLogDebug( wxT( "Failed to parse message metadata: %s" ), e.what() );
                            }
                        }
                        
                        // Restore thinking content and activity segments as part of the message object
                        if( !msg.metadata.IsEmpty() && self->m_chatWebview && tabIndex == self->m_currentTabIndex )
                        {
                            try
                            {
                                nlohmann::json mj = nlohmann::json::parse( msg.metadata.ToStdString() );

                                wxString segmentsJs;

                                // Extract research_summary early so we can strip it from text segments
                                wxString rsContent;
                                if( mj.contains( "research_summary" )
                                    && mj["research_summary"].is_string()
                                    && !mj["research_summary"].get<std::string>().empty() )
                                {
                                    rsContent = wxString::FromUTF8(
                                        mj["research_summary"].get<std::string>() );
                                }

                                // Strip research content from msg.content for text segments
                                wxString textContent = msg.content;
                                if( !rsContent.IsEmpty() )
                                {
                                    int pos = textContent.Find( rsContent );
                                    if( pos != wxNOT_FOUND )
                                    {
                                        textContent = textContent.Mid( 0, pos )
                                                    + textContent.Mid( pos + rsContent.length() );
                                        while( !textContent.IsEmpty()
                                               && ( textContent[0] == '\n' || textContent[0] == '\r' ) )
                                            textContent = textContent.Mid( 1 );
                                    }
                                }

                                if( mj.contains( "activity_markers" )
                                    && mj["activity_markers"].is_array()
                                    && !mj["activity_markers"].empty() )
                                {
                                    wxLogDebug( "LOAD: msg[%d] has %zu activity_markers",
                                                msgIndex, mj["activity_markers"].size() );
                                    nlohmann::json tb;
                                    if( mj.contains( "thinking_blocks" )
                                        && mj["thinking_blocks"].is_array() )
                                        tb = mj["thinking_blocks"];
                                    segmentsJs = buildSegmentsJs( textContent, mj["activity_markers"], tb );
                                }
                                else if( mj.contains( "thinking_blocks" )
                                         && mj["thinking_blocks"].is_array()
                                         && !mj["thinking_blocks"].empty() )
                                {
                                    segmentsJs = buildSegmentsJs( textContent, nlohmann::json(),
                                                                   mj["thinking_blocks"] );
                                }
                                else
                                {
                                    wxLogDebug( "LOAD: msg[%d] has NO activity_markers in metadata (keys: %s)",
                                                msgIndex, 
                                                wxString::FromUTF8( [&]() {
                                                    std::string keys;
                                                    for( auto it = mj.begin(); it != mj.end(); ++it )
                                                    {
                                                        if( !keys.empty() ) keys += ", ";
                                                        keys += it.key();
                                                    }
                                                    return keys;
                                                }() ) );
                                }

                                // Inject research segment if research_summary exists
                                if( !rsContent.IsEmpty() )
                                {
                                    wxString rsSegment = wxString::Format(
                                        wxT( "{type:'research',title:'Research Summary',content:'%s'}" ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( rsContent ) );

                                    if( segmentsJs.IsEmpty() )
                                    {
                                        if( !textContent.Trim().IsEmpty() )
                                        {
                                            segmentsJs = wxString::Format(
                                                wxT( ",segments:[%s,{type:'text',content:'%s'}]" ),
                                                rsSegment,
                                                CHAT_WEBVIEW_PANEL::EscapeForJS( textContent ) );
                                        }
                                        else
                                        {
                                            segmentsJs = wxString::Format(
                                                wxT( ",segments:[%s]" ), rsSegment );
                                        }
                                    }
                                    else
                                    {
                                        segmentsJs.Replace( wxT( ",segments:[" ),
                                                            wxString::Format( wxT( ",segments:[%s," ), rsSegment ),
                                                            false );
                                    }
                                }

                                // Attach pending plan document to this message
                                wxString planDocJs;
                                if( !pendingPlanDoc.IsEmpty() )
                                {
                                    planDocJs = wxString::Format(
                                        wxT( ",planDocument:{markdown:'%s',renderMode:'%s'}" ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( pendingPlanDoc ),
                                        pendingPlanRenderMode );
                                }

                                if( !segmentsJs.IsEmpty() || !planDocJs.IsEmpty() )
                                {
                                    wxString msgId = wxString::Format( wxT( "loaded_%d" ), msgIndex );
                                    wxLogDebug( "loadMessagesForTab: msg[%d] using enriched path (segments=%s, bridgeReady=%d)",
                                                msgIndex,
                                                segmentsJs.IsEmpty() ? "no" : "yes",
                                                self->m_chatWebview->IsBridgeReady() ? 1 : 0 );
                                    wxString script = wxString::Format(
                                        wxT( "if(window.kichat){window.kichat.addMessage("
                                             "{id:'%s',role:'assistant',content:'%s'%s%s});}" ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( msgId ),
                                        CHAT_WEBVIEW_PANEL::EscapeForJS( msg.content ),
                                        segmentsJs,
                                        planDocJs );
                                    if( self->m_chatWebview->IsBridgeReady() )
                                        self->m_chatWebview->RunScriptFireAndForget( script );
                                    else
                                        self->m_chatWebview->QueuePendingCommand( script );

                                    if( pendingPlanShowApproval )
                                        self->m_chatWebview->ShowPlanApproval();

                                    pendingPlanDoc.Clear();
                                    pendingPlanRenderMode.Clear();
                                    pendingPlanShowApproval = false;
                                    msgIndex++;
                                    continue;
                                }
                            }
                            catch( const nlohmann::json::exception& )
                            {
                            }
                        }

                        // Regular assistant message - add to webview
                        if( self->m_chatWebview && tabIndex == self->m_currentTabIndex )
                        {
                            wxString msgId = wxString::Format( wxT( "loaded_%d" ), msgIndex );
                            self->m_chatWebview->AddMessage( msgId, wxT( "assistant" ), msg.content );
                        }
                    }
                    msgIndex++;
                }

                // If a plan document was at the very end with no following assistant message,
                // emit it standalone via SetPlanDocument as a fallback
                if( !pendingPlanDoc.IsEmpty() && self->m_chatWebview
                    && tabIndex == self->m_currentTabIndex )
                {
                    wxString planMsgId = wxString::Format( wxT( "loaded_plan_%d" ), msgIndex );
                    self->m_chatWebview->SetPlanDocument( planMsgId, pendingPlanDoc,
                                                          pendingPlanRenderMode );
                    if( pendingPlanShowApproval )
                        self->m_chatWebview->ShowPlanApproval();
                }

                // Mark as loaded (after successful load)
                tab.messagesLoaded = true;
                tab.isLoadingMessages.store( false );
            } );
        }
    } ).detach();
}


int AI_CHAT_PANEL_BASE::createNewTab()
{
    if( static_cast<int>( m_tabs.size() ) >= TabConfig::MAX_TABS )
    {
        wxMessageBox( wxString::Format( wxT( "Maximum of %d tabs allowed. Please close a tab first." ),
                                        TabConfig::MAX_TABS ),
                      wxT( "Tab Limit" ), wxOK | wxICON_WARNING );
        return -1;
    }

    TAB_DATA newTab;
    // Generate a temporary conversation ID for the tab (will be replaced when first message is sent)
    newTab.conversationId = wxString::Format( wxT( "temp_%ld" ), wxGetLocalTime() );
    // NOTE: sessionId is global (m_sessionId), not per-tab
    newTab.title = wxT( "New Chat" );
    newTab.hasUnsavedChanges = false;
    newTab.messagesLoaded = true;  // New tabs don't need loading from DB
    
    // Initialize backend client and DEDICATED tool executor for this tab (prevents deadlocks!)
    newTab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( m_backendUrl.ToStdString() );
    newTab.toolExecutor = std::make_unique<AI_TOOL_EXECUTOR>();
    configureToolExecutor( newTab.toolExecutor.get() );  // Apply stored callbacks immediately
    newTab.backendClient->SetToolExecutor( newTab.toolExecutor.get() );

    m_tabs.push_back( std::move( newTab ) );
    int newIndex = static_cast<int>( m_tabs.size() ) - 1;
    
    // Add to webview tab bar
    if( m_chatWebview )
        m_chatWebview->AddTab( m_tabs[newIndex].conversationId, m_tabs[newIndex].title );

    // New tab starts with no conversation (will be created on first message)
    // Session ID is app-global, doesn't need to be set here
    m_conversationId = wxEmptyString;

    return newIndex;
}


bool AI_CHAT_PANEL_BASE::loadConversationToTab( const wxString& aConversationId )
{
    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    
    auto conv = db.LoadConversation( aConversationId );
    if( !conv.has_value() )
        return false;

    // Note: We don't stop streaming - allows parallel tasks across tabs

    // Check if this conversation is already open in a tab
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].conversationId == aConversationId )
        {
            // Switch to existing tab (no save needed - each tab has its own panel)
            switchToTab( static_cast<int>( i ) );
            return true;
        }
    }

    // Create new tab for this conversation
    // No save needed - each tab has its own persistent panel

    if( static_cast<int>( m_tabs.size() ) >= TabConfig::MAX_TABS )
    {
        // Close oldest tab to make room (not the current one)
        int tabToClose = ( m_currentTabIndex == 0 ) ? 1 : 0;
        
        // Remove from webview first
        if( m_chatWebview && !m_tabs[tabToClose].conversationId.IsEmpty() )
            m_chatWebview->RemoveTab( m_tabs[tabToClose].conversationId );
        
        m_tabs.erase( m_tabs.begin() + tabToClose );
        if( m_currentTabIndex > tabToClose )
            m_currentTabIndex--;
    }

    TAB_DATA newTab;
    newTab.conversationId = aConversationId;
    // NOTE: sessionId is global (m_sessionId) - shared by all tabs in this app session
    newTab.title = conv->title.IsEmpty() ? wxString( wxT( "Loaded Chat" ) ) : conv->title;
    newTab.hasUnsavedChanges = false;

    // Initialize backend client and DEDICATED tool executor for this tab (prevents deadlocks!)
    newTab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( m_backendUrl.ToStdString() );
    newTab.toolExecutor = std::make_unique<AI_TOOL_EXECUTOR>();
    configureToolExecutor( newTab.toolExecutor.get() );  // Apply stored callbacks immediately
    newTab.backendClient->SetToolExecutor( newTab.toolExecutor.get() );

    m_tabs.push_back( std::move( newTab ) );
    int newIndex = static_cast<int>( m_tabs.size() ) - 1;
    
    // Add to webview tab bar
    if( m_chatWebview )
        m_chatWebview->AddTab( m_tabs[newIndex].conversationId, m_tabs[newIndex].title );
    
    // Switch to the new tab (switchToTab handles message loading)
    m_currentTabIndex = -1;  // Reset so switchToTab actually switches
    switchToTab( newIndex );

    return true;
}


void AI_CHAT_PANEL_BASE::loadConversationToTabAsync( const wxString& aConversationId )
{
    // Check if already open in a tab (quick check, no DB access)
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].conversationId == aConversationId )
        {
            // Switch to existing tab (no save needed - each tab has its own panel)
            switchToTab( static_cast<int>( i ) );
            return;
        }
    }

    // NOTE: Don't show loading skeleton on current tab - we're creating a NEW tab
    // The new tab will show its own loading skeleton via loadMessagesForTab

    auto panelAlive = m_panelAlive;
    wxString convId = aConversationId;
    AI_CHAT_PANEL_BASE* self = this;

    // Join previous conversation load thread if still running
    if( m_conversationLoadThread && m_conversationLoadThread->joinable() )
    {
        m_conversationLoadThread->detach(); // Detach old thread to prevent blocking UI
    }

    // Load conversation metadata in background
    m_conversationLoadThread = std::make_unique<std::thread>( [self, panelAlive, convId]() {
        if( !panelAlive->load() )
            return;

        // Load conversation from DB
        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        auto conv = db.LoadConversation( convId );

        if( !conv.has_value() )
        {
            // Conversation not found - just return (no UI to update since we didn't show a skeleton)
            return;
        }

        // Prepare tab data
        wxString title = conv->title.IsEmpty() ? wxString( wxT( "Loaded Chat" ) ) : conv->title;

        // Update UI on main thread (use panel's CallAfter for reliable delivery)
        if( !panelAlive->load() )
            return;

        self->CallAfter( [self, panelAlive, convId, title]() {
            if( !panelAlive->load() || self->m_isDestroying.load() )
                return;

            // No save needed - each tab has its own persistent panel

            // Handle max tabs
            if( static_cast<int>( self->m_tabs.size() ) >= TabConfig::MAX_TABS )
            {
                int tabToClose = ( self->m_currentTabIndex == 0 ) ? 1 : 0;
                
                // Remove from webview first
                if( self->m_chatWebview && !self->m_tabs[tabToClose].conversationId.IsEmpty() )
                    self->m_chatWebview->RemoveTab( self->m_tabs[tabToClose].conversationId );
                
                self->m_tabs.erase( self->m_tabs.begin() + tabToClose );
                if( self->m_currentTabIndex > tabToClose )
                    self->m_currentTabIndex--;
            }
            
            // Create new tab
            TAB_DATA newTab;
            newTab.conversationId = convId;
            // NOTE: sessionId is global (m_sessionId), not per-tab
            newTab.title = title;
            newTab.hasUnsavedChanges = false;
            
            // Initialize backend client and DEDICATED tool executor for this tab (prevents deadlocks!)
            newTab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( self->m_backendUrl.ToStdString() );
            newTab.toolExecutor = std::make_unique<AI_TOOL_EXECUTOR>();
            self->configureToolExecutor( newTab.toolExecutor.get() );  // Apply stored callbacks immediately
            newTab.backendClient->SetToolExecutor( newTab.toolExecutor.get() );
            
            self->m_tabs.push_back( std::move( newTab ) );
            int newIndex = static_cast<int>( self->m_tabs.size() ) - 1;
            
            // Add tab to webview
            if( self->m_chatWebview )
                self->m_chatWebview->AddTab( self->m_tabs[newIndex].conversationId, self->m_tabs[newIndex].title );
            
            // Switch to the new tab (switchToTab handles message loading)
            self->m_currentTabIndex = -1;  // Reset so switchToTab actually switches
            self->switchToTab( newIndex );
            
            // Save tab state after loading from history
            self->saveOpenTabs();
        } );
    } );
}


void AI_CHAT_PANEL_BASE::saveOpenTabs()
{
    if( m_tabs.empty() )
        return;
    
    // Use cached project path (safe for destructor - doesn't call pure virtual)
    wxString projectPath = m_cachedProjectPath;
    if( projectPath.IsEmpty() )
        projectPath = wxT( "Untitled" );
    
    // Build the list of open tabs
    std::vector<OPEN_TAB> openTabs;
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        // Only save tabs that have valid conversation IDs (not empty, not temp)
        if( m_tabs[i].conversationId.IsEmpty() || m_tabs[i].conversationId.StartsWith( wxT( "temp_" ) ) )
            continue;
        
        OPEN_TAB tab;
        tab.conversation_id = m_tabs[i].conversationId;
        tab.tab_order = static_cast<int>( i );
        tab.is_active = ( static_cast<int>( i ) == m_currentTabIndex );
        tab.project_file_path = projectPath;
        openTabs.push_back( tab );
    }
    
    // Save to database (it's OK if openTabs is empty - just clears the saved tabs)
    CONVERSATION_DB& db = CONVERSATION_DB::Instance();
    if( !db.IsOpen() )
    {
        // Database not initialized - skip saving silently during shutdown
        return;
    }
    
    if( !db.SaveOpenTabs( openTabs, projectPath ) )
    {
        // Only warn if we actually had tabs to save
        if( !openTabs.empty() )
        {
            wxLogWarning( wxT( "AI: Failed to save open tabs" ) );
        }
    }
}


bool AI_CHAT_PANEL_BASE::loadPersistedTabs()
{
    try
    {
        // Get project file path for scoping and cache it
        wxString projectPath = GetCurrentFileName();
        if( projectPath.IsEmpty() )
            projectPath = wxT( "Untitled" );
        
        // Update cached path (safe access for destructor)
        m_cachedProjectPath = projectPath;
        
        // Load from database
        CONVERSATION_DB& db = CONVERSATION_DB::Instance();
        std::vector<OPEN_TAB> openTabs = db.LoadOpenTabs( projectPath );
        
        if( openTabs.empty() )
        {
            return false;
        }
        
        // Reorder: Put active tab first (position 0) for better UX
        auto activeIt = std::find_if( openTabs.begin(), openTabs.end(),
                                      []( const OPEN_TAB& tab ) { return tab.is_active; } );
        if( activeIt != openTabs.end() && activeIt != openTabs.begin() )
        {
            OPEN_TAB activeTab = *activeIt;
            openTabs.erase( activeIt );
            openTabs.insert( openTabs.begin(), activeTab );
        }
    
    // Clear any existing tabs (the initial empty tab created during buildUI)
    // We need to do this BEFORE adding restored tabs
    while( !m_tabs.empty() )
    {
        // Remove from webview first (before destroying the tab data)
        if( m_chatWebview && !m_tabs.back().conversationId.IsEmpty() )
        {
            m_chatWebview->RemoveTab( m_tabs.back().conversationId );
        }
        
        m_tabs.pop_back();
    }

    // Process pending deletions before creating new panels
    wxYield();
    
    // Active tab is now always at position 0 (we reordered above)
    int activeTabIndex = 0;
    
    // Create tabs from persisted data
    for( size_t i = 0; i < openTabs.size(); i++ )
    {
        const OPEN_TAB& openTab = openTabs[i];
        
        // Verify the conversation still exists in the database
        auto conv = db.LoadConversation( openTab.conversation_id );
        if( !conv.has_value() )
        {
            continue;
        }
        
        // Create the tab
        TAB_DATA newTab;
        newTab.conversationId = openTab.conversation_id;
        // NOTE: sessionId is global (m_sessionId) - generated once at app start, shared by all tabs
        newTab.title = conv->title.IsEmpty() ? wxString( wxT( "Loaded Chat" ) ) : conv->title;
        newTab.hasUnsavedChanges = false;
        
        // Initialize backend client and DEDICATED tool executor for this tab (prevents deadlocks!)
        newTab.backendClient = std::make_unique<AI_BACKEND_CLIENT>( m_backendUrl.ToStdString() );
        newTab.toolExecutor = std::make_unique<AI_TOOL_EXECUTOR>();
        configureToolExecutor( newTab.toolExecutor.get() );  // Apply stored callbacks immediately
        newTab.backendClient->SetToolExecutor( newTab.toolExecutor.get() );
        
        m_tabs.push_back( std::move( newTab ) );
        int tabIndex = static_cast<int>( m_tabs.size() ) - 1;
        
        // Add to webview tab bar
        if( m_chatWebview )
        {
            m_chatWebview->AddTab( m_tabs[tabIndex].conversationId, m_tabs[tabIndex].title );
        }
        
        // Active tab is now always at index 0 (we reordered the vector before this loop)
        // No need to track it dynamically
    }
    
    // If we didn't restore any tabs, return false to let caller create a fresh one
    if( m_tabs.empty() )
    {
        return false;
    }
    
    // Switch to the previously active tab (switchToTab will load messages if needed)
    m_currentTabIndex = -1;  // Reset so switchToTab actually switches
    switchToTab( activeTabIndex );
    // Note: Don't call loadMessagesForTab here - switchToTab already does it

    return true;
    }
    catch( const std::exception& e )
    {
        wxLogError( "[AI_CHAT] Exception loading persisted tabs: %s", e.what() );
        return false;
    }
    catch( ... )
    {
        wxLogError( "[AI_CHAT] Unknown exception loading persisted tabs" );
        return false;
    }
}


void AI_CHAT_PANEL_BASE::HandleFileEditEvent( const AI_BACKEND_EVENT& aEvent, int aTabIndex )
{
    if( !aEvent.fileModified || !m_frame )
        return;

    wxString filePath = GetCurrentFileName();
    if( filePath.IsEmpty() )
        return;
    
    // Mark this file as modified by the specific tab (for per-tab reload tracking)
    markFileModifiedByTab( filePath, aTabIndex );

    // During streaming: Queue for batch update instead of immediate reload
    // This shows incremental changes every 500ms without overwhelming the UI
    bool anyStreaming = isAnyTabStreaming();
    
    if( anyStreaming )
    {
        m_batchUpdatePending.store( true );
        
        // Start batch timer if not already running (500ms batching interval)
        if( !m_streamingBatchTimer )
        {
            m_streamingBatchTimer = new wxTimer( this );
            Bind( wxEVT_TIMER, &AI_CHAT_PANEL_BASE::onStreamingBatchTimer, this,
                  m_streamingBatchTimer->GetId() );
        }
        
        if( !m_streamingBatchTimer->IsRunning() )
        {
            m_streamingBatchTimer->Start( 500, wxTIMER_ONE_SHOT );
        }
        
        return;
    }

    // Not streaming - perform immediate reload (for manual refresh, etc.)
    std::lock_guard<std::mutex> lock( m_reloadMutex );

    // Check memory before reload to prevent crashes
    wxMemorySize freeMemory = wxGetFreeMemory();
    if( freeMemory != wxNOT_FOUND && freeMemory < 500 * 1024 * 1024 )  // < 500 MB free
    {
        wxLogWarning( wxT( "AI: Low memory (%lld MB free), skipping reload" ),
                     ( freeMemory / (1024*1024) ).GetValue() );
        return;
    }

    // Check if user requested stop on any tab
    if( isAnyTabStopRequested() )
    {
        return;
    }

    // Capture state before reload if not already captured
    if( m_aiEditInProgress && !m_aiEditStateCaptured )
    {
        CaptureStateForAIEdit( filePath );
        m_aiEditStateCaptured = true;
    }

    // CRITICAL: Flush pending conversions before reload
    // Conversion is queued/debounced, so we must flush before loading the kicad file
    // Get the tab that triggered this event
    if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) && m_tabs[aTabIndex].toolExecutor )
    {
        wxLogDebug( wxT( "AI DEBUG [HandleFileEditEvent]: Flushing pending conversion for tab %d" ), aTabIndex );
        m_tabs[aTabIndex].toolExecutor->flushPendingConversion( true );
    }

    // Direct reload (no timer needed when not streaming)
    wxLogDebug( wxT( "AI DEBUG [HandleFileEditEvent]: Calling ReloadFromFile: %s" ), filePath );
    if( ReloadFromFile( filePath ) )
    {
        wxLogDebug( wxT( "AI DEBUG [HandleFileEditEvent]: ReloadFromFile succeeded" ) );
        CompareAndCreateAIEditUndoEntries();
        
        // Autoplace fields for modified symbols
        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) && m_tabs[aTabIndex].toolExecutor )
        {
            std::set<std::string> modifiedUUIDs = m_tabs[aTabIndex].toolExecutor->GetModifiedSymbolUUIDs();
            if( !modifiedUUIDs.empty() )
            {
                AutoplaceModifiedSymbols( modifiedUUIDs );
                m_tabs[aTabIndex].toolExecutor->ClearModifiedSymbolUUIDs();
            }
        }
        
        // Auto-annotate all symbols after trace edits
        AnnotateAllSymbols();
        
        // Only save document if conversion succeeded
        // If conversion failed, saving would trigger kicad-to-trace conversion
        // which would overwrite the AI's edit to the trace_sch file
        bool conversionOk = true;
        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) && m_tabs[aTabIndex].toolExecutor )
        {
            conversionOk = m_tabs[aTabIndex].toolExecutor->WasLastConversionSuccessful();
        }
        
        wxLogDebug( wxT( "AI DEBUG [HandleFileEditEvent]: conversionOk=%s" ), conversionOk ? wxT( "true" ) : wxT( "false" ) );
        
        if( conversionOk )
        {
            wxLogDebug( wxT( "AI DEBUG [HandleFileEditEvent]: Calling SaveDocument()" ) );
            SaveDocument();
            MarkDocumentAsSaved();
        }
        else
        {
            wxString convError = ( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) && m_tabs[aTabIndex].toolExecutor ) ? 
                wxString::FromUTF8( m_tabs[aTabIndex].toolExecutor->GetLastConversionError() ) : 
                wxString( wxT( "Unknown error" ) );
            wxLogWarning( wxT( "AI DEBUG [HandleFileEditEvent]: SKIPPING SaveDocument() - conversion failed: %s" ), convError );
        }
    }
    else
    {
        wxLogWarning( wxT( "AI DEBUG [HandleFileEditEvent]: ReloadFromFile FAILED for: %s" ), filePath );
    }
}


void AI_CHAT_PANEL_BASE::onStreamingBatchTimer( wxTimerEvent& aEvent )
{
    // Safety check: Only process if any tab is still streaming
    if( !isAnyTabStreaming() )
    {
        return;
    }

    // Check if batch update is actually pending
    if( !m_batchUpdatePending.exchange( false ) )
    {
        return;
    }

    wxString filePath = GetCurrentFileName();
    if( filePath.IsEmpty() )
    {
        wxLogWarning( wxT( "AI: Batch update - no current file" ) );
        return;
    }

    // Check memory before reload
    wxMemorySize freeMemory = wxGetFreeMemory();
    if( freeMemory != wxNOT_FOUND && freeMemory < 500 * 1024 * 1024 )
    {
        wxLogWarning( wxT( "AI: Low memory (%lld MB free), skipping batch update" ),
                     ( freeMemory / (1024*1024) ).GetValue() );
        return;
    }

    // CRITICAL: Flush pending conversions BEFORE reload
    // This ensures trace_pcb -> kicad_pcb conversion happens before KiCad loads the file
    // Flush conversions for ALL streaming tabs (not just current)
    bool conversionHappened = false;
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].isStreaming.load() && m_tabs[i].toolExecutor )
    {
            bool tabConversion = m_tabs[i].toolExecutor->flushPendingConversion( true );
            conversionHappened = conversionHappened || tabConversion;
            wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: Tab %zu flushPendingConversion returned %s" ), 
                       i, tabConversion ? wxT( "true" ) : wxT( "false" ) );
        }
    }

    // Capture state before first reload during AI edit
    if( m_aiEditInProgress && !m_aiEditStateCaptured )
    {
        CaptureStateForAIEdit( filePath );
        m_aiEditStateCaptured = true;
    }

    // Perform the reload (safe during streaming with proper batching)
    wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: Calling ReloadFromFile: %s" ), filePath );
    if( ReloadFromFile( filePath ) )
    {
        wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: ReloadFromFile succeeded" ) );
        CompareAndCreateAIEditUndoEntries();
        
        // Autoplace fields for modified symbols (check all streaming tabs)
        for( size_t i = 0; i < m_tabs.size(); i++ )
        {
            if( m_tabs[i].isStreaming.load() && m_tabs[i].toolExecutor )
            {
                std::set<std::string> modifiedUUIDs = m_tabs[i].toolExecutor->GetModifiedSymbolUUIDs();
                if( !modifiedUUIDs.empty() )
                {
                    AutoplaceModifiedSymbols( modifiedUUIDs );
                    m_tabs[i].toolExecutor->ClearModifiedSymbolUUIDs();
                }
            }
        }
        
        // Auto-annotate all symbols after trace edits
        AnnotateAllSymbols();
        
        // Only save document if all conversions succeeded
        // If conversion failed, saving would trigger kicad-to-trace conversion
        // which would overwrite the AI's edit to the trace_sch file
        bool allConversionsOk = true;
        for( size_t i = 0; i < m_tabs.size(); i++ )
        {
            if( m_tabs[i].isStreaming.load() && m_tabs[i].toolExecutor && 
                !m_tabs[i].toolExecutor->WasLastConversionSuccessful() )
            {
                allConversionsOk = false;
                wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: Tab %zu conversion failed" ), i );
                break;
            }
        }
        
        wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: allConversionsOk=%s" ), allConversionsOk ? wxT( "true" ) : wxT( "false" ) );
        
        if( allConversionsOk )
        {
            wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: Calling SaveDocument()" ) );
            SaveDocument();
        }
        else
        {
            wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: SKIPPING SaveDocument() due to conversion failure" ) );
        }
    }
    else
    {
        wxLogDebug( wxT( "AI DEBUG [onStreamingBatchTimer]: ReloadFromFile FAILED" ) );
    }
}


void AI_CHAT_PANEL_BASE::onIdleStatusTimer( wxTimerEvent& aEvent )
{
    // Critical: Don't touch UI if panel is being destroyed
    if( m_isDestroying.load() )
        return;

    // Find which tab's timer fired by checking the timer ID
    int tabIndex = -1;
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].idleStatusTimer && m_tabs[i].idleStatusTimer->GetId() == aEvent.GetId() )
        {
            tabIndex = static_cast<int>( i );
            break;
        }
    }

    if( tabIndex < 0 )
        return;

    TAB_DATA& tab = m_tabs[tabIndex];

    // Only show status if this tab is still streaming
    if( !tab.isStreaming.load() || tab.stopRequested.load() )
        return;

    // If the last event was a tool call or non-generic status, the existing status
    // (e.g. "Running autoroute") is still relevant — don't overwrite it with "Working..."
    if( tab.lastEventType == AI_EVENT_TYPE::STATUS
        || tab.lastEventType == AI_EVENT_TYPE::TOOL_CALL )
    {
        tab.idleStatusTimer->Start( IDLE_STATUS_TIMEOUT_MS, wxTIMER_ONE_SHOT );
        return;
    }

    // Track how many times the idle timer has fired for this stream
    // We use a static map, but reset it when pendingStreamingResponse is empty (new stream)
    static std::map<int, int> tabIdleCount;
    
    // If no response yet, this is a fresh stream - reset counter
    if( tab.pendingStreamingResponse.IsEmpty() && tab.streamingBuffer.IsEmpty() )
    {
        tabIdleCount[tabIndex] = 0;
    }
    
    int& idleCount = tabIdleCount[tabIndex];
    
    if( idleCount == 0 )
    {
        // First idle timeout - just show typing indicator (the dots animation)
        if( m_chatWebview && tabIndex == m_currentTabIndex )
        {
            m_chatWebview->ShowTypingIndicator();
        }
    }
    else
    {
        // Subsequent idle timeouts - rotate through friendly messages
        static const wxString idleMessages[] = {
            wxT( "Thinking..." ),
            wxT( "Planning next steps..." ),
            wxT( "Still working on it..." ),
            wxT( "Analyzing..." ),
            wxT( "Processing..." )
        };
        
        wxString statusMsg = idleMessages[(idleCount - 1) % 5];

        // Update webview (if this is the active tab)
        if( m_chatWebview && tabIndex == m_currentTabIndex )
        {
            m_chatWebview->HideTypingIndicator();
            m_chatWebview->ShowStatus( statusMsg, true );
        }
    }
    
    idleCount++;
}


void AI_CHAT_PANEL_BASE::resetIdleStatusTimer( int aTabIndex )
{
    // Validate tab index
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];

    // Only reset if this tab is streaming
    if( !tab.isStreaming.load() )
        return;

    // Create timer if it doesn't exist
    if( !tab.idleStatusTimer )
    {
        tab.idleStatusTimer = new wxTimer( this, wxID_ANY );
        Bind( wxEVT_TIMER, &AI_CHAT_PANEL_BASE::onIdleStatusTimer, this, tab.idleStatusTimer->GetId() );
    }

    // Stop and restart the timer (resets the 2-second countdown)
    if( tab.idleStatusTimer->IsRunning() )
    {
        tab.idleStatusTimer->Stop();
    }
    tab.idleStatusTimer->Start( IDLE_STATUS_TIMEOUT_MS, wxTIMER_ONE_SHOT );
}


void AI_CHAT_PANEL_BASE::stopIdleStatusTimer( int aTabIndex )
{
    // Validate tab index
    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];

    // Stop the timer if it exists and is running
    if( tab.idleStatusTimer )
    {
        if( tab.idleStatusTimer->IsRunning() )
        {
            tab.idleStatusTimer->Stop();
        }
    }
}


void AI_CHAT_PANEL_BASE::onReloadDebounceTimer( wxTimerEvent& aEvent )
{
    std::lock_guard<std::mutex> lock( m_reloadMutex );
    
    // CRITICAL: Check if already reloading (prevents COM reentrancy)
    if( m_reloadInProgress.load() )
    {
        m_reloadPending.store( true );  // Re-queue
        // Do NOT start timer here - will be handled after current reload completes
        return;
    }
    
    wxString pathToReload = m_pendingReloadPath;
    m_reloadPending.store( false );
    
    // Check if user cancelled during debounce
    if( isAnyTabStopRequested() )
    {
        m_reloadInProgress.store( false );
        return;
    }
    
    // Mark reload in progress before starting
    m_reloadInProgress.store( true );
    
    // CRITICAL: Flush pending conversions before reload
    // This ensures trace_sch -> kicad_sch conversion completes before we load the file
    // Flush conversions for ALL tabs
    bool conversionHappened = false;
    for( size_t i = 0; i < m_tabs.size(); i++ )
    {
        if( m_tabs[i].toolExecutor )
        {
            bool tabConversion = m_tabs[i].toolExecutor->flushPendingConversion( true );
            conversionHappened = conversionHappened || tabConversion;
            wxLogDebug( wxT( "AI DEBUG [onReloadDebounceTimer]: Tab %zu flushPendingConversion returned %s" ), 
                       i, tabConversion ? wxT( "true" ) : wxT( "false" ) );
        }
    }
    
    // Perform the actual reload on main thread (we're already on main thread via timer)
    wxLogDebug( wxT( "AI DEBUG [onReloadDebounceTimer]: Calling ReloadFromFile: %s" ), pathToReload );
    bool success = ReloadFromFile( pathToReload );
    
    if( success )
    {
        wxLogDebug( wxT( "AI DEBUG [onReloadDebounceTimer]: ReloadFromFile succeeded" ) );
        // Create undo entries after successful reload
        CompareAndCreateAIEditUndoEntries();
        
        // Autoplace fields for modified symbols (check all tabs)
        for( size_t i = 0; i < m_tabs.size(); i++ )
        {
            if( m_tabs[i].toolExecutor )
            {
                std::set<std::string> modifiedUUIDs = m_tabs[i].toolExecutor->GetModifiedSymbolUUIDs();
                if( !modifiedUUIDs.empty() )
                {
                    AutoplaceModifiedSymbols( modifiedUUIDs );
                    m_tabs[i].toolExecutor->ClearModifiedSymbolUUIDs();
                }
            }
        }
        
        // Auto-annotate all symbols after trace edits
        AnnotateAllSymbols();
        
        // Check if all conversions succeeded
        bool allConversionsOk = true;
        for( size_t i = 0; i < m_tabs.size(); i++ )
        {
            if( m_tabs[i].toolExecutor && !m_tabs[i].toolExecutor->WasLastConversionSuccessful() )
            {
                allConversionsOk = false;
                wxString convError = wxString::FromUTF8( m_tabs[i].toolExecutor->GetLastConversionError() );
                wxLogWarning( wxT( "AI DEBUG [onReloadDebounceTimer]: Tab %zu conversion failed: %s" ), i, convError );
            }
        }
        
        wxLogDebug( wxT( "AI DEBUG [onReloadDebounceTimer]: allConversionsOk=%s" ), allConversionsOk ? wxT( "true" ) : wxT( "false" ) );
        
        // Only save document if all conversions succeeded
        // If conversion failed, saving would trigger kicad-to-trace conversion
        // which would overwrite the AI's edit to the trace_sch file
        if( allConversionsOk )
        {
            wxLogDebug( wxT( "AI DEBUG [onReloadDebounceTimer]: Calling SaveDocument()" ) );
            SaveDocument();
            MarkDocumentAsSaved();
        }
        else
        {
            wxLogWarning( wxT( "AI DEBUG [onReloadDebounceTimer]: SKIPPING SaveDocument() to preserve trace_sch edits" ) );
        }
    }
    else
    {
        wxLogWarning( wxT( "AI: Reload failed for: %s" ), pathToReload );
    }
    
    // Mark reload complete
    m_reloadInProgress.store( false );
    
    // If another reload was queued during this one, start timer for it
    if( m_reloadPending.load() && !m_pendingReloadPath.IsEmpty() )
    {
        m_reloadInProgress.store( true );
        m_reloadDebounceTimer->Start( 1000, wxTIMER_ONE_SHOT );
    }
}
