/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2013 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2013 Wayne Stambaugh <stambaughw@gmail.com>
 * Copyright (C) 2013-2023 CERN (www.cern.ch)
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


#include <confirm.h>
#include <common.h>
#include <connection_graph.h>
#include <dialog_migrate_buses.h>
#include <dialog_symbol_remap.h>
#include <dialog_import_choose_project.h>
#include <eeschema_settings.h>
#include <id.h>
#include <kiface_base.h>
#include <kiplatform/app.h>
#include <kiplatform/ui.h>
#include <libraries/legacy_symbol_library.h>
#include <libraries/symbol_library_adapter.h>
#include <local_history.h>
#include <amplitude_client.h>
#include <sch_symbol.h>
#include <set>
#include <lockfile.h>
#include <pgm_base.h>
#include <core/profile.h>
#include <project/project_file.h>
#include <project_rescue.h>
#include <project_sch.h>
#include <dialog_HTML_reporter_base.h>
#include <io/common/plugin_common_choose_project.h>
#include <reporter.h>
#include <richio.h>
#include <sch_bus_entry.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_io/kicad_legacy/sch_io_kicad_legacy.h>
#include <sch_file_versions.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <sim/simulator_frame.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_navigate_tool.h>
#include <trace_helpers.h>
#include <widgets/filedlg_import_non_kicad.h>
#include <widgets/kistatusbar.h>
#include <widgets/wx_infobar.h>
#include <wildcards_and_files_ext.h>
#include <local_history.h>
#include <drawing_sheet/ds_data_model.h>
#include <wx/app.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/log.h>
#include <wx/richmsgdlg.h>
#include <wx/stdpaths.h>
#include <tools/sch_inspection_tool.h>
#include <tools/sch_selection_tool.h>
#include <paths.h>
#include <wx_filename.h>  // For ::ResolvePossibleSymlinks
#include <widgets/wx_progress_reporters.h>
#include <widgets/wx_html_report_box.h>
#include <gestfich.h>

#include <kiplatform/io.h>

#include "widgets/filedlg_hook_save_project.h"
#include "widgets/panel_remote_symbol.h"
#include "save_project_utils.h"

