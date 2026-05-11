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
#include <pcb_edit_frame.h>
#include <project/project_file.h>
#include <tools/drc_tool.h>
#include <drc/drc_engine.h>
#include <board_commit.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr_parser.h>
#include <richio.h>
#include <zone.h>
#include <pcb_plotter.h>
#include <pcb_plot_params.h>
#include <wildcards_and_files_ext.h>
#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <base_units.h>
#include <pcb_track.h>
#include <reporter.h>
#include <wx/filename.h>
#include <wx/file.h>
#include <wx/app.h>
#include <plotters/plotter.h>
#include <future>
#include <chrono>
#include <thread>
#include <wx/base64.h>
#include <plotters/plotter.h>
#include <vector>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_drill.h>
#include <exporters/place_file_exporter.h>
#include <kiway.h>
#include <paths.h>
#include <wx/dir.h>
#include <tools/board_editor_control.h>
#include <gestfich.h>
#include <layer_ids.h>
#include <lset.h>
#include <widgets/appearance_controls.h>
#include <netlist_reader/pcb_netlist.h>
#include <netlist_reader/board_netlist_updater.h>
#include <tool/tool_manager.h>
#include <tool/actions.h>
#include <trace_helpers.h>
#include <pcb_signal_path_tracer.h>
#include <tools/pcb_actions.h>


