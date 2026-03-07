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

#include <auth/auth_manager.h>
#include <amplitude_client.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>
#include <wx/socket.h>
#include <wx/uri.h>
#include <wx/url.h>
#include <wx/protocol/http.h>
#include <wx/sstream.h>
#include <wx/utils.h>
#include <wx/config.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <json_common.h>
#include <kiplatform/secrets.h>

#if defined( __WXMSW__ )
#include <wx/msw/registry.h>
#endif

// Define custom events
wxDEFINE_EVENT( EVT_AUTH_STATE_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_AUTH_TOKEN_RECEIVED, wxCommandEvent );

// Static member initialization
AUTH_MANAGER* AUTH_MANAGER::s_instance = nullptr;
const wxString AUTH_MANAGER::KEYCHAIN_SERVICE_NAME = wxT( "com.buildwithtrace.trace" );
const wxString AUTH_MANAGER::KEYCHAIN_ACCOUNT_NAME = wxT( "auth_token" );
const wxString AUTH_MANAGER::KEYCHAIN_REFRESH_ACCOUNT_NAME = wxT( "refresh_token" );


AUTH_MANAGER& AUTH_MANAGER::Instance()
{
    if( !s_instance )
    {
        s_instance = new AUTH_MANAGER();
    }
    return *s_instance;
}


void AUTH_MANAGER::Destroy()
{
    delete s_instance;
    s_instance = nullptr;
}


AUTH_MANAGER::AUTH_MANAGER() :
        m_state( AUTH_STATE::SIGNING_IN ),
        m_tokenExpiresAt( 0 ),
        m_callbackServer( nullptr ),
        m_callbackPort( 0 ),
        m_waitingForCallback( false ),
        m_lastFailedRefreshAt( 0 )
{
}


AUTH_MANAGER::~AUTH_MANAGER()
{
    stopCallbackServer();
}


bool AUTH_MANAGER::StartLogin( const wxString& aLoginUrl )
{
    // If already signing in, allow retry - user may have closed the browser tab
    // This opens a new browser window for them to try again
    if( m_state == AUTH_STATE::SIGNING_IN )
    {
        wxLogDebug( wxT( "Sign-in already in progress, allowing retry" ) );
    }
    
    setState( AUTH_STATE::SIGNING_IN );
    
    wxString callbackUrl;
    
#if defined( __WXMAC__ )
    // On macOS, use custom URL scheme (trace://auth) for professional callback
    callbackUrl = wxT( "trace://auth" );
#elif defined( __WXMSW__ )
    // On Windows, register and use custom URL scheme
    RegisterURLScheme();
    callbackUrl = wxT( "trace://auth" );
#else
    // On Linux, fall back to localhost callback server
    if( !startCallbackServer() )
    {
        setState( AUTH_STATE::AUTH_ERROR );
        return false;
    }
    callbackUrl = wxString::Format( wxT( "http://localhost:%d" ), m_callbackPort );
#endif
    
    // Build the login URL with callback parameter
    wxString fullLoginUrl = wxString::Format( wxT( "%s?callback=%s" ),
                                              aLoginUrl,
                                              wxURI( callbackUrl ).BuildURI() );
    
    // Open browser
    if( !openBrowser( fullLoginUrl ) )
    {
#if !defined( __WXMAC__ ) && !defined( __WXMSW__ )
        stopCallbackServer();
#endif
        setState( AUTH_STATE::AUTH_ERROR );
        return false;
    }
    
    m_waitingForCallback = true;
    return true;
}


