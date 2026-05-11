/*
 * This program source code file is part of Trace, a fork of KiCad.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file diagnostic_console.h
 * @brief Session log file management and optional GUI diagnostic console.
 *
 * TRACE_LOG_FILE handles per-session log files that are always active.
 * DIAGNOSTIC_CONSOLE provides a live GUI log viewer only in diagnostic builds
 * (KICAD_DIAGNOSTIC_LOGGING).
 */

#ifndef DIAGNOSTIC_CONSOLE_H
#define DIAGNOSTIC_CONSOLE_H

#include <kicommon.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/arrstr.h>
#include <mutex>
#include <vector>

// ============================================================================
// TRACE_LOG_FILE -- always-on per-session file logging
// ============================================================================

/**
 * Custom wxLog target that forwards messages to the session log file.
 * Always compiled -- not gated by KICAD_DIAGNOSTIC_LOGGING.
 */
class KICOMMON_API DIAGNOSTIC_LOG_TARGET : public wxLog
{
public:
    DIAGNOSTIC_LOG_TARGET();
    virtual ~DIAGNOSTIC_LOG_TARGET();

protected:
    virtual void DoLogRecord( wxLogLevel level, const wxString& msg,
                              const wxLogRecordInfo& info ) override;
};

/**
 * Manages the per-session log file in ~/.trace/.
 *
 * Always compiled. Handles file creation, size-capped writing, crash detection,
 * cleanup of old logs, system info collection, and signal handler installation.
 */
class KICOMMON_API TRACE_LOG_FILE
{
public:
    static void OpenLogFile();
    static void CloseLogFile();

    /**
     * Write a message to the log file. Thread-safe.
     * Enforces a 5 MB size cap via tail-preserving truncation.
     */
    static void LogToFile( const wxString& aMessage );

    static wxString GetLogFilePath();
    static wxString GetLogDirectory();

    /**
     * Scan ~/.trace/ for previous session logs that are missing the
     * clean-shutdown footer sentinel. Returns their full paths.
     */
    static std::vector<wxString> CheckForCrashLogs();

    /**
     * Delete all diagnostic_*.log files in ~/.trace/ except @a aExcludeFile.
     */
    static void CleanupOldLogs( const wxString& aExcludeFile );

    /**
     * Build a JSON string with OS, app version, architecture, memory, and
     * display configuration for crash report submission.
     */
    static wxString CollectSystemInfo();

    /**
     * Install async-signal-safe handlers for SIGSEGV, SIGABRT, SIGFPE
     * that write a crash marker to the log file descriptor before dying.
     */
    static void InstallSignalHandlers();

private:
    static constexpr long MAX_LOG_SIZE = 5 * 1024 * 1024;      // 5 MB
    static constexpr long KEEP_LOG_SIZE = 4 * 1024 * 1024;     // 4 MB after truncation

    static std::mutex  s_mutex;
    static FILE*       s_logFile;
    static int         s_logFd;         // raw fd for signal handler
    static wxString    s_logFilePath;
};


// ============================================================================
// DIAGNOSTIC_CONSOLE -- GUI log viewer (diagnostic builds only)
// ============================================================================

#ifdef KICAD_DIAGNOSTIC_LOGGING

#include <wx/frame.h>
#include <wx/textctrl.h>
#include <atomic>
#include <memory>

/**
 * A singleton wxFrame that displays live trace output.
 * Only available when KICAD_DIAGNOSTIC_LOGGING is defined.
 */
class KICOMMON_API DIAGNOSTIC_CONSOLE : public wxFrame
{
public:
    static DIAGNOSTIC_CONSOLE& Instance();
    static void Create( wxWindow* aParent = nullptr );
    static void Log( const wxString& aMessage );
    static bool IsCreated();
    static void DestroyConsole();
    void ShowConsole();

private:
    DIAGNOSTIC_CONSOLE( wxWindow* aParent );
    ~DIAGNOSTIC_CONSOLE();

    void appendText( const wxString& aMessage );
    void onClose( wxCloseEvent& aEvent );

    wxTextCtrl*                         m_textCtrl;
    std::shared_ptr<std::atomic<bool>>  m_alive;

    static std::mutex                   s_mutex;
    static DIAGNOSTIC_CONSOLE*          s_instance;

    DECLARE_EVENT_TABLE()
};

#endif // KICAD_DIAGNOSTIC_LOGGING

#endif // DIAGNOSTIC_CONSOLE_H