AI_CHAT_PANEL::AI_CHAT_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame ) :
        AI_CHAT_PANEL_BASE( aParent, aFrame )
{
    // Set up DRC callback for direct access to violations
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetDrcCallback( [this, aFrame]() -> nlohmann::json
    {
        wxLogTrace( traceAiToolCall, wxT( "AI PCB: DRC callback invoked" ) );

        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, promise]()
        {
            try
            {
                nlohmann::json result = aFrame->runDrcAndSerialize();
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "DRC failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "DRC failed due to unknown error";
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
            error["error"] = "DRC timed out";
            return error;
        }
    } );

    // Set up trace_pcb_signal_path callback for tracing electrical connectivity on the PCB
    SetTracePcbSignalPathCallback(
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
                wxString pad = argsCopy.value( "pad", "" );
                int maxHops = argsCopy.value( "max_hops", 3 );

                PCB_SIGNAL_PATH_TRACER tracer( aFrame->GetBoard() );

                if( !component.IsEmpty() )
                    promise->set_value( tracer.TraceFromComponent( component, pad, maxHops ) );
                else
                    promise->set_value( tracer.TraceFromNet( netName, maxHops ) );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "PCB signal path trace failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "PCB signal path trace failed due to unknown error";
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
            error["error"] = "PCB signal path trace timed out";
            return error;
        }
    } );
    
    // highlight_net callback: highlights a net by name in the PCB
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
                BOARD* board = aFrame->GetBoard();

                if( clearExisting )
                    aFrame->GetToolManager()->RunAction( PCB_ACTIONS::clearHighlight );

                if( !netName.IsEmpty() )
                {
                    NETINFO_ITEM* net = board->FindNet( netName );

                    if( net )
                    {
                        aFrame->GetToolManager()->RunAction( PCB_ACTIONS::highlightNet,
                                                             net->GetNetCode() );
                        aFrame->GetCanvas()->Refresh();

                        nlohmann::json result;
                        result["success"] = true;
                        result["net"] = std::string( netName.ToUTF8() );
                        result["net_code"] = net->GetNetCode();
                        result["editor"] = "pcbnew";
                        promise->set_value( result );
                    }
                    else
                    {
                        nlohmann::json error;
                        error["error"] = "Net '" + std::string( netName.ToUTF8() ) + "' not found";
                        promise->set_value( error );
                    }
                }
                else
                {
                    aFrame->GetCanvas()->Refresh();

                    nlohmann::json result;
                    result["success"] = true;
                    result["action"] = "cleared";
                    result["editor"] = "pcbnew";
                    promise->set_value( result );
                }
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

    // select_items callback: selects items by reference designator in the PCB
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
                BOARD* board = aFrame->GetBoard();

                if( clearExisting )
                    aFrame->GetToolManager()->RunAction( ACTIONS::selectionClear );

                nlohmann::json selected = nlohmann::json::array();
                nlohmann::json notFound = nlohmann::json::array();

                for( const auto& ref : refs )
                {
                    FOOTPRINT* fp = board->FindFootprintByReference( wxString( ref ) );

                    if( fp )
                    {
                        aFrame->GetToolManager()->RunAction( ACTIONS::selectItem, fp );
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
    // CRITICAL: Plotting code (PCB_PLOTTER) MUST run on the main UI thread
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetSnapshotCallback( [this]() -> std::string
    {
        auto promise = std::make_shared<std::promise<std::string>>();
        std::future<std::string> future = promise->get_future();

        CallAfter( [this, promise]()
        {
            try
    {
        PCB_EDIT_FRAME* frame = GetPcbFrame();
        if( !frame )
                {
                    promise->set_value( "" );
                    return;
                }
        
        // Create temporary file
        wxString tempFile = wxFileName::CreateTempFileName( wxT( "pcb_snapshot_" ) );
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

    // Set up Gerber callback for direct Gerber generation
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetGerberCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, aParams, promise]()
        {
            try
            {
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    nlohmann::json error;
                    error["error"] = "No board loaded";
                    promise->set_value( error );
                    return;
                }

                // Create Gerber job
                std::unique_ptr<JOB_EXPORT_PCB_GERBERS> gerberJob( new JOB_EXPORT_PCB_GERBERS() );
                gerberJob->m_filename = board->GetFileName();

                // Set output directory if provided
                if( aParams.contains( "output_directory" ) && !aParams["output_directory"].is_null() && aParams["output_directory"].is_string() )
                {
                    wxString outDir = wxString::FromUTF8( aParams["output_directory"].get<std::string>() );
                    gerberJob->SetConfiguredOutputPath( outDir );
                }

                // Set layers if provided
                if( aParams.contains( "layers" ) && !aParams["layers"].is_null() && aParams["layers"].is_string() )
                {
                    wxString layers = wxString::FromUTF8( aParams["layers"].get<std::string>() );
                    gerberJob->m_argLayers = layers;
                }

                // Set common layers if provided
                if( aParams.contains( "common_layers" ) && !aParams["common_layers"].is_null() && aParams["common_layers"].is_string() )
                {
                    wxString commonLayers = wxString::FromUTF8( aParams["common_layers"].get<std::string>() );
                    gerberJob->m_argCommonLayers = commonLayers;
                }

                // Set other optional parameters
                if( aParams.contains( "precision" ) && !aParams["precision"].is_null() )
                    gerberJob->m_precision = aParams["precision"].get<int>();
                if( aParams.contains( "use_x2_format" ) && !aParams["use_x2_format"].is_null() )
                    gerberJob->m_useX2Format = aParams["use_x2_format"].get<bool>();
                if( aParams.contains( "include_netlist" ) && !aParams["include_netlist"].is_null() )
                    gerberJob->m_includeNetlistAttributes = aParams["include_netlist"].get<bool>();
                if( aParams.contains( "disable_aperture_macros" ) && !aParams["disable_aperture_macros"].is_null() )
                    gerberJob->m_disableApertureMacros = aParams["disable_aperture_macros"].get<bool>();
                if( aParams.contains( "use_protel_extension" ) && !aParams["use_protel_extension"].is_null() )
                    gerberJob->m_useProtelFileExtension = aParams["use_protel_extension"].get<bool>();
                if( aParams.contains( "check_zones_before_plot" ) && !aParams["check_zones_before_plot"].is_null() )
                    gerberJob->m_checkZonesBeforePlot = aParams["check_zones_before_plot"].get<bool>();
                if( aParams.contains( "use_board_plot_params" ) && !aParams["use_board_plot_params"].is_null() )
                    gerberJob->m_useBoardPlotParams = aParams["use_board_plot_params"].get<bool>();
                if( aParams.contains( "create_jobs_file" ) && !aParams["create_jobs_file"].is_null() )
                    gerberJob->m_createJobsFile = aParams["create_jobs_file"].get<bool>();

                // Process job
                NULL_REPORTER reporter;
                int exitCode = aFrame->Kiway().ProcessJob( KIWAY::FACE_PCB, gerberJob.get(), &reporter, nullptr );

                nlohmann::json result;
                if( exitCode == 0 )
                {
                    result["success"] = true;
                    result["output_directory"] = gerberJob->GetConfiguredOutputPath().ToStdString();
                    // Note: File list would need to be collected from the job handler
                    // For now, return success
                    result["files"] = nlohmann::json::array();
                }
                else
                {
                    result["error"] = "Gerber generation failed with exit code " + std::to_string( exitCode );
                }

                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Gerber generation failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Gerber generation failed due to unknown error";
                promise->set_value( error );
            }
        } );

        // Wait for result from main thread (with timeout)
        if( future.wait_for( std::chrono::seconds( 60 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Gerber generation timed out";
            return error;
        }
    } );

    // Set up Drill callback for direct drill file generation
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetDrillCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, aParams, promise]()
        {
            try
            {
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    nlohmann::json error;
                    error["error"] = "No board loaded";
                    promise->set_value( error );
                    return;
                }

                // Create drill job
                std::unique_ptr<JOB_EXPORT_PCB_DRILL> drillJob( new JOB_EXPORT_PCB_DRILL() );
                drillJob->m_filename = board->GetFileName();

                // Set output directory if provided
                if( aParams.contains( "output_directory" ) && !aParams["output_directory"].is_null() && aParams["output_directory"].is_string() )
                {
                    wxString outDir = wxString::FromUTF8( aParams["output_directory"].get<std::string>() );
                    drillJob->SetConfiguredOutputPath( outDir );
                }

                // Set format
                if( aParams.contains( "format" ) && !aParams["format"].is_null() && aParams["format"].is_string() )
                {
                    std::string format = aParams["format"].get<std::string>();
                    if( format == "excellon" )
                        drillJob->m_format = JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON;
                    else if( format == "gerber" )
                        drillJob->m_format = JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::GERBER;
                }

                // Set drill origin
                if( aParams.contains( "drill_origin" ) && !aParams["drill_origin"].is_null() && aParams["drill_origin"].is_string() )
                {
                    std::string origin = aParams["drill_origin"].get<std::string>();
                    if( origin == "absolute" )
                        drillJob->m_drillOrigin = JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::ABS;
                    else if( origin == "plot" )
                        drillJob->m_drillOrigin = JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::PLOT;
                }

                // Set units
                if( aParams.contains( "units" ) && !aParams["units"].is_null() && aParams["units"].is_string() )
                {
                    std::string units = aParams["units"].get<std::string>();
                    if( units == "mm" )
                        drillJob->m_drillUnits = JOB_EXPORT_PCB_DRILL::DRILL_UNITS::MM;
                    else if( units == "inch" )
                        drillJob->m_drillUnits = JOB_EXPORT_PCB_DRILL::DRILL_UNITS::INCH;
                }

                // Set zeros format
                if( aParams.contains( "zeros_format" ) && !aParams["zeros_format"].is_null() && aParams["zeros_format"].is_string() )
                {
                    std::string zeros = aParams["zeros_format"].get<std::string>();
                    if( zeros == "decimal" )
                        drillJob->m_zeroFormat = JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::DECIMAL;
                    else if( zeros == "suppress_leading" )
                        drillJob->m_zeroFormat = JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::SUPPRESS_LEADING;
                    else if( zeros == "suppress_trailing" )
                        drillJob->m_zeroFormat = JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::SUPPRESS_TRAILING;
                    else if( zeros == "keep" )
                        drillJob->m_zeroFormat = JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::KEEP_ZEROS;
                }

                // Set other optional parameters
                if( aParams.contains( "excellon_mirror_y" ) && !aParams["excellon_mirror_y"].is_null() )
                    drillJob->m_excellonMirrorY = aParams["excellon_mirror_y"].get<bool>();
                if( aParams.contains( "excellon_minimal_header" ) && !aParams["excellon_minimal_header"].is_null() )
                    drillJob->m_excellonMinimalHeader = aParams["excellon_minimal_header"].get<bool>();
                if( aParams.contains( "excellon_separate_th" ) && !aParams["excellon_separate_th"].is_null() )
                    drillJob->m_excellonCombinePTHNPTH = !aParams["excellon_separate_th"].get<bool>(); // Note: inverted
                if( aParams.contains( "excellon_oval_format" ) && !aParams["excellon_oval_format"].is_null() && aParams["excellon_oval_format"].is_string() )
                {
                    std::string oval = aParams["excellon_oval_format"].get<std::string>();
                    drillJob->m_excellonOvalDrillRoute = ( oval == "route" );
                }
                if( aParams.contains( "generate_map" ) && !aParams["generate_map"].is_null() )
                    drillJob->m_generateMap = aParams["generate_map"].get<bool>();
                if( aParams.contains( "map_format" ) && !aParams["map_format"].is_null() && aParams["map_format"].is_string() )
                {
                    std::string mapFmt = aParams["map_format"].get<std::string>();
                    if( mapFmt == "pdf" )
                        drillJob->m_mapFormat = JOB_EXPORT_PCB_DRILL::MAP_FORMAT::PDF;
                    else if( mapFmt == "gerberx2" )
                        drillJob->m_mapFormat = JOB_EXPORT_PCB_DRILL::MAP_FORMAT::GERBER_X2;
                    else if( mapFmt == "ps" )
                        drillJob->m_mapFormat = JOB_EXPORT_PCB_DRILL::MAP_FORMAT::POSTSCRIPT;
                    else if( mapFmt == "dxf" )
                        drillJob->m_mapFormat = JOB_EXPORT_PCB_DRILL::MAP_FORMAT::DXF;
                    else if( mapFmt == "svg" )
                        drillJob->m_mapFormat = JOB_EXPORT_PCB_DRILL::MAP_FORMAT::SVG;
                }
                if( aParams.contains( "generate_tenting" ) && !aParams["generate_tenting"].is_null() )
                    drillJob->m_generateTenting = aParams["generate_tenting"].get<bool>();
                if( aParams.contains( "gerber_precision" ) && !aParams["gerber_precision"].is_null() )
                    drillJob->m_gerberPrecision = aParams["gerber_precision"].get<int>();

                // Process job
                NULL_REPORTER reporter;
                int exitCode = aFrame->Kiway().ProcessJob( KIWAY::FACE_PCB, drillJob.get(), &reporter, nullptr );

                nlohmann::json result;
                if( exitCode == 0 )
                {
                    result["success"] = true;
                    result["output_directory"] = drillJob->GetConfiguredOutputPath().ToStdString();
                    // Note: File list would need to be collected from the job handler
                    // For now, return success
                    result["files"] = nlohmann::json::array();
                }
                else
                {
                    result["error"] = "Drill file generation failed with exit code " + std::to_string( exitCode );
                }

                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Drill file generation failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Drill file generation failed due to unknown error";
                promise->set_value( error );
            }
        } );

        // Wait for result from main thread (with timeout)
        if( future.wait_for( std::chrono::seconds( 60 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Drill file generation timed out";
            return error;
        }
    } );

    // Set up Position File callback for pick-and-place CSV generation
    SetPositionFileCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, aParams, promise]()
        {
            try
            {
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    nlohmann::json error;
                    error["error"] = "No board loaded";
                    promise->set_value( error );
                    return;
                }

                bool unitsMM = true;
                bool smdOnly = false;
                bool excludeTH = false;
                bool excludeDNP = true;
                bool excludeBOM = false;
                bool topSide = true;
                bool bottomSide = true;
                bool formatCSV = true;
                bool useAuxOrigin = false;
                bool negateBottomX = false;

                if( aParams.contains( "smd_only" ) && !aParams["smd_only"].is_null() )
                    smdOnly = aParams["smd_only"].get<bool>();
                if( aParams.contains( "exclude_th" ) && !aParams["exclude_th"].is_null() )
                    excludeTH = aParams["exclude_th"].get<bool>();
                if( aParams.contains( "exclude_dnp" ) && !aParams["exclude_dnp"].is_null() )
                    excludeDNP = aParams["exclude_dnp"].get<bool>();
                if( aParams.contains( "use_drill_origin" ) && !aParams["use_drill_origin"].is_null() )
                    useAuxOrigin = aParams["use_drill_origin"].get<bool>();

                if( aParams.contains( "side" ) && !aParams["side"].is_null() && aParams["side"].is_string() )
                {
                    std::string side = aParams["side"].get<std::string>();
                    if( side == "top" )
                    {
                        topSide = true;
                        bottomSide = false;
                    }
                    else if( side == "bottom" )
                    {
                        topSide = false;
                        bottomSide = true;
                    }
                }

                if( aParams.contains( "format" ) && !aParams["format"].is_null() && aParams["format"].is_string() )
                {
                    std::string fmt = aParams["format"].get<std::string>();
                    formatCSV = ( fmt == "csv" );
                }

                PLACE_FILE_EXPORTER exporter( board, unitsMM, smdOnly, excludeTH,
                                              excludeDNP, excludeBOM, topSide, bottomSide,
                                              formatCSV, useAuxOrigin, negateBottomX );

                std::string posData = exporter.GenPositionData();
                int fpCount = exporter.GetFootprintCount();

                wxFileName boardFn( board->GetFileName() );
                wxString outDir;
                if( aParams.contains( "output_directory" ) && !aParams["output_directory"].is_null()
                    && aParams["output_directory"].is_string() )
                {
                    outDir = wxString::FromUTF8( aParams["output_directory"].get<std::string>() );
                }
                else
                {
                    outDir = boardFn.GetPath();
                }

                wxString baseName = boardFn.GetName();
                wxString ext = formatCSV ? wxT( ".csv" ) : wxT( ".pos" );
                wxString sideSuffix;
                if( topSide && bottomSide )
                    sideSuffix = wxT( "_pos_all" );
                else if( topSide )
                    sideSuffix = wxT( "_pos_top" );
                else
                    sideSuffix = wxT( "_pos_bottom" );

                wxString outPath = outDir + wxFileName::GetPathSeparator()
                                   + baseName + sideSuffix + ext;

                wxFile outFile( outPath, wxFile::write );
                if( !outFile.IsOpened() )
                {
                    nlohmann::json error;
                    error["error"] = "Failed to create position file: " + outPath.ToStdString();
                    promise->set_value( error );
                    return;
                }
                outFile.Write( wxString::FromUTF8( posData ) );
                outFile.Close();

                nlohmann::json result;
                result["success"] = true;
                result["file_path"] = outPath.ToStdString();
                result["footprint_count"] = fpCount;
                result["format"] = formatCSV ? "csv" : "ascii";

                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Position file generation failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["error"] = "Position file generation failed due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 60 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["error"] = "Position file generation timed out";
            return error;
        }
    } );

    // Set up Autoroute callback for AI-triggered autorouting
    // This runs the cloud autoroute with parameters provided by the AI
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetAutorouteCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        wxLogTrace( traceAiToolCall, wxT( "AI PCB: Autoroute callback invoked" ) );

        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        nlohmann::json routingParams = aParams.value( "params", nlohmann::json::object() );

        // Phase 1: Quick UI pre-work on the main thread
        CallAfter( [aFrame, routingParams, promise]()
        {
            try
            {
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    nlohmann::json error;
                    error["success"] = false;
                    error["message"] = "No board loaded";
                    error["progress_log"] = nlohmann::json::array();
                    promise->set_value( error );
                    return;
                }

                // TODO: Migrate autoroute to use AI_COMMIT for proper undo support.
                // For now, autoroute replaces the board wholesale via cloud processing,
                // which is incompatible with the item-level commit approach.
                aFrame->ClearUndoRedoList();
                if( aFrame->GetCanvas() )
                {
                    for( PCB_TRACK* track : board->Tracks() )
                        aFrame->GetCanvas()->GetView()->Remove( track );
                }

                // Phase 2: Run the heavy network I/O off the main thread
                std::thread( [aFrame, board, routingParams, promise]()
                {
                    try
                    {
                        nlohmann::json result = PerformCloudAutoroute(
                                board, nullptr, routingParams, nullptr, nullptr );

                        // Phase 3: Post-work back on main thread (view updates, save)
                        aFrame->CallAfter( [aFrame, board, result, promise]()
                        {
                            try
                            {
                                if( result.value( "success", false ) )
                                {
                                    aFrame->OnModify();

                                    if( aFrame->GetCanvas() )
                                    {
                                        for( PCB_TRACK* track : board->Tracks() )
                                            aFrame->GetCanvas()->GetView()->Add( track );
                                    }

                                    wxString boardFileName = board->GetFileName();
                                    if( !boardFileName.IsEmpty() )
                                    {
                                        wxLogTrace( traceFileSave, wxT( "AI PCB: Autoroute saving board: %s" ), boardFileName );
                                        aFrame->SavePcbFile( boardFileName );
                                        ConvertKicadPcbToTracePcb( boardFileName );
                                    }

                                    aFrame->Refresh();
                                }
                                else
                                {
                                    if( aFrame->GetCanvas() )
                                    {
                                        for( PCB_TRACK* track : board->Tracks() )
                                            aFrame->GetCanvas()->GetView()->Add( track );
                                    }
                                    aFrame->Refresh();
                                }

                                promise->set_value( result );
                            }
                            catch( const std::exception& e )
                            {
                                nlohmann::json error;
                                error["success"] = false;
                                error["message"] = std::string( "Autoroute post-processing failed: " ) + e.what();
                                error["progress_log"] = nlohmann::json::array();
                                promise->set_value( error );
                            }
                        } );
                    }
                    catch( const std::exception& e )
                    {
                        nlohmann::json error;
                        error["success"] = false;
                        error["message"] = std::string( "Autorouting failed: " ) + e.what();
                        error["progress_log"] = nlohmann::json::array();
                        promise->set_value( error );
                    }
                    catch( ... )
                    {
                        nlohmann::json error;
                        error["success"] = false;
                        error["message"] = "Autorouting failed due to unknown error";
                        error["progress_log"] = nlohmann::json::array();
                        promise->set_value( error );
                    }
                } ).detach();
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["message"] = std::string( "Autorouting failed: " ) + e.what();
                error["progress_log"] = nlohmann::json::array();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["success"] = false;
                error["message"] = "Autorouting failed due to unknown error";
                error["progress_log"] = nlohmann::json::array();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 120 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["success"] = false;
            error["message"] = "Autorouting timed out (>2 minutes)";
            error["progress_log"] = nlohmann::json::array();
            return error;
        }
    } );

    // Set up Update PCB from Schematic callback
    SetUpdatePcbFromSchematicCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, aParams, promise]()
        {
            try
            {
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    nlohmann::json error;
                    error["success"] = false;
                    error["error"] = "No board loaded";
                    promise->set_value( error );
                    return;
                }

                NETLIST netlist;

                if( !aFrame->FetchNetlistFromSchematic( netlist,
                        _( "Updating PCB requires a fully annotated schematic." ) ) )
                {
                    nlohmann::json error;
                    error["success"] = false;
                    error["error"] = "Failed to fetch netlist from schematic. "
                                     "Ensure the schematic is fully annotated and "
                                     "pcbnew is not running in standalone mode.";
                    promise->set_value( error );
                    return;
                }

                bool relinkFootprints    = aParams.value( "relink_footprints", false );
                bool groupFootprints     = aParams.value( "group_footprints", false );
                bool replaceFootprints   = aParams.value( "replace_footprints", true );
                bool deleteExtra         = aParams.value( "delete_extra_footprints", false );
                bool overrideLocks       = aParams.value( "override_locks", false );
                bool updateFields        = aParams.value( "update_fields", true );
                bool removeExtraFields   = aParams.value( "remove_extra_fields", false );

                netlist.SetFindByTimeStamp( !relinkFootprints );
                netlist.SetReplaceFootprints( replaceFootprints );

                aFrame->GetToolManager()->DeactivateTool();
                aFrame->GetToolManager()->RunAction( ACTIONS::selectionClear );

                // Inline reporter to capture structured messages by severity
                struct JSON_REPORTER : public REPORTER
                {
                    struct MSG { wxString text; SEVERITY severity; };
                    std::vector<MSG> messages;

                    REPORTER& Report( const wxString& aText,
                                      SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override
                    {
                        REPORTER::Report( aText, aSeverity );
                        messages.push_back( { aText, aSeverity } );
                        return *this;
                    }
                };

                JSON_REPORTER reporter;

                BOARD_NETLIST_UPDATER updater( aFrame, board );
                updater.SetReporter( &reporter );
                updater.SetIsDryRun( false );
                updater.SetLookupByTimestamp( !relinkFootprints );
                updater.SetDeleteUnusedFootprints( deleteExtra );
                updater.SetReplaceFootprints( replaceFootprints );
                updater.SetTransferGroups( groupFootprints );
                updater.SetOverrideLocks( overrideLocks );
                updater.SetUpdateFields( updateFields );
                updater.SetRemoveExtraFields( removeExtraFields );

                bool updateOk = updater.UpdateNetlist( netlist );

                bool runDrag = false;
                aFrame->OnNetlistChanged( updater, &runDrag );

                aFrame->OnModify();

                wxString boardFileName = board->GetFileName();
                if( !boardFileName.IsEmpty() )
                {
                    wxLogTrace( traceFileSave, wxT( "AI PCB: Saving board file: %s" ), boardFileName );
                    aFrame->SavePcbFile( boardFileName );
                    wxLogTrace( traceFileSave, wxT( "AI PCB: SavePcbFile completed" ) );
                    ConvertKicadPcbToTracePcb( boardFileName );
                }

                aFrame->Refresh();

                nlohmann::json result;
                result["success"] = updateOk;

                nlohmann::json messagesArr = nlohmann::json::array();
                int errorCount = 0;
                int warningCount = 0;
                int actionCount = 0;

                for( const auto& msg : reporter.messages )
                {
                    nlohmann::json entry;
                    entry["text"] = msg.text.ToStdString();

                    if( msg.severity == RPT_SEVERITY_ERROR )
                    {
                        entry["severity"] = "error";
                        errorCount++;
                    }
                    else if( msg.severity == RPT_SEVERITY_WARNING )
                    {
                        entry["severity"] = "warning";
                        warningCount++;
                    }
                    else if( msg.severity == RPT_SEVERITY_ACTION )
                    {
                        entry["severity"] = "action";
                        actionCount++;
                    }
                    else
                    {
                        entry["severity"] = "info";
                    }

                    messagesArr.push_back( entry );
                }

                result["messages"] = messagesArr;

                nlohmann::json summary;
                summary["errors"] = errorCount;
                summary["warnings"] = warningCount;
                summary["actions"] = actionCount;
                result["summary"] = summary;

                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = std::string( "Update PCB from schematic failed: " ) + e.what();
                promise->set_value( error );
            }
            catch( ... )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = "Update PCB from schematic failed due to unknown error";
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 60 ) ) == std::future_status::ready )
        {
            return future.get();
        }
        else
        {
            nlohmann::json error;
            error["success"] = false;
            error["error"] = "Update PCB from schematic timed out (>60 seconds)";
            return error;
        }
    } );

    // =============================================================================
    // Layer Switching Callbacks (PCBnew-specific)
    // =============================================================================

    // Set up Switch Layer callback
    SetSwitchLayerCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        nlohmann::json paramsCopy = aParams;

        CallAfter( [aFrame, promise, paramsCopy]()
        {
            try
            {
                nlohmann::json result;
                
                if( !aFrame )
                {
                    result["success"] = false;
                    result["error"] = "Frame not available";
                    promise->set_value( result );
                    return;
                }
                
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    result["success"] = false;
                    result["error"] = "No board loaded";
                    promise->set_value( result );
                    return;
                }
                
                PCB_LAYER_ID targetLayer = UNDEFINED_LAYER;
                
                // Priority 1: Find by layer_id
                if( paramsCopy.contains( "layer_id" ) && !paramsCopy["layer_id"].is_null() )
                {
                    int layerId = paramsCopy["layer_id"].get<int>();
                    targetLayer = static_cast<PCB_LAYER_ID>( layerId );
                }
                // Priority 2: Find by layer_name
                else if( paramsCopy.contains( "layer_name" ) && !paramsCopy["layer_name"].is_null() && paramsCopy["layer_name"].is_string() )
                {
                    wxString layerName = wxString::FromUTF8( paramsCopy["layer_name"].get<std::string>() );
                    targetLayer = board->GetLayerID( layerName );
                }
                
                if( targetLayer == UNDEFINED_LAYER )
                {
                    result["success"] = false;
                    result["error"] = "Layer not found";
                    promise->set_value( result );
                    return;
                }
                
                if( !board->IsLayerEnabled( targetLayer ) )
                {
                    result["success"] = false;
                    result["error"] = "Layer '" + board->GetLayerName( targetLayer ).ToStdString() + "' is not enabled in this board";
                    promise->set_value( result );
                    return;
                }
                
                // Perform the layer switch
                aFrame->SetActiveLayer( targetLayer );
                
                // Build success response
                result["success"] = true;
                result["active_layer"] = board->GetLayerName( targetLayer ).ToStdString();
                result["layer_id"] = static_cast<int>( targetLayer );
                result["is_copper"] = IsCopperLayer( targetLayer );
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = std::string( "Failed to switch layer: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["success"] = false;
        error["error"] = "Switch layer request timed out";
        return error;
    } );

    // Set up Get Layers callback
    SetLayersCallback( [this, aFrame]() -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();

        CallAfter( [aFrame, promise]()
        {
            try
            {
                nlohmann::json result;
                
                if( !aFrame )
                {
                    result["error"] = "Frame not available";
                    promise->set_value( result );
                    return;
                }
                
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    result["error"] = "No board loaded";
                    promise->set_value( result );
                    return;
                }
                
                LSET enabledLayers = board->GetEnabledLayers();
                LSET visibleLayers = board->GetVisibleLayers();
                PCB_LAYER_ID activeLayer = aFrame->GetActiveLayer();
                
                nlohmann::json layers = nlohmann::json::array();
                
                for( PCB_LAYER_ID layer : enabledLayers.Seq() )
                {
                    nlohmann::json layerInfo;
                    layerInfo["id"] = static_cast<int>( layer );
                    layerInfo["name"] = board->GetLayerName( layer ).ToStdString();
                    layerInfo["canonical_name"] = LSET::Name( layer ).ToStdString();
                    layerInfo["visible"] = visibleLayers.test( layer );
                    layerInfo["is_copper"] = IsCopperLayer( layer );
                    layerInfo["is_active"] = ( layer == activeLayer );
                    layers.push_back( layerInfo );
                }
                
                result["layers"] = layers;
                result["active_layer"] = board->GetLayerName( activeLayer ).ToStdString();
                result["active_layer_id"] = static_cast<int>( activeLayer );
                result["copper_layer_count"] = board->GetCopperLayerCount();
                result["total_enabled_layers"] = static_cast<int>( enabledLayers.count() );
                
                // Get current preset if available
                if( aFrame->GetAppearancePanel() )
                {
                    wxString preset = aFrame->GetAppearancePanel()->GetActiveLayerPreset();
                    if( !preset.IsEmpty() )
                        result["current_preset"] = preset.ToStdString();
                }
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to get layers: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["error"] = "Get layers request timed out";
        return error;
    } );

    // Set up Apply Layer Preset callback
    SetLayerPresetCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        nlohmann::json paramsCopy = aParams;

        CallAfter( [aFrame, promise, paramsCopy]()
        {
            try
            {
                nlohmann::json result;
                
                if( !aFrame || !aFrame->GetAppearancePanel() )
                {
                    result["success"] = false;
                    result["error"] = "Appearance panel not available";
                    promise->set_value( result );
                    return;
                }
                
                if( !paramsCopy.contains( "preset_name" ) || paramsCopy["preset_name"].is_null() || !paramsCopy["preset_name"].is_string() )
                {
                    result["success"] = false;
                    result["error"] = "preset_name is required";
                    promise->set_value( result );
                    return;
                }
                
                wxString presetName = wxString::FromUTF8( paramsCopy["preset_name"].get<std::string>() );
                
                // Apply the preset via APPEARANCE_CONTROLS
                aFrame->GetAppearancePanel()->ApplyLayerPreset( presetName );
                
                // Build success response
                BOARD* board = aFrame->GetBoard();
                result["success"] = true;
                result["preset_applied"] = presetName.ToStdString();
                if( board )
                    result["active_layer"] = board->GetLayerName( aFrame->GetActiveLayer() ).ToStdString();
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = std::string( "Failed to apply preset: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["success"] = false;
        error["error"] = "Apply preset request timed out";
        return error;
    } );

    // Set up Set Layer Visibility callback
    SetLayerVisibilityCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        nlohmann::json paramsCopy = aParams;

        CallAfter( [aFrame, promise, paramsCopy]()
        {
            try
            {
                nlohmann::json result;
                
                if( !aFrame )
                {
                    result["success"] = false;
                    result["error"] = "Frame not available";
                    promise->set_value( result );
                    return;
                }
                
                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    result["success"] = false;
                    result["error"] = "No board loaded";
                    promise->set_value( result );
                    return;
                }
                
                PCB_LAYER_ID targetLayer = UNDEFINED_LAYER;
                
                if( paramsCopy.contains( "layer_id" ) && !paramsCopy["layer_id"].is_null() )
                {
                    targetLayer = static_cast<PCB_LAYER_ID>( paramsCopy["layer_id"].get<int>() );
                }
                else if( paramsCopy.contains( "layer_name" ) && !paramsCopy["layer_name"].is_null() && paramsCopy["layer_name"].is_string() )
                {
                    wxString layerName = wxString::FromUTF8( paramsCopy["layer_name"].get<std::string>() );
                    targetLayer = board->GetLayerID( layerName );
                }
                
                if( targetLayer == UNDEFINED_LAYER || !board->IsLayerEnabled( targetLayer ) )
                {
                    result["success"] = false;
                    result["error"] = "Layer not found or not enabled";
                    promise->set_value( result );
                    return;
                }
                
                bool visible = paramsCopy.value( "visible", true );
                
                // Update visibility
                LSET visibleLayers = board->GetVisibleLayers();
                visibleLayers.set( targetLayer, visible );
                board->SetVisibleLayers( visibleLayers );
                
                // Sync the view
                aFrame->GetCanvas()->GetView()->SetLayerVisible( targetLayer, visible );
                if( aFrame->GetAppearancePanel() )
                    aFrame->GetAppearancePanel()->OnBoardChanged();
                aFrame->GetCanvas()->Refresh();
                
                result["success"] = true;
                result["layer"] = board->GetLayerName( targetLayer ).ToStdString();
                result["visible"] = visible;
                
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["success"] = false;
                error["error"] = std::string( "Failed to set visibility: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 10 ) ) == std::future_status::ready )
            return future.get();
        
        nlohmann::json error;
        error["success"] = false;
        error["error"] = "Set visibility request timed out";
        return error;
    } );

    // =============================================================================
    // Fetch Dimensions Callback (PCBnew-specific)
    // =============================================================================

    SetFetchDimensionsCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        nlohmann::json paramsCopy = aParams;

        CallAfter( [aFrame, promise, paramsCopy]()
        {
            try
            {
                nlohmann::json result;

                if( !aFrame )
                {
                    result["error"] = "Frame not available";
                    promise->set_value( result );
                    return;
                }

                BOARD* board = aFrame->GetBoard();
                if( !board )
                {
                    result["error"] = "No board loaded";
                    promise->set_value( result );
                    return;
                }

                if( !paramsCopy.contains( "footprints" ) || !paramsCopy["footprints"].is_array() )
                {
                    result["error"] = "'footprints' array is required";
                    promise->set_value( result );
                    return;
                }

                nlohmann::json dimensions = nlohmann::json::object();

                for( const auto& fpQuery : paramsCopy["footprints"] )
                {
                    if( !fpQuery.is_string() )
                        continue;

                    std::string query = fpQuery.get<std::string>();
                    bool isRef = ( query.find( ':' ) == std::string::npos );

                    FOOTPRINT* matched = nullptr;
                    for( FOOTPRINT* fp : board->Footprints() )
                    {
                        if( isRef )
                        {
                            if( fp->GetReference().ToStdString() == query )
                            {
                                matched = fp;
                                break;
                            }
                        }
                        else
                        {
                            if( fp->GetFPIDAsString().ToStdString() == query )
                            {
                                matched = fp;
                                break;
                            }
                        }
                    }

                    if( !matched )
                    {
                        dimensions[query] = { { "error", "not found" } };
                        continue;
                    }

                    double widthMm = 0.0;
                    double heightMm = 0.0;
                    std::string source = "none";

                    const SHAPE_POLY_SET& frontCy = matched->GetCourtyard( F_CrtYd );
                    const SHAPE_POLY_SET& backCy = matched->GetCourtyard( B_CrtYd );

                    if( frontCy.OutlineCount() > 0 )
                    {
                        BOX2I bbox = frontCy.BBox();
                        widthMm = pcbIUScale.IUTomm( bbox.GetWidth() );
                        heightMm = pcbIUScale.IUTomm( bbox.GetHeight() );
                        source = "courtyard_front";
                    }
                    else if( backCy.OutlineCount() > 0 )
                    {
                        BOX2I bbox = backCy.BBox();
                        widthMm = pcbIUScale.IUTomm( bbox.GetWidth() );
                        heightMm = pcbIUScale.IUTomm( bbox.GetHeight() );
                        source = "courtyard_back";
                    }
                    else if( !matched->Pads().empty() )
                    {
                        BOX2I padsBbox;
                        bool first = true;

                        for( PAD* pad : matched->Pads() )
                        {
                            if( first )
                            {
                                padsBbox = pad->GetBoundingBox();
                                first = false;
                            }
                            else
                            {
                                padsBbox.Merge( pad->GetBoundingBox() );
                            }
                        }

                        widthMm = pcbIUScale.IUTomm( padsBbox.GetWidth() );
                        heightMm = pcbIUScale.IUTomm( padsBbox.GetHeight() );
                        source = "pads";
                    }

                    widthMm = std::round( widthMm * 1000.0 ) / 1000.0;
                    heightMm = std::round( heightMm * 1000.0 ) / 1000.0;

                    dimensions[query] = {
                        { "width", widthMm },
                        { "height", heightMm },
                        { "source", source }
                    };
                }

                result["dimensions"] = dimensions;
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to fetch dimensions: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
            return future.get();

        nlohmann::json error;
        error["error"] = "Fetch dimensions request timed out";
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

    // Set up DRC preset callback
    SetDrcPresetCallback( [this, aFrame]( const std::string& aPresetId ) -> nlohmann::json
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        std::future<nlohmann::json> future = promise->get_future();
        std::string presetIdCopy = aPresetId;

        CallAfter( [aFrame, promise, presetIdCopy]()
        {
            try
            {
                wxString resPath = PATHS::GetStockDataPath() + wxT( "/manufacturers/drc/" )
                                   + wxString( presetIdCopy ) + wxT( ".kicad_dru" );

                if( !wxFileExists( resPath ) )
                {
                    nlohmann::json error;
                    error["error"] = "DRC preset file not found: " + presetIdCopy;
                    promise->set_value( error );
                    return;
                }

                wxString projectDruPath = aFrame->Prj().GetProjectPath()
                                          + aFrame->Prj().GetProjectName() + wxT( ".kicad_dru" );

                wxCopyFile( resPath, projectDruPath, true );

                // Reload the DRC engine with the new rules
                DRC_TOOL* drcTool = aFrame->GetToolManager()->GetTool<DRC_TOOL>();
                try
                {
                    drcTool->GetDRCEngine()->InitEngine( aFrame->GetDesignRulesPath() );
                }
                catch( PARSE_ERROR& )
                {
                    // Rules file was copied but failed to parse — still report success
                    // since the file is in place; the user will see DRC errors.
                }

                nlohmann::json result;
                result["status"] = "ok";
                result["preset"] = presetIdCopy;
                result["dru_path"] = projectDruPath.ToStdString();
                promise->set_value( result );
            }
            catch( const std::exception& e )
            {
                nlohmann::json error;
                error["error"] = std::string( "Failed to load DRC preset: " ) + e.what();
                promise->set_value( error );
            }
        } );

        if( future.wait_for( std::chrono::seconds( 30 ) ) == std::future_status::ready )
            return future.get();

        return nlohmann::json{ { "error", "DRC preset load timed out" } };
    } );
}


PCB_EDIT_FRAME* AI_CHAT_PANEL::GetPcbFrame() const
{
    return static_cast<PCB_EDIT_FRAME*>( GetFrame() );
}


bool AI_CHAT_PANEL::ReloadFromFile( const wxString& aFileName )
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return false;

    frame->CaptureNetPointersBeforeReload();

    bool result = frame->ReloadBoardFromFile( aFileName );
    return result;
}


bool AI_CHAT_PANEL::CaptureStateForAIEdit( const wxString& aFilePath )
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return false;

    bool result = frame->CaptureBoardStateForAIEdit( aFilePath );
    return result;
}


bool AI_CHAT_PANEL::CompareAndCreateAIEditUndoEntries()
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return false;

    bool result = frame->CompareAndCreateAIEditUndoEntries();
    return result;
}


void AI_CHAT_PANEL::RemapUndoRedoAfterReload()
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( frame )
        frame->RemapUndoRedoAfterReload();
}


bool AI_CHAT_PANEL::SaveDocument()
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame || !frame->GetBoard() )
        return false;

    try
    {
        bool result = frame->SavePcbFile( frame->GetBoard()->GetFileName() );
        return result;
    }
    catch( ... )
    {
        return false;
    }
}


