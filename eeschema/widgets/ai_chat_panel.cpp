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

#include "ai_chat_panel.h"
#include <ai_backend_client.h>
#include <amplitude_client.h>
#include <iostream>
#include <set>
#include <sch_edit_frame.h>
#include <project/project_file.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <local_history.h>
#include <kiway.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/tglbtn.h>
#include <wx/txtstrm.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/base64.h>
#include <wx/dcclient.h>
#include <vector>
#include <wx/settings.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/defs.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>
#include <wx/app.h>

#include <auth/auth_manager.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_io/sch_io.h>
#include <io/io_utils.h>
#include <gestfich.h>
#include <kiplatform/secrets.h>
#include <paths.h>
#include <python_manager.h>
#include <kiway_player.h>
#include <wildcards_and_files_ext.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <future>
#include <wx/utils.h>
#include <bitmaps.h>
#include <class_draw_panel_gal.h>
#include <sch_plotter.h>
#include <plotters/plotter.h>
#include <settings/color_settings.h>
#include <trace_helpers.h>
#include <sch_commit.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_parser.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <richio.h>
#include <lib_symbol.h>
#include <sch_pin.h>
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <sch_signal_path_tracer.h>
#include <tools/sch_actions.h>
#include <tools/sch_tool_utils.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <sch_reference_list.h>


