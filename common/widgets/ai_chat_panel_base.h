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

#ifndef AI_CHAT_PANEL_BASE_H
#define AI_CHAT_PANEL_BASE_H

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/log.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <mutex>
#include <functional>
#include <set>
#include <map>
#include <vector>
#include <widgets/chat_webview_panel.h>
#include <json_common.h>
#include <ai_backend_client.h>
#include <ai_tool_executor.h>

// ============================================
// Quota/Banner Configuration Constants
// ============================================
namespace QuotaConfig
{
    /// Default daily request limit (used if backend returns 0)
    constexpr int DEFAULT_DAILY_LIMIT = 50;
    
    /// Show warning when trial has <= this many hours left
    constexpr int TRIAL_LOW_HOURS_THRESHOLD = 4;
    
    /// Show warning at this percentage of daily limit
    constexpr int DAILY_USAGE_WARNING_PERCENT = 80;
    
    /// Show warning when on-demand credits <= this value
    constexpr int CREDITS_WARNING_THRESHOLD = 10;
    
    /// Show "Low credits!" when on-demand credits <= this value
    constexpr int CREDITS_CRITICAL_THRESHOLD = 5;
}

// ============================================
// Tab Configuration Constants
// ============================================
namespace TabConfig
{
    /// Maximum number of conversation tabs allowed
    constexpr int MAX_TABS = 10;
}

class EDA_DRAW_FRAME;
class wxCommandEvent;
class wxTimerEvent;

/**
 * Message data for serialization/deserialization.
 */
struct CHAT_MESSAGE_DATA
{
    wxString role;     ///< 'user' or 'assistant' or 'system'
    wxString content;  ///< Message text
    wxString metadata; ///< JSON metadata (optional)
};

// Forward declarations
class AI_BACKEND_CLIENT;

/**
 * Data for a single conversation tab.
 * 
 * Supports TRUE parallel execution: each tab has its own backend client
 * and can stream independently without blocking other tabs.
 * Visual UI is handled by the webview (CHAT_WEBVIEW_PANEL).
 */
struct TAB_DATA
{
    wxString                       conversationId;  ///< Unique conversation ID (UUID)
    // NOTE: sessionId is GLOBAL (m_sessionId) - shared by all tabs in an app session
    wxString                       title;           ///< Tab title (derived from first message or "New Chat")
    std::vector<CHAT_MESSAGE_DATA> messages;        ///< Chat messages in this tab (for DB persistence)
    bool                           hasUnsavedChanges; ///< True if messages not saved locally
    wxString                       draftInput;      ///< Saved input text when switching away from this tab
    
    // Streaming state (preserved across tab switches) - per-tab for parallel execution
    wxString                       pendingStreamingResponse; ///< In-progress AI response
    std::atomic<bool>              isStreaming;      ///< True if this tab is actively receiving a stream
    std::atomic<bool>              stopRequested;    ///< True if user requested to stop this tab's stream
    std::unique_ptr<std::thread>   requestThread;    ///< Background thread for this tab's request
    wxString                       streamingBuffer;  ///< Buffer for streaming text in this tab
    int                            pendingDeltaCount; ///< Count of pending text deltas for batching
    bool                           isFirstStreamingFlush; ///< True if next flush is first for this stream
    wxString                       streamingMessageId;   ///< Unique ID for current streaming message (for webview)
    int                            streamingMessageCounter; ///< Counter for generating unique message IDs
    wxString                       lastSavedDbMessageId; ///< DB message ID for version metadata updates
    wxString                       pendingUserMsgId;     ///< User message ID shared between SQLite and webview
    
    // Per-tab backend client for TRUE parallel execution (like Cursor)
    std::unique_ptr<AI_BACKEND_CLIENT> backendClient; ///< Each tab gets its own HTTP client
    std::unique_ptr<AI_TOOL_EXECUTOR>  toolExecutor;  ///< Each tab has its own tool executor (prevents file access deadlocks!)
    
    // Per-tab file modification tracking (fixes multi-tab editing conflicts)
    std::atomic<bool>              fileModifiedDuringStream; ///< True if this tab's tools modified files
    std::set<wxString>             modifiedFiles;            ///< Files modified by this tab during streaming
    
    // Per-tab version tracking (prevents cross-tab version ID contamination)
    wxString                       lastSavedVersionId;       ///< Last saved version ID for this tab's stream
    
    // Tab state tracking
    bool                           messagesLoaded;     ///< True if messages have been loaded from DB for this tab
    std::atomic<bool>              isLoadingMessages;  ///< True while messages are being loaded (prevents duplicate loads)
    
    // Idle status timer - shows "Working..." if no events received for 2 seconds during streaming
    wxTimer*                       idleStatusTimer;    ///< Per-tab timer for idle detection (owned by this struct)
    AI_EVENT_TYPE                  lastEventType;      ///< Last SSE event type received (for idle timer decision)
    wxString                       lastStatusMessage;  ///< Last non-generic status shown (preserved across idle ticks)

    bool                           thinkingActive;     ///< True while AI is in a thinking phase (for HideThinking dedup)

    wxString                       accumulatedThinking; ///< Accumulated thinking content for persistence
    long long                      thinkingStartMs = 0;     ///< Timestamp (ms) when thinking started
    long long                      thinkingDurationMs = 0;  ///< Total accumulated thinking duration (ms)
    size_t                         lastThinkingBlockUsed = 0; ///< Index+1 of the last ThinkingBlock used as marker context (0 = none used)

