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

#ifndef PROCESS_EXECUTOR_H
#define PROCESS_EXECUTOR_H

#ifdef _WIN32
#include <string>

/**
 * Result structure for process execution
 */
struct PROCESS_RESULT
{
    int         exitCode;       ///< Process exit code
    std::string output;         ///< Combined stdout/stderr output
    bool        success;        ///< Whether process execution succeeded
    bool        timedOut;       ///< True if killed due to stall timeout
    std::string lastPhase;      ///< Last [PHASE] message received (for debugging)
    
    PROCESS_RESULT() : exitCode( -1 ), success( false ), timedOut( false ) {}
};

/**
 * Execute a process silently without showing a console window (Windows only)
 * 
 * Uses CreateProcessW with CREATE_NO_WINDOW flag to prevent any visible window.
 * Captures stdout and stderr output through anonymous pipes.
 * 
 * @param aCommandLine The full command line to execute (wide string)
 * @return PROCESS_RESULT containing exit code, output, and success status
 */
PROCESS_RESULT ExecuteProcessSilent( const std::wstring& aCommandLine );

/**
 * Execute a process with progress-based timeout (Windows only)
 * 
 * Uses CreateProcessW with CREATE_NO_WINDOW flag to prevent any visible window.
 * Monitors stdout/stderr for progress markers ([PHASE], [HEARTBEAT]) and kills
 * the process if no output is received for the specified timeout period.
 * 
 * @param aCommandLine The full command line to execute (wide string)
 * @param aStallTimeoutMs Timeout in milliseconds - kill process if no output for this duration
 * @return PROCESS_RESULT containing exit code, output, success status, and timeout flag
 */
PROCESS_RESULT ExecuteProcessWithProgress( const std::wstring& aCommandLine,
                                           int aStallTimeoutMs = 30000 );

#endif // _WIN32

#endif // PROCESS_EXECUTOR_H
