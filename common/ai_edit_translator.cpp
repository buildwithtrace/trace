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

#include <ai_edit_translator.h>

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <python_manager.h>
#include <paths.h>
#include <pgm_base.h>
#include <config.h>
#include <env_vars.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <regex>

#ifdef _WIN32
#include <process_executor.h>
#endif

#ifndef _WIN32
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif


AI_EDIT_TRANSLATOR::AI_EDIT_TRANSLATOR( const std::string& aAppType )
    : m_appType( aAppType )
{
}


bool AI_EDIT_TRANSLATOR::runConverter( const std::string& aTraceFilePath,
                                       const std::string& aKicadFilePath )
{
    if( !wxFileExists( aTraceFilePath ) )
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Trace file not found: %s" ),
                   wxString::FromUTF8( aTraceFilePath ) );
        return false;
    }

    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    if( pythonPath.IsEmpty() )
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Could not find Python interpreter" ) );
        return false;
    }

    std::string subdir = ( m_appType == "pcbnew" ) ? "pcbnew" : "eeschema";
    std::string fromFormat = ( m_appType == "pcbnew" ) ? "trace_pcb" : "trace_sch";
    std::string toFormat = ( m_appType == "pcbnew" ) ? "kicad_pcb" : "kicad_sch";

    // Find trace.py using the same multi-strategy search as syncTraceToKicad
    wxFileName traceScript;
    bool scriptFound = false;

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
                    wxString fullPath = stockDataPath
                        + wxString::Format( "/scripting/trace/%s/trace.py", subdir );
                    wxFileName resolved( fullPath );
                    if( resolved.FileExists() )
                    {
                        traceScript = resolved;
                        scriptFound = true;
                    }
                }
            }
        }
    }

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
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Could not find trace.py script" ) );
        return false;
    }

    // Build optional flags -- for temp-file-based "before" conversions there is no existing
    // KiCad file to merge into, so we only pass library paths (needed for symbol/footprint
    // resolution during conversion).
    wxString symbolPathsFlag;
    wxString footprintPathsFlag;

    auto getLibraryPaths = []( const wxString& aEnvVarBaseName ) -> wxString
    {
        wxString paths;
        if( Pgm().IsGUI() )
        {
            const ENV_VAR_MAP& envVars = Pgm().GetLocalEnvVariables();
            std::optional<wxString> envValue =
                    ENV_VAR::GetVersionedEnvVarValue( envVars, aEnvVarBaseName );
            if( envValue && !envValue->IsEmpty() )
                paths = *envValue;
        }
        if( paths.IsEmpty() )
        {
            wxString envValue;
            wxString envVarName = ENV_VAR::GetVersionedEnvVarName( aEnvVarBaseName );
            if( wxGetEnv( envVarName, &envValue ) && !envValue.IsEmpty() )
                paths = envValue;
        }
        return paths;
    };

    wxString symbolPaths = getLibraryPaths( wxS( "SYMBOL_DIR" ) );
    if( !symbolPaths.IsEmpty() )
    {
#ifdef _WIN32
        symbolPaths.Replace( wxT( "\\" ), wxT( "/" ) );
#endif
        symbolPathsFlag = wxString::Format( wxT( " --symbol-paths \"%s\"" ), symbolPaths );
    }

    wxString footprintPaths = getLibraryPaths( wxS( "FOOTPRINT_DIR" ) );
    if( !footprintPaths.IsEmpty() )
    {
#ifdef _WIN32
        footprintPaths.Replace( wxT( "\\" ), wxT( "/" ) );
#endif
        footprintPathsFlag = wxString::Format( wxT( " --footprint-paths \"%s\"" ), footprintPaths );
    }

    // Build command with the correct -f/-t flag-based CLI
    // On Unix the shell redirect 2>&1 merges stderr into stdout for pipe capture.
    // On Windows we use ExecuteProcessSilent which captures both streams via Win32
    // pipes, so the redirect is unnecessary (and would require cmd.exe, causing a
    // console window flash).
    wxString command = wxString::Format(
        wxT( "\"%s\" \"%s\" \"%s\" \"%s\" -f %s -t %s%s%s" ),
        pythonPath,
        traceScript.GetFullPath(),
        wxString::FromUTF8( aTraceFilePath ),
        wxString::FromUTF8( aKicadFilePath ),
        wxString::FromUTF8( fromFormat ),
        wxString::FromUTF8( toFormat ),
        symbolPathsFlag,
        footprintPathsFlag );