AI_CHAT_PANEL::AI_CHAT_PANEL( wxWindow* aParent, SCH_EDIT_FRAME* aFrame ) :
        AI_CHAT_PANEL_BASE( aParent, aFrame )
{
    // Set up ERC callback for direct access to violations
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetErcCallback( [this, aFrame]() -> nlohmann::json
    {
        wxLogTrace( traceAiToolCall, wxT( "AI SCH: ERC callback invoked" ) );

        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, promise]()
        {
            try
            {
                nlohmann::json result = aFrame->runErcAndSerialize();
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "ERC failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "ERC failed due to unknown error";
                promise->set_value( error );
            }
        } );

        // Wait for result from main thread (with timeout)
        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "ERC timed out";
            return error;
        }
    } );
    
    // Set up Annotate callback for direct access to annotation functionality
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetAnnotateCallback( [this, aFrame]( const nlohmann::json& aOptions ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        // Capture options by value for thread safety
        nlohmann::json optionsCopy = aOptions;

        CallAfter( [aFrame, promise, optionsCopy]()
        {
            try
            {
                // Skip undo: during AI streaming, ReloadFromFile will invalidate
                // any items that the commit references. The final AI edit undo
                // entry at stream-end captures the cumulative diff.
                nlohmann::json result = aFrame->runAnnotateAndSerialize( optionsCopy,
                                                                         /*aSkipUndo=*/true );
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Annotate failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Annotate failed due to unknown error";
                promise->set_value( error );
            }
        } );

        // Wait for result from main thread (with timeout)
        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Annotate timed out";
            return error;
        }
    } );
    
    // Set up fetch_component_pins callback for querying symbol pin data from local libraries
    SetFetchPinsCallback( [this, aFrame]( const std::string& aSymbolId ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        std::string symbolIdCopy = aSymbolId;

        CallAfter( [aFrame, promise, symbolIdCopy]()
        {
            try
            {
                // Parse "LibNickname:SymbolName" into a LIB_ID
                LIB_ID libId;
                if( libId.Parse( symbolIdCopy ) != -1 )
                {
                    nlohmann::json error;
                    error["error"] = "Invalid symbol ID format: " + symbolIdCopy;
                    promise->set_value( error );
                    return;
                }

                SYMBOL_LIBRARY_ADAPTER* adapter =
                        PROJECT_SCH::SymbolLibAdapter( &aFrame->Prj() );

                if( !adapter )
                {
                    nlohmann::json error;
                    error["error"] = "Symbol library adapter not available";
                    promise->set_value( error );
                    return;
                }

                LIB_SYMBOL* libSymbol = adapter->LoadSymbol( libId );

                if( !libSymbol )
                {
                    nlohmann::json error;
                    error["error"] = "Symbol '" + symbolIdCopy + "' not found in library";
                    promise->set_value( error );
                    return;
                }

                std::vector<SCH_PIN*> allPins = libSymbol->GetPins();

                // Build pin list and group by unit
                nlohmann::json pinsArray = nlohmann::json::array();
                std::map<int, nlohmann::json> unitPins;
                std::set<wxString> seenNumbers;

                for( const SCH_PIN* pin : allPins )
                {
                    wxString number = pin->GetNumber();

                    nlohmann::json pinObj;
                    pinObj["number"] = number.ToStdString();
                    pinObj["name"]   = pin->GetName().ToStdString();
                    pinObj["type"]   = pin->GetCanonicalElectricalTypeName().ToStdString();

                    int unit = pin->GetUnit();
                    if( unit < 1 )
                        unit = 1;

                    if( !unitPins.count( unit ) )
                        unitPins[unit] = nlohmann::json::array();

                    unitPins[unit].push_back( pinObj );

                    if( seenNumbers.find( number ) == seenNumbers.end() )
                    {
                        seenNumbers.insert( number );
                        pinsArray.push_back( pinObj );
                    }
                }

                nlohmann::json unitsArray = nlohmann::json::array();
                for( auto& [u, pins] : unitPins )
                {
                    nlohmann::json unitObj;
                    unitObj["unit"] = u;
                    unitObj["pins"] = pins;
                    unitsArray.push_back( unitObj );
                }

                nlohmann::json result;
                result["symbol"]     = symbolIdCopy;
                result["total_pins"] = static_cast<int>( pinsArray.size() );
                result["pins"]       = pinsArray;
                result["units"]      = unitsArray;

                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to fetch pins: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Failed to fetch pins due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 15 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Fetch pins timed out";
            return error;
        }
    } );

    // Set up trace_signal_path callback for tracing electrical connectivity
    SetTraceSignalPathCallback(
            [this, aFrame]( const nlohmann::json& aArgs ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        auto argsCopy = aArgs;

        CallAfter( [aFrame, promise, argsCopy]()
        {
            try
            {
                wxString netName = argsCopy.value( "net_name", "" );
                wxString component = argsCopy.value( "component", "" );
                wxString pinName = argsCopy.value( "pin_name", "" );
                int maxHops = argsCopy.value( "max_hops", 3 );

                SCH_SIGNAL_PATH_TRACER tracer( &aFrame->Schematic() );

                if( !component.IsEmpty() )
                    promise->set_value( tracer.TraceFromComponent( component, pinName, maxHops ) );
                else
                    promise->set_value( tracer.TraceFromNet( netName, maxHops ) );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Signal path trace failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Signal path trace failed due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Signal path trace timed out";
            return error;
        }
    } );

    // highlight_net callback: highlights a net by name in the schematic
    SetHighlightNetCallback(
            [this, aFrame]( const nlohmann::json& aArgs ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        auto argsCopy = aArgs;

        CallAfter( [aFrame, promise, argsCopy]()
        {
            try
            {
                wxString netName = argsCopy.value( "net_name", "" );
                bool clearExisting = argsCopy.value( "clear_existing", true );

                if( clearExisting || netName.IsEmpty() )
                    aFrame->SetHighlightedConnection( wxEmptyString );

                if( !netName.IsEmpty() )
                    aFrame->SetHighlightedConnection( netName );

                aFrame->GetToolManager()->RunAction( SCH_ACTIONS::updateNetHighlighting );
                aFrame->GetCanvas()->Refresh();

                nlohmann::json result;
                result["success"] = true;
                result["net"] = std::string( netName.ToUTF8() );
                result["editor"] = "eeschema";
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Highlight net failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Highlight net failed due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Highlight net timed out";
            return error;
        }
    } );

    // select_items callback: selects items by reference designator in the schematic
    SetSelectItemsCallback(
            [this, aFrame]( const nlohmann::json& aArgs ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        auto argsCopy = aArgs;

        CallAfter( [aFrame, promise, argsCopy]()
        {
            try
            {
                auto refs = argsCopy.value( "references", std::vector<std::string>{} );
                bool zoomToFit = argsCopy.value( "zoom_to_fit", false );
                bool clearExisting = argsCopy.value( "clear_existing", true );

                if( clearExisting )
                    aFrame->GetToolManager()->RunAction( ACTIONS::selectionClear );

                nlohmann::json selected = nlohmann::json::array();
                nlohmann::json notFound = nlohmann::json::array();

                for( const auto& ref : refs )
                {
                    auto found = FindSymbolByRefAndUnit( aFrame->Schematic(),
                                                         wxString( ref ), 1 );
                    if( found.has_value() )
                    {
                        SCH_SYMBOL* sym = found->GetSymbol();
                        aFrame->GetToolManager()->RunAction( ACTIONS::selectItem, sym );
                        selected.push_back( ref );
                    }
                    else
                    {
                        notFound.push_back( ref );
                    }
                }

                if( zoomToFit && !selected.empty() )
                    aFrame->GetToolManager()->RunAction( ACTIONS::zoomFitSelection );

                nlohmann::json result;
                result["selected"] = selected;
                result["not_found"] = notFound;
                result["total_selected"] = selected.size();
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Select items failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Select items failed due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Select items timed out";
            return error;
        }
    } );

    // Set up snapshot callback for direct snapshot generation
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    // CRITICAL: Plotting code (SCH_PLOTTER) MUST run on the main UI thread
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetSnapshotCallback( [this]() -> std::string
    {
        auto promise = std::make_shared<std::promise<std::string>>();
        std::future<std::string> future = promise->get_future();

        CallAfter( [this, promise]()
        {
            try
    {
        SCH_EDIT_FRAME* frame = GetSchFrame();
        if( !frame )
                {
                    promise->set_value( "" );
                    return;
                }
        
        // Create temporary file
        wxString tempFile = wxFileName::CreateTempFileName( wxT( "schematic_snapshot_" ) );
        if( tempFile.IsEmpty() )
                {
                    promise->set_value( "" );
                    return;
                }
        
                // Generate snapshot to temp file (now on main thread - safe!)
        if( !GenerateSnapshot( tempFile ) )
        {
            wxRemoveFile( tempFile );
                    promise->set_value( "" );
                    return;
        }
        
        // Read file and base64 encode
        wxFile svgFile( tempFile, wxFile::read );
        if( !svgFile.IsOpened() )
        {
            wxRemoveFile( tempFile );
                    promise->set_value( "" );
                    return;
        }
        
        wxFileOffset fileSize = svgFile.Length();
        std::vector<char> buffer( fileSize );
        svgFile.Read( buffer.data(), fileSize );
        svgFile.Close();
        
        wxString base64 = wxBase64Encode( buffer.data(), buffer.size() );
        
        // Cleanup
        wxRemoveFile( tempFile );
        
                promise->set_value( base64.ToStdString() );
            }
            catch( const std::exception& e )
            {
                promise->set_value( "" );
            }
            catch( ... )
            {
                promise->set_value( "" );
            }
        } );

        // Wait for result from main thread (with timeout)
        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            return "";
        }
    } );

    // Set up Hierarchy callback for get_hierarchy tool
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    SetHierarchyCallback( [this, aFrame]() -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, promise]()
        {
            try
            {
                nlohmann::json result;
                
                // Safety checks
                if( !aFrame || !aFrame->Schematic().IsValid() )
                {
                    result["error"] = "Schematic not valid";
                    promise->set_value( result );
                    return;
                }
                
                SCH_SHEET_LIST hierarchy = aFrame->Schematic().Hierarchy();
                
                if( hierarchy.empty() )
                {
                    result["error"] = "No sheets in hierarchy";
                    promise->set_value( result );
                    return;
                }
                
                // Get current sheet info
                SCH_SHEET_PATH& currentSheet = aFrame->GetCurrentSheet();
                nlohmann::json currentInfo;
                
                if( currentSheet.Last() )
                {
                    currentInfo["name"] = currentSheet.Last()->GetName().ToStdString();
                    
                    // Get page number, defaulting to "1" if not set
                    wxString currentPageNum = currentSheet.GetPageNumber();
                    if( currentPageNum.IsEmpty() )
                        currentPageNum = "1";
                    currentInfo["page_number"] = currentPageNum.ToStdString();
                    
                    currentInfo["path"] = currentSheet.PathHumanReadable( true, true ).ToStdString();
                    
                    SCH_SCREEN* screen = currentSheet.LastScreen();
                    if( screen )
                        currentInfo["file"] = screen->GetFileName().ToStdString();
                }
                result["current_sheet"] = currentInfo;
                
                // Build sheets array
                nlohmann::json sheets = nlohmann::json::array();
                int sheetIndex = 0;
                for( const SCH_SHEET_PATH& path : hierarchy )
                {
                    nlohmann::json sheetInfo;
                    
                    SCH_SHEET* sheet = path.Last();
                    if( !sheet )
                        continue;
                    
                    sheetInfo["name"] = sheet->GetName().ToStdString();
                    
                    // Get page number, defaulting to index+1 if not set
                    wxString pageNum = path.GetPageNumber();
                    if( pageNum.IsEmpty() )
                        pageNum = wxString::Format( "%d", sheetIndex + 1 );
                    sheetInfo["page_number"] = pageNum.ToStdString();
                    
                    sheetInfo["path"] = path.PathHumanReadable( true, true ).ToStdString();
                    
                    SCH_SCREEN* screen = path.LastScreen();
                    if( screen )
                        sheetInfo["file"] = screen->GetFileName().ToStdString();
                    
                    sheets.push_back( sheetInfo );
                    sheetIndex++;
                }
                result["sheets"] = sheets;
                result["total_sheets"] = hierarchy.size();
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to get hierarchy: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Unknown error getting hierarchy";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["error"] = "Hierarchy request timed out";
        return error;
    } );

    // Set up Switch Sheet callback for switch_sheet tool
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    SetSwitchSheetCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        nlohmann::json paramsCopy = aParams;

        CallAfter( [aFrame, promise, paramsCopy]()
        {
            try
            {
                nlohmann::json result;
                
                // Safety checks
                if( !aFrame )
                {
                    result["success"] = false;
                    result["error"] = "Frame not available";
                    promise->set_value( result );
                    return;
                }
                
                if( !aFrame->Schematic().IsValid() )
                {
                    result["success"] = false;
                    result["error"] = "Schematic not valid";
                    promise->set_value( result );
                    return;
                }
                
                SCH_SHEET_LIST hierarchy = aFrame->Schematic().Hierarchy();
                
                if( hierarchy.empty() )
                {
                    result["success"] = false;
                    result["error"] = "No sheets in hierarchy";
                    promise->set_value( result );
                    return;
                }
                
                SCH_SHEET_PATH targetPath;
                bool found = false;
                
                // Priority 1: Find by page number (most reliable)
                if( paramsCopy.contains( "page_number" ) && !paramsCopy["page_number"].is_null() && paramsCopy["page_number"].is_string() )
                {
                    wxString pageNum = wxString::FromUTF8( paramsCopy["page_number"].get<std::string>() );
                    
                    int pathIndex = 0;
                    for( const SCH_SHEET_PATH& path : hierarchy )
                    {
                        wxString pathPageNum = path.GetPageNumber();
                        if( pathPageNum.IsEmpty() )
                            pathPageNum = wxString::Format( "%d", pathIndex + 1 );

                        if( pathPageNum == pageNum )
                        {
                            targetPath = path;
                            found = true;
                            break;
                        }
                        pathIndex++;
                    }
                    
                    if( !found )
                    {
                        result["success"] = false;
                        result["error"] = "Sheet with page number '" + pageNum.ToStdString() + "' not found";
                        promise->set_value( result );
                        return;
                    }
                }
                // Priority 2: Find by sheet name
                else if( paramsCopy.contains( "sheet_name" ) && !paramsCopy["sheet_name"].is_null() && paramsCopy["sheet_name"].is_string() )
                {
                    wxString name = wxString::FromUTF8( paramsCopy["sheet_name"].get<std::string>() );
                    
                    for( const SCH_SHEET_PATH& path : hierarchy )
                    {
                        SCH_SHEET* sheet = path.Last();
                        if( sheet && sheet->GetName() == name )
                        {
                            targetPath = path;
                            found = true;
                            break;
                        }
                    }
                    
                    if( !found )
                    {
                        result["success"] = false;
                        result["error"] = "Sheet named '" + name.ToStdString() + "' not found";
                        promise->set_value( result );
                        return;
                    }
                }
                // Priority 3: Find by path
                else if( paramsCopy.contains( "sheet_path" ) && !paramsCopy["sheet_path"].is_null() && paramsCopy["sheet_path"].is_string() )
                {
                    wxString pathStr = wxString::FromUTF8( paramsCopy["sheet_path"].get<std::string>() );
                    
                    for( const SCH_SHEET_PATH& path : hierarchy )
                    {
                        if( path.PathHumanReadable( true, true ) == pathStr )
                        {
                            targetPath = path;
                            found = true;
                            break;
                        }
                    }
                    
                    if( !found )
                    {
                        result["success"] = false;
                        result["error"] = "Sheet at path '" + pathStr.ToStdString() + "' not found";
                        promise->set_value( result );
                        return;
                    }
                }
                
                // Verify the target sheet has a valid screen before switching
                if( !targetPath.LastScreen() )
                {
                    result["success"] = false;
                    result["error"] = "Target sheet has no loaded screen (sub-sheet file may be missing or unloadable)";
                    promise->set_value( result );
                    return;
                }

                // Perform the sheet switch
                aFrame->SetCurrentSheet( targetPath );
                aFrame->DisplayCurrentSheet();
                aFrame->UpdateHierarchyNavigator();
                
                // Build success response
                result["success"] = true;
                
                SCH_SHEET* sheet = targetPath.Last();
                if( sheet )
                {
                    result["current_sheet"] = sheet->GetName().ToStdString();
                    
                    // Get page number, defaulting to "1" if not set
                    wxString pageNum = targetPath.GetPageNumber();
                    if( pageNum.IsEmpty() )
                        pageNum = "1";
                    result["page_number"] = pageNum.ToStdString();
                    
                    result["path"] = targetPath.PathHumanReadable( true, true ).ToStdString();
                    
                    SCH_SCREEN* screen = targetPath.LastScreen();
                    if( screen )
                        result["file"] = screen->GetFileName().ToStdString();
                }
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = std::string( "Failed to switch sheet: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = "Unknown error switching sheet";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["success"] = false;
        error["error"] = "Switch sheet request timed out";
        return error;
    } );

    // Set up manufacturer preference callback
    SetSetManufacturerCallback( [this, aFrame]( const std::string& aMfrId ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        std::string mfrIdCopy = aMfrId;

        CallAfter( [aFrame, promise, mfrIdCopy]()
        {
            try
            {
                aFrame->Prj().GetProjectFile().m_PreferredManufacturer = wxString( mfrIdCopy );
                aFrame->Prj().GetProjectFile().SaveToFile();

                // Sync the toolbar dropdown
                if( aFrame->m_currentManufacturerCtrl )
                {
                    int sel = 0;
                    if( mfrIdCopy == "pcbway" )     sel = 1;
                    else if( mfrIdCopy == "pikkolo" ) sel = 2;
                    aFrame->m_currentManufacturerCtrl->SetSelection( sel );
                }

                nlohmann::json result;
                result["status"] = "ok";
                result["manufacturer"] = mfrIdCopy;
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to set manufacturer: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready )
            return future.get();

        return nlohmann::json{ { "error", "Set manufacturer timed out" } };
    } );
}


AI_CHAT_PANEL::~AI_CHAT_PANEL()
{
}


SCH_EDIT_FRAME* AI_CHAT_PANEL::GetSchFrame() const
{
    return static_cast<SCH_EDIT_FRAME*>( GetFrame() );
}


bool AI_CHAT_PANEL::ReloadFromFile( const wxString& aFileName )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return false;
    
    return frame->ReloadSchematicFromFile( aFileName, /*aSilent=*/true );
}


bool AI_CHAT_PANEL::CaptureStateForAIEdit( const wxString& aFilePath )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return false;
    
    return frame->CaptureSchematicStateForAIEdit( aFilePath );
}


bool AI_CHAT_PANEL::CompareAndCreateAIEditUndoEntries()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return false;
    
    return frame->CompareAndCreateAIEditUndoEntries();
}


void AI_CHAT_PANEL::RemapUndoRedoAfterReload()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( frame )
        frame->RemapUndoRedoAfterReload();
}


std::unique_ptr<AI_COMMIT> AI_CHAT_PANEL::CreateAICommit()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return nullptr;

    auto commit = std::make_unique<AI_COMMIT>( false );

    commit->SetCommit( std::make_unique<SCH_COMMIT>( frame ) );

    commit->SetSchItemResolver( [frame]( const KIID& aUuid ) -> EDA_ITEM*
    {
        return static_cast<EDA_ITEM*>( frame->Schematic().ResolveItem( aUuid, nullptr, true ) );
    } );

    commit->SetScreenProvider( [frame]() -> BASE_SCREEN*
    {
        return static_cast<BASE_SCREEN*>( frame->GetScreen() );
    } );

    commit->SetSchItemParser( [frame]( const std::string& aSexp, BASE_SCREEN* aBaseScreen ) -> EDA_ITEM*
    {
        SCH_SCREEN* aScreen = dynamic_cast<SCH_SCREEN*>( aBaseScreen );
        try
        {
            STRING_LINE_READER reader( aSexp, wxT( "AI edit" ) );
            SCH_IO_KICAD_SEXPR_PARSER parser( &reader );

            SCH_SHEET tempSheet;
            SCH_SCREEN* tempScreen = new SCH_SCREEN( &frame->Schematic() );
            tempSheet.SetScreen( tempScreen );
            parser.ParseSchematic( &tempSheet, true );

            for( SCH_ITEM* item : tempScreen->Items() )
            {
                SCH_ITEM* copy = item->Duplicate( IGNORE_PARENT_GROUP, nullptr, true );
                if( copy && aScreen )
                    copy->SetParent( aScreen );
                return static_cast<EDA_ITEM*>( copy );
            }

            return nullptr;
        }
        catch( const std::exception& e )
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse schematic item: %s" ),
                         wxString::FromUTF8( e.what() ) );
            return nullptr;
        }
    } );

    // Set item swapper for MODIFY operations
    commit->SetItemSwapper( []( EDA_ITEM* aLive, EDA_ITEM* aNew )
    {
        SCH_ITEM* liveSch = dynamic_cast<SCH_ITEM*>( aLive );
        SCH_ITEM* newSch = dynamic_cast<SCH_ITEM*>( aNew );
        if( liveSch && newSch )
            liveSch->SwapItemData( newSch );
        delete aNew;
    } );

    // Set undo blocker
    commit->SetUndoBlocker( [frame]( bool aBlock )
    {
        frame->SetUndoRedoBlocked( aBlock );
    } );

    return commit;
}


void AI_CHAT_PANEL::AutoplaceModifiedSymbols( const std::set<std::string>& aModifiedUUIDs )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return;
    
    frame->AutoplaceModifiedSymbols( aModifiedUUIDs );
}


void AI_CHAT_PANEL::AnnotateAllSymbols()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return;
    
    nlohmann::json options;
    // During AI edits, skip pushing an undo entry for annotation.
    // The AI edit undo entry (from CompareAndCreateAIEditUndoEntries)
    // captures the full before/after diff including annotation changes.
    frame->runAnnotateAndSerialize( options, /*aSkipUndo=*/true );
}


bool AI_CHAT_PANEL::SaveDocument()
{
    wxLogTrace( traceFileSave, wxT( "AI SCH: SaveDocument called" ) );

    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
    {
        wxLogTrace( traceFileSave, wxT( "AI SCH: SaveDocument - no frame!" ) );
        return false;
    }
    
    wxLogDebug( wxT( "AI DEBUG [SaveDocument]: Calling frame->SaveProject()" ) );
    // Save the project to persist annotation changes
    bool result = frame->SaveProject( false );
    wxLogDebug( wxT( "AI DEBUG [SaveDocument]: SaveProject returned %s" ), result ? wxT( "true" ) : wxT( "false" ) );
    wxLogTrace( traceFileSave, wxT( "AI SCH: SaveDocument result=%d" ), result );
    return result;
}


void AI_CHAT_PANEL::MarkDocumentAsSaved()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return;
    
    // Safety check: ensure schematic is valid
    if( !frame->Schematic().IsValid() || !frame->Schematic().RootScreen() )
        return;
    
    // Mark all screens as not modified since the file on disk matches
    // the in-memory state after AI edits + reload
    SCH_SCREENS screens( frame->Schematic().Root() );
    for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
    {
        screen->SetContentModified( false );
    }
}


