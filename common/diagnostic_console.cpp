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

#include <diagnostic_console.h>

#include <wx/sizer.h>
#include <wx/font.h>
#include <wx/datetime.h>
#include <wx/display.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/log.h>
#include <wx/utils.h>
#include <build_version.h>

#include <csignal>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#define POSIX_FILENO _fileno
#define POSIX_WRITE  _write
#define POSIX_FSYNC  _commit
#else
#include <unistd.h>
#define POSIX_FILENO fileno
#define POSIX_WRITE  write
#define POSIX_FSYNC  fsync
#endif


// ============================================================================
// DIAGNOSTIC_LOG_TARGET implementation (always compiled)
// ============================================================================

DIAGNOSTIC_LOG_TARGET::DIAGNOSTIC_LOG_TARGET()
{
}


DIAGNOSTIC_LOG_TARGET::~DIAGNOSTIC_LOG_TARGET()
{
}


void DIAGNOSTIC_LOG_TARGET::DoLogRecord( wxLogLevel level, const wxString& msg,
                                          const wxLogRecordInfo& info )
{
    wxString prefix;

    switch( level )
    {
    case wxLOG_FatalError: prefix = wxT( "[FATAL] " ); break;
    case wxLOG_Error:      prefix = wxT( "[ERROR] " ); break;
    case wxLOG_Warning:    prefix = wxT( "[WARN]  " ); break;
    case wxLOG_Message:    prefix = wxT( "[MSG]   " ); break;
    case wxLOG_Status:     prefix = wxT( "[STATUS]" ); break;
    case wxLOG_Info:       prefix = wxT( "[INFO]  " ); break;
    case wxLOG_Debug:      prefix = wxT( "[DEBUG] " ); break;
    case wxLOG_Trace:      prefix = wxT( "[TRACE] " ); break;
    default:               prefix = wxT( "[LOG]   " ); break;
    }

    wxDateTime now = wxDateTime::Now();
    wxString timestamp = now.Format( wxT( "[%Y-%m-%d %H:%M:%S] " ) );

    wxString fullMsg = prefix + msg + wxT( "\n" );
    wxString fileMsg = timestamp + fullMsg;

    fprintf( stderr, "%s", fullMsg.ToStdString().c_str() );
    fflush( stderr );

    TRACE_LOG_FILE::LogToFile( fileMsg );
}


// ============================================================================
// TRACE_LOG_FILE implementation (always compiled)
// ============================================================================

std::mutex  TRACE_LOG_FILE::s_mutex;
FILE*       TRACE_LOG_FILE::s_logFile = nullptr;
int         TRACE_LOG_FILE::s_logFd = -1;
wxString    TRACE_LOG_FILE::s_logFilePath;


wxString TRACE_LOG_FILE::GetLogDirectory()
{
    wxString homeDir = wxGetHomeDir();
    wxString logDir = homeDir + wxFileName::GetPathSeparator() + wxT( ".trace" );

#ifdef __linux__
    if( wxFileExists( logDir ) )
    {
        logDir = homeDir + wxFileName::GetPathSeparator()
               + wxT( ".local/share/trace/logs" );
    }
#endif

    return logDir;
}


wxString TRACE_LOG_FILE::GetLogFilePath()
{
    std::lock_guard<std::mutex> lock( s_mutex );
    return s_logFilePath;
}


