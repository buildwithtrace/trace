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

#include "ai_backend_client.h"
#include <amplitude_client.h>
#include "ai_tool_executor.h"
#include <widgets/chat_webview_panel.h>
#include <python_manager.h>
#include <paths.h>
#include <pgm_base.h>
#include <config.h>
#include <env_vars.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>

#ifdef _WIN32
#include <process_executor.h>
#endif

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>
#include <wx/log.h>

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <array>


namespace
{
// Context for streaming callbacks
struct StreamContext
{
    AI_BACKEND_CLIENT*                                 client;
    std::string                                        buffer;
    std::function<void( const std::string& )>          lineCallback;
    std::atomic<bool>*                                 stopRequested;
};


size_t stream_write_callback( void* contents, size_t size, size_t nmemb, void* userp )
{
    size_t         realsize = size * nmemb;
    StreamContext* ctx = static_cast<StreamContext*>( userp );

    if( !ctx || ( ctx->stopRequested && ctx->stopRequested->load() ) )
        return 0; // Signal to abort

    ctx->buffer.append( static_cast<const char*>( contents ), realsize );

    // Process complete lines (SSE format: "data: {...}\n\n")
    size_t pos;
    while( ( pos = ctx->buffer.find( "\n\n" ) ) != std::string::npos )
    {
        std::string eventBlock = ctx->buffer.substr( 0, pos );
        ctx->buffer.erase( 0, pos + 2 );

        // Process each line in the event block
        std::istringstream stream( eventBlock );
        std::string        line;
        while( std::getline( stream, line ) )
        {
            if( !line.empty() && ctx->lineCallback )
                ctx->lineCallback( line );
        }
    }

    return realsize;
}


} // namespace


AI_BACKEND_CLIENT::AI_BACKEND_CLIENT( const std::string& aBackendUrl )
    : m_backendUrl( aBackendUrl ),
      m_toolExecutor( nullptr ),
      m_isStreaming( false ),
      m_stopRequested( false )
{
}


AI_BACKEND_CLIENT::~AI_BACKEND_CLIENT()
{
    StopStream();
}


void AI_BACKEND_CLIENT::StopStream()
{
    m_stopRequested.store( true );
}


// Helper function to collect library paths from environment variables
static wxString GetLibraryPaths( const wxString& aEnvVarBaseName )
{
    wxString paths;
    
    // Get paths from versioned environment variable
    if( Pgm().IsGUI() )
    {
        const ENV_VAR_MAP& envVars = Pgm().GetLocalEnvVariables();
        std::optional<wxString> envValue = ENV_VAR::GetVersionedEnvVarValue( envVars, aEnvVarBaseName );
        
        if( envValue && !envValue->IsEmpty() )
        {
            paths = *envValue;
        }
    }
    
    // Also check direct environment variable (for standalone use)
    if( paths.IsEmpty() )
    {
        wxString envValue;
        wxString envVarName = ENV_VAR::GetVersionedEnvVarName( aEnvVarBaseName );
        if( wxGetEnv( envVarName, &envValue ) && !envValue.IsEmpty() )
        {
            paths = envValue;
        }
    }
    
    // Return paths as-is (they may contain multiple paths separated by colons/semicolons)
    // Python scripts will handle both colon and semicolon separators
    if( !paths.IsEmpty() )
    {
        // Only normalize Windows-style semicolons to colons for consistency
        // (Python scripts handle both, but colon is standard for Unix)
        #ifdef __WXMSW__
        wxString normalized = paths;
        normalized.Replace( wxT( ";" ), wxT( ":" ), true );
        return normalized;
        #else
        return paths;
        #endif
    }
    
    return wxEmptyString;
}