bool AI_CHAT_PANEL::GenerateSnapshot( const wxString& aOutputPath )
{
    return GenerateSchematicSnapshot( aOutputPath );
}


// NOTE: HandleBackendEvent was removed - dead code
// All event handling now goes through handleBackendEventDirect


void AI_CHAT_PANEL::HandleFileEditEvent( const AI_BACKEND_EVENT& aEvent, int aTabIndex )
        {
    if( !aEvent.fileModified )
        return;

    // During streaming: Queue for batch update (base class handles this)
    // This shows incremental changes periodically without file locking issues
    bool anyStreaming = isAnyTabStreaming();
    
    if( anyStreaming )
    {
        m_batchUpdatePending.store( true );
        
        // Start batch timer (base class will handle reload timing)
        AI_CHAT_PANEL_BASE::HandleFileEditEvent( aEvent, aTabIndex );
        return;
    }

    // Not streaming - can try incremental update immediately
    // Try incremental update if diff info available
    if( aEvent.hasDiffInfo && aEvent.diffType == "incremental" )
    {
        SCH_EDIT_FRAME* frame = GetSchFrame();
        if( frame )
        {
            bool incrementalSuccess = frame->ApplyIncrementalDiff( aEvent.diffInfo );
            
            if( incrementalSuccess )
            {
                // Incremental update succeeded - create undo entries immediately
                CompareAndCreateAIEditUndoEntries();
                return;
            }
            
        }
    }

    // Fall back to base class full reload
    AI_CHAT_PANEL_BASE::HandleFileEditEvent( aEvent, aTabIndex );
}


