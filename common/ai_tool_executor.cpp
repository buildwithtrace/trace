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

#include "ai_tool_executor.h"
#include "vector_search_engine.h"
#include <amplitude_client.h>
#include <common.h>
#include <conversation_db.h>
#include <python_manager.h>
#include <paths.h>
#include <pgm_base.h>
#include <config.h>
#include <env_vars.h>
#include <trace_helpers.h>
#include <libraries/library_manager.h>

#ifdef _WIN32
#include <process_executor.h>
#endif

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/textfile.h>
#include <wx/utils.h>
#include <wx/base64.h>
#include <wx/dir.h>
#include <wx/tokenzr.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <future>
#include <regex>
#include <cstdio>
#include <array>
#include <set>

// Cross-platform popen/pclose
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

// For PATH_MAX and realpath on Unix
#ifdef __UNIX__
#include <climits>
#include <cstdlib>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <process/python_process_manager.h>
#endif

#ifdef _WIN32
#include <process/python_process_manager.h>
#endif


// =============================================================================
// Security: Allowed file extensions for read/write operations
// =============================================================================
const std::set<std::string> AI_TOOL_EXECUTOR::s_allowedExtensions = {
    ".trace_sch",   // Trace schematic files
    ".trace_pcb",   // Trace PCB files
    ".kicad_sch",   // KiCad schematic files
    ".kicad_pcb",   // KiCad PCB files
    ".kicad_sym",   // KiCad symbol library files (AI-generated symbols)
    ".kicad_mod",   // KiCad footprint files (AI-generated footprints)
    ".svg",         // Snapshot files
    ".request",     // IPC request files for snapshots
    ".response",    // IPC response files for snapshots
    ".backup",      // Backup files
    ".zip",         // Zip archive files
    ".gbr",         // Gerber files
    ".drl"          // Drill files
};

const std::set<std::string> AI_TOOL_EXECUTOR::s_allowedFilenames = {
    "fp-lib-table",    // Footprint library table (no extension)
    "sym-lib-table"    // Symbol library table (no extension)
};


// =============================================================================
// Concurrent Editing: Static members for file locking
// =============================================================================
std::map<std::string, std::shared_mutex> AI_TOOL_EXECUTOR::s_fileLocks;
std::mutex AI_TOOL_EXECUTOR::s_fileLocksMapMutex;


AI_TOOL_EXECUTOR::AI_TOOL_EXECUTOR( const std::string& aAppType ) :
    m_appType( aAppType ),
    m_conversionPending( false ),
    m_lastConversionSucceeded( true )  // Assume success until proven otherwise
{
    m_lastConversionRequest = std::chrono::steady_clock::now();
}


// =============================================================================
// Concurrent Editing: File locking and version control
// =============================================================================

std::shared_mutex& AI_TOOL_EXECUTOR::getFileLock( const std::string& aCanonicalPath )
{
    std::lock_guard<std::mutex> mapLock( s_fileLocksMapMutex );
    // operator[] creates entry if not exists
    return s_fileLocks[aCanonicalPath];
}


std::string AI_TOOL_EXECUTOR::computeFileHash( const std::string& aContent )
{
    // Fast hash for change detection (FNV-1a)
    // Not cryptographic, but fast and good for detecting changes
    const uint64_t FNV_PRIME = 1099511628211ULL;
    const uint64_t FNV_OFFSET = 14695981039346656037ULL;
    
    uint64_t hash = FNV_OFFSET;
    for( char c : aContent )
    {
        hash ^= static_cast<uint64_t>( static_cast<unsigned char>( c ) );
        hash *= FNV_PRIME;
    }
    
    // Convert to hex string
    std::ostringstream oss;
    oss << std::hex << std::setfill( '0' ) << std::setw( 16 ) << hash;
    return oss.str();
}


std::pair<std::string, std::string> AI_TOOL_EXECUTOR::readFileWithHash( const std::string& aFilePath )
{
    std::string canonical = getCanonicalPath( aFilePath );
    if( canonical.empty() )
        canonical = aFilePath;  // Fallback if canonical fails
    
    // Acquire shared (read) lock
    std::shared_mutex& fileLock = getFileLock( canonical );
    std::shared_lock<std::shared_mutex> readLock( fileLock );
    
    // Read file content
    std::string content = readFileContent( aFilePath );
    std::string hash = computeFileHash( content );
    
    return { content, hash };
}


bool AI_TOOL_EXECUTOR::writeFileIfUnchanged( const std::string& aFilePath,
                                              const std::string& aNewContent,
                                              const std::string& aExpectedHash,
                                              std::string&       aConflictContent )
{
    wxLogTrace( traceAiFileOp, wxT( "writeFileIfUnchanged: path=%s, size=%zu, expectedHash=%s" ),
                aFilePath, aNewContent.size(), aExpectedHash );

    std::string canonical = getCanonicalPath( aFilePath );
    if( canonical.empty() )
        canonical = aFilePath;
    
    // Acquire exclusive (write) lock
    std::shared_mutex& fileLock = getFileLock( canonical );
    std::unique_lock<std::shared_mutex> writeLock( fileLock );
    
    // Re-read file to check for changes
    std::string currentContent = readFileContent( aFilePath );
    std::string currentHash = computeFileHash( currentContent );
    
    if( currentHash != aExpectedHash )
    {
        wxLogTrace( traceAiFileOp, wxT( "writeFileIfUnchanged: CONFLICT! currentHash=%s != expectedHash=%s" ),
                    currentHash, aExpectedHash );
        // CONFLICT: File was modified by another operation
        aConflictContent = currentContent;
        return false;
    }
    
    // No conflict - safe to write
    bool result = writeFileContent( aFilePath, aNewContent );
    wxLogTrace( traceAiFileOp, wxT( "writeFileIfUnchanged: result=%d" ), result );
    return result;
}


// =============================================================================
// Security: Project directory management
// =============================================================================

void AI_TOOL_EXECUTOR::SetAllowedProjectDirs( const std::vector<std::string>& aDirs )
{
    m_allowedProjectDirs.clear();
    for( const auto& dir : aDirs )
    {
        std::string canonical = getCanonicalPath( dir );
        if( !canonical.empty() )
        {
            m_allowedProjectDirs.push_back( canonical );
        }
    }
}


void AI_TOOL_EXECUTOR::AddAllowedProjectDir( const std::string& aDir )
{
    std::string canonical = getCanonicalPath( aDir );
    if( !canonical.empty() )
    {
        // Check if already in the list
        for( const auto& existing : m_allowedProjectDirs )
        {
            if( existing == canonical )
                return;  // Already exists
        }
        m_allowedProjectDirs.push_back( canonical );
    }
}


void AI_TOOL_EXECUTOR::ClearAllowedProjectDirs()
{
    m_allowedProjectDirs.clear();
}


std::string AI_TOOL_EXECUTOR::getCanonicalPath( const std::string& aFilePath )
{
    wxFileName path( aFilePath );
    
    // Normalize the path (resolve . and ..)
    path.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE );
    
    // Get the full path
    wxString fullPath = path.GetFullPath();
    
    // On platforms that support it, try to resolve symlinks
#ifdef __UNIX__
    char resolvedPath[PATH_MAX];
    if( realpath( fullPath.ToUTF8(), resolvedPath ) != nullptr )
    {
        return std::string( resolvedPath );
    }
#endif
    
    return fullPath.ToStdString();
}


std::string AI_TOOL_EXECUTOR::resolveEnvVars( const std::string& aFilePath,
                                               const std::string& aProjectDir )
{
    std::string result = aFilePath;

    // ${KIPRJMOD} is special — always resolved to the project directory
    const std::string kiprjmod = "${KIPRJMOD}";
    size_t pos = result.find( kiprjmod );

    if( pos != std::string::npos )
        result.replace( pos, kiprjmod.length(), aProjectDir );

    // Resolve any remaining ${VAR} references via the standard KiCad mechanism
    if( result.find( "${" ) != std::string::npos )
    {
        wxString expanded = ExpandEnvVarSubstitutions( wxString( result ), nullptr );
        result = expanded.ToStdString();
    }

    return result;
}


bool AI_TOOL_EXECUTOR::isPathWithinDirectory( const std::string& aPath, const std::string& aDirectory )
{
    // Both paths should already be canonical
    // Check if aPath starts with aDirectory
    if( aPath.length() < aDirectory.length() )
        return false;
    
    // Compare the directory prefix
    if( aPath.compare( 0, aDirectory.length(), aDirectory ) != 0 )
        return false;
    
    // Make sure we're at a directory boundary
    // Either the paths are equal, or the next character is a path separator
    if( aPath.length() == aDirectory.length() )
        return true;
    
    char nextChar = aPath[aDirectory.length()];
#ifdef _WIN32
    return nextChar == '/' || nextChar == '\\';
#else
    return nextChar == '/';
#endif
}


std::pair<bool, std::string> AI_TOOL_EXECUTOR::validateFilePath( const std::string& aFilePath,
                                                                  const std::string& aOperation )
{
    if( aFilePath.empty() )
    {
        return { false, "Security: Empty file path" };
    }

    // Resolve to canonical path to prevent path traversal attacks
    std::string canonicalPath = getCanonicalPath( aFilePath );
    if( canonicalPath.empty() )
    {
        std::string error = "Security: Could not resolve path: " + aFilePath;
        return { false, error };
    }

    // Check 1: Extension validation (also allows specific extensionless filenames)
    wxFileName path( canonicalPath );
    wxString ext = "." + path.GetExt().Lower();
    std::string extStr = ext.ToStdString();
    std::string filename = path.GetFullName().ToStdString();
    
    bool allowed = ( s_allowedExtensions.find( extStr ) != s_allowedExtensions.end() )
                || ( s_allowedFilenames.find( filename ) != s_allowedFilenames.end() );
    
    if( !allowed )
    {
        std::string error = "Security: Blocked " + aOperation + " to disallowed file type: " 
                          + extStr + " (file: " + filename + ")";
        return { false, error };
    }

    // Check 2: Directory allowlist validation
    // If no allowed directories are set, allow any path (development mode)
    if( m_allowedProjectDirs.empty() )
    {
        return { true, "" };
    }

    // Check if path is within any allowed directory
    for( const auto& allowedDir : m_allowedProjectDirs )
    {
        if( isPathWithinDirectory( canonicalPath, allowedDir ) )
        {
            // Path is within an allowed directory
            return { true, "" };
        }
    }

    // Path is not within any allowed directory
    std::string error = "Security: Blocked " + aOperation + " to file outside allowed projects: " 
                      + canonicalPath;
    return { false, error };
}