    /// Tracks status labels at the text offset where they occurred, enabling
    /// reconstruction of the interleaved view (text → activity → text → ...) on reload.
    struct ActivityMarker
    {
        int       charOffset;    ///< Character offset in pendingStreamingResponse when this status appeared
        wxString  label;         ///< Status label (e.g., "Reading file...", "Writing file...")
        long long timestampMs;   ///< Wall-clock milliseconds since epoch when this marker was created
        wxString  thinkingContext; ///< Thinking content captured just before this tool call (write/edit only)
    };
    std::vector<ActivityMarker>    activityMarkers;   ///< Status events recorded with their text positions

    /// A single contiguous block of thinking content, captured between tool calls / text output.
    struct ThinkingBlock
    {
        wxString  content;       ///< The thinking text for this block
        int       charOffset;    ///< Position in pendingStreamingResponse where this thinking occurred
        long long durationMs;    ///< How long this thinking block lasted (ms)
        wxString  agentId;       ///< Source agent (empty = main agent, "research_X" = orchestrator sub-agent)
    };
    std::vector<ThinkingBlock>    thinkingBlocks;     ///< Per-block thinking content for interleaved persistence

    wxString                       researchSummary;    ///< Research text captured at clarification phase transition
    wxString                       currentPhase;       ///< Current workflow phase (research, clarification, planning, etc.)

    // Analytics: per-conversation engagement tracking
    int                            userMessageCount;       ///< Number of user messages sent in this conversation
    int                            aiMessageCount;         ///< Number of AI responses received
    int                            totalUserCharsSent;     ///< Total characters typed by user
    int                            toolCallCount;          ///< Number of AI tool calls executed
    int                            fileEditCount;          ///< Number of file-modifying tool calls (search_replace, write)
    std::chrono::steady_clock::time_point conversationStartTime; ///< When first message was sent

    TAB_DATA() : hasUnsavedChanges( false ), isStreaming( false ), stopRequested( false ), pendingDeltaCount( 0 ), isFirstStreamingFlush( false ), streamingMessageCounter( 0 ), fileModifiedDuringStream( false ), messagesLoaded( false ), isLoadingMessages( false ), idleStatusTimer( nullptr ), lastEventType( AI_EVENT_TYPE::TEXT_DELTA ), thinkingActive( false ), userMessageCount( 0 ), aiMessageCount( 0 ), totalUserCharsSent( 0 ), toolCallCount( 0 ), fileEditCount( 0 ) {}
    
    // Move constructor for std::vector compatibility
    TAB_DATA( TAB_DATA&& other ) noexcept
        : conversationId( std::move( other.conversationId ) ),
          title( std::move( other.title ) ),
          messages( std::move( other.messages ) ),
          hasUnsavedChanges( other.hasUnsavedChanges ),
          draftInput( std::move( other.draftInput ) ),
          pendingStreamingResponse( std::move( other.pendingStreamingResponse ) ),
          isStreaming( other.isStreaming.load() ),
          stopRequested( other.stopRequested.load() ),
          requestThread( std::move( other.requestThread ) ),
          streamingBuffer( std::move( other.streamingBuffer ) ),
          pendingDeltaCount( other.pendingDeltaCount ),
          isFirstStreamingFlush( other.isFirstStreamingFlush ),
          streamingMessageId( std::move( other.streamingMessageId ) ),
          streamingMessageCounter( other.streamingMessageCounter ),
          lastSavedDbMessageId( std::move( other.lastSavedDbMessageId ) ),
          pendingUserMsgId( std::move( other.pendingUserMsgId ) ),
          backendClient( std::move( other.backendClient ) ),
          toolExecutor( std::move( other.toolExecutor ) ),
          fileModifiedDuringStream( other.fileModifiedDuringStream.load() ),
          modifiedFiles( std::move( other.modifiedFiles ) ),
          lastSavedVersionId( std::move( other.lastSavedVersionId ) ),
          messagesLoaded( other.messagesLoaded ),
          isLoadingMessages( other.isLoadingMessages.load() ),
          idleStatusTimer( other.idleStatusTimer ),
          lastEventType( other.lastEventType ),
          lastStatusMessage( std::move( other.lastStatusMessage ) ),
          thinkingActive( other.thinkingActive ),
          accumulatedThinking( std::move( other.accumulatedThinking ) ),
          thinkingStartMs( other.thinkingStartMs ),
          thinkingDurationMs( other.thinkingDurationMs ),
          activityMarkers( std::move( other.activityMarkers ) ),
          thinkingBlocks( std::move( other.thinkingBlocks ) ),
          userMessageCount( other.userMessageCount ),
          aiMessageCount( other.aiMessageCount ),
          totalUserCharsSent( other.totalUserCharsSent ),
          toolCallCount( other.toolCallCount ),
          fileEditCount( other.fileEditCount ),
          conversationStartTime( other.conversationStartTime )
    {
        other.idleStatusTimer = nullptr;  // Transfer ownership
    }
    
