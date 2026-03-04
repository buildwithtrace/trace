/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2015 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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
 * @file kicad.cpp
 * Main KiCad project manager file.
 */


#include <wx/filename.h>
#include <wx/log.h>
#include <wx/app.h>
#include <wx/stdpaths.h>
#include <wx/msgdlg.h>
#include <wx/cmdline.h>
#include <wx/file.h>
#include <wx/utils.h>

#include <env_vars.h>
#include <file_history.h>
#include <hotkeys_basic.h>
#include <kiway.h>
#include <macros.h>
#include <paths.h>
#include <richio.h>
#include <settings/settings_manager.h>
#include <settings/kicad_settings.h>
#include <../include/startwizard/startwizard.h>
#include <systemdirsappend.h>
#include <thread_pool.h>
#include <trace_helpers.h>
#include <wildcards_and_files_ext.h>
#include <confirm.h>

#include <git/git_backend.h>
#include <git/libgit_backend.h>
#include <stdexcept>

#include "pgm_kicad.h"
#include "kicad_manager_frame.h"

#include <kiplatform/app.h>
#include <kiplatform/environment.h>
#include <auth/auth_manager.h>

#ifdef KICAD_IPC_API
#include <api/api_server.h>
#endif

// a dummy to quiet linking with EDA_BASE_FRAME::config();
#include <kiface_base.h>

#include <libraries/library_manager.h>

#if defined( __WXMSW__ )
// Forward declaration for URL protocol registration
void RegisterTraceProtocol( bool aSilent );
#endif


KIFACE_BASE& Kiface()
{
    // This function should never be called.  It is only referenced from
    // EDA_BASE_FRAME::config() and this is only provided to satisfy the linker,
    // not to be actually called.
    wxLogFatalError( wxT( "Unexpected call to Kiface() in kicad/kicad.cpp" ) );

    throw std::logic_error( "Unexpected call to Kiface() in kicad/kicad.cpp" );
}


static PGM_KICAD program;

PGM_KICAD& PgmTop()
{
    return program;
}