bool AUTH_MANAGER::HandleURLCallback( const wxString& aURL )
{
    wxLogDebug( wxT( "AUTH_MANAGER::HandleURLCallback called with: %s" ), aURL );
    
    // Handle trace://auth?token=xxx&user=xxx callback from OS
    if( !aURL.StartsWith( wxT( "trace://auth" ) ) )
    {
        wxLogDebug( wxT( "URL does not start with trace://auth" ) );
        return false;
    }
    
    // Note: On Windows, we may be a new instance launched by the URL protocol,
    // so we don't check m_waitingForCallback here. The token will be stored
    // and available when the user opens the main app.
    
    // Parse the URL to extract parameters
    wxString queryString;
    int queryPos = aURL.Find( '?' );
    if( queryPos != wxNOT_FOUND )
    {
        queryString = aURL.Mid( queryPos + 1 );
    }
    
    wxLogDebug( wxT( "Query string: %s" ), queryString );
    
    // Parse token and user from query string
    wxString token;
    AUTH_USER user;
    
    if( !parseCallbackParams( queryString, token, user ) )
    {
        wxLogDebug( wxT( "parseCallbackParams failed" ) );
        setState( AUTH_STATE::AUTH_ERROR );
        return false;
    }
    
    wxLogDebug( wxT( "Parsed token (first 20 chars): %s..." ), token.Left( 20 ) );
    wxLogDebug( wxT( "Parsed user email: %s" ), user.email );
    
    // Store credentials
    m_token = token;
    m_currentUser = user;
    
    storeToken( token );
    storeUserInfo( user );
    
    m_waitingForCallback = false;
    // Clear failed refresh timestamp on successful login
    m_lastFailedRefreshAt = 0;
    setState( AUTH_STATE::SIGNED_IN );
    
    AMPLITUDE_CLIENT::Instance().SetUserId( user.id.ToStdString() );
    AMPLITUDE_CLIENT::Instance().SetUserProperties( {
        { "email", user.email.ToStdString() }
    } );
    AMPLITUDE_CLIENT::Instance().Track( "user_logged_in", {
        { "auth_method", "web" },
    } );
    
    wxLogDebug( wxT( "Auth state set to SIGNED_IN" ) );
    
    // Fire token received event
    if( wxTheApp )
    {
        wxCommandEvent evt( EVT_AUTH_TOKEN_RECEIVED );
        evt.SetString( token );
        wxTheApp->QueueEvent( evt.Clone() );
        wxLogDebug( wxT( "EVT_AUTH_TOKEN_RECEIVED queued" ) );
    }
    
    return true;
}


#if defined( __WXMSW__ )
bool AUTH_MANAGER::RegisterURLScheme()
{
    // Register trace:// URL scheme in Windows registry
    // HKEY_CURRENT_USER\Software\Classes\trace
    
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    
    // Create the main protocol key
    wxRegKey protocolKey( wxRegKey::HKCU, wxT( "Software\\Classes\\trace" ) );
    if( !protocolKey.Create() )
        return false;
    
    // Set default value (protocol description)
    protocolKey.SetValue( wxEmptyString, wxT( "URL:Trace Protocol" ) );
    
    // Mark as URL protocol
    protocolKey.SetValue( wxT( "URL Protocol" ), wxEmptyString );
    
    // Create DefaultIcon key
    wxRegKey iconKey( wxRegKey::HKCU, wxT( "Software\\Classes\\trace\\DefaultIcon" ) );
    if( !iconKey.Create() )
        return false;
    iconKey.SetValue( wxEmptyString, exePath + wxT( ",0" ) );
    
    // Create shell\open\command key
    wxRegKey commandKey( wxRegKey::HKCU, wxT( "Software\\Classes\\trace\\shell\\open\\command" ) );
    if( !commandKey.Create() )
        return false;
    
    // Command: "path\to\kicad.exe" "%1"
    wxString command = wxString::Format( wxT( "\"%s\" \"%%1\"" ), exePath );
    commandKey.SetValue( wxEmptyString, command );
    
    return true;
}
#endif


void AUTH_MANAGER::SignOut()
{
    stopCallbackServer();
    
    // Call backend logout endpoint to invalidate server-side session
    // Do this before clearing local tokens so we have the token to send
    if( !m_token.IsEmpty() )
    {
        try
        {
            wxString backendUrl;
            if( !wxGetEnv( wxT( "TRACE_BACKEND_URL" ), &backendUrl ) || backendUrl.IsEmpty() )
            {
#ifdef TRACE_BACKEND_URL
                backendUrl = wxT( TRACE_BACKEND_URL );
#else
                backendUrl = wxT( "http://localhost:8000/api/v2" );
#endif
            }
            
            wxString url = backendUrl + wxT( "/auth/logout" );
            wxString jsonBody = wxString::Format( wxT( "{\"access_token\":\"%s\"}" ), m_token );
            
            KICAD_CURL_EASY curl;
            curl.SetURL( url.ToStdString() );
            curl.SetPostFields( jsonBody.ToStdString() );
            curl.SetHeader( "Content-Type", "application/json" );
            curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 5L );
            
            // Fire and forget - don't block on response
            curl.Perform();
        }
        catch( ... )
        {
            // Ignore errors - we're signing out anyway
        }
    }
    
    m_token.Clear();
    m_refreshToken.Clear();

    AMPLITUDE_CLIENT::Instance().Track( "user_signed_out" );

    m_currentUser = AUTH_USER();
    m_tokenExpiresAt = 0;
    
    clearStoredToken();
    clearStoredRefreshToken();
    clearStoredUserInfo();
    
    setState( AUTH_STATE::SIGNED_OUT );
}


