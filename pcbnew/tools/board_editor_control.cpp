/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2014 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

#include "board_editor_control.h"

#include <functional>
#include <memory>
#include <thread>
#include <chrono>

#include <pgm_base.h>
#include <executable_names.h>
#include <advanced_config.h>
#include <bitmaps.h>
#include <gestfich.h>
#include <pcb_painter.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <pcb_generator.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_target.h>
#include <pcb_track.h>
#include <zone.h>
#include <pcb_marker.h>
#include <confirm.h>
#include <dialogs/dialog_page_settings.h>
#include <dialogs/dialog_update_pcb.h>
#include <dialogs/dialog_assign_netclass.h>
#include <dialog_plot.h>
#include <kiface_base.h>
#include <kiway.h>
#include <netlist_reader/pcb_netlist.h>
#include <origin_viewitem.h>
#include <pcb_edit_frame.h>
#include <pcbnew_id.h>
#include <project.h>
#include <project/project_file.h> // LAST_PATH_TYPE
#include <settings/settings_manager.h>
#include <kiplatform/ui.h>
#include <pcbnew_settings.h>
#include <tool/tool_manager.h>
#include <tool/tool_event.h>
#include <tools/drawing_tool.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_edit_table_tool.h>
#include <tools/pcb_picker_tool.h>
#include <tools/pcb_selection_conditions.h>
#include <tools/pcb_selection_tool.h>
#include <tools/edit_tool.h>
#include <tools/tool_event_utils.h>
#include <tools/zone_filler_tool.h>
#include <richio.h>
#include <amplitude_client.h>
#include <router/router_tool.h>
#include <view/view_controls.h>
#include <view/view_group.h>
#include <wildcards_and_files_ext.h>
#include <drawing_sheet/ds_proxy_undo_item.h>
#include <footprint_edit_frame.h>
#include <wx/filedlg.h>
#include <auth/auth_manager.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <widgets/wx_progress_reporters.h>
#include <specctra_import_export/specctra.h>
#include <wx/msgdlg.h>
#include <wx/log.h>

#include <widgets/legacyfiledlg_netlist_options.h>

using namespace std::placeholders;


class ZONE_CONTEXT_MENU : public ACTION_MENU
{
public:
    ZONE_CONTEXT_MENU() :
        ACTION_MENU( true )
    {
        SetIcon( BITMAPS::add_zone );
        SetTitle( _( "Zones" ) );

        Add( PCB_ACTIONS::zoneFill );
        Add( PCB_ACTIONS::zoneFillAll );
        Add( PCB_ACTIONS::zoneUnfill );
        Add( PCB_ACTIONS::zoneUnfillAll );

        AppendSeparator();

        Add( PCB_ACTIONS::zoneMerge );
        Add( PCB_ACTIONS::zoneDuplicate );
        Add( PCB_ACTIONS::drawZoneCutout );
        Add( PCB_ACTIONS::drawSimilarZone );

        AppendSeparator();

        Add( PCB_ACTIONS::zonesManager );
    }

protected:
    ACTION_MENU* create() const override
    {
        return new ZONE_CONTEXT_MENU();
    }
};


class LOCK_CONTEXT_MENU : public CONDITIONAL_MENU
{
public:
    LOCK_CONTEXT_MENU( TOOL_INTERACTIVE* aTool ) :
        CONDITIONAL_MENU( aTool )
    {
        SetIcon( BITMAPS::locked );
        SetTitle( _( "Locking" ) );

        AddItem( PCB_ACTIONS::lock, PCB_SELECTION_CONDITIONS::HasUnlockedItems );
        AddItem( PCB_ACTIONS::unlock, PCB_SELECTION_CONDITIONS::HasLockedItems );
        AddItem( PCB_ACTIONS::toggleLock, SELECTION_CONDITIONS::ShowAlways );
    }

    ACTION_MENU* create() const override
    {
        return new LOCK_CONTEXT_MENU( this->m_tool );
    }
};


BOARD_EDITOR_CONTROL::BOARD_EDITOR_CONTROL() :
    PCB_TOOL_BASE( "pcbnew.EditorControl" ),
    m_frame( nullptr ),
    m_inPlaceFootprint( false ),
    m_placingFootprint( false )
{
    m_placeOrigin = std::make_unique<KIGFX::ORIGIN_VIEWITEM>( KIGFX::COLOR4D( 0.8, 0.0, 0.0, 1.0 ),
                                                             KIGFX::ORIGIN_VIEWITEM::CIRCLE_CROSS );
}


BOARD_EDITOR_CONTROL::~BOARD_EDITOR_CONTROL()
{
}


void BOARD_EDITOR_CONTROL::Reset( RESET_REASON aReason )
{
    m_frame = getEditFrame<PCB_EDIT_FRAME>();

    if( aReason == MODEL_RELOAD || aReason == GAL_SWITCH || aReason == REDRAW )
    {
        m_placeOrigin->SetPosition( getModel<BOARD>()->GetDesignSettings().GetAuxOrigin() );
        getView()->Remove( m_placeOrigin.get() );
        getView()->Add( m_placeOrigin.get() );
    }
}

// Update left-toolbar Line modes group icon based on current settings
int BOARD_EDITOR_CONTROL::OnAngleSnapModeChanged( const TOOL_EVENT& aEvent )
{
    PCB_EDIT_FRAME* f = getEditFrame<PCB_EDIT_FRAME>();

    if( !f )
        return 0;

    LEADER_MODE mode = GetAppSettings<PCBNEW_SETTINGS>( "pcbnew" )->m_AngleSnapMode;

    switch( mode )
    {
    case LEADER_MODE::DIRECT: f->SelectToolbarAction( PCB_ACTIONS::lineModeFree ); break;
    case LEADER_MODE::DEG90:  f->SelectToolbarAction( PCB_ACTIONS::lineMode90 );   break;
    default:
    case LEADER_MODE::DEG45:  f->SelectToolbarAction( PCB_ACTIONS::lineMode45 );   break;
    }

    return 0;
}

int BOARD_EDITOR_CONTROL::ChangeLineMode( const TOOL_EVENT& aEvent )
{
    LEADER_MODE mode = aEvent.Parameter<LEADER_MODE>();
    GetAppSettings<PCBNEW_SETTINGS>( "pcbnew" )->m_AngleSnapMode = mode;
    m_toolMgr->PostAction( ACTIONS::refreshPreview );
    m_toolMgr->RunAction( PCB_ACTIONS::angleSnapModeChanged );
    return 0;
}


bool BOARD_EDITOR_CONTROL::Init()
{
    auto activeToolCondition =
            [this]( const SELECTION& aSel )
            {
                return ( !m_frame->ToolStackIsEmpty() );
            };

    auto inactiveStateCondition =
            [this]( const SELECTION& aSel )
            {
                return ( m_frame->ToolStackIsEmpty() && aSel.Size() == 0 );
            };

    auto placeModuleCondition =
            [this]( const SELECTION& aSel )
            {
                return m_frame->IsCurrentTool( PCB_ACTIONS::placeFootprint ) && aSel.GetSize() == 0;
            };

    auto& ctxMenu = m_menu->GetMenu();

    // "Cancel" goes at the top of the context menu when a tool is active
    ctxMenu.AddItem( ACTIONS::cancelInteractive, activeToolCondition, 1 );
    ctxMenu.AddSeparator( 1 );

    // "Get and Place Footprint" should be available for Place Footprint tool
    ctxMenu.AddItem( PCB_ACTIONS::getAndPlace, placeModuleCondition, 1000 );
    ctxMenu.AddSeparator( 1000 );

    // Finally, add the standard zoom & grid items
    getEditFrame<PCB_BASE_FRAME>()->AddStandardSubMenus( *m_menu.get() );

    std::shared_ptr<ZONE_CONTEXT_MENU> zoneMenu = std::make_shared<ZONE_CONTEXT_MENU>();
    zoneMenu->SetTool( this );

    std::shared_ptr<LOCK_CONTEXT_MENU> lockMenu = std::make_shared<LOCK_CONTEXT_MENU>( this );

    // Add the PCB control menus to relevant other tools

    PCB_SELECTION_TOOL* selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();

    if( selTool )
    {
        TOOL_MENU&        toolMenu = selTool->GetToolMenu();
        CONDITIONAL_MENU& menu = toolMenu.GetMenu();

        // Add "Get and Place Footprint" when Selection tool is in an inactive state
        menu.AddItem( PCB_ACTIONS::getAndPlace, inactiveStateCondition );
        menu.AddSeparator();

        toolMenu.RegisterSubMenu( zoneMenu );
        toolMenu.RegisterSubMenu( lockMenu );

        menu.AddMenu( lockMenu.get(), SELECTION_CONDITIONS::NotEmpty, 100 );

        menu.AddMenu( zoneMenu.get(), SELECTION_CONDITIONS::OnlyTypes( { PCB_ZONE_T } ), 100 );
    }

    DRAWING_TOOL* drawingTool = m_toolMgr->GetTool<DRAWING_TOOL>();

    if( drawingTool )
    {
        TOOL_MENU&        toolMenu = drawingTool->GetToolMenu();
        CONDITIONAL_MENU& menu = toolMenu.GetMenu();

        toolMenu.RegisterSubMenu( zoneMenu );

        // Functor to say if the PCB_EDIT_FRAME is in a given mode
        // Capture the tool pointer and tool mode by value
        auto toolActiveFunctor =
                [=]( DRAWING_TOOL::MODE aMode )
                {
                    return [=]( const SELECTION& sel )
                           {
                               return drawingTool->GetDrawingMode() == aMode;
                           };
                };

        menu.AddMenu( zoneMenu.get(), toolActiveFunctor( DRAWING_TOOL::MODE::ZONE ), 300 );
    }

    // Ensure the left toolbar's Line modes group reflects the current setting at startup
    if( m_toolMgr )
        m_toolMgr->RunAction( PCB_ACTIONS::angleSnapModeChanged );

    return true;
}


