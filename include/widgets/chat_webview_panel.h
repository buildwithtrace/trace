/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#ifndef CHAT_WEBVIEW_PANEL_H
#define CHAT_WEBVIEW_PANEL_H

#include <widgets/webview_panel.h>
#include <functional>
#include <atomic>
#include <vector>

/**
 * Structure representing a file attachment for multimodal chat input.
 */
struct ChatAttachment
{
    wxString name;         ///< Original filename
    wxString mimeType;     ///< MIME type (e.g., "image/png", "application/pdf")
    wxString base64Data;   ///< Base64-encoded file content
    size_t   size;         ///< File size in bytes
};

/**
 * A webview-based chat panel that renders the entire chat UI using React.
 * 
 * This panel replaces the native wxWidgets-based chat UI with a modern
 * React-based UI rendered in a webview. It provides:
 * - Tab management (multiple conversations)
 * - Smooth animations and transitions
 * - Better markdown rendering
 * - Modern UI/UX patterns
 * - Easier maintenance and styling
 * 
 * Communication with the React app is done via:
 * - C++ to React: RunScriptAsync() calls to window.kichat.* methods
 * - React to C++: Message handlers registered via AddMessageHandler()
 */
class CHAT_WEBVIEW_PANEL : public WEBVIEW_PANEL
{
public:
    /**
     * Callback types for events from the React UI.
     */
    using SendMessageCallback = std::function<void( const wxString& content, 
                                                      const std::vector<ChatAttachment>& attachments )>;
    using StopRequestCallback = std::function<void()>;
    using ModeChangedCallback = std::function<void( const wxString& mode )>;
    using TabSelectedCallback = std::function<void( const wxString& tabId )>;
    using TabCloseCallback = std::function<void( const wxString& tabId )>;
    using NewTabCallback = std::function<void()>;
    using HistoryClickCallback = std::function<void()>;
    using HistorySelectCallback = std::function<void( const wxString& conversationId )>;
    
    // Plan mode callbacks
    using PlanApproveCallback = std::function<void()>;
    using PlanMoreResearchCallback = std::function<void( const wxString& feedback )>;
    using PlanAdjustCallback = std::function<void( const wxString& feedback )>;
    using PlanCancelCallback = std::function<void()>;

    // Auth/payment callbacks
    using SignInClickCallback = std::function<void()>;
    using UpgradeClickCallback = std::function<void()>;

    /**
     * Constructor.
     * @param aParent Parent window.
     * @param aId Window ID.
     */
    CHAT_WEBVIEW_PANEL( wxWindow* aParent, wxWindowID aId = wxID_ANY );

    ~CHAT_WEBVIEW_PANEL() override;

    /**
     * Initialize the webview with the React chat UI.
     * Must be called after construction.
     */
    void InitializeChatUI();

    /**
     * Check if the React bridge is ready.
     */
    bool IsBridgeReady() const { return m_bridgeReady.load(); }

    // ========================================================================
    // Tab Management (C++ -> React)
    // ========================================================================

    /**
     * Add a new tab to the tab bar.
     * @param aId Unique tab ID.
     * @param aTitle Tab title.
     */
    void AddTab( const wxString& aId, const wxString& aTitle );

    /**
     * Remove a tab from the tab bar.
     * @param aId Tab ID to remove.
     */
    void RemoveTab( const wxString& aId );

    /**
     * Select a tab (make it active).
     * @param aId Tab ID to select.
     */
    void SelectTab( const wxString& aId );

    /**
     * Update a tab's title.
     * @param aId Tab ID.
     * @param aTitle New title.
     */
    void SetTabTitle( const wxString& aId, const wxString& aTitle );

    // ========================================================================
    // Message Management (C++ -> React)
    // ========================================================================

    /**
     * Add a new message to the chat.
     * @param aId Unique message ID.
     * @param aRole Message role: "user", "assistant", or "system".
     * @param aContent Message content.
     * @param aAttachments Optional file attachments for the message.
     */
    void AddMessage( const wxString& aId, const wxString& aRole, const wxString& aContent,
                     const std::vector<ChatAttachment>& aAttachments = {} );

    /**
     * Append text to an existing message (for streaming).
     * @param aId Message ID to append to.
     * @param aText Text to append.
     */
    void AppendToMessage( const wxString& aId, const wxString& aText );

    /**
     * Finalize a streaming message (removes cursor, enables truncation).
     * @param aId Message ID to finalize.
     */
    void FinalizeMessage( const wxString& aId );

    /**
     * Clear all messages from the chat.
     */
    void ClearMessages();

    // ========================================================================
    // Indicators (C++ -> React)
    // ========================================================================

    /**
     * Show the typing indicator (animated dots).
     */
    void ShowTypingIndicator();

    /**
     * Hide the typing indicator.
     */
    void HideTypingIndicator();

    /**
     * Show a status message (e.g., "Executing tool...").
     * @param aStatus Status text to display.
     */
    void ShowStatus( const wxString& aStatus );

    /**
     * Hide the status message.
     */
    void HideStatus();

    // ========================================================================
    // UI State (C++ -> React)
    // ========================================================================

    /**
     * Set the theme (light/dark).
     * @param aIsDark True for dark theme, false for light.
     */
    void SetTheme( bool aIsDark );

    /**
     * Set the current chat mode.
     * @param aMode Mode string: "ask", "plan", or "agent".
     */
    void SetMode( const wxString& aMode );

    /**
     * Enable or disable the input area.
     * @param aEnabled True to enable input.
     */
    void SetInputEnabled( bool aEnabled );

    /**
     * Set the input placeholder text.
     * @param aPlaceholder Placeholder text.
     */
    void SetInputPlaceholder( const wxString& aPlaceholder );