void TRACE_LOG_FILE::OpenLogFile()
{
    wxString logDir = GetLogDirectory();

    if( !wxDirExists( logDir ) )
    {
        wxLogNull suppressWarnings;

        if( !wxFileName::Mkdir( logDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
            return;
    }

    wxDateTime now = wxDateTime::Now();
    wxString filename = now.Format( wxT( "diagnostic_%Y-%m-%d_%H%M%S.log" ) );
    s_logFilePath = logDir + wxFileName::GetPathSeparator() + filename;

    s_logFile = fopen( s_logFilePath.ToStdString().c_str(), "w" );

    if( s_logFile )
    {
        s_logFd = POSIX_FILENO( s_logFile );

        wxString header = wxString::Format(
            wxT( "=== Trace Diagnostic Log Started: %s ===\n" )
            wxT( "Log file: %s\n" )
            wxT( "Enabled traces: TRACE_AI, TRACE_AI_TOOL_CALL, " )
            wxT( "TRACE_AI_FILE_OP, TRACE_FILE_SAVE, TRACE_AI_BACKEND\n\n" ),
            now.FormatISOCombined(),
            s_logFilePath );
        fprintf( s_logFile, "%s", header.ToStdString().c_str() );
        fflush( s_logFile );
    }
}


void TRACE_LOG_FILE::CloseLogFile()
{
    if( s_logFile )
    {
        wxDateTime now = wxDateTime::Now();
        wxString footer = wxString::Format(
            wxT( "\n=== Trace Diagnostic Log Ended: %s ===\n" ),
            now.FormatISOCombined() );
        fprintf( s_logFile, "%s", footer.ToStdString().c_str() );
        fflush( s_logFile );

        fclose( s_logFile );
        s_logFile = nullptr;
        s_logFd = -1;
    }
}


void TRACE_LOG_FILE::LogToFile( const wxString& aMessage )
{
    std::lock_guard<std::mutex> lock( s_mutex );

    if( !s_logFile )
        return;

    fprintf( s_logFile, "%s", aMessage.ToStdString().c_str() );
    fflush( s_logFile );

    long pos = ftell( s_logFile );

    if( pos > MAX_LOG_SIZE )
    {
        long keepFrom = pos - KEEP_LOG_SIZE;

        if( keepFrom < 0 )
            keepFrom = 0;

        std::vector<char> buf( pos - keepFrom );
        fseek( s_logFile, keepFrom, SEEK_SET );
        size_t bytesRead = fread( buf.data(), 1, buf.size(), s_logFile );

        // Reopen the file in write mode to truncate
        freopen( s_logFilePath.ToStdString().c_str(), "w", s_logFile );

        if( s_logFile )
        {
            s_logFd = POSIX_FILENO( s_logFile );

            const char* truncHeader = "[--- log truncated ---]\n";
            fprintf( s_logFile, "%s", truncHeader );
            fwrite( buf.data(), 1, bytesRead, s_logFile );
            fflush( s_logFile );
        }
    }
}


std::vector<wxString> TRACE_LOG_FILE::CheckForCrashLogs()
{
    std::vector<wxString> crashLogs;
    wxString logDir = GetLogDirectory();
    wxString currentLog = GetLogFilePath();

    if( !wxDirExists( logDir ) )
        return crashLogs;

    wxDir dir( logDir );

    if( !dir.IsOpened() )
        return crashLogs;

    wxString filename;
    bool cont = dir.GetFirst( &filename, wxT( "diagnostic_*.log" ), wxDIR_FILES );

    while( cont )
    {
        wxString fullPath = logDir + wxFileName::GetPathSeparator() + filename;

        if( fullPath != currentLog )
        {
            wxFFile file( fullPath, wxT( "r" ) );

            if( file.IsOpened() )
            {
                wxFileOffset len = file.Length();
                wxString tail;

                if( len > 512 )
                {
                    file.Seek( len - 512 );
                    file.ReadAll( &tail );
                }
                else
                {
                    file.ReadAll( &tail );
                }

                if( !tail.Contains( wxT( "=== Trace Diagnostic Log Ended" ) ) )
                {
                    crashLogs.push_back( fullPath );
                }
            }
        }

        cont = dir.GetNext( &filename );
    }

    return crashLogs;
}


void TRACE_LOG_FILE::CleanupOldLogs( const wxString& aExcludeFile )
{
    wxString logDir = GetLogDirectory();

    if( !wxDirExists( logDir ) )
        return;

    wxDir dir( logDir );

    if( !dir.IsOpened() )
        return;

    wxString filename;
    bool cont = dir.GetFirst( &filename, wxT( "diagnostic_*.log" ), wxDIR_FILES );

    while( cont )
    {
        wxString fullPath = logDir + wxFileName::GetPathSeparator() + filename;

        if( fullPath != aExcludeFile )
        {
            wxRemoveFile( fullPath );
        }

        cont = dir.GetNext( &filename );
    }
}


wxString TRACE_LOG_FILE::CollectSystemInfo()
{
    wxString info;

    info += wxT( "{\n" );
    info += wxString::Format( wxT( "  \"os\": \"%s\",\n" ), wxGetOsDescription() );
    info += wxString::Format( wxT( "  \"app_version\": \"%s\",\n" ), GetTraceBuildVersion() );
    info += wxString::Format( wxT( "  \"version_short\": \"%s\",\n" ), GetTraceMajorMinorPatchVersion() );
    info += wxString::Format( wxT( "  \"kicad_version\": \"%s\",\n" ), GetMajorMinorPatchVersion() );

#if defined( __x86_64__ ) || defined( _M_X64 )
    info += wxT( "  \"arch\": \"x86_64\",\n" );
#elif defined( __aarch64__ ) || defined( _M_ARM64 )
    info += wxT( "  \"arch\": \"arm64\",\n" );
#elif defined( __i386__ ) || defined( _M_IX86 )
    info += wxT( "  \"arch\": \"x86\",\n" );
#else
    info += wxT( "  \"arch\": \"unknown\",\n" );
#endif

    info += wxString::Format( wxT( "  \"pointer_size\": %zu,\n" ), sizeof( void* ) );

    wxMemorySize freeMem = wxGetFreeMemory();

    if( freeMem != -1 )
        info += wxString::Format( wxT( "  \"free_memory_mb\": %lld,\n" ),
                                  static_cast<long long>( freeMem.GetValue() / ( 1024 * 1024 ) ) );

    int displayCount = wxDisplay::GetCount();
    info += wxString::Format( wxT( "  \"display_count\": %d,\n" ), displayCount );

    if( displayCount > 0 )
    {
        wxDisplay primaryDisplay( static_cast<unsigned>( 0 ) );
        wxRect geom = primaryDisplay.GetGeometry();
        info += wxString::Format( wxT( "  \"primary_display\": \"%dx%d\",\n" ),
                                  geom.GetWidth(), geom.GetHeight() );
    }

#ifdef KICAD_DIAGNOSTIC_LOGGING
    info += wxT( "  \"diagnostic_build\": true\n" );
#else
    info += wxT( "  \"diagnostic_build\": false\n" );
#endif

    info += wxT( "}" );

    return info;
}


// ============================================================================
// Signal handlers (always compiled)
// ============================================================================

namespace
{
    volatile sig_atomic_t g_signalLogFd = -1;

    void crashSignalHandler( int aSigNum )
    {
        int fd = g_signalLogFd;

        if( fd >= 0 )
        {
            // Async-signal-safe: only write() and fsync()
            const char* prefix = "\n=== CRASH: signal ";
            POSIX_WRITE( fd, prefix, strlen( prefix ) );

            char sigBuf[16];
            int  idx = 0;
            int  val = aSigNum;

            if( val < 0 )
            {
                sigBuf[idx++] = '-';
                val = -val;
            }

            char digits[16];
            int  dIdx = 0;

            do
            {
                digits[dIdx++] = '0' + ( val % 10 );
                val /= 10;
            } while( val > 0 );

            while( dIdx > 0 )
                sigBuf[idx++] = digits[--dIdx];

            POSIX_WRITE( fd, sigBuf, idx );

            const char* suffix = " ===\n";
            POSIX_WRITE( fd, suffix, strlen( suffix ) );
            POSIX_FSYNC( fd );
        }

        // Re-raise with default handler so OS can generate core dump
        signal( aSigNum, SIG_DFL );
        raise( aSigNum );
    }
}


void TRACE_LOG_FILE::InstallSignalHandlers()
{
    g_signalLogFd = s_logFd;

    signal( SIGSEGV, crashSignalHandler );
    signal( SIGABRT, crashSignalHandler );
#ifndef _WIN32
    signal( SIGBUS,  crashSignalHandler );
#endif
    signal( SIGFPE,  crashSignalHandler );
}


// ============================================================================
// DIAGNOSTIC_CONSOLE implementation (diagnostic builds only)
// ============================================================================

#ifdef KICAD_DIAGNOSTIC_LOGGING

std::mutex DIAGNOSTIC_CONSOLE::s_mutex;
DIAGNOSTIC_CONSOLE* DIAGNOSTIC_CONSOLE::s_instance = nullptr;


BEGIN_EVENT_TABLE( DIAGNOSTIC_CONSOLE, wxFrame )
    EVT_CLOSE( DIAGNOSTIC_CONSOLE::onClose )
END_EVENT_TABLE()


DIAGNOSTIC_CONSOLE& DIAGNOSTIC_CONSOLE::Instance()
{
    std::lock_guard<std::mutex> lock( s_mutex );

    if( !s_instance )
    {
        s_instance = new DIAGNOSTIC_CONSOLE( nullptr );
    }

    return *s_instance;
}


void DIAGNOSTIC_CONSOLE::Create( wxWindow* aParent )
{
    std::lock_guard<std::mutex> lock( s_mutex );

    if( !s_instance )
    {
        s_instance = new DIAGNOSTIC_CONSOLE( aParent );
        s_instance->Show();
    }
}


void DIAGNOSTIC_CONSOLE::Log( const wxString& aMessage )
{
    DIAGNOSTIC_CONSOLE* instance = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;

    {
        std::lock_guard<std::mutex> lock( s_mutex );
        if( s_instance && s_instance->m_alive && s_instance->m_alive->load() )
        {
            instance = s_instance;
            alive = s_instance->m_alive;
        }
    }

    if( instance && alive )
    {
        instance->CallAfter( [alive, msg = aMessage]()
        {
            std::lock_guard<std::mutex> innerLock( s_mutex );
            if( alive->load() && s_instance )
            {
                s_instance->appendText( msg );
            }
        } );
    }
}


bool DIAGNOSTIC_CONSOLE::IsCreated()
{
    std::lock_guard<std::mutex> lock( s_mutex );
    return s_instance != nullptr;
}


void DIAGNOSTIC_CONSOLE::DestroyConsole()
{
    std::lock_guard<std::mutex> lock( s_mutex );

    if( s_instance )
    {
        if( s_instance->m_alive )
        {
            s_instance->m_alive->store( false );
        }

        s_instance->wxFrame::Destroy();
        s_instance = nullptr;
    }
}


void DIAGNOSTIC_CONSOLE::ShowConsole()
{
    Show();
    Raise();
}


DIAGNOSTIC_CONSOLE::DIAGNOSTIC_CONSOLE( wxWindow* aParent )
    : wxFrame( aParent, wxID_ANY, wxT( "Trace Diagnostic Console" ),
               wxDefaultPosition, wxSize( 900, 400 ),
               wxDEFAULT_FRAME_STYLE ),
      m_alive( std::make_shared<std::atomic<bool>>( true ) )
{
    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_textCtrl = new wxTextCtrl( this, wxID_ANY, wxEmptyString,
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP );

    m_textCtrl->SetFont( wxFont( wxNORMAL_FONT->GetPointSize(),
                                  wxFONTFAMILY_TELETYPE,
                                  wxFONTSTYLE_NORMAL,
                                  wxFONTWEIGHT_NORMAL,
                                  false,
                                  wxEmptyString ) );

    m_textCtrl->SetBackgroundColour( wxColour( 30, 30, 30 ) );
    m_textCtrl->SetForegroundColour( wxColour( 200, 200, 200 ) );

    sizer->Add( m_textCtrl, 1, wxEXPAND | wxALL, 0 );
    SetSizer( sizer );

#ifdef __WXMAC__
    SetPosition( wxPoint( 50, 50 ) );
#else
    wxDisplay display;
    wxRect screenRect = display.GetClientArea();
    SetPosition( wxPoint( screenRect.GetRight() - 920, screenRect.GetBottom() - 450 ) );
#endif

    wxDateTime now = wxDateTime::Now();
    wxString logPath = TRACE_LOG_FILE::GetLogFilePath();
    wxString startMsg = wxString::Format(
        wxT( "=== Trace Diagnostic Console Started: %s ===\n" )
        wxT( "Log file: %s\n" )
        wxT( "Enabled traces: TRACE_AI, TRACE_AI_TOOL_CALL, " )
        wxT( "TRACE_AI_FILE_OP, TRACE_FILE_SAVE, TRACE_AI_BACKEND\n\n" ),
        now.FormatISOCombined(),
        logPath.IsEmpty() ? wxString( wxT( "(none)" ) ) : logPath );

    m_textCtrl->AppendText( startMsg );
}


DIAGNOSTIC_CONSOLE::~DIAGNOSTIC_CONSOLE()
{
    if( m_alive )
    {
        m_alive->store( false );
    }
}


void DIAGNOSTIC_CONSOLE::appendText( const wxString& aMessage )
{
    if( m_textCtrl )
    {
        m_textCtrl->AppendText( aMessage );
        m_textCtrl->ShowPosition( m_textCtrl->GetLastPosition() );
    }
}


void DIAGNOSTIC_CONSOLE::onClose( wxCloseEvent& aEvent )
{
    if( aEvent.CanVeto() )
    {
        Hide();
        aEvent.Veto();
    }
    else
    {
        std::lock_guard<std::mutex> lock( s_mutex );
        if( m_alive )
        {
            m_alive->store( false );
        }
        s_instance = nullptr;
        Destroy();
    }
}

#endif // KICAD_DIAGNOSTIC_LOGGING