#ifndef _WIN32
    command += wxT( " 2>&1" );
#endif

    wxLogDebug( wxT( "AI_EDIT_TRANSLATOR: Running converter: %s" ), command );

#ifdef _WIN32
    PROCESS_RESULT procResult = ExecuteProcessSilent( command.ToStdWstring() );

    if( !procResult.success || procResult.exitCode != 0 )
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Converter failed (exit %d): %s" ),
                   procResult.exitCode, wxString::FromUTF8( procResult.output ) );
        return false;
    }

    std::string output = procResult.output;
#else
    // Unix/macOS: fork/exec with stall timeout (matches syncTraceToKicad)
    std::string output;
    int exitCode = -1;
    bool timedOut = false;
    const int stallTimeoutMs = 30000;

    int pipefd[2];
    if( pipe( pipefd ) == -1 )
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Failed to create pipe" ) );
        return false;
    }

    pid_t pid = fork();
    if( pid == -1 )
    {
        close( pipefd[0] );
        close( pipefd[1] );
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Failed to fork process" ) );
        return false;
    }

    if( pid == 0 )
    {
        close( pipefd[0] );
        dup2( pipefd[1], STDOUT_FILENO );
        dup2( pipefd[1], STDERR_FILENO );
        close( pipefd[1] );
        execl( "/bin/sh", "sh", "-c", command.ToStdString().c_str(), NULL );
        _exit( 127 );
    }

    close( pipefd[1] );

    auto lastActivityTime = std::chrono::steady_clock::now();
    char buffer[4096];
    bool processRunning = true;

    while( processRunning )
    {
        fd_set readfds;
        FD_ZERO( &readfds );
        FD_SET( pipefd[0], &readfds );

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select( pipefd[0] + 1, &readfds, NULL, NULL, &tv );

        if( ret > 0 )
        {
            ssize_t bytesRead = read( pipefd[0], buffer, sizeof( buffer ) - 1 );
            if( bytesRead > 0 )
            {
                buffer[bytesRead] = '\0';
                output += buffer;
                lastActivityTime = std::chrono::steady_clock::now();
            }
            else if( bytesRead == 0 )
            {
                processRunning = false;
            }
        }
        else if( ret == 0 )
        {
            auto elapsed = std::chrono::steady_clock::now() - lastActivityTime;
            auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count();

            if( elapsedMs > stallTimeoutMs )
            {
                wxLogError( wxT( "AI_EDIT_TRANSLATOR: Converter stalled for %lldms, killing" ),
                           static_cast<long long>( elapsedMs ) );
                kill( pid, SIGTERM );
                usleep( 100000 );
                kill( pid, SIGKILL );
                timedOut = true;
                processRunning = false;
            }
            else
            {
                int status;
                pid_t result = waitpid( pid, &status, WNOHANG );
                if( result == pid )
                {
                    exitCode = WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
                    processRunning = false;
                }
            }
        }
        else
        {
            processRunning = false;
        }
    }

    if( !timedOut && exitCode == -1 )
    {
        int status;
        waitpid( pid, &status, 0 );
        exitCode = WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
    }

    close( pipefd[0] );

    if( timedOut || exitCode != 0 )
    {
        wxLogError( wxT( "AI_EDIT_TRANSLATOR: Converter failed (exit %d, timedOut=%d): %s" ),
                   exitCode, timedOut ? 1 : 0, wxString::FromUTF8( output ) );
        return false;
    }
#endif

    return wxFileExists( aKicadFilePath );
}