std::unique_ptr<AI_COMMIT> AI_CHAT_PANEL::CreateAICommit()
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return nullptr;

    auto commit = std::make_unique<AI_COMMIT>( true );

    commit->SetCommit( std::make_unique<BOARD_COMMIT>( frame ) );

    commit->SetBoardItemResolver( [frame]( const KIID& aUuid ) -> EDA_ITEM*
    {
        BOARD* board = frame->GetBoard();
        if( !board )
            return nullptr;
        return static_cast<EDA_ITEM*>( board->ResolveItem( aUuid, true ) );
    } );

    commit->SetBoardProvider( [frame]() -> BOARD*
    {
        return frame->GetBoard();
    } );

    commit->SetBoardItemParser( []( const std::string& aSexp, BOARD* aBoard ) -> EDA_ITEM*
    {
        try
        {
            std::string wrappedSexp =
                "(kicad_pcb (version 20240108) (generator \"ai_commit\")\n"
                "  (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal))\n"
                "  " + aSexp + "\n"
                ")\n";

            STRING_LINE_READER reader( wrappedSexp, wxT( "AI edit" ) );
            PCB_IO_KICAD_SEXPR_PARSER parser( &reader, nullptr, nullptr );

            BOARD_ITEM* parsed = parser.Parse();
            if( !parsed )
                return nullptr;

            BOARD* tempBoard = dynamic_cast<BOARD*>( parsed );
            if( !tempBoard )
                return static_cast<EDA_ITEM*>( parsed );

            BOARD_ITEM* result = nullptr;
            for( FOOTPRINT* fp : tempBoard->Footprints() )
            {
                tempBoard->Remove( fp );
                result = fp;
                break;
            }
            if( !result )
            {
                for( PCB_TRACK* track : tempBoard->Tracks() )
                {
                    tempBoard->Remove( track );
                    result = track;
                    break;
                }
            }
            if( !result )
            {
                for( BOARD_ITEM* item : tempBoard->Drawings() )
                {
                    tempBoard->Remove( item );
                    result = item;
                    break;
                }
            }
            if( !result )
            {
                for( ZONE* zone : tempBoard->Zones() )
                {
                    tempBoard->Remove( zone );
                    result = zone;
                    break;
                }
            }

            delete tempBoard;
            return static_cast<EDA_ITEM*>( result );
        }
        catch( const std::exception& e )
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse board item: %s" ),
                         wxString::FromUTF8( e.what() ) );
            return nullptr;
        }
    } );

    // Set item swapper for MODIFY operations
    commit->SetItemSwapper( []( EDA_ITEM* aLive, EDA_ITEM* aNew )
    {
        BOARD_ITEM* liveBoard = dynamic_cast<BOARD_ITEM*>( aLive );
        BOARD_ITEM* newBoard = dynamic_cast<BOARD_ITEM*>( aNew );
        if( liveBoard && newBoard )
            liveBoard->SwapItemData( newBoard );
        delete aNew;
    } );

    // Set undo blocker (PCB already has this mechanism)
    commit->SetUndoBlocker( [frame]( bool aBlock )
    {
        frame->UndoRedoBlock( aBlock );
    } );

    return commit;
}