void AUTH_MANAGER::SetStateChangeCallback( std::function<void( AUTH_STATE )> aCallback )
{
    m_stateChangeCallback = aCallback;
}


bool AUTH_MANAGER::TryRestoreSession()
{
    // If we're already signed out and have a recent failed refresh, don't spam logs
    if( m_state == AUTH_STATE::SIGNED_OUT && m_lastFailedRefreshAt > 0 )
    {
        wxLongLong now = wxGetUTCTimeMillis();
        const wxLongLong REFRESH_COOLDOWN_MS = 60 * 1000LL; // 60 seconds
        if( (now - m_lastFailedRefreshAt) < REFRESH_COOLDOWN_MS )
        {
            // Still in cooldown, don't retry
            return false;
        }
    }
    
    wxString token = loadStoredToken();
    
    if( token.IsEmpty() )
    {
        setState( AUTH_STATE::SIGNED_OUT );
        return false;
    }
    
    // Also load refresh token
    wxString refreshToken;
    KIPLATFORM::SECRETS::GetSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_REFRESH_ACCOUNT_NAME, refreshToken );
    
    // Parse JWT to extract user info and expiry
    wxArrayString parts = wxSplit( token, '.' );
    if( parts.size() < 2 )
    {
        wxLogDebug( wxT( "Invalid JWT format - clearing stored token" ) );
        clearStoredToken();
        return false;
    }
    
    nlohmann::json jwtPayload;
    wxLongLong tokenExpiresAt = 0;
    
    try
    {
        // Decode the payload (second part)
        wxString payload = parts[1];
        
        // Add padding if needed for base64 decode
        while( payload.length() % 4 != 0 )
            payload += '=';
        
        // Replace URL-safe base64 chars
        payload.Replace( "-", "+" );
        payload.Replace( "_", "/" );
        
        // Decode base64
        wxMemoryBuffer decoded = wxBase64Decode( payload );
        if( decoded.GetDataLen() == 0 )
        {
            wxLogDebug( wxT( "Failed to decode JWT payload" ) );
            clearStoredToken();
            return false;
        }
        
        wxString jsonStr( (const char*)decoded.GetData(), decoded.GetDataLen() );
        jwtPayload = nlohmann::json::parse( jsonStr.ToStdString() );
        
        // Extract expiry time (Unix timestamp in seconds)
        if( jwtPayload.contains( "exp" ) )
        {
            long long expUnix = jwtPayload["exp"].get<long long>();
            tokenExpiresAt = expUnix * 1000LL;  // Convert to milliseconds
        }
    }
    catch( const std::exception& e )
    {
        wxLogDebug( wxT( "Failed to parse JWT: %s" ), e.what() );
        clearStoredToken();
        return false;
    }
    
    // Check if token is expired
    wxLongLong now = wxGetUTCTimeMillis();
    bool isExpired = ( tokenExpiresAt > 0 && tokenExpiresAt < now );
    
    if( isExpired )
    {
        wxLogDebug( wxT( "Stored access token is expired" ) );
        
        // Try to refresh using refresh token
        if( !refreshToken.IsEmpty() )
        {
            // Check if we recently failed a refresh attempt (within last 60 seconds)
            // This prevents infinite retry loops when refresh token is invalid
            wxLongLong currentTime = wxGetUTCTimeMillis();
            const wxLongLong REFRESH_COOLDOWN_MS = 60 * 1000LL; // 60 seconds
            
            if( m_lastFailedRefreshAt > 0 && (currentTime - m_lastFailedRefreshAt) < REFRESH_COOLDOWN_MS )
            {
                wxLongLong secondsRemaining = (REFRESH_COOLDOWN_MS - (currentTime - m_lastFailedRefreshAt)) / 1000LL;
                wxLogDebug( wxT( "Skipping token refresh - recent failure, cooldown active (%lld seconds remaining)" ), 
                           secondsRemaining.GetValue() );
                // Set state to SIGNED_OUT during cooldown to stop the timer from retrying
                setState( AUTH_STATE::SIGNED_OUT );
                return false;
            }
            
            wxLogDebug( wxT( "Attempting automatic token refresh on startup..." ) );
            
            // Temporarily set these so RefreshAccessToken() can work
            m_token = token;
            m_refreshToken = refreshToken;
            m_tokenExpiresAt = tokenExpiresAt;
            
            // Extract user info for proper state
            if( jwtPayload.contains( "sub" ) && jwtPayload["sub"].is_string()
                && jwtPayload.contains( "email" ) && jwtPayload["email"].is_string() )
            {
                m_currentUser.id = wxString::FromUTF8( jwtPayload["sub"].get<std::string>() );
                m_currentUser.email = wxString::FromUTF8( jwtPayload["email"].get<std::string>() );
                
                if( jwtPayload.contains( "user_metadata" ) && jwtPayload["user_metadata"].is_object()
                    && jwtPayload["user_metadata"].contains( "full_name" )
                    && jwtPayload["user_metadata"]["full_name"].is_string() )
                {
                    m_currentUser.fullName = wxString::FromUTF8( 
                        jwtPayload["user_metadata"]["full_name"].get<std::string>() );
                }
            }
            
            setState( AUTH_STATE::SIGNED_IN );
            
            // Try to refresh the token
            if( RefreshAccessToken() )
            {
                wxLogDebug( wxT( "Automatic token refresh successful - session restored silently" ) );
                // Clear failed refresh timestamp on success
                m_lastFailedRefreshAt = 0;

                AMPLITUDE_CLIENT::Instance().SetUserId( m_currentUser.id.ToStdString() );
                AMPLITUDE_CLIENT::Instance().SetUserProperties( {
                    { "email", m_currentUser.email.ToStdString() }
                } );
                AMPLITUDE_CLIENT::Instance().Track( "session_restored" );

                return true;
            }
            else
            {
                // Refresh failed - clear everything and require sign in
                wxLogDebug( wxT( "Automatic token refresh failed - refresh token may be expired" ) );
                m_token.Clear();
                m_refreshToken.Clear();
                m_currentUser = AUTH_USER();
                m_tokenExpiresAt = 0;
                clearStoredToken();
                clearStoredRefreshToken();
                clearStoredUserInfo();
                setState( AUTH_STATE::SIGNED_OUT );
                // Record the failure timestamp to prevent immediate retries
                m_lastFailedRefreshAt = now;
                return false;
            }
        }
        else
        {
            // No refresh token available - clear expired token
            wxLogDebug( wxT( "No refresh token available - clearing expired access token" ) );
            clearStoredToken();
            return false;
        }
    }
    
    // Token is still valid - restore session
    m_token = token;
    m_refreshToken = refreshToken;
    m_tokenExpiresAt = tokenExpiresAt;
    
    // Load or extract user info
    m_currentUser = loadStoredUserInfo();
    
    if( !m_currentUser.IsValid() && jwtPayload.contains( "sub" ) && jwtPayload["sub"].is_string()
        && jwtPayload.contains( "email" ) && jwtPayload["email"].is_string() )
    {
        // Extract from JWT
        m_currentUser.id = wxString::FromUTF8( jwtPayload["sub"].get<std::string>() );
        m_currentUser.email = wxString::FromUTF8( jwtPayload["email"].get<std::string>() );
        
        if( jwtPayload.contains( "user_metadata" ) && jwtPayload["user_metadata"].is_object()
            && jwtPayload["user_metadata"].contains( "full_name" )
            && jwtPayload["user_metadata"]["full_name"].is_string() )
        {
            m_currentUser.fullName = wxString::FromUTF8( 
                jwtPayload["user_metadata"]["full_name"].get<std::string>() );
        }
        
        // Store for next time
        storeUserInfo( m_currentUser );
    }
    
    if( m_currentUser.IsValid() )
    {
        setState( AUTH_STATE::SIGNED_IN );
        wxLogDebug( wxT( "Session restored: %s (token expires in %lld seconds)" ), 
                   m_currentUser.email, 
                   (tokenExpiresAt - now) / 1000LL );
        // Clear failed refresh timestamp on successful restore
        m_lastFailedRefreshAt = 0;
        return true;
    }
    
    return false;
}