bool SCH_EDIT_FRAME::OpenProjectFiles( const std::vector<wxString>& aFileSet, int aCtl )
{
    // ensure the splash screen does not obscure any dialog at startup
    Pgm().HideSplash();

    // implement the pseudo code from KIWAY_PLAYER.h:
    wxString msg;

    EESCHEMA_SETTINGS* cfg = dynamic_cast<EESCHEMA_SETTINGS*>( Kiface().KifaceSettings() );

    // This is for python:
    if( aFileSet.size() != 1 )
    {
        msg.Printf( "Eeschema:%s() takes only a single filename.", __WXFUNCTION__ );
        DisplayError( this, msg );
        return false;
    }

    wxString   fullFileName( aFileSet[0] );
    wxFileName wx_filename( fullFileName );
    
    // Convert trace_sch files to kicad_sch before processing
    if( wx_filename.GetExt() == FILEEXT::TraceSchematicFileExtension )
    {
        wx_filename.SetExt( FILEEXT::KiCadSchematicFileExtension );
        fullFileName = wx_filename.GetFullPath();
        wx_filename = wxFileName( fullFileName );
    }
    
    Kiway().LocalHistory().Init( wx_filename.GetPath() );

    // We insist on caller sending us an absolute path, if it does not, we say it's a bug.
    wxASSERT_MSG( wx_filename.IsAbsolute(), wxS( "Path is not absolute!" ) );

    if( !LockFile( fullFileName ) )
    {
        // If project-level lock override was already granted, silently override this file's lock
        if( Prj().IsLockOverrideGranted() )
        {
            m_file_checker->OverrideLock();
        }
        else
        {
            msg.Printf( _( "Schematic '%s' is already open by '%s' at '%s'." ), fullFileName,
                    m_file_checker->GetUsername(), m_file_checker->GetHostname() );

            if( !AskOverrideLock( this, msg ) )
                return false;

            m_file_checker->OverrideLock();
        }
    }

    if( !AskToSaveChanges() )
        return false;

#ifdef PROFILE
    PROF_TIMER openFiles( "OpenProjectFile" );
#endif

    wxFileName pro = fullFileName;
    pro.SetExt( FILEEXT::ProjectFileExtension );

    // If kicad_sch doesn't exist but trace_sch does, convert it automatically
    wxFileName fullFile( fullFileName );
    if( !wxFileName::IsFileReadable( fullFileName ) && 
        fullFile.GetExt() == FILEEXT::KiCadSchematicFileExtension )
    {
        wxFileName traceSchFile( fullFile );
        traceSchFile.SetExt( FILEEXT::TraceSchematicFileExtension );
        wxString traceSchPath = traceSchFile.GetFullPath();
        
        if( wxFileName::FileExists( traceSchPath ) )
        {
            // Convert trace_sch to kicad_sch automatically
            if( ConvertTraceSchToKicadSch( traceSchPath ) )
            {
                wxLogMessage( wxT( "Automatically converted %s to %s" ), traceSchPath, fullFileName );
            }
            else
            {
                wxLogWarning( wxT( "Failed to convert %s to %s" ), traceSchPath, fullFileName );
            }
        }
    }

    bool is_new = !wxFileName::IsFileReadable( fullFileName );

    // If its a non-existent schematic, automatically create it (no dialog)
    // This allows opening the schematic editor to always work, creating a new schematic if needed
    if( is_new && !( aCtl & KICTL_CREATE ) )
    {
        // Automatically create new schematic without asking
        // This is the expected behavior when opening the schematic editor
        // The file will be created as an empty schematic below
    }

    wxCommandEvent e( EDA_EVT_SCHEMATIC_CHANGING );
    ProcessEventLocally( e );

    // unload current project file before loading new
    {
        ClearUndoRedoList();
        ClearRepeatItemsList();
        SetScreen( nullptr );
        m_toolManager->GetTool<SCH_INSPECTION_TOOL>()->Reset( TOOL_BASE::SUPERMODEL_RELOAD );
    }

    SetStatusText( wxEmptyString );
    m_infoBar->Dismiss();

    if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( GetStatusBar() ) )
        statusBar->ClearLoadWarningMessages();

    WX_PROGRESS_REPORTER progressReporter( this, is_new ? _( "Create Schematic" )
                                                        : _( "Load Schematic" ), 1,
                                           PR_CAN_ABORT );
    WX_STRING_REPORTER loadReporter;
    LOAD_INFO_REPORTER_SCOPE loadReporterScope( &loadReporter );

    bool differentProject = pro.GetFullPath() != Prj().GetProjectFullName();

    // This is for handling standalone mode schematic changes
    if( differentProject )
    {
        if( !Prj().IsNullProject() )
        {
            SaveProjectLocalSettings();
            GetSettingsManager()->SaveProject();
        }

        // disconnect existing project from schematic before we unload the project
        Schematic().SetProject( nullptr );
        GetSettingsManager()->UnloadProject( &Prj(), false );

        GetSettingsManager()->LoadProject( pro.GetFullPath() );

        wxFileName legacyPro( pro );
        legacyPro.SetExt( FILEEXT::LegacyProjectFileExtension );

        // Do not allow saving a project if one doesn't exist.  This normally happens if we are
        // standalone and opening a schematic that has been moved from its project folder.
        if( !pro.Exists() && !legacyPro.Exists() && !( aCtl & KICTL_CREATE ) )
            Prj().SetReadOnly();
    }

    // Start a new schematic object now that we sorted out our project
    std::unique_ptr<SCHEMATIC> newSchematic = std::make_unique<SCHEMATIC>( &Prj() );

    SCH_IO_MGR::SCH_FILE_T schFileType = SCH_IO_MGR::GuessPluginTypeFromSchPath( fullFileName,
                                                                                 KICTL_KICAD_ONLY );

    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
    {
        // Don't reload the symbol libraries if we are just launching Eeschema from KiCad again.
        // They are already saved in the kiface project object.
        if( differentProject || !Prj().GetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS ) )
        {
            // load the libraries here, not in SCH_SCREEN::Draw() which is a context
            // that will not tolerate DisplayError() dialog since we're already in an
            // event handler in there.
            // And when a schematic file is loaded, we need these libs to initialize
            // some parameters (links to PART LIB, dangling ends ...)
            Prj().SetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS, nullptr );
            PROJECT_SCH::LegacySchLibs( &Prj() );
        }
    }
    else
    {
        // No legacy symbol libraries including the cache are loaded with the new file format.
        Prj().SetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS, nullptr );
    }

    wxFileName rfn( GetCurrentFileName() );
    rfn.MakeRelativeTo( Prj().GetProjectPath() );
    LoadWindowState( rfn.GetFullPath() );

    KIPLATFORM::APP::SetShutdownBlockReason( this, _( "Schematic file changes are unsaved" ) );

    if( Kiface().IsSingle() )
    {
        // Don't register untitled files for app restart - they should not persist across sessions
        // This gives Word-like behavior where untitled documents are ephemeral
        wxFileName fn( fullFileName );
        wxString nameLower = fn.GetName().Lower();
        if( !nameLower.StartsWith( "untitled" ) )
        {
        KIPLATFORM::APP::RegisterApplicationRestart( fullFileName );
        }
    }

    if( is_new || schFileType == SCH_IO_MGR::SCH_FILE_T::SCH_FILE_UNKNOWN )
    {
        newSchematic->CreateDefaultScreens();
        SetSchematic( newSchematic.release() );

        // mark new, unsaved file as modified.
        GetScreen()->SetContentModified();
        GetScreen()->SetFileName( fullFileName );

        if( schFileType == SCH_IO_MGR::SCH_FILE_T::SCH_FILE_UNKNOWN )
        {
            msg.Printf( _( "'%s' is not a KiCad schematic file.\nUse File -> Import for "
                           "non-KiCad schematic files." ),
                        fullFileName );

            progressReporter.Hide();
            DisplayErrorMessage( this, msg );
        }
    }
    else
    {
        SetScreen( nullptr );

        IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( schFileType ) );

        pi->SetProgressReporter( &progressReporter );

        bool failedLoad = false;

        try
        {
            {
                wxBusyCursor    busy;
                WINDOW_DISABLER raii( this );

                // Check if project file has top-level sheets defined
                PROJECT_FILE& projectFile = Prj().GetProjectFile();
                const std::vector<TOP_LEVEL_SHEET_INFO>& topLevelSheets = projectFile.GetTopLevelSheets();

                if( !topLevelSheets.empty() )
                {
                    std::vector<SCH_SHEET*> loadedSheets;

                    // Load each top-level sheet
                    for( const TOP_LEVEL_SHEET_INFO& sheetInfo : topLevelSheets )
                    {
                        wxFileName sheetFileName( Prj().GetProjectPath(), sheetInfo.filename );

                        // When loading legacy schematic files, ensure we are referencing the correct extension
                        if( schFileType == SCH_IO_MGR::SCH_LEGACY )
                            sheetFileName.SetExt( FILEEXT::LegacySchematicFileExtension );

                        // Redirect .trace_sch references to .kicad_sch
                        if( sheetFileName.GetExt() == FILEEXT::TraceSchematicFileExtension )
                            sheetFileName.SetExt( FILEEXT::KiCadSchematicFileExtension );

                        wxString sheetPath = sheetFileName.GetFullPath();

                        // If kicad_sch doesn't exist but trace_sch does, convert it
                        if( !wxFileName::FileExists( sheetPath ) && 
                            sheetFileName.GetExt() == FILEEXT::KiCadSchematicFileExtension )
                        {
                            wxFileName traceSchFile( sheetFileName );
                            traceSchFile.SetExt( FILEEXT::TraceSchematicFileExtension );
                            wxString traceSchPath = traceSchFile.GetFullPath();
                            
                            if( wxFileName::FileExists( traceSchPath ) )
                            {
                                // Convert trace_sch to kicad_sch
                                if( ConvertTraceSchToKicadSch( traceSchPath ) )
                                {
                                    wxLogMessage( wxT( "Converted %s to %s" ), traceSchPath, sheetPath );
                                }
                                else
                                {
                                    wxLogWarning( wxT( "Failed to convert %s to %s" ), traceSchPath, sheetPath );
                                    continue;
                                }
                            }
                        }

                        if( !wxFileName::FileExists( sheetPath ) )
                        {
                            wxLogWarning( wxT( "Top-level sheet file not found: %s" ), sheetPath );
                            continue;
                        }

                        SCH_SHEET* sheet = pi->LoadSchematicFile( sheetPath, newSchematic.get() );

                        if( sheet )
                        {
                            // Don't override the UUID from the project file - the file's UUID is
                            // authoritative because subsheet instance paths reference it.
                            // The project file will be updated on save to match the file's UUID.
                            // This prevents page number lookup failures when the project file
                            // has a stale UUID.

                            sheet->SetName( sheetInfo.name );
                            loadedSheets.push_back( sheet );

                            wxLogTrace( tracePathsAndFiles,
                                       wxS( "Loaded top-level sheet '%s' (UUID %s) from %s" ),
                                       sheet->GetName(),
                                       sheet->m_Uuid.AsString(),
                                       sheetPath );
                        }
                    }

                    if( !loadedSheets.empty() )
                    {
                        newSchematic->SetTopLevelSheets( loadedSheets );
                    }
                    else
                    {
                        wxLogTrace( tracePathsAndFiles,
                                   wxS( "Loaded multi-root schematic with no top-level sheets!" ) );
                        newSchematic->CreateDefaultScreens();
                    }
                }
                else
                {
                    // Legacy single-root format: Load the single root sheet
                    SCH_SHEET* rootSheet = pi->LoadSchematicFile( fullFileName, newSchematic.get() );

                    if( rootSheet )
                    {
                        newSchematic->SetTopLevelSheets( { rootSheet } );

                        // Make ${SHEETNAME} work on the root sheet until we properly support
                        // naming the root sheet
                        if( SCH_SHEET* topSheet = newSchematic->GetTopLevelSheet() )
                            topSheet->SetName( _( "Root" ) );

                        wxLogTrace( tracePathsAndFiles,
                                   wxS( "Loaded schematic with root sheet UUID %s" ),
                                   rootSheet->m_Uuid.AsString() );
                        wxLogTrace( traceSchCurrentSheet,
                                   "After loading: Current sheet path='%s', size=%zu, empty=%d",
                                   newSchematic->CurrentSheet().Path().AsString(),
                                   newSchematic->CurrentSheet().size(),
                                   newSchematic->CurrentSheet().empty() ? 1 : 0 );
                    }
                    else
                    {
                        newSchematic->CreateDefaultScreens();
                    }

                }
            }

            if( !pi->GetError().IsEmpty() )
            {
                DisplayErrorMessage( this, _( "The entire schematic could not be loaded.  Errors "
                                              "occurred attempting to load hierarchical sheets." ),
                                     pi->GetError() );
            }
        }
        catch( const FUTURE_FORMAT_ERROR& ffe )
        {
            newSchematic->CreateDefaultScreens();
            msg.Printf( _( "Error loading schematic '%s'." ), fullFileName );
            progressReporter.Hide();
            DisplayErrorMessage( this, msg, ffe.Problem() );

            failedLoad = true;
        }
        catch( const IO_ERROR& ioe )
        {
            newSchematic->CreateDefaultScreens();
            msg.Printf( _( "Error loading schematic '%s'." ), fullFileName );
            progressReporter.Hide();
            DisplayErrorMessage( this, msg, ioe.What() );

            failedLoad = true;
        }
        catch( const std::bad_alloc& )
        {
            newSchematic->CreateDefaultScreens();
            msg.Printf( _( "Memory exhausted loading schematic '%s'." ), fullFileName );
            progressReporter.Hide();
            DisplayErrorMessage( this, msg, wxEmptyString );

            failedLoad = true;
        }

        SetSchematic( newSchematic.release() );

        // This fixes a focus issue after the progress reporter is done on GTK.  It shouldn't
        // cause any issues on macOS and Windows.  If it does, it will have to be conditionally
        // compiled.
        Raise();

        if( failedLoad )
        {
            // Do not leave g_RootSheet == NULL because it is expected to be
            // a valid sheet. Therefore create a dummy empty root sheet and screen.
            CreateDefaultScreens();
            m_toolManager->RunAction( ACTIONS::zoomFitScreen );

            // Show any messages collected before the failure
            if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( GetStatusBar() ) )
                statusBar->SetLoadWarningMessages( loadReporter.GetMessages() );

            msg.Printf( _( "Failed to load '%s'." ), fullFileName );
            SetMsgPanel( wxEmptyString, msg );

            return false;
        }

        // Load project settings after schematic has been set up with the project link, since this will
        // update some of the needed schematic settings such as drawing defaults
        LoadProjectSettings();

        // It's possible the schematic parser fixed errors due to bugs so warn the user
        // that the schematic has been fixed (modified).
        SCH_SHEET_LIST sheetList = Schematic().Hierarchy();

        if( sheetList.IsModified() )
        {
            DisplayInfoMessage( this,
                                _( "An error was found when loading the schematic that has "
                                   "been automatically fixed.  Please save the schematic to "
                                   "repair the broken file or it may not be usable with other "
                                   "versions of KiCad." ) );
        }

        if( sheetList.AllSheetPageNumbersEmpty() )
            sheetList.SetInitialPageNumbers();

        UpdateFileHistory( fullFileName );

        if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( GetStatusBar() ) )
            statusBar->SetLoadWarningMessages( loadReporter.GetMessages() );

        SCH_SCREENS schematic( Schematic().Root() );

        // LIB_ID checks and symbol rescue only apply to the legacy file formats.
        if( schFileType == SCH_IO_MGR::SCH_LEGACY )
        {
            // Convert any legacy bus-bus entries to just be bus wires
            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
            {
                std::vector<SCH_ITEM*> deleted;

                for( SCH_ITEM* item : screen->Items() )
                {
                    if( item->Type() == SCH_BUS_BUS_ENTRY_T )
                    {
                        SCH_BUS_BUS_ENTRY* entry = static_cast<SCH_BUS_BUS_ENTRY*>( item );
                        std::unique_ptr<SCH_LINE> wire = std::make_unique<SCH_LINE>();

                        wire->SetLayer( LAYER_BUS );
                        wire->SetStartPoint( entry->GetPosition() );
                        wire->SetEndPoint( entry->GetEnd() );

                        screen->Append( wire.release() );
                        deleted.push_back( item );
                    }
                }

                for( SCH_ITEM* item : deleted )
                    screen->Remove( item );
            }


            // Convert old projects over to use symbol library table.
            if( schematic.HasNoFullyDefinedLibIds() )
            {
                DIALOG_SYMBOL_REMAP dlgRemap( this );

                dlgRemap.ShowQuasiModal();
            }
            else
            {
                // Double check to ensure no legacy library list entries have been
                // added to the project file symbol library list.
                wxString paths;
                wxArrayString libNames;

                LEGACY_SYMBOL_LIBS::GetLibNamesAndPaths( &Prj(), &paths, &libNames );

                if( !libNames.IsEmpty() )
                {
                    if( eeconfig()->m_Appearance.show_illegal_symbol_lib_dialog )
                    {
                        wxRichMessageDialog invalidLibDlg(
                                this,
                                _( "Illegal entry found in project file symbol library list." ),
                                _( "Project Load Warning" ),
                                wxOK | wxCENTER | wxICON_EXCLAMATION );
                        invalidLibDlg.ShowDetailedText(
                                _( "Symbol libraries defined in the project file symbol library "
                                   "list are no longer supported and will be removed.\n\n"
                                   "This may cause broken symbol library links under certain "
                                   "conditions." ) );
                        invalidLibDlg.ShowCheckBox( _( "Do not show this dialog again." ) );
                        invalidLibDlg.ShowModal();
                        eeconfig()->m_Appearance.show_illegal_symbol_lib_dialog =
                                !invalidLibDlg.IsCheckBoxChecked();
                    }

                    libNames.Clear();
                    paths.Clear();
                    LEGACY_SYMBOL_LIBS::SetLibNamesAndPaths( &Prj(), paths, libNames );
                }

                // Check for cache file
                wxFileName cacheFn( fullFileName );
                cacheFn.SetName( cacheFn.GetName() + "-cache" );
                cacheFn.SetExt( FILEEXT::LegacySymbolLibFileExtension );
                bool cacheExists = cacheFn.FileExists();

                if( cacheExists )
                {
                    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &Prj() );
                    std::optional<LIBRARY_TABLE*> table = adapter->ProjectTable();

                    if( table && *table )
                    {
                        wxString nickname = Prj().GetProjectName() + "-cache";

                        if( !(*table)->HasRow( nickname ) )
                        {
                            LIBRARY_TABLE_ROW& row = (*table)->InsertRow();
                            row.SetNickname( nickname );
                            row.SetURI( cacheFn.GetFullPath() );
                            row.SetType( SCH_IO_MGR::ShowType( SCH_IO_MGR::SCH_LEGACY ) );
                            row.SetDescription( _( "Legacy project cache library" ) );
                            (*table)->Save();
                        }

                        std::vector<wxString> cacheSymbols = adapter->GetSymbolNames( nickname );
                        std::set<wxString> cacheSymbolSet( cacheSymbols.begin(), cacheSymbols.end() );

                        if( !cacheSymbolSet.empty() )
                        {
                            std::vector<wxString> loadedLibs;

                            for( const wxString& libName : adapter->GetLibraryNames() )
                            {
                                if( libName == nickname )
                                    continue;

                                std::optional<LIB_STATUS> status = adapter->GetLibraryStatus( libName );

                                if( status && status->load_status == LOAD_STATUS::LOADED )
                                    loadedLibs.push_back( libName );
                            }

                            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
                            {
                                for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
                                {
                                    SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
                                    LIB_ID newId = symbol->GetLibId();
                                    UTF8 fullLibName = newId.Format();

                                    if( cacheSymbolSet.count( fullLibName.wx_str() ) )
                                    {
                                        bool alreadyExists = false;

                                        for( const wxString& libName : loadedLibs )
                                        {
                                            if( adapter->LoadSymbol( libName, fullLibName.wx_str() ) )
                                            {
                                                alreadyExists = true;
                                                break;
                                            }
                                        }

                                        if( !alreadyExists )
                                        {
                                            newId.SetLibNickname( nickname );
                                            newId.SetLibItemName( fullLibName );
                                            symbol->SetLibId( newId );
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if( ( !cfg || !cfg->m_RescueNeverShow ) && !cacheExists )
                {
                    SCH_EDITOR_CONTROL* editor = m_toolManager->GetTool<SCH_EDITOR_CONTROL>();
                    editor->RescueSymbolLibTableProject( false );
                }
            }

            // Ensure there is only one legacy library loaded and that it is the cache library.
            LEGACY_SYMBOL_LIBS* legacyLibs = PROJECT_SCH::LegacySchLibs( &Schematic().Project() );

            if( legacyLibs->GetLibraryCount() == 0 )
            {
                wxString extMsg;
                wxFileName cacheFn = pro;

                wxLogTrace( traceAutoSave, "[SetName dbg] cacheFn BEFORE path='%s' name='%s' full='%s' arg='%s'",
                            cacheFn.GetPath(), cacheFn.GetName(), cacheFn.GetFullPath(), cacheFn.GetName() + "-cache" );
                cacheFn.SetName( cacheFn.GetName() + "-cache" );
                wxLogTrace( traceAutoSave, "[SetName dbg] cacheFn AFTER  path='%s' name='%s' full='%s'",
                            cacheFn.GetPath(), cacheFn.GetName(), cacheFn.GetFullPath() );
                cacheFn.SetExt( FILEEXT::LegacySymbolLibFileExtension );

                msg.Printf( _( "The project symbol library cache file '%s' was not found." ),
                            cacheFn.GetFullName() );
                extMsg = _( "This can result in a broken schematic under certain conditions.  "
                            "If the schematic does not have any missing symbols upon opening, "
                            "save it immediately before making any changes to prevent data "
                            "loss.  If there are missing symbols, either manual recovery of "
                            "the schematic or recovery of the symbol cache library file and "
                            "reloading the schematic is required." );

                KICAD_MESSAGE_DIALOG dlgMissingCache( this, msg, _( "Warning" ),
                                                      wxOK | wxCANCEL | wxICON_EXCLAMATION | wxCENTER );
                dlgMissingCache.SetExtendedMessage( extMsg );
                dlgMissingCache.SetOKCancelLabels( KICAD_MESSAGE_DIALOG::ButtonLabel( _( "Load Without Cache File" ) ),
                                                   KICAD_MESSAGE_DIALOG::ButtonLabel( _( "Abort" ) ) );

                if( dlgMissingCache.ShowModal() == wxID_CANCEL )
                {
                    Schematic().Reset();
                    CreateDefaultScreens();
                    return false;
                }
            }

            // Update all symbol library links for all sheets.
            schematic.UpdateSymbolLinks( &loadReporter );

            m_infoBar->RemoveAllButtons();
            m_infoBar->AddCloseButton();
            m_infoBar->ShowMessage( _( "This file was created by an older version of KiCad. "
                                       "It will be converted to the new format when saved." ),
                                    wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );

            // Legacy schematic can have duplicate time stamps so fix that before converting
            // to the s-expression format.
            schematic.ReplaceDuplicateTimeStamps();

            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
                screen->FixLegacyPowerSymbolMismatches();

            // Allow the schematic to be saved to new file format without making any edits.
            OnModify();
        }
        else  // S-expression schematic.
        {
            SCH_SCREEN* first_screen = schematic.GetFirst();

            // Skip the first screen as it is a virtual root with no version info.
            if( first_screen && first_screen->GetFileFormatVersionAtLoad() == 0 )
                first_screen = schematic.GetNext();

            if( first_screen && first_screen->GetFileFormatVersionAtLoad() < SEXPR_SCHEMATIC_FILE_VERSION )
            {
                m_infoBar->RemoveAllButtons();
                m_infoBar->AddCloseButton();
                m_infoBar->ShowMessage( _( "This file was created by an older version of KiCad. "
                                           "It will be converted to the new format when saved." ),
                                        wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
            }

            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
                screen->UpdateLocalLibSymbolLinks();

            SCH_SCREEN* rootScreen = Schematic().RootScreen();

            // Restore all of the loaded symbol and sheet instances from the root sheet.
            if( rootScreen && rootScreen->GetFileFormatVersionAtLoad() < 20221002 )
                sheetList.UpdateSymbolInstanceData( rootScreen->GetSymbolInstances() );

            if( rootScreen && rootScreen->GetFileFormatVersionAtLoad() < 20221110 )
                sheetList.UpdateSheetInstanceData( rootScreen->GetSheetInstances());

            if( rootScreen && rootScreen->GetFileFormatVersionAtLoad() < 20230221 )
                for( SCH_SCREEN* screen = schematic.GetFirst(); screen;
                     screen = schematic.GetNext() )
                    screen->FixLegacyPowerSymbolMismatches();

            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
                screen->MigrateSimModels();

            Schematic().LoadVariants();
            UpdateVariantSelectionCtrl( Schematic().GetVariantNamesForUI() );
        }

        // After the schematic is successfully loaded, we load the drawing sheet.
        // This allows us to use the drawing sheet embedded in the schematic (if any)
        // instead of the default one.
        LoadDrawingSheet();

        schematic.PruneOrphanedSymbolInstances( Prj().GetProjectName(), sheetList );
        schematic.PruneOrphanedSheetInstances( Prj().GetProjectName(), sheetList );

        wxLogTrace( traceSchCurrentSheet,
                   "Before CheckForMissingSymbolInstances: Current sheet path='%s', size=%zu",
                   GetCurrentSheet().Path().AsString(),
                   GetCurrentSheet().size() );
        sheetList.CheckForMissingSymbolInstances( Prj().GetProjectName() );

        Schematic().ConnectionGraph()->Reset();

        SetScreen( GetCurrentSheet().LastScreen() );

        wxLogTrace( traceSchCurrentSheet,
                   "After SetScreen: Current sheet path='%s', size=%zu",
                   GetCurrentSheet().Path().AsString(),
                   GetCurrentSheet().size() );

        // Migrate conflicting bus definitions
        // TODO(JE) This should only run once based on schematic file version
        if( Schematic().ConnectionGraph()->GetBusesNeedingMigration().size() > 0 )
        {
            DIALOG_MIGRATE_BUSES dlg( this );
            dlg.ShowQuasiModal();
            OnModify();
        }

        SCH_COMMIT dummy( this );

        progressReporter.Report( _( "Updating connections..." ) );
        progressReporter.KeepRefreshing();

        RecalculateConnections( &dummy, GLOBAL_CLEANUP, &progressReporter );

        if( schematic.HasSymbolFieldNamesWithWhiteSpace() )
        {
            m_infoBar->QueueShowMessage( _( "This schematic contains symbols that have leading "
                                            "and/or trailing white space field names." ),
                                         wxICON_WARNING );
        }
    }

    // Load any exclusions from the project file
    Schematic().ResolveERCExclusionsPostUpdate();

    initScreenZoom();
    SetSheetNumberAndCount();

    RecomputeIntersheetRefs();
    GetCurrentSheet().UpdateAllScreenReferences();

    // Re-create junctions if needed. Eeschema optimizes wires by merging
    // colinear segments. If a schematic is saved without a valid
    // cache library or missing installed libraries, this can cause connectivity errors
    // unless junctions are added.
    //
    // TODO: (RFB) This really needs to be put inside the Load() function of the SCH_IO_KICAD_LEGACY
    // I can't put it right now because of the extra code that is above to convert legacy bus-bus
    // entries to bus wires
    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
        Schematic().FixupJunctionsAfterImport();

    SyncView();
    GetScreen()->ClearDrawingState();

    TestDanglingEnds();

    UpdateHierarchyNavigator( false, true );

    wxCommandEvent changedEvt( EDA_EVT_SCHEMATIC_CHANGED );
    ProcessEventLocally( changedEvt );

    if( !differentProject )
    {
        // If we didn't reload the project, we still need to call ProjectChanged() to ensure
        // frame-specific initialization happens (like registering the autosave saver).
        // When running under the project manager, KIWAY::ProjectChanged() was called before
        // this frame existed, so we need to call our own ProjectChanged() now.
        ProjectChanged();
    }

    for( wxEvtHandler* listener : m_schematicChangeListeners )
    {
        wxCHECK2( listener, continue );

        // Use the windows variant when handling event messages in case there is any special
        // event handler pre and/or post processing specific to windows.
        wxWindow* win = dynamic_cast<wxWindow*>( listener );

        if( win )
            win->HandleWindowEvent( e );
        else
            listener->SafelyProcessEvent( e );
    }

    updateTitle();
    m_toolManager->GetTool<SCH_NAVIGATE_TOOL>()->ResetHistory();

    wxFileName fn = Prj().AbsolutePath( GetScreen()->GetFileName() );

    if( fn.FileExists() && !fn.IsFileWritable() )
    {
        m_infoBar->RemoveAllButtons();
        m_infoBar->AddCloseButton();
        m_infoBar->ShowMessage( _( "Schematic is read only." ),
                                wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
    }

#ifdef PROFILE
    openFiles.Show();
#endif
    // Ensure all items are redrawn (especially the drawing-sheet items):
    if( GetCanvas() )
        GetCanvas()->DisplaySheet( GetCurrentSheet().LastScreen() );

    // Trigger a library load to handle any project-specific libraries
    CallAfter( [&]()
            {
                KIFACE *schface = Kiway().KiFACE( KIWAY::FACE_SCH );
                schface->PreloadLibraries( &Kiway() );

                Pgm().PreloadDesignBlockLibraries( &Kiway() );
            } );

    m_remoteSymbolPane->BindWebViewLoaded();

    // Deferred auto-conversion: Convert kicad_sch to trace_sch AFTER UI is fully initialized
    // This prevents the synchronous wxExecute from interfering with toolbar creation
    wxFileName loadedFile( fullFileName );
    if( loadedFile.GetExt() == FILEEXT::KiCadSchematicFileExtension )
    {
        wxString kicadSchPath = fullFileName;
        CallAfter( [kicadSchPath]()
        {
            wxFileName traceSchFile( kicadSchPath );
            traceSchFile.SetExt( FILEEXT::TraceSchematicFileExtension );

            // Only convert if trace_sch doesn't exist (don't overwrite existing trace files)
            if( !wxFileName::FileExists( traceSchFile.GetFullPath() ) )
            {
                if( ConvertKicadSchToTraceSch( kicadSchPath ) )
                {
                    wxMessageBox( wxString::Format( 
                        _( "Automatically converted %s to Trace format: %s" ),
                        wxFileName( kicadSchPath ).GetFullName(),
                        traceSchFile.GetFullName() ),
                        _( "Trace Information" ), wxOK | wxICON_INFORMATION );
                }
            }
        });
    }

    return true;
}


bool SCH_EDIT_FRAME::ReloadSchematicFromFile( const wxString& aFileName, bool aSilent )
{
    wxString msg;
    wxString fullFileName = aFileName;
    wxFileName wx_filename( fullFileName );

    // Convert .trace_sch to .kicad_sch (Trace's custom format must be converted first)
    if( wx_filename.GetExt() == FILEEXT::TraceSchematicFileExtension )
    {
        wx_filename.SetExt( FILEEXT::KiCadSchematicFileExtension );
        fullFileName = wx_filename.GetFullPath();
    }

    // Ensure absolute path
    if( !wx_filename.IsAbsolute() )
    {
        wx_filename.MakeAbsolute();
        fullFileName = wx_filename.GetFullPath();
    }

    // Validate file exists and is readable
    if( !wxFileName::IsFileReadable( fullFileName ) )
    {
        msg.Printf( _( "Schematic file '%s' does not exist or is not readable." ), fullFileName );
        DisplayErrorMessage( this, msg );  // Always show - critical error
        return false;
    }

    // Preserve current sheet path before reload
    SCH_SHEET_PATH savedSheetPath = GetCurrentSheet();

    wxCommandEvent e( EDA_EVT_SCHEMATIC_CHANGING );
    ProcessEventLocally( e );

    // Reset inspection tool but DO NOT clear undo/redo stack
    SetScreen( nullptr );
    m_toolManager->GetTool<SCH_INSPECTION_TOOL>()->Reset( TOOL_BASE::SUPERMODEL_RELOAD );

    SetStatusText( wxEmptyString );
    m_infoBar->Dismiss();

    // Use progress reporter only in non-silent mode
    std::unique_ptr<WX_PROGRESS_REPORTER> progressReporter;
    if( !aSilent )
        progressReporter = std::make_unique<WX_PROGRESS_REPORTER>( this, _( "Reload Schematic" ), 1, PR_CAN_ABORT );

    // Determine file type
    SCH_IO_MGR::SCH_FILE_T schFileType = SCH_IO_MGR::GuessPluginTypeFromSchPath( fullFileName,
                                                                                 KICTL_KICAD_ONLY );

    // Handle legacy symbol libraries if needed
    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
    {
        if( !Prj().GetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS ) )
        {
            Prj().SetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS, nullptr );
            PROJECT_SCH::LegacySchLibs( &Prj() );
        }
    }
    else
    {
        Prj().SetElem( PROJECT::ELEM::LEGACY_SYMBOL_LIBS, nullptr );
    }

    // Create new schematic object to load into
    std::unique_ptr<SCHEMATIC> newSchematic = std::make_unique<SCHEMATIC>( &Prj() );
    bool failedLoad = false;

    SetScreen( nullptr );

    IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( schFileType ) );
    if( progressReporter )
        pi->SetProgressReporter( progressReporter.get() );

    try
    {
        {
            wxBusyCursor    busy;
            std::unique_ptr<WINDOW_DISABLER> raii;
            if( !aSilent )
                raii = std::make_unique<WINDOW_DISABLER>( this );

            // Check if project file has top-level sheets defined
            PROJECT_FILE& projectFile = Prj().GetProjectFile();
            const std::vector<TOP_LEVEL_SHEET_INFO>& topLevelSheets = projectFile.GetTopLevelSheets();

            if( !topLevelSheets.empty() )
            {
                std::vector<SCH_SHEET*> loadedSheets;

                // New multi-root format: Load all top-level sheets
                for( const TOP_LEVEL_SHEET_INFO& sheetInfo : topLevelSheets )
                {
                    wxFileName sheetFileName( Prj().GetProjectPath(), sheetInfo.filename );

                    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
                        sheetFileName.SetExt( FILEEXT::LegacySchematicFileExtension );

                    // Redirect .trace_sch references to .kicad_sch
                    if( sheetFileName.GetExt() == FILEEXT::TraceSchematicFileExtension )
                        sheetFileName.SetExt( FILEEXT::KiCadSchematicFileExtension );

                    wxString sheetPath = sheetFileName.GetFullPath();

                    if( !wxFileName::FileExists( sheetPath ) )
                    {
                        wxLogWarning( wxT( "Top-level sheet file not found: %s" ), sheetPath );
                        continue;
                    }

                    SCH_SHEET* sheet = pi->LoadSchematicFile( sheetPath, newSchematic.get() );

                    if( sheet )
                    {
                        sheet->SetName( sheetInfo.name );
                        loadedSheets.push_back( sheet );
                    }
                }

                if( !loadedSheets.empty() )
                {
                    newSchematic->SetTopLevelSheets( loadedSheets );
                }
                else
                {
                    newSchematic->CreateDefaultScreens();
                }
            }
            else
            {
                // Legacy single-root format: Load the single root sheet
                SCH_SHEET* rootSheet = pi->LoadSchematicFile( fullFileName, newSchematic.get() );
                rootSheet->SetName( _( "Root" ) );
                newSchematic->SetTopLevelSheets( { rootSheet } );
            }
        }

        if( !pi->GetError().IsEmpty() && !aSilent )
        {
            DisplayErrorMessage( this, _( "The entire schematic could not be loaded.  Errors "
                                          "occurred attempting to load hierarchical sheets." ),
                                 pi->GetError() );
        }
    }
    catch( const FUTURE_FORMAT_ERROR& ffe )
    {
        newSchematic->CreateDefaultScreens();
        msg.Printf( _( "Error loading schematic '%s'." ), fullFileName );
        if( progressReporter )
            progressReporter->Hide();
        DisplayErrorMessage( this, msg, ffe.Problem() );  // Always show - critical error
        failedLoad = true;
    }
    catch( const IO_ERROR& ioe )
    {
        newSchematic->CreateDefaultScreens();
        msg.Printf( _( "Error loading schematic '%s'." ), fullFileName );
        if( progressReporter )
            progressReporter->Hide();
        DisplayErrorMessage( this, msg, ioe.What() );  // Always show - critical error
        failedLoad = true;
    }
    catch( const std::bad_alloc& )
    {
        newSchematic->CreateDefaultScreens();
        msg.Printf( _( "Memory exhausted loading schematic '%s'." ), fullFileName );
        if( progressReporter )
            progressReporter->Hide();
        DisplayErrorMessage( this, msg, wxEmptyString );  // Always show - critical error
        failedLoad = true;
    }

    SetSchematic( newSchematic.release() );

    if( !aSilent )
        Raise();

    if( failedLoad )
    {
        CreateDefaultScreens();

        // Preserve the original filename so that subsequent code
        // (stream-end handler, batch timer, SaveDocument) doesn't get
        // "untitled.kicad_sch" from GetCurrentFileName().
        // CreateDefaultScreens() resets the screen filename to "untitled.kicad_sch"
        // which poisons all downstream callers of GetCurrentFileName().
        SCH_SCREEN* screen = Schematic().RootScreen();
        if( screen )
            screen->SetFileName( fullFileName );

        m_toolManager->RunAction( ACTIONS::zoomFitScreen );
        msg.Printf( _( "Failed to load '%s'." ), fullFileName );
        SetMsgPanel( wxEmptyString, msg );
        return false;
    }

    // Load project settings
    LoadProjectSettings();

    // Check if schematic was modified during load
    SCH_SHEET_LIST sheetList = Schematic().Hierarchy();

    if( sheetList.IsModified() )
    {
        // Always show - user needs to know file has issues
        DisplayInfoMessage( this,
                            _( "An error was found when loading the schematic that has "
                               "been automatically fixed.  Please save the schematic to "
                               "repair the broken file or it may not be usable with other "
                               "versions of KiCad." ) );
    }

    if( sheetList.AllSheetPageNumbersEmpty() )
        sheetList.SetInitialPageNumbers();

    UpdateFileHistory( fullFileName );

    SCH_SCREENS schematic( Schematic().Root() );

    // Handle legacy format conversions
    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
    {
        // Convert any legacy bus-bus entries to just be bus wires
        for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
        {
            std::vector<SCH_ITEM*> deleted;

            for( SCH_ITEM* item : screen->Items() )
            {
                if( item->Type() == SCH_BUS_BUS_ENTRY_T )
                {
                    SCH_BUS_BUS_ENTRY* entry = static_cast<SCH_BUS_BUS_ENTRY*>( item );
                    std::unique_ptr<SCH_LINE> wire = std::make_unique<SCH_LINE>();

                    wire->SetLayer( LAYER_BUS );
                    wire->SetStartPoint( entry->GetPosition() );
                    wire->SetEndPoint( entry->GetEnd() );

                    screen->Append( wire.release() );
                    deleted.push_back( item );
                }
            }

            for( SCH_ITEM* item : deleted )
                screen->Remove( item );
        }

        // Handle symbol library issues
        EESCHEMA_SETTINGS* cfg = dynamic_cast<EESCHEMA_SETTINGS*>( Kiface().KifaceSettings() );

        if( schematic.HasNoFullyDefinedLibIds() )
        {
            DIALOG_SYMBOL_REMAP dlgRemap( this );
            dlgRemap.ShowQuasiModal();
        }
        else
        {
            wxString paths;
            wxArrayString libNames;

            LEGACY_SYMBOL_LIBS::GetLibNamesAndPaths( &Prj(), &paths, &libNames );

            if( !libNames.IsEmpty() )
            {
                if( eeconfig()->m_Appearance.show_illegal_symbol_lib_dialog )
                {
                    wxRichMessageDialog invalidLibDlg(
                            this,
                            _( "Illegal entry found in project file symbol library list." ),
                            _( "Project Load Warning" ),
                            wxOK | wxCENTER | wxICON_EXCLAMATION );
                    invalidLibDlg.ShowDetailedText(
                            _( "Symbol libraries defined in the project file symbol library "
                               "list are no longer supported and will be removed.\n\n"
                               "This may cause broken symbol library links under certain "
                               "conditions." ) );
                    invalidLibDlg.ShowCheckBox( _( "Do not show this dialog again." ) );
                    invalidLibDlg.ShowModal();
                    eeconfig()->m_Appearance.show_illegal_symbol_lib_dialog =
                            !invalidLibDlg.IsCheckBoxChecked();
                }

                libNames.Clear();
                paths.Clear();
                LEGACY_SYMBOL_LIBS::SetLibNamesAndPaths( &Prj(), paths, libNames );
            }

            if( !cfg || !cfg->m_RescueNeverShow )
            {
                SCH_EDITOR_CONTROL* editor = m_toolManager->GetTool<SCH_EDITOR_CONTROL>();
                editor->RescueSymbolLibTableProject( false );
            }
        }

        // Ensure cache library exists
        LEGACY_SYMBOL_LIBS* legacyLibs = PROJECT_SCH::LegacySchLibs( &Schematic().Project() );

        if( legacyLibs->GetLibraryCount() == 0 )
        {
            wxFileName pro( fullFileName );
            pro.SetExt( FILEEXT::ProjectFileExtension );
            wxFileName cacheFn = pro;
            cacheFn.SetName( cacheFn.GetName() + "-cache" );
            cacheFn.SetExt( FILEEXT::LegacySymbolLibFileExtension );

            msg.Printf( _( "The project symbol library cache file '%s' was not found." ),
                        cacheFn.GetFullName() );
            wxString extMsg = _( "This can result in a broken schematic under certain conditions.  "
                                "If the schematic does not have any missing symbols upon opening, "
                                "save it immediately before making any changes to prevent data "
                                "loss.  If there are missing symbols, either manual recovery of "
                                "the schematic or recovery of the symbol cache library file and "
                                "reloading the schematic is required." );

            wxMessageDialog dlgMissingCache( this, msg, _( "Warning" ),
                                             wxOK | wxCANCEL | wxICON_EXCLAMATION | wxCENTER );
            dlgMissingCache.SetExtendedMessage( extMsg );
            dlgMissingCache.SetOKCancelLabels(
                    wxMessageDialog::ButtonLabel( _( "Load Without Cache File" ) ),
                    wxMessageDialog::ButtonLabel( _( "Abort" ) ) );

            if( dlgMissingCache.ShowModal() == wxID_CANCEL )
            {
                Schematic().Reset();
                CreateDefaultScreens();
                return false;
            }
        }

        schematic.UpdateSymbolLinks();

        if( !aSilent )
        {
        m_infoBar->RemoveAllButtons();
        m_infoBar->AddCloseButton();
        m_infoBar->ShowMessage( _( "This file was created by an older version of KiCad. "
                                   "It will be converted to the new format when saved." ),
                                wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
        }

        schematic.ReplaceDuplicateTimeStamps();

        for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
            screen->FixLegacyPowerSymbolMismatches();

        OnModify();
    }
    else  // S-expression schematic
    {
        SCH_SCREEN* first_screen = schematic.GetFirst();

        if( first_screen->GetFileFormatVersionAtLoad() == 0 )
            first_screen = schematic.GetNext();

        if( first_screen && first_screen->GetFileFormatVersionAtLoad() < SEXPR_SCHEMATIC_FILE_VERSION )
        {
            if( !aSilent )
        {
            m_infoBar->RemoveAllButtons();
            m_infoBar->AddCloseButton();
            m_infoBar->ShowMessage( _( "This file was created by an older version of KiCad. "
                                       "It will be converted to the new format when saved." ),
                                    wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
            }
        }

        for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
            screen->UpdateLocalLibSymbolLinks();

        if( Schematic().RootScreen()->GetFileFormatVersionAtLoad() < 20221002 )
            sheetList.UpdateSymbolInstanceData( Schematic().RootScreen()->GetSymbolInstances() );

        if( Schematic().RootScreen()->GetFileFormatVersionAtLoad() < 20221110 )
            sheetList.UpdateSheetInstanceData( Schematic().RootScreen()->GetSheetInstances() );

        if( Schematic().RootScreen()->GetFileFormatVersionAtLoad() < 20230221 )
            for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
                screen->FixLegacyPowerSymbolMismatches();

        for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
            screen->MigrateSimModels();
    }

    // Load drawing sheet
    LoadDrawingSheet();

    schematic.PruneOrphanedSymbolInstances( Prj().GetProjectName(), sheetList );
    schematic.PruneOrphanedSheetInstances( Prj().GetProjectName(), sheetList );
    sheetList.CheckForMissingSymbolInstances( Prj().GetProjectName() );

    Schematic().ConnectionGraph()->Reset();

    // Try to restore the saved sheet path, or use the current sheet
    SCH_SHEET_PATH targetSheet = savedSheetPath;
    SCH_SHEET_LIST newSheetList = Schematic().Hierarchy();

    // Try to find the saved sheet in the new hierarchy
    bool foundSheet = false;
    for( const SCH_SHEET_PATH& path : newSheetList )
    {
        if( path.Path() == savedSheetPath.Path() )
        {
            targetSheet = path;
            foundSheet = true;
            break;
        }
    }

    // If we couldn't find the saved sheet, use the current sheet
    if( !foundSheet )
    {
        targetSheet = GetCurrentSheet();
    }

    SetCurrentSheet( targetSheet );
    SetScreen( GetCurrentSheet().LastScreen() );

    // Migrate conflicting bus definitions
    if( Schematic().ConnectionGraph()->GetBusesNeedingMigration().size() > 0 )
    {
        DIALOG_MIGRATE_BUSES dlg( this );
        dlg.ShowQuasiModal();
        OnModify();
    }

    SCH_COMMIT dummy( this );

    if( progressReporter )
    {
        progressReporter->Report( _( "Updating connections..." ) );
        progressReporter->KeepRefreshing();
    }

    RecalculateConnections( &dummy, GLOBAL_CLEANUP, progressReporter.get() );

    if( schematic.HasSymbolFieldNamesWithWhiteSpace() && !aSilent )
    {
        m_infoBar->QueueShowMessage( _( "This schematic contains symbols that have leading "
                                        "and/or trailing white space field names." ),
                                     wxICON_WARNING );
    }

    // Load any exclusions from the project file
    Schematic().ResolveERCExclusionsPostUpdate();

    initScreenZoom();
    SetSheetNumberAndCount();

    RecomputeIntersheetRefs();
    GetCurrentSheet().UpdateAllScreenReferences();

    if( schFileType == SCH_IO_MGR::SCH_LEGACY )
        Schematic().FixupJunctionsAfterImport();

    SyncView();
    GetScreen()->ClearDrawingState();

    TestDanglingEnds();

    UpdateHierarchyNavigator( false, true );

    wxCommandEvent changedEvt( EDA_EVT_SCHEMATIC_CHANGED );
    ProcessEventLocally( changedEvt );

    for( wxEvtHandler* listener : m_schematicChangeListeners )
    {
        wxCHECK2( listener, continue );

        wxWindow* win = dynamic_cast<wxWindow*>( listener );

        if( win )
            win->HandleWindowEvent( changedEvt );
        else
            listener->SafelyProcessEvent( changedEvt );
    }

    updateTitle();

    wxFileName fn = Prj().AbsolutePath( GetScreen()->GetFileName() );

    if( fn.FileExists() && !fn.IsFileWritable() )
    {
        // Always show - user MUST know they can't save!
        m_infoBar->RemoveAllButtons();
        m_infoBar->AddCloseButton();
        m_infoBar->ShowMessage( _( "Schematic is read only." ),
                                wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
    }

    // Ensure all items are redrawn
    if( GetCanvas() )
        GetCanvas()->DisplaySheet( GetCurrentSheet().LastScreen() );

    return true;
}


bool SCH_EDIT_FRAME::CaptureSchematicStateForAIEdit( const wxString& aTraceSchPath )
{
    // Clear any previous state
    m_aiEditBeforeState.clear();
    m_aiEditTraceSchBackupPath.Clear();

    if( !Schematic().IsValid() )
        return false;


    // Create backup of trace_sch file
    wxFileName traceFile( aTraceSchPath );
    if( traceFile.FileExists() )
    {
        wxString backupPath = aTraceSchPath + wxT( ".ai_backup" );
        if( wxCopyFile( aTraceSchPath, backupPath, true ) )
        {
            m_aiEditTraceSchBackupPath = backupPath;
        }
        else
        {
            wxLogWarning( wxT( "Failed to create backup of trace_sch file: %s" ), aTraceSchPath );
            // Continue anyway - backup is for safety, not critical
        }
    }

    // Iterate all screens and capture all items
    SCH_SCREENS screens( Schematic().Root() );
    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        if( !screen )
            continue;

        // Iterate all items in this screen
        for( SCH_ITEM* item : screen->Items() )
        {
            if( !item )
                continue;

            // Store a copy of the item by UUID (needed because items will be invalid after reload)
            std::unique_ptr<SCH_ITEM> itemCopy( item->Duplicate( IGNORE_PARENT_GROUP, nullptr, true ) );
            if( itemCopy )
            {
                // CRITICAL: Set parent to nullptr to prevent findParent() from walking into
                // stale screen pointers after reload. The duplicated item's parent inherited
                // from the original points to the OLD screen which will be freed on reload.
                // Setting to nullptr makes findParent() safely return nullptr instead of crashing.
                itemCopy->SetParent( nullptr );
                
                m_aiEditBeforeState[item->m_Uuid] = std::move( itemCopy );
            }
        }
    }

    return true;
}


bool SCH_EDIT_FRAME::CompareAndCreateAIEditUndoEntries()
{
    if( m_aiEditBeforeState.empty() )
    {
        // No before state captured, nothing to compare
        return false;
    }

    // SAFETY: Check if schematic is valid before accessing
    try
    {
    if( !Schematic().IsValid() )
    {
        // Clear state and return
            m_aiEditBeforeState.clear();
            m_aiEditTraceSchBackupPath.Clear();
            return false;
        }
    }
    catch( ... )
    {
        // Schematic access failed - clear state and return
        m_aiEditBeforeState.clear();
        m_aiEditTraceSchBackupPath.Clear();
        return false;
    }

    // Build map of current items by UUID
    std::map<KIID, SCH_ITEM*> currentState;
    
    try
    {
    SCH_SCREENS screens( Schematic().Root() );
    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items() )
        {
            if( !item )
                continue;

            currentState[item->m_Uuid] = item;
        }
        }
    }
    catch( ... )
    {
        // Failed to iterate screens - clear state and return
        m_aiEditBeforeState.clear();
        m_aiEditTraceSchBackupPath.Clear();
        return false;
    }

    // Create a PICKED_ITEMS_LIST for this undo entry
    PICKED_ITEMS_LIST* undoList = new PICKED_ITEMS_LIST();
    
    // Use the AI edit description if set, otherwise use generic label
    wxString undoDescription;
    if( !m_aiEditDescription.IsEmpty() )
    {
        // Truncate long descriptions for undo menu
        wxString desc = m_aiEditDescription;
        if( desc.length() > 40 )
            desc = desc.Left( 37 ) + wxT( "..." );
        undoDescription = wxString::Format( _( "AI: %s" ), desc );
    }
    else
    {
        undoDescription = _( "AI Edit" );
    }
    undoList->SetDescription( undoDescription );

    bool hasChanges = false;

    // Find deleted items (in old but not new)
    for( const auto& pair : m_aiEditBeforeState )
    {
        const KIID& uuid = pair.first;
        const std::unique_ptr<SCH_ITEM>& oldItemCopy = pair.second;

        if( currentState.find( uuid ) == currentState.end() )
        {
            // Item was deleted - we need to find which screen it should be on
            // Since we don't have the screen from the old state, we'll use the current screen
            // or try to find it in the hierarchy
            SCH_SCREEN* screen = GetScreen();
            if( screen && oldItemCopy )
            {
                try
                {
                    // Create a new copy of the old item to add back (for undo)
                    // Wrap in try-catch as stored items may have stale parent pointers
                    SCH_ITEM* restoredItem = oldItemCopy->Duplicate( IGNORE_PARENT_GROUP, nullptr, true );
                    if( restoredItem )
                    {
                        // CRITICAL: Set parent to screen before adding to undo list
                        // This prevents crashes when PutDataInPreviousState calls GetBoundingBox()
                        // which needs the parent for GetPenWidth()
                        restoredItem->SetParent( screen );
                        
                        ITEM_PICKER picker( screen, restoredItem, UNDO_REDO::DELETED );
                        picker.SetFlags( oldItemCopy->GetFlags() );
                        undoList->PushItem( picker );
                        hasChanges = true;
                    }
                }
                catch( ... )
                {
                    // Duplicate failed due to stale pointers - skip this item for undo
                    wxLogDebug( wxT( "Failed to duplicate item for undo - skipping" ) );
                }
            }
        }
    }

    // Find new items (in new but not old) and changed items (in both but different)
    // We need to track which screen each item belongs to
    std::map<KIID, SCH_SCREEN*> itemToScreenMap;
    try
    {
        SCH_SCREENS screensForMap( Schematic().Root() );
        for( SCH_SCREEN* screen = screensForMap.GetFirst(); screen; screen = screensForMap.GetNext() )
    {
        if( !screen )
            continue;
        
        for( SCH_ITEM* item : screen->Items() )
        {
            if( item )
                itemToScreenMap[item->m_Uuid] = screen;
        }
        }
    }
    catch( ... )
    {
        // Failed to build item-to-screen map - continue with what we have
        wxLogDebug( wxT( "AI Edit: Failed to build item-to-screen map" ) );
    }

    for( const auto& pair : currentState )
    {
        const KIID& uuid = pair.first;
        SCH_ITEM* newItem = pair.second;

        auto screenIt = itemToScreenMap.find( uuid );
        SCH_SCREEN* screen = ( screenIt != itemToScreenMap.end() ) ? screenIt->second : GetScreen();

        auto oldIt = m_aiEditBeforeState.find( uuid );
        if( oldIt == m_aiEditBeforeState.end() )
        {
            // Item is new
            if( screen )
            {
                ITEM_PICKER picker( screen, newItem, UNDO_REDO::NEWITEM );
                picker.SetFlags( newItem->GetFlags() );
                undoList->PushItem( picker );
                hasChanges = true;
            }
        }
        else
        {
            // Item exists in both - check if it changed
            SCH_ITEM* oldItem = oldIt->second.get();
            
            if( !oldItem )
                continue;
            
            // Compare items to see if they changed.
            // NOTE: The stored oldItem has its parent set to nullptr (done in CaptureSchematicStateForAIEdit)
            // to prevent findParent() from walking into stale screen pointers. This means field
            // comparisons via GetParentSymbol() will return nullptr and may report items as different
            // even if they're the same - but that's safe (conservative: we create undo entries).
            bool itemsAreDifferent = true;  // Default to different (safe)
            
            if( oldItem->Type() == newItem->Type() )
            {
                // Try comparison - safe now that parent is nullptr
                itemsAreDifferent = !( *oldItem == *newItem );
            }
            
            if( itemsAreDifferent )
                {
                    // Items are different - create CHANGED entry
                    if( screen )
                    {
                        // Create a copy of the current item for undo
                        SCH_ITEM* itemCopy = newItem->Duplicate( IGNORE_PARENT_GROUP, nullptr, true );
                        if( itemCopy )
                        {
                            // CRITICAL: Set parent to screen for the copy (stored in link)
                            // This prevents crashes when PutDataInPreviousState restores the old state
                        itemCopy->SetParent( screen );
                        
                        ITEM_PICKER picker( screen, newItem, UNDO_REDO::CHANGED );
                        picker.SetLink( itemCopy );
                        picker.SetFlags( newItem->GetFlags() );
                        undoList->PushItem( picker );
                        hasChanges = true;
                    }
                }
            }
        }
    }

    // Save the undo list if there are changes
    if( hasChanges && undoList->GetCount() > 0 )
    {
        try
    {
        SaveCopyInUndoList( *undoList, UNDO_REDO::UNSPECIFIED, false );
        }
        catch( ... )
        {
            // Failed to save undo list - cleanup
            wxLogDebug( wxT( "AI Edit: Failed to save undo list" ) );
            delete undoList;
            undoList = nullptr;
        }
    }
    else
    {
        delete undoList;
    }

    // Clean up state and backup file
    m_aiEditBeforeState.clear();
    m_aiEditDescription.Clear();
    if( !m_aiEditTraceSchBackupPath.IsEmpty() && wxFile::Exists( m_aiEditTraceSchBackupPath ) )
    {
        wxRemoveFile( m_aiEditTraceSchBackupPath );
    }
    m_aiEditTraceSchBackupPath.Clear();

    return hasChanges;
}


void SCH_EDIT_FRAME::RemapUndoRedoAfterReload()
{
    if( !Schematic().IsValid() )
        return;

    // Build UUID → (item*, screen*) map from the current (post-reload) schematic
    std::map<KIID, std::pair<SCH_ITEM*, SCH_SCREEN*>> uuidMap;

    try
    {
        SCH_SCREENS screens( Schematic().Root() );

        for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
        {
            if( !screen )
                continue;

            for( SCH_ITEM* item : screen->Items() )
            {
                if( item )
                    uuidMap[item->m_Uuid] = { item, screen };
            }
        }
    }
    catch( ... )
    {
        return;
    }


    auto remapList = [&]( UNDO_REDO_CONTAINER& aContainer )
    {
        // Walk entries back-to-front so removal doesn't shift indices we still need
        for( int ci = static_cast<int>( aContainer.m_CommandsList.size() ) - 1; ci >= 0; --ci )
        {
            PICKED_ITEMS_LIST* cmd = aContainer.m_CommandsList[ci];
            if( !cmd )
                continue;

            bool anyValid = false;

            for( unsigned pi = 0; pi < cmd->GetCount(); ++pi )
            {
                UNDO_REDO status = cmd->GetPickedItemStatus( pi );
                EDA_ITEM* item   = cmd->GetPickedItem( pi );

                if( !item )
                    continue;

                auto it = uuidMap.find( item->m_Uuid );

                if( status == UNDO_REDO::DELETED )
                {
                    // DELETED entries hold an independent copy; only the screen
                    // needs updating so AddToScreen targets the right screen.
                    if( !uuidMap.empty() )
                    {
                        // Use the root screen as a fallback
                        SCH_SCREEN* rootScreen = Schematic().RootScreen();
                        if( rootScreen )
                            cmd->GetItemWrapper( pi ).SetScreen( rootScreen );
                    }
                    anyValid = true;
                }
                else if( it != uuidMap.end() )
                {
                    // CHANGED / NEWITEM: point at the new live item + screen
                    cmd->SetPickedItem( it->second.first, pi );
                    cmd->GetItemWrapper( pi ).SetScreen( it->second.second );
                    anyValid = true;
                }
                // else: item not found in new schematic – leave as-is;
                //       PutDataInPreviousState will skip if item isn't on screen
            }

            if( !anyValid )
            {
                // Every picker in this command is stale – drop the entry
                cmd->ClearListAndDeleteItems( []( EDA_ITEM* aItem ) { delete aItem; } );
                delete cmd;
                aContainer.m_CommandsList.erase( aContainer.m_CommandsList.begin() + ci );
            }
        }
    };

    remapList( m_undoList );
    remapList( m_redoList );

}


void SCH_EDIT_FRAME::AutoplaceModifiedSymbols( const std::set<std::string>& aModifiedUUIDs )
{
    if( aModifiedUUIDs.empty() )
        return;

    // SAFETY: Check if schematic is valid before accessing
    try
    {
        if( !Schematic().IsValid() )
            return;
    }
    catch( ... )
    {
        return;
    }

    bool anyAutoplaced = false;
    std::set<size_t> modifiedScreenIndices;

    try
    {
        SCH_SCREENS screens( Schematic().Root() );

        for( size_t i = 0; i < screens.GetCount(); i++ )
        {
            SCH_SCREEN* screen = screens.GetScreen( i );
            if( !screen )
                continue;
                
            for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
            {
                if( !item )
                    continue;
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                // Check if this symbol's UUID is in the modified set
                std::string uuidStr = symbol->m_Uuid.AsString().ToStdString();

                if( aModifiedUUIDs.count( uuidStr ) )
                {
                    // Autoplace fields for this symbol
                    // Uses AUTOPLACE_AUTO algorithm (same as when placing new symbols)
                    // Only affects fields with CanAutoplace() == true
                    symbol->AutoplaceFields( screen, AUTOPLACE_AUTO );
                    anyAutoplaced = true;
                    modifiedScreenIndices.insert( i );
                }
            }
        }

        // If we autoplaced any symbols, save the modified screens and refresh the canvas
        if( anyAutoplaced )
        {
            // Save each modified screen to persist the autoplace changes
            for( size_t idx : modifiedScreenIndices )
            {
                SCH_SCREEN* screen = screens.GetScreen( idx );
                SCH_SHEET* sheet = screens.GetSheet( idx );
                
                if( screen && sheet && !screen->GetFileName().IsEmpty() )
                {
                    try
                    {
                        IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
                        pi->SaveSchematicFile( screen->GetFileName(), sheet, &Schematic() );
                    }
                    catch( ... )
                    {
                        // Silently ignore save errors - the autoplace still happened in memory
                    }
                }
            }
            
            // Refresh the canvas to show the updated field positions
            GetCanvas()->Refresh();
        }
    }
    catch( ... )
    {
        // Failed to iterate screens - silently return
        return;
    }
}


void SCH_EDIT_FRAME::OnImportProject( wxCommandEvent& aEvent )
{
    if( Schematic().RootScreen() && !Schematic().RootScreen()->Items().empty() )
    {
        wxString msg = _( "This operation replaces the contents of the current schematic, "
                          "which will be permanently lost.\n\n"
                          "Do you want to proceed?" );

        if( !IsOK( this, msg ) )
            return;
    }

    // Set the project location if none is set or if we are running in standalone mode
    bool     setProject = Prj().GetProjectFullName().IsEmpty() || Kiface().IsSingle();
    wxString path = wxPathOnly( Prj().GetProjectFullName() );

    wxString fileFiltersStr;
    wxString allWildcardsStr;

    for( const SCH_IO_MGR::SCH_FILE_T& fileType : SCH_IO_MGR::SCH_FILE_T_vector )
    {
        if( fileType == SCH_IO_MGR::SCH_KICAD || fileType == SCH_IO_MGR::SCH_LEGACY )
            continue; // this is "Import non-KiCad schematic"

        IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( fileType ) );

        if( !pi )
            continue;

        const IO_BASE::IO_FILE_DESC& desc = pi->GetSchematicFileDesc();

        if( desc.m_FileExtensions.empty() || !desc.m_CanRead )
            continue;

        if( !fileFiltersStr.IsEmpty() )
            fileFiltersStr += wxChar( '|' );

        fileFiltersStr += desc.FileFilter();

        for( const std::string& ext : desc.m_FileExtensions )
            allWildcardsStr << wxS( "*." ) << formatWildcardExt( ext ) << wxS( ";" );
    }

    fileFiltersStr = _( "All supported formats" ) + wxS( "|" ) + allWildcardsStr + wxS( "|" )
                     + fileFiltersStr;

    wxFileDialog dlg( this, _( "Import Schematic" ), path, wxEmptyString, fileFiltersStr,
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST ); // TODO

    FILEDLG_IMPORT_NON_KICAD importOptions( eeconfig()->m_System.show_import_issues );
    dlg.SetCustomizeHook( importOptions );

    KIPLATFORM::UI::AllowNetworkFileSystems( &dlg );

    if( dlg.ShowModal() == wxID_CANCEL )
        return;

    eeconfig()->m_System.show_import_issues = importOptions.GetShowIssues();

    // Don't leave dangling pointers to previously-opened document.
    m_toolManager->GetTool<SCH_SELECTION_TOOL>()->ClearSelection();
    ClearUndoRedoList();
    ClearRepeatItemsList();

    if( setProject )
    {
        Schematic().SetProject( nullptr );
        GetSettingsManager()->UnloadProject( &Prj(), false );

        // Clear view before destroying schematic as repaints depend on schematic being valid
        SetScreen( nullptr );

        Schematic().Reset();

        wxFileName projectFn( dlg.GetPath() );
        projectFn.SetExt( FILEEXT::ProjectFileExtension );
        GetSettingsManager()->LoadProject( projectFn.GetFullPath() );
    }

    wxFileName fn = dlg.GetPath();

    if( !fn.IsFileReadable() )
    {
        wxLogError( _( "Insufficient permissions to read file '%s'." ), fn.GetFullPath() );
        return;
    }

    SCH_IO_MGR::SCH_FILE_T pluginType = SCH_IO_MGR::SCH_FILE_T::SCH_FILE_UNKNOWN;

    for( const SCH_IO_MGR::SCH_FILE_T& fileType : SCH_IO_MGR::SCH_FILE_T_vector )
    {
        IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( fileType ) );

        if( !pi )
            continue;

        if( pi->CanReadSchematicFile( fn.GetFullPath() ) )
        {
            pluginType = fileType;
            break;
        }
    }

    if( pluginType == SCH_IO_MGR::SCH_FILE_T::SCH_FILE_UNKNOWN )
    {
        wxLogError( _( "No loader can read the specified file: '%s'." ), fn.GetFullPath() );
        CreateDefaultScreens();
        SetScreen( Schematic().RootScreen() );
        return;
    }

    importFile( dlg.GetPath(), pluginType );

    RefreshCanvas();
}


bool SCH_EDIT_FRAME::saveSchematicFile( SCH_SHEET* aSheet, const wxString& aSavePath )
{
    wxString msg;
    wxFileName schematicFileName;
    wxFileName oldFileName;
    bool success;

    SCH_SCREEN* screen = aSheet->GetScreen();

    wxCHECK( screen, false );

    // Cannot save to nowhere
    if( aSavePath.IsEmpty() )
        return false;

    // Construct the name of the file to be saved
    schematicFileName = Prj().AbsolutePath( aSavePath );
    oldFileName = schematicFileName;

    // Write through symlinks, don't replace them
    WX_FILENAME::ResolvePossibleSymlinks( schematicFileName );

    if( !schematicFileName.DirExists() )
    {
        if( !wxMkdir( schematicFileName.GetPath() ) )
        {
            msg.Printf( _( "Error saving schematic file '%s'.\n%s" ),
                        schematicFileName.GetFullPath(),
                        "Could not create directory: %s" + schematicFileName.GetPath() );
            DisplayError( this, msg );

            return false;
        }
    }

    if( !IsWritable( schematicFileName ) )
        return false;

    wxFileName projectFile( schematicFileName );

    projectFile.SetExt( FILEEXT::ProjectFileExtension );

    if( projectFile.FileExists() )
    {
        // Save various ERC settings, such as violation severities (which may have been edited
        // via the ERC dialog as well as the Schematic Setup dialog), ERC exclusions, etc.
        saveProjectSettings();
    }

    // Save
    wxLogTrace( traceAutoSave, wxS( "Saving file " ) + schematicFileName.GetFullPath() );

    if( m_infoBar->GetMessageType() == WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE )
        m_infoBar->Dismiss();

    SCH_IO_MGR::SCH_FILE_T pluginType = SCH_IO_MGR::GuessPluginTypeFromSchPath(
            schematicFileName.GetFullPath() );

    if( pluginType == SCH_IO_MGR::SCH_FILE_UNKNOWN )
        pluginType = SCH_IO_MGR::SCH_KICAD;

    IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( pluginType ) );

    // On Windows, ensure the target file is writeable by clearing problematic attributes like
    // hidden or read-only. This can happen when files are synced via cloud services.
    if( schematicFileName.FileExists() )
        KIPLATFORM::IO::MakeWriteable( schematicFileName.GetFullPath() );

    try
    {
        pi->SaveSchematicFile( schematicFileName.GetFullPath(), aSheet, &Schematic() );
        success = true;

        AMPLITUDE_CLIENT::Instance().Track( "schematic_saved", {
            { "app_type", "eeschema" },
        } );
    }
    catch( const IO_ERROR& ioe )
    {
        msg.Printf( _( "Error saving schematic file '%s'.\n%s" ),
                    schematicFileName.GetFullPath(),
                    ioe.What() );
        DisplayError( this, msg );

        success = false;
    }

    if( success )
    {
        screen->SetContentModified( false );

        msg.Printf( _( "File '%s' saved." ),  screen->GetFileName() );
        SetStatusText( msg, 0 );

        // Record a full project snapshot so related files (symbols, libs, sheets) are captured.
        Kiway().LocalHistory().CommitFullProjectSnapshot( schematicFileName.GetPath(), wxS( "SCH Save" ) );
        Kiway().LocalHistory().TagSave( schematicFileName.GetPath(), wxS( "sch" ) );

        // Convert to trace_sch format if this is a .kicad_sch file
        if( schematicFileName.GetExt() == FILEEXT::KiCadSchematicFileExtension )
        {
            wxLogDebug( wxT( "AI DEBUG [saveSchematicFile]: Calling ConvertKicadSchToTraceSch: %s" ), 
                       schematicFileName.GetFullPath() );
            ConvertKicadSchToTraceSch( schematicFileName.GetFullPath() );
        }

        if( m_autoSaveTimer )
            m_autoSaveTimer->Stop();

        m_autoSavePending = false;
        m_autoSaveRequired = false;
    }

    return success;
}