int BOARD_EDITOR_CONTROL::Save( const TOOL_EVENT& aEvent )
{
    m_frame->SaveBoard();
    return 0;
}


int BOARD_EDITOR_CONTROL::SaveAs( const TOOL_EVENT& aEvent )
{
    m_frame->SaveBoard( true );
    return 0;
}


int BOARD_EDITOR_CONTROL::SaveCopy( const TOOL_EVENT& aEvent )
{
    m_frame->SaveBoard( true, true );
    return 0;
}


int BOARD_EDITOR_CONTROL::ExportFootprints( const TOOL_EVENT& aEvent )
{
    m_frame->ExportFootprintsToLibrary( false );
    return 0;
}


int BOARD_EDITOR_CONTROL::PageSettings( const TOOL_EVENT& aEvent )
{
    PICKED_ITEMS_LIST   undoCmd;
    DS_PROXY_UNDO_ITEM* undoItem = new DS_PROXY_UNDO_ITEM( m_frame );
    ITEM_PICKER         wrapper( nullptr, undoItem, UNDO_REDO::PAGESETTINGS );

    undoCmd.PushItem( wrapper );
    undoCmd.SetDescription( _( "Page Settings" ) );
    m_frame->SaveCopyInUndoList( undoCmd, UNDO_REDO::PAGESETTINGS );

    DIALOG_PAGES_SETTINGS dlg( m_frame, m_frame->GetBoard()->GetEmbeddedFiles(), pcbIUScale.IU_PER_MILS,
                               VECTOR2I( MAX_PAGE_SIZE_PCBNEW_MILS, MAX_PAGE_SIZE_PCBNEW_MILS ) );
    dlg.SetWksFileName( BASE_SCREEN::m_DrawingSheetFileName );

    if( dlg.ShowModal() == wxID_OK )
    {
        m_frame->GetCanvas()->GetView()->UpdateAllItemsConditionally(
                [&]( KIGFX::VIEW_ITEM* aItem ) -> int
                {
                    EDA_TEXT* text = dynamic_cast<EDA_TEXT*>( aItem );

                    if( text && text->HasTextVars() )
                    {
                        text->ClearRenderCache();
                        text->ClearBoundingBoxCache();
                        return KIGFX::GEOMETRY | KIGFX::REPAINT;
                    }

                    return 0;
                } );

        m_frame->OnModify();
    }
    else
    {
        m_frame->RollbackFromUndo();
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::Plot( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "pcb_plot_opened", {
        { "app_type", "pcbnew" },
    } );
    DIALOG_PLOT dlg( m_frame );
    dlg.ShowQuasiModal();
    return 0;
}


int BOARD_EDITOR_CONTROL::Search( const TOOL_EVENT& aEvent )
{
    m_frame->ToggleSearch();
    return 0;
}


int BOARD_EDITOR_CONTROL::Find( const TOOL_EVENT& aEvent )
{
    m_frame->ShowFindDialog();
    return 0;
}


int BOARD_EDITOR_CONTROL::FindNext( const TOOL_EVENT& aEvent )
{
    m_frame->FindNext( aEvent.IsAction( &ACTIONS::findPrevious ) );
    return 0;
}


int BOARD_EDITOR_CONTROL::BoardSetup( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "board_setup_opened", {
        { "app_type", "pcbnew" },
    } );
    getEditFrame<PCB_EDIT_FRAME>()->ShowBoardSetupDialog();
    return 0;
}


int BOARD_EDITOR_CONTROL::ImportNetlist( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "netlist_import_dialog_opened", {
        { "app_type", "pcbnew" },
    } );
    getEditFrame<PCB_EDIT_FRAME>()->InstallNetlistFrame();
    return 0;
}


int BOARD_EDITOR_CONTROL::ImportSpecctraSession( const TOOL_EVENT& aEvent )
{
    wxString fullFileName = frame()->GetBoard()->GetFileName();
    wxString path;
    wxString name;
    wxString ext;

    wxFileName::SplitPath( fullFileName, &path, &name, &ext );
    name += wxT( "." ) + wxString( FILEEXT::SpecctraSessionFileExtension );

    fullFileName = wxFileSelector( _( "Specctra Session File" ), path, name,
                                   wxT( "." ) + wxString( FILEEXT::SpecctraSessionFileExtension ),
                                   FILEEXT::SpecctraSessionFileWildcard(), wxFD_OPEN | wxFD_CHANGE_DIR,
                                   frame() );

    if( !fullFileName.IsEmpty() )
        getEditFrame<PCB_EDIT_FRAME>()->ImportSpecctraSession( fullFileName );

    return 0;
}


// SSE streaming context for cloud autoroute
namespace {
struct AutorouteStreamContext
{
    std::string        buffer;
    std::string        sesContent;
    std::string        lastMessage;
    std::string        error;
    bool               done;
    bool               hasError;
    std::atomic<bool>* stopRequested;
    std::function<void( const std::string& )> progressCallback;
};

size_t autoroute_stream_callback( void* contents, size_t size, size_t nmemb, void* userp )
{
    size_t realsize = size * nmemb;
    AutorouteStreamContext* ctx = static_cast<AutorouteStreamContext*>( userp );

    if( !ctx || ( ctx->stopRequested && ctx->stopRequested->load() ) )
        return 0; // Signal to abort

    ctx->buffer.append( static_cast<const char*>( contents ), realsize );

    // Process complete SSE events (format: "data: {...}\n\n")
    size_t pos;
    while( ( pos = ctx->buffer.find( "\n\n" ) ) != std::string::npos )
    {
        std::string eventBlock = ctx->buffer.substr( 0, pos );
        ctx->buffer.erase( 0, pos + 2 );

        // Process each line in the event block
        std::istringstream stream( eventBlock );
        std::string line;
        while( std::getline( stream, line ) )
        {
            // Skip empty lines
            if( line.empty() || line == "\r" )
                continue;

            // Parse SSE data lines
            if( line.substr( 0, 6 ) == "data: " )
            {
                std::string jsonStr = line.substr( 6 );
                try
                {
                    nlohmann::json j = nlohmann::json::parse( jsonStr );
                    std::string type = j.value( "type", "" );

                    if( type == "progress" )
                    {
                        std::string message = j.value( "message", "" );
                        if( !message.empty() )
                        {
                            ctx->lastMessage = message;
                            if( ctx->progressCallback )
                                ctx->progressCallback( message );
                        }
                    }
                    else if( type == "done" )
                    {
                        ctx->done = true;
                        if( j.contains( "data" ) && j["data"].contains( "ses_content" )
                            && j["data"]["ses_content"].is_string() )
                        {
                            ctx->sesContent = j["data"]["ses_content"].get<std::string>();
                        }
                        else if( j.contains( "ses_content" ) && j["ses_content"].is_string() )
                        {
                            ctx->sesContent = j["ses_content"].get<std::string>();
                        }
                    }
                    else if( type == "error" )
                    {
                        ctx->hasError = true;
                        ctx->error = j.value( "message", "Unknown error" );
                        if( j.contains( "data" ) && j["data"].contains( "message" )
                            && j["data"]["message"].is_string() )
                        {
                            ctx->error = j["data"]["message"].get<std::string>();
                        }
                    }
                }
                catch( const nlohmann::json::exception& )
                {
                    // Skip malformed JSON
                }
            }
        }
    }

    return realsize;
}
} // anonymous namespace


/**
 * Perform cloud autorouting with the given parameters.
 * This is a shared helper used by both the CloudAutoroute button and the AI tool callback.
 * 
 * @param aBoard The board to autoroute.
 * @param aEditFrame The PCB edit frame (can be nullptr for headless operation).
 * @param aParams Autorouting parameters (can be empty object).
 * @param aProgressCallback Optional callback for progress updates.
 * @param aStopRequested Optional atomic flag to check for cancellation.
 * @return JSON result with success, message, and progress_log fields.
 */