std::pair<bool, std::string> AI_BACKEND_CLIENT::syncKicadToTrace( const std::string& aKicadFilePath,
                                                                   const std::string& aTraceFilePath,
                                                                   const std::string& aAppType )
{
    if( !wxFileExists( aKicadFilePath ) )
        return { false, "KiCad file not found: " + aKicadFilePath };

    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    if( pythonPath.IsEmpty() )
        return { false, "Could not find Python interpreter" };

    // Determine which converter to use
    std::string subdir = ( aAppType == "pcbnew" ) ? "pcbnew" : "eeschema";
    std::string fromFormat = ( aAppType == "pcbnew" ) ? "kicad_pcb" : "kicad_sch";
    std::string toFormat = ( aAppType == "pcbnew" ) ? "trace_pcb" : "trace_sch";

    // Find trace.py script - try multiple locations
    wxFileName traceScript;
    bool scriptFound = false;

    // Try environment variable first
    wxString envTraceDir;
    if( wxGetEnv( wxT( "KICAD_TRACE_DIR" ), &envTraceDir ) && !envTraceDir.IsEmpty() )
    {
        wxFileName envScript( envTraceDir + "/" + subdir + "/trace.py" );
        if( envScript.FileExists() )
        {
            traceScript = envScript;
            scriptFound = true;
        }
    }

    // Try inside app bundle: Trace.app/Contents/SharedSupport/scripting/trace/{subdir}/trace.py
    if( !scriptFound )
    {
        wxFileName exePath( Pgm().GetExecutablePath() );
        wxFileName bundlePath( exePath );
        bundlePath.AppendDir( wxS( "Contents" ) );
        bundlePath.AppendDir( wxS( "SharedSupport" ) );
        bundlePath.AppendDir( wxS( "scripting" ) );
        bundlePath.AppendDir( wxS( "trace" ) );
        bundlePath.AppendDir( wxString( subdir ) );
        bundlePath.SetFullName( wxS( "trace.py" ) );

        if( bundlePath.FileExists() )
        {
            traceScript = bundlePath;
            scriptFound = true;
        }
    }

    // Try build-time configured path
    if( !scriptFound )
    {
        wxString configuredDir( KICAD_TRACE_DIR, wxConvUTF8 );
        if( !configuredDir.IsEmpty() )
        {
            wxFileName configScript( configuredDir + "/" + subdir + "/trace.py" );
            if( configScript.IsAbsolute() && configScript.FileExists() )
            {
                traceScript = configScript;
                scriptFound = true;
            }
            else
            {
                wxString stockDataPath = PATHS::GetStockDataPath();
                if( !stockDataPath.IsEmpty() )
                {
                    wxString fullScriptPath = stockDataPath + wxString::Format( "/scripting/trace/%s/trace.py", subdir );
                    wxFileName resolvedScript( fullScriptPath );
                    if( resolvedScript.FileExists() )
                    {
                        traceScript = resolvedScript;
                        scriptFound = true;
                    }
                }
            }
        }
    }

    // Try relative to executable
    if( !scriptFound )
    {
        wxFileName exePath( Pgm().GetExecutablePath() );
        exePath.RemoveLastDir();
        wxFileName tracePath( exePath );
        tracePath.AppendDir( wxS( "trace" ) );
        tracePath.AppendDir( wxString( subdir ) );
        tracePath.SetFullName( wxS( "trace.py" ) );
        if( tracePath.FileExists() )
        {
            traceScript = tracePath;
            scriptFound = true;
        }
    }

    // Try going up one more level
    if( !scriptFound )
    {
        wxFileName exePath( Pgm().GetExecutablePath() );
        exePath.RemoveLastDir();
        if( exePath.GetDirCount() > 0 )
        {
            exePath.RemoveLastDir();
            wxFileName tracePath( exePath );
            tracePath.AppendDir( wxS( "trace" ) );
            tracePath.AppendDir( wxString( subdir ) );
            tracePath.SetFullName( wxS( "trace.py" ) );
            if( tracePath.FileExists() )
            {
                traceScript = tracePath;
                scriptFound = true;
            }
        }
    }

    if( !scriptFound )
        return { false, "Could not find trace.py script" };

    // Collect and pass library paths
    wxString symbolPathsFlag;
    wxString footprintPathsFlag;
    
    wxString symbolPaths = GetLibraryPaths( wxS( "SYMBOL_DIR" ) );
    if( !symbolPaths.IsEmpty() )
    {
        symbolPathsFlag = wxString::Format( wxT( " --symbol-paths \"%s\"" ), symbolPaths );
    }
    
    wxString footprintPaths = GetLibraryPaths( wxS( "FOOTPRINT_DIR" ) );
    if( !footprintPaths.IsEmpty() )
    {
        footprintPathsFlag = wxString::Format( wxT( " --footprint-paths \"%s\"" ), footprintPaths );
    }

    // Build command - use popen for thread safety (wxExecute crashes on non-main thread on macOS)
#ifdef _WIN32
    // Windows: Build command without cmd.exe wrapper to avoid window flash
    wxString pythonCmd = wxString::Format( 
        wxT( "\"%s\" \"%s\" \"%s\" \"%s\" -f %s -t %s" ),
        pythonPath,
        traceScript.GetFullPath(),
        aKicadFilePath,
        aTraceFilePath,
        fromFormat,
        toFormat );

    // Execute without visible window using Windows API
    PROCESS_RESULT result = ExecuteProcessSilent( pythonCmd.ToStdWstring() );
    
    if( !result.success )
    {
        return { false, "Failed to execute conversion command" };
    }
    
    std::string output = result.output;
    int exitCode = result.exitCode;
    
#else
    // Unix/macOS: Use 2>&1 to capture stderr
    wxString command = wxString::Format( 
        wxT( "\"%s\" \"%s\" \"%s\" \"%s\" -f %s -t %s%s%s 2>&1" ),
        pythonPath,
        traceScript.GetFullPath(),
        aKicadFilePath,
        aTraceFilePath,
        fromFormat,
        toFormat,
        symbolPathsFlag,
        footprintPathsFlag );

    // Execute using popen (thread-safe)
    std::string output;
    FILE* pipe = popen( command.ToStdString().c_str(), "r" );
    if( !pipe )
    {
        return { false, "Failed to execute conversion command" };
    }

    std::array<char, 256> buffer;
    while( fgets( buffer.data(), buffer.size(), pipe ) != nullptr )
    {
        output += buffer.data();
    }

    int exitCode = pclose( pipe );
#endif

    if( exitCode != 0 )
    {
        return { false, "Conversion failed: " + output };
    }

    // Verify output file exists
    if( !wxFileExists( aTraceFilePath ) )
    {
        return { false, "Conversion completed but trace file was not created" };
    }

    return { true, "" };
}