void AI_CHAT_PANEL::RequestVersionList()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return;

    wxString filePath = frame->GetCurrentFileName();
    if( filePath.IsEmpty() )
        return;

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
        return;

    // Use the backend client directly
    AI_BACKEND_CLIENT* client = GetBackendClient();
    if( !client )
        return;

    nlohmann::json versions = client->GetSchematicVersions(
            filePath.ToStdString(), authToken.ToStdString() );

    // Handle versions response via event (use current tab index)
    AI_BACKEND_EVENT event;
    event.type = AI_EVENT_TYPE::VERSIONS_LIST;
    event.data = versions;
    handleBackendEventDirect( event, m_currentTabIndex );
}


void AI_CHAT_PANEL::RestoreVersion( const wxString& versionId,
                                    std::function<void( bool )> aOnComplete )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame || versionId.IsEmpty() )
    {
        if( aOnComplete )
            aOnComplete( false );
        return;
    }

    for( size_t t = 0; t < m_tabs.size(); t++ )
    {
        if( static_cast<int>( t ) != m_currentTabIndex && m_tabs[t].isStreaming.load() )
        {
            wxLogWarning( wxT( "RestoreVersion called while tab %zu is streaming" ),
                          t );
        }
    }

    wxString filePath = frame->GetCurrentFileName();
    if( filePath.IsEmpty() )
    {
        if( aOnComplete )
            aOnComplete( false );
        return;
    }

    AMPLITUDE_CLIENT::Instance().Track( "version_restored", {
        { "app_type", "eeschema" }
    });

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
    {
        if( aOnComplete )
            aOnComplete( false );
        return;
    }

    wxString traceSchPath = filePath;
    if( traceSchPath.EndsWith( wxT( ".kicad_sch" ) ) )
        traceSchPath = traceSchPath.BeforeLast( wxT( '.' ) ) + wxT( ".trace_sch" );
    frame->CaptureSchematicStateForAIEdit( traceSchPath );

    AI_BACKEND_CLIENT* client = GetBackendClient();
    if( !client )
    {
        if( aOnComplete )
            aOnComplete( false );
        return;
    }

    m_chatWebview->ShowStatus( wxT( "Restoring version..." ) );

    auto panelAlive = m_panelAlive;
    std::string vidStr = versionId.ToStdString();
    std::string tspStr = traceSchPath.ToStdString();
    std::string authStr = authToken.ToStdString();
    AI_CHAT_PANEL* self = this;

    std::thread( [self, panelAlive, client, vidStr, tspStr, authStr, aOnComplete]()
    {
        bool success = client->RestoreSchematicVersion( vidStr, tspStr, authStr );

        if( panelAlive->load() )
        {
            self->CallAfter( [self, panelAlive, success, aOnComplete]()
            {
                if( !panelAlive->load() )
                    return;

                self->m_chatWebview->HideStatus();
                if( success )
                {
                    SCH_EDIT_FRAME* f = self->GetSchFrame();
                    if( f )
                        f->RefreshCanvas();
                }
                else
                {
                    self->m_chatWebview->ShowStatus(
                        wxT( "Remote restore failed. Use Edit > Undo (Ctrl+Z) "
                             "or Local History to restore manually." ) );
                }

                if( aOnComplete )
                    aOnComplete( success );
            });
        }
    }).detach();
}