std::string AI_TOOL_EXECUTOR::readFileContent( const std::string& aFilePath )
{
    wxLogTrace( traceAiFileOp, wxT( "readFileContent: path=%s" ), aFilePath );

    // Security: Validate file path before reading
    auto [isValid, errorMsg] = validateFilePath( aFilePath, "read" );
    if( !isValid )
    {
        wxLogTrace( traceAiFileOp, wxT( "readFileContent: BLOCKED - %s" ), errorMsg );
        return "";
    }

    // Use canonical path for actual file access
    std::string canonicalPath = getCanonicalPath( aFilePath );
    
    std::ifstream file( canonicalPath );
    if( !file.is_open() )
    {
        wxLogTrace( traceAiFileOp, wxT( "readFileContent: FAILED to open file" ) );
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    wxLogTrace( traceAiFileOp, wxT( "readFileContent: success, size=%zu" ), buffer.str().size() );
    return buffer.str();
}


bool AI_TOOL_EXECUTOR::writeFileContent( const std::string& aFilePath, const std::string& aContent )
{
    wxLogTrace( traceAiFileOp, wxT( "writeFileContent: path=%s, size=%zu" ), aFilePath, aContent.size() );

    // Security: Validate file path before writing
    auto [isValid, errorMsg] = validateFilePath( aFilePath, "write" );
    if( !isValid )
    {
        wxLogTrace( traceAiFileOp, wxT( "writeFileContent: BLOCKED - %s" ), errorMsg );
        return false;
    }

    // Use canonical path for actual file access
    std::string canonicalPath = getCanonicalPath( aFilePath );

    // Create backup first
    wxFileName path( canonicalPath );
    if( path.FileExists() )
    {
        std::string backupPath = canonicalPath + ".backup";
        
        // Validate backup path too (should pass since .backup is allowed)
        auto [backupValid, backupError] = validateFilePath( backupPath, "backup" );
        if( backupValid )
        {
            wxCopyFile( canonicalPath, backupPath );
            wxLogTrace( traceAiFileOp, wxT( "writeFileContent: created backup" ) );
        }
    }

    std::ofstream file( canonicalPath );
    if( !file.is_open() )
    {
        wxLogTrace( traceAiFileOp, wxT( "writeFileContent: FAILED to open file for writing" ) );
        return false;
    }

    file << aContent;
    file.close();

    wxLogTrace( traceAiFileOp, wxT( "writeFileContent: write complete, success=1" ) );
    return true;
}


size_t AI_TOOL_EXECUTOR::countOccurrences( const std::string& aHaystack, const std::string& aNeedle )
{
    size_t count = 0;
    size_t pos = 0;

    while( ( pos = aHaystack.find( aNeedle, pos ) ) != std::string::npos )
    {
        count++;
        pos += aNeedle.length();
    }

    return count;
}


std::string AI_TOOL_EXECUTOR::normalizeLineEndings( const std::string& aContent )
{
    // Normalize CRLF (\r\n) and CR (\r) to LF (\n)
    std::string result;
    result.reserve( aContent.size() );
    
    for( size_t i = 0; i < aContent.size(); i++ )
    {
        if( aContent[i] == '\r' )
        {
            result += '\n';
            // Skip the following \n if this is CRLF
            if( i + 1 < aContent.size() && aContent[i + 1] == '\n' )
                i++;
        }
        else
        {
            result += aContent[i];
        }
    }
    
    return result;
}


bool AI_TOOL_EXECUTOR::isWhitespaceOnly( const std::string& aContent )
{
    return aContent.empty() || 
           std::all_of( aContent.begin(), aContent.end(), 
                        []( unsigned char c ) { return std::isspace( c ); } );
}


std::string AI_TOOL_EXECUTOR::rtrimNewlines( const std::string& aContent )
{
    // Strip trailing newlines from content
    std::string result = aContent;
    while( !result.empty() && result.back() == '\n' )
        result.pop_back();
    return result;
}


std::string AI_TOOL_EXECUTOR::getTraceFilePath( const std::string& aKicadFilePath )
{
    wxFileName path( aKicadFilePath );
    wxString   ext = path.GetExt();

    if( ext == "kicad_sch" )
        path.SetExt( "trace_sch" );
    else if( ext == "kicad_pcb" )
        path.SetExt( "trace_pcb" );

    return path.GetFullPath().ToStdString();
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::ExecuteTool( const std::string&    aToolName,
                                               const nlohmann::json& aToolArgs,
                                               const std::string&    aFilePath,
                                               const std::string&    aKicadFilePath )
{
    wxLogTrace( traceAiToolCall, wxT( "ExecuteTool: START tool=%s, filePath=%s" ),
                aToolName, aFilePath );

    auto toolStartTime = std::chrono::steady_clock::now();

    if( aFilePath.empty() )
    {
        wxLogTrace( traceAiToolCall, wxT( "ExecuteTool: FAILED - no file path provided" ) );
        AMPLITUDE_CLIENT::Instance().Track( "ai_tool_result", {
            { "tool_name", aToolName },
            { "success", false },
            { "file_modified", false },
            { "duration_ms", 0.0 },
            { "error", "no_file_path" }
        });
        return AI_TOOL_RESULT( "Error: No file path provided for tool execution", false, false );
    }

    // Get project directory from file path
    wxFileName mainFilePath( aFilePath );
    std::string projectDir = mainFilePath.GetPath().ToStdString();

    AI_TOOL_RESULT result( "", false, false );

    try
    {
        // =================================================================
        // New consolidated tools
        // =================================================================
        
        // list_dir - replaces list_trace_files
        if( aToolName == "list_dir" )
        {
            result = executeListDir( aToolArgs, projectDir );
        }
        // read_file - replaces get_file_chunk, get_element_at_line
        else if( aToolName == "read_file" )
        {
            result = executeReadFile( aToolArgs, projectDir, aFilePath );
        }
        // write - replaces create_trace_file for new files
        else if( aToolName == "write" )
        {
            result = executeWrite( aToolArgs, projectDir, aKicadFilePath );
        }
        // search_replace - replaces replace_in_file, append_to_file, insert_at_line
        else if( aToolName == "search_replace" )
        {
            result = executeSearchReplace( aToolArgs, projectDir, aFilePath, aKicadFilePath );
        }
        // grep - replaces search_in_file
        else if( aToolName == "grep" )
        {
            result = executeGrep( aToolArgs, projectDir, aFilePath );
        }
        
        // =================================================================
        // Unchanged tools
        // =================================================================
        
        else if( aToolName == "delete_trace_file" )
        {
            result = executeDeleteTraceFile( aToolArgs, projectDir, aFilePath );
        }
        else if( aToolName == "take_snapshot" )
        {
            result = executeTakeSnapshot( aFilePath, aKicadFilePath );
        }
        else if( aToolName == "run_drc" || aToolName == "get_drc_violations" )
        {
            result = executeRunDrc( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "run_erc" || aToolName == "get_erc_violations" )
        {
            result = executeRunErc( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "run_annotate" || aToolName == "annotate_schematic" )
        {
            result = executeRunAnnotate( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "generate_gerbers" )
        {
            result = executeGenerateGerbers( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "generate_drill_files" || aToolName == "generate_drill" )
        {
            result = executeGenerateDrill( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "generate_position_file" )
        {
            result = executeGeneratePositionFile( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "zip_gerber_files" || aToolName == "zip_gerbers" )
        {
            result = executeZipGerberFiles( aFilePath );
        }
        else if( aToolName == "autoroute" )
        {
            result = executeAutoroute( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "update_pcb_from_schematic" )
        {
            result = executeUpdatePcbFromSchematic( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "get_hierarchy" )
        {
            result = executeGetHierarchy( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "switch_sheet" )
        {
            result = executeSwitchSheet( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "switch_layer" )
        {
            result = executeSwitchLayer( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "get_layers" )
        {
            result = executeGetLayers( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "apply_layer_preset" )
        {
            result = executeApplyLayerPreset( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "set_layer_visibility" )
        {
            result = executeSetLayerVisibility( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "fetch_dimensions" )
        {
            result = executeFetchDimensions( aToolArgs, aKicadFilePath );
        }
        else if( aToolName == "todo_write" )
        {
            result = executeTodoWrite( aToolArgs );
        }
        else if( aToolName == "todo_read" )
        {
            result = executeTodoRead( aToolArgs );
        }
        else if( aToolName == "search_symbols" )
        {
            result = executeSearchSymbol( aToolArgs );
        }
        else if( aToolName == "search_footprints" )
        {
            result = executeSearchFootprint( aToolArgs );
        }
        else if( aToolName == "fetch_component_pins" )
        {
            result = executeFetchComponentPins( aToolArgs );
        }
        else if( aToolName == "set_preferred_manufacturer" )
        {
            result = executeSetPreferredManufacturer( aToolArgs );
        }
        else if( aToolName == "set_manufacturer_drc" )
        {
            result = executeSetManufacturerDrc( aToolArgs );
        }
        else if( aToolName == "trace_signal_path" )
        {
            result = executeTraceSignalPath( aToolArgs );
        }
        else if( aToolName == "trace_pcb_signal_path" )
        {
            result = executeTracePcbSignalPath( aToolArgs );
        }
        else if( aToolName == "highlight_net" )
        {
            result = executeHighlightNet( aToolArgs );
        }
        else if( aToolName == "select_items" )
        {
            result = executeSelectItems( aToolArgs );
        }
        else
        {
            result = AI_TOOL_RESULT( "Unknown tool: " + aToolName, false, false );
        }
    }
    catch( const std::exception& e )
    {
        wxLogTrace( traceAiToolCall, wxT( "ExecuteTool: EXCEPTION tool=%s, error=%s" ),
                    aToolName, e.what() );
        double durationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - toolStartTime ).count();
        AMPLITUDE_CLIENT::Instance().Track( "ai_tool_result", {
            { "tool_name", aToolName },
            { "success", false },
            { "file_modified", false },
            { "duration_ms", durationMs },
            { "error", std::string( e.what() ) }
        });
        return AI_TOOL_RESULT( std::string( "Error executing " ) + aToolName + ": " + e.what(),
                               false, false );
    }

    double durationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - toolStartTime ).count();

    wxLogTrace( traceAiToolCall, wxT( "ExecuteTool: END tool=%s, success=%d, fileModified=%d, duration=%.1fms" ),
                aToolName, result.success, result.fileModified, durationMs );

    AMPLITUDE_CLIENT::Instance().Track( "ai_tool_result", {
        { "tool_name", aToolName },
        { "success", result.success },
        { "file_modified", result.fileModified },
        { "duration_ms", durationMs }
    });

    return result;
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeTakeSnapshot( const std::string& aFilePath,
                                                       const std::string& aKicadFilePath )
{
    if( !m_snapshotCallback )
    {
        return AI_TOOL_RESULT( "Error: Snapshot callback not available", false, false );
    }
    
    try
    {
        std::string base64Content = m_snapshotCallback();
        
        if( base64Content.empty() )
        {
            return AI_TOOL_RESULT( "Error: Failed to generate snapshot", false, false );
        }
        
        return AI_TOOL_RESULT( base64Content );
    }
    catch( const std::exception& e )
    {
        return AI_TOOL_RESULT( std::string( "Error generating snapshot: " ) + e.what(),
                               false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeReadFile( const nlohmann::json& aArgs,
                                                   const std::string&    aProjectDir,
                                                   const std::string&    aDefaultFilePath )
{
    // Get target_file parameter (also accept file_path as an alias)
    std::string targetFile;
    if( aArgs.contains( "target_file" ) && aArgs["target_file"].is_string() )
        targetFile = aArgs["target_file"].get<std::string>();
    else if( aArgs.contains( "file_path" ) && aArgs["file_path"].is_string() )
        targetFile = aArgs["file_path"].get<std::string>();

    // Resolve ${KIPRJMOD} and other environment variables
    if( !targetFile.empty() )
        targetFile = resolveEnvVars( targetFile, aProjectDir );
    
    // Resolve file path
    std::string filePath;
    if( targetFile.empty() )
    {
        filePath = aDefaultFilePath;
    }
    else
    {
        // Check if it's an absolute path or relative
        wxFileName fn( targetFile );
        if( fn.IsAbsolute() )
        {
            filePath = targetFile;
        }
        else
        {
            wxFileName resolved( aProjectDir, targetFile );
            filePath = resolved.GetFullPath().ToStdString();
        }
    }
    
    if( filePath.empty() )
        return AI_TOOL_RESULT( "Error: No file path specified", false, false );
    
    // Security: Validate file path
    auto [isValid, errorMsg] = validateFilePath( filePath, "read" );
    if( !isValid )
        return AI_TOOL_RESULT( "Security error: " + errorMsg, false, false );
    
    // Check if file exists
    if( !wxFileExists( filePath ) )
        return AI_TOOL_RESULT( "Error: File not found: " + filePath, false, false );
    
    // Read file content
    std::string content = readFileContent( filePath );
    
    if( content.empty() )
        return AI_TOOL_RESULT( "File is empty." );
    
    // Get offset and limit parameters
    int offset = aArgs.value( "offset", 1 );  // 1-indexed, default to start
    int limit = aArgs.value( "limit", -1 );   // -1 means read all
    
    // Split content into lines
    std::vector<std::string> lines;
    std::istringstream stream( content );
    std::string line;
    while( std::getline( stream, line ) )
        lines.push_back( line );
    
    int totalLines = static_cast<int>( lines.size() );
    
    // Validate offset
    if( offset < 1 )
        offset = 1;
    if( offset > totalLines )
        return AI_TOOL_RESULT( "Error: offset " + std::to_string( offset ) + 
                               " exceeds file length (" + std::to_string( totalLines ) + " lines)",
                               false, false );
    
    // Calculate end line
    int endLine;
    if( limit < 0 )
    {
        endLine = totalLines;
    }
    else
    {
        endLine = std::min( offset + limit - 1, totalLines );
    }
    
    // Build output with line numbers in format LINE_NUMBER|LINE_CONTENT
    std::ostringstream output;
    for( int i = offset; i <= endLine; i++ )
    {
        // Right-align line number to 6 characters
        output << std::setw( 6 ) << i << "|" << lines[i - 1] << "\n";
    }
    
    return AI_TOOL_RESULT( output.str() );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeWrite( const nlohmann::json& aArgs,
                                                const std::string&    aProjectDir,
                                                const std::string&    aKicadFilePath )
{
    // Get file_path parameter
    std::string filePath;
    if( aArgs.contains( "file_path" ) && aArgs["file_path"].is_string() )
        filePath = aArgs["file_path"].get<std::string>();
    
    if( filePath.empty() )
        return AI_TOOL_RESULT( "Error: file_path is required", false, false );

    // Resolve ${KIPRJMOD} and other environment variables
    filePath = resolveEnvVars( filePath, aProjectDir );
    
    // Get contents parameter
    std::string contents;
    if( aArgs.contains( "contents" ) && aArgs["contents"].is_string() )
        contents = aArgs["contents"].get<std::string>();
    
    wxLogTrace( traceAiToolCall, wxT( "executeWrite: filePath=%s, contentSize=%zu" ),
                filePath, contents.size() );

    // Resolve file path
    wxFileName fn( filePath );
    std::string resolvedPath;
    if( fn.IsAbsolute() )
    {
        resolvedPath = filePath;
    }
    else
    {
        wxFileName resolved( aProjectDir, filePath );
        resolvedPath = resolved.GetFullPath().ToStdString();
    }
    
    // Security: Validate file path
    auto [isValid, errorMsg] = validateFilePath( resolvedPath, "write" );
    if( !isValid )
    {
        wxLogTrace( traceAiToolCall, wxT( "executeWrite: BLOCKED - %s" ), errorMsg );
        return AI_TOOL_RESULT( "Security error: " + errorMsg, false, false );
    }

    // Create parent directories if they don't exist (needed for .pretty dirs)
    {
        wxFileName parentDir( resolvedPath );
        wxString dirPath = parentDir.GetPath();
        if( !dirPath.IsEmpty() && !wxDirExists( dirPath ) )
        {
            wxFileName::Mkdir( dirPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );
        }
    }
    
    // Acquire exclusive lock for the file
    std::string canonical = getCanonicalPath( resolvedPath );
    if( canonical.empty() )
        canonical = resolvedPath;
    std::shared_mutex& fileLock = getFileLock( canonical );
    std::unique_lock<std::shared_mutex> writeLock( fileLock );

    extractAndTrackSymbolUUIDs( contents );
    
    // Read existing content before overwrite (for commit-based undo snapshots)
    std::string beforeWriteContent;
    {
        wxFileName preWriteFile( resolvedPath );
        wxString preWriteExt = preWriteFile.GetExt();
        if( ( preWriteExt == "trace_sch" || preWriteExt == "trace_pcb" )
            && wxFileExists( resolvedPath ) )
        {
            beforeWriteContent = readFileContent( resolvedPath );
        }
    }

    // Write the file
    if( !writeFileContent( resolvedPath, contents ) )
    {
        wxLogTrace( traceAiToolCall, wxT( "executeWrite: FAILED to write file" ) );
        return AI_TOOL_RESULT( "Error: Failed to write file: " + filePath, false, false );
    }
    
    // Notify callback if set
    if( m_fileModifiedCallback )
        m_fileModifiedCallback( resolvedPath );

    // If a project library table was written, reload it in the library manager
    // so the new entries appear immediately in Manage Libraries without restart.
    {
        wxFileName writtenFn( resolvedPath );
        wxString   name = writtenFn.GetFullName();

        if( name == wxT( "fp-lib-table" ) || name == wxT( "sym-lib-table" ) )
        {
            bool isFp = ( name == wxT( "fp-lib-table" ) );

            if( wxTheApp )
            {
                wxTheApp->CallAfter( [isFp]()
                {
                    if( !PgmOrNull() )
                        return;

                    LIBRARY_TABLE_TYPE type = isFp ? LIBRARY_TABLE_TYPE::FOOTPRINT
                                                   : LIBRARY_TABLE_TYPE::SYMBOL;

                    Pgm().GetLibraryManager().ReloadTables(
                            LIBRARY_TABLE_SCOPE::PROJECT, { type } );

                    wxLogDebug( wxT( "AI: Reloaded project %s library table" ),
                                isFp ? wxT( "footprint" ) : wxT( "symbol" ) );
                } );
            }
        }
    }
    
    // Determine KiCad file path for conversion
    // IMPORTANT: Always compute from the resolved trace file path, not the passed-in aKicadFilePath
    // The passed-in aKicadFilePath is the main file's path, but we may be writing a different file
    // (e.g., creating a new subsheet x.trace_sch should convert to x.kicad_sch, not main.kicad_sch)
    std::string kicadPath = getKicadFilePath( resolvedPath );
    
    // Convert trace to KiCad format if it's a trace file
    wxFileName writtenFile( resolvedPath );
    wxString ext = writtenFile.GetExt();
    
    AI_TOOL_RESULT result;
    result.fileModified = true;
    result.success = true;
    
    if( ext == "trace_sch" || ext == "trace_pcb" )
    {
        CONVERSION_RESULT convResult = syncTraceToKicad( resolvedPath, kicadPath );
        result.conversionLogs = convResult.output;
        
        wxLogTrace( traceAiToolCall, wxT( "executeWrite: conversion result success=%d, timedOut=%d" ),
                    convResult.success, convResult.timedOut );
        
        // Track conversion result for later querying
        m_lastConversionSucceeded.store( convResult.success );
        {
            std::lock_guard<std::mutex> resultLock( m_conversionResultMutex );
            m_lastConversionError = convResult.success ? "" : convResult.errorMessage;
        }
        wxLogDebug( wxT( "AI DEBUG [executeWrite]: Conversion result: success=%s, error=%s" ), 
                   convResult.success ? wxT( "true" ) : wxT( "false" ),
                   wxString::FromUTF8( convResult.errorMessage ) );
        
        if( convResult.timedOut )
        {
            result.result = "File written successfully. Note: Conversion to KiCad format "
                            "timed out (no progress for 30 seconds). Your changes are saved "
                            "in the trace file. Last phase: " + convResult.lastPhase + 
                            ". The conversion may complete on next file save.";
            result.success = true;  // File was written, just conversion stalled
        }
        else if( convResult.success )
        {
            result.result = "File written successfully. Conversion completed.";
        }
        else if( convResult.lintLevel == "kicad_sch" && !beforeWriteContent.empty() )
        {
            // Level 2 failure: generated kicad_sch was malformed.
            // Auto-revert the trace file to the pre-edit state so the editor
            // doesn't load a broken kicad_sch.
            wxLogWarning( wxT( "AI: kicad_sch validation failed in executeWrite, "
                               "reverting trace file to pre-edit state" ) );

            writeFileContent( resolvedPath, beforeWriteContent );
            syncTraceToKicad( resolvedPath, kicadPath );

            result.result = "EDIT REVERTED — the generated kicad_sch was malformed and would "
                            "crash the editor. Your trace_sch edit has been rolled back. "
                            "Validation error: " + convResult.errorMessage +
                            "\nPlease fix the issue and try again with a corrected edit.";
            result.success = false;

            result.lintLevel   = convResult.lintLevel;
            result.lintError   = convResult.errorMessage;
            result.badContent  = convResult.badContentSnippet;
            result.badFilePath = resolvedPath;
        }
        else
        {
            result.result = "File written, but conversion failed: " + convResult.errorMessage;
            result.success = false;
        }

        // Capture snapshot for commit-based undo AFTER conversion so we know the result
        {
            std::lock_guard<std::mutex> snapLock( m_snapshotMutex );
            TRACE_CONTENT_SNAPSHOT snap;
            snap.beforeContent       = beforeWriteContent;
            snap.afterContent        = contents;
            snap.filePath            = resolvedPath;
            snap.kicadFilePath       = kicadPath;
            snap.conversionSucceeded = convResult.success;
            m_pendingSnapshots.push_back( std::move( snap ) );
        }
    }
    else
    {
        result.result = "File written successfully.";
    }
    
    return result;
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSearchReplace( const nlohmann::json& aArgs,
                                                        const std::string&    aProjectDir,
                                                        const std::string&    aDefaultFilePath,
                                                        const std::string&    aKicadFilePath )
{
    // Get file_path parameter
    std::string filePath;
    if( aArgs.contains( "file_path" ) && aArgs["file_path"].is_string() )
        filePath = aArgs["file_path"].get<std::string>();

    // Resolve ${KIPRJMOD} and other environment variables
    if( !filePath.empty() )
        filePath = resolveEnvVars( filePath, aProjectDir );
    
    // Resolve file path
    std::string resolvedPath;
    if( filePath.empty() )
    {
        resolvedPath = aDefaultFilePath;
    }
    else
    {
        wxFileName fn( filePath );
        if( fn.IsAbsolute() )
        {
            resolvedPath = filePath;
        }
        else
        {
            wxFileName resolved( aProjectDir, filePath );
            resolvedPath = resolved.GetFullPath().ToStdString();
        }
    }
    
    if( resolvedPath.empty() )
        return AI_TOOL_RESULT( "Error: No file path specified", false, false );
    
    // Security: Validate file path
    auto [isValid, errorMsg] = validateFilePath( resolvedPath, "write" );
    if( !isValid )
        return AI_TOOL_RESULT( "Security error: " + errorMsg, false, false );
    
    // Get old_string and new_string parameters
    std::string oldString = aArgs.value( "old_string", "" );
    std::string newString = aArgs.value( "new_string", "" );
    bool replaceAll = aArgs.value( "replace_all", false );
    
    wxLogTrace( traceAiToolCall, wxT( "executeSearchReplace: file=%s, oldLen=%zu, newLen=%zu, replaceAll=%d" ),
                resolvedPath, oldString.size(), newString.size(), replaceAll );

    if( oldString.empty() )
        return AI_TOOL_RESULT( "Error: old_string cannot be empty", false, false );
    
    if( oldString == newString )
        return AI_TOOL_RESULT( "Error: old_string and new_string are identical - no change needed",
                               false, false );
    
    // Read file with hash for optimistic concurrency
    auto [content, contentHash] = readFileWithHash( resolvedPath );
    
    if( content.empty() && !wxFileExists( resolvedPath ) )
        return AI_TOOL_RESULT( "Error: File not found: " + resolvedPath, false, false );
    
    // Count occurrences
    size_t count = countOccurrences( content, oldString );
    
    wxLogTrace( traceAiToolCall, wxT( "executeSearchReplace: found %zu occurrences" ), count );

    if( count == 0 )
    {
        return AI_TOOL_RESULT(
                "Error: old_string not found in file. The file may have been modified by "
                "another operation. Please use read_file to read the current content "
                "and retry with the correct text.",
                false, false );
    }
    
    if( count > 1 && !replaceAll )
    {
        return AI_TOOL_RESULT( "Error: old_string found " + std::to_string( count )
                                       + " times in file - must be unique. Add more context to "
                                         "make it unique, or set replace_all=true to replace all occurrences.",
                               false, false );
    }

    extractAndTrackSymbolUUIDs( newString );
    
    // Perform replacement
    std::string newContent = content;
    if( replaceAll )
    {
        // Replace all occurrences
        size_t pos = 0;
        while( ( pos = newContent.find( oldString, pos ) ) != std::string::npos )
        {
            newContent.replace( pos, oldString.length(), newString );
            pos += newString.length();
        }
    }
    else
    {
        // Replace single occurrence
        size_t pos = newContent.find( oldString );
        newContent.replace( pos, oldString.length(), newString );
    }
    
    // Write to file with optimistic concurrency check
    std::string conflictContent;
    if( !writeFileIfUnchanged( resolvedPath, newContent, contentHash, conflictContent ) )
    {
        return AI_TOOL_RESULT( 
            "Error: File was modified by another operation while preparing this edit. "
            "Use read_file to re-read the file, then retry the edit with the current content. "
            "This is a safety feature to prevent data loss from concurrent edits.",
            false, false );
    }
    
    // Compute diff analysis for trace files
    DIFF_RESULT diffInfo;
    bool        hasDiffInfo = false;
    
    if( m_appType == "eeschema" || m_appType == "pcbnew" )
    {
        try
        {
            AI_DIFF_ANALYZER analyzer;
            diffInfo = analyzer.AnalyzeFileDiff( content, newContent );
            hasDiffInfo = true;
        }
        catch( const std::exception& )
        {
            // Continue without diff info - will fall back to full reload
        }
    }
    
    // Notify callback if set
    if( m_fileModifiedCallback )
        m_fileModifiedCallback( resolvedPath );
    
    // Determine KiCad file path for conversion
    // IMPORTANT: Always compute from the resolved trace file path, not the passed-in aKicadFilePath
    // The passed-in aKicadFilePath is the main file's path, but we may be editing a different file
    // (e.g., editing subsheet x.trace_sch should convert to x.kicad_sch, not main.kicad_sch)
    std::string kicadPath = getKicadFilePath( resolvedPath );
    
    // Convert trace to KiCad format
    AI_TOOL_RESULT result;
    result.fileModified = true;
    result.success = true;
    result.diffInfo = diffInfo;
    result.hasDiffInfo = hasDiffInfo;
    
    if( !kicadPath.empty() )
    {
        CONVERSION_RESULT convResult = syncTraceToKicad( resolvedPath, kicadPath );
        result.conversionLogs = convResult.output;
        
        wxLogTrace( traceAiToolCall, wxT( "executeSearchReplace: conversion result success=%d, timedOut=%d" ),
                    convResult.success, convResult.timedOut );
        
        // Track conversion result for later querying
        m_lastConversionSucceeded.store( convResult.success );
        {
            std::lock_guard<std::mutex> resultLock( m_conversionResultMutex );
            m_lastConversionError = convResult.success ? "" : convResult.errorMessage;
        }
        wxLogDebug( wxT( "AI DEBUG [executeSearchReplace]: Conversion result: success=%s, error=%s" ), 
                   convResult.success ? wxT( "true" ) : wxT( "false" ),
                   wxString::FromUTF8( convResult.errorMessage ) );
        
        std::string countMsg = replaceAll ? " (" + std::to_string( count ) + " occurrences)" : "";
        
        if( convResult.timedOut )
        {
            result.result = "Replacement successful" + countMsg + ". Note: Conversion to KiCad format "
                            "timed out (no progress for 30 seconds). Your changes are saved "
                            "in the trace file. Last phase: " + convResult.lastPhase + 
                            ". The conversion may complete on next file save.";
            result.success = true;  // Replacement was successful, just conversion stalled
        }
        else if( convResult.success )
        {
            result.result = "Replacement successful" + countMsg + ". Conversion completed.";
        }
        else if( convResult.lintLevel == "kicad_sch" && !content.empty() )
        {
            // Level 2 failure: generated kicad_sch was malformed.
            // Auto-revert the trace file to the pre-edit state.
            wxLogWarning( wxT( "AI: kicad_sch validation failed in executeSearchReplace, "
                               "reverting trace file to pre-edit state" ) );

            writeFileContent( resolvedPath, content );
            syncTraceToKicad( resolvedPath, kicadPath );

            result.result = "EDIT REVERTED — the generated kicad_sch was malformed and would "
                            "crash the editor. Your trace_sch edit has been rolled back. "
                            "Validation error: " + convResult.errorMessage +
                            "\nPlease fix the issue and try again with a corrected edit.";
            result.success = false;

            result.lintLevel   = convResult.lintLevel;
            result.lintError   = convResult.errorMessage;
            result.badContent  = convResult.badContentSnippet;
            result.badFilePath = resolvedPath;
        }
        else
        {
            result.result = "Replacement successful" + countMsg + ", but conversion failed: " + convResult.errorMessage;
            result.success = false;
        }

        // Capture snapshot for commit-based undo AFTER conversion so we know the result
        {
            wxFileName traceFileName( resolvedPath );
            wxString snapExt = traceFileName.GetExt();
            if( snapExt == "trace_sch" || snapExt == "trace_pcb" )
            {
                std::lock_guard<std::mutex> snapLock( m_snapshotMutex );
                TRACE_CONTENT_SNAPSHOT snap;
                snap.beforeContent       = content;
                snap.afterContent        = newContent;
                snap.filePath            = resolvedPath;
                snap.kicadFilePath       = kicadPath;
                snap.conversionSucceeded = convResult.success;
                m_pendingSnapshots.push_back( std::move( snap ) );
            }
        }
    }
    else
    {
        result.result = "Replacement successful (conversion skipped - no KiCad path)";
    }
    
    return result;
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGrep( const nlohmann::json& aArgs,
                                               const std::string&    aProjectDir,
                                               const std::string&    aDefaultFilePath )
{
    // Get pattern parameter (required)
    std::string pattern = aArgs.value( "pattern", "" );
    if( pattern.empty() )
        return AI_TOOL_RESULT( "Error: pattern is required", false, false );
    
    // Get optional parameters
    std::string path = aArgs.value( "path", "" );
    std::string outputMode = aArgs.value( "output_mode", "content" );
    int contextBefore = aArgs.value( "B", 0 );
    int contextAfter = aArgs.value( "A", 0 );
    int contextBoth = aArgs.value( "C", 0 );
    bool caseInsensitive = aArgs.value( "i", false );
    int headLimit = aArgs.value( "head_limit", -1 );
    
    // C overrides A and B
    if( contextBoth > 0 )
    {
        contextBefore = contextBoth;
        contextAfter = contextBoth;
    }
    
    // Resolve search path
    std::string searchPath;
    if( path.empty() )
    {
        searchPath = aProjectDir;
    }
    else
    {
        wxFileName fn( path );
        if( fn.IsAbsolute() )
        {
            searchPath = path;
        }
        else
        {
            wxFileName resolved( aProjectDir, path );
            searchPath = resolved.GetFullPath().ToStdString();
        }
    }
    
    // Collect files to search
    std::vector<std::string> filesToSearch;
    
    if( wxDirExists( searchPath ) )
    {
        // Search directory - get all trace files
        auto traceFiles = listTraceFilesInDir( searchPath );
        for( const auto& file : traceFiles )
        {
            wxFileName fullPath( searchPath, file );
            filesToSearch.push_back( fullPath.GetFullPath().ToStdString() );
        }
    }
    else if( wxFileExists( searchPath ) )
    {
        // Single file
        filesToSearch.push_back( searchPath );
    }
    else
    {
        return AI_TOOL_RESULT( "Error: Path not found: " + searchPath, false, false );
    }
    
    if( filesToSearch.empty() )
        return AI_TOOL_RESULT( "No files to search in: " + searchPath );
    
    // Compile regex
    std::regex regex;
    try
    {
        std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript;
        if( caseInsensitive )
            flags |= std::regex_constants::icase;
        regex = std::regex( pattern, flags );
    }
    catch( const std::regex_error& e )
    {
        return AI_TOOL_RESULT( "Error: Invalid regex pattern: " + std::string( e.what() ), false, false );
    }
    
    // Search files
    std::ostringstream output;
    int totalMatches = 0;
    int filesWithMatches = 0;
    
    for( const auto& filePath : filesToSearch )
    {
        // Security check
        auto [isValid, errorMsg] = validateFilePath( filePath, "read" );
        if( !isValid )
            continue;
        
        std::string content = readFileContent( filePath );
        if( content.empty() )
            continue;
        
        // Split into lines
        std::vector<std::string> lines;
        std::istringstream stream( content );
        std::string line;
        while( std::getline( stream, line ) )
            lines.push_back( line );
        
        // Find matching lines
        std::vector<int> matchingLines;
        for( size_t i = 0; i < lines.size(); i++ )
        {
            if( std::regex_search( lines[i], regex ) )
            {
                matchingLines.push_back( static_cast<int>( i + 1 ) );
            }
        }
        
        if( matchingLines.empty() )
            continue;
        
        filesWithMatches++;
        totalMatches += matchingLines.size();
        
        // Get relative filename for display
        wxFileName fn( filePath );
        std::string displayName = fn.GetFullName().ToStdString();
        
        if( outputMode == "files_with_matches" )
        {
            output << displayName << "\n";
            if( headLimit > 0 && filesWithMatches >= headLimit )
                break;
        }
        else if( outputMode == "count" )
        {
            output << displayName << ":" << matchingLines.size() << "\n";
            if( headLimit > 0 && filesWithMatches >= headLimit )
                break;
        }
        else  // content mode
        {
            output << displayName << "\n";
            
            int shown = 0;
            int lastEndLine = -1;
            
            for( int lineNum : matchingLines )
            {
                if( headLimit > 0 && shown >= headLimit )
                    break;
                
                int startContext = std::max( 1, lineNum - contextBefore );
                int endContext = std::min( static_cast<int>( lines.size() ), lineNum + contextAfter );
                
                // Add separator if there's a gap
                if( lastEndLine >= 0 && startContext > lastEndLine + 1 )
                    output << "--\n";
                
                // Avoid overlapping with previous context
                if( startContext <= lastEndLine )
                    startContext = lastEndLine + 1;
                
                for( int i = startContext; i <= endContext; i++ )
                {
                    // Use ':' for match lines, '-' for context lines
                    char separator = ( i == lineNum ) ? ':' : '-';
                    output << i << separator << lines[i - 1] << "\n";
                }
                
                lastEndLine = endContext;
                shown++;
            }
            
            output << "\n";
        }
    }
    
    if( totalMatches == 0 )
        return AI_TOOL_RESULT( "No matches found for pattern: " + pattern );
    
    // Add summary
    std::ostringstream summary;
    summary << "Found " << totalMatches << " match" << ( totalMatches == 1 ? "" : "es" )
            << " in " << filesWithMatches << " file" << ( filesWithMatches == 1 ? "" : "s" ) << "\n\n";
    
    return AI_TOOL_RESULT( summary.str() + output.str() );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeListDir( const nlohmann::json& aArgs,
                                                  const std::string&    aProjectDir )
{
    // Get path parameter (optional, defaults to project dir)
    std::string path = aArgs.value( "path", "" );
    
    // Resolve path
    std::string resolvedPath;
    if( path.empty() )
    {
        resolvedPath = aProjectDir;
    }
    else
    {
        wxFileName fn( path );
        if( fn.IsAbsolute() )
        {
            resolvedPath = path;
        }
        else
        {
            wxFileName resolved( aProjectDir, path );
            resolvedPath = resolved.GetFullPath().ToStdString();
        }
    }
    
    // Check if directory exists
    if( !wxDirExists( resolvedPath ) )
        return AI_TOOL_RESULT( "Error: Directory not found: " + resolvedPath, false, false );
    
    // Get ignore_globs parameter
    std::vector<std::string> ignoreGlobs;
    if( aArgs.contains( "ignore_globs" ) && aArgs["ignore_globs"].is_array() )
    {
        for( const auto& glob : aArgs["ignore_globs"] )
        {
            if( glob.is_string() )
                ignoreGlobs.push_back( glob.get<std::string>() );
        }
    }
    
    // List trace files in directory
    auto files = listTraceFilesInDir( resolvedPath );
    
    // Apply ignore globs (simple pattern matching)
    std::vector<std::string> filteredFiles;
    for( const auto& file : files )
    {
        bool ignored = false;
        for( const auto& glob : ignoreGlobs )
        {
            // Simple glob matching: support * and **
            std::string pattern = glob;
            // Prepend **/ if not already starting with it
            if( pattern.find( "**/" ) != 0 && pattern.find( "*" ) == 0 )
            {
                pattern = "**/" + pattern;
            }
            
            // Convert glob to regex (simplified)
            std::string regexPattern;
            for( char c : pattern )
            {
                if( c == '*' )
                    regexPattern += ".*";
                else if( c == '?' )
                    regexPattern += ".";
                else if( c == '.' || c == '(' || c == ')' || c == '[' || c == ']' || 
                         c == '{' || c == '}' || c == '+' || c == '^' || c == '$' || c == '|' )
                    regexPattern += "\\" + std::string( 1, c );
                else
                    regexPattern += c;
            }
            
            try
            {
                std::regex re( regexPattern, std::regex_constants::icase );
                if( std::regex_match( file, re ) )
                {
                    ignored = true;
                    break;
                }
            }
            catch( const std::regex_error& )
            {
                // Invalid pattern, skip
            }
        }
        
        if( !ignored )
            filteredFiles.push_back( file );
    }
    
    if( filteredFiles.empty() )
        return AI_TOOL_RESULT( "No trace files found in directory" );
    
    nlohmann::json result = nlohmann::json::array();
    for( const auto& file : filteredFiles )
    {
        result.push_back( file );
    }
    
    return AI_TOOL_RESULT( result.dump() );
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


/**
 * Parse the [LINT_ERROR] JSON line from converter output and extract the snippet field.
 * Returns the snippet string, or empty if not found.
 */
static std::string parseLintSnippet( const std::string& aOutput )
{
    const std::string tag = "[LINT_ERROR] ";
    size_t pos = aOutput.find( tag );
    if( pos == std::string::npos )
        return {};

    size_t jsonStart = pos + tag.size();
    size_t jsonEnd   = aOutput.find( '\n', jsonStart );
    std::string jsonStr = ( jsonEnd != std::string::npos )
                              ? aOutput.substr( jsonStart, jsonEnd - jsonStart )
                              : aOutput.substr( jsonStart );

    try
    {
        auto j = nlohmann::json::parse( jsonStr );
        if( j.contains( "snippet" ) && j["snippet"].is_string() )
            return j["snippet"].get<std::string>();
    }
    catch( ... )
    {
    }
    return {};
}


CONVERSION_RESULT AI_TOOL_EXECUTOR::syncTraceToKicad( const std::string& aTraceFilePath,
                                                       const std::string& aKicadFilePath )
{
    wxLogTrace( traceAi, wxT( "syncTraceToKicad: START trace=%s -> kicad=%s" ),
                aTraceFilePath, aKicadFilePath );

    if( !wxFileExists( aTraceFilePath ) )
    {
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: FAILED - trace file not found" ) );
        return CONVERSION_RESULT( false, "Trace file not found: " + aTraceFilePath, "" );
    }


    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    if( pythonPath.IsEmpty() )
    {
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: FAILED - Python interpreter not found" ) );
        return CONVERSION_RESULT( false, "Could not find Python interpreter", "" );
    }

    // Determine which converter to use
    std::string subdir = ( m_appType == "pcbnew" ) ? "pcbnew" : "eeschema";
    std::string fromFormat = ( m_appType == "pcbnew" ) ? "trace_pcb" : "trace_sch";
    std::string toFormat = ( m_appType == "pcbnew" ) ? "kicad_pcb" : "kicad_sch";

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
        return CONVERSION_RESULT( false, "Could not find trace.py script", "" );

    // Build command - use popen for thread safety (wxExecute crashes on non-main thread on macOS)
    wxString existingPcbFlag;
    wxString existingSchFlag;
    wxString kicadSchFlag;
    wxString symbolPathsFlag;
    wxString footprintPathsFlag;
    
    // For eeschema conversions, pass the existing schematic file if it exists
    if( m_appType == "eeschema" && wxFileExists( aKicadFilePath ) )
    {
        existingSchFlag = wxString::Format( wxT( " --existing-sch \"%s\"" ), aKicadFilePath );
    }
    
    // For pcbnew conversions, pass the existing PCB and schematic files
    if( m_appType == "pcbnew" && wxFileExists( aKicadFilePath ) )
    {
        existingPcbFlag = wxString::Format( wxT( " --existing-pcb \"%s\"" ), aKicadFilePath );
        
        // Derive the schematic path from the PCB path
        wxFileName kicadSchFile( aKicadFilePath );
        kicadSchFile.SetExt( wxT( "kicad_sch" ) );
        if( kicadSchFile.FileExists() )
        {
            kicadSchFlag = wxString::Format( wxT( " --kicad-sch \"%s\"" ), 
                                             kicadSchFile.GetFullPath() );
        }
    }
    
    // Collect and pass library paths
    wxString symbolPaths = GetLibraryPaths( wxS( "SYMBOL_DIR" ) );

    // Include the project directory so project-local .kicad_sym libraries
    // (e.g. Trace_Symbols.kicad_sym) are found by the converter.
    wxFileName kicadFile( wxString::FromUTF8( aKicadFilePath ) );
    wxString   projDir = kicadFile.GetPath();
    if( !projDir.IsEmpty() )
    {
        if( symbolPaths.IsEmpty() )
            symbolPaths = projDir;
        else
        {
#ifdef _WIN32
            symbolPaths = projDir + wxT( ";" ) + symbolPaths;
#else
            symbolPaths = projDir + wxT( ":" ) + symbolPaths;
#endif
        }
    }

    if( !symbolPaths.IsEmpty() )
    {
#ifdef _WIN32
        symbolPaths.Replace( wxT("\\"), wxT("/") );
#endif
        symbolPathsFlag = wxString::Format( wxT( " --symbol-paths \"%s\"" ), symbolPaths );
    }
    
    wxString footprintPaths = GetLibraryPaths( wxS( "FOOTPRINT_DIR" ) );

    // Include the project directory so project-local .pretty libraries
    // (e.g. Trace_Footprints.pretty) are found by the converter.
    if( !projDir.IsEmpty() )
    {
        if( footprintPaths.IsEmpty() )
            footprintPaths = projDir;
        else
        {
#ifdef _WIN32
            footprintPaths = projDir + wxT( ";" ) + footprintPaths;
#else
            footprintPaths = projDir + wxT( ":" ) + footprintPaths;
#endif
        }
    }

    if( !footprintPaths.IsEmpty() )
    {
#ifdef _WIN32
        footprintPaths.Replace( wxT("\\"), wxT("/") );
#endif
        footprintPathsFlag = wxString::Format( wxT( " --footprint-paths \"%s\"" ), footprintPaths );
    }
    
    // Build command - use popen for thread safety with platform-specific handling
#ifdef _WIN32
    // Windows: Build command without cmd.exe wrapper to avoid window flash
    wxString pythonCmd = wxString::Format( 
        wxT( "\"%s\" \"%s\" \"%s\" \"%s\" -f %s -t %s%s%s%s%s%s" ),
        pythonPath,
        traceScript.GetFullPath(),
        aTraceFilePath,
        aKicadFilePath,
        fromFormat,
        toFormat,
        existingPcbFlag,
        existingSchFlag,
        kicadSchFlag,
        symbolPathsFlag,
        footprintPathsFlag );


    // Execute with progress-based timeout using Windows API
    PROCESS_RESULT result = ExecuteProcessWithProgress( pythonCmd.ToStdWstring(), 30000 );
    
    if( result.timedOut )
    {
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: TIMEOUT lastPhase=%s" ), result.lastPhase );
        return CONVERSION_RESULT( false, "Conversion timed out (no progress for 30 seconds)", 
                                  result.output, true, result.lastPhase );
    }
    
    if( !result.success && result.exitCode != 0 )
    {
        std::string lintLevel = ( result.exitCode == 2 ) ? "kicad_sch" : "trace_sch";
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: FAILED exitCode=%d lintLevel=%s" ),
                    result.exitCode, lintLevel );
        CONVERSION_RESULT cr( false, "Conversion failed: " + result.output, result.output,
                              false, "", lintLevel, result.exitCode );
        cr.badContentSnippet = parseLintSnippet( result.output );
        return cr;
    }
    
    std::string output = result.output;
    int exitCode = result.exitCode;
    
#else
    // Unix/macOS: Execute with progress-based timeout using fork/exec
    wxString command = wxString::Format( 
        wxT( "\"%s\" \"%s\" \"%s\" \"%s\" -f %s -t %s%s%s%s%s%s 2>&1" ),
        pythonPath,
        traceScript.GetFullPath(),
        aTraceFilePath,
        aKicadFilePath,
        fromFormat,
        toFormat,
        existingPcbFlag,
        existingSchFlag,
        kicadSchFlag,
        symbolPathsFlag,
        footprintPathsFlag );

    std::string output;
    int exitCode = -1;
    bool timedOut = false;
    std::string lastPhase;
    const int stallTimeoutMs = 30000;
    
    int pipefd[2];
    if( pipe( pipefd ) == -1 )
    {
        return CONVERSION_RESULT( false, "Failed to create pipe", "" );
    }
    
    pid_t pid = fork();
    if( pid == -1 )
    {
        close( pipefd[0] );
        close( pipefd[1] );
        return CONVERSION_RESULT( false, "Failed to fork process", "" );
    }
    
    if( pid == 0 )
    {
        // Child process
        close( pipefd[0] );
        dup2( pipefd[1], STDOUT_FILENO );
        dup2( pipefd[1], STDERR_FILENO );
        close( pipefd[1] );
        execl( "/bin/sh", "sh", "-c", command.ToStdString().c_str(), NULL );
        _exit( 127 );
    }
    
    // Parent process
    close( pipefd[1] );
    
    // Register process for cleanup
    PYTHON_PROCESS_MANAGER::GetInstance().RegisterProcess( 
        wxTheApp ? wxTheApp->GetAppName() : wxT("trace"), pid );
    
    // Non-blocking read with select()
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
        tv.tv_usec = 100000;  // 100ms poll interval
        
        int ret = select( pipefd[0] + 1, &readfds, NULL, NULL, &tv );
        
        if( ret > 0 )
        {
            ssize_t bytesRead = read( pipefd[0], buffer, sizeof( buffer ) - 1 );
            if( bytesRead > 0 )
            {
                buffer[bytesRead] = '\0';
                output += buffer;
                lastActivityTime = std::chrono::steady_clock::now();
                
                // Extract last phase for debugging
                std::string chunk( buffer );
                size_t phasePos = chunk.rfind( "[PHASE]" );
                if( phasePos != std::string::npos )
                {
                    size_t endPos = chunk.find( '\n', phasePos );
                    if( endPos != std::string::npos )
                    {
                        lastPhase = chunk.substr( phasePos + 8, endPos - phasePos - 8 );
                    }
                    else
                    {
                        lastPhase = chunk.substr( phasePos + 8 );
                    }
                    // Trim whitespace
                    while( !lastPhase.empty() && 
                           ( lastPhase.back() == '\r' || lastPhase.back() == '\n' ) )
                    {
                        lastPhase.pop_back();
                    }
                }
            }
            else if( bytesRead == 0 )
            {
                // EOF - pipe closed
                processRunning = false;
            }
        }
        else if( ret == 0 )
        {
            // Timeout - check for stall
            auto elapsed = std::chrono::steady_clock::now() - lastActivityTime;
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count();
            
            if( elapsedMs > stallTimeoutMs )
            {
                // Process stalled - terminate it
                std::cerr << "syncTraceToKicad: Process stalled for " << elapsedMs 
                          << "ms, terminating. Last phase: " << lastPhase << std::endl;
                kill( pid, SIGTERM );
                usleep( 100000 );  // 100ms grace period
                kill( pid, SIGKILL );
                timedOut = true;
                processRunning = false;
            }
            else
            {
                // Check if child has exited
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
            // Error in select
            processRunning = false;
        }
    }
    
    // Final wait if not already done
    if( !timedOut && exitCode == -1 )
    {
        int status;
        waitpid( pid, &status, 0 );
        exitCode = WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
    }
    
    close( pipefd[0] );
    PYTHON_PROCESS_MANAGER::GetInstance().UnregisterProcess( pid );
    
    if( timedOut )
    {
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: TIMEOUT lastPhase=%s" ), lastPhase );
        return CONVERSION_RESULT( false, "Conversion timed out (no progress for 30 seconds)", 
                                  output, true, lastPhase );
    }
#endif

    // Always capture output, whether success or failure
    if( exitCode != 0 )
    {
        std::string lintLevel = ( exitCode == 2 ) ? "kicad_sch" : "trace_sch";
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: FAILED exitCode=%d lintLevel=%s" ),
                    exitCode, lintLevel );
        CONVERSION_RESULT cr( false, "Conversion failed: " + output, output,
                              false, "", lintLevel, exitCode );
        cr.badContentSnippet = parseLintSnippet( output );
        return cr;
    }

    // Verify output file exists
    if( !wxFileExists( aKicadFilePath ) )
    {
        wxLogTrace( traceAi, wxT( "syncTraceToKicad: FAILED - KiCad file not created" ) );
        return CONVERSION_RESULT( false, "Conversion completed but KiCad file was not created", output );
    }
    
    wxLogTrace( traceAi, wxT( "syncTraceToKicad: SUCCESS" ) );
    return CONVERSION_RESULT( true, "", output );
}


std::string AI_TOOL_EXECUTOR::getKicadFilePath( const std::string& aTraceFilePath )
{
    wxFileName path( aTraceFilePath );
    wxString   ext = path.GetExt();

    if( ext == "trace_sch" )
        path.SetExt( "kicad_sch" );
    else if( ext == "trace_pcb" )
        path.SetExt( "kicad_pcb" );

    return path.GetFullPath().ToStdString();
}


std::vector<std::string> AI_TOOL_EXECUTOR::listTraceFilesInDir( const std::string& aProjectDir )
{
    std::vector<std::string> files;
    
    wxDir dir( aProjectDir );
    if( !dir.IsOpened() )
        return files;
    
    wxString filename;
    
    // For pcbnew, list both trace_pcb AND trace_sch files (for reading schematic context)
    if( m_appType == "pcbnew" )
    {
        // List trace_pcb files first
        bool found = dir.GetFirst( &filename, wxT( "*.trace_pcb" ), wxDIR_FILES );
        while( found )
        {
            files.push_back( filename.ToStdString() );
            found = dir.GetNext( &filename );
        }
        
        // Also list trace_sch files (for reading schematic context)
        found = dir.GetFirst( &filename, wxT( "*.trace_sch" ), wxDIR_FILES );
        while( found )
        {
            files.push_back( filename.ToStdString() );
            found = dir.GetNext( &filename );
        }
    }
    else
    {
        // Eeschema: only trace_sch files
        bool found = dir.GetFirst( &filename, wxT( "*.trace_sch" ), wxDIR_FILES );
        while( found )
        {
            files.push_back( filename.ToStdString() );
            found = dir.GetNext( &filename );
        }
    }
    
    // Sort for consistent ordering
    std::sort( files.begin(), files.end() );
    
    return files;
}


std::string AI_TOOL_EXECUTOR::resolveFilePath( const nlohmann::json& aArgs,
                                                const std::string&    aProjectDir,
                                                const std::string&    aDefaultFilePath,
                                                bool&                 outIsMultiFile )
{
    auto files = listTraceFilesInDir( aProjectDir );
    outIsMultiFile = files.size() > 1;
    
    // Check if filename is provided in args (support both "filename" and "file_name")
    std::string filename;
    if( aArgs.contains( "filename" ) && aArgs["filename"].is_string() )
    {
        filename = aArgs["filename"].get<std::string>();
    }
    else if( aArgs.contains( "file_name" ) && aArgs["file_name"].is_string() )
    {
        filename = aArgs["file_name"].get<std::string>();
    }
    
    if( !filename.empty() )
    {
        wxFileName resolved( aProjectDir, filename );
        return resolved.GetFullPath().ToStdString();
    }
    
    // For single-file projects, use default
    if( files.size() <= 1 )
        return aDefaultFilePath;
    
    // Multi-file without filename specified - return empty to signal error
    return "";
}


std::string AI_TOOL_EXECUTOR::copyFileHeaders( const std::string& aSourceFilePath )
{
    std::string content = readFileContent( aSourceFilePath );
    if( content.empty() )
        return "";
    
    std::ostringstream headers;
    std::istringstream stream( content );
    std::string        line;
    
    // Extract header lines (kicad_ver, kicad_gen, kicad_gen_ver, paper)
    // Skip file_uid as it will be regenerated
    while( std::getline( stream, line ) )
    {
        // Check if line starts with a header keyword
        if( line.find( "kicad_ver " ) == 0 ||
            line.find( "kicad_gen " ) == 0 ||
            line.find( "kicad_gen_ver " ) == 0 ||
            line.find( "paper " ) == 0 )
        {
            headers << line << "\n";
        }
        
        // Stop after paper line (headers are at the beginning)
        if( line.find( "paper " ) == 0 )
            break;
    }
    
    return headers.str();
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeDeleteTraceFile( const nlohmann::json& aArgs,
                                                          const std::string&    aProjectDir,
                                                          const std::string&    aMainFilePath )
{
    // Get required filename argument (support both "filename" and "file_name")
    std::string filename;
    if( aArgs.contains( "filename" ) && aArgs["filename"].is_string() )
        filename = aArgs["filename"].get<std::string>();
    else if( aArgs.contains( "file_name" ) && aArgs["file_name"].is_string() )
        filename = aArgs["file_name"].get<std::string>();
    
    if( filename.empty() )
        return AI_TOOL_RESULT( "Error: filename is required for delete_trace_file", false, false );
    
    // Resolve full path
    wxFileName targetFile( aProjectDir, filename );
    std::string targetPath = targetFile.GetFullPath().ToStdString();
    
    // Security: Validate file path before deletion
    auto [isValid, errorMsg] = validateFilePath( targetPath, "delete" );
    if( !isValid )
    {
        return AI_TOOL_RESULT( "Security error: " + errorMsg, false, false );
    }
    
    // Acquire exclusive lock for the file to prevent race conditions
    std::string canonical = getCanonicalPath( targetPath );
    if( canonical.empty() )
        canonical = targetPath;
    std::shared_mutex& fileLock = getFileLock( canonical );
    std::unique_lock<std::shared_mutex> writeLock( fileLock );
    
    // Check if file exists (with lock held to prevent TOCTOU race)
    if( !wxFileExists( targetPath ) )
        return AI_TOOL_RESULT( "Error: File not found: " + filename, false, false );
    
    // Check if trying to delete the main file
    wxFileName mainFile( aMainFilePath );
    if( targetFile.GetFullPath() == mainFile.GetFullPath() )
        return AI_TOOL_RESULT( "Error: Cannot delete the main/root schematic file", false, false );
    
    // Request user confirmation via callback
    if( !m_confirmationCallback )
    {
        return AI_TOOL_RESULT( "Error: No confirmation handler set", false, false );
    }
    
    
    // Call the confirmation callback and wait for the result
    std::future<bool> confirmFuture = m_confirmationCallback( filename );
    bool confirmed = false;
    
    try
    {
        confirmed = confirmFuture.get();  // Blocks until UI responds
    }
    catch( const std::exception& e )
    {
        return AI_TOOL_RESULT( "Error: Confirmation failed: " + std::string( e.what() ), false, false );
    }
    
    if( !confirmed )
    {
        return AI_TOOL_RESULT( "Delete cancelled by user", false, false );
    }
        
        // Delete the trace file
        if( !wxRemoveFile( targetPath ) )
            return AI_TOOL_RESULT( "Error: Failed to delete trace file: " + filename, false, false );
        
        // Delete the corresponding KiCad file
        std::string kicadPath = getKicadFilePath( targetPath );
        if( wxFileExists( kicadPath ) )
        {
            if( wxRemoveFile( kicadPath ) )
            {
                // KiCad file deleted successfully
            }
            else
            {
                return AI_TOOL_RESULT( "Deleted " + filename + " but failed to delete corresponding KiCad file", 
                                       true, true );
            }
        }
        
        // Notify callback if set
        if( m_fileModifiedCallback )
            m_fileModifiedCallback( targetPath );
        
        return AI_TOOL_RESULT( "Successfully deleted " + filename + " and corresponding KiCad file", 
                               true, true );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeRunDrc( const nlohmann::json& aArgs,
                                                 const std::string&    aKicadFilePath )
{

    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "run_drc only available in PCB editor (pcbnew)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_drcCallback )
    {
        nlohmann::json error;
        error["error"] = "DRC callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Call the callback to get violations directly from the editor
    try
    {
        nlohmann::json violations = m_drcCallback();

        // Format response
        nlohmann::json response;
        response["violations"] = violations;
        response["count"] = violations.size();
        if( !aKicadFilePath.empty() )
            response["board_file"] = aKicadFilePath;

        return AI_TOOL_RESULT( response.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to get DRC violations: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeRunErc( const nlohmann::json& aArgs,
                                                 const std::string&    aKicadFilePath )
{

    if( m_appType != "eeschema" )
    {
        nlohmann::json error;
        error["error"] = "run_erc only available in schematic editor (eeschema)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_ercCallback )
    {
        nlohmann::json error;
        error["error"] = "ERC callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Call the callback to get violations directly from the editor
    try
    {
        nlohmann::json violations = m_ercCallback();

        // Format response
        nlohmann::json response;
        response["violations"] = violations;
        response["count"] = violations.size();
        if( !aKicadFilePath.empty() )
            response["schematic_file"] = aKicadFilePath;

        return AI_TOOL_RESULT( response.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to get ERC violations: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeRunAnnotate( const nlohmann::json& aArgs,
                                                       const std::string&    aKicadFilePath )
{
    
    if( m_appType != "eeschema" )
    {
        return AI_TOOL_RESULT( 
            "{\"error\": \"run_annotate only available in schematic editor\"}",
            false, true );
    }

    if( !m_annotateCallback )
    {
        return AI_TOOL_RESULT( 
            "{\"error\": \"Annotate callback not set\"}", 
            false, true );
    }

    try
    {
        nlohmann::json result = m_annotateCallback( aArgs );
        
        if( result.contains( "error" ) )
        {
            return AI_TOOL_RESULT( result.dump( 2 ), false, true );
        }
        
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Annotation failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, true );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGenerateGerbers( const nlohmann::json& aArgs,
                                                          const std::string&    aKicadFilePath )
{
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "generate_gerbers only available in PCB editor (pcbnew)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_gerberCallback )
    {
        nlohmann::json error;
        error["error"] = "Gerber callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        // Build JSON object with only provided parameters (omit missing ones)
        // All parameters are optional - callback will use KiCad defaults
        nlohmann::json params;
        
        if( aArgs.contains( "output_directory" ) && !aArgs["output_directory"].is_null() )
            params["output_directory"] = aArgs["output_directory"];
        if( aArgs.contains( "layers" ) && !aArgs["layers"].is_null() )
            params["layers"] = aArgs["layers"];
        if( aArgs.contains( "common_layers" ) && !aArgs["common_layers"].is_null() )
            params["common_layers"] = aArgs["common_layers"];
        if( aArgs.contains( "precision" ) && !aArgs["precision"].is_null() )
            params["precision"] = aArgs["precision"];
        if( aArgs.contains( "use_x2_format" ) && !aArgs["use_x2_format"].is_null() )
            params["use_x2_format"] = aArgs["use_x2_format"];
        if( aArgs.contains( "include_netlist" ) && !aArgs["include_netlist"].is_null() )
            params["include_netlist"] = aArgs["include_netlist"];
        if( aArgs.contains( "disable_aperture_macros" ) && !aArgs["disable_aperture_macros"].is_null() )
            params["disable_aperture_macros"] = aArgs["disable_aperture_macros"];
        if( aArgs.contains( "use_protel_extension" ) && !aArgs["use_protel_extension"].is_null() )
            params["use_protel_extension"] = aArgs["use_protel_extension"];
        if( aArgs.contains( "check_zones_before_plot" ) && !aArgs["check_zones_before_plot"].is_null() )
            params["check_zones_before_plot"] = aArgs["check_zones_before_plot"];
        if( aArgs.contains( "use_board_plot_params" ) && !aArgs["use_board_plot_params"].is_null() )
            params["use_board_plot_params"] = aArgs["use_board_plot_params"];
        if( aArgs.contains( "create_jobs_file" ) && !aArgs["create_jobs_file"].is_null() )
            params["create_jobs_file"] = aArgs["create_jobs_file"];
        if( aArgs.contains( "sketch_pads_on_fab_layers" ) && !aArgs["sketch_pads_on_fab_layers"].is_null() )
            params["sketch_pads_on_fab_layers"] = aArgs["sketch_pads_on_fab_layers"];
        if( aArgs.contains( "hide_dnp_fps_on_fab_layers" ) && !aArgs["hide_dnp_fps_on_fab_layers"].is_null() )
            params["hide_dnp_fps_on_fab_layers"] = aArgs["hide_dnp_fps_on_fab_layers"];
        if( aArgs.contains( "sketch_dnp_fps_on_fab_layers" ) && !aArgs["sketch_dnp_fps_on_fab_layers"].is_null() )
            params["sketch_dnp_fps_on_fab_layers"] = aArgs["sketch_dnp_fps_on_fab_layers"];
        if( aArgs.contains( "crossout_dnp_fps_on_fab_layers" ) && !aArgs["crossout_dnp_fps_on_fab_layers"].is_null() )
            params["crossout_dnp_fps_on_fab_layers"] = aArgs["crossout_dnp_fps_on_fab_layers"];
        if( aArgs.contains( "plot_footprint_values" ) && !aArgs["plot_footprint_values"].is_null() )
            params["plot_footprint_values"] = aArgs["plot_footprint_values"];
        if( aArgs.contains( "plot_ref_des" ) && !aArgs["plot_ref_des"].is_null() )
            params["plot_ref_des"] = aArgs["plot_ref_des"];
        if( aArgs.contains( "plot_drawing_sheet" ) && !aArgs["plot_drawing_sheet"].is_null() )
            params["plot_drawing_sheet"] = aArgs["plot_drawing_sheet"];
        if( aArgs.contains( "subtract_solder_mask_from_silk" ) && !aArgs["subtract_solder_mask_from_silk"].is_null() )
            params["subtract_solder_mask_from_silk"] = aArgs["subtract_solder_mask_from_silk"];
        if( aArgs.contains( "use_drill_origin" ) && !aArgs["use_drill_origin"].is_null() )
            params["use_drill_origin"] = aArgs["use_drill_origin"];

        nlohmann::json result = m_gerberCallback( params );
        
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to generate Gerber files: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGenerateDrill( const nlohmann::json& aArgs,
                                                        const std::string&    aKicadFilePath )
{
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "generate_drill_files only available in PCB editor (pcbnew)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_drillCallback )
    {
        nlohmann::json error;
        error["error"] = "Drill callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        // Build JSON object with only provided parameters (omit missing ones)
        // All parameters are optional - callback will use KiCad defaults
        nlohmann::json params;
        
        if( aArgs.contains( "output_directory" ) && !aArgs["output_directory"].is_null() )
            params["output_directory"] = aArgs["output_directory"];
        if( aArgs.contains( "format" ) && !aArgs["format"].is_null() )
            params["format"] = aArgs["format"];
        if( aArgs.contains( "drill_origin" ) && !aArgs["drill_origin"].is_null() )
            params["drill_origin"] = aArgs["drill_origin"];
        if( aArgs.contains( "units" ) && !aArgs["units"].is_null() )
            params["units"] = aArgs["units"];
        if( aArgs.contains( "zeros_format" ) && !aArgs["zeros_format"].is_null() )
            params["zeros_format"] = aArgs["zeros_format"];
        if( aArgs.contains( "excellon_mirror_y" ) && !aArgs["excellon_mirror_y"].is_null() )
            params["excellon_mirror_y"] = aArgs["excellon_mirror_y"];
        if( aArgs.contains( "excellon_minimal_header" ) && !aArgs["excellon_minimal_header"].is_null() )
            params["excellon_minimal_header"] = aArgs["excellon_minimal_header"];
        if( aArgs.contains( "excellon_separate_th" ) && !aArgs["excellon_separate_th"].is_null() )
            params["excellon_separate_th"] = aArgs["excellon_separate_th"];
        if( aArgs.contains( "excellon_oval_format" ) && !aArgs["excellon_oval_format"].is_null() )
            params["excellon_oval_format"] = aArgs["excellon_oval_format"];
        if( aArgs.contains( "generate_map" ) && !aArgs["generate_map"].is_null() )
            params["generate_map"] = aArgs["generate_map"];
        if( aArgs.contains( "map_format" ) && !aArgs["map_format"].is_null() )
            params["map_format"] = aArgs["map_format"];
        if( aArgs.contains( "generate_tenting" ) && !aArgs["generate_tenting"].is_null() )
            params["generate_tenting"] = aArgs["generate_tenting"];
        if( aArgs.contains( "gerber_precision" ) && !aArgs["gerber_precision"].is_null() )
            params["gerber_precision"] = aArgs["gerber_precision"];

        nlohmann::json result = m_drillCallback( params );
        
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to generate drill files: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGeneratePositionFile( const nlohmann::json& aArgs,
                                                               const std::string&    aKicadFilePath )
{
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "generate_position_file only available in PCB editor (pcbnew)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_positionFileCallback )
    {
        nlohmann::json error;
        error["error"] = "Position file callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json params;

        if( aArgs.contains( "output_directory" ) && !aArgs["output_directory"].is_null() )
            params["output_directory"] = aArgs["output_directory"];
        if( aArgs.contains( "side" ) && !aArgs["side"].is_null() )
            params["side"] = aArgs["side"];
        if( aArgs.contains( "format" ) && !aArgs["format"].is_null() )
            params["format"] = aArgs["format"];
        if( aArgs.contains( "smd_only" ) && !aArgs["smd_only"].is_null() )
            params["smd_only"] = aArgs["smd_only"];
        if( aArgs.contains( "exclude_dnp" ) && !aArgs["exclude_dnp"].is_null() )
            params["exclude_dnp"] = aArgs["exclude_dnp"];
        if( aArgs.contains( "exclude_th" ) && !aArgs["exclude_th"].is_null() )
            params["exclude_th"] = aArgs["exclude_th"];
        if( aArgs.contains( "use_drill_origin" ) && !aArgs["use_drill_origin"].is_null() )
            params["use_drill_origin"] = aArgs["use_drill_origin"];

        nlohmann::json result = m_positionFileCallback( params );

        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to generate position file: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeZipGerberFiles( const std::string& aFilePath )
{
    // Get project directory from file path
    wxFileName filePath( aFilePath );
    std::string projectDir = filePath.GetPath().ToStdString();
    
    if( projectDir.empty() )
    {
        nlohmann::json error;
        error["error"] = "Could not determine project directory from file path";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Scan project directory for .gbr and .drl files
    wxDir dir( projectDir );
    if( !dir.IsOpened() )
    {
        nlohmann::json error;
        error["error"] = "Could not open project directory: " + projectDir;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    std::vector<std::string> filesToZip;
    wxString filename;
    
    // Find all files and filter for .gbr and .drl extensions
    bool found = dir.GetFirst( &filename, wxT( "*" ), wxDIR_FILES );
    while( found )
    {
        wxString ext = filename.AfterLast( '.' ).Lower();
        if( ext == wxT( "gbr" ) || ext == wxT( "drl" ) )
        {
            wxFileName file( projectDir, filename );
            std::string fullPath = file.GetFullPath().ToStdString();
            
            // Validate file path
            auto [isValid, errorMsg] = validateFilePath( fullPath, "read" );
            if( isValid )
            {
                filesToZip.push_back( fullPath );
            }
            else
            {
                // Skipping invalid file
            }
        }
        
        found = dir.GetNext( &filename );
    }

    if( filesToZip.empty() )
    {
        nlohmann::json error;
        error["error"] = "No .gbr or .drl files found in project directory: " + projectDir;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Generate default zip filename based on project directory name
    wxFileName projectDirName( projectDir, wxEmptyString );
    std::string zipFilename;
    
    // Get the last directory component
    const wxArrayString& dirs = projectDirName.GetDirs();
    if( !dirs.IsEmpty() )
    {
        zipFilename = dirs.Last().ToStdString();
    }
    
    if( zipFilename.empty() )
    {
        // Fallback: use "gerbers" if we can't determine directory name
        zipFilename = "gerbers";
    }
    zipFilename += "_gerbers.zip";
    
    wxFileName zipPath( projectDir, zipFilename );
    std::string zipFilePath = zipPath.GetFullPath().ToStdString();
    
    // Validate zip file path
    auto [zipValid, zipError] = validateFilePath( zipFilePath, "write" );
    if( !zipValid )
    {
        nlohmann::json error;
        error["error"] = "Invalid zip file path: " + zipError;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        // Create zip file
        wxFFileOutputStream outputStream( zipFilePath );
        if( !outputStream.IsOk() )
        {
            nlohmann::json error;
            error["error"] = "Failed to create zip file: " + zipFilePath;
            return AI_TOOL_RESULT( error.dump( 2 ), false, false );
        }

        wxZipOutputStream zipStream( outputStream, -1, wxConvUTF8 );
        
        // Add each file to the zip
        for( const auto& fileToZip : filesToZip )
        {
            wxFileName file( fileToZip );
            file.MakeRelativeTo( projectDir );
            wxString relativePath = file.GetFullPath();
            
            // Read file content
            wxFFileInputStream fileStream( fileToZip );
            if( !fileStream.IsOk() )
            {
                continue;
            }
            
            // Add to zip
            zipStream.PutNextEntry( relativePath );
            zipStream.Write( fileStream );
            zipStream.CloseEntry();
        }
        
        zipStream.Close();
        outputStream.Close();

        // Build result JSON
        nlohmann::json result;
        result["success"] = true;
        result["zip_path"] = zipFilePath;
        result["files_included"] = filesToZip.size();
        result["files"] = nlohmann::json::array();
        for( const auto& file : filesToZip )
        {
            wxFileName fn( file );
            result["files"].push_back( fn.GetFullName().ToStdString() );
        }

        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to create zip file: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeAutoroute( const nlohmann::json& aArgs,
                                                    const std::string&    aKicadFilePath )
{
    // Autoroute is only valid for pcbnew
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "Autoroute tool is only available in PCB editor (pcbnew)";
        error["success"] = false;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_autorouteCallback )
    {
        nlohmann::json error;
        error["error"] = "Autoroute callback not available. This feature requires the PCB editor.";
        error["success"] = false;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Extract params from tool arguments (default to empty object if not provided)
    nlohmann::json params = aArgs.value( "params", nlohmann::json::object() );

    // Build the callback input
    nlohmann::json callbackInput;
    callbackInput["params"] = params;

    // Call the autoroute callback
    // The callback handles: DSN export, API call, SSE streaming, SES import, and board save
    nlohmann::json result = m_autorouteCallback( callbackInput );

    // Check if the board was modified (autorouting imports new traces)
    bool fileModified = result.value( "success", false );

    // Return the result to the AI
    return AI_TOOL_RESULT( result.dump( 2 ), fileModified, result.value( "success", false ) );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeUpdatePcbFromSchematic( const nlohmann::json& aArgs,
                                                                 const std::string&    aKicadFilePath )
{
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "update_pcb_from_schematic is only available in PCB editor (pcbnew)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_updatePcbFromSchematicCallback )
    {
        nlohmann::json error;
        error["error"] = "Update PCB from schematic callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_updatePcbFromSchematicCallback( aArgs );

        bool fileModified = result.value( "success", false );

        return AI_TOOL_RESULT( result.dump( 2 ), fileModified, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Update PCB from schematic failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGetHierarchy( const nlohmann::json& aArgs,
                                                       const std::string&    aKicadFilePath )
{
    // get_hierarchy is only valid for eeschema
    if( m_appType != "eeschema" )
    {
        nlohmann::json error;
        error["error"] = "get_hierarchy tool is only available in schematic editor (eeschema)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_hierarchyCallback )
    {
        nlohmann::json error;
        error["error"] = "Hierarchy callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_hierarchyCallback();
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to get hierarchy: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSwitchSheet( const nlohmann::json& aArgs,
                                                      const std::string&    aKicadFilePath )
{
    // switch_sheet is only valid for eeschema
    if( m_appType != "eeschema" )
    {
        nlohmann::json error;
        error["error"] = "switch_sheet tool is only available in schematic editor (eeschema)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_switchSheetCallback )
    {
        nlohmann::json error;
        error["error"] = "Switch sheet callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Validate at least one identifier is provided
    bool hasPageNumber = aArgs.contains( "page_number" ) && !aArgs["page_number"].is_null();
    bool hasSheetName = aArgs.contains( "sheet_name" ) && !aArgs["sheet_name"].is_null();
    bool hasSheetPath = aArgs.contains( "sheet_path" ) && !aArgs["sheet_path"].is_null();

    if( !hasPageNumber && !hasSheetName && !hasSheetPath )
    {
        nlohmann::json error;
        error["error"] = "At least one of page_number, sheet_name, or sheet_path is required";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_switchSheetCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to switch sheet: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSwitchLayer( const nlohmann::json& aArgs,
                                                      const std::string&    aKicadFilePath )
{
    // switch_layer is only valid for pcbnew
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "switch_layer tool is only available in PCB editor (pcbnew)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_switchLayerCallback )
    {
        nlohmann::json error;
        error["error"] = "Switch layer callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Validate at least one identifier is provided
    bool hasLayerName = aArgs.contains( "layer_name" ) && !aArgs["layer_name"].is_null();
    bool hasLayerId = aArgs.contains( "layer_id" ) && !aArgs["layer_id"].is_null();

    if( !hasLayerName && !hasLayerId )
    {
        nlohmann::json error;
        error["error"] = "At least one of layer_name or layer_id is required";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_switchLayerCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to switch layer: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeGetLayers( const nlohmann::json& aArgs,
                                                    const std::string&    aKicadFilePath )
{
    // get_layers is only valid for pcbnew
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "get_layers tool is only available in PCB editor (pcbnew)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_layersCallback )
    {
        nlohmann::json error;
        error["error"] = "Layers callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_layersCallback();
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to get layers: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeApplyLayerPreset( const nlohmann::json& aArgs,
                                                           const std::string&    aKicadFilePath )
{
    // apply_layer_preset is only valid for pcbnew
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "apply_layer_preset tool is only available in PCB editor (pcbnew)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_layerPresetCallback )
    {
        nlohmann::json error;
        error["error"] = "Layer preset callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Validate preset_name is provided
    if( !aArgs.contains( "preset_name" ) || aArgs["preset_name"].is_null() )
    {
        nlohmann::json error;
        error["error"] = "preset_name is required";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_layerPresetCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to apply layer preset: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSetLayerVisibility( const nlohmann::json& aArgs,
                                                             const std::string&    aKicadFilePath )
{
    // set_layer_visibility is only valid for pcbnew
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "set_layer_visibility tool is only available in PCB editor (pcbnew)";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Check if callback is available
    if( !m_layerVisibilityCallback )
    {
        nlohmann::json error;
        error["error"] = "Layer visibility callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    // Validate at least one identifier is provided
    bool hasLayerName = aArgs.contains( "layer_name" ) && !aArgs["layer_name"].is_null();
    bool hasLayerId = aArgs.contains( "layer_id" ) && !aArgs["layer_id"].is_null();

    if( !hasLayerName && !hasLayerId )
    {
        nlohmann::json error;
        error["error"] = "At least one of layer_name or layer_id is required";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_layerVisibilityCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to set layer visibility: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


// =============================================================================
// Fetch Dimensions Tool
// =============================================================================

AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeFetchDimensions( const nlohmann::json& aArgs,
                                                         const std::string&    aKicadFilePath )
{
    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "fetch_dimensions is only available in PCB editor (pcbnew)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_fetchDimensionsCallback )
    {
        nlohmann::json error;
        error["error"] = "Fetch dimensions callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !aArgs.contains( "footprints" ) || !aArgs["footprints"].is_array() )
    {
        nlohmann::json error;
        error["error"] = "'footprints' array is required";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_fetchDimensionsCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to fetch dimensions: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


// =============================================================================
// Fetch Component Pins
// =============================================================================

AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeFetchComponentPins( const nlohmann::json& aArgs )
{
    if( m_appType != "eeschema" )
    {
        nlohmann::json error;
        error["error"] = "fetch_component_pins is only available in the schematic editor (eeschema)";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_fetchPinsCallback )
    {
        nlohmann::json error;
        error["error"] = "Fetch pins callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    std::string symbolId;

    if( aArgs.contains( "symbol" ) && aArgs["symbol"].is_string() )
        symbolId = aArgs["symbol"].get<std::string>();
    else
    {
        nlohmann::json error;
        error["error"] = "'symbol' string argument is required (e.g. \"Device:R\")";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_fetchPinsCallback( symbolId );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Failed to fetch pins: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


// =============================================================================
// Todo Tools
// =============================================================================

AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeTodoWrite( const nlohmann::json& aArgs )
{
    if( m_conversationId.empty() )
        return AI_TOOL_RESULT( "Error: No conversation ID set for todo operations", false, false );

    bool merge = aArgs.value( "merge", false );

    if( !aArgs.contains( "todos" ) || !aArgs["todos"].is_array() )
        return AI_TOOL_RESULT( "Error: 'todos' array is required", false, false );

    std::vector<TODO_ITEM> items;
    for( const auto& todoJson : aArgs["todos"] )
    {
        TODO_ITEM item;
        item.id = wxString::FromUTF8( todoJson.value( "id", "" ) );
        item.content = wxString::FromUTF8( todoJson.value( "content", "" ) );
        item.status = wxString::FromUTF8( todoJson.value( "status", "pending" ) );

        if( item.content.IsEmpty() )
            continue;

        if( item.id.IsEmpty() )
            item.id = CONVERSATION_DB::GenerateUUID();

        items.push_back( item );
    }

    if( items.empty() )
        return AI_TOOL_RESULT( "Error: No valid todo items provided", false, false );

    wxString convId = wxString::FromUTF8( m_conversationId );
    bool ok = CONVERSATION_DB::Instance().SaveOrUpdateTodos( convId, items, merge );

    if( !ok )
        return AI_TOOL_RESULT( "Error: Failed to save todos to database", false, false );

    // Return the full updated list
    auto allTodos = CONVERSATION_DB::Instance().LoadTodos( convId );
    nlohmann::json result = nlohmann::json::array();
    for( const auto& t : allTodos )
    {
        result.push_back( {
            { "id", t.id.ToStdString() },
            { "content", t.content.ToStdString() },
            { "status", t.status.ToStdString() }
        } );
    }

    return AI_TOOL_RESULT( result.dump( 2 ) );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeTodoRead( const nlohmann::json& aArgs )
{
    if( m_conversationId.empty() )
        return AI_TOOL_RESULT( "Error: No conversation ID set for todo operations", false, false );

    wxString convId = wxString::FromUTF8( m_conversationId );
    auto todos = CONVERSATION_DB::Instance().LoadTodos( convId );

    nlohmann::json result = nlohmann::json::array();
    for( const auto& t : todos )
    {
        result.push_back( {
            { "id", t.id.ToStdString() },
            { "content", t.content.ToStdString() },
            { "status", t.status.ToStdString() }
        } );
    }

    return AI_TOOL_RESULT( result.dump( 2 ) );
}


// =============================================================================
// Conversion Debouncing (prevents memory spikes from rapid tool calls)
// =============================================================================

void AI_TOOL_EXECUTOR::queueConversion( const std::string& aTraceFilePath,
                                        const std::string& aKicadFilePath )
{
    std::lock_guard<std::mutex> lock( m_conversionMutex );
    
    // Update pending conversion (last request wins)
    m_pendingConversionTrace = aTraceFilePath;
    m_pendingConversionKicad = aKicadFilePath;
    m_conversionPending.store( true );
    m_lastConversionRequest = std::chrono::steady_clock::now();
    
}


bool AI_TOOL_EXECUTOR::flushPendingConversion( bool force )
{
    std::lock_guard<std::mutex> lock( m_conversionMutex );
    
    if( !m_conversionPending.load() )
        return false; // Nothing queued
    
    // Check if debounce period has elapsed (unless forced)
    if( !force )
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - m_lastConversionRequest ).count();
        
        if( elapsed < CONVERSION_DEBOUNCE_MS )
        {
            // Still within debounce window - wait for more edits
            return false;
        }
    }
    else
    {
        // Force flushing pending conversion (bypassing debounce)
    }
    
    std::string tracePath = m_pendingConversionTrace;
    std::string kicadPath = m_pendingConversionKicad;
    
    // Clear pending flag before conversion (prevents re-entry)
    m_conversionPending.store( false );
    m_pendingConversionTrace.clear();
    m_pendingConversionKicad.clear();
    
    // Execute conversion (unlock during slow Python call to avoid blocking other operations)
    m_conversionMutex.unlock();
    wxLogDebug( wxT( "AI DEBUG [flushPendingConversion]: Calling syncTraceToKicad: %s -> %s" ), 
               wxString::FromUTF8( tracePath ), wxString::FromUTF8( kicadPath ) );
    CONVERSION_RESULT convResult = syncTraceToKicad( tracePath, kicadPath );
    m_conversionMutex.lock();
    
    // Track conversion result for later querying
    m_lastConversionSucceeded.store( convResult.success );
    wxLogDebug( wxT( "AI DEBUG [flushPendingConversion]: Conversion result: success=%s, error=%s" ), 
               convResult.success ? wxT( "true" ) : wxT( "false" ),
               wxString::FromUTF8( convResult.errorMessage ) );
    {
        std::lock_guard<std::mutex> resultLock( m_conversionResultMutex );
        m_lastConversionError = convResult.success ? "" : convResult.errorMessage;
    }
    
    if( convResult.success )
    {
        // Conversion successful
    }
    else
    {
        // Conversion failed
    }
    
    return convResult.success;
}


void AI_TOOL_EXECUTOR::extractAndTrackSymbolUUIDs( const std::string& aContent )
{
    // Parse trace_sch content looking for component statements with UUIDs
    // Format: comp REF SYMBOL ... uid UUID
    // Example: comp R1 Device:R "10k" @ 100,100 rot 0 props {...} pins (...) uid 12345678-1234-1234-1234-123456789abc
    
    std::regex compUidRegex( R"(comp\s+\S+\s+.*?\buid\s+([0-9a-fA-F-]+))" );
    
    std::sregex_iterator it( aContent.begin(), aContent.end(), compUidRegex );
    std::sregex_iterator end;
    
    std::lock_guard<std::mutex> lock( m_modifiedSymbolsMutex );
    
    while( it != end )
    {
        std::smatch match = *it;
        if( match.size() > 1 )
        {
            std::string uuid = match[1].str();
            m_modifiedSymbolUUIDs.insert( uuid );
        }
        ++it;
    }
}


// ---------------------------------------------------------------------------
// Vector search tools
// ---------------------------------------------------------------------------

AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSearchSymbol( const nlohmann::json& aArgs )
{
    std::vector<std::string> queries;

    if( aArgs.contains( "queries" ) && aArgs["queries"].is_array() )
    {
        for( const auto& q : aArgs["queries"] )
            queries.push_back( q.get<std::string>() );
    }

    if( queries.empty() )
        return AI_TOOL_RESULT( "Error: queries is required (array of strings)", false, false );

    int topK = aArgs.value( "top_k", 5 );

    wxString libPaths = GetLibraryPaths( wxS( "SYMBOL_DIR" ) );
    if( libPaths.IsEmpty() )
        return AI_TOOL_RESULT( "Error: No symbol library paths configured", false, false );

    return executeVectorSearch( "symbol", queries, topK, libPaths.ToStdString() );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSearchFootprint( const nlohmann::json& aArgs )
{
    std::vector<std::string> queries;

    if( aArgs.contains( "queries" ) && aArgs["queries"].is_array() )
    {
        for( const auto& q : aArgs["queries"] )
            queries.push_back( q.get<std::string>() );
    }

    if( queries.empty() )
        return AI_TOOL_RESULT( "Error: queries is required (array of strings)", false, false );

    int topK = aArgs.value( "top_k", 5 );

    wxString libPaths = GetLibraryPaths( wxS( "FOOTPRINT_DIR" ) );
    if( libPaths.IsEmpty() )
        return AI_TOOL_RESULT( "Error: No footprint library paths configured", false, false );

    return executeVectorSearch( "footprint", queries, topK, libPaths.ToStdString() );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeVectorSearch( const std::string& aMode,
                                                       const std::vector<std::string>& aQueries,
                                                       int                aTopK,
                                                       const std::string& aLibPaths )
{
    auto& engine = VECTOR_SEARCH_ENGINE::GetInstance();

    // Lazy-init the engine on first use
    if( !engine.IsInitialized() )
    {
        // Locate model, tokenizer, and index directory using the same search chain
        wxString modelPath;
        wxString tokenizerPath;
        wxString indexDir = wxFileName::GetHomeDir() + wxT( "/.trace" );

#ifdef __linux__
        if( wxFileExists( indexDir ) && !wxDirExists( indexDir ) )
            indexDir = wxFileName::GetHomeDir() + wxT( "/.local/share/trace" );
#endif

        wxString stockDataPath = PATHS::GetStockDataPath();
        if( !stockDataPath.IsEmpty() )
        {
            modelPath = stockDataPath + wxT( "/models/all-MiniLM-L6-v2.onnx" );
            tokenizerPath = stockDataPath
                            + wxT( "/scripting/trace/common/tokenizer/tokenizer.json" );
        }

        // Also check environment override
        wxString envTraceDir;
        if( wxGetEnv( wxT( "KICAD_TRACE_DIR" ), &envTraceDir ) && !envTraceDir.IsEmpty() )
        {
            wxFileName envModel( envTraceDir + "/common/models/all-MiniLM-L6-v2.onnx" );
            if( envModel.FileExists() )
                modelPath = envModel.GetFullPath();

            wxFileName envTok( envTraceDir + "/common/tokenizer/tokenizer.json" );
            if( envTok.FileExists() )
                tokenizerPath = envTok.GetFullPath();
        }

        wxString symbolLibDirs = GetLibraryPaths( wxS( "SYMBOL_DIR" ) );
        wxString footprintLibDirs = GetLibraryPaths( wxS( "FOOTPRINT_DIR" ) );

        if( !modelPath.IsEmpty() && wxFileExists( modelPath )
            && !tokenizerPath.IsEmpty() && wxFileExists( tokenizerPath ) )
        {
            engine.Init( modelPath, tokenizerPath, indexDir, symbolLibDirs, footprintLibDirs );
        }
    }

    // If engine is ready, use it (fast path: ~12ms)
    if( engine.IsInitialized() )
    {
        wxLogDebug( wxT( "AI DEBUG [vectorSearch]: Using native C++ engine" ) );
        std::string result = engine.Search( aMode, aQueries, aTopK );
        return AI_TOOL_RESULT( result );
    }

    // Fallback to Python subprocess (slow path: ~370ms)
    wxLogDebug( wxT( "AI DEBUG [vectorSearch]: Falling back to Python subprocess" ) );

    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    if( pythonPath.IsEmpty() )
        return AI_TOOL_RESULT( "Error: Could not find Python interpreter", false, false );

    // Locate local_vector_search.py
    wxFileName searchScript;
    bool scriptFound = false;

    wxString envTraceDir;
    if( wxGetEnv( wxT( "KICAD_TRACE_DIR" ), &envTraceDir ) && !envTraceDir.IsEmpty() )
    {
        wxFileName envScript( envTraceDir + "/common/local_vector_search.py" );
        if( envScript.FileExists() )
        {
            searchScript = envScript;
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
        bundlePath.AppendDir( wxS( "common" ) );
        bundlePath.SetFullName( wxS( "local_vector_search.py" ) );
        if( bundlePath.FileExists() )
        {
            searchScript = bundlePath;
            scriptFound = true;
        }
    }

    if( !scriptFound )
    {
        wxString configuredDir( KICAD_TRACE_DIR, wxConvUTF8 );
        if( !configuredDir.IsEmpty() )
        {
            wxString stockDataPath = PATHS::GetStockDataPath();
            if( !stockDataPath.IsEmpty() )
            {
                wxFileName resolvedScript(
                    stockDataPath + "/scripting/trace/common/local_vector_search.py" );
                if( resolvedScript.FileExists() )
                {
                    searchScript = resolvedScript;
                    scriptFound = true;
                }
            }
        }
    }

    if( !scriptFound )
        return AI_TOOL_RESULT( "Error: Could not find local_vector_search.py", false, false );

    // Locate model and tokenizer for Python path
    wxFileName scriptDir( searchScript.GetPath(), wxEmptyString );
    wxFileName modelFile( scriptDir );
    modelFile.AppendDir( wxS( "models" ) );
    modelFile.SetFullName( wxS( "all-MiniLM-L6-v2.onnx" ) );

    if( !modelFile.FileExists() )
    {
        wxString stockDataPath = PATHS::GetStockDataPath();
        if( !stockDataPath.IsEmpty() )
            modelFile = wxFileName( stockDataPath + "/models/all-MiniLM-L6-v2.onnx" );
    }

    if( !modelFile.FileExists() )
        return AI_TOOL_RESULT( "Error: Could not find ONNX model file", false, false );

    wxFileName tokenizerDir( scriptDir );
    tokenizerDir.AppendDir( wxS( "tokenizer" ) );

    if( !tokenizerDir.DirExists() )
    {
        wxString stockDataPath = PATHS::GetStockDataPath();
        if( !stockDataPath.IsEmpty() )
        {
            tokenizerDir = wxFileName(
                stockDataPath + "/scripting/trace/common/tokenizer/", wxEmptyString );
        }
    }

    if( !tokenizerDir.DirExists() )
        return AI_TOOL_RESULT( "Error: Could not find tokenizer directory", false, false );

    wxString userDbDir = wxFileName::GetHomeDir() + wxT( "/.trace" );

#ifdef __linux__
    if( wxFileExists( userDbDir ) && !wxDirExists( userDbDir ) )
        userDbDir = wxFileName::GetHomeDir() + wxT( "/.local/share/trace" );
#endif

    wxString userDbPath = userDbDir + wxT( "/vector_index.db" );
    wxString libPathsWx( aLibPaths );

    nlohmann::json queryJson = aQueries;
    wxString queryArg = wxString::Format( wxT( " search --queries '%s'" ),
                                          wxString( queryJson.dump() ) );

    wxString stderrFile = wxFileName::CreateTempFileName( wxT( "trace_vs_" ) );

    wxString command = wxString::Format(
        wxT( "\"%s\" \"%s\" --mode %s%s --lib-paths \"%s\" --top-k %d"
             " --model-path \"%s\" --tokenizer-path \"%s\" --db-path \"%s\""
             " 2>\"%s\"" ),
        pythonPath,
        searchScript.GetFullPath(),
        wxString( aMode ),
        queryArg,
        libPathsWx,
        aTopK,
        modelFile.GetFullPath(),
        tokenizerDir.GetPath(),
        userDbPath,
        stderrFile );

    std::string output;
    FILE* pipe = popen( command.ToStdString().c_str(), "r" );
    if( !pipe )
    {
        wxRemoveFile( stderrFile );
        return AI_TOOL_RESULT( "Error: Failed to execute vector search command", false, false );
    }

    std::array<char, 4096> buffer;
    while( fgets( buffer.data(), buffer.size(), pipe ) != nullptr )
        output += buffer.data();

    int exitCode = pclose( pipe );

    wxTextFile stderrLog;
    if( stderrLog.Open( stderrFile ) )
    {
        for( size_t i = 0; i < stderrLog.GetLineCount(); i++ )
        {
            wxString line = stderrLog.GetLine( i );
            if( !line.IsEmpty() )
                wxLogDebug( wxT( "AI DEBUG [vectorSearch]: %s" ), line );
        }
        stderrLog.Close();
    }
    wxRemoveFile( stderrFile );

    if( exitCode != 0 )
        return AI_TOOL_RESULT( "Error: Vector search failed: " + output, false, false );

    return AI_TOOL_RESULT( output );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSetPreferredManufacturer( const nlohmann::json& aArgs )
{
    std::string mfrId = aArgs.value( "manufacturer_id", "" );

    if( m_setManufacturerCallback )
    {
        nlohmann::json result = m_setManufacturerCallback( mfrId );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }

    return AI_TOOL_RESULT( R"({"error":"Set manufacturer callback not configured"})", false, false );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSetManufacturerDrc( const nlohmann::json& aArgs )
{
    if( m_appType != "pcbnew" )
        return AI_TOOL_RESULT( R"({"error":"Only available in PCB editor"})", false, false );

    std::string presetId = aArgs.value( "preset_id", "" );

    if( m_drcPresetCallback )
    {
        nlohmann::json result = m_drcPresetCallback( presetId );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }

    return AI_TOOL_RESULT( R"({"error":"DRC preset callback not configured"})", false, false );
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeTraceSignalPath( const nlohmann::json& aArgs )
{
    wxLogTrace( traceAiToolCall, wxT( "AI: executeTraceSignalPath" ) );

    if( m_appType != "eeschema" )
    {
        nlohmann::json error;
        error["error"] = "trace_signal_path is only available in the schematic editor";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_traceSignalPathCallback )
    {
        nlohmann::json error;
        error["error"] = "Signal path tracer callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_traceSignalPathCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Signal path trace failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeTracePcbSignalPath( const nlohmann::json& aArgs )
{
    wxLogTrace( traceAiToolCall, wxT( "AI: executeTracePcbSignalPath" ) );

    if( m_appType != "pcbnew" )
    {
        nlohmann::json error;
        error["error"] = "trace_pcb_signal_path is only available in the PCB editor";
        error["current_app"] = m_appType;
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    if( !m_tracePcbSignalPathCallback )
    {
        nlohmann::json error;
        error["error"] = "PCB signal path tracer callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_tracePcbSignalPathCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "PCB signal path trace failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeHighlightNet( const nlohmann::json& aArgs )
{
    wxLogTrace( traceAiToolCall, wxT( "AI: executeHighlightNet" ) );

    if( !m_highlightNetCallback )
    {
        nlohmann::json error;
        error["error"] = "Highlight net callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_highlightNetCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Highlight net failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}


AI_TOOL_RESULT AI_TOOL_EXECUTOR::executeSelectItems( const nlohmann::json& aArgs )
{
    wxLogTrace( traceAiToolCall, wxT( "AI: executeSelectItems" ) );

    if( !m_selectItemsCallback )
    {
        nlohmann::json error;
        error["error"] = "Select items callback not configured";
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }

    try
    {
        nlohmann::json result = m_selectItemsCallback( aArgs );
        return AI_TOOL_RESULT( result.dump( 2 ), false, true );
    }
    catch( const std::exception& e )
    {
        nlohmann::json error;
        error["error"] = std::string( "Select items failed: " ) + e.what();
        return AI_TOOL_RESULT( error.dump( 2 ), false, false );
    }
}