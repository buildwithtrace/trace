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

#ifndef AUTH_MANAGER_H
#define AUTH_MANAGER_H

#include <wx/string.h>
#include <wx/event.h>
#include <kicommon.h>
#include <memory>
#include <functional>
#include <atomic>

// Forward declarations
class wxSocketServer;
class wxSocketEvent;

/**
 * User information from authentication
 */
struct KICOMMON_API AUTH_USER
{
    wxString id;
    wxString email;
    wxString fullName;
    wxString avatarUrl;
    
    bool IsValid() const { return !id.IsEmpty() && !email.IsEmpty(); }
};

/**
 * Authentication state
 */
enum class AUTH_STATE
{
    SIGNED_OUT,       ///< No user is logged in
    SIGNING_IN,       ///< Login flow in progress
    SIGNED_IN,        ///< User is authenticated
    AUTH_ERROR        ///< Authentication error occurred
};

/**
 * Custom event for authentication state changes
 */
KICOMMON_API wxDECLARE_EVENT( EVT_AUTH_STATE_CHANGED, wxCommandEvent );
KICOMMON_API wxDECLARE_EVENT( EVT_AUTH_TOKEN_RECEIVED, wxCommandEvent );

/**
 * Manages authentication state for the application.
 * 
 * Handles:
 * - Starting a local HTTP callback server for OAuth flow
 * - Opening browser for login
 * - Receiving and storing authentication tokens
 * - Providing tokens for API requests
 * - Secure token storage using platform keychain
 */
class KICOMMON_API AUTH_MANAGER : public wxEvtHandler
{
public:
    /**
     * Get the singleton instance of the auth manager.
     */
    static AUTH_MANAGER& Instance();
    
    /**
     * Destroy the singleton instance (call on app shutdown).
     */
    static void Destroy();
    
    /**
     * Get current authentication state.
     */
    AUTH_STATE GetState() const { return m_state; }
    
    /**
     * Check if user is currently authenticated.
     */
    bool IsAuthenticated() const { return m_state == AUTH_STATE::SIGNED_IN; }
    
    /**
     * Get current user information.
     * @return User info, or invalid AUTH_USER if not authenticated.
     */
    AUTH_USER GetCurrentUser() const { return m_currentUser; }
    
    /**
     * Get the current authentication token (access token).
     * @return JWT token string, or empty if not authenticated.
     */
    wxString GetAuthToken() const { return m_token; }
    
    /**
     * Get the refresh token for transparent token renewal.
     * @return Refresh token string, or empty if not available.
     */
    wxString GetRefreshToken() const { return m_refreshToken; }
    
    /**
     * Get the current authentication token.
     * @return JWT token string, or empty if not authenticated.
     * @deprecated Use GetAuthToken() instead.
     */
    wxString GetToken() const { return m_token; }
    
    /**
     * Refresh the access token using the stored refresh token.
     * @return True if token was refreshed successfully.
     */
    bool RefreshAccessToken();
    
    /**
     * Update stored tokens (called when tokens are refreshed).
     * @param aAccessToken New access token
     * @param aRefreshToken New refresh token (optional)
     * @param aExpiresIn Seconds until token expires (default 3600 = 1 hour)
     */
    void SetTokens( const wxString& aAccessToken, const wxString& aRefreshToken = wxEmptyString, int aExpiresIn = 3600 );
    
    /**
     * Check if access token is expiring soon (within 5 minutes).
     * @return True if token should be refreshed proactively.
     */
    bool IsTokenExpiringSoon() const;
    
    /**
     * Start the login flow.
     * 
     * This will:
     * 1. Start a local HTTP callback server
     * 2. Open the browser to the login page with callback URL
     * 3. Wait for authentication callback
     * 
     * @param aLoginUrl Base URL for login page (e.g., "https://buildwithtrace.com/login")
     * @return True if login flow started successfully.
     */
    bool StartLogin( const wxString& aLoginUrl = wxT( "https://buildwithtrace.com/login" ) );
    
    /**
     * Sign out the current user.
     * 
     * Clears stored tokens and user information.
     */
    void SignOut();
    