    /**
     * Set the streaming state (controls send/stop button).
     * @param aIsStreaming True when AI is generating a response.
     */
    void SetStreaming( bool aIsStreaming );

    /**
     * Show the history menu with a list of conversations.
     * @param aConversations JSON array of conversation objects.
     */
    void ShowHistoryMenu( const wxString& aConversationsJson );

    // ========================================================================
    // Plan Mode (C++ -> React)
    // ========================================================================

    /**
     * Show the phase indicator with a label and color.
     * @param aLabel Phase label text (e.g., "Research Phase").
     * @param aColor CSS color value (e.g., "#ffc107" for amber).
     */
    void ShowPhaseIndicator( const wxString& aLabel, const wxString& aColor );

    /**
     * Hide the phase indicator.
     */
    void HidePhaseIndicator();

    /**
     * Show the plan approval buttons (Approve, More Research, Adjust, Cancel).
     */
    void ShowPlanApproval();

    /**
     * Hide the plan approval buttons.
     */
    void HidePlanApproval();

    /**
     * Set a plan document on a message for rendering as a collapsible widget.
     * @param aMessageId Message ID to attach the plan to.
     * @param aMarkdown Plan markdown content.
     * @param aRenderMode "full" to show approval buttons, "summary" for display only.
     */
    void SetPlanDocument( const wxString& aMessageId, const wxString& aMarkdown, 
                          const wxString& aRenderMode );

    // ========================================================================
    // Auth/Payment (C++ -> React)
    // ========================================================================

    /**
     * Set the authentication state in the React UI.
     * When not authenticated, shows the sign-in header.
     * When authenticated, shows the tab bar and chat UI.
     * @param aIsAuthenticated True if user is signed in.
     */
    void SetAuthState( bool aIsAuthenticated );

    /**
     * Show a quota/plan banner with an optional upgrade button.
     * @param aMessage Banner message text.
     * @param aShowUpgrade True to show the "Upgrade" button.
     */
    void ShowBanner( const wxString& aMessage, bool aShowUpgrade );

    /**
     * Hide the quota/plan banner.
     */
    void HideBanner();

    // ========================================================================
    // Event Callbacks (React -> C++)
    // ========================================================================

    /**
     * Set callback for when user sends a message.
     */
    void SetSendMessageCallback( SendMessageCallback aCallback )
    {
        m_sendMessageCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user requests to stop generation.
     */
    void SetStopRequestCallback( StopRequestCallback aCallback )
    {
        m_stopRequestCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user changes the chat mode.
     */
    void SetModeChangedCallback( ModeChangedCallback aCallback )
    {
        m_modeChangedCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user selects a tab.
     */
    void SetTabSelectedCallback( TabSelectedCallback aCallback )
    {
        m_tabSelectedCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user closes a tab.
     */
    void SetTabCloseCallback( TabCloseCallback aCallback )
    {
        m_tabCloseCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks new tab button.
     */
    void SetNewTabCallback( NewTabCallback aCallback )
    {
        m_newTabCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks history button.
     */
    void SetHistoryClickCallback( HistoryClickCallback aCallback )
    {
        m_historyClickCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user selects a conversation from history.
     */
    void SetHistorySelectCallback( HistorySelectCallback aCallback )
    {
        m_historySelectCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks Approve on a plan.
     */
    void SetPlanApproveCallback( PlanApproveCallback aCallback )
    {
        m_planApproveCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks More Research on a plan.
     */
    void SetPlanMoreResearchCallback( PlanMoreResearchCallback aCallback )
    {
        m_planMoreResearchCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks Adjust Plan.
     */
    void SetPlanAdjustCallback( PlanAdjustCallback aCallback )
    {
        m_planAdjustCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks Cancel on a plan.
     */
    void SetPlanCancelCallback( PlanCancelCallback aCallback )
    {
        m_planCancelCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks Sign In.
     */
    void SetSignInClickCallback( SignInClickCallback aCallback )
    {
        m_signInClickCallback = std::move( aCallback );
    }

    /**
     * Set callback for when user clicks Upgrade.
     */
    void SetUpgradeClickCallback( UpgradeClickCallback aCallback )
    {
        m_upgradeClickCallback = std::move( aCallback );
    }

private:
    /**
     * Handle messages from the React UI.
     * @param aMessage JSON message from React.
     */
    void onBridgeMessage( const wxString& aMessage );

    /**
     * Escape a string for safe inclusion in JavaScript.
     * @param aStr String to escape.
     * @return Escaped string suitable for JS string literal.
     */
    static wxString EscapeForJS( const wxString& aStr );

    /**
     * Get the embedded HTML content for the chat UI.
     * This is the built React app bundled as a single HTML file.
     */
    static wxString GetChatUIHtml();

    std::atomic<bool>        m_bridgeReady;
    std::vector<wxString>    m_pendingCommands;  // Commands queued before bridge ready
    SendMessageCallback      m_sendMessageCallback;
    StopRequestCallback      m_stopRequestCallback;
    ModeChangedCallback      m_modeChangedCallback;
    TabSelectedCallback      m_tabSelectedCallback;
    TabCloseCallback         m_tabCloseCallback;
    NewTabCallback           m_newTabCallback;
    HistoryClickCallback     m_historyClickCallback;
    HistorySelectCallback    m_historySelectCallback;
    
    // Plan mode callbacks
    PlanApproveCallback      m_planApproveCallback;
    PlanMoreResearchCallback m_planMoreResearchCallback;
    PlanAdjustCallback       m_planAdjustCallback;
    PlanCancelCallback       m_planCancelCallback;
    
    // Auth/payment callbacks
    SignInClickCallback      m_signInClickCallback;
    UpgradeClickCallback     m_upgradeClickCallback;
};

#endif // CHAT_WEBVIEW_PANEL_H