std::optional<AI_BACKEND_EVENT> AI_BACKEND_CLIENT::parseSSEEvent( const std::string& aLine )
{
    // SSE format: "data: {json}"
    if( aLine.substr( 0, 6 ) != "data: " )
    {
        return std::nullopt;
    }

    std::string jsonStr = aLine.substr( 6 );

    try
    {
        nlohmann::json json = nlohmann::json::parse( jsonStr );
        AI_BACKEND_EVENT event;

        std::string eventType = json.value( "type", "" );

        if( eventType == "text_delta" )
        {
            event.type = AI_EVENT_TYPE::TEXT_DELTA;
            event.content = json.value( "content", "" );
            if( event.content.empty() )
                return std::nullopt;  // Skip empty text deltas
            if( json.contains( "conversation_id" ) && json["conversation_id"].is_string() )
                event.conversationId = json["conversation_id"].get<std::string>();
        }
        else if( eventType == "status" )
        {
            event.type = AI_EVENT_TYPE::STATUS;
            event.content = json.value( "content", "" );
        }
        else if( eventType == "quota_warning" )
        {
            event.type = AI_EVENT_TYPE::QUOTA_WARNING;
            event.content = json.value( "content", "" );
            if( json.contains( "data" ) && json["data"].is_object() )
            {
                event.data = json["data"];
            }
        }
        else if( eventType == "title_update" )
        {
            event.type = AI_EVENT_TYPE::TITLE_UPDATE;
            event.content = json.value( "content", "" );
        }
        else if( eventType == "mode_transition" )
        {
            event.type = AI_EVENT_TYPE::MODE_TRANSITION;
            event.content = json.value( "content", "" );
            event.fromMode = json.value( "from_mode", "" );
            event.toMode = json.value( "to_mode", "" );
            event.transitionReason = json.value( "reason", "" );

            AMPLITUDE_CLIENT::Instance().Track( "ai_mode_transition", {
                { "from_mode", event.fromMode },
                { "to_mode", event.toMode },
                { "reason", event.transitionReason },
            } );
        }
        else if( eventType == "phase_update" )
        {
            event.type = AI_EVENT_TYPE::PHASE_UPDATE;
            event.content = json.value( "content", "" );
            event.data = json;
        }
        else if( eventType == "plan_document" )
        {
            event.type = AI_EVENT_TYPE::PLAN_DOCUMENT;
            event.content = json.value( "content", "" );
            event.data = json;
        }
        else if( eventType == "interrupt" )
        {
            event.type = AI_EVENT_TYPE::INTERRUPT;
            event.content = json.value( "content", "" );
            event.data = json;
        }
        else if( eventType == "thinking" )
        {
            event.type = AI_EVENT_TYPE::THINKING;
            event.content = json.value( "content", "" );
            if( json.contains( "agent_id" ) && json["agent_id"].is_string() )
                event.agentId = wxString::FromUTF8( json["agent_id"].get<std::string>() );
        }
        else if( eventType == "tool_call" )
        {
            event.type = AI_EVENT_TYPE::TOOL_CALL;
            event.toolName = json.value( "tool_name", "" );
            event.toolCallId = json.value( "tool_call_id", "" );
            if( json.contains( "tool_args" ) )
                event.toolArgs = json["tool_args"];
            event.content = json.value( "content", "" );
        }
        else if( eventType == "file_edit" )
        {
            event.type = AI_EVENT_TYPE::FILE_EDIT;
            event.fileModified = json.value( "success", false );
            event.content = json.value( "message", "" );
        }
        else if( eventType == "progress" )
        {
            event.type = AI_EVENT_TYPE::PROGRESS;
            event.data = json;
        }
        else if( eventType == "metrics" )
        {
            event.type = AI_EVENT_TYPE::METRICS;
            event.data = json;
        }
        else if( eventType == "error" )
        {
            event.type = AI_EVENT_TYPE::EVENT_ERROR;
            event.error = json.value( "error", json.value( "content", "Unknown error" ) );
        }
        else if( eventType == "auth_error" )
        {
            event.type = AI_EVENT_TYPE::AUTH_ERROR;
            event.error = json.value( "error", json.value( "content", "Authentication failed" ) );
        }
        else if( eventType == "done" )
        {
            event.type = AI_EVENT_TYPE::DONE;
            event.content = json.value( "response", "" );
            event.fileModified = json.value( "file_modified", false );
            if( json.contains( "conversation_id" ) && json["conversation_id"].is_string() )
                event.conversationId = json["conversation_id"].get<std::string>();
            if( json.contains( "version_id" ) && json["version_id"].is_string() )
                event.versionId = json["version_id"].get<std::string>();
            if( json.contains( "error" ) && json["error"].is_string() )
                event.error = json["error"].get<std::string>();
        }
        else if( eventType == "versions_list" )
        {
            event.type = AI_EVENT_TYPE::VERSIONS_LIST;
            event.data = json;
        }
        else if( eventType == "version_saved" )
        {
            event.type = AI_EVENT_TYPE::VERSION_SAVED;
            if( json.contains( "version_id" ) && json["version_id"].is_string() )
                event.versionId = json["version_id"].get<std::string>();
        }
        else if( eventType == "version_restored" )
        {
            event.type = AI_EVENT_TYPE::VERSION_RESTORED;
            event.fileModified = json.value( "success", false );
        }
        else
        {
            // Unknown event type - skip
            return std::nullopt;
        }

        // Multi-agent: extract agent_id if present
        if( json.contains( "agent_id" ) && json["agent_id"].is_string() )
            event.agentId = wxString::FromUTF8( json["agent_id"].get<std::string>() );

        return event;
    }
    catch( const std::exception& )
    {
        return std::nullopt;
    }
}