void AI_CHAT_PANEL::saveVersionToDatabase( const wxString& aDescription )
{
    saveVersionToDatabase( aDescription, m_currentTabIndex );
}


void AI_CHAT_PANEL::saveVersionToDatabase( const wxString& aDescription, int aTabIndex )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return;

    if( aTabIndex < 0 || aTabIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    TAB_DATA& tab = m_tabs[aTabIndex];

    wxString filePath = frame->GetCurrentFileName();
    if( filePath.IsEmpty() )
        return;

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
        return;

    wxString traceSchPath = filePath;
    if( traceSchPath.EndsWith( wxT( ".kicad_sch" ) ) )
        traceSchPath = traceSchPath.BeforeLast( wxT( '.' ) ) + wxT( ".trace_sch" );
    else if( !traceSchPath.EndsWith( wxT( ".trace_sch" ) ) )
        return;

    AI_BACKEND_CLIENT* client = tab.backendClient.get();
    if( !client )
        client = GetBackendClient();
    if( !client )
        return;

    wxString convId = tab.conversationId;
    if( convId.IsEmpty() )
        convId = GetConversationId();

    std::string versionId = client->SaveSchematicVersion(
            traceSchPath.ToStdString(),
            aDescription.ToStdString(),
            convId.ToStdString(),
            authToken.ToStdString() );

    if( !versionId.empty() )
    {
        tab.lastSavedVersionId = wxString::FromUTF8( versionId );
    }

    // Also commit to local .history for offline fallback
    try
    {
        LOCAL_HISTORY& history = frame->Kiway().LocalHistory();
        wxString projectPath = frame->Prj().GetProjectPath();
        if( !projectPath.IsEmpty() )
        {
            history.CommitSnapshot( { traceSchPath }, wxT( "AI: " ) + aDescription );
        }
    }
    catch( ... )
    {
        wxLogDebug( "saveVersionToDatabase: local history commit failed (non-fatal)" );
    }
}