bool PGM_KICAD::OnPgmInit()
{
    Pgm().App().SetAppDisplayName( wxT( "Trace" ) );

#if defined(DEBUG)
    wxString absoluteArgv0 = wxStandardPaths::Get().GetExecutablePath();

    if( !wxIsAbsolutePath( absoluteArgv0 ) )
    {
        wxLogError( wxT( "No meaningful argv[0]" ) );
        return false;
    }
#endif


    // Initialize the git backend before trying to initialize individual programs
    SetGitBackend( new LIBGIT_BACKEND() );
    GetGitBackend()->Init();

    static const wxCmdLineEntryDesc desc[] = {
        { wxCMD_LINE_OPTION, "f", "frame", "Frame to load", wxCMD_LINE_VAL_STRING, 0 },
        { wxCMD_LINE_SWITCH, "n", "new", "New instance of KiCad, does not attempt to load previously open files",
          wxCMD_LINE_VAL_NONE, 0 },
#ifndef __WXOSX__
        { wxCMD_LINE_SWITCH, nullptr, "software-rendering", "Use software rendering instead of OpenGL",
          wxCMD_LINE_VAL_NONE, 0 },
#endif
        { wxCMD_LINE_PARAM, nullptr, nullptr, "File to load", wxCMD_LINE_VAL_STRING,
          wxCMD_LINE_PARAM_MULTIPLE | wxCMD_LINE_PARAM_OPTIONAL },
        { wxCMD_LINE_NONE, nullptr, nullptr, nullptr, wxCMD_LINE_VAL_NONE, 0 }
    };

    wxCmdLineParser parser( Pgm().App().argc, Pgm().App().argv );
    parser.SetDesc( desc );
    parser.Parse( false );

    FRAME_T appType = KICAD_MAIN_FRAME_T;

    const struct
    {
        wxString name;
        FRAME_T  type;
    } frameTypes[] = { { wxT( "pcb" ), FRAME_PCB_EDITOR },
                       { wxT( "fpedit" ), FRAME_FOOTPRINT_EDITOR },
                       { wxT( "sch" ), FRAME_SCH },
                       { wxT( "calc" ), FRAME_CALC },
                       { wxT( "bm2cmp" ), FRAME_BM2CMP },
                       { wxT( "ds" ), FRAME_PL_EDITOR },
                       { wxT( "gerb" ), FRAME_GERBER },
                       { wxT( "" ), FRAME_T_COUNT } };

    wxString frameName;

    if( parser.Found( "frame", &frameName ) )
    {
        appType = FRAME_T_COUNT;

        for( const auto& it : frameTypes )
        {
            if( it.name == frameName )
                appType = it.type;
        }

        if( appType == FRAME_T_COUNT )
        {
            wxLogError( wxT( "Unknown frame: %s" ), frameName );
            // Clean up
            OnPgmExit();
            return false;
        }
    }

    if( appType == KICAD_MAIN_FRAME_T )
    {
        Kiway.SetCtlBits( KFCTL_CPP_PROJECT_SUITE );
    }
    else
    {
        Kiway.SetCtlBits( KFCTL_STANDALONE );
    }

#ifndef __WXMAC__
    if( parser.Found( "software-rendering" ) )
    {
        wxSetEnv( "KICAD_SOFTWARE_RENDERING", "1" );
    }
#endif

    bool skipPythonInit = false;

    if( appType == FRAME_BM2CMP || appType == FRAME_PL_EDITOR || appType == FRAME_GERBER
        || appType == FRAME_CALC )
        skipPythonInit = true;

    if( !InitPgm( false, skipPythonInit ) )
        return false;

    // Set library environment variables at runtime using stock paths
    // This ensures paths are correct regardless of where Trace is installed
#ifdef _WIN32
    wxString symbolPath = PATHS::GetStockSymbolsPath();
    wxString footprintPath = PATHS::GetStockFootprintsPath();
    wxString model3dPath = PATHS::GetStock3dmodelsPath();
    
    // Only set if the directories exist (they won't exist in dev builds)
    if( wxDirExists( symbolPath ) )
        wxSetEnv( "KICAD9_SYMBOL_DIR", symbolPath );
    
    if( wxDirExists( footprintPath ) )
        wxSetEnv( "KICAD9_FOOTPRINT_DIR", footprintPath );
    
    if( wxDirExists( model3dPath ) )
        wxSetEnv( "KICAD9_3DMODEL_DIR", model3dPath );
#endif

    m_bm.InitSettings( new KICAD_SETTINGS );
    GetSettingsManager().RegisterSettings( PgmSettings() );
    GetSettingsManager().SetKiway( &Kiway );
    m_bm.Init();


    // Add search paths to feed the PGM_KICAD::SysSearch() function,
    // currently limited in support to only look for project templates
    {
        SEARCH_STACK bases;

        SystemDirsAppend( &bases );

        for( unsigned i = 0; i < bases.GetCount(); ++i )
        {
            wxFileName fn( bases[i], wxEmptyString );

            // Add Trace template file path to search path list.
            fn.AppendDir( wxT( "template" ) );

            // Only add path if exists and can be read by the user.
            if( fn.DirExists() && fn.IsDirReadable() )
                m_bm.m_search.AddPaths( fn.GetPath() );
        }

        // The versioned TEMPLATE_DIR takes precedence over the search stack template path.
        if( std::optional<wxString> v = ENV_VAR::GetVersionedEnvVarValue( GetLocalEnvVariables(),
                                                                          wxT( "TEMPLATE_DIR" ) ) )
        {
            if( !v->IsEmpty() )
                m_bm.m_search.Insert( *v, 0 );
        }

        // We've been adding system (installed default) search paths so far, now for user paths
        // The default user search path is inside KIPLATFORM::ENV::GetDocumentsPath()
        m_bm.m_search.Insert( PATHS::GetUserTemplatesPath(), 0 );

        // ...but the user can override that default with the TRACE_USER_TEMPLATE_DIR env var
        ENV_VAR_MAP_CITER it = GetLocalEnvVariables().find( "TRACE_USER_TEMPLATE_DIR" );

        if( it != GetLocalEnvVariables().end() && it->second.GetValue() != wxEmptyString )
            m_bm.m_search.Insert( it->second.GetValue(), 0 );
    }

    // Restore authentication session from keychain before creating any frames
    // This ensures the Account menu shows the correct state in all windows
    AUTH_MANAGER::Instance().TryRestoreSession();

    wxFrame*      frame = nullptr;
    KIWAY_PLAYER* playerFrame = nullptr;
    KICAD_MANAGER_FRAME* managerFrame = nullptr;

    if( appType == KICAD_MAIN_FRAME_T )
    {
        managerFrame = new KICAD_MANAGER_FRAME( nullptr, wxT( "Trace" ), wxDefaultPosition,
                                                wxWindow::FromDIP( wxSize( 775, -1 ), NULL ) );
        frame = managerFrame;

        STARTWIZARD startWizard;
        startWizard.CheckAndRun( frame );
    }
    else
    {
        // Use KIWAY to create a top window, which registers its existence also.
        // "TOP_FRAME" is a macro that is passed on compiler command line from CMake,
        // and is one of the types in FRAME_T.
        playerFrame = Kiway.Player( appType, true );
        frame = playerFrame;

        if( frame == nullptr )
        {
            return false;
        }
    }

    Pgm().App().SetTopWindow( frame );

    if( playerFrame )
        Pgm().App().SetAppDisplayName( playerFrame->GetAboutTitle() );

    Kiway.SetTop( frame );

    KIPLATFORM::ENV::SetAppDetailsForWindow( frame, '"' + wxStandardPaths::Get().GetExecutablePath() + '"' + " -n",
                                             frame->GetTitle() );

    KICAD_SETTINGS* settings = static_cast<KICAD_SETTINGS*>( PgmSettings() );

    GetLibraryManager().LoadGlobalTables();

#ifdef KICAD_IPC_API
    m_api_server = std::make_unique<KICAD_API_SERVER>();
    m_api_common_handler = std::make_unique<API_HANDLER_COMMON>();
    m_api_server->RegisterHandler( m_api_common_handler.get() );
#endif

    wxString projToLoad;

    HideSplash();

    if( playerFrame && parser.GetParamCount() )
    {
        // Now after the frame processing, the rest of the positional args are files
        std::vector<wxString> fileArgs;
        /*
            gerbview handles multiple project data files, i.e. gerber files on
            cmd line. Others currently do not, they handle only one. For common
            code simplicity we simply pass all the arguments in however, each
            program module can do with them what they want, ignore, complain
            whatever.  We don't establish policy here, as this is a multi-purpose
            launcher.
        */

        for( size_t i = 0; i < parser.GetParamCount(); i++ )
            fileArgs.push_back( parser.GetParam( i ) );

        // special attention to a single argument: argv[1] (==argSet[0])
        if( fileArgs.size() == 1 )
        {
            wxFileName argv1( fileArgs[0] );

#if defined( PGM_DATA_FILE_EXT )
            // PGM_DATA_FILE_EXT, if present, may be different for each compile,
            // it may come from CMake on the compiler command line, but often does not.
            // This facility is mostly useful for those program footprints
            // supporting a single argv[1].
            if( !argv1.GetExt() )
                argv1.SetExt( wxT( PGM_DATA_FILE_EXT ) );
#endif
            argv1.MakeAbsolute();

            fileArgs[0] = argv1.GetFullPath();
        }

        // Use the KIWAY_PLAYER::OpenProjectFiles() API function:
        if( !playerFrame->OpenProjectFiles( fileArgs ) )
        {
            // OpenProjectFiles() API asks that it report failure to the UI.
            // Nothing further to say here.

            // We've already initialized things at this point, but wx won't call OnExit if
            // we fail out. Call our own cleanup routine here to ensure the relevant resources
            // are freed at the right time (if they aren't, segfaults will occur).
            OnPgmExit();

            // Fail the process startup if the file could not be opened,
            // although this is an optional choice, one that can be reversed
            // also in the KIFACE specific OpenProjectFiles() return value.
            return false;
        }
    }
    else if( managerFrame )
    {
        if( parser.GetParamCount() > 0 )
        {
            wxString param0 = parser.GetParam( 0 );
            
            // Skip trace:// URLs - these are auth callbacks, not project files
            if( !param0.StartsWith( wxT( "trace://" ) ) )
            {
                wxFileName tmp( param0 );

            if( tmp.GetExt() != FILEEXT::ProjectFileExtension && tmp.GetExt() != FILEEXT::LegacyProjectFileExtension )
            {
                DisplayErrorMessage( nullptr, wxString::Format( _( "File '%s'\n"
                                                                   "does not appear to be a KiCad project file." ),
                                                                tmp.GetFullPath() ) );
            }
            else
            {
                projToLoad = tmp.GetFullPath();
                }
            }
        }

        // If no file was given as an argument, check that there was a file open.
        // But skip loading previous projects if this is an auth callback instance
        bool isAuthCallback = false;
        wxString authCallbackUrl;
        if( wxGetEnv( wxT( "TRACE_AUTH_CALLBACK_URL" ), &authCallbackUrl ) )
            isAuthCallback = true;
            
        if( projToLoad.IsEmpty() && settings->m_OpenProjects.size() && !parser.FoundSwitch( "new" ) && !isAuthCallback )
        {
            wxString last_pro = settings->m_OpenProjects.front();
            settings->m_OpenProjects.erase( settings->m_OpenProjects.begin() );

            if( wxFileExists( last_pro ) )
            {
                // Try to open the last opened project,
                // if a project name is not given when starting Kicad
                projToLoad = last_pro;
            }
        }

        // Do not attempt to load a non-existent project file.
        if( !projToLoad.empty() )
        {
            wxFileName fn( projToLoad );

            if( fn.Exists() && (   fn.GetExt() == FILEEXT::ProjectFileExtension
                                || fn.GetExt() == FILEEXT::LegacyProjectFileExtension ) )
            {
                fn.MakeAbsolute();

                if( appType == KICAD_MAIN_FRAME_T )
                    managerFrame->LoadProject( fn );
            }
        }
    }

    frame->Show( true );
    frame->Raise();

#ifdef KICAD_IPC_API
    m_api_server->SetReadyToReply();
#endif

    return true;
}


