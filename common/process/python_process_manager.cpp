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

#include "python_process_manager.h"
#include <wx/utils.h>
#include <wx/process.h>
#include <algorithm>

PYTHON_PROCESS_MANAGER& PYTHON_PROCESS_MANAGER::GetInstance()
{
    static PYTHON_PROCESS_MANAGER instance;
    return instance;
}


PYTHON_PROCESS_MANAGER::PYTHON_PROCESS_MANAGER()
{
}


PYTHON_PROCESS_MANAGER::~PYTHON_PROCESS_MANAGER()
{
    // Clean up all remaining processes on destruction
    // Use fire-and-forget approach - no need to wait, OS handles cleanup
    std::lock_guard<std::mutex> lock( m_mutex );
    
    for( auto& [appName, pids] : m_processMap )
    {
        for( long pid : pids )
        {
            if( pid > 0 )
            {
                wxKill( pid, wxSIGTERM );
                wxKill( pid, wxSIGKILL );
            }
        }
    }
    
    m_processMap.clear();
}


void PYTHON_PROCESS_MANAGER::RegisterProcess( const wxString& aAppName, long aPid )
{
    if( aPid <= 0 )
        return;
    
    std::lock_guard<std::mutex> lock( m_mutex );
    m_processMap[aAppName].insert( aPid );
}


void PYTHON_PROCESS_MANAGER::UnregisterProcess( long aPid )
{
    if( aPid <= 0 )
        return;
    
    std::lock_guard<std::mutex> lock( m_mutex );
    
    // Find and remove the PID from any app's set
    for( auto it = m_processMap.begin(); it != m_processMap.end(); )
    {
        it->second.erase( aPid );
        
        // Remove empty app entries
        if( it->second.empty() )
        {
            it = m_processMap.erase( it );
        }
        else
        {
            ++it;
        }
    }
}


void PYTHON_PROCESS_MANAGER::KillProcessesForApp( const wxString& aAppName )
{
    std::set<long> pidsToKill;
    
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        
        auto it = m_processMap.find( aAppName );
        if( it == m_processMap.end() )
            return;
        
        // Copy the PIDs to kill (we'll remove them from the map after killing)
        pidsToKill = it->second;
    }
    
    // Kill processes - use fire-and-forget approach to avoid blocking UI thread
    // No need to wait between SIGTERM and SIGKILL; OS handles cleanup
    for( long pid : pidsToKill )
    {
        if( pid > 0 )
        {
            wxKill( pid, wxSIGTERM );
            wxKill( pid, wxSIGKILL );
        }
    }
    
    // Remove the app entry from the map
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_processMap.erase( aAppName );
    }
}