bool AI_BACKEND_CLIENT::processEvent( AI_BACKEND_EVENT&  aEvent,
                                      const std::string& aFilePath,
                                      const std::string& aKicadFilePath,
                                      const std::string& aSessionId,
                                      const std::string& aAuthToken )
{
    bool fileModified = false;

    if( aEvent.type == AI_EVENT_TYPE::TOOL_CALL && m_toolExecutor )
    {
        // Fire the UI callback immediately so the status line updates
        if( m_eventCallback )
        {
            m_eventCallback( aEvent );
        }

        // Dispatch tool execution to a background thread
        std::string toolName = aEvent.toolName;
        nlohmann::json toolArgs = aEvent.toolArgs;
        std::string toolCallId = aEvent.toolCallId;
        AI_TOOL_EXECUTOR* executor = m_toolExecutor;
        auto eventCb = m_eventCallback;

        auto fut = std::async( std::launch::async,
            [this, executor, toolName, toolArgs, toolCallId, aFilePath, aKicadFilePath,
             aSessionId, aAuthToken, eventCb]() -> bool
            {
                AI_TOOL_RESULT result = executor->ExecuteTool(
                    toolName, toolArgs, aFilePath, aKicadFilePath );

                if( !toolCallId.empty() )
                {
                    std::string resultMessage = result.result;
                    if( !result.conversionLogs.empty() )
                    {
                        resultMessage += "\n\n=== Conversion Logs ===\n" + result.conversionLogs;
                    }
        
            SubmitToolResult( aSessionId, toolCallId, resultMessage, aAuthToken );
                }

                if( result.fileModified )
                    m_anyToolModifiedFile.store( true );

                if( eventCb )
                {
                    AI_BACKEND_EVENT completionEvent;
                    completionEvent.toolName = toolName;
                    completionEvent.toolCallId = toolCallId;

                    if( toolName == "search_replace" || toolName == "write" )
                    {
                        completionEvent.type = AI_EVENT_TYPE::FILE_EDIT;
                        completionEvent.fileModified = result.fileModified;
                        completionEvent.content = result.result;
                        completionEvent.toolArgs = toolArgs;
                        completionEvent.diffInfo = result.diffInfo;
                        completionEvent.hasDiffInfo = result.hasDiffInfo;
                        completionEvent.diffType = result.hasDiffInfo && result.diffInfo.isSimple
                                                    ? "incremental" : "full_reload";
                    }
                    else if( toolName == "todo_write" || toolName == "todo_read" )
                    {
                        completionEvent.type = AI_EVENT_TYPE::TODO_UPDATE;
                        completionEvent.content = result.result;
                    }
                    else
                    {
                        completionEvent.type = AI_EVENT_TYPE::TOOL_CALL;
                        completionEvent.content = result.result;
                    }
                    eventCb( completionEvent );
                }

                return result.fileModified;
            });

        {
            std::lock_guard<std::mutex> lock( m_pendingToolsMutex );
            m_pendingToolFutures.push_back( std::move( fut ) );
        }

        return false;
    }

    // For non-tool events, emit callback directly
    if( m_eventCallback )
    {
        m_eventCallback( aEvent );
    }

    return fileModified;
}


void AI_BACKEND_CLIENT::WaitForPendingTools()
{
    std::lock_guard<std::mutex> lock( m_pendingToolsMutex );
    for( auto& fut : m_pendingToolFutures )
    {
        if( fut.valid() )
            fut.get();
    }
    m_pendingToolFutures.clear();
}