int PGM_KICAD::OnPgmRun()
{
    return 0;
}


void PGM_KICAD::OnPgmExit()
{
    // Abort and wait on any background jobs
    GetKiCadThreadPool().purge();
    GetKiCadThreadPool().wait();

    Kiway.OnKiwayEnd();

#ifdef KICAD_IPC_API
    m_api_server.reset();
#endif

    if( m_settings_manager && m_settings_manager->IsOK() )
    {
        SaveCommonSettings();
        m_settings_manager->Save();
    }

    // Destroy everything in PGM_KICAD,
    // especially wxSingleInstanceCheckerImpl earlier than wxApp and earlier
    // than static destruction would.
    Destroy();
    GetGitBackend()->Shutdown();
    delete GetGitBackend();
    SetGitBackend( nullptr );
}


void PGM_KICAD::MacOpenFile( const wxString& aFileName )
{
#if defined(__WXMAC__)

    KICAD_MANAGER_FRAME* frame = (KICAD_MANAGER_FRAME*) Pgm().App().GetTopWindow();

    if( !aFileName.empty() && wxFileExists( aFileName ) )
        frame->LoadProject( wxFileName( aFileName ) );

#endif
}


void PGM_KICAD::Destroy()
{
    // unlike a normal destructor, this is designed to be called more
    // than once safely:

    m_bm.End();

    PGM_BASE::Destroy();
}