std::string AI_EDIT_TRANSLATOR::extractUuidFromSexp( const std::string& aSexp )
{
    // Match (uuid "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX") or without quotes
    static const std::regex uuidRegex(
        R"(\(uuid\s+\"?([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\"?\))" );

    std::smatch match;
    if( std::regex_search( aSexp, match, uuidRegex ) )
        return match[1].str();

    return "";
}


std::vector<std::string> AI_EDIT_TRANSLATOR::extractTopLevelItems( const std::string& aContent )
{
    std::vector<std::string> items;

    // KiCad s-expression files have a top-level wrapper like (kicad_sch ...) or (kicad_pcb ...)
    // Inside the wrapper, top-level items start with ( at depth 1.
    // We need to find each complete item block at depth 1 (the children of the root).

    // Skip past the root element's opening tag to find child items.
    // The root element starts with (kicad_sch or (kicad_pcb followed by properties,
    // then child elements like (symbol ...), (wire ...), (footprint ...), etc.

    int depth = 0;
    size_t itemStart = 0;
    bool inString = false;
    bool escaped = false;

    // Item types we care about for undo/redo tracking
    static const std::vector<std::string> trackedTypes = {
        // Schematic items
        "symbol", "wire", "bus", "junction", "no_connect", "bus_entry",
        "label", "global_label", "hierarchical_label", "net_class_flag",
        "text", "text_box", "polyline", "arc", "circle", "rectangle",
        "sheet", "sheet_instances", "symbol_instances", "image",
        // PCB items
        "footprint", "segment", "arc", "via", "zone", "gr_line",
        "gr_arc", "gr_circle", "gr_rect", "gr_poly", "gr_text",
        "gr_text_box", "dimension", "target", "group"
    };

    for( size_t i = 0; i < aContent.size(); i++ )
    {
        char c = aContent[i];

        if( escaped )
        {
            escaped = false;
            continue;
        }

        if( c == '\\' && inString )
        {
            escaped = true;
            continue;
        }

        if( c == '"' )
        {
            inString = !inString;
            continue;
        }

        if( inString )
            continue;

        if( c == '(' )
        {
            depth++;
            if( depth == 2 )
            {
                itemStart = i;
            }
        }
        else if( c == ')' )
        {
            if( depth == 2 )
            {
                std::string block = aContent.substr( itemStart, i - itemStart + 1 );

                // Check if this is a tracked item type
                size_t typeStart = block.find( '(' );
                if( typeStart != std::string::npos )
                {
                    typeStart++; // skip '('
                    size_t typeEnd = block.find_first_of( " \t\n\r)", typeStart );
                    if( typeEnd != std::string::npos )
                    {
                        std::string itemType = block.substr( typeStart, typeEnd - typeStart );
                        for( const auto& tracked : trackedTypes )
                        {
                            if( itemType == tracked )
                            {
                                items.push_back( block );
                                break;
                            }
                        }
                    }
                }
            }
            depth--;
        }
    }

    return items;
}


std::map<std::string, std::string> AI_EDIT_TRANSLATOR::extractItemsByUuid(
    const std::string& aKicadContent )
{
    std::map<std::string, std::string> result;

    std::vector<std::string> items = extractTopLevelItems( aKicadContent );

    for( const auto& item : items )
    {
        std::string uuid = extractUuidFromSexp( item );
        if( !uuid.empty() )
        {
            result[uuid] = item;
        }
    }

    return result;
}