AI_STREAM_RESULT AI_BACKEND_CLIENT::StreamChat( const std::string& aMessage,
                                                 const std::string& aFilePath,
                                                 const std::string& aKicadFilePath,
                                                 const std::string& aSessionId,
                                                 const std::string& aConversationId,
                                                 const std::string& aMode,
                                                 const std::string& aAppType,
                                                 const std::string& aAuthToken,
                                                 const std::string& aRefreshToken,
                                                 const std::vector<ChatAttachment>& aAttachments )
{
    AI_STREAM_RESULT result;
    result.status = "success";

    m_isStreaming.store( true );
    m_stopRequested.store( false );

    // Build request payload
    nlohmann::json payload;
    payload["message"] = aMessage;
    payload["session_id"] = aSessionId;
    payload["app_type"] = aAppType;
    payload["mode"] = aMode;

    if( !aConversationId.empty() )
        payload["conversation_id"] = aConversationId;

    if( !m_editMessageId.empty() )
    {
        payload["edit_message_id"] = m_editMessageId;
        m_editMessageId.clear();
    }

    // Set conversation ID on tool executor if we already know it
    if( !aConversationId.empty() && m_toolExecutor )
        m_toolExecutor->SetConversationId( aConversationId );

    // Add attachments if present
    if( !aAttachments.empty() )
    {
        nlohmann::json attachmentsJson = nlohmann::json::array();
        for( const auto& att : aAttachments )
        {
            attachmentsJson.push_back({
                {"name", att.name.ToStdString()},
                {"type", att.mimeType.ToStdString()},
                {"data", att.base64Data.ToStdString()},
                {"size", att.size}
            });
        }
        payload["attachments"] = attachmentsJson;
    }

    if( !aFilePath.empty() )
    {
        payload["file_path"] = aFilePath;
        
        // Add project directory for multisheet support
        wxFileName filePath( aFilePath );
        payload["project_dir"] = filePath.GetPath().ToStdString();

        // Check if trace file exists and has content, if not, convert from kicad file
        bool traceFileValid = false;
        if( wxFileExists( aFilePath ) )
        {
            std::ifstream checkFile( aFilePath );
            if( checkFile.is_open() )
            {
                checkFile.seekg( 0, std::ios::end );
                traceFileValid = checkFile.tellg() > 0;
                checkFile.close();
            }
        }

        if( !traceFileValid && !aKicadFilePath.empty() )
        {
            auto [success, errorMsg] = syncKicadToTrace( aKicadFilePath, aFilePath, aAppType );
            if( !success )
            {
                wxLogWarning( wxT( "AI_BACKEND_CLIENT: Failed to convert KiCad to trace: %s" ),
                             wxString::FromUTF8( errorMsg ) );
            }
        }

        // Read file content for context
        std::ifstream file( aFilePath );
        if( file.is_open() )
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            // Count lines
            int lineCount = 0;
            for( char c : content )
            {
                if( c == '\n' )
                    lineCount++;
            }
            payload["total_lines"] = lineCount;

            // Send trace file content directly
            if( aAppType == "pcbnew" )
                payload["pcb_content"] = content;
            else
                payload["schematic_content"] = content;
        }
    }

    std::string url = m_backendUrl + "/chat/stream";
    std::string body = payload.dump();

    // Setup curl
    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    
    // Set Authorization header (secure - not in request body)
    if( !aAuthToken.empty() )
    {
        curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    }

    // Set timeouts - use idle detection instead of absolute timeout for SSE streams
    // CURLOPT_TIMEOUT would kill the connection after N seconds regardless of data flow,
    // which breaks long-running SSE streams. Instead, detect stalled connections:
    // abort if fewer than 1 byte/sec is received for 120 seconds straight.
    // The backend sends heartbeats every 15s, so this only triggers on true disconnects.
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 0L );              // no absolute timeout
    curl_easy_setopt( curl.GetCurl(), CURLOPT_CONNECTTIMEOUT, 120L );     // 2 min connect
    curl_easy_setopt( curl.GetCurl(), CURLOPT_LOW_SPEED_LIMIT, 1L );     // 1 byte/sec minimum
    curl_easy_setopt( curl.GetCurl(), CURLOPT_LOW_SPEED_TIME, 120L );    // for 120 seconds

    // Setup streaming callback
    StreamContext ctx;
    ctx.client = this;
    ctx.stopRequested = &m_stopRequested;
    ctx.lineCallback = [&]( const std::string& line )
    {
        if( m_stopRequested.load() )
        {
            return;
        }

        auto eventOpt = parseSSEEvent( line );
        if( !eventOpt )
        {
            return;
        }

        AI_BACKEND_EVENT event = *eventOpt;
        result.eventCount++;  // Track event count for debugging

        // Accumulate text deltas
        if( event.type == AI_EVENT_TYPE::TEXT_DELTA )
        {
            result.response += event.content;

            // Pass conversation ID to tool executor as soon as we receive it
            if( !event.conversationId.empty() && m_toolExecutor )
            {
                if( result.conversationId.empty() )
                    result.conversationId = event.conversationId;
                m_toolExecutor->SetConversationId( event.conversationId );
            }
        }

        // Process event (may execute tools)
        bool modified = processEvent( event, aFilePath, aKicadFilePath, aSessionId,
                                       aAuthToken );
        if( modified )
        {
            result.fileModified = true;
            

        }

        // Handle done event
        if( event.type == AI_EVENT_TYPE::DONE )
        {
            if( !event.conversationId.empty() )
                result.conversationId = event.conversationId;
            if( event.fileModified )
                result.fileModified = true;
            if( !event.error.empty() )
            {
                result.status = "error";
                result.error = event.error;
            }
        }

        // Handle errors
        if( event.type == AI_EVENT_TYPE::EVENT_ERROR )
        {
            result.status = "error";
            result.error = event.error;
        }
        else if( event.type == AI_EVENT_TYPE::AUTH_ERROR )
        {
            result.status = "auth_error";
            result.error = event.error;
        }
    };

    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEFUNCTION, stream_write_callback );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEDATA, &ctx );

    // Set transfer callback to check for cancellation frequently
    // This allows curl operations to be properly cancelled when stopRequested is true
    curl.SetTransferCallback( [this]( size_t, size_t, size_t, size_t ) -> int
    {
        // Return non-zero to abort curl operation if stopRequested is set
        return m_stopRequested.load() ? 1 : 0;
    }, 100000L ); // Check every 100ms (100000 microseconds)

    // Perform request
    int curlResult = curl.Perform();

    WaitForPendingTools();
    if( m_anyToolModifiedFile.exchange( false ) )
        result.fileModified = true;

    m_isStreaming.store( false );

    if( m_stopRequested.load() )
    {
        result.status = "stopped";
        return result;
    }

    // Always check HTTP status code - even when curl succeeds, we may have HTTP errors (401, 402, 403)
    int httpCode = curl.GetResponseStatusCode();
    
    // Try to parse error message from JSON response body for error responses
    std::string errorMessage;
    if( httpCode >= 400 || curlResult != CURLE_OK )
    {
        try
        {
            std::string responseBody = curl.GetBuffer();
            if( !responseBody.empty() )
            {
                nlohmann::json errorJson = nlohmann::json::parse( responseBody );
                if( errorJson.contains( "detail" ) )
                {
                    if( errorJson["detail"].is_object() && errorJson["detail"].contains( "message" )
                        && errorJson["detail"]["message"].is_string() )
                        errorMessage = errorJson["detail"]["message"].get<std::string>();
                    else if( errorJson["detail"].is_string() )
                        errorMessage = errorJson["detail"].get<std::string>();
                }
            }
        }
        catch( ... )
        {
            // Ignore JSON parse errors, use default messages
        }
    }
    
    // Handle HTTP error status codes (these may occur even when curlResult is OK)
    if( httpCode == 401 )
    {
        result.status = "auth_error";
        result.error = errorMessage.empty() ? "Authentication failed. Token may have expired." : errorMessage;
    }
    else if( httpCode == 402 )
    {
        result.status = "quota_exceeded";
        result.error = errorMessage.empty() ? "You've reached your plan limit. Upgrade your plan to continue." : errorMessage;
        AMPLITUDE_CLIENT::Instance().Track( "quota_exceeded", {
            { "app_type", aAppType },
        } );
    }
    else if( httpCode == 403 )
    {
        result.status = "plan_restricted";
        result.error = errorMessage.empty() ? "This feature requires a paid plan. Upgrade to access." : errorMessage;
        AMPLITUDE_CLIENT::Instance().Track( "plan_restricted", {
            { "app_type", aAppType },
        } );
    }
    else if( curlResult != CURLE_OK )
    {
        result.status = "error";
        result.error = errorMessage.empty() ? "HTTP request failed: " + curl.GetErrorText( curlResult ) : errorMessage;
    }
    else if( httpCode >= 400 )
    {
        // Other HTTP errors (500, etc.)
        result.status = "error";
        result.error = errorMessage.empty() ? "Server error: HTTP " + std::to_string( httpCode ) : errorMessage;
    }

    AMPLITUDE_CLIENT::Instance().Track( "ai_stream_completed", {
        { "status", result.status },
        { "error_type", result.status != "success" ? result.status : "" },
        { "error_message", result.error },
        { "app_type", aAppType },
        { "mode", aMode },
    } );

    return result;
}