bool AUTH_MANAGER::startCallbackServer()
{
    // Find an available port
    m_callbackPort = findAvailablePort();
    
    if( m_callbackPort == 0 )
    {
        return false;
    }
    
    // Create server socket
    wxIPV4address addr;
    addr.Service( m_callbackPort );
    addr.LocalHost();
    
    m_callbackServer = new wxSocketServer( addr );
    
    if( !m_callbackServer->IsOk() )
    {
        delete m_callbackServer;
        m_callbackServer = nullptr;
        return false;
    }
    
    // Set up event handling
    m_callbackServer->SetEventHandler( *this );
    m_callbackServer->SetNotify( wxSOCKET_CONNECTION_FLAG );
    m_callbackServer->Notify( true );
    
    Bind( wxEVT_SOCKET, &AUTH_MANAGER::onServerEvent, this, m_callbackServer->GetSocket() );
    
    return true;
}


void AUTH_MANAGER::stopCallbackServer()
{
    m_waitingForCallback = false;
    
    if( m_callbackServer )
    {
        // Unbind event before destroying to prevent dangling references
        m_callbackServer->Notify( false );
        Unbind( wxEVT_SOCKET, &AUTH_MANAGER::onServerEvent, this, m_callbackServer->GetSocket() );
        m_callbackServer->Destroy();
        m_callbackServer = nullptr;
    }
    
    m_callbackPort = 0;
}