KIWAY  Kiway( KFCTL_CPP_PROJECT_SUITE );

#ifdef NDEBUG
// Define a custom assertion handler
void CustomAssertHandler(const wxString& file,
                         int line,
                         const wxString& func,
                         const wxString& cond,
                         const wxString& msg)
{
    Pgm().HandleAssert( file, line, func, cond, msg );
}
#endif

/**
 * Not publicly visible because most of the action is in #PGM_KICAD these days.
 */
struct APP_KICAD : public wxApp
{
    APP_KICAD() : wxApp()
    {
        SetPgm( &program );

        // Init the environment each platform wants
        KIPLATFORM::ENV::Init();
    }


    bool OnInit()           override
    {
#if defined( __WXMSW__ )
        // CRITICAL: Handle URL protocol registration FIRST, before any initialization
        // This must be at the absolute start before any Python or platform init
        if( this->argc >= 2 )
        {
            wxString arg1 = this->argv[1];
            if( arg1 == wxT( "--register-protocol" ) )
            {
                RegisterTraceProtocol( false ); // With GUI dialogs
                return false; // Exit immediately
            }
            else if( arg1 == wxT( "--register-protocol-silent" ) )
            {
                RegisterTraceProtocol( true );  // Silent for CMake install
                return false; // Exit immediately
            }
        }

        // CRITICAL: Clear Python environment variables BEFORE any initialization
        // This prevents conflicts between system Python and vcpkg Python
        wxSetEnv( wxT("PYTHONHOME"), wxEmptyString );
        wxSetEnv( wxT("PYTHONPATH"), wxEmptyString );
        
        // Windows: Set library paths for AI/Python conversion
        // Check if running from build or install directory and set paths accordingly
        #if defined(TRACE_WIN_SYMBOL_PATH_BUILD) && defined(TRACE_WIN_SYMBOL_PATH_INSTALL)
        {
            wxFileName exePath( wxStandardPaths::Get().GetExecutablePath() );
            wxString exeDir = exePath.GetPath();
            
            // Determine if running from build or install directory
            // Build directory contains pattern like "build/msvc-win64-release/kicad/trace.exe"
            // Install directory contains pattern like "build/install/msvc-win64-release/bin/trace.exe"
            bool isInstallDir = exeDir.Contains( wxT("install") ) || 
                               !exeDir.Contains( wxT("build") );
            
            wxString symbolPath, footprintPath, modelPath;
            
            if( isInstallDir )
            {
                // Running from install directory
                symbolPath = wxT(TRACE_WIN_SYMBOL_PATH_INSTALL);
                footprintPath = wxT(TRACE_WIN_FOOTPRINT_PATH_INSTALL);
                modelPath = wxT(TRACE_WIN_3DMODEL_PATH_INSTALL);
            }
            else
            {
                // Running from build directory  
                symbolPath = wxT(TRACE_WIN_SYMBOL_PATH_BUILD);
                footprintPath = wxT(TRACE_WIN_FOOTPRINT_PATH_BUILD);
                modelPath = wxT(TRACE_WIN_3DMODEL_PATH_BUILD);
            }
            
            // Set environment variables for Python scripts to use
            wxSetEnv( wxT("KICAD9_SYMBOL_DIR"), symbolPath );
            wxSetEnv( wxT("KICAD9_FOOTPRINT_DIR"), footprintPath );
            wxSetEnv( wxT("KICAD9_3DMODEL_DIR"), modelPath );
        }
        #endif
#endif

#ifdef NDEBUG
        // These checks generate extra assert noise
        wxSizerFlags::DisableConsistencyChecks();
        wxDISABLE_DEBUG_SUPPORT();
        wxSetAssertHandler( CustomAssertHandler );
#endif

        // Perform platform-specific init tasks
        if( !KIPLATFORM::APP::Init() )
            return false;

#ifndef DEBUG
        // Enable logging traces to the console in release build.
        // This is usually disabled, but it can be useful for users to run to help
        // debug issues and other problems.
        if( wxGetEnv( wxS( "KICAD_ENABLE_WXTRACE" ), nullptr ) )
        {
            wxLog::EnableLogging( true );
            wxLog::SetLogLevel( wxLOG_Trace );
        }
#endif

#if defined( __WXMSW__ )
        // On Windows, check if we were launched with a trace:// URL (auth callback)
        for( int i = 1; i < this->argc; ++i )
        {
            wxString arg = this->argv[i];
            if( arg.StartsWith( wxT( "trace://" ) ) )
            {
                // This is an auth callback - handle it and notify any existing instance
                // For now, just set a flag so we can handle it after init
                wxSetEnv( wxT( "TRACE_AUTH_CALLBACK_URL" ), arg );
                break;
            }
        }
#endif

        if( !program.OnPgmInit() )
        {
            program.OnPgmExit();
            return false;
        }

#if defined( __WXMSW__ )
        // Handle any pending auth callback
        wxString authCallbackUrl;
        if( wxGetEnv( wxT( "TRACE_AUTH_CALLBACK_URL" ), &authCallbackUrl ) )
        {
            wxUnsetEnv( wxT( "TRACE_AUTH_CALLBACK_URL" ) );
            AUTH_MANAGER::Instance().HandleURLCallback( authCallbackUrl );
            
            // Auth callback instance should exit immediately after handling the callback
            // This prevents duplicate windows and ensures the main instance detects the change
            return false;  // Exit the callback instance
        }
#endif

        return true;
    }