nlohmann::json PerformCloudAutoroute( BOARD* aBoard,
                                       PCB_EDIT_FRAME* aEditFrame,
                                       const nlohmann::json& aParams,
                                       std::function<void( const std::string& )> aProgressCallback,
                                       std::atomic<bool>* aStopRequested )
{
    nlohmann::json result;
    result["success"] = false;
    result["progress_log"] = nlohmann::json::array();

    auto logProgress = [&]( const std::string& msg )
    {
        result["progress_log"].push_back( msg );
        if( aProgressCallback )
            aProgressCallback( msg );
    };

    // Check authentication
    if( !AUTH_MANAGER::Instance().IsAuthenticated() )
    {
        result["message"] = "Authentication required. Please sign in to use cloud autorouting.";
        return result;
    }

    // Get auth token
    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();
    if( authToken.IsEmpty() )
    {
        result["message"] = "Could not retrieve authentication token. Please sign in again.";
        return result;
    }

    logProgress( "Exporting board to DSN format..." );

    // Export board to DSN string
    std::string dsnContent;
    try
    {
        dsnContent = DSN::ExportBoardToSpecctraString( aBoard );
    }
    catch( const IO_ERROR& ioe )
    {
        result["message"] = std::string( "Failed to export board to DSN format: " ) + ioe.What().ToStdString();
        return result;
    }

    if( dsnContent.empty() )
    {
        result["message"] = "Failed to export board to DSN format.";
        return result;
    }

    logProgress( "Connecting to autoroute server..." );

    // Prepare request with new API format: dsn_content + params
    nlohmann::json payload;
    payload["dsn_content"] = dsnContent;
    payload["params"] = aParams;

    std::string url;
    #ifdef TRACE_BACKEND_URL
    
    url = std::string( TRACE_BACKEND_URL ) + "/pcb/autoroute";
    #else
    url = "http://localhost:8000/api/v2/pcb/autoroute";
    #endif

    std::string body = payload.dump();

    // Setup streaming context
    std::atomic<bool> localStopRequested( false );
    std::atomic<bool>* stopPtr = aStopRequested ? aStopRequested : &localStopRequested;
    std::atomic<bool> curlCompleted( false );
    std::atomic<int> curlResult( 0 );
    std::atomic<int> httpCode( 0 );
    std::atomic<bool> timeoutOccurred( false );
    
    AutorouteStreamContext ctx;
    ctx.done = false;
    ctx.hasError = false;
    ctx.stopRequested = stopPtr;
    ctx.progressCallback = logProgress;

    // Perform HTTP request with streaming
    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    curl.SetHeader( "Accept", "text/event-stream" );
    curl.SetHeader( "Authorization", std::string( "Bearer " ) + std::string( authToken.ToUTF8() ) );

    // Set timeouts - 60 seconds
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 60L );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_CONNECTTIMEOUT, 30L );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEFUNCTION, autoroute_stream_callback );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEDATA, &ctx );

    // Set transfer callback to check for cancellation frequently
    curl.SetTransferCallback( [stopPtr]( size_t, size_t, size_t, size_t ) -> int
    {
        return stopPtr->load() ? 1 : 0;
    }, 100000L );

    logProgress( "Sending board to autoroute server..." );

    // Start background thread for curl operation
    auto startTime = std::chrono::steady_clock::now();
    constexpr int timeoutSeconds = 60;
    
    std::thread curlThread( [&]()
    {
        curlResult.store( curl.Perform() );
        httpCode.store( curl.GetResponseStatusCode() );
        curlCompleted.store( true );
    } );

    // Polling loop - check for cancellation and timeout
    while( !curlCompleted.load() )
    {
        if( stopPtr->load() )
            break;

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>( currentTime - startTime ).count();
        
        if( elapsed >= timeoutSeconds )
        {
            timeoutOccurred.store( true );
            stopPtr->store( true );
            logProgress( "Autorouting took too long. Cancelling..." );
            break;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    // Wait for background thread to finish
    if( curlThread.joinable() )
    {
        if( stopPtr->load() || timeoutOccurred.load() )
        {
            auto joinStart = std::chrono::steady_clock::now();
            constexpr int joinTimeoutMs = 2000;
            
            while( curlThread.joinable() && !curlCompleted.load() )
            {
                auto joinElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - joinStart ).count();
                
                if( joinElapsed > joinTimeoutMs )
                {
                    curlThread.detach();
                    break;
                }
                
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            }
            
            if( curlThread.joinable() )
                curlThread.join();
        }
        else
        {
            curlThread.join();
        }
    }

    // Handle cancellation or timeout
    if( stopPtr->load() && !curlCompleted.load() )
    {
        if( timeoutOccurred.load() )
            result["message"] = "Autorouting took too long (>1 minute). Operation cancelled.";
        else
            result["message"] = "Autorouting was cancelled.";
        return result;
    }

    int finalCurlResult = curlResult.load();
    int finalHttpCode = httpCode.load();

    if( finalCurlResult != CURLE_OK )
    {
        result["message"] = std::string( "Failed to connect to autoroute server: " ) + 
                           curl_easy_strerror( static_cast<CURLcode>( finalCurlResult ) );
        return result;
    }

    if( finalHttpCode != 200 )
    {
        result["message"] = "Autoroute server returned error code " + std::to_string( finalHttpCode );
        return result;
    }

    if( ctx.hasError )
    {
        result["message"] = ctx.error;
        return result;
    }

    if( !ctx.done || ctx.sesContent.empty() )
    {
        result["message"] = "No routing result received from server.";
        return result;
    }

    logProgress( "Importing routing results..." );

    // Import SES content into the board (this modifies the board data structure, not UI)
    try
    {
        DSN::ImportSpecctraSessionFromString( aBoard, ctx.sesContent );
    }
    catch( const IO_ERROR& ioe )
    {
        result["message"] = std::string( "Failed to import routing results: " ) + ioe.What().ToStdString();
        return result;
    }

    // Store SES content in result for potential debugging
    result["ses_content_imported"] = true;

    logProgress( "Autorouting complete!" );

    result["success"] = true;
    result["message"] = "Autorouting completed successfully.";
    return result;
}