bool PrepareSaveAsFiles( SCHEMATIC& aSchematic, SCH_SCREENS& aScreens,
                         const wxFileName& aOldRoot, const wxFileName& aNewRoot,
                         bool aSaveCopy, bool aCopySubsheets, bool aIncludeExternSheets,
                         std::unordered_map<SCH_SCREEN*, wxString>& aFilenameMap,
                         wxString& aErrorMsg )
{
    SCH_SCREEN* screen;

    for( size_t i = 0; i < aScreens.GetCount(); i++ )
    {
        screen = aScreens.GetScreen( i );

        wxCHECK2( screen, continue );

        if( screen == aSchematic.RootScreen() )
            continue;

        wxFileName src = screen->GetFileName();

        if( !src.IsAbsolute() )
            src.MakeAbsolute( aOldRoot.GetPath() );

        bool internalSheet = src.GetPath().StartsWith( aOldRoot.GetPath() );

        if( aCopySubsheets && ( internalSheet || aIncludeExternSheets ) )
        {
            wxFileName dest = src;

            if( internalSheet && dest.MakeRelativeTo( aOldRoot.GetPath() ) )
                dest.MakeAbsolute( aNewRoot.GetPath() );
            else
                dest.Assign( aNewRoot.GetPath(), dest.GetFullName() );

            wxLogTrace( tracePathsAndFiles,
                        wxS( "Moving schematic from '%s' to '%s'." ),
                        screen->GetFileName(),
                        dest.GetFullPath() );

            if( !dest.DirExists() && !dest.Mkdir() )
            {
                aErrorMsg.Printf( _( "Folder '%s' could not be created.\n\n"
                                     "Make sure you have write permissions and try again." ),
                                 dest.GetPath() );
                return false;
            }

            if( aSaveCopy )
                aFilenameMap[screen] = dest.GetFullPath();
            else
                screen->SetFileName( dest.GetFullPath() );
        }
        else
        {
            if( aSaveCopy )
                aFilenameMap[screen] = wxString();

            screen->SetFileName( src.GetFullPath() );
        }
    }

    for( SCH_SHEET_PATH& sheet : aSchematic.Hierarchy() )
    {
        if( !sheet.Last()->IsTopLevelSheet() )
            sheet.MakeFilePathRelativeToParentSheet();
    }

    return true;
}