    // Move assignment for std::vector compatibility
    TAB_DATA& operator=( TAB_DATA&& other ) noexcept
    {
        if( this != &other )
        {
            conversationId = std::move( other.conversationId );
            title = std::move( other.title );
            messages = std::move( other.messages );
            hasUnsavedChanges = other.hasUnsavedChanges;
            draftInput = std::move( other.draftInput );
            pendingStreamingResponse = std::move( other.pendingStreamingResponse );
            isStreaming.store( other.isStreaming.load() );
            stopRequested.store( other.stopRequested.load() );
            requestThread = std::move( other.requestThread );
            streamingBuffer = std::move( other.streamingBuffer );
            pendingDeltaCount = other.pendingDeltaCount;
            isFirstStreamingFlush = other.isFirstStreamingFlush;
            streamingMessageId = std::move( other.streamingMessageId );
            streamingMessageCounter = other.streamingMessageCounter;
            lastSavedDbMessageId = std::move( other.lastSavedDbMessageId );
            backendClient = std::move( other.backendClient );
            toolExecutor = std::move( other.toolExecutor );
            fileModifiedDuringStream.store( other.fileModifiedDuringStream.load() );
            modifiedFiles = std::move( other.modifiedFiles );
            lastSavedVersionId = std::move( other.lastSavedVersionId );
            messagesLoaded = other.messagesLoaded;
            isLoadingMessages.store( other.isLoadingMessages.load() );
            // Transfer idle status timer ownership
            if( idleStatusTimer )
            {
                idleStatusTimer->Stop();
                delete idleStatusTimer;
            }
            idleStatusTimer = other.idleStatusTimer;
            other.idleStatusTimer = nullptr;
            lastEventType = other.lastEventType;
            lastStatusMessage = std::move( other.lastStatusMessage );
            thinkingActive = other.thinkingActive;
            accumulatedThinking = std::move( other.accumulatedThinking );
            thinkingStartMs = other.thinkingStartMs;
            thinkingDurationMs = other.thinkingDurationMs;
            activityMarkers = std::move( other.activityMarkers );
            thinkingBlocks = std::move( other.thinkingBlocks );
            userMessageCount = other.userMessageCount;
            aiMessageCount = other.aiMessageCount;
            totalUserCharsSent = other.totalUserCharsSent;
            toolCallCount = other.toolCallCount;
            fileEditCount = other.fileEditCount;
            conversationStartTime = other.conversationStartTime;
        }
        return *this;
    }
    
    // Delete copy operations (has std::atomic and std::unique_ptr)
    TAB_DATA( const TAB_DATA& ) = delete;
    TAB_DATA& operator=( const TAB_DATA& ) = delete;
};

/**
 * AI chat mode selection - determines how messages are processed by the backend.
 * 
 * PLAN Mode:
 *   - Sends mode="plan" to backend
 *   - Full multi-agent planning workflow
 *   - Asks clarifying questions, generates detailed plan, then executes with tools
 *   - Use for complex circuit design tasks
 * 
 * ASK Mode (default):
 *   - Sends mode="ask" to backend
 *   - Read-only Q&A about circuits
 *   - Can analyze schematic, answer questions about components/connections
 *   - Does NOT modify files or execute tools that change anything
 *   - Best for learning about circuits or getting explanations
 * 
 * AGENT Mode:
 *   - Sends mode="agent" to backend
 *   - Direct implementation with all tools
 *   - Immediately executes requests without asking questions
 *   - Use for quick edits when you know exactly what you want
 */
enum class AI_MODE
{
    PLAN, // Full planning workflow - questions, plan, execute
    ASK,  // Read-only Q&A - analyze and explain only
    AGENT // Direct implementation - immediate execution with tools (default)
};

/**
 * Base panel providing an AI agent chat interface.
 * 
 * Communicates with a local Python subprocess via stdin/stdout JSON messages.
 * The subprocess handles file I/O and communication with the remote AI server.
 * 
 * Derived classes should implement the virtual methods for app-specific functionality.
 */
class AI_CHAT_PANEL_BASE : public wxPanel
{
public:
    AI_CHAT_PANEL_BASE( wxWindow* aParent, EDA_DRAW_FRAME* aFrame );
    virtual ~AI_CHAT_PANEL_BASE();

    /**
     * Set the backend URL for the AI service.
     * @param aUrl The URL to send chat requests to.
     */
    void SetBackendUrl( const wxString& aUrl ) { m_backendUrl = aUrl; }

    /**
     * Get the current backend URL.
     */
    wxString GetBackendUrl() const { return m_backendUrl; }

protected:
    /**
     * Virtual methods to be implemented by derived classes for app-specific functionality.
     */

    /**
     * Reload the file from disk.
     * @param aFileName The absolute path to the file to reload.
     * @return True if the file was reloaded successfully.
     */
    virtual bool ReloadFromFile( const wxString& aFileName ) = 0;

    /**
     * Capture the current state before an AI edit.
     * This saves all items to a map keyed by UUID and creates a backup of the trace file.
     * Should only be called once per AI edit sequence (on the first replace_in_file call).
     * @param aFilePath Path to the trace file to backup.
     * @return True if state was captured successfully.
     */
    virtual bool CaptureStateForAIEdit( const wxString& aFilePath ) = 0;

    /**
     * Compare the state before and after an AI edit, and create undo entries.
     * This should be called after ReloadFromFile() completes.
     * Creates undo entries for deleted, new, and changed items.
     * @return True if comparison and undo entry creation succeeded.
     */
    virtual bool CompareAndCreateAIEditUndoEntries() = 0;

