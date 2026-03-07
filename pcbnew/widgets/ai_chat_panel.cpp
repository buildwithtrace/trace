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
#include <kiway.h>
#include <paths.h>
#include <wx/dir.h>
#include <tools/board_editor_control.h>
#include <gestfich.h>
#include <layer_ids.h>
#include <lset.h>
#include <widgets/appearance_controls.h>


AI_CHAT_PANEL::AI_CHAT_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame ) :
        AI_CHAT_PANEL_BASE( aParent, aFrame )
{
    // Set up DRC callback for direct access to violations
    // Uses CallAfter + promise/future to execute on main thread (tool calls run on background thread)
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetDrcCallback( [this, aFrame]() -> nlohmann::json
    {
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

    // Set up Autoroute callback for AI-triggered autorouting
    // This runs the cloud autoroute with parameters provided by the AI
    // NOTE: Use panel's CallAfter (not wxTheApp) for reliable delivery in all configurations
    SetAutorouteCallback( [this, aFrame]( const nlohmann::json& aParams ) -> nlohmann::json
    {
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
    
    return frame->ReloadBoardFromFile( aFileName );
}


bool AI_CHAT_PANEL::CaptureStateForAIEdit( const wxString& aFilePath )
{
    return true;
    // PCB_EDIT_FRAME* frame = GetPcbFrame();
    // if( !frame )
    //     return false;
    
    // return frame->CaptureBoardStateForAIEdit( aFilePath );
}


bool AI_CHAT_PANEL::CompareAndCreateAIEditUndoEntries()
{
    return true;
    // PCB_EDIT_FRAME* frame = GetPcbFrame();
    // if( !frame )
    //     return false;
    
    // return frame->CompareAndCreateAIEditUndoEntries();
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
    if( m_requestInProgress.load() )
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