bool AI_CHAT_PANEL::GenerateSchematicSnapshot( const wxString& aOutputPath )
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return false;

    if( !frame->GetScreen() )
        return false;
    
    try
    {
        SCH_PLOTTER plotter( frame );
        
        SCH_RENDER_SETTINGS renderSettings( *frame->GetRenderSettings() );
        renderSettings.m_ShowHiddenPins = false;
        renderSettings.m_ShowHiddenFields = false;
        
        SCH_PLOT_OPTS plotOpts;
        plotOpts.m_plotAll = false;
        plotOpts.m_plotDrawingSheet = true;
        plotOpts.m_blackAndWhite = false;
        plotOpts.m_useBackgroundColor = true;
        plotOpts.m_theme = frame->GetColorSettings()->GetFilename();
        plotOpts.m_pageSizeSelect = PAGE_SIZE_AUTO;
        plotOpts.m_plotHopOver = frame->Schematic().Settings().m_HopOverScale > 0.0;
        
        wxFileName outputFile( aOutputPath );
        plotOpts.m_outputDirectory = outputFile.GetPath();
        plotOpts.m_outputFile = outputFile.GetName();
        
        plotter.Plot( PLOT_FORMAT::SVG, plotOpts, &renderSettings, nullptr );
        
        wxString lastOutputPath = plotter.GetLastOutputFilePath();
        if( !lastOutputPath.IsEmpty() && wxFile::Exists( lastOutputPath ) )
        {
            if( lastOutputPath != aOutputPath )
            {
                wxRenameFile( lastOutputPath, aOutputPath, true );
            }
            return true;
        }
        
        return false;
    }
    catch( const IO_ERROR& e )
    {
        // Log error but don't show message box (we're in background thread)
        return false;
    }
    catch( ... )
    {
        return false;
    }
}