    int  OnExit()           override
    {
        program.OnPgmExit();

        // Avoid wxLog crashing when used in destructors.
        wxLog::EnableLogging( false );

        return wxApp::OnExit();
    }

    int OnRun()             override
    {
        try
        {
            return wxApp::OnRun();
        }
        catch(...)
        {
            Pgm().HandleException( std::current_exception() );
        }

        return -1;
    }

    int FilterEvent( wxEvent& aEvent ) override
    {
        if( aEvent.GetEventType() == wxEVT_SHOW )
        {
            wxShowEvent& event = static_cast<wxShowEvent&>( aEvent );
            wxDialog*    dialog = dynamic_cast<wxDialog*>( event.GetEventObject() );

            std::vector<void*>& dlgs = Pgm().m_ModalDialogs;

            if( dialog )
            {
                if( event.IsShown() && dialog->IsModal() )
                {
                    dlgs.push_back( dialog );
                }
                // Under GTK, sometimes the modal flag is cleared before hiding
                else if( !event.IsShown() && !dlgs.empty() )
                {
                    // If we close the expected dialog, remove it from our stack
                    if( dlgs.back() == dialog )
                        dlgs.pop_back();
                    // If an out-of-order, remove all dialogs added after the closed one
                    else if( auto it = std::find( dlgs.begin(), dlgs.end(), dialog ) ; it != dlgs.end() )
                        dlgs.erase( it, dlgs.end() );
                }
            }
        }

        return Event_Skip;
    }

#if defined( DEBUG )
    /**
     * Process any unhandled events at the application level.
     */
    bool ProcessEvent( wxEvent& aEvent ) override
    {
        if( aEvent.GetEventType() == wxEVT_CHAR || aEvent.GetEventType() == wxEVT_CHAR_HOOK )
        {
            wxKeyEvent* keyEvent = static_cast<wxKeyEvent*>( &aEvent );

            if( keyEvent )
            {
                wxLogTrace( kicadTraceKeyEvent, "APP_KICAD::ProcessEvent %s", dump( *keyEvent ) );
            }
        }

        aEvent.Skip();
        return false;
    }