bool AI_BACKEND_CLIENT::SubmitToolResult( const std::string& aSessionId,
                                          const std::string& aToolCallId,
                                          const std::string& aResult,
                                          const std::string& aAuthToken )
{
    nlohmann::json payload;
    payload["session_id"] = aSessionId;
    payload["tool_call_id"] = aToolCallId;
    payload["result"] = aResult;

    std::string url = m_backendUrl + "/tools/result";
    std::string body = payload.dump();

    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    
    if( !aAuthToken.empty() )
        curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 30L );

    int result = curl.Perform();
    if( result != CURLE_OK )
    {
        return false;
    }

    return true;
}


std::string AI_BACKEND_CLIENT::SaveSchematicVersion( const std::string& aFilePath,
                                                      const std::string& aDescription,
                                                      const std::string& aConversationId,
                                                      const std::string& aAuthToken,
                                                      const std::string& aSchematicContent )
{
    if( aAuthToken.empty() )
    {
        return "";
    }

    std::string content = aSchematicContent;
    if( content.empty() )
    {
        std::ifstream file( aFilePath );
        if( !file.is_open() )
        {
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        content = buffer.str();
    }

    if( content.empty() )
    {
        return "";
    }

    nlohmann::json payload;
    payload["project_file_path"] = aFilePath;
    payload["schematic_content"] = content;
    payload["description"] = aDescription;

    if( !aConversationId.empty() )
        payload["conversation_id"] = aConversationId;

    std::string url = m_backendUrl + "/schematic/version";
    std::string body = payload.dump();

    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 30L );

    int result = curl.Perform();
    if( result != CURLE_OK )
    {
        return "";
    }

    try
    {
        nlohmann::json response = nlohmann::json::parse( curl.GetBuffer() );
        std::string    versionId = response.value( "version_id", "" );

        if( !versionId.empty() )
        {
            AMPLITUDE_CLIENT::Instance().Track( "schematic_version_saved", {
                { "version_id", versionId },
            } );
        }

        return versionId;
    }
    catch( const std::exception& )
    {
        return "";
    }
}