AI_EDIT_TRANSLATION AI_EDIT_TRANSLATOR::diffKicadContent( const std::string& aBeforeKicad,
                                                          const std::string& aAfterKicad )
{
    AI_EDIT_TRANSLATION result;

    auto beforeItems = extractItemsByUuid( aBeforeKicad );
    auto afterItems  = extractItemsByUuid( aAfterKicad );

    for( const auto& [uuid, sexp] : afterItems )
    {
        auto beforeIt = beforeItems.find( uuid );
        if( beforeIt == beforeItems.end() )
        {
            AI_EDIT_OP op;
            op.type = AI_EDIT_OP_TYPE::ADD;
            op.itemUuid = KIID( wxString::FromUTF8( uuid ) );
            op.newSexp = sexp;
            result.ops.push_back( std::move( op ) );
        }
        else if( beforeIt->second != sexp )
        {
            AI_EDIT_OP op;
            op.type = AI_EDIT_OP_TYPE::MODIFY;
            op.itemUuid = KIID( wxString::FromUTF8( uuid ) );
            op.newSexp = sexp;
            op.oldSexp = beforeIt->second;
            result.ops.push_back( std::move( op ) );
        }
    }

    for( const auto& [uuid, sexp] : beforeItems )
    {
        if( afterItems.find( uuid ) == afterItems.end() )
        {
            AI_EDIT_OP op;
            op.type = AI_EDIT_OP_TYPE::REMOVE;
            op.itemUuid = KIID( wxString::FromUTF8( uuid ) );
            op.oldSexp = sexp;
            result.ops.push_back( std::move( op ) );
        }
    }

    result.success = true;
    wxLogDebug( wxT( "AI_EDIT_TRANSLATOR: Translated %zu ops (%zu adds, %zu removes, %zu modifies)" ),
               result.ops.size(),
               std::count_if( result.ops.begin(), result.ops.end(),
                              []( const AI_EDIT_OP& op ) { return op.type == AI_EDIT_OP_TYPE::ADD; } ),
               std::count_if( result.ops.begin(), result.ops.end(),
                              []( const AI_EDIT_OP& op ) { return op.type == AI_EDIT_OP_TYPE::REMOVE; } ),
               std::count_if( result.ops.begin(), result.ops.end(),
                              []( const AI_EDIT_OP& op ) { return op.type == AI_EDIT_OP_TYPE::MODIFY; } ) );

    return result;
}