    /**
     * Override main loop exception handling on debug builds.
     *
     * It can be painfully difficult to debug exceptions that happen in wxUpdateUIEvent
     * handlers.  The override provides a bit more useful information about the exception
     * and a breakpoint can be set to pin point the event where the exception was thrown.
     */
    bool OnExceptionInMainLoop() override
    {
        try
        {
            throw;
        }
        catch(...)
        {
            Pgm().HandleException( std::current_exception() );
        }

        return false;   // continue on. Return false to abort program
    }
#endif

    /**
     * Set MacOS file associations.
     *
     * @see http://wiki.wxwidgets.org/WxMac-specific_topics
     */
#if defined( __WXMAC__ )
    void MacOpenFile( const wxString& aFileName ) override
    {
        Pgm().MacOpenFile( aFileName );
    }
    
    /**
     * Handle custom URL scheme callbacks (trace://auth).
     * Used for OAuth authentication flow on macOS.
     */
    void MacOpenURL( const wxString& aURL ) override
    {
        wxLogDebug( wxT( "MacOpenURL called with: %s" ), aURL );
        
        if( aURL.StartsWith( wxT( "trace://auth" ) ) )
        {
            // Handle authentication callback
            bool result = AUTH_MANAGER::Instance().HandleURLCallback( aURL );
            wxLogDebug( wxT( "HandleURLCallback returned: %d" ), result );
        }
    }
#endif
};


