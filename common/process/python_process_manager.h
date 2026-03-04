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

#ifndef PYTHON_PROCESS_MANAGER_H
#define PYTHON_PROCESS_MANAGER_H

#include <kicommon.h>
#include <wx/string.h>
#include <map>
#include <set>
#include <mutex>

/**
 * Centralized manager for tracking and terminating Python processes spawned by KiCad applications.
 * 
 * Tracks Python process PIDs by application name (eeschema, pcbnew, kicad, etc.) and provides
 * clean termination when applications close. This replaces the previous pgrep-based approach
 * with a more reliable and efficient tracking mechanism.
 */
class KICOMMON_API PYTHON_PROCESS_MANAGER
{
public:
    /**
     * Get the singleton instance of the process manager.
     */
    static PYTHON_PROCESS_MANAGER& GetInstance();

    /**
     * Register a Python process PID for a specific application.
     * 
     * @param aAppName The name of the application (e.g., "eeschema", "pcbnew", "kicad")
     * @param aPid The process ID of the Python process
     */
    void RegisterProcess( const wxString& aAppName, long aPid );

    /**
     * Unregister a Python process PID.
     * 
     * Called when a process terminates naturally (not killed).
     * 
     * @param aPid The process ID to unregister
     */
    void UnregisterProcess( long aPid );

    /**
     * Kill all Python processes registered for a specific application.
     * 
     * Attempts graceful termination (SIGTERM) first, then force kill (SIGKILL) if needed.
     * 
     * @param aAppName The name of the application whose processes should be killed
     */
    void KillProcessesForApp( const wxString& aAppName );

private:
    PYTHON_PROCESS_MANAGER();
    ~PYTHON_PROCESS_MANAGER();
    PYTHON_PROCESS_MANAGER( const PYTHON_PROCESS_MANAGER& ) = delete;
    PYTHON_PROCESS_MANAGER& operator=( const PYTHON_PROCESS_MANAGER& ) = delete;

    std::map<wxString, std::set<long>> m_processMap;  ///< Map of app name to set of PIDs
    std::mutex                         m_mutex;       ///< Protects m_processMap from concurrent access
};

#endif // PYTHON_PROCESS_MANAGER_H

