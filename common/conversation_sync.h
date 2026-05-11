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

#ifndef CONVERSATION_SYNC_H
#define CONVERSATION_SYNC_H

#include <wx/string.h>
#include <kicommon.h>
#include <atomic>
#include <thread>
#include <mutex>

/**
 * Background service for synchronizing local conversations to Supabase.
 * 
 * Features:
 * - Syncs unsynced conversations and messages to Supabase
 * - Runs in background thread every 30 seconds
 * - Fetches conversations from Supabase on sign-in
 * - Handles conflict resolution (Supabase is source of truth)
 * 
 * Usage:
 *   // Start sync on app startup or sign-in
 *   CONVERSATION_SYNC::Instance().Start();
 *   
 *   // Stop sync on app shutdown or sign-out
 *   CONVERSATION_SYNC::Instance().Stop();
 *   
 *   // Trigger immediate sync (e.g., after sending message)
 *   CONVERSATION_SYNC::Instance().SyncNow();
 */
class KICOMMON_API CONVERSATION_SYNC
{
public:
    /**
     * Get the singleton instance.
     */
    static CONVERSATION_SYNC& Instance();

    /**
     * Start the background sync thread.
     * Safe to call multiple times - will only start once.
     */
    void Start();

    /**
     * Stop the background sync thread.
     * Waits for current sync operation to complete.
     */
    void Stop();

    /**
     * Check if sync is running.
     */
    bool IsRunning() const { return m_running.load(); }

    /**
     * Trigger an immediate sync.
     * Non-blocking - schedules sync on background thread.
     */
    void SyncNow();

    /**
     * Fetch conversations from Supabase and merge with local.
     * Called after successful sign-in.
     * @return Number of conversations fetched.
     */
    int FetchFromSupabase();

    /**
     * Set the Supabase API endpoint URL.
     * @param aUrl The Supabase REST API URL.
     */
    void SetSupabaseUrl( const wxString& aUrl ) { m_supabaseUrl = aUrl; }

    /**
     * Get sync interval in seconds.
     */
    static constexpr int SYNC_INTERVAL_SECONDS = 30;

private:
    CONVERSATION_SYNC();
    ~CONVERSATION_SYNC();

    // Prevent copying
    CONVERSATION_SYNC( const CONVERSATION_SYNC& ) = delete;
    CONVERSATION_SYNC& operator=( const CONVERSATION_SYNC& ) = delete;

    /**
     * Background thread main loop.
     */
    void syncLoop();

    /**
     * Perform a single sync operation.
     * @return True if sync succeeded.
     */
    bool doSync();

    /**
     * Push unsynced conversations to Supabase.
     * @return Number of conversations synced.
     */
    int pushConversations();

    /**
     * Push unsynced messages to Supabase.
     * @return Number of messages synced.
     */
    int pushMessages();

    std::atomic<bool>         m_running;
    std::atomic<bool>         m_syncRequested;
    std::thread               m_thread;
    std::mutex                m_mutex;
    wxString                  m_supabaseUrl;
};

#endif // CONVERSATION_SYNC_H