#if defined( __WXMSW__ )
#include <windows.h>

/**
 * Register the trace:// URL protocol for Windows authentication callbacks.
 * Uses Windows Registry API directly - no elevation needed for HKEY_CURRENT_USER.
 * 
 * @param aSilent If true, suppress GUI dialogs (for automated installation)
 */
void RegisterTraceProtocol( bool aSilent )
{
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    wxString command = wxString::Format( wxT("\"%s\" \"%%1\""), exePath );
    
    HKEY hKey = NULL;
    LONG result;
    bool success = true;
    
    // Create trace protocol key
    result = RegCreateKeyExW( HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\trace", 
                              0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL );
    if( result == ERROR_SUCCESS )
    {
        const wchar_t* urlProtocol = L"URL:Trace Protocol";
        RegSetValueExW( hKey, NULL, 0, REG_SZ, (const BYTE*)urlProtocol, 
                       (wcslen(urlProtocol) + 1) * sizeof(wchar_t) );
        RegSetValueExW( hKey, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t) );
        RegCloseKey( hKey );
    }
    else
    {
        success = false;
    }
    
    // Create shell\open\command key
    result = RegCreateKeyExW( HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\trace\\shell\\open\\command",
                              0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL );
    if( result == ERROR_SUCCESS )
    {
        std::wstring cmdWide = command.ToStdWstring();
        RegSetValueExW( hKey, NULL, 0, REG_SZ, (const BYTE*)cmdWide.c_str(), 
                       (cmdWide.length() + 1) * sizeof(wchar_t) );
        RegCloseKey( hKey );
    }
    else
    {
        success = false;
    }
    
    // Provide feedback only if not silent
    if( !aSilent )
    {
        if( success )
        {
            wxMessageBox( wxT("Trace URL protocol registered successfully.\n")
                         wxT("Authentication callbacks will now work properly."),
                         wxT("Registration Complete"), wxOK | wxICON_INFORMATION );
        }
        else
        {
            wxMessageBox( wxT("Failed to register Trace URL protocol.\n")
                         wxT("You may need to run as administrator."),
                         wxT("Registration Failed"), wxOK | wxICON_WARNING );
        }
    }
}
#endif


IMPLEMENT_APP( APP_KICAD )


// The C++ project manager supports one open PROJECT, so Prj() calls within
// this link image need this function.
PROJECT& Prj()
{
    return Kiway.Prj();
}