int BOARD_EDITOR_CONTROL::CloudAutoroute( const TOOL_EVENT& aEvent )
{
    auto routeStartTime = std::chrono::steady_clock::now();
    PCB_EDIT_FRAME* editFrame = getEditFrame<PCB_EDIT_FRAME>();
    BOARD* board = editFrame->GetBoard();

    // Create progress dialog with cancel button
    WX_PROGRESS_REPORTER progressDlg( editFrame, _( "Cloud Autorouting" ), 1, PR_CAN_ABORT );
    progressDlg.SetCurrentProgress( 0.0 );
    progressDlg.Report( _( "Starting autoroute..." ) );
    progressDlg.KeepRefreshing();

    // Setup stop flag for cancellation
    std::atomic<bool> stopRequested( false );

    int preRouteTrackCount = static_cast<int>( board->Tracks().size() );
    int totalNetCount = board->GetNetCount();
    int footprintCount = static_cast<int>( board->Footprints().size() );

    AMPLITUDE_CLIENT::Instance().Track( "cloud_autoroute_started", {
        { "track_count_before", preRouteTrackCount },
        { "net_count", totalNetCount },
        { "footprint_count", footprintCount }
    });

    // Progress callback that updates the dialog (must be thread-safe for wxWidgets)
    // Note: WX_PROGRESS_REPORTER::Report is thread-safe
    auto progressCallback = [&progressDlg]( const std::string& message )
    {
        progressDlg.Report( wxString::FromUTF8( message ) );
    };

    // Run autoroute in a background thread so we can update UI
    std::atomic<bool> completed( false );
    nlohmann::json result;

    // Clear view before import (on main thread)
    editFrame->ClearUndoRedoList();
    if( editFrame->GetCanvas() )
    {
        for( PCB_TRACK* track : board->Tracks() )
            editFrame->GetCanvas()->GetView()->Remove( track );
    }

    std::thread autorouteThread( [&]()
    {
        // PerformCloudAutoroute now only does network operations and board data modification
        // UI operations are done on the main thread after this completes
        result = PerformCloudAutoroute( board, nullptr, nlohmann::json::object(),
                                        progressCallback, &stopRequested );
        completed.store( true );
    } );

    // Main thread polling loop - check for cancellation and update UI
    while( !completed.load() )
    {
        if( !progressDlg.KeepRefreshing() )
        {
            // User clicked cancel button
            stopRequested.store( true );
            break;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    // Wait for thread to finish
    if( autorouteThread.joinable() )
    {
        if( stopRequested.load() && !completed.load() )
        {
            // Give thread a moment to finish after cancellation
            auto joinStart = std::chrono::steady_clock::now();
            while( !completed.load() )
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - joinStart ).count();
                if( elapsed > 2000 )
                {
                    autorouteThread.detach();
                    break;
                }
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            }
            if( autorouteThread.joinable() )
                autorouteThread.join();
        }
        else
        {
            autorouteThread.join();
        }
    }

    progressDlg.SetCurrentProgress( 1.0 );

    // Handle result and do UI operations on main thread
    auto routeEndTime = std::chrono::steady_clock::now();
    double routeDurationSec = std::chrono::duration<double>( routeEndTime - routeStartTime ).count();

    if( result.value( "success", false ) )
    {
        // UI operations must happen on main thread
        editFrame->OnModify();

        if( editFrame->GetCanvas() )
        {
            for( PCB_TRACK* track : board->Tracks() )
                editFrame->GetCanvas()->GetView()->Add( track );
        }

        // Save board and sync to trace_pcb
        wxString boardFileName = board->GetFileName();
        if( !boardFileName.IsEmpty() )
        {
            editFrame->SavePcbFile( boardFileName );
            ConvertKicadPcbToTracePcb( boardFileName );
        }

        int postRouteTrackCount = static_cast<int>( board->Tracks().size() );
        int tracksAdded = postRouteTrackCount - preRouteTrackCount;

        AMPLITUDE_CLIENT::Instance().Track( "cloud_autoroute_completed", {
            { "success", true },
            { "duration_seconds", routeDurationSec },
            { "track_count_before", preRouteTrackCount },
            { "track_count_after", postRouteTrackCount },
            { "tracks_added", tracksAdded },
            { "net_count", totalNetCount },
            { "footprint_count", footprintCount }
        });

        editFrame->Refresh();
        editFrame->SetStatusText( _( "Cloud autorouting completed successfully." ) );
    }
    else
    {
        // Restore view if autoroute failed (re-add tracks that were removed)
        if( editFrame->GetCanvas() )
        {
            for( PCB_TRACK* track : board->Tracks() )
                editFrame->GetCanvas()->GetView()->Add( track );
        }
        editFrame->Refresh();

        wxString errorMsg = wxString::FromUTF8( result.value( "message", "Unknown error" ) );

        AMPLITUDE_CLIENT::Instance().Track( "cloud_autoroute_completed", {
            { "success", false },
            { "duration_seconds", routeDurationSec },
            { "error_message", result.value( "message", "Unknown error" ) },
            { "track_count_before", preRouteTrackCount },
            { "net_count", totalNetCount },
            { "footprint_count", footprintCount }
        });

        DisplayErrorMessage( editFrame, _( "Autoroute Error" ), errorMsg );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ExportSpecctraDSN( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "specctra_dsn_exported" );
    wxString    fullFileName = m_frame->GetLastPath( LAST_PATH_SPECCTRADSN );
    wxFileName  fn;

    if( fullFileName.IsEmpty() )
    {
        fn = m_frame->GetBoard()->GetFileName();
        fn.SetExt( FILEEXT::SpecctraDsnFileExtension );
    }
    else
    {
        fn = fullFileName;
    }

    fullFileName = wxFileSelector( _( "Specctra DSN File" ), fn.GetPath(), fn.GetFullName(),
                                   FILEEXT::SpecctraDsnFileExtension, FILEEXT::SpecctraDsnFileWildcard(),
                                   wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxFD_CHANGE_DIR, frame() );

    if( !fullFileName.IsEmpty() )
    {
        m_frame->SetLastPath( LAST_PATH_SPECCTRADSN, fullFileName );
        getEditFrame<PCB_EDIT_FRAME>()->ExportSpecctraFile( fullFileName );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ExportNetlist( const TOOL_EVENT& aEvent )
{
    wxCHECK( m_frame, 0 );

    wxFileName fn = m_frame->Prj().GetProjectFullName();

    // Use a different file extension for the board netlist so the schematic netlist file
    // is accidentally overwritten.
    fn.SetExt( wxT( "pcb_net" ) );

    wxFileDialog dlg( m_frame, _( "Export Board Netlist" ), fn.GetPath(), fn.GetFullName(),
                      _( "KiCad board netlist files" ) + AddFileExtListToFilter( { "pcb_net" } ),
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

    dlg.SetExtraControlCreator( &LEGACYFILEDLG_NETLIST_OPTIONS::Create );

    KIPLATFORM::UI::AllowNetworkFileSystems( &dlg );

    if( dlg.ShowModal() == wxID_CANCEL )
        return 0;

    fn = dlg.GetPath();

    if( !fn.IsDirWritable() )
    {
        DisplayErrorMessage( m_frame, wxString::Format( _( "Insufficient permissions to folder '%s'." ),
                                                        fn.GetPath() ) );
        return 0;
    }

    const LEGACYFILEDLG_NETLIST_OPTIONS* noh =
            dynamic_cast<const LEGACYFILEDLG_NETLIST_OPTIONS*>( dlg.GetExtraControl() );
    wxCHECK( noh, 0 );

    NETLIST netlist;

    for( const FOOTPRINT* footprint : board()->Footprints() )
    {
        COMPONENT* component = new COMPONENT( footprint->GetFPID(), footprint->GetReference(),
                                              footprint->GetValue(), footprint->GetPath(),
                                              { footprint->m_Uuid } );

        for( const PAD* pad : footprint->Pads() )
        {
            const wxString& netname = pad->GetShortNetname();

            if( !netname.IsEmpty() )
                component->AddNet( pad->GetNumber(), netname, pad->GetPinFunction(), pad->GetPinType() );
        }

        nlohmann::ordered_map<wxString, wxString> fields;

        for( PCB_FIELD* field : footprint->GetFields() )
        {
            wxCHECK2( field, continue );

            fields[field->GetCanonicalName()] = field->GetText();
        }

        component->SetFields( fields );

        netlist.AddComponent( component );
    }

    FILE_OUTPUTFORMATTER formatter( fn.GetFullPath() );

    netlist.Format( "pcb_netlist", &formatter, 0, noh->GetNetlistOptions() );

    return 0;
}


int BOARD_EDITOR_CONTROL::GenerateGerbers( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "gerbers_generated" );
    PCB_PLOT_PARAMS plotSettings = m_frame->GetPlotSettings();

    plotSettings.SetFormat( PLOT_FORMAT::GERBER );

    m_frame->SetPlotSettings( plotSettings );

    DIALOG_PLOT dlg( m_frame );
    dlg.ShowQuasiModal(  );

    return 0;
}


int BOARD_EDITOR_CONTROL::RepairBoard( const TOOL_EVENT& aEvent )
{
    int      errors = 0;
    wxString details;
    bool     quiet = aEvent.Parameter<bool>();

    // Repair duplicate IDs and missing nets.
    std::set<KIID> ids;
    int            duplicates = 0;

    auto processItem =
            [&]( EDA_ITEM* aItem )
            {
                if( ids.count( aItem->m_Uuid ) )
                {
                    duplicates++;
                    const_cast<KIID&>( aItem->m_Uuid ) = KIID();
                }

                ids.insert( aItem->m_Uuid );

                BOARD_CONNECTED_ITEM* cItem = dynamic_cast<BOARD_CONNECTED_ITEM*>( aItem );

                if( cItem && cItem->GetNetCode() )
                {
                    NETINFO_ITEM* netinfo = cItem->GetNet();

                    if( netinfo && !board()->FindNet( netinfo->GetNetname() ) )
                    {
                        board()->Add( netinfo );

                        details += wxString::Format( _( "Orphaned net %s re-parented.\n" ),
                                                     netinfo->GetNetname() );
                        errors++;
                    }
                }
            };

    // Footprint IDs are the most important, so give them the first crack at "claiming" a
    // particular KIID.

    for( FOOTPRINT* footprint : board()->Footprints() )
        processItem( footprint );

    // After that the principal use is for DRC marker pointers, which are most likely to pads
    // or tracks.

    for( FOOTPRINT* footprint : board()->Footprints() )
    {
        for( PAD* pad : footprint->Pads() )
            processItem( pad );
    }

    for( PCB_TRACK* track : board()->Tracks() )
        processItem( track );

    // From here out I don't think order matters much.

    for( FOOTPRINT* footprint : board()->Footprints() )
    {
        processItem( &footprint->Reference() );
        processItem( &footprint->Value() );

        for( BOARD_ITEM* item : footprint->GraphicalItems() )
            processItem( item );

        for( ZONE* zone : footprint->Zones() )
            processItem( zone );

        for( PCB_GROUP* group : footprint->Groups() )
            processItem( group );
    }

    // Everything owned by the board not handled above
    for( BOARD_ITEM* item : board()->GetItemSet() )
    {
        // Top-level footprints and tracks were handled above.
        switch( item->Type() )
        {
        case PCB_FOOTPRINT_T:
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
            break;

        default:
            processItem( item );
            break;
        }
    }

    if( duplicates )
    {
        errors += duplicates;
        details += wxString::Format( _( "%d duplicate IDs replaced.\n" ), duplicates );
    }

    /*******************************
     * Your test here
     */

    /*******************************
     * Inform the user
     */

    if( errors )
    {
        m_frame->OnModify();

        wxString msg = wxString::Format( _( "%d potential problems repaired." ), errors );

        if( !quiet )
            DisplayInfoMessage( m_frame, msg, details );
    }
    else if( !quiet )
    {
        DisplayInfoMessage( m_frame, _( "No board problems found." ) );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::UpdatePCBFromSchematic( const TOOL_EVENT& aEvent )
{
    AMPLITUDE_CLIENT::Instance().Track( "pcb_updated_from_schematic" );
    NETLIST netlist;

    if( m_frame->FetchNetlistFromSchematic( netlist, _( "Updating PCB requires a fully annotated "
                                                        "schematic." ) ) )
    {
        DIALOG_UPDATE_PCB updateDialog( m_frame, &netlist );
        updateDialog.ShowModal();
    }

    return 0;
}

int BOARD_EDITOR_CONTROL::UpdateSchematicFromPCB( const TOOL_EVENT& aEvent )
{
    if( Kiface().IsSingle() )
    {
        DisplayErrorMessage( m_frame, _( "Cannot update schematic because Pcbnew is opened in "
                                         "stand-alone mode. In order to create or update PCBs "
                                         "from schematics, you must launch the KiCad project "
                                         "manager and create a project." ) );
        return 0;
    }

    TOOL_EVENT dummy;
    ShowEeschema( dummy );

    KIWAY_PLAYER* frame = m_frame->Kiway().Player( FRAME_SCH, false );

    if( frame )
    {
        std::string payload;

        if( wxWindow* blocking_win = frame->Kiway().GetBlockingDialog() )
            blocking_win->Close( true );

        m_frame->Kiway().ExpressMail( FRAME_SCH, MAIL_SCH_UPDATE, payload, m_frame );
    }
    return 0;
}


int BOARD_EDITOR_CONTROL::ShowEeschema( const TOOL_EVENT& aEvent )
{
    wxString        msg;
    PCB_EDIT_FRAME* boardFrame = m_frame;
    PROJECT&        project = boardFrame->Prj();
    wxFileName      schematic( project.GetProjectPath(), project.GetProjectName(),
                               FILEEXT::KiCadSchematicFileExtension );

    if( !schematic.FileExists() )
    {
        wxFileName legacySchematic( project.GetProjectPath(), project.GetProjectName(),
                                    FILEEXT::LegacySchematicFileExtension );

        if( legacySchematic.FileExists() )
        {
            schematic = legacySchematic;
        }
        else
        {
            msg.Printf( _( "Schematic file '%s' not found." ), schematic.GetFullPath() );
            DisplayErrorMessage( m_frame, msg );
            return 0;
        }
    }

    if( Kiface().IsSingle() )
    {
        ExecuteFile( EESCHEMA_EXE, schematic.GetFullPath() );
    }
    else
    {
        KIWAY_PLAYER* frame = m_frame->Kiway().Player( FRAME_SCH, false );

        // Please: note: DIALOG_EDIT_LIBENTRY_FIELDS_IN_LIB::initBuffers() calls
        // Kiway.Player( FRAME_SCH, true )
        // therefore, the schematic editor is sometimes running, but the schematic project
        // is not loaded, if the library editor was called, and the dialog field editor was used.
        // On Linux, it happens the first time the schematic editor is launched, if
        // library editor was running, and the dialog field editor was open
        // On Windows, it happens always after the library editor was called,
        // and the dialog field editor was used
        if( !frame )
        {
            try
            {
                frame = boardFrame->Kiway().Player( FRAME_SCH, true );
            }
            catch( const IO_ERROR& err )
            {

                DisplayErrorMessage( boardFrame, _( "Eeschema failed to load." ) + wxS( "\n" ) + err.What() );
                return 0;
            }
        }

        wxEventBlocker blocker( boardFrame );

        // If Kiway() cannot create the eeschema frame, it shows a error message, and
        // frame is null
        if( !frame )
            return 0;

        if( !frame->IsShownOnScreen() ) // the frame exists, (created by the dialog field editor)
                                        // but no project loaded.
        {
            frame->OpenProjectFiles( std::vector<wxString>( 1, schematic.GetFullPath() ) );
            frame->Show( true );
        }

        // On Windows, Raise() does not bring the window on screen, when iconized or not shown
        // On Linux, Raise() brings the window on screen, but this code works fine
        if( frame->IsIconized() )
        {
            frame->Iconize( false );

            // If an iconized frame was created by Pcbnew, Iconize( false ) is not enough
            // to show the frame at its normal size: Maximize should be called.
            frame->Maximize( false );
        }

        frame->Raise();
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleLayersManager( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleLayersManager();
    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleProperties( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleProperties();
    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleNetInspector( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleNetInspector();
    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleAIChat( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleAIChat();
    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleLibraryTree( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleLibraryTree();
    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleSearch( const TOOL_EVENT& aEvent )
{
    getEditFrame<PCB_EDIT_FRAME>()->ToggleSearch();
    return 0;
}


int BOARD_EDITOR_CONTROL::TogglePythonConsole( const TOOL_EVENT& aEvent )
{
    m_frame->ScriptingConsoleEnableDisable();
    return 0;
}


// Track & via size control
int BOARD_EDITOR_CONTROL::TrackWidthInc( const TOOL_EVENT& aEvent )
{
    BOARD_DESIGN_SETTINGS& bds = getModel<BOARD>()->GetDesignSettings();
    PCB_SELECTION&         selection = m_toolMgr->GetTool<PCB_SELECTION_TOOL>()->GetSelection();

    if( m_frame->ToolStackIsEmpty()
        && SELECTION_CONDITIONS::OnlyTypes( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } )( selection ) )
    {
        BOARD_COMMIT commit( this );

        for( EDA_ITEM* item : selection )
        {
            if( item->IsType( { PCB_TRACE_T, PCB_ARC_T } ) )
            {
                PCB_TRACK* track = static_cast<PCB_TRACK*>( item );

                for( int i = 0; i < (int) bds.m_TrackWidthList.size(); ++i )
                {
                    int candidate = bds.m_NetSettings->GetDefaultNetclass()->GetTrackWidth();

                    if( i > 0 )
                        candidate = bds.m_TrackWidthList[ i ];

                    if( candidate > track->GetWidth() )
                    {
                        commit.Modify( track );
                        track->SetWidth( candidate );
                        break;
                    }
                }
            }
        }

        commit.Push( _( "Increase Track Width" ) );
        return 0;
    }

    ROUTER_TOOL* routerTool = m_toolMgr->GetTool<ROUTER_TOOL>();

    if( routerTool && routerTool->IsToolActive()
        && routerTool->Router()->Mode() == PNS::PNS_MODE_ROUTE_DIFF_PAIR )
    {
        int widthIndex = bds.GetDiffPairIndex() + 1;

        // If we go past the last track width entry in the list, start over at the beginning
        if( widthIndex >= (int) bds.m_DiffPairDimensionsList.size() )
            widthIndex = 0;

        bds.SetDiffPairIndex( widthIndex );
        bds.UseCustomDiffPairDimensions( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }
    else
    {
        int widthIndex = bds.GetTrackWidthIndex();

        if( routerTool && routerTool->IsToolActive()
            && routerTool->Router()->GetState() == PNS::ROUTER::RouterState::ROUTE_TRACK
            && bds.m_UseConnectedTrackWidth && !bds.m_TempOverrideTrackWidth )
        {
            bds.m_TempOverrideTrackWidth = true;
        }
        else
        {
            widthIndex++;
        }

        // If we go past the last track width entry in the list, start over at the beginning
        if( widthIndex >= (int) bds.m_TrackWidthList.size() )
            widthIndex = 0;

        bds.SetTrackWidthIndex( widthIndex );
        bds.UseCustomTrackViaSize( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::TrackWidthDec( const TOOL_EVENT& aEvent )
{
    BOARD_DESIGN_SETTINGS& bds = getModel<BOARD>()->GetDesignSettings();
    PCB_SELECTION&         selection = m_toolMgr->GetTool<PCB_SELECTION_TOOL>()->GetSelection();

    if( m_frame->ToolStackIsEmpty()
        && SELECTION_CONDITIONS::OnlyTypes( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } )( selection ) )
    {
        BOARD_COMMIT commit( this );

        for( EDA_ITEM* item : selection )
        {
            if( item->IsType( { PCB_TRACE_T, PCB_ARC_T } ) )
            {
                PCB_TRACK* track = static_cast<PCB_TRACK*>( item );

                for( int i = (int) bds.m_TrackWidthList.size() - 1; i >= 0; --i )
                {
                    int candidate = bds.m_NetSettings->GetDefaultNetclass()->GetTrackWidth();

                    if( i > 0 )
                        candidate = bds.m_TrackWidthList[ i ];

                    if( candidate < track->GetWidth() )
                    {
                        commit.Modify( track );
                        track->SetWidth( candidate );
                        break;
                    }
                }
            }
        }

        commit.Push( _( "Decrease Track Width" ) );
        return 0;
    }

    ROUTER_TOOL* routerTool = m_toolMgr->GetTool<ROUTER_TOOL>();

    if( routerTool && routerTool->IsToolActive()
            && routerTool->Router()->Mode() == PNS::PNS_MODE_ROUTE_DIFF_PAIR )
    {
        int widthIndex = bds.GetDiffPairIndex() - 1;

        // If we get to the lowest entry start over at the highest
        if( widthIndex < 0 )
            widthIndex = bds.m_DiffPairDimensionsList.size() - 1;

        bds.SetDiffPairIndex( widthIndex );
        bds.UseCustomDiffPairDimensions( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }
    else
    {
        int widthIndex = bds.GetTrackWidthIndex();

        if( routerTool && routerTool->IsToolActive()
            && routerTool->Router()->GetState() == PNS::ROUTER::RouterState::ROUTE_TRACK
            && bds.m_UseConnectedTrackWidth && !bds.m_TempOverrideTrackWidth )
        {
            bds.m_TempOverrideTrackWidth = true;
        }
        else
        {
            widthIndex--;
        }

        // If we get to the lowest entry start over at the highest
        if( widthIndex < 0 )
            widthIndex = (int) bds.m_TrackWidthList.size() - 1;

        bds.SetTrackWidthIndex( widthIndex );
        bds.UseCustomTrackViaSize( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ViaSizeInc( const TOOL_EVENT& aEvent )
{
    BOARD_DESIGN_SETTINGS& bds = getModel<BOARD>()->GetDesignSettings();
    PCB_SELECTION&         selection = m_toolMgr->GetTool<PCB_SELECTION_TOOL>()->GetSelection();

    if( m_frame->ToolStackIsEmpty()
        && SELECTION_CONDITIONS::OnlyTypes( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } )( selection ) )
    {
        BOARD_COMMIT commit( this );

        for( EDA_ITEM* item : selection )
        {
            if( item->Type() == PCB_VIA_T )
            {
                PCB_VIA* via = static_cast<PCB_VIA*>( item );

                for( int i = 0; i < (int) bds.m_ViasDimensionsList.size(); ++i )
                {
                    VIA_DIMENSION dims( bds.m_NetSettings->GetDefaultNetclass()->GetViaDiameter(),
                                        bds.m_NetSettings->GetDefaultNetclass()->GetViaDrill() );

                    if( i> 0 )
                        dims = bds.m_ViasDimensionsList[ i ];

                    // TODO(JE) padstacks
                    if( dims.m_Diameter > via->GetWidth( PADSTACK::ALL_LAYERS ) )
                    {
                        commit.Modify( via );
                        via->SetWidth( PADSTACK::ALL_LAYERS, dims.m_Diameter );
                        via->SetDrill( dims.m_Drill );
                        break;
                    }
                }
            }
        }

        commit.Push( _( "Increase Via Size" ) );
    }
    else
    {
        int sizeIndex = bds.GetViaSizeIndex() + 1;

        // If we go past the last via entry in the list, start over at the beginning
        if( sizeIndex >= (int) bds.m_ViasDimensionsList.size() )
            sizeIndex = 0;

        bds.SetViaSizeIndex( sizeIndex );
        bds.UseCustomTrackViaSize( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ViaSizeDec( const TOOL_EVENT& aEvent )
{
    BOARD_DESIGN_SETTINGS& bds = getModel<BOARD>()->GetDesignSettings();
    PCB_SELECTION&         selection = m_toolMgr->GetTool<PCB_SELECTION_TOOL>()->GetSelection();

    if( m_frame->ToolStackIsEmpty()
        && SELECTION_CONDITIONS::OnlyTypes( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } )( selection ) )
    {
        BOARD_COMMIT commit( this );

        for( EDA_ITEM* item : selection )
        {
            if( item->Type() == PCB_VIA_T )
            {
                PCB_VIA* via = static_cast<PCB_VIA*>( item );

                for( int i = (int) bds.m_ViasDimensionsList.size() - 1; i >= 0; --i )
                {
                    VIA_DIMENSION dims( bds.m_NetSettings->GetDefaultNetclass()->GetViaDiameter(),
                                        bds.m_NetSettings->GetDefaultNetclass()->GetViaDrill() );

                    if( i > 0 )
                        dims = bds.m_ViasDimensionsList[ i ];

                    // TODO(JE) padstacks
                    if( dims.m_Diameter < via->GetWidth( PADSTACK::ALL_LAYERS ) )
                    {
                        commit.Modify( via );
                        via->SetWidth( PADSTACK::ALL_LAYERS, dims.m_Diameter );
                        via->SetDrill( dims.m_Drill );
                        break;
                    }
                }
            }
        }

        commit.Push( "Decrease Via Size" );
    }
    else
    {
        int sizeIndex = 0; // Assume we only have a single via size entry

        // If there are more, cycle through them backwards
        if( bds.m_ViasDimensionsList.size() > 0 )
        {
            sizeIndex = bds.GetViaSizeIndex() - 1;

            // If we get to the lowest entry start over at the highest
            if( sizeIndex < 0 )
                sizeIndex = bds.m_ViasDimensionsList.size() - 1;
        }

        bds.SetViaSizeIndex( sizeIndex );
        bds.UseCustomTrackViaSize( false );

        m_toolMgr->RunAction( PCB_ACTIONS::trackViaSizeChanged );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::AutoTrackWidth( const TOOL_EVENT& aEvent )
{
    BOARD_DESIGN_SETTINGS& bds = getModel<BOARD>()->GetDesignSettings();

    if( bds.UseCustomTrackViaSize() )
    {
        bds.UseCustomTrackViaSize( false );
        bds.m_UseConnectedTrackWidth = true;
    }
    else
    {
        bds.m_UseConnectedTrackWidth = !bds.m_UseConnectedTrackWidth;
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::PlaceFootprint( const TOOL_EVENT& aEvent )
{
    if( m_inPlaceFootprint )
        return 0;

    REENTRANCY_GUARD guard( &m_inPlaceFootprint );

    FOOTPRINT*            fp = aEvent.Parameter<FOOTPRINT*>();
    bool                  fromOtherCommand = fp != nullptr;
    KIGFX::VIEW_CONTROLS* controls = getViewControls();
    BOARD_COMMIT          commit( m_frame );
    BOARD*                board = getModel<BOARD>();
    COMMON_SETTINGS*      common_settings = Pgm().GetCommonSettings();

    m_toolMgr->RunAction( ACTIONS::selectionClear );

    TOOL_EVENT pushedEvent = aEvent;
    m_frame->PushTool( aEvent );

    auto setCursor =
            [&]()
            {
                m_frame->GetCanvas()->SetCurrentCursor( KICURSOR::PENCIL );
            };

    auto cleanup =
            [&] ()
            {
                m_toolMgr->RunAction( ACTIONS::selectionClear );
                commit.Revert();

                if( fromOtherCommand )
                {
                    PICKED_ITEMS_LIST* undo = m_frame->PopCommandFromUndoList();

                    if( undo )
                    {
                        m_frame->PutDataInPreviousState( undo );
                        m_frame->ClearListAndDeleteItems( undo );
                        delete undo;
                    }
                }

                fp = nullptr;
                m_placingFootprint = false;
            };

    Activate();
    // Must be done after Activate() so that it gets set into the correct context
    controls->ShowCursor( true );
    // Set initial cursor
    setCursor();

    VECTOR2I cursorPos = controls->GetCursorPosition();
    bool     ignorePrimePosition = false;
    bool     reselect = false;

    // Prime the pump
    if( fp )
    {
        m_placingFootprint = true;
        fp->SetPosition( cursorPos );
        m_toolMgr->RunAction<EDA_ITEM*>( ACTIONS::selectItem, fp );
        m_toolMgr->PostAction( ACTIONS::refreshPreview );
    }
    else if( aEvent.HasPosition() )
    {
        m_toolMgr->PrimeTool( aEvent.Position() );
    }
    else if( common_settings->m_Input.immediate_actions && !aEvent.IsReactivate() )
    {
        m_toolMgr->PrimeTool( { 0, 0 } );
        ignorePrimePosition = true;
    }

    // Main loop: keep receiving events
    while( TOOL_EVENT* evt = Wait() )
    {
        setCursor();
        cursorPos = controls->GetCursorPosition( !evt->DisableGridSnapping() );

        if( reselect && fp )
            m_toolMgr->RunAction<EDA_ITEM*>( ACTIONS::selectItem, fp );

        if( evt->IsCancelInteractive() || ( fp && evt->IsAction( &ACTIONS::undo ) ) )
        {
            if( fp )
            {
                cleanup();
            }
            else
            {
                m_frame->PopTool( pushedEvent );
                break;
            }
        }
        else if( evt->IsActivate() )
        {
            if( fp )
                cleanup();

            if( evt->IsMoveTool() )
            {
                // leave ourselves on the stack so we come back after the move
                break;
            }
            else
            {
                frame()->PopTool( pushedEvent );
                break;
            }
        }
        else if( evt->IsClick( BUT_LEFT ) )
        {
            if( !fp )
            {
                // Pick the footprint to be placed
                fp = m_frame->SelectFootprintFromLibrary();

                if( fp == nullptr )
                    continue;

                // If we started with a hotkey which has a position then warp back to that.
                // Otherwise update to the current mouse position pinned inside the autoscroll
                // boundaries.
                if( evt->IsPrime() && !ignorePrimePosition )
                {
                    cursorPos = evt->Position();
                    getViewControls()->WarpMouseCursor( cursorPos, true );
                }
                else
                {
                    getViewControls()->PinCursorInsideNonAutoscrollArea( true );
                    cursorPos = getViewControls()->GetMousePosition();
                }

                m_placingFootprint = true;

                fp->SetLink( niluuid );

                fp->SetFlags( IS_NEW ); // whatever

                // Set parent so that clearance can be loaded
                fp->SetParent( board );
                board->UpdateUserUnits( fp, m_frame->GetCanvas()->GetView() );

                for( PAD* pad : fp->Pads() )
                {
                    pad->SetLocalRatsnestVisible( m_frame->GetPcbNewSettings()->m_Display.m_ShowGlobalRatsnest );

                    // Pads in the library all have orphaned nets.  Replace with Default.
                    pad->SetNetCode( 0 );
                }

                // Put it on FRONT layer,
                // (Can be stored flipped if the lib is an archive built from a board)
                if( fp->IsFlipped() )
                    fp->Flip( fp->GetPosition(), m_frame->GetPcbNewSettings()->m_FlipDirection );

                fp->SetOrientation( ANGLE_0 );
                fp->SetPosition( cursorPos );

                commit.Add( fp );
                m_toolMgr->RunAction<EDA_ITEM*>( ACTIONS::selectItem, fp );

                m_toolMgr->PostAction( ACTIONS::refreshPreview );
            }
            else
            {
                m_toolMgr->RunAction( ACTIONS::selectionClear );
                commit.Push( _( "Place Footprint" ) );
                fp = nullptr;  // to indicate that there is no footprint that we currently modify
                m_placingFootprint = false;
            }
        }
        else if( evt->IsClick( BUT_RIGHT ) )
        {
            m_menu->ShowContextMenu( selection() );
        }
        else if( fp && ( evt->IsMotion() || evt->IsAction( &ACTIONS::refreshPreview ) ) )
        {
            fp->SetPosition( cursorPos );
            selection().SetReferencePoint( cursorPos );
            getView()->Update( &selection() );
            getView()->Update( fp );
        }
        else if( fp && evt->IsAction( &PCB_ACTIONS::properties ) )
        {
            // Calling 'Properties' action clears the selection, so we need to restore it
            reselect = true;
        }
        else if( fp && (   ZONE_FILLER_TOOL::IsZoneFillAction( evt )
                        || evt->IsAction( &ACTIONS::redo ) ) )
        {
            wxBell();
        }
        else
        {
            evt->SetPassEvent();
        }

        // Enable autopanning and cursor capture only when there is a footprint to be placed
        controls->SetAutoPan( fp != nullptr );
        controls->CaptureCursor( fp != nullptr );
    }

    controls->SetAutoPan( false );
    controls->CaptureCursor( false );
    m_frame->GetCanvas()->SetCurrentCursor( KICURSOR::ARROW );

    return 0;
}


int BOARD_EDITOR_CONTROL::ToggleLockSelected( const TOOL_EVENT& aEvent )
{
    return modifyLockSelected( TOGGLE );
}


int BOARD_EDITOR_CONTROL::LockSelected( const TOOL_EVENT& aEvent )
{
    return modifyLockSelected( ON );
}


int BOARD_EDITOR_CONTROL::UnlockSelected( const TOOL_EVENT& aEvent )
{
    return modifyLockSelected( OFF );
}


int BOARD_EDITOR_CONTROL::modifyLockSelected( MODIFY_MODE aMode )
{
    PCB_SELECTION_TOOL*  selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
    const PCB_SELECTION& selection = selTool->GetSelection();
    BOARD_COMMIT         commit( m_frame );

    if( selection.Empty() )
        m_toolMgr->RunAction( ACTIONS::selectionCursor );

    // Resolve TOGGLE mode
    if( aMode == TOGGLE )
    {
        aMode = ON;

        for( EDA_ITEM* item : selection )
        {
            if( !item->IsBOARD_ITEM() )
                continue;

            if( static_cast<BOARD_ITEM*>( item )->IsLocked() )
            {
                aMode = OFF;
                break;
            }
        }
    }

    for( EDA_ITEM* item : selection )
    {
        if( !item->IsBOARD_ITEM() )
            continue;

        BOARD_ITEM* const board_item = static_cast<BOARD_ITEM*>( item );

        // Disallow locking free pads - it's confusing and not persisted
        // through save/load anyway.
        if( board_item->Type() == PCB_PAD_T )
            continue;

        EDA_GROUP* parent_group = board_item->GetParentGroup();

        if( parent_group && parent_group->AsEdaItem()->Type() == PCB_GENERATOR_T )
        {
            PCB_GENERATOR* generator = static_cast<PCB_GENERATOR*>( parent_group );

            if( generator && commit.GetStatus( generator ) != CHT_MODIFY )
            {
                commit.Modify( generator );

                if( aMode == ON )
                    generator->SetLocked( true );
                else
                    generator->SetLocked( false );
            }
        }

        commit.Modify( board_item );

        if( aMode == ON )
            board_item->SetLocked( true );
        else
            board_item->SetLocked( false );
    }

    if( !commit.Empty() )
    {
        commit.Push( aMode == ON ? _( "Lock" ) : _( "Unlock" ), SKIP_TEARDROPS );

        m_toolMgr->PostEvent( EVENTS::SelectedEvent );
        m_frame->OnModify();
    }

    return 0;
}


static bool mergeZones( EDA_DRAW_FRAME* aFrame, BOARD_COMMIT& aCommit,
                        std::vector<ZONE*>& aOriginZones, std::vector<ZONE*>& aMergedZones )
{
    aCommit.Modify( aOriginZones[0] );

    aOriginZones[0]->Outline()->ClearArcs();

    for( unsigned int i = 1; i < aOriginZones.size(); i++ )
    {
        SHAPE_POLY_SET otherOutline = aOriginZones[i]->Outline()->CloneDropTriangulation();
        otherOutline.ClearArcs();
        aOriginZones[0]->Outline()->BooleanAdd( otherOutline );
    }

    aOriginZones[0]->Outline()->Simplify();

    // We should have one polygon, possibly with holes.  If we end up with two polygons (either
    // because the intersection was a single point or because the intersection was within one of
    // the zone's holes) then we can't merge.
    if( aOriginZones[0]->Outline()->IsSelfIntersecting() || aOriginZones[0]->Outline()->OutlineCount() > 1 )
    {
        DisplayErrorMessage( aFrame, _( "Zones have insufficient overlap for merging." ) );
        aCommit.Revert();
        return false;
    }

    for( unsigned int i = 1; i < aOriginZones.size(); i++ )
        aCommit.Remove( aOriginZones[i] );

    aMergedZones.push_back( aOriginZones[0] );

    aOriginZones[0]->SetLocalFlags( 1 );
    aOriginZones[0]->HatchBorder();
    aOriginZones[0]->CacheTriangulation();

    return true;
}


int BOARD_EDITOR_CONTROL::ZoneMerge( const TOOL_EVENT& aEvent )
{
    const PCB_SELECTION& selection = m_toolMgr->GetTool<PCB_SELECTION_TOOL>()->GetSelection();
    BOARD*               board = getModel<BOARD>();
    BOARD_COMMIT         commit( m_frame );

    if( selection.Size() < 2 )
        return 0;

    int netcode = -1;

    ZONE* firstZone = nullptr;
    std::vector<ZONE*> toMerge, merged;

    for( EDA_ITEM* item : selection )
    {
        ZONE* curr_area = dynamic_cast<ZONE*>( item );

        if( !curr_area )
            continue;

        if( !firstZone )
            firstZone = curr_area;

        netcode = curr_area->GetNetCode();

        if( firstZone->GetNetCode() != netcode )
        {
            wxLogMessage( _( "Some zone netcodes did not match and were not merged." ) );
            continue;
        }

        if( curr_area->GetAssignedPriority() != firstZone->GetAssignedPriority() )
        {
            wxLogMessage( _( "Some zone priorities did not match and were not merged." ) );
            continue;
        }

        if( curr_area->GetIsRuleArea() != firstZone->GetIsRuleArea() )
        {
            wxLogMessage( _( "Some zones were rule areas and were not merged." ) );
            continue;
        }

        if( curr_area->GetLayerSet() != firstZone->GetLayerSet() )
        {
            wxLogMessage( _( "Some zone layer sets did not match and were not merged." ) );
            continue;
        }

        bool intersects = curr_area == firstZone;

        for( ZONE* candidate : toMerge )
        {
            if( intersects )
                break;

            if( board->TestZoneIntersection( curr_area, candidate ) )
                intersects = true;
        }

        if( !intersects )
        {
            wxLogMessage( _( "Some zones did not intersect and were not merged." ) );
            continue;
        }

        toMerge.push_back( curr_area );
    }

    m_toolMgr->RunAction( ACTIONS::selectionClear );

    if( !toMerge.empty() )
    {
        if( mergeZones( m_frame, commit, toMerge, merged ) )
        {
            commit.Push( _( "Merge Zones" ) );

            for( EDA_ITEM* item : merged )
                m_toolMgr->RunAction( ACTIONS::selectItem, item );
        }
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::ZoneDuplicate( const TOOL_EVENT& aEvent )
{
    PCB_SELECTION_TOOL*  selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
    const PCB_SELECTION& selection = selTool->GetSelection();

    // because this pops up the zone editor, it would be confusing to handle multiple zones,
    // so just handle single selections containing exactly one zone
    if( selection.Size() != 1 )
        return 0;

    ZONE* oldZone = dynamic_cast<ZONE*>( selection[0] );

    if( !oldZone )
        return 0;

    ZONE_SETTINGS zoneSettings;
    zoneSettings << *oldZone;
    int dialogResult;

    if( oldZone->GetIsRuleArea() )
        dialogResult = InvokeRuleAreaEditor( m_frame, &zoneSettings, board() );
    else if( oldZone->IsOnCopperLayer() )
        dialogResult = InvokeCopperZonesEditor( m_frame, nullptr, &zoneSettings );
    else
        dialogResult = InvokeNonCopperZonesEditor( m_frame, &zoneSettings );

    if( dialogResult != wxID_OK )
        return 0;

    // duplicate the zone
    BOARD_COMMIT commit( m_frame );

    std::unique_ptr<ZONE> newZone = std::make_unique<ZONE>( *oldZone );
    newZone->ClearSelected();
    newZone->UnFill();
    zoneSettings.ExportSetting( *newZone );

    // If the new zone is on the same layer(s) as the initial zone,
    // offset it a bit so it can more easily be picked.
    if( oldZone->GetLayerSet() == zoneSettings.m_Layers )
        newZone->Move( VECTOR2I( pcbIUScale.IU_PER_MM, pcbIUScale.IU_PER_MM ) );

    commit.Add( newZone.release() );
    commit.Push( _( "Duplicate Zone" ) );

    return 0;
}


int BOARD_EDITOR_CONTROL::CrossProbeToSch( const TOOL_EVENT& aEvent )
{
    doCrossProbePcbToSch( aEvent, false );
    return 0;
}


int BOARD_EDITOR_CONTROL::ExplicitCrossProbeToSch( const TOOL_EVENT& aEvent )
{
    doCrossProbePcbToSch( aEvent, true );
    return 0;
}


void BOARD_EDITOR_CONTROL::doCrossProbePcbToSch( const TOOL_EVENT& aEvent, bool aForce )
{
    // Don't get in an infinite loop PCB -> SCH -> PCB -> SCH -> ...
    if( m_frame->m_probingSchToPcb )
        return;

    PCB_SELECTION_TOOL*  selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
    const PCB_SELECTION& selection = selTool->GetSelection();
    EDA_ITEM*            focusItem = nullptr;

    if( aEvent.Matches( EVENTS::PointSelectedEvent ) )
        focusItem = selection.GetLastAddedItem();

    m_frame->SendSelectItemsToSch( selection.GetItems(), focusItem, aForce );

    // Update 3D viewer highlighting
    m_frame->Update3DView( false, frame()->GetPcbNewSettings()->m_Display.m_Live3DRefresh );
}


int BOARD_EDITOR_CONTROL::AssignNetclass( const TOOL_EVENT& aEvent )
{
    PCB_SELECTION_TOOL*  selectionTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();

    const PCB_SELECTION& selection = selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, PCB_SELECTION_TOOL* sTool )
            {
                // Iterate from the back so we don't have to worry about removals.
                for( int i = aCollector.GetCount() - 1; i >= 0; --i )
                {
                    if( !dynamic_cast<BOARD_CONNECTED_ITEM*>( aCollector[ i ] ) )
                        aCollector.Remove( aCollector[ i ] );
                }

                sTool->FilterCollectorForLockedItems( aCollector );
            } );

    std::set<wxString> netNames;
    std::set<int>      netCodes;

    for( EDA_ITEM* item : selection )
    {
        const NETINFO_ITEM& net = *static_cast<BOARD_CONNECTED_ITEM*>( item )->GetNet();

        if( !net.HasAutoGeneratedNetname() )
        {
            netNames.insert( net.GetNetname() );
            netCodes.insert( net.GetNetCode() );
        }
    }

    if( netNames.empty() )
    {
        m_frame->ShowInfoBarError( _( "Selection contains no items with labeled nets." ) );
        return 0;
    }

    selectionTool->ClearSelection();
    for( const int& code : netCodes )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::selectNet, code );
    }
    canvas()->ForceRefresh();

    DIALOG_ASSIGN_NETCLASS dlg( m_frame, netNames, board()->GetNetClassAssignmentCandidates(),
            [this]( const std::vector<wxString>& aNetNames )
            {
                PCB_SELECTION_TOOL*  selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
                selTool->ClearSelection();

                for( const wxString& curr_netName : aNetNames )
                {
                    int curr_netCode = board()->GetNetInfo().GetNetItem( curr_netName )->GetNetCode();

                    if( curr_netCode > 0 )
                        selTool->SelectAllItemsOnNet( curr_netCode );
                }

                canvas()->ForceRefresh();
                m_frame->UpdateMsgPanel();
            } );

    if( dlg.ShowModal() == wxID_OK )
    {
        board()->SynchronizeNetsAndNetClasses( false );
        // Refresh UI that depends on netclasses, such as the properties panel
        m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    }

    return 0;
}


int BOARD_EDITOR_CONTROL::EditFpInFpEditor( const TOOL_EVENT& aEvent )
{
    PCB_SELECTION_TOOL*  selTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
    const PCB_SELECTION& selection = selTool->RequestSelection( EDIT_TOOL::FootprintFilter );

    if( selection.Empty() )
    {
        // Giant hack: by default we assign Edit Table to the same hotkey, so give the table
        // tool a chance to handle it if we can't.
        if( PCB_EDIT_TABLE_TOOL* tableTool = m_toolMgr->GetTool<PCB_EDIT_TABLE_TOOL>() )
            tableTool->EditTable( aEvent );

        return 0;
    }

    FOOTPRINT* fp = selection.FirstOfKind<FOOTPRINT>();

    if( !fp )
        return 0;

    PCB_BASE_EDIT_FRAME* editFrame = getEditFrame<PCB_BASE_EDIT_FRAME>();

    if( KIWAY_PLAYER* frame = editFrame->Kiway().Player( FRAME_FOOTPRINT_EDITOR, true ) )
    {
        FOOTPRINT_EDIT_FRAME* fp_editor = static_cast<FOOTPRINT_EDIT_FRAME*>( frame );

        if( aEvent.IsAction( &PCB_ACTIONS::editFpInFpEditor ) )
            fp_editor->LoadFootprintFromBoard( fp );
        else if( aEvent.IsAction( &PCB_ACTIONS::editLibFpInFpEditor ) )
            fp_editor->LoadFootprintFromLibrary( fp->GetFPID() );

        fp_editor->Show( true );
        fp_editor->Raise();        // Iconize( false );
    }

    if( selection.IsHover() )
        m_toolMgr->RunAction( ACTIONS::selectionClear );

    return 0;
}


void BOARD_EDITOR_CONTROL::DoSetDrillOrigin( KIGFX::VIEW* aView, PCB_BASE_FRAME* aFrame,
                                             EDA_ITEM* originViewItem, const VECTOR2D& aPosition )
{
    aFrame->GetDesignSettings().SetAuxOrigin( VECTOR2I( aPosition ) );
    originViewItem->SetPosition( aPosition );
    aView->MarkDirty();
    aFrame->OnModify();
}


int BOARD_EDITOR_CONTROL::DrillOrigin( const TOOL_EVENT& aEvent )
{
    if( aEvent.IsAction( &PCB_ACTIONS::drillResetOrigin ) )
    {
        m_frame->SaveCopyInUndoList( m_placeOrigin.get(), UNDO_REDO::GRIDORIGIN );
        DoSetDrillOrigin( getView(), m_frame, m_placeOrigin.get(), VECTOR2D( 0, 0 ) );
        return 0;
    }

    if( aEvent.IsAction( &PCB_ACTIONS::drillSetOrigin ) )
    {
        VECTOR2I origin = aEvent.Parameter<VECTOR2I>();
        m_frame->SaveCopyInUndoList( m_placeOrigin.get(), UNDO_REDO::GRIDORIGIN );
        DoSetDrillOrigin( getView(), m_frame, m_placeOrigin.get(), origin );
        return 0;
    }

    PCB_PICKER_TOOL* picker = m_toolMgr->GetTool<PCB_PICKER_TOOL>();

    // Deactivate other tools; particularly important if another PICKER is currently running
    Activate();

    picker->SetCursor( KICURSOR::PLACE );
    picker->ClearHandlers();

    picker->SetClickHandler(
            [this] ( const VECTOR2D& pt ) -> bool
            {
                m_frame->SaveCopyInUndoList( m_placeOrigin.get(), UNDO_REDO::DRILLORIGIN );
                DoSetDrillOrigin( getView(), m_frame, m_placeOrigin.get(), pt );
                return false;   // drill origin is a one-shot; don't continue with tool
            } );

    m_toolMgr->RunAction( ACTIONS::pickerTool, &aEvent );

    return 0;
}


void BOARD_EDITOR_CONTROL::setTransitions()
{
    Go( &BOARD_EDITOR_CONTROL::New,                    ACTIONS::doNew.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::Open,                   ACTIONS::open.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::Save,                   ACTIONS::save.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::SaveAs,                 ACTIONS::saveAs.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::SaveCopy,               ACTIONS::saveCopy.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::Revert,                 ACTIONS::revert.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::PageSettings,           ACTIONS::pageSettings.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::Plot,                   ACTIONS::plot.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::Search,                 ACTIONS::showSearch.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::Find,                   ACTIONS::find.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::FindNext,               ACTIONS::findNext.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::FindNext,               ACTIONS::findPrevious.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::OpenNonKicadBoard,      PCB_ACTIONS::openNonKicadBoard.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportFootprints,       PCB_ACTIONS::exportFootprints.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::BoardSetup,             PCB_ACTIONS::boardSetup.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ImportNetlist,          PCB_ACTIONS::importNetlist.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ImportSpecctraSession,  PCB_ACTIONS::importSpecctraSession.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::CloudAutoroute,         PCB_ACTIONS::cloudAutoroute.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportSpecctraDSN,      PCB_ACTIONS::exportSpecctraDSN.MakeEvent() );

    if( ADVANCED_CFG::GetCfg().m_ShowPcbnewExportNetlist && m_frame && m_frame->GetExportNetlistAction() )
        Go( &BOARD_EDITOR_CONTROL::ExportNetlist, m_frame->GetExportNetlistAction()->MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::GenerateDrillFiles,     PCB_ACTIONS::generateDrillFiles.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenerateGerbers,        PCB_ACTIONS::generateGerbers.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GeneratePosFile,        PCB_ACTIONS::generatePosFile.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenFootprintsReport,    PCB_ACTIONS::generateReportFile.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenD356File,            PCB_ACTIONS::generateD356File.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenBOMFileFromBoard,    PCB_ACTIONS::generateBOM.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenIPC2581File,         PCB_ACTIONS::generateIPC2581File.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::GenerateODBPPFiles,     PCB_ACTIONS::generateODBPPFile.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::ExportGenCAD,           PCB_ACTIONS::exportGenCAD.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportVRML,             PCB_ACTIONS::exportVRML.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportIDF,              PCB_ACTIONS::exportIDF.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportSTEP,             PCB_ACTIONS::exportSTEP.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportCmpFile,          PCB_ACTIONS::exportCmpFile.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ExportHyperlynx,        PCB_ACTIONS::exportHyperlynx.MakeEvent() );

    // Track & via size control
    Go( &BOARD_EDITOR_CONTROL::TrackWidthInc,          PCB_ACTIONS::trackWidthInc.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::TrackWidthDec,          PCB_ACTIONS::trackWidthDec.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ViaSizeInc,             PCB_ACTIONS::viaSizeInc.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ViaSizeDec,             PCB_ACTIONS::viaSizeDec.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::AutoTrackWidth,         PCB_ACTIONS::autoTrackWidth.MakeEvent() );

    // Zone actions
    Go( &BOARD_EDITOR_CONTROL::ZoneMerge,              PCB_ACTIONS::zoneMerge.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ZoneDuplicate,          PCB_ACTIONS::zoneDuplicate.MakeEvent() );

    // Placing tools
    Go( &BOARD_EDITOR_CONTROL::PlaceFootprint,         PCB_ACTIONS::placeFootprint.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::DrillOrigin,            PCB_ACTIONS::drillOrigin.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::DrillOrigin,            PCB_ACTIONS::drillResetOrigin.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::DrillOrigin,            PCB_ACTIONS::drillSetOrigin.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::EditFpInFpEditor,       PCB_ACTIONS::editFpInFpEditor.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::EditFpInFpEditor,       PCB_ACTIONS::editLibFpInFpEditor.MakeEvent() );

    // Cross-select
    Go( &BOARD_EDITOR_CONTROL::CrossProbeToSch,        EVENTS::PointSelectedEvent );
    Go( &BOARD_EDITOR_CONTROL::CrossProbeToSch,        EVENTS::SelectedEvent );
    Go( &BOARD_EDITOR_CONTROL::CrossProbeToSch,        EVENTS::UnselectedEvent );
    Go( &BOARD_EDITOR_CONTROL::CrossProbeToSch,        EVENTS::ClearedEvent );
    Go( &BOARD_EDITOR_CONTROL::ExplicitCrossProbeToSch, PCB_ACTIONS::selectOnSchematic.MakeEvent() );

    // Other
    Go( &BOARD_EDITOR_CONTROL::ToggleLockSelected,     PCB_ACTIONS::toggleLock.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::LockSelected,           PCB_ACTIONS::lock.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::UnlockSelected,         PCB_ACTIONS::unlock.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::AssignNetclass,         PCB_ACTIONS::assignNetClass.MakeEvent() );

    Go( &BOARD_EDITOR_CONTROL::UpdatePCBFromSchematic, ACTIONS::updatePcbFromSchematic.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::UpdateSchematicFromPCB, ACTIONS::updateSchematicFromPcb.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ShowEeschema,           PCB_ACTIONS::showEeschema.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleLayersManager,    PCB_ACTIONS::showLayersManager.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleProperties,       ACTIONS::showProperties.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleNetInspector,     PCB_ACTIONS::showNetInspector.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleAIChat,           PCB_ACTIONS::showAIChat.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleLibraryTree,      PCB_ACTIONS::showDesignBlockPanel.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ToggleSearch,           PCB_ACTIONS::showSearch.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::TogglePythonConsole,    PCB_ACTIONS::showPythonConsole.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::RepairBoard,            PCB_ACTIONS::repairBoard.MakeEvent() );
    // Line modes: explicit, next, and notification
    Go( &BOARD_EDITOR_CONTROL::ChangeLineMode,        PCB_ACTIONS::lineModeFree.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ChangeLineMode,        PCB_ACTIONS::lineMode90.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::ChangeLineMode,        PCB_ACTIONS::lineMode45.MakeEvent() );
    Go( &BOARD_EDITOR_CONTROL::OnAngleSnapModeChanged,PCB_ACTIONS::angleSnapModeChanged.MakeEvent() );
}