void AUTH_MANAGER::onServerEvent( wxSocketEvent& aEvent )
{
    if( aEvent.GetSocketEvent() != wxSOCKET_CONNECTION )
        return;
    
    wxSocketBase* sock = m_callbackServer->Accept( false );
    
    if( sock )
    {
        sock->SetEventHandler( *this );
        sock->SetNotify( wxSOCKET_INPUT_FLAG | wxSOCKET_LOST_FLAG );
        sock->Notify( true );
        
        Bind( wxEVT_SOCKET, &AUTH_MANAGER::onSocketEvent, this, sock->GetSocket() );
    }
}


void AUTH_MANAGER::onSocketEvent( wxSocketEvent& aEvent )
{
    wxSocketBase* sock = aEvent.GetSocket();
    
    if( !sock )
        return;
    
    if( aEvent.GetSocketEvent() == wxSOCKET_INPUT )
    {
        // Read the HTTP request
        char buffer[4096];
        sock->Read( buffer, sizeof( buffer ) - 1 );
        size_t bytesRead = sock->LastCount();
        buffer[bytesRead] = '\0';
        
        wxString request( buffer, wxConvUTF8 );
        
        // Process the callback
        bool success = processCallback( request );
        
        // Send HTTP response
        wxString response;
        if( success )
        {
            response = wxT( "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/html\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<!DOCTYPE html><html><head>"
                           "<title>Authentication Successful</title>"
                           "<style>"
                           "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; "
                           "display: flex; justify-content: center; align-items: center; height: 100vh; "
                           "margin: 0; background: #000; color: #fff; }"
                           ".container { text-align: center; }"
                           "h1 { font-size: 24px; margin-bottom: 16px; }"
                           "p { color: #888; }"
                           "</style></head><body>"
                           "<div class='container'>"
                           "<h1>✓ Authentication Successful</h1>"
                           "<p>You can close this window and return to Trace.</p>"
                           "</div></body></html>" );
        }
        else
        {
            response = wxT( "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Type: text/html\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<!DOCTYPE html><html><head>"
                           "<title>Authentication Failed</title>"
                           "</head><body>"
                           "<h1>Authentication Failed</h1>"
                           "<p>Please try again.</p>"
                           "</body></html>" );
        }
        
        sock->Write( response.utf8_str(), response.length() );
        
        // Close socket after sending response
        sock->Close();
    }
    
    if( aEvent.GetSocketEvent() == wxSOCKET_LOST )
    {
        // Unbind events before destroying to prevent dangling references
        sock->Notify( false );
        Unbind( wxEVT_SOCKET, &AUTH_MANAGER::onSocketEvent, this, sock->GetSocket() );
        sock->Destroy();
    }
}