nlohmann::json AI_BACKEND_CLIENT::GetSchematicVersions( const std::string& aFilePath,
                                                         const std::string& aAuthToken,
                                                         int                aLimit )
{
    if( aAuthToken.empty() )
        return nlohmann::json::array();

    nlohmann::json payload;
    payload["project_file_path"] = aFilePath;
    payload["limit"] = aLimit;

    std::string url = m_backendUrl + "/schematic/versions";
    std::string body = payload.dump();

    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 30L );

    int result = curl.Perform();
    if( result != CURLE_OK )
    {
        return nlohmann::json::array();
    }

    try
    {
        nlohmann::json response = nlohmann::json::parse( curl.GetBuffer() );
        return response.value( "versions", nlohmann::json::array() );
    }
    catch( const std::exception& )
    {
        return nlohmann::json::array();
    }
}


bool AI_BACKEND_CLIENT::RestoreSchematicVersion( const std::string& aVersionId,
                                                  const std::string& aFilePath,
                                                  const std::string& aAuthToken )
{
    if( aAuthToken.empty() )
    {
        return false;
    }

    // No body needed - version_id is in URL, auth in header
    std::string url = m_backendUrl + "/schematic/restore/" + aVersionId;
    std::string body = "{}";

    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 30L );

    int result = curl.Perform();
    if( result != CURLE_OK )
    {
        return false;
    }

    try
    {
        nlohmann::json response = nlohmann::json::parse( curl.GetBuffer() );
        std::string    content = response.value( "schematic_content", "" );

        if( content.empty() )
        {
            return false;
        }

        // Write content to file
        std::ofstream file( aFilePath );
        if( !file.is_open() )
        {
            return false;
        }

        file << content;
        file.close();

        AMPLITUDE_CLIENT::Instance().Track( "schematic_version_restored", {
            { "version_id", aVersionId },
        } );

        return true;
    }
    catch( const std::exception& )
    {
        return false;
    }
}


USER_QUOTA_INFO AI_BACKEND_CLIENT::GetUserQuota( const std::string& aAuthToken )
{
    USER_QUOTA_INFO info;
    
    if( aAuthToken.empty() )
    {
        return info;
    }

    std::string url = m_backendUrl + "/user/quota";

    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_HTTPGET, 1L );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 10L );

    int result = curl.Perform();
    if( result != CURLE_OK )
    {
        return info;
    }

    try
    {
        nlohmann::json response = nlohmann::json::parse( curl.GetBuffer() );
        
        info.success = response.value( "success", false );
        info.allowed = response.value( "allowed", false );
        info.plan = response.value( "plan", "" );
        info.code = response.value( "code", "" );
        info.reason = response.value( "reason", "" );
        
        // Cost-based billing fields (NEW)
        if( response.contains( "daily_cost_used" ) && !response["daily_cost_used"].is_null() )
        {
            info.dailyCostUsed = response.value( "daily_cost_used", 0.0 );
        }
        if( response.contains( "daily_cost_cap" ) && !response["daily_cost_cap"].is_null() )
        {
            info.dailyCostCap = response.value( "daily_cost_cap", -1.0 );
        }
        if( response.contains( "monthly_cost_used" ) && !response["monthly_cost_used"].is_null() )
        {
            info.monthlyCostUsed = response.value( "monthly_cost_used", 0.0 );
        }
        if( response.contains( "monthly_cost_cap" ) && !response["monthly_cost_cap"].is_null() )
        {
            info.monthlyCostCap = response.value( "monthly_cost_cap", -1.0 );
        }
        
        // Legacy fields (DEPRECATED - kept for backward compatibility)
        info.dailyLimit = response.value( "daily_limit", 0 );
        info.dailyUsage = response.value( "daily_usage", 0 );
        
        // Parse credits_remaining for on-demand plans
        if( response.contains( "credits_remaining" ) && !response["credits_remaining"].is_null() )
        {
            info.creditsRemaining = response.value( "credits_remaining", -1 );
        }
        
        // Parse trial_hours_left directly from API
        if( response.contains( "trial_hours_left" ) && !response["trial_hours_left"].is_null() )
        {
            info.trialHoursLeft = response.value( "trial_hours_left", -1 );
        }
        
        // Check if in trial
        info.isTrial = response.value( "is_trial", false );
        if( !info.isTrial )
        {
            // Fallback to code-based detection for backward compatibility
            info.isTrial = ( info.code == "TRIAL_ACTIVE" || info.code == "TRIAL_LIMIT_REACHED" || info.plan == "trial" );
        }
        
        return info;
    }
    catch( const std::exception& )
    {
        return info;
    }
}