bool AI_CHAT_PANEL::GenerateSnapshot( const wxString& aOutputPath )
{
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return false;
    
    BOARD* board = frame->GetBoard();
    if( !board )
        return false;
    
    try
    {
        // Use NULL_REPORTER (we're in background thread, can't show messages)
        REPORTER& reporter = NULL_REPORTER::GetInstance();
        
        // Set up plot parameters for SVG export
        PCB_PLOT_PARAMS plotOpts;
        plotOpts.SetFormat( PLOT_FORMAT::SVG );
        plotOpts.SetSvgFitPageToBoard( true );
        plotOpts.SetPlotFrameRef( false );
        plotOpts.SetMirror( false );
        plotOpts.SetColorSettings( frame->GetColorSettings() );
        
        // Select all visible layers for the snapshot
        LSET layerSelection = LSET::AllLayersMask();
        plotOpts.SetLayerSelection( layerSelection );
        
        PCB_PLOTTER plotter( board, &reporter, plotOpts );
        
        // Plot all copper layers and common layers
        LSEQ layersToPlot;
        LSEQ commonLayers;
        
        // Add all copper layers
        for( int layer = F_Cu; layer <= B_Cu; layer++ )
        {
            layersToPlot.push_back( static_cast<PCB_LAYER_ID>( layer ) );
        }
        
        // Add common layers
        commonLayers.push_back( Edge_Cuts );
        commonLayers.push_back( F_SilkS );
        commonLayers.push_back( B_SilkS );
        commonLayers.push_back( F_Paste );
        commonLayers.push_back( B_Paste );
        commonLayers.push_back( F_Mask );
        commonLayers.push_back( B_Mask );
        
        if( !plotter.Plot( aOutputPath, layersToPlot, commonLayers, false, true ) )
        {
            return false;
        }
        
        // Check if file was created (PCB_PLOTTER doesn't return the output path like SCH_PLOTTER)
        if( wxFile::Exists( aOutputPath ) )
        {
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
    PCB_EDIT_FRAME* frame = GetPcbFrame();
    if( !frame )
        return wxEmptyString;
    
    return frame->GetCurrentFileName();
}


wxString AI_CHAT_PANEL::GetAppType() const
{
    return wxT( "pcbnew" );
}


wxString AI_CHAT_PANEL::ConvertToTraceFile( const wxString& aFilePath ) const
{
    wxFileName traceFn( aFilePath );
    if( traceFn.GetExt() == wxString::FromUTF8( FILEEXT::KiCadPcbFileExtension ) )
    {
        traceFn.SetExt( wxString::FromUTF8( FILEEXT::TracePcbFileExtension ) );
        return traceFn.GetFullPath();
    }
    
    // If not a kicad_pcb file, use as-is (fallback)
    return aFilePath;
}


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
        PCB_EDIT_FRAME* frame = GetPcbFrame();
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