    /**
     * Autoplace fields for symbols that were modified during an AI edit session.
     * Only affects fields with CanAutoplace() == true (do_not_autoplace: no).
     * Default implementation does nothing - derived classes can override.
     * @param aModifiedUUIDs Set of UUID strings for symbols that were added or modified.
     */
    virtual void AutoplaceModifiedSymbols( const std::set<std::string>& aModifiedUUIDs )
    {
        (void) aModifiedUUIDs;  // Default: do nothing
    }

    /**
     * Annotate all symbols in the schematic after trace edits.
     * This runs annotation with default settings to assign reference designators.
     * Default implementation does nothing - derived classes can override.
     */
    virtual void AnnotateAllSymbols() {}

    /**
     * Save the document to disk.
     * Called after annotation to persist changes before marking as saved.
     * Default implementation does nothing - derived classes can override.
     * @return True if save was successful, false otherwise.
     */
    virtual bool SaveDocument() { return true; }

    /**
     * Mark the document as saved (not modified) after AI edits complete.
     * This prevents the "Save changes?" dialog when closing since the
     * file on disk matches the in-memory state after AI edits + reload.
     */
    virtual void MarkDocumentAsSaved() {}

    /**
     * Handle file edit events with optional incremental diff support.
     * Default implementation does full reload; derived classes can override
     * to support incremental updates.
     * @param aEvent The file edit event with diff info.
     * @param aTabIndex The tab index that triggered the file edit.
     */
    virtual void HandleFileEditEvent( const AI_BACKEND_EVENT& aEvent, int aTabIndex );

    /**
     * Generate a snapshot of the current view.
     * @param aOutputPath Path where the snapshot file should be saved.
     * @return True if successful, false otherwise.
     */
    virtual bool GenerateSnapshot( const wxString& aOutputPath ) = 0;

    /**
     * Get the current file name.
     * @return The current file path.
     */
    virtual wxString GetCurrentFileName() const = 0;

    /**
     * Ensure the schematic/PCB is saved to disk before AI can read it.
     * For unsaved schematics, this will auto-save to a temp location.
     * @return The full path to the saved file, or empty string on failure.
     */
    virtual wxString EnsureFileSavedForAI() { return GetCurrentFileName(); }

    /**
     * Get the application type (e.g., "eeschema" or "pcbnew").
     * @return The application type string.
     */
    virtual wxString GetAppType() const = 0;

    /**
     * Convert a file path to the corresponding trace file path.
     * @param aFilePath The original file path (e.g., .kicad_sch or .kicad_pcb).
     * @return The corresponding trace file path (e.g., .trace_sch or .trace_pcb).
     */
    virtual wxString ConvertToTraceFile( const wxString& aFilePath ) const = 0;

    /**
     * Get the frame pointer.
     */
    EDA_DRAW_FRAME* GetFrame() const { return m_frame; }

    // Protected accessors for derived classes (e.g., version management)
    wxString GetSessionId() const { return m_sessionId; }
    void     SetSessionId( const wxString& aId ) { m_sessionId = aId; }
    wxString GetConversationId() const { return m_conversationId; }
    void     SetConversationId( const wxString& aId ) { m_conversationId = aId; }

    /**
     * Get the AI backend client for direct use (e.g., version management).
     */
    AI_BACKEND_CLIENT* GetBackendClient() const 
    { 
        // Return current tab's backend client (tabs have their own clients now)
        if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
            return m_tabs[m_currentTabIndex].backendClient.get();
        return nullptr;
    }

    /**
     * Request the list of file versions from the backend.
     * Results will be emitted as a versions_list event.
     * Default implementation is empty - derived classes can override.
     */
    virtual void RequestVersionList() {}

    /**
     * Check if another tab is streaming and has modified the current file.
     * Used to guard RestoreVersion against overwriting in-flight edits.
     * @return true if restore should be blocked.
     */
    bool isOtherTabStreamingToCurrentFile() const
    {
        wxString currentFile = GetCurrentFileName();
        if( currentFile.IsEmpty() )
            return false;

        wxString traceFile = currentFile;
        if( traceFile.EndsWith( wxT( ".kicad_sch" ) ) )
            traceFile = traceFile.BeforeLast( wxT( '.' ) ) + wxT( ".trace_sch" );

        for( size_t t = 0; t < m_tabs.size(); t++ )
        {
            if( static_cast<int>( t ) == m_currentTabIndex )
                continue;
            if( !m_tabs[t].isStreaming.load() )
                continue;
            if( m_tabs[t].modifiedFiles.count( traceFile ) > 0
                || m_tabs[t].modifiedFiles.count( currentFile ) > 0 )
            {
                return true;
            }
        }
        return false;
    }

    /**
     * Request to restore a specific file version.
     * @param versionId The version ID to restore.
     * @param aOnComplete Callback invoked on UI thread when restore finishes (bool success).
     *        If nullptr, no callback is made.
     * Default implementation calls aOnComplete(false) immediately - derived classes override.
     */
    virtual void RestoreVersion( const wxString& versionId,
                                 std::function<void( bool )> aOnComplete = nullptr )
    {
        if( !versionId.IsEmpty() )
        {
            wxLogWarning( wxT( "RestoreVersion: no-op in base class (pcbnew does not support version restore)" ) );
            if( m_chatWebview )
                m_chatWebview->ShowStatus( wxT( "Version restore not supported for this editor type." ) );
        }

        if( aOnComplete )
            aOnComplete( false );
    }