AI_STREAM_RESULT AI_BACKEND_CLIENT::PostPlanAction( const std::string& aAction,
                                                     const std::string& aSessionId,
                                                     const std::string& aConversationId,
                                                     const std::string& aFeedback,
                                                     const std::string& aAuthToken,
                                                     const std::string& aFilePath,
                                                     const std::string& aKicadFilePath )
{
    AI_STREAM_RESULT result;
    result.status = "success";

    m_isStreaming.store( true );
    m_stopRequested.store( false );

    AMPLITUDE_CLIENT::Instance().Track( "ai_plan_action", {
        { "action", aAction },
    } );

    // Build request payload
    nlohmann::json payload;
    payload["action"] = aAction;
    payload["session_id"] = aSessionId;
    
    if( !aConversationId.empty() )
        payload["conversation_id"] = aConversationId;
    
    if( !aFeedback.empty() )
        payload["feedback"] = aFeedback;

    std::string url = m_backendUrl + "/plan/action";
    std::string body = payload.dump();

    // Setup curl
    KICAD_CURL_EASY curl;
    curl.SetURL( url );
    curl.SetPostFields( body );
    curl.SetHeader( "Content-Type", "application/json" );
    
    // Set Authorization header
    if( !aAuthToken.empty() )
    {
        curl.SetHeader( "Authorization", std::string( "Bearer " ) + aAuthToken );
    }

    // Setup streaming context
    StreamContext ctx;
    ctx.client = this;
    ctx.stopRequested = &m_stopRequested;

    // Process SSE events with exception safety
    // CRITICAL: Include processEvent for tool execution (like StreamChat does)
    ctx.lineCallback = [this, &result, aSessionId, aAuthToken, aFilePath, aKicadFilePath]( const std::string& line )
    {
        try
        {
            if( m_stopRequested.load() )
                return;

            auto eventOpt = parseSSEEvent( line );
            if( !eventOpt )
                return;

            AI_BACKEND_EVENT event = *eventOpt;
            result.eventCount++;

            // Accumulate text response
            if( event.type == AI_EVENT_TYPE::TEXT_DELTA )
            {
                result.response += event.content;
            }

            // CRITICAL: Process event (may execute tools locally and submit results)
            // This is required for tools like read_file, write, search_replace to work
            // NOTE: processEvent already calls m_eventCallback internally, so we don't call it again here
            bool modified = processEvent( event, aFilePath, aKicadFilePath, aSessionId, aAuthToken );
            if( modified )
            {
                result.fileModified = true;
            }

            // Track conversation ID
            if( !event.conversationId.empty() )
            {
                result.conversationId = event.conversationId;
            }

            // Handle errors
            if( event.type == AI_EVENT_TYPE::EVENT_ERROR )
            {
                result.status = "error";
                result.error = event.content;
            }
            else if( event.type == AI_EVENT_TYPE::AUTH_ERROR )
            {
                result.status = "auth_error";
                result.error = "Authentication required";
            }
        }
        catch( const std::exception& e )
        {
            wxLogError( "[PostPlanAction] SSE processing exception: %s", e.what() );
            result.status = "error";
            result.error = std::string( "Processing error: " ) + e.what();
        }
        catch( ... )
        {
            wxLogError( "[PostPlanAction] Unknown SSE processing exception" );
            result.status = "error";
            result.error = "Unknown processing error";
        }
    };

    // Set streaming write callback
    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEFUNCTION, stream_write_callback );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_WRITEDATA, &ctx );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 0L );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_LOW_SPEED_LIMIT, 1L );
    curl_easy_setopt( curl.GetCurl(), CURLOPT_LOW_SPEED_TIME, 120L );

    // Perform request
    int curlResult = curl.Perform();

    WaitForPendingTools();
    if( m_anyToolModifiedFile.exchange( false ) )
        result.fileModified = true;

    m_isStreaming.store( false );

    if( m_stopRequested.load() )
    {
        result.status = "stopped";
    }
    else if( curlResult != CURLE_OK )
    {
        result.status = "error";
        result.error = "Network error: " + std::string( curl_easy_strerror( static_cast<CURLcode>( curlResult ) ) );
    }

    return result;
}