bool SCH_EDIT_FRAME::SaveProject( bool aSaveAs )
{
    wxString msg;
    SCH_SCREEN* screen;
    SCH_SCREENS screens( Schematic().Root() );
    bool        saveCopy          = aSaveAs && !Kiface().IsSingle();
    bool        success           = true;
    bool        updateFileHistory = false;
    bool        createNewProject  = false;
    bool        copySubsheets     = false;
    bool        includeExternSheets = false;

    // I want to see it in the debugger, show me the string!  Can't do that with wxFileName.
    wxString    fileName = Prj().AbsolutePath( Schematic().Root().GetFileName() );
    wxFileName  fn = fileName;

    // Path to save each screen to: will be the stored filename by default, but is overwritten by
    // a Save As Copy operation.
    std::unordered_map<SCH_SCREEN*, wxString> filenameMap;

    // Handle "Save As" and saving a new project/schematic for the first time in standalone
    if( Prj().IsNullProject() || aSaveAs )
    {
        // Null project should only be possible in standalone mode.
        wxCHECK( Kiface().IsSingle() || aSaveAs, false );

        wxFileName newFileName;
        wxFileName savePath( Prj().GetProjectFullName() );

        if( !savePath.IsOk() || !savePath.IsDirWritable() )
        {
            savePath = GetMruPath();

            if( !savePath.IsOk() || !savePath.IsDirWritable() )
                savePath = PATHS::GetDefaultUserProjectsPath();
        }

        if( savePath.HasExt() )
            savePath.SetExt( FILEEXT::KiCadSchematicFileExtension );
        else
            savePath.SetName( wxEmptyString );

        wxFileDialog dlg( this, _( "Schematic Files" ), savePath.GetPath(), savePath.GetFullName(),
                          FILEEXT::KiCadSchematicFileWildcard(),
                          wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

        FILEDLG_HOOK_SAVE_PROJECT newProjectHook;

        // Add a "Create a project" checkbox in standalone mode and one isn't loaded
        if( Kiface().IsSingle() || aSaveAs )
        {
            dlg.SetCustomizeHook( newProjectHook );
        }

        KIPLATFORM::UI::AllowNetworkFileSystems( &dlg );

        if( dlg.ShowModal() == wxID_CANCEL )
            return false;

        newFileName = EnsureFileExtension( dlg.GetPath(), FILEEXT::KiCadSchematicFileExtension );

        if( ( !newFileName.DirExists() && !newFileName.Mkdir() ) ||
            !newFileName.IsDirWritable() )
        {
            msg.Printf( _( "Folder '%s' could not be created.\n\n"
                           "Make sure you have write permissions and try again." ),
                        newFileName.GetPath() );

            KICAD_MESSAGE_DIALOG dlgBadPath( this, msg, _( "Error" ),
                                             wxOK | wxICON_EXCLAMATION | wxCENTER );

            dlgBadPath.ShowModal();
            return false;
        }

        if( newProjectHook.IsAttachedToDialog() )
        {
            createNewProject = newProjectHook.GetCreateNewProject();
            copySubsheets = newProjectHook.GetCopySubsheets();
            includeExternSheets = newProjectHook.GetIncludeExternSheets();
        }

        SCH_SCREEN* rootScreenForSaveAs = Schematic().RootScreen();
        
        if( !saveCopy )
        {
            Schematic().Root().SetFileName( newFileName.GetFullName() );
            if( rootScreenForSaveAs )
                rootScreenForSaveAs->SetFileName( newFileName.GetFullPath() );
            updateFileHistory = true;
        }
        else
        {
            if( rootScreenForSaveAs )
                filenameMap[rootScreenForSaveAs] = newFileName.GetFullPath();
        }

        if( !PrepareSaveAsFiles( Schematic(), screens, fn, newFileName, saveCopy,
                                 copySubsheets, includeExternSheets, filenameMap, msg ) )
        {
            KICAD_MESSAGE_DIALOG dlgBadFilePath( this, msg, _( "Error" ),
                                                 wxOK | wxICON_EXCLAMATION | wxCENTER );

            dlgBadFilePath.ShowModal();
            return false;
        }
    }
    else if( !fn.FileExists() )
    {
        // File doesn't exist yet; true if we just imported something
        updateFileHistory = true;
    }
    else if( screens.GetFirst() && screens.GetFirst()->GetFileFormatVersionAtLoad() < SEXPR_SCHEMATIC_FILE_VERSION )
    {
        // Allow the user to save un-edited files in new format
    }
    else if( !IsContentModified() )
    {
        return true;
    }

    if( filenameMap.empty() || !saveCopy )
    {
        for( size_t i = 0; i < screens.GetCount(); i++ )
            filenameMap[screens.GetScreen( i )] = screens.GetScreen( i )->GetFileName();
    }

    // Ensure root screen is always in the map (safety check for hierarchy issues)
    SCH_SCREEN* rootScreen = Schematic().RootScreen();
    if( rootScreen && filenameMap.find( rootScreen ) == filenameMap.end() )
    {
        filenameMap[rootScreen] = rootScreen->GetFileName();
    }

    // Warn user on potential file overwrite.  This can happen on shared sheets.
    wxArrayString overwrittenFiles;
    wxArrayString lockedFiles;

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        screen = screens.GetScreen( i );

        wxCHECK2( screen, continue );

        // Convert legacy schematics file name extensions for the new format.
        wxFileName tmpFn = filenameMap[screen];

        if( !tmpFn.IsOk() )
            continue;

        if( tmpFn.FileExists() && !tmpFn.IsFileWritable() )
            lockedFiles.Add( tmpFn.GetFullPath() );

        if( tmpFn.GetExt() == FILEEXT::KiCadSchematicFileExtension )
            continue;

        tmpFn.SetExt( FILEEXT::KiCadSchematicFileExtension );

        if( tmpFn.FileExists() )
            overwrittenFiles.Add( tmpFn.GetFullPath() );
    }

    if( !lockedFiles.IsEmpty() )
    {
        for( const wxString& lockedFile : lockedFiles )
        {
            if( msg.IsEmpty() )
                msg = lockedFile;
            else
                msg += "\n" + lockedFile;
        }

        wxRichMessageDialog dlg( this, wxString::Format( _( "Failed to save %s." ),
                                                         Schematic().Root().GetFileName() ),
                                 _( "Locked File Warning" ),
                                 wxOK | wxICON_WARNING | wxCENTER );
        dlg.SetExtendedMessage( _( "You do not have write permissions to:\n\n" ) + msg );

        dlg.ShowModal();
        return false;
    }

    if( !overwrittenFiles.IsEmpty() )
    {
        for( const wxString& overwrittenFile : overwrittenFiles )
        {
            if( msg.IsEmpty() )
                msg = overwrittenFile;
            else
                msg += "\n" + overwrittenFile;
        }

        wxRichMessageDialog dlg( this, _( "Saving will overwrite existing files." ),
                                 _( "Save Warning" ),
                                 wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxCENTER |
                                 wxICON_EXCLAMATION );
        dlg.ShowDetailedText( _( "The following files will be overwritten:\n\n" ) + msg );
        dlg.SetOKCancelLabels( KICAD_MESSAGE_DIALOG::ButtonLabel( _( "Overwrite Files" ) ),
                               KICAD_MESSAGE_DIALOG::ButtonLabel( _( "Abort Project Save" ) ) );

        if( dlg.ShowModal() == wxID_CANCEL )
            return false;
    }

    screens.BuildClientSheetPathList();

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        screen = screens.GetScreen( i );

        wxCHECK2( screen, continue );

        // Convert legacy schematics file name extensions for the new format.
        wxFileName tmpFn = filenameMap[screen];

        if( tmpFn.IsOk() && tmpFn.GetExt() != FILEEXT::KiCadSchematicFileExtension )
        {
            updateFileHistory = true;
            tmpFn.SetExt( FILEEXT::KiCadSchematicFileExtension );

            for( EDA_ITEM* item : screen->Items().OfType( SCH_SHEET_T ) )
            {
                SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );
                wxFileName sheetFileName = sheet->GetFileName();

                if( !sheetFileName.IsOk()
                    || sheetFileName.GetExt() == FILEEXT::KiCadSchematicFileExtension )
                    continue;

                sheetFileName.SetExt( FILEEXT::KiCadSchematicFileExtension );
                sheet->SetFileName( sheetFileName.GetFullPath() );
                UpdateItem( sheet );
            }

            filenameMap[screen] = tmpFn.GetFullPath();

            if( !saveCopy )
                screen->SetFileName( tmpFn.GetFullPath() );
        }

        // Do not save sheet symbols with no valid filename set
        if( !tmpFn.IsOk() )
            continue;

        std::vector<SCH_SHEET_PATH>& sheets = screen->GetClientSheetPaths();

        if( sheets.size() == 1 )
            screen->SetVirtualPageNumber( 1 );
        else
            screen->SetVirtualPageNumber( 0 );  // multiple uses; no way to store the real sheet #

        // This is a new schematic file so make sure it has a unique ID.
        if( !saveCopy && tmpFn.GetFullPath() != screen->GetFileName() )
            screen->AssignNewUuid();

        success &= saveSchematicFile( screens.GetSheet( i ), tmpFn.GetFullPath() );
    }

    if( success )
        m_autoSaveRequired = false;

    SCH_SCREEN* rootScreenForLock = Schematic().RootScreen();
    
    if( aSaveAs && success && rootScreenForLock )
        LockFile( rootScreenForLock->GetFileName() );

    if( updateFileHistory && rootScreenForLock )
        UpdateFileHistory( rootScreenForLock->GetFileName() );

    // Save the sheet name map to the project file
    std::vector<FILE_INFO_PAIR>& sheets = Prj().GetProjectFile().GetSheets();
    sheets.clear();

    for( SCH_SHEET_PATH& sheetPath : Schematic().Hierarchy() )
    {
        SCH_SHEET* sheet = sheetPath.Last();

        wxCHECK2( sheet, continue );

        // Do not save the virtual root sheet
        if( !sheet->IsVirtualRootSheet() )
        {
            sheets.emplace_back( std::make_pair( sheet->m_Uuid, sheet->GetName() ) );
        }
    }

    // Get the project path from the root screen's filename
    SCH_SCREEN* rootScreenForPath = Schematic().RootScreen();
    wxFileName projectPath;
    
    if( rootScreenForPath && filenameMap.count( rootScreenForPath ) )
    {
        projectPath = filenameMap.at( rootScreenForPath );
    }
    else if( !filenameMap.empty() )
    {
        // Fallback: use the first screen's filename
        projectPath = filenameMap.begin()->second;
    }
    else
    {
        // Last resort: use the project's full name
        projectPath = Prj().GetProjectFullName();
    }
    
    projectPath.SetExt( FILEEXT::ProjectFileExtension );

    if( Prj().IsNullProject() || ( aSaveAs && !saveCopy ) )
    {
        Prj().SetReadOnly( !createNewProject );
        GetSettingsManager()->SaveProjectAs( projectPath.GetFullPath() );
    }
    else if( saveCopy && createNewProject )
    {
        GetSettingsManager()->SaveProjectCopy( projectPath.GetFullPath() );
    }
    else
    {
        SaveProjectLocalSettings();
        saveProjectSettings();
    }

    if( !Kiface().IsSingle() )
    {
        WX_STRING_REPORTER backupReporter;

        if( !GetSettingsManager()->TriggerBackupIfNeeded( backupReporter ) )
            SetStatusText( backupReporter.GetMessages(), 0 );
    }

    updateTitle();

    if( m_infoBar->GetMessageType() == WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE )
        m_infoBar->Dismiss();

    return success;
}