bool AUTH_MANAGER::processCallback( const wxString& aRequest )
{
    // Parse the GET request to extract parameters
    // Format: GET /?token=xxx&user=xxx HTTP/1.1
    
    // Find the path
    int getPos = aRequest.Find( wxT( "GET " ) );
    if( getPos == wxNOT_FOUND )
        return false;
    
    int spacePos = aRequest.Find( wxT( " HTTP" ) );
    if( spacePos == wxNOT_FOUND )
        return false;
    
    wxString path = aRequest.Mid( getPos + 4, spacePos - getPos - 4 );
    
    // Find query string
    int queryPos = path.Find( wxT( "?" ) );
    if( queryPos == wxNOT_FOUND )
        return false;
    
    wxString queryString = path.Mid( queryPos + 1 );
    
    // Parse token and user
    wxString token;
    AUTH_USER user;
    
    if( !parseCallbackParams( queryString, token, user ) )
        return false;
    
    // Store credentials
    m_token = token;
    m_currentUser = user;
    
    storeToken( token );
    storeUserInfo( user );
    
    // Stop server and update state
    stopCallbackServer();
    setState( AUTH_STATE::SIGNED_IN );
    
    // Fire token received event
    wxCommandEvent evt( EVT_AUTH_TOKEN_RECEIVED );
    evt.SetString( token );
    ProcessEvent( evt );
    
    return true;
}


bool AUTH_MANAGER::parseCallbackParams( const wxString& aParams, wxString& aToken, AUTH_USER& aUser )
{
    // Parse URL-encoded parameters
    wxArrayString pairs;
    wxString remaining = aParams;
    
    while( !remaining.IsEmpty() )
    {
        int ampPos = remaining.Find( wxT( "&" ) );
        if( ampPos == wxNOT_FOUND )
        {
            pairs.Add( remaining );
            remaining.Clear();
        }
        else
        {
            pairs.Add( remaining.Left( ampPos ) );
            remaining = remaining.Mid( ampPos + 1 );
        }
    }
    
    for( const wxString& pair : pairs )
    {
        int eqPos = pair.Find( wxT( "=" ) );
        if( eqPos == wxNOT_FOUND )
            continue;
        
        wxString key = pair.Left( eqPos );
        wxString value = wxURI::Unescape( pair.Mid( eqPos + 1 ) );
        
        if( key == wxT( "token" ) )
        {
            aToken = value;
        }
        else if( key == wxT( "refresh_token" ) )
        {
            // Store refresh token for later use
            m_refreshToken = value;
            KIPLATFORM::SECRETS::StoreSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_REFRESH_ACCOUNT_NAME, value );
            wxLogDebug( wxT( "Stored refresh token (length: %zu)" ), value.length() );
        }
        else if( key == wxT( "user" ) )
        {
            // Parse JSON user data
            try
            {
                auto json = nlohmann::json::parse( value.utf8_str().data() );
                
                if( json.contains( "id" ) && json["id"].is_string() )
                    aUser.id = wxString::FromUTF8( json["id"].get<std::string>() );
                if( json.contains( "email" ) && json["email"].is_string() )
                    aUser.email = wxString::FromUTF8( json["email"].get<std::string>() );
                if( json.contains( "full_name" ) && json["full_name"].is_string() )
                    aUser.fullName = wxString::FromUTF8( json["full_name"].get<std::string>() );
                if( json.contains( "avatar_url" ) && json["avatar_url"].is_string() )
                    aUser.avatarUrl = wxString::FromUTF8( json["avatar_url"].get<std::string>() );
            }
            catch( ... )
            {
                // JSON parse error
            }
        }
    }
    
    return !aToken.IsEmpty();
}


bool AUTH_MANAGER::storeToken( const wxString& aToken )
{
    return KIPLATFORM::SECRETS::StoreSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_ACCOUNT_NAME, aToken );
}


wxString AUTH_MANAGER::loadStoredToken()
{
    wxString token;
    KIPLATFORM::SECRETS::GetSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_ACCOUNT_NAME, token );
    return token;
}


void AUTH_MANAGER::clearStoredToken()
{
    // Actually delete the token from keychain
    KIPLATFORM::SECRETS::EraseSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_ACCOUNT_NAME );
}


void AUTH_MANAGER::clearStoredRefreshToken()
{
    // Actually delete the refresh token from keychain
    KIPLATFORM::SECRETS::EraseSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_REFRESH_ACCOUNT_NAME );
}