AI_EDIT_TRANSLATION AI_EDIT_TRANSLATOR::Translate( const std::string& aBeforeTraceContent,
                                                    const std::string& aAfterTraceContent )
{
    AI_EDIT_TRANSLATION result;

    if( aBeforeTraceContent == aAfterTraceContent )
    {
        result.success = true;
        return result;
    }

    wxString tempDir = wxFileName::GetTempDir() + wxT( "/trace_edit_translate" );
    wxFileName::Mkdir( tempDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    std::string traceExt = ( m_appType == "pcbnew" ) ? ".trace_pcb" : ".trace_sch";
    std::string kicadExt = ( m_appType == "pcbnew" ) ? ".kicad_pcb" : ".kicad_sch";

    wxString beforeTracePath = tempDir + wxT( "/before" ) + wxString::FromUTF8( traceExt );
    wxString afterTracePath  = tempDir + wxT( "/after" )  + wxString::FromUTF8( traceExt );
    wxString beforeKicadPath = tempDir + wxT( "/before" ) + wxString::FromUTF8( kicadExt );
    wxString afterKicadPath  = tempDir + wxT( "/after" )  + wxString::FromUTF8( kicadExt );

    {
        std::ofstream beforeFile( beforeTracePath.ToStdString() );
        if( !beforeFile.is_open() )
        {
            result.errorMessage = "Failed to write before trace temp file";
            return result;
        }
        beforeFile << aBeforeTraceContent;
    }
    {
        std::ofstream afterFile( afterTracePath.ToStdString() );
        if( !afterFile.is_open() )
        {
            result.errorMessage = "Failed to write after trace temp file";
            return result;
        }
        afterFile << aAfterTraceContent;
    }

    if( !runConverter( beforeTracePath.ToStdString(), beforeKicadPath.ToStdString() ) )
    {
        result.errorMessage = "Failed to convert before trace content";
        wxRemoveFile( beforeTracePath );
        wxRemoveFile( afterTracePath );
        return result;
    }

    if( !runConverter( afterTracePath.ToStdString(), afterKicadPath.ToStdString() ) )
    {
        result.errorMessage = "Failed to convert after trace content";
        wxRemoveFile( beforeTracePath );
        wxRemoveFile( afterTracePath );
        wxRemoveFile( beforeKicadPath );
        return result;
    }

    std::string beforeKicad, afterKicad;
    {
        std::ifstream f( beforeKicadPath.ToStdString() );
        if( !f.is_open() )
        {
            result.errorMessage = "Failed to read before KiCad file";
            return result;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        beforeKicad = ss.str();
    }
    {
        std::ifstream f( afterKicadPath.ToStdString() );
        if( !f.is_open() )
        {
            result.errorMessage = "Failed to read after KiCad file";
            return result;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        afterKicad = ss.str();
    }

    wxRemoveFile( beforeTracePath );
    wxRemoveFile( afterTracePath );
    wxRemoveFile( beforeKicadPath );
    wxRemoveFile( afterKicadPath );

    result = diffKicadContent( beforeKicad, afterKicad );

    m_lastAfterKicadContent = std::move( afterKicad );

    return result;
}


AI_EDIT_TRANSLATION AI_EDIT_TRANSLATOR::TranslateWithKicadFile(
        const std::string& aBeforeTraceContent,
        const std::string& aAfterKicadFilePath,
        const std::string& aAfterTraceContent )
{
    AI_EDIT_TRANSLATION result;

    // Read the "after" KiCad s-expression directly from disk (already converted by
    // syncTraceToKicad in the tool executor).
    std::string afterKicad;
    {
        std::ifstream f( aAfterKicadFilePath );
        if( !f.is_open() )
        {
            wxLogWarning( wxT( "AI_EDIT_TRANSLATOR: Could not read after KiCad file '%s', "
                               "falling back to full conversion" ),
                         wxString::FromUTF8( aAfterKicadFilePath ) );
            return Translate( aBeforeTraceContent, aAfterTraceContent );
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        afterKicad = ss.str();
    }

    if( afterKicad.empty() )
    {
        wxLogWarning( wxT( "AI_EDIT_TRANSLATOR: After KiCad file is empty, falling back" ) );
        return Translate( aBeforeTraceContent, aAfterTraceContent );
    }

    // For the "before" state, use the cached content from the previous edit if available.
    // The "before" of edit N is the "after" of edit N-1.
    std::string beforeKicad;

    if( !m_lastAfterKicadContent.empty() )
    {
        beforeKicad = m_lastAfterKicadContent;
    }
    else if( aBeforeTraceContent.empty() )
    {
        // No previous content (new file). The entire after content is "added".
        beforeKicad = "";
    }
    else
    {
        // First edit in session -- need to convert the "before" trace content.
        wxString tempDir = wxFileName::GetTempDir() + wxT( "/trace_edit_translate" );
        wxFileName::Mkdir( tempDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

        std::string traceExt = ( m_appType == "pcbnew" ) ? ".trace_pcb" : ".trace_sch";
        std::string kicadExt = ( m_appType == "pcbnew" ) ? ".kicad_pcb" : ".kicad_sch";

        wxString beforeTracePath = tempDir + wxT( "/before" ) + wxString::FromUTF8( traceExt );
        wxString beforeKicadPath = tempDir + wxT( "/before" ) + wxString::FromUTF8( kicadExt );

        {
            std::ofstream beforeFile( beforeTracePath.ToStdString() );
            if( !beforeFile.is_open() )
            {
                result.errorMessage = "Failed to write before trace temp file";
                return result;
            }
            beforeFile << aBeforeTraceContent;
        }

        if( !runConverter( beforeTracePath.ToStdString(), beforeKicadPath.ToStdString() ) )
        {
            result.errorMessage = "Failed to convert before trace content";
            wxRemoveFile( beforeTracePath );
            return result;
        }

        {
            std::ifstream f( beforeKicadPath.ToStdString() );
            if( !f.is_open() )
            {
                result.errorMessage = "Failed to read before KiCad file";
                wxRemoveFile( beforeTracePath );
                return result;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            beforeKicad = ss.str();
        }

        wxRemoveFile( beforeTracePath );
        wxRemoveFile( beforeKicadPath );
    }

    result = diffKicadContent( beforeKicad, afterKicad );

    m_lastAfterKicadContent = std::move( afterKicad );

    return result;
}