bool SCH_EDIT_FRAME::importFile( const wxString& aFileName, int aFileType,
                                 const std::map<std::string, UTF8>* aProperties )
{
    wxFileName             filename( aFileName );
    wxFileName             newfilename;
    SCH_IO_MGR::SCH_FILE_T fileType = (SCH_IO_MGR::SCH_FILE_T) aFileType;

    wxCommandEvent changingEvt( EDA_EVT_SCHEMATIC_CHANGING );
    ProcessEventLocally( changingEvt );

    if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( GetStatusBar() ) )
        statusBar->ClearLoadWarningMessages();

    WX_STRING_REPORTER loadReporter;
    LOAD_INFO_REPORTER_SCOPE loadReporterScope( &loadReporter );

    std::unique_ptr<SCHEMATIC> newSchematic = std::make_unique<SCHEMATIC>( &Prj() );

    switch( fileType )
    {
    case SCH_IO_MGR::SCH_ALTIUM:
    case SCH_IO_MGR::SCH_CADSTAR_ARCHIVE:
    case SCH_IO_MGR::SCH_EAGLE:
    case SCH_IO_MGR::SCH_LTSPICE:
    case SCH_IO_MGR::SCH_EASYEDA:
    case SCH_IO_MGR::SCH_EASYEDAPRO:
    case SCH_IO_MGR::SCH_PADS:
    case SCH_IO_MGR::SCH_GEDA:
    {
        // We insist on caller sending us an absolute path, if it does not, we say it's a bug.
        // Unless we are passing the files in aproperties, in which case aFileName can be empty.
        wxCHECK_MSG( aFileName.IsEmpty() || filename.IsAbsolute(), false,
                     wxS( "Import schematic: path is not absolute!" ) );

        try
        {
            IO_RELEASER<SCH_IO>  pi( SCH_IO_MGR::FindPlugin( fileType ) );
            DIALOG_HTML_REPORTER errorReporter( this );
            WX_PROGRESS_REPORTER progressReporter( this, _( "Import Schematic" ), 1, PR_CAN_ABORT );

            if( PROJECT_CHOOSER_PLUGIN* c_pi = dynamic_cast<PROJECT_CHOOSER_PLUGIN*>( pi.get() ) )
            {
                c_pi->RegisterCallback( std::bind( DIALOG_IMPORT_CHOOSE_PROJECT::RunModal,
                                                   this, std::placeholders::_1 ) );
            }

            if( eeconfig()->m_System.show_import_issues )
                pi->SetReporter( errorReporter.m_Reporter );
            else
                pi->SetReporter( &NULL_REPORTER::GetInstance() );

            pi->SetProgressReporter( &progressReporter );

            SCH_SHEET* loadedSheet = pi->LoadSchematicFile( aFileName, newSchematic.get(), nullptr,
                                                            aProperties );

            SetSchematic( newSchematic.release() );

            if( loadedSheet )
            {
                Schematic().SetTopLevelSheets( { loadedSheet } );

                if( errorReporter.m_Reporter->HasMessage() )
                {
                    errorReporter.m_Reporter->Flush(); // Build HTML messages
                    errorReporter.ShowModal();
                }

                // Non-KiCad schematics do not use a drawing-sheet (or if they do, it works
                // differently to KiCad), so set it to an empty one.
                DS_DATA_MODEL& drawingSheet = DS_DATA_MODEL::GetTheInstance();
                drawingSheet.SetEmptyLayout();
                BASE_SCREEN::m_DrawingSheetFileName = "empty.kicad_wks";

                newfilename.SetPath( Prj().GetProjectPath() );
                newfilename.SetName( Prj().GetProjectName() );
                newfilename.SetExt( FILEEXT::KiCadSchematicFileExtension );

                SetScreen( Schematic().RootScreen() );

                if( SCH_SHEET* topSheet = Schematic().GetTopLevelSheet() )
                    topSheet->SetFileName( newfilename.GetFullName() );

                GetScreen()->SetFileName( newfilename.GetFullPath() );
                GetScreen()->SetContentModified();

                progressReporter.Report( _( "Updating connections..." ) );

                if( !progressReporter.KeepRefreshing() )
                    THROW_IO_ERROR( _( "File import canceled by user." ) );

                RecalculateConnections( nullptr, GLOBAL_CLEANUP, &progressReporter );

                // Only perform the dangling end test on root sheet.
                GetScreen()->TestDanglingEnds();
            }
            else
            {
                CreateDefaultScreens();
            }
        }
        catch( const IO_ERROR& ioe )
        {
            // Do not leave g_RootSheet == NULL because it is expected to be
            // a valid sheet. Therefore create a dummy empty root sheet and screen.
            CreateDefaultScreens();
            m_toolManager->RunAction( ACTIONS::zoomFitScreen );

            wxString msg = wxString::Format( _( "Error loading schematic '%s'." ), aFileName );
            DisplayErrorMessage( this, msg, ioe.What() );

            msg.Printf( _( "Failed to load '%s'." ), aFileName );
            SetMsgPanel( wxEmptyString, msg );
        }
        catch( const std::exception& exc )
        {
            CreateDefaultScreens();
            m_toolManager->RunAction( ACTIONS::zoomFitScreen );

            wxString msg = wxString::Format( _( "Unhandled exception occurred loading schematic "
                                                "'%s'." ), aFileName );
            DisplayErrorMessage( this, msg, exc.what() );

            msg.Printf( _( "Failed to load '%s'." ), aFileName );
            SetMsgPanel( wxEmptyString, msg );
        }

        ClearUndoRedoList();
        ClearRepeatItemsList();

        initScreenZoom();
        SetSheetNumberAndCount();
        SyncView();

        UpdateHierarchyNavigator( false, true );

        wxCommandEvent e( EDA_EVT_SCHEMATIC_CHANGED );
        ProcessEventLocally( e );

        for( wxEvtHandler* listener : m_schematicChangeListeners )
        {
            wxCHECK2( listener, continue );

            // Use the windows variant when handling event messages in case there is any
            // special event handler pre and/or post processing specific to windows.
            wxWindow* win = dynamic_cast<wxWindow*>( listener );

            if( win )
                win->HandleWindowEvent( e );
            else
                listener->SafelyProcessEvent( e );
        }

        updateTitle();

        if( KISTATUSBAR* statusBar = dynamic_cast<KISTATUSBAR*>( GetStatusBar() ) )
            statusBar->SetLoadWarningMessages( loadReporter.GetMessages() );

        break;
    }

    default:
        break;
    }

    return true;
}


bool SCH_EDIT_FRAME::AskToSaveChanges()
{
    SCH_SCREENS screenList( Schematic().Root() );

    // Save any currently open and modified project files.
    for( SCH_SCREEN* screen = screenList.GetFirst(); screen; screen = screenList.GetNext() )
    {
        SIMULATOR_FRAME* simFrame = (SIMULATOR_FRAME*) Kiway().Player( FRAME_SIMULATOR, false );

        // Simulator must be closed before loading another schematic, otherwise it may crash.
        // If there are any changes in the simulator the user will be prompted to save them.
        if( simFrame && !simFrame->Close() )
            return false;

        if( screen->IsContentModified() )
        {
            if( !HandleUnsavedChanges( this, _( "The current schematic has been modified.  "
                                                "Save changes?" ),
                                       [&]() -> bool
                                       {
                                           return SaveProject();
                                       } ) )
            {
                return false;
            }
        }
    }

    return true;
}