    /**
     * Save the current file state as a version in the database.
     * Called automatically after AI edits are completed.
     * @param aDescription Description of the changes (e.g., from AI edit summary)
     * Default implementation is empty - derived classes can override.
     */
    virtual void saveVersionToDatabase( const wxString& aDescription ) { (void) aDescription; }

    /**
     * Save current schematic version to the database for a specific tab.
     * @param aDescription Description of the changes
     * @param aTabIndex The tab index this version belongs to
     */
    virtual void saveVersionToDatabase( const wxString& aDescription, int aTabIndex )
    {
        (void) aTabIndex;
        saveVersionToDatabase( aDescription );
    }

    /**
     * Get the last saved version ID from database-persisted undo.
     * @return The version ID string, or empty if no version saved.
     */
    wxString GetLastSavedVersionId() const
    {
        if( m_currentTabIndex >= 0 && m_currentTabIndex < static_cast<int>( m_tabs.size() ) )
            return m_tabs[m_currentTabIndex].lastSavedVersionId;
        return wxEmptyString;
    }

    wxString GetLastSavedVersionId( int aTabIndex ) const
    {
        if( aTabIndex >= 0 && aTabIndex < static_cast<int>( m_tabs.size() ) )
            return m_tabs[aTabIndex].lastSavedVersionId;
        return wxEmptyString;
    }

protected:
    /**
     * Set DRC callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable run_drc tool.
     * @param aCallback Callback that runs DRC and returns violations as JSON.
     */
    void SetDrcCallback( std::function<nlohmann::json()> aCallback );

    /**
     * Set ERC callback for the AI tool executor.
     * Called by derived Eeschema AI_CHAT_PANEL to enable run_erc tool.
     * @param aCallback Callback that runs ERC and returns violations as JSON.
     */
    void SetErcCallback( std::function<nlohmann::json()> aCallback );

    /**
     * Set Annotate callback for the AI tool executor.
     * Called by derived Eeschema AI_CHAT_PANEL to enable run_annotate tool.
     * @param aCallback Callback that runs annotation and returns result messages as JSON.
     */
    void SetAnnotateCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Gerber callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable generate_gerbers tool.
     * @param aCallback Callback that generates Gerber files and returns list of generated files as JSON.
     */
    void SetGerberCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Drill callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable generate_drill_files tool.
     * @param aCallback Callback that generates drill files and returns list of generated files as JSON.
     */
    void SetDrillCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Autoroute callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable autoroute tool.
     * @param aCallback Callback that runs autorouting and returns result as JSON.
     *                  Input: { "params": { ... routing parameters ... } }
     *                  Output: { "success": bool, "message": string, "progress_log": [...] }
     */
    void SetAutorouteCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Hierarchy callback for the AI tool executor.
     * Called by derived Eeschema AI_CHAT_PANEL to enable get_hierarchy tool.
     * @param aCallback Callback that returns hierarchy info as JSON.
     */
    void SetHierarchyCallback( std::function<nlohmann::json()> aCallback );

    /**
     * Set Switch Sheet callback for the AI tool executor.
     * Called by derived Eeschema AI_CHAT_PANEL to enable switch_sheet tool.
     * @param aCallback Callback that switches sheets and returns result as JSON.
     */
    void SetSwitchSheetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Switch Layer callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable switch_layer tool.
     * @param aCallback Callback that switches layers and returns result as JSON.
     */
    void SetSwitchLayerCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Layers callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable get_layers tool.
     * @param aCallback Callback that returns layer info as JSON.
     */
    void SetLayersCallback( std::function<nlohmann::json()> aCallback );

    /**
     * Set Layer Preset callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable apply_layer_preset tool.
     * @param aCallback Callback that applies preset and returns result as JSON.
     */
    void SetLayerPresetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Layer Visibility callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable set_layer_visibility tool.
     * @param aCallback Callback that sets visibility and returns result as JSON.
     */
    void SetLayerVisibilityCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );

    /**
     * Set Fetch Dimensions callback for the AI tool executor.
     * Called by derived PCBnew AI_CHAT_PANEL to enable fetch_dimensions tool.
     * @param aCallback Callback that fetches footprint dimensions and returns result as JSON.
     */
    void SetFetchDimensionsCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback );
    
    /*
     * Set snapshot callback for the AI tool executor.
     * Called by derived AI_CHAT_PANEL classes to enable take_snapshot tool.
     * @param aCallback Callback that generates snapshot and returns base64-encoded SVG content.
     */
    void SetSnapshotCallback( std::function<std::string()> aCallback );

    /**
     * Handle a single event from the direct backend client.
     * @param aEvent The backend event.
     * @param aTabIndex The tab index this event belongs to (for parallel streaming safety).
     */
    void handleBackendEventDirect( const AI_BACKEND_EVENT& aEvent, int aTabIndex );

    /**
     * Helper for thread-safe UI updates.
     * Checks if panel is alive before calling CallAfter.
     */
    template <typename Func>
    void safeCallAfter( Func&& func );

    /**
     * Update UI elements based on authentication state.
     */
    void updateAuthUI();

    /**
     * Show quota/plan limit banner above input box.
     * @param aMessage The message to display
     * @param aShowUpgrade True to show upgrade button, false for info-only banner
     */
    void showQuotaBanner( const wxString& aMessage, bool aShowUpgrade = false );

    /**
     * Hide the quota banner.
     */
    void hideQuotaBanner();

    /**
     * Fetch and display quota info (trial time, daily usage).
     * Called when panel loads and user is authenticated.
     * @param aIsStartup True if called on startup (shows banner once, hides after first message)
     */
    void fetchAndShowQuotaInfo( bool aIsStartup = false );

    /**
     * Handle auth state changes from AUTH_MANAGER (e.g., user signed in via browser).
     * @param aEvent The command event from AUTH_MANAGER.
     */
    void onAuthStateChanged( wxCommandEvent& aEvent );

    /**
     * Handle system theme change.
     * @param aEvent The system colour changed event.
     */
    void onThemeChanged( wxSysColourChangedEvent& aEvent );