    /**
     * Handle authentication callback from custom URL scheme.
     * Called by the app when it receives a trace://auth?... URL.
     * 
     * @param aURL The full callback URL (e.g., "trace://auth?token=xxx&user=xxx")
     * @return True if the callback was processed successfully.
     */
    bool HandleURLCallback( const wxString& aURL );
    
#if defined( __WXMSW__ )
    /**
     * Register the trace:// URL scheme in Windows registry.
     * Called automatically on first login attempt.
     * 
     * @return True if registration was successful.
     */
    bool RegisterURLScheme();
#endif
    
    /**
     * Set callback for auth state changes.
     * @param aCallback Function to call when auth state changes.
     */
    void SetStateChangeCallback( std::function<void( AUTH_STATE )> aCallback );
    
    /**
     * Try to restore authentication from stored token.
     * Should be called on app startup.
     * 
     * @return True if a valid token was restored.
     */
    bool TryRestoreSession();
    
    /**
     * Get the callback server port (for testing/debugging).
     */
    int GetCallbackPort() const { return m_callbackPort; }

private:
    AUTH_MANAGER();
    ~AUTH_MANAGER();
    
    // Non-copyable
    AUTH_MANAGER( const AUTH_MANAGER& ) = delete;
    AUTH_MANAGER& operator=( const AUTH_MANAGER& ) = delete;
    
    /**
     * Start the local HTTP callback server.
     * @return True if server started successfully.
     */
    bool startCallbackServer();
    
    /**
     * Stop the callback server.
     */
    void stopCallbackServer();
    
    /**
     * Handle incoming connection on callback server.
     */
    void onServerEvent( wxSocketEvent& aEvent );
    
    /**
     * Handle socket event (data received).
     */
    void onSocketEvent( wxSocketEvent& aEvent );
    
    /**
     * Process the HTTP request from callback.
     * @param aRequest Raw HTTP request data.
     * @return True if callback was processed successfully.
     */
    bool processCallback( const wxString& aRequest );
    
    /**
     * Parse token and user from callback URL parameters.
     */
    bool parseCallbackParams( const wxString& aParams, wxString& aToken, AUTH_USER& aUser );
    
    /**
     * Store token securely in platform keychain.
     */
    bool storeToken( const wxString& aToken );
    
    /**
     * Load token from platform keychain.
     */
    wxString loadStoredToken();
    
    /**
     * Clear stored token from keychain.
     */
    void clearStoredToken();
    
    /**
     * Clear stored refresh token from keychain.
     */
    void clearStoredRefreshToken();
    
    /**
     * Store user info to config.
     */
    void storeUserInfo( const AUTH_USER& aUser );
    
    /**
     * Load user info from config.
     */
    AUTH_USER loadStoredUserInfo();
    
    /**
     * Clear stored user info.
     */
    void clearStoredUserInfo();
    
    /**
     * Set auth state and notify listeners.
     */
    void setState( AUTH_STATE aState );
    
    /**
     * Open URL in default browser.
     */
    bool openBrowser( const wxString& aUrl );
    
    /**
     * Find an available port for the callback server.
     */
    int findAvailablePort();
    
private:
    static AUTH_MANAGER*        s_instance;
    
    AUTH_STATE                  m_state;
    AUTH_USER                   m_currentUser;
    wxString                    m_token;          // Access token
    wxString                    m_refreshToken;   // Refresh token for getting new access tokens
    wxLongLong                  m_tokenExpiresAt; // Absolute time when token expires (milliseconds since epoch)
    
    wxSocketServer*             m_callbackServer;
    int                         m_callbackPort;
    std::atomic<bool>           m_waitingForCallback;
    
    std::function<void( AUTH_STATE )> m_stateChangeCallback;
    
    // Track last failed refresh attempt to prevent infinite retry loops
    wxLongLong                  m_lastFailedRefreshAt; // Timestamp of last failed refresh (0 = never failed or cleared)
    
    // Constants
    static const int            DEFAULT_CALLBACK_PORT_START = 19847;
    static const int            DEFAULT_CALLBACK_PORT_END = 19857;
    static const wxString       KEYCHAIN_SERVICE_NAME;
    static const wxString       KEYCHAIN_ACCOUNT_NAME;           // For access token
    static const wxString       KEYCHAIN_REFRESH_ACCOUNT_NAME;   // For refresh token
};

#endif // AUTH_MANAGER_H