void AUTH_MANAGER::storeUserInfo( const AUTH_USER& aUser )
{
    wxConfigBase* config = wxConfigBase::Get();
    if( !config )
        return;
    
    config->SetPath( wxT( "/Auth" ) );
    config->Write( wxT( "UserId" ), aUser.id );
    config->Write( wxT( "UserEmail" ), aUser.email );
    config->Write( wxT( "UserFullName" ), aUser.fullName );
    config->Write( wxT( "UserAvatarUrl" ), aUser.avatarUrl );
    config->Flush();
}


AUTH_USER AUTH_MANAGER::loadStoredUserInfo()
{
    AUTH_USER user;
    wxConfigBase* config = wxConfigBase::Get();
    if( !config )
        return user;
    
    config->SetPath( wxT( "/Auth" ) );
    user.id = config->Read( wxT( "UserId" ), wxEmptyString );
    user.email = config->Read( wxT( "UserEmail" ), wxEmptyString );
    user.fullName = config->Read( wxT( "UserFullName" ), wxEmptyString );
    user.avatarUrl = config->Read( wxT( "UserAvatarUrl" ), wxEmptyString );
    
    return user;
}


void AUTH_MANAGER::clearStoredUserInfo()
{
    wxConfigBase* config = wxConfigBase::Get();
    if( !config )
        return;
    
    config->SetPath( wxT( "/Auth" ) );
    config->DeleteEntry( wxT( "UserId" ) );
    config->DeleteEntry( wxT( "UserEmail" ) );
    config->DeleteEntry( wxT( "UserFullName" ) );
    config->DeleteEntry( wxT( "UserAvatarUrl" ) );
    config->Flush();
}


void AUTH_MANAGER::setState( AUTH_STATE aState )
{
    AUTH_STATE oldState = m_state;
    m_state = aState;
    
    wxLogDebug( wxT( "AUTH_MANAGER::setState: %d -> %d" ), (int)oldState, (int)aState );
    
    if( oldState != aState )
    {
        // Fire callback
        if( m_stateChangeCallback )
        {
            m_stateChangeCallback( aState );
        }
        
        // Fire event - use ProcessEvent for immediate delivery to bound handlers
        wxCommandEvent evt( EVT_AUTH_STATE_CHANGED );
        evt.SetInt( static_cast<int>( aState ) );
        
        wxLogDebug( wxT( "AUTH_MANAGER: Firing EVT_AUTH_STATE_CHANGED" ) );
        
        // ProcessEvent delivers to handlers bound to this object
        ProcessEvent( evt );
        
        // Also post to the app to ensure all windows get notified
        if( wxTheApp )
        {
            wxCommandEvent* appEvt = new wxCommandEvent( EVT_AUTH_STATE_CHANGED );
            appEvt->SetInt( static_cast<int>( aState ) );
            wxTheApp->QueueEvent( appEvt );
        }
    }
}


bool AUTH_MANAGER::openBrowser( const wxString& aUrl )
{
    return wxLaunchDefaultBrowser( aUrl );
}


int AUTH_MANAGER::findAvailablePort()
{
    for( int port = DEFAULT_CALLBACK_PORT_START; port <= DEFAULT_CALLBACK_PORT_END; port++ )
    {
        wxIPV4address addr;
        addr.Service( port );
        addr.LocalHost();
        
        wxSocketServer testServer( addr, wxSOCKET_NOWAIT );
        if( testServer.IsOk() )
        {
            testServer.Destroy();
            return port;
        }
    }
    
    return 0;  // No available port found
}