protected:
    void buildUI();
    void onSendMessage( wxCommandEvent& aEvent );
    void sendMessageFromWebview( const wxString& aMessage,
                                  const std::vector<ChatAttachment>& aAttachments = {} );
    void onStopRequest( wxCommandEvent& aEvent );
    void onStopRequestForTab( int aTabIndex );
    
    /**
     * Check if any tab is currently streaming.
     * Used for operations that need to know if streaming is in progress
     * without caring which specific tab.
     * @return True if at least one tab is streaming.
     */
    bool isAnyTabStreaming() const;
    
    /**
     * Try to claim ownership of a file for a tab.
     * A tab must own a file before it can modify it.
     * @param aFilePath The file path to claim.
     * @param aTabIndex The tab index claiming ownership.
     * @return True if ownership was granted, false if another tab owns it.
     */
    bool claimFileOwnership( const wxString& aFilePath, int aTabIndex );
    
    /**
     * Release ownership of all files owned by a tab.
     * Called when a tab's streaming completes.
     * @param aTabIndex The tab index releasing ownership.
     */
    void releaseFileOwnership( int aTabIndex );
    
    /**
     * Check which tab owns a file.
     * @param aFilePath The file path to check.
     * @return The tab index that owns the file, or -1 if no owner.
     */
    int getFileOwner( const wxString& aFilePath );
    
    /**
     * Mark a file as modified by a specific tab.
     * Called when a tool modifies a file during streaming.
     * @param aFilePath The file path that was modified.
     * @param aTabIndex The tab that modified it.
     */
    void markFileModifiedByTab( const wxString& aFilePath, int aTabIndex );
    
    /**
     * Check if any tab has requested stop.
     * @return True if at least one tab has stopRequested set.
     */
    bool isAnyTabStopRequested() const;

    /**
     * Send a message to the AI backend asynchronously.
     * Uses AI_BACKEND_CLIENT for direct streaming communication.
     * @param aMessage The user's message to send.
     */
    void sendToBackendAsync( const wxString& aMessage,
                             const std::vector<ChatAttachment>& aAttachments = {} );

    /**
     * Handle the response from the AI backend.
     * Called via CallAfter() from the background thread.
     * @param aResponse The AI's response text.
     * @param aSuccess True if the request succeeded, false on error.
     * @param aTabIndex The tab index this response belongs to (for parallel streaming safety).
     * @param aFileModified True if the file was modified.
     */
    void onBackendResponse( const wxString& aResponse, bool aSuccess, int aTabIndex, bool aFileModified = false );

    /**
     * Handle a streaming text delta event.
     * Called via CallAfter() to append text incrementally.
     * @param aText The text chunk to append.
     * @param aIsFirst True if this is the first chunk (removes "Thinking...").
     * @param aTabIndex The tab index this text belongs to (for parallel streaming safety).
     */
    void onStreamingText( const wxString& aText, bool aIsFirst, int aTabIndex );

    /**
     * Buffer a text delta for batched updates.
     * Accumulates text deltas and flushes when threshold is reached.
     * @param aText The text chunk to buffer.
     * @param aTabIndex The tab index to buffer text for (for parallel streaming safety).
     */
    void bufferStreamingText( const wxString& aText, int aTabIndex );

    /**
     * Flush the streaming buffer to the UI.
     * Called by timer, when threshold reached, or before non-text events.
     */
    void flushStreamingBuffer();
    void flushStreamingBuffer( int aTabIndex );  ///< Flush specific tab's buffer

    /**
     * Handle timer event for flushing streaming buffer.
     * @param aEvent The timer event.
     */
    void onStreamingFlushTimer( wxTimerEvent& aEvent );

    /**
     * Handle reload debounce timer expiration.
     * Performs the actual file reload after debounce period.
     * @param aEvent The timer event.
     */
    void onReloadDebounceTimer( wxTimerEvent& aEvent );

    /**
     * Handles periodic batch updates during streaming.
     * Flushes pending conversions and reloads the file to show incremental changes.
     * @param aEvent The timer event.
     */
    void onStreamingBatchTimer( wxTimerEvent& aEvent );

    /**
     * Handle idle status timer expiration.
     * Shows "Working..." status when no backend events received for 2 seconds.
     * @param aEvent The timer event.
     */
    void onIdleStatusTimer( wxTimerEvent& aEvent );

    /**
     * Reset (restart) the idle status timer for a tab.
     * Called when any backend event is received to restart the 2-second countdown.
     * @param aTabIndex The tab index to reset the timer for.
     */
    void resetIdleStatusTimer( int aTabIndex );

    /**
     * Stop the idle status timer for a tab.
     * Called when streaming ends or is stopped.
     * @param aTabIndex The tab index to stop the timer for.
     */
    void stopIdleStatusTimer( int aTabIndex );

    /**
     * Handle a streaming status event (tool calls, etc.).
     * @param aStatus The status message to display.
     * @param aTabIndex The tab index this status belongs to (for parallel streaming safety).
     */
    void onStreamingStatus( const wxString& aStatus, int aTabIndex );

    /**
     * Update button state between Send and Stop modes.
     * @param aIsStopMode True to show Stop button, false to show Send button.
     */
    void updateButtonState( bool aIsStopMode );

    EDA_DRAW_FRAME*            m_frame;
    CHAT_WEBVIEW_PANEL*        m_chatWebview;       ///< WebView-based chat UI (tabs + messages + input)
    AI_MODE                    m_currentMode;        ///< Current chat mode (ask/plan/agent) - tracked for backend requests
    wxString                   m_backendUrl;        ///< URL of the remote AI backend service
    std::atomic<bool>          m_requestInProgress; ///< Prevents duplicate requests
    wxString                   m_sessionId;         ///< Session ID for plan mode continuity
    wxString                   m_conversationId;    ///< Conversation ID from backend for linking DB entries
    wxString                   m_cachedProjectPath; ///< Cached project path for safe access in destructor

    // Callback storage for setting on newly created tab tool executors
    std::function<nlohmann::json()> m_drcCallback;
    std::function<nlohmann::json()> m_ercCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_annotateCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_gerberCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_drillCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_autorouteCallback;
    std::function<nlohmann::json()> m_hierarchyCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_switchSheetCallback;
    // Layer callbacks (pcbnew)
    std::function<nlohmann::json( const nlohmann::json& )> m_switchLayerCallback;
    std::function<nlohmann::json()> m_layersCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_layerPresetCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_layerVisibilityCallback;
    // Fetch dimensions callback (pcbnew)
    std::function<nlohmann::json( const nlohmann::json& )> m_fetchDimensionsCallback;
    std::function<std::string()> m_snapshotCallback;
    std::function<std::future<bool>( const std::string& )> m_confirmationCallback;

    // Streaming buffer for batching text deltas (per-tab buffers stored in TAB_DATA)
    wxTimer* m_streamingFlushTimer;       ///< Timer to trigger buffer flush (shared, checks per-tab buffers)
    static constexpr int STREAMING_FLUSH_DELTA_COUNT = 10;   ///< Flush after N deltas
    static constexpr int STREAMING_FLUSH_INTERVAL_MS = 50;  ///< Or flush every 50ms

    // Idle status timer - shows "Working..." if no backend events for 2 seconds during streaming
    static constexpr int IDLE_STATUS_TIMEOUT_MS = 2000;  ///< Show "Working..." after 2 seconds of no events

    // AI edit undo/redo state tracking
    bool m_aiEditInProgress;    ///< True if an AI edit sequence is in progress
    bool m_aiEditStateCaptured; ///< True if we've captured state for this edit sequence

    // File reload thread safety and debouncing
    std::mutex          m_reloadMutex;           ///< Protects concurrent reload operations
    wxTimer*            m_reloadDebounceTimer;   ///< Timer for batching rapid reload requests
    std::atomic<bool>   m_reloadPending;         ///< Flag indicating reload is queued
    std::atomic<bool>   m_reloadInProgress;      ///< Flag indicating reload is running
    wxString            m_pendingReloadPath;     ///< Path for queued reload operation
    
    // Multi-tab file ownership tracking (prevents concurrent editing conflicts)
    std::map<wxString, int> m_fileOwnership;     ///< Maps file path -> owning tab index (-1 = no owner)
    std::mutex              m_fileOwnershipMutex; ///< Protects m_fileOwnership map
    wxTimer*            m_streamingBatchTimer;   ///< Timer for periodic batch updates during streaming
    std::atomic<bool>   m_batchUpdatePending;    ///< Flag indicating batch update is queued
    
    // Note: Concurrent file edits are handled by AI_TOOL_EXECUTOR's optimistic concurrency
    // using file hashes. If Tab A edits while Tab B is editing, Tab B's write will fail
    // with a conflict error, and the AI will re-read the file with fresh content.

    // Race condition safety
    std::shared_ptr<std::atomic<bool>> m_panelAlive; ///< Shared flag to prevent accessing destroyed panel

    // Destruction flag to prevent CallAfter() callbacks from executing after destruction
    std::atomic<bool> m_isDestroying; ///< True when panel is being destroyed

    // Streaming response tracking (per-tab pendingStreamingResponse handles this now)
    int      m_streamingTabIndex;        ///< Tab index that started the current stream (-1 if none)

    // Multi-conversation tab support
    std::vector<TAB_DATA>   m_tabs;         ///< Data for each tab
    int                     m_currentTabIndex; ///< Index of currently active tab

    // Plan Mode state tracking
    wxString       m_lastSavedPlanContent; ///< Track last saved plan to prevent duplicates
    wxString       m_lastSavedPlanPath;    ///< Path to last saved plan file
    
    // Plan Mode Frontend-Backend Sync (NEW for overhaul)
    AI_MODE        m_expectedMode;        ///< Expected mode after plan approval (for verification)
    bool           m_awaitingModeConfirmation; ///< True while waiting for backend MODE_TRANSITION
    bool           m_planModeExecuting;   ///< True when plan mode is executing (dropdown should be read-only)

    /**
     * Handle tab selection change.
     */
    void onTabSelected( wxCommandEvent& aEvent );

    /**
     * Handle new tab button click.
     */
    void onNewTab( wxCommandEvent& aEvent );

    /**
     * Handle tab close button click.
     */
    void onTabClose( wxCommandEvent& aEvent );

    /**
     * Handle history dropdown selection.
     */
    void onHistorySelect( wxCommandEvent& aEvent );

    /**
     * Switch to a tab by updating the webview and hiding/showing appropriate state.
     * Also loads messages if the tab hasn't been loaded yet.
     */
    void switchToTab( int aTabIndex );

    /**
     * Load messages for a specific tab into its per-tab content panel.
     * @param aTabIndex The index of the tab to load messages for.
     */
    void loadMessagesForTab( int aTabIndex );

    /**
     * Reload messages from SQLite into the webview, correctly handling
     * plan_document type messages. Used by edit/undo/regenerate callbacks.
     */
    void reloadMessagesToWebview( const wxString& aConversationId );

    /**
     * Find a tab by conversation ID (thread-safe lookup).
     * @param aConversationId The conversation ID to search for.
     * @return Pointer to the tab data if found, nullptr otherwise.
     */
    TAB_DATA* findTabByConversationId( const wxString& aConversationId );

    /**
     * Resolve a tabId (conversation ID) to a tab index. Falls back to m_currentTabIndex.
     */
    int resolveTabIndex( const wxString& aTabId );

    /**
     * Create a new conversation tab.
     * @return The index of the new tab, or -1 on failure.
     */
    int createNewTab();

    /**
     * Load conversation from local database into a tab.
     * @param aConversationId The conversation ID to load.
     * @return True if loaded successfully.
     */
    bool loadConversationToTab( const wxString& aConversationId );

    /**
     * Load conversation metadata asynchronously and then load into tab.
     * @param aConversationId The conversation ID to load.
     */
    void loadConversationToTabAsync( const wxString& aConversationId );

    /**
     * Save the current open tabs state to the database.
     * Called when tabs change (create, close, switch) and on destruction.
     */
    void saveOpenTabs();

    /**
     * Load persisted open tabs from the database.
     * Called during buildUI() to restore tabs from the previous session.
     * @return True if tabs were loaded, false if no persisted tabs (create fresh).
     */
    bool loadPersistedTabs();
    
    /**
     * Configure a tool executor with all registered callbacks.
     * Called when creating new tabs to ensure they have all the necessary callbacks.
     * @param aToolExecutor The tool executor to configure.
     */
    void configureToolExecutor( AI_TOOL_EXECUTOR* aToolExecutor );

    // Plan Mode Phase Handling (NEW for two-phase workflow)
    
    /**
     * Handle plan_document event from backend.
     * Displays the plan and shows approval buttons.
     * @param aEvent The JSON event containing the plan document.
     * @param aTabIndex The tab index to update.
     */
    void handlePlanDocumentEvent( const nlohmann::json& aEvent, int aTabIndex );
    
    /**
     * Handle phase_update event from backend.
     * Updates the phase indicator UI.
     * @param aEvent The JSON event containing phase info.
     * @param aTabIndex The tab index to update.
     */
    void handlePhaseUpdateEvent( const nlohmann::json& aEvent, int aTabIndex );
    
    /**
     * Send plan approval response to backend.
     * @param aApprovalType The approval type: "approve", "cancel", etc.
     * @param aTabIndex The tab index for the approval.
     * @deprecated Use sendPlanAction instead for structured actions.
     */
    void sendPlanApproval( const std::string& aApprovalType, int aTabIndex );
    
    /**
     * Send a structured plan action to the backend.
     * 
     * This calls the /api/v2/plan/action endpoint directly, bypassing
     * LLM classification. Use this for button clicks.
     * 
     * @param aAction Action type: "approve", "cancel", "adjust_plan", "more_research".
     * @param aFeedback User feedback for adjust_plan/more_research (optional).
     * @param aTabIndex The tab index for the action.
     */
    void sendPlanAction( const std::string& aAction,
                         const std::string& aFeedback,
                         int aTabIndex );
    
    /**
     * Auto-save plan document to project folder.
     * @param aPlanDocument The markdown plan document to save.
     */
    wxString savePlanToProjectFolder( const wxString& aPlanDocument );
    
    // Background thread tracking for clean shutdown
    std::unique_ptr<std::thread> m_syncThread;            ///< Thread for DB initialization
    std::unique_ptr<std::thread> m_conversationLoadThread; ///< Thread for conversation metadata loading
};

// Template implementation for safeCallAfter
// Uses this->CallAfter (panel's queue) instead of wxTheApp->CallAfter for reliable
// delivery of streaming events (TEXT_DELTA, etc.) in all build/configurations.
template <typename Func>
void AI_CHAT_PANEL_BASE::safeCallAfter( Func&& func )
{
    if( m_panelAlive && m_panelAlive->load() )
    {
        auto panelAlive = m_panelAlive; // Capture shared_ptr
        CallAfter(
                [panelAlive, f = std::forward<Func>( func )]() mutable
                {
                    if( panelAlive->load() )
                    {
                        f();
                    }
                } );
    }
}

#endif // AI_CHAT_PANEL_BASE_H