wxString AI_CHAT_PANEL::GetCurrentFileName() const
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return wxEmptyString;
    
    return frame->GetCurrentFileName();
}


wxString AI_CHAT_PANEL::EnsureFileSavedForAI()
{
    SCH_EDIT_FRAME* frame = GetSchFrame();
    if( !frame )
        return wxEmptyString;

    // Safety check: ensure schematic is valid before accessing it
    if( !frame->Schematic().IsValid() || !frame->Schematic().RootScreen() )
        return wxEmptyString;

    wxString filePath = frame->GetCurrentFileName();
    if( filePath.IsEmpty() )
        return wxEmptyString;

    wxFileName fn( filePath );
    
    // Check if it's already an absolute path to an existing file
    if( fn.IsAbsolute() && fn.FileExists() )
    {
        // IMPORTANT: If document has unsaved changes, save them first!
        // This prevents user's manual edits from being lost when AI modifies the file.
        SCH_SCREENS screens( frame->Schematic().Root() );
        bool hasUnsavedChanges = false;
        for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
        {
            if( screen->IsContentModified() )
            {
                hasUnsavedChanges = true;
                break;
            }
        }
        
        if( hasUnsavedChanges )
        {
            if( frame->SaveProject() )
            {
                // Unsaved changes saved successfully
            }
            else
            {
                wxLogWarning( wxT( "AI: Failed to save unsaved changes, proceeding anyway" ) );
                // Continue anyway - user may have cancelled save dialog
            }
        }
        
        // File exists - make sure trace_sch exists too
        wxString traceSchPath = filePath;
        if( traceSchPath.EndsWith( wxT( ".kicad_sch" ) ) )
        {
            traceSchPath = traceSchPath.BeforeLast( wxT( '.' ) ) + wxT( ".trace_sch" );
            
            // If trace_sch doesn't exist, create it from kicad_sch
            if( !wxFileExists( traceSchPath ) )
            {
                ConvertKicadSchToTraceSch( filePath );
            }
        }
        return filePath;
    }

    // File is unsaved (just a name like "Untitled 1.kicad_sch")
    // Auto-save to a temp location
    wxString tempDir = wxStandardPaths::Get().GetDocumentsDir() + wxFileName::GetPathSeparator() 
                     + wxT( "Trace-Temp" );
    
    // Create temp directory if it doesn't exist
    if( !wxDirExists( tempDir ) )
    {
        wxMkdir( tempDir );
    }
    
    // Generate a unique directory name based on the schematic name
    wxString baseName = fn.GetName();  // e.g., "Untitled 1"
    wxString projectDir = tempDir + wxFileName::GetPathSeparator() + baseName;
    
    // Create project directory if it doesn't exist
    if( !wxDirExists( projectDir ) )
    {
        wxMkdir( projectDir );
    }
    
    // Build full path
    wxString fullPath = projectDir + wxFileName::GetPathSeparator() 
                      + baseName + wxT( ".kicad_sch" );
    
    // Save the schematic to this location (silent save, don't change current file path)
    // We use saveSchematicFile directly to avoid changing the project
    SCH_SHEET* rootSheet = nullptr;
    if( !frame->Schematic().GetTopLevelSheets().empty() )
        rootSheet = frame->Schematic().GetTopLevelSheets()[0];
    
    if( rootSheet && rootSheet->GetScreen() )
    {
        try
        {
            // Get the IO plugin for KiCad format
            IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
            
            // Save the schematic
            pi->SaveSchematicFile( fullPath, rootSheet, &frame->Schematic() );
            
            
            // Convert to trace_sch format
            ConvertKicadSchToTraceSch( fullPath );
            
            return fullPath;
        }
        catch( const IO_ERROR& e )
        {
            wxLogWarning( wxT( "AI: Failed to auto-save schematic: %s" ), e.What() );
            return wxEmptyString;
        }
    }
    
    return wxEmptyString;
}


wxString AI_CHAT_PANEL::GetAppType() const
{
    return wxT( "eeschema" );
}


wxString AI_CHAT_PANEL::ConvertToTraceFile( const wxString& aFilePath ) const
{
    wxFileName traceFn( aFilePath );
    if( traceFn.GetExt() == wxString::FromUTF8( FILEEXT::KiCadSchematicFileExtension ) )
    {
        traceFn.SetExt( wxString::FromUTF8( FILEEXT::TraceSchematicFileExtension ) );
        return traceFn.GetFullPath();
    }
    
    // If not a kicad_sch file, use as-is (fallback)
    return aFilePath;
}