bool AUTH_MANAGER::RefreshAccessToken()
{
    // If no refresh token, can't refresh
    if( m_refreshToken.IsEmpty() )
    {
        wxLogDebug( wxT( "No refresh token available" ) );
        return false;
    }
    
    try
    {
        // Get backend URL from environment variable (overrides build default)
        wxString backendUrl;
        if( !wxGetEnv( wxT( "TRACE_BACKEND_URL" ), &backendUrl ) || backendUrl.IsEmpty() )
        {
            // Use build-time configured URL (set in CMakeLists.txt, includes /api/v2)
#ifdef TRACE_BACKEND_URL
            backendUrl = wxT( TRACE_BACKEND_URL );
#else
            // Fallback if CMake config is missing
            backendUrl = wxT( "http://localhost:8000/api/v2" );
            wxLogWarning( "TRACE_BACKEND_URL not defined, using localhost fallback" );
#endif
        }
        else
        {
            wxLogMessage( "Using backend URL from TRACE_BACKEND_URL environment variable: %s", backendUrl );
        }
        
        // Build JSON request body
        wxString jsonBody = wxString::Format( 
            wxT( "{\"refresh_token\":\"%s\"}" ),
            m_refreshToken
        );
        
        // Make HTTP POST request to /auth/refresh using CURL (supports HTTPS)
        // Note: backendUrl already includes /api/v2 prefix
        wxString url = backendUrl + wxT( "/auth/refresh" );
        
        wxLogDebug( wxT( "Attempting token refresh at: %s" ), url );
        
        KICAD_CURL_EASY curl;
        curl.SetURL( url.ToStdString() );
        curl.SetPostFields( jsonBody.ToStdString() );
        curl.SetHeader( "Content-Type", "application/json" );
        
        // Set timeout
        curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 10L );
        
        // Perform the request (response is stored in internal buffer)
        int curlResult = curl.Perform();
        
        if( curlResult != CURLE_OK )
        {
            wxLogDebug( wxT( "Token refresh CURL request failed: %d" ), curlResult );
            return false;
        }
        
        // Check HTTP status code
        int statusCode = curl.GetResponseStatusCode();
        
        if( statusCode != 200 )
        {
            wxLogDebug( wxT( "Token refresh failed with status %d" ), statusCode );
            return false;
        }
        
        wxLogDebug( wxT( "Token refresh response received (status 200)" ) );
        
        // Parse JSON response from buffer
        // Response format: { "access_token": "...", "refresh_token": "...", "expires_in": 3600 }
        nlohmann::json responseJson = nlohmann::json::parse( curl.GetBuffer() );
        
        if( !responseJson.contains( "access_token" ) || !responseJson.contains( "refresh_token" ) )
        {
            wxLogDebug( wxT( "Invalid token refresh response" ) );
            return false;
        }

        // Extract new tokens and expiry
        wxString newAccessToken = wxString::FromUTF8( responseJson["access_token"].get<std::string>() );
        wxString newRefreshToken = wxString::FromUTF8( responseJson["refresh_token"].get<std::string>() );
        int expiresIn = responseJson.value( "expires_in", 3600 );  // Default 1 hour if not provided
        
        // Update stored tokens with expiry time
        SetTokens( newAccessToken, newRefreshToken, expiresIn );
        
        wxLogDebug( wxT( "Token refresh successful" ) );
        return true;
    }
    catch( const std::exception& e )
    {
        wxLogDebug( wxT( "Exception during token refresh: %s" ), e.what() );
        return false;
    }
    catch( ... )
    {
        wxLogDebug( wxT( "Unknown exception during token refresh" ) );
        return false;
    }
}


void AUTH_MANAGER::SetTokens( const wxString& aAccessToken, const wxString& aRefreshToken, int aExpiresIn )
{
    // Update access token
    if( !aAccessToken.IsEmpty() )
    {
        m_token = aAccessToken;
        storeToken( aAccessToken );
        
        // Calculate absolute expiry time (current time + expires_in)
        m_tokenExpiresAt = wxGetUTCTimeMillis() + (aExpiresIn * 1000LL);
        
        wxLogDebug( wxT( "Access token updated (length: %zu, expires in %d seconds)" ), 
                   aAccessToken.length(), aExpiresIn );
        
        // Clear failed refresh timestamp on successful token update
        m_lastFailedRefreshAt = 0;
    }
    
    // Update refresh token if provided
    if( !aRefreshToken.IsEmpty() )
    {
        m_refreshToken = aRefreshToken;
        KIPLATFORM::SECRETS::StoreSecret( KEYCHAIN_SERVICE_NAME, KEYCHAIN_REFRESH_ACCOUNT_NAME, aRefreshToken );
        wxLogDebug( wxT( "Refresh token updated (length: %zu)" ), aRefreshToken.length() );
    }
}


bool AUTH_MANAGER::IsTokenExpiringSoon() const
{
    if( m_tokenExpiresAt == 0 )
        return false;  // No expiry info
    
    // Token is expiring soon if less than 5 minutes remain
    wxLongLong now = wxGetUTCTimeMillis();
    wxLongLong fiveMinutes = 5 * 60 * 1000LL;
    
    return (m_tokenExpiresAt - now) < fiveMinutes;
}

