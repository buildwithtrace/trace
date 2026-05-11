/*
 * This program source code file is part of Trace, an AI-native PCB design application.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _WIN32

#include <process_executor.h>
#include <process/python_process_manager.h>
#include <windows.h>
#include <iostream>
#include <chrono>
#include <wx/app.h>


PROCESS_RESULT ExecuteProcessSilent( const std::wstring& aCommandLine )
{
    PROCESS_RESULT result;
    result.exitCode = 0;
    result.success = false;
    result.timedOut = false;
    
    // Create pipes for stdout/stderr capture
    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;  // Pipe handles are inherited by child process
    
    if( !CreatePipe( &hReadPipe, &hWritePipe, &sa, 0 ) )
    {
        std::cerr << "ExecuteProcessSilent: Failed to create pipe" << std::endl;
        return result;
    }
    
    // Ensure the read handle is not inherited (only child should have write handle)
    if( !SetHandleInformation( hReadPipe, HANDLE_FLAG_INHERIT, 0 ) )
    {
        std::cerr << "ExecuteProcessSilent: Failed to set handle information" << std::endl;
        CloseHandle( hReadPipe );
        CloseHandle( hWritePipe );
        return result;
    }
    
    // Set up process startup info
    STARTUPINFOW si;
    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    si.wShowWindow = SW_HIDE;  // Hide window (combined with CREATE_NO_WINDOW)
    
    PROCESS_INFORMATION pi;
    ZeroMemory( &pi, sizeof(pi) );
    
    // CREATE_NO_WINDOW prevents console window from appearing
    // This is the key flag that eliminates the window flash
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    
    // CreateProcess requires a modifiable buffer for the command line
    std::wstring cmdLine = aCommandLine;
    
    BOOL processCreated = CreateProcessW(
        NULL,                       // Application name (NULL = use command line)
        &cmdLine[0],               // Command line (modifiable buffer)
        NULL,                       // Process security attributes
        NULL,                       // Thread security attributes
        TRUE,                       // Inherit handles (for pipes)
        flags,                      // Creation flags
        NULL,                       // Environment (use parent's)
        NULL,                       // Current directory (use parent's)
        &si,                        // Startup info
        &pi                         // Process info (output)
    );
    
    // Close write end of pipe in parent process
    // Child process has its own handle to write end
    CloseHandle( hWritePipe );
    
    if( !processCreated )
    {
        DWORD error = GetLastError();
        std::cerr << "ExecuteProcessSilent: CreateProcess failed with error " << error << std::endl;
        CloseHandle( hReadPipe );
        return result;
    }
    
    // Read output from pipe
    // We read in a loop until the child closes its write end (by terminating)
    char buffer[4096];
    DWORD bytesRead;
    
    while( ReadFile( hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL ) && bytesRead > 0 )
    {
        buffer[bytesRead] = '\0';
        result.output += buffer;
    }
    
    // Wait for process to complete
    WaitForSingleObject( pi.hProcess, INFINITE );
    
    // Get exit code
    DWORD exitCode = 0;
    if( GetExitCodeProcess( pi.hProcess, &exitCode ) )
    {
        result.exitCode = static_cast<int>( exitCode );
    }
    else
    {
        std::cerr << "ExecuteProcessSilent: Failed to get exit code" << std::endl;
    }
    
    // Cleanup handles
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    CloseHandle( hReadPipe );
    
    result.success = true;
    return result;
}


PROCESS_RESULT ExecuteProcessWithProgress( const std::wstring& aCommandLine,
                                           int aStallTimeoutMs )
{
    PROCESS_RESULT result;
    result.exitCode = -1;
    result.success = false;
    result.timedOut = false;
    
    // Create pipes for stdout/stderr capture
    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof( SECURITY_ATTRIBUTES );
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    
    if( !CreatePipe( &hReadPipe, &hWritePipe, &sa, 0 ) )
    {
        std::cerr << "ExecuteProcessWithProgress: Failed to create pipe" << std::endl;
        return result;
    }
    
    if( !SetHandleInformation( hReadPipe, HANDLE_FLAG_INHERIT, 0 ) )
    {
        std::cerr << "ExecuteProcessWithProgress: Failed to set handle information" << std::endl;
        CloseHandle( hReadPipe );
        CloseHandle( hWritePipe );
        return result;
    }
    
    // Set up process startup info
    STARTUPINFOW si;
    ZeroMemory( &si, sizeof( si ) );
    si.cb = sizeof( STARTUPINFOW );
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    si.wShowWindow = SW_HIDE;
    
    PROCESS_INFORMATION pi;
    ZeroMemory( &pi, sizeof( pi ) );
    
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    
    std::wstring cmdLine = aCommandLine;
    
    BOOL processCreated = CreateProcessW(
        NULL,
        &cmdLine[0],
        NULL,
        NULL,
        TRUE,
        flags,
        NULL,
        NULL,
        &si,
        &pi
    );
    
    CloseHandle( hWritePipe );
    
    if( !processCreated )
    {
        DWORD error = GetLastError();
        std::cerr << "ExecuteProcessWithProgress: CreateProcess failed with error " << error << std::endl;
        CloseHandle( hReadPipe );
        return result;
    }
    
    // Register with process manager for cleanup on app exit
    if( wxTheApp )
    {
        PYTHON_PROCESS_MANAGER::GetInstance().RegisterProcess( 
            wxTheApp->GetAppName(), static_cast<long>( pi.dwProcessId ) );
    }
    
    // Non-blocking read loop with stall detection
    auto lastActivityTime = std::chrono::steady_clock::now();
    char buffer[4096];
    DWORD bytesAvailable, bytesRead;
    bool processRunning = true;
    
    while( processRunning )
    {
        // Check for available data (non-blocking)
        if( PeekNamedPipe( hReadPipe, NULL, 0, NULL, &bytesAvailable, NULL ) 
            && bytesAvailable > 0 )
        {
            if( ReadFile( hReadPipe, buffer, sizeof( buffer ) - 1, &bytesRead, NULL ) 
                && bytesRead > 0 )
            {
                buffer[bytesRead] = '\0';
                result.output += buffer;
                lastActivityTime = std::chrono::steady_clock::now();
                
                // Extract last phase for debugging
                std::string chunk( buffer );
                size_t phasePos = chunk.rfind( "[PHASE]" );
                if( phasePos != std::string::npos )
                {
                    size_t endPos = chunk.find( '\n', phasePos );
                    if( endPos != std::string::npos )
                    {
                        result.lastPhase = chunk.substr( phasePos + 8, endPos - phasePos - 8 );
                    }
                    else
                    {
                        result.lastPhase = chunk.substr( phasePos + 8 );
                    }
                    // Trim whitespace from lastPhase
                    while( !result.lastPhase.empty() && 
                           ( result.lastPhase.back() == '\r' || result.lastPhase.back() == '\n' ) )
                    {
                        result.lastPhase.pop_back();
                    }
                }
            }
        }
        
        // Check if process has exited (100ms poll interval)
        DWORD waitResult = WaitForSingleObject( pi.hProcess, 100 );
        if( waitResult == WAIT_OBJECT_0 )
        {
            processRunning = false;
            
            // Drain remaining output
            while( PeekNamedPipe( hReadPipe, NULL, 0, NULL, &bytesAvailable, NULL ) 
                   && bytesAvailable > 0 )
            {
                if( ReadFile( hReadPipe, buffer, sizeof( buffer ) - 1, &bytesRead, NULL ) 
                    && bytesRead > 0 )
                {
                    buffer[bytesRead] = '\0';
                    result.output += buffer;
                }
                else
                {
                    break;
                }
            }
        }
        else
        {
            // Check for stall timeout
            auto elapsed = std::chrono::steady_clock::now() - lastActivityTime;
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count();
            
            if( elapsedMs > aStallTimeoutMs )
            {
                // Process stalled - terminate it
                std::cerr << "ExecuteProcessWithProgress: Process stalled for " << elapsedMs 
                          << "ms, terminating. Last phase: " << result.lastPhase << std::endl;
                TerminateProcess( pi.hProcess, 1 );
                result.timedOut = true;
                processRunning = false;
            }
        }
    }
    
    // Get exit code
    DWORD exitCode = 0;
    if( GetExitCodeProcess( pi.hProcess, &exitCode ) )
    {
        result.exitCode = static_cast<int>( exitCode );
    }
    
    result.success = !result.timedOut && result.exitCode == 0;
    
    // Unregister from process manager
    if( wxTheApp )
    {
        PYTHON_PROCESS_MANAGER::GetInstance().UnregisterProcess( 
            static_cast<long>( pi.dwProcessId ) );
    }
    
    // Cleanup handles
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    CloseHandle( hReadPipe );
    
    return result;
}

#endif // _WIN32
