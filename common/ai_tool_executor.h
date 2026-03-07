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

#ifndef AI_TOOL_EXECUTOR_H
#define AI_TOOL_EXECUTOR_H

#include <kicommon.h>
#include <ai_diff_analyzer.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <future>
#include <chrono>
#include <json_common.h>

/**
 * Result of a trace to KiCad conversion operation.
 */
struct KICOMMON_API CONVERSION_RESULT
{
    bool        success;      ///< True if conversion succeeded
    std::string errorMessage; ///< Error message if conversion failed
    std::string output;       ///< Full output from conversion (stdout + stderr)

    CONVERSION_RESULT() : success( false ) {}
    CONVERSION_RESULT( bool aSuccess, const std::string& aError, const std::string& aOutput )
        : success( aSuccess ), errorMessage( aError ), output( aOutput )
    {
    }
};

/**
 * Result of a tool execution.
 */
struct KICOMMON_API AI_TOOL_RESULT
{
    std::string result;       ///< Result message or content
    bool        fileModified; ///< True if the file was modified
    bool        success;      ///< True if execution succeeded
    DIFF_RESULT diffInfo;     ///< Diff analysis result (for replace_in_file)
    bool        hasDiffInfo;  ///< True if diffInfo was computed
    std::string conversionLogs; ///< Logs from trace_sch to kicad_sch conversion (if applicable)

    AI_TOOL_RESULT() : fileModified( false ), success( true ), hasDiffInfo( false ) {}
    AI_TOOL_RESULT( const std::string& aResult, bool aModified = false, bool aSuccess = true )
        : result( aResult ), fileModified( aModified ), success( aSuccess ), hasDiffInfo( false )
    {
    }
};

/**
 * Executes AI tools locally.
 * 
 * Handles tool calls from the backend, executes them on local files,
 * and returns results. Supports schematic and PCB operations.
 */
class KICOMMON_API AI_TOOL_EXECUTOR
{
public:
    /**
     * Construct a tool executor.
     * @param aAppType Application type ("eeschema" or "pcbnew").
     */
    explicit AI_TOOL_EXECUTOR( const std::string& aAppType = "eeschema" );

    /**
     * Execute a tool with the given arguments.
     * @param aToolName Name of the tool to execute.
     * @param aToolArgs JSON arguments for the tool.
     * @param aFilePath Path to the trace file being worked on.
     * @param aKicadFilePath Path to the corresponding KiCad file (for conversion).
     * @return Tool execution result.
     */
    AI_TOOL_RESULT ExecuteTool( const std::string&      aToolName,
                                const nlohmann::json&   aToolArgs,
                                const std::string&      aFilePath,
                                const std::string&      aKicadFilePath = "" );

    /**
     * Set application type.
     */
    void SetAppType( const std::string& aAppType ) { m_appType = aAppType; }

    /**
     * Set callback for getting DRC violations from PCB editor.
     * This allows direct access to the editor's DRC data without file-based IPC.
     */
    void SetDrcCallback( std::function<nlohmann::json()> aCallback )
    {
        m_drcCallback = aCallback;
    }

    /**
     * Set callback for getting ERC violations from schematic editor.
     * This allows direct access to the editor's ERC data without file-based IPC.
     */
    void SetErcCallback( std::function<nlohmann::json()> aCallback )
    {
        m_ercCallback = aCallback;
    }

    /**
     * Set callback for running annotation in schematic editor.
     * This allows direct access to the editor's annotation functionality.
     * @param aCallback Function that takes JSON options and returns JSON result.
     */
    void SetAnnotateCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_annotateCallback = aCallback;
    }

    /**
     * Set callback for generating Gerber files from PCB editor.
     * This allows direct access to the editor's Gerber generation functionality.
     * @param aCallback Function that takes JSON options and returns JSON with list of generated files.
     */
    void SetGerberCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_gerberCallback = aCallback;
    }

    /**
     * Set callback for generating drill files from PCB editor.
     * This allows direct access to the editor's drill file generation functionality.
     * @param aCallback Function that takes JSON options and returns JSON with list of generated files.
     */
    void SetDrillCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_drillCallback = aCallback;
    }

    /**
     * Set callback for autorouting from PCB editor.
     * This allows direct access to the editor's autorouting functionality.
     * @param aCallback Function that takes JSON params and returns JSON with routing result.
     *                  Input: { "params": { ... routing parameters ... } }
     *                  Output: { "success": bool, "message": string, "progress_log": [...] }
     */
    void SetAutorouteCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_autorouteCallback = aCallback;
    }

    /**
     * Set callback for getting schematic hierarchy information.
     * This allows direct access to the editor's sheet hierarchy.
     * @param aCallback Function that returns JSON with hierarchy info.
     */
    void SetHierarchyCallback( std::function<nlohmann::json()> aCallback )
    {
        m_hierarchyCallback = aCallback;
    }

    /**
     * Set callback for switching to a different sheet in the hierarchy.
     * This allows direct access to the editor's sheet navigation.
     * @param aCallback Function that takes JSON params and returns JSON result.
     */
    void SetSwitchSheetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_switchSheetCallback = aCallback;
    }

    /**
     * Set callback for switching to a different layer in PCB editor.
     * @param aCallback Function that takes JSON params and returns JSON result.
     *                  Input: { "layer_name": string, "layer_id": int }
     *                  Output: { "success": bool, "active_layer": string, "layer_id": int }
     */
    void SetSwitchLayerCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_switchLayerCallback = aCallback;
    }

    /**
     * Set callback for getting PCB layer information.
     * @param aCallback Function that returns JSON with layer info.
     */
    void SetLayersCallback( std::function<nlohmann::json()> aCallback )
    {
        m_layersCallback = aCallback;
    }

    /**
     * Set callback for applying a layer preset.
     * @param aCallback Function that takes preset name and returns JSON result.
     */
    void SetLayerPresetCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_layerPresetCallback = aCallback;
    }

    /**
     * Set callback for setting layer visibility.
     * @param aCallback Function that takes layer and visibility params.
     */
    void SetLayerVisibilityCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_layerVisibilityCallback = aCallback;
    }

    /**
     * Set callback for fetching footprint dimensions from the PCB editor.
     * @param aCallback Function that takes JSON with footprint list and returns dimensions.
     */
    void SetFetchDimensionsCallback( std::function<nlohmann::json( const nlohmann::json& )> aCallback )
    {
        m_fetchDimensionsCallback = aCallback;
    }
  
    /*
     * Set callback for generating snapshots.
     * This allows direct access to snapshot generation without file-based IPC.
     * @param aCallback Callback that generates snapshot and returns base64-encoded SVG content.
     */
    void SetSnapshotCallback( std::function<std::string()> aCallback )
    {
        m_snapshotCallback = aCallback;
    }

    /**
     * Set the current conversation ID for todo operations.
     */
    void SetConversationId( const std::string& aConversationId )
    {
        m_conversationId = aConversationId;
    }

    /**
     * Get application type.
     */
    std::string GetAppType() const { return m_appType; }

    /**
     * Check if the last conversion (trace_sch to kicad_sch) succeeded.
     * @return True if last conversion succeeded, false if it failed or hasn't run.
     */
    bool WasLastConversionSuccessful() const { return m_lastConversionSucceeded.load(); }

    /**
     * Get the error message from the last failed conversion.
     * @return Error message, or empty string if last conversion succeeded.
     */
    std::string GetLastConversionError() const
    {
        std::lock_guard<std::mutex> lock( m_conversionResultMutex );
        return m_lastConversionError;
    }

    /**
     * Reset conversion tracking state.
     * Call this before starting a new AI edit session.
     */
    void ResetConversionState()
    {
        m_lastConversionSucceeded.store( true );  // Assume success until proven otherwise
        std::lock_guard<std::mutex> lock( m_conversionResultMutex );
        m_lastConversionError.clear();
    }

    /**
     * Get the set of symbol UUIDs that were modified during this session.
     * Used for autoplacing fields after AI edits complete.
     * @return Set of UUID strings for modified symbols.
     */
    std::set<std::string> GetModifiedSymbolUUIDs() const
    {
        std::lock_guard<std::mutex> lock( m_modifiedSymbolsMutex );
        return m_modifiedSymbolUUIDs;
    }

    /**
     * Clear the set of modified symbol UUIDs.
     * Call this after autoplacement is complete.
     */
    void ClearModifiedSymbolUUIDs()
    {
        std::lock_guard<std::mutex> lock( m_modifiedSymbolsMutex );
        m_modifiedSymbolUUIDs.clear();
    }

    /**
     * Set callback for file modification events.
     * Called when a file is successfully modified.
     */
    void SetFileModifiedCallback( std::function<void( const std::string& )> aCallback )
    {
        m_fileModifiedCallback = aCallback;
    }

    /**
     * Set callback for delete confirmation requests.
     * Called when delete_trace_file needs user confirmation.
     * @param aCallback Function that takes a filename and returns a future<bool> indicating confirmation.
     */
    void SetConfirmationCallback( std::function<std::future<bool>( const std::string& )> aCallback )
    {
        m_confirmationCallback = aCallback;
    }

    /**
     * Set allowed project directories for file operations.
     * Files outside these directories will be blocked for security.
     * If empty, all paths are allowed (development mode).
     * @param aDirs List of allowed directory paths.
     */
    void SetAllowedProjectDirs( const std::vector<std::string>& aDirs );

    /**
     * Add a single allowed project directory.
     * @param aDir Directory path to allow.
     */
    void AddAllowedProjectDir( const std::string& aDir );

    /**
     * Clear all allowed project directories.
     * After clearing, all paths will be allowed (development mode).
     */
    void ClearAllowedProjectDirs();

    /**
     * Execute pending conversion if debounce period has elapsed.
     * Called at end of stream or periodically to flush queued conversions.
     * @param force If true, bypass debounce and execute immediately.
     */
    // Returns true if a conversion was actually performed
    bool flushPendingConversion( bool force = false );

private:
    /**
     * Read file content from disk.
     */
    std::string readFileContent( const std::string& aFilePath );

    /**
     * Write file content to disk (with backup).
     */
    bool writeFileContent( const std::string& aFilePath, const std::string& aContent );

    /**
     * Execute read_file tool.
     * Reads file content with optional offset and limit.
     * Returns content with line numbers in format LINE_NUMBER|LINE_CONTENT.
     * @param aArgs Tool arguments (target_file, offset, limit).
     * @param aProjectDir Project directory for resolving relative paths.
     * @param aDefaultFilePath Default file path if target_file not specified.
     * @return File content with line numbers.
     */
    AI_TOOL_RESULT executeReadFile( const nlohmann::json& aArgs,
                                    const std::string&    aProjectDir,
                                    const std::string&    aDefaultFilePath );

    /**
     * Execute write tool.
     * Creates or overwrites a file with the given contents.
     * @param aArgs Tool arguments (file_path, contents).
     * @param aProjectDir Project directory for resolving relative paths.
     * @param aKicadFilePath Path to corresponding KiCad file (for conversion).
     * @return Success/error message.
     */
    AI_TOOL_RESULT executeWrite( const nlohmann::json& aArgs,
                                 const std::string&    aProjectDir,
                                 const std::string&    aKicadFilePath );

    /**
     * Execute search_replace tool.
     * Performs exact string replacement in a file.
     * Uses optimistic concurrency: verifies file unchanged since read before write.
     * @param aArgs Tool arguments (file_path, old_string, new_string, replace_all).
     * @param aProjectDir Project directory for resolving relative paths.
     * @param aDefaultFilePath Default file path if file_path not specified.
     * @param aKicadFilePath Path to corresponding KiCad file (for conversion).
     * @return Success/error message with diff info.
     */
    AI_TOOL_RESULT executeSearchReplace( const nlohmann::json& aArgs,
                                         const std::string&    aProjectDir,
                                         const std::string&    aDefaultFilePath,
                                         const std::string&    aKicadFilePath );

    /**
     * Execute grep tool.
     * Searches for a pattern in files using regex.
     * @param aArgs Tool arguments (pattern, path, type, glob, output_mode, A, B, C, i, multiline, head_limit).
     * @param aProjectDir Project directory for searching.
     * @param aDefaultFilePath Default file path if path not specified.
     * @return Matching lines with file paths and line numbers.
     */
    AI_TOOL_RESULT executeGrep( const nlohmann::json& aArgs,
                                const std::string&    aProjectDir,
                                const std::string&    aDefaultFilePath );

    /**
     * Execute list_dir tool.
     * Lists files and directories in a given path.
     * For pcbnew, lists both .trace_sch and .trace_pcb files.
     * @param aArgs Tool arguments (path, ignore_globs).
     * @param aProjectDir Project directory (default path).
     * @return List of file and directory names.
     */
    AI_TOOL_RESULT executeListDir( const nlohmann::json& aArgs,
                                   const std::string&    aProjectDir );

    /**
     * Execute take_snapshot tool.
     * Generates a snapshot via direct callback.
     */
    AI_TOOL_RESULT executeTakeSnapshot( const std::string& aFilePath,
                                        const std::string& aKicadFilePath );

    /**
     * Execute run_drc tool.
     * Runs DRC and returns violations from PCB editor.
     * @param aArgs Tool arguments.
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON array of DRC violations.
     */
    AI_TOOL_RESULT executeRunDrc( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute run_erc tool.
     * Runs ERC and returns violations from schematic editor.
     * @param aArgs Tool arguments.
     * @param aKicadFilePath Path to .kicad_sch file.
     * @return JSON array of ERC violations.
     */
    AI_TOOL_RESULT executeRunErc( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute run_annotate tool.
     * Runs annotation and returns result messages from schematic editor.
     * @param aArgs Tool arguments (scope, reset, start_number, etc.).
     * @param aKicadFilePath Path to .kicad_sch file.
     * @return JSON object with annotation messages and summary.
     */
    AI_TOOL_RESULT executeRunAnnotate( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute generate_gerbers tool.
     * Generates Gerber files from PCB editor.
     * @param aArgs Tool arguments (all optional, uses KiCad defaults if not provided).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with list of generated files.
     */
    AI_TOOL_RESULT executeGenerateGerbers( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute generate_drill_files tool.
     * Generates drill files from PCB editor.
     * @param aArgs Tool arguments (all optional, uses KiCad defaults if not provided).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with list of generated files.
     */
    AI_TOOL_RESULT executeGenerateDrill( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute autoroute tool.
     * Runs cloud autorouting on the PCB.
     * @param aArgs Tool arguments (params for autorouting).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with routing result.
     */
    AI_TOOL_RESULT executeAutoroute( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute get_hierarchy tool.
     * Returns the schematic sheet hierarchy information.
     * @param aArgs Tool arguments (unused).
     * @param aKicadFilePath Path to .kicad_sch file.
     * @return JSON object with hierarchy info.
     */
    AI_TOOL_RESULT executeGetHierarchy( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute switch_sheet tool.
     * Switches to a different sheet in the hierarchy.
     * @param aArgs Tool arguments (page_number, sheet_name, or sheet_path).
     * @param aKicadFilePath Path to .kicad_sch file.
     * @return JSON object with switch result.
     */
    AI_TOOL_RESULT executeSwitchSheet( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute switch_layer tool.
     * Switches to a different layer in the PCB editor.
     * @param aArgs Tool arguments (layer_name or layer_id).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with switch result.
     */
    AI_TOOL_RESULT executeSwitchLayer( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute get_layers tool.
     * Returns information about all PCB layers.
     * @param aArgs Tool arguments (unused).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with layer info.
     */
    AI_TOOL_RESULT executeGetLayers( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute apply_layer_preset tool.
     * Applies a layer visibility preset.
     * @param aArgs Tool arguments (preset_name).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with preset result.
     */
    AI_TOOL_RESULT executeApplyLayerPreset( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute set_layer_visibility tool.
     * Sets visibility of a specific layer.
     * @param aArgs Tool arguments (layer_name or layer_id, visible).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object with visibility result.
     */
    AI_TOOL_RESULT executeSetLayerVisibility( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute fetch_dimensions tool.
     * Returns width/height dimensions for requested footprints on the board.
     * @param aArgs Tool arguments (footprints array).
     * @param aKicadFilePath Path to .kicad_pcb file.
     * @return JSON object mapping footprint identifiers to {width, height} in mm.
     */
    AI_TOOL_RESULT executeFetchDimensions( const nlohmann::json& aArgs, const std::string& aKicadFilePath );

    /**
     * Execute zip_gerber_files tool.
     * Zips all .gbr and .drl files in the project directory.
     * @param aFilePath Path to trace file (used to determine project directory).
     * @return JSON object with zip file path and list of included files.
     */
    AI_TOOL_RESULT executeZipGerberFiles( const std::string& aFilePath );

    /**
     * Execute todo_write tool.
     * Saves or updates todos for the current conversation.
     * @param aArgs Tool arguments (todos array, merge flag).
     * @return JSON result with updated todo list.
     */
    AI_TOOL_RESULT executeTodoWrite( const nlohmann::json& aArgs );

    /**
     * Execute todo_read tool.
     * Loads all todos for the current conversation.
     * @param aArgs Tool arguments (unused).
     * @return JSON array of todo items.
     */
    AI_TOOL_RESULT executeTodoRead( const nlohmann::json& aArgs );

    /**
     * Execute search_symbols tool.
     * Searches for symbols by semantic similarity using local vector index.
     * @param aArgs Tool arguments (query, top_k).
     * @return JSON object with ranked symbol matches.
     */
    AI_TOOL_RESULT executeSearchSymbol( const nlohmann::json& aArgs );

    /**
     * Execute search_footprints tool.
     * Searches for footprints by semantic similarity using local vector index.
     * @param aArgs Tool arguments (query, top_k).
     * @return JSON object with ranked footprint matches.
     */
    AI_TOOL_RESULT executeSearchFootprint( const nlohmann::json& aArgs );

    /**
     * Shared implementation for vector search tools.
     * Shells out to the Python local_vector_search.py script.
     * @param aMode "symbol" or "footprint".
     * @param aQueries List of search queries.
     * @param aTopK Number of results to return per query.
     * @param aLibPaths Colon-separated library directory paths.
     * @return JSON search results (dict mapping each query to its results).
     */
    AI_TOOL_RESULT executeVectorSearch( const std::string& aMode,
                                        const std::vector<std::string>& aQueries,
                                        int                aTopK,
                                        const std::string& aLibPaths );

    /**
     * Execute delete_trace_file tool.
     * Deletes a trace_sch file and its corresponding kicad_sch after user confirmation.
     */
    AI_TOOL_RESULT executeDeleteTraceFile( const nlohmann::json& aArgs,
                                           const std::string&    aProjectDir,
                                           const std::string&    aMainFilePath );

    /**
     * Convert trace file to KiCad format.
     * Uses popen for thread-safe execution from background threads.
     * @return CONVERSION_RESULT with success status, error message, and full output.
     */
    CONVERSION_RESULT syncTraceToKicad( const std::string& aTraceFilePath,
                                        const std::string& aKicadFilePath );

    /**
     * Queue a conversion request with debouncing.
     * Multiple rapid calls will be batched into a single conversion.
     * @param aTraceFilePath Path to trace file.
     * @param aKicadFilePath Path to KiCad file.
     */
    void queueConversion( const std::string& aTraceFilePath,
                          const std::string& aKicadFilePath );

    /**
     * Get the trace file path from a KiCad file path.
     */
    std::string getTraceFilePath( const std::string& aKicadFilePath );

    /**
     * Get the KiCad file path from a trace file path.
     */
    std::string getKicadFilePath( const std::string& aTraceFilePath );

    /**
     * Count occurrences of a substring in a string.
     */
    size_t countOccurrences( const std::string& aHaystack, const std::string& aNeedle );

    /**
     * Normalize line endings from CRLF/CR to LF.
     * Handles Windows (CRLF) and old Mac (CR) line endings.
     */
    std::string normalizeLineEndings( const std::string& aContent );

    /**
     * Check if a string contains only whitespace characters.
     */
    bool isWhitespaceOnly( const std::string& aContent );

    /**
     * Strip trailing newlines from a string.
     */
    std::string rtrimNewlines( const std::string& aContent );

    /**
     * List all trace files in a directory.
     * For pcbnew: lists both .trace_sch and .trace_pcb files.
     * For eeschema: lists only .trace_sch files.
     * @return Vector of filenames (not full paths).
     */
    std::vector<std::string> listTraceFilesInDir( const std::string& aProjectDir );

    /**
     * Resolve file path from filename argument.
     * If filename is provided, resolve relative to project dir.
     * If not provided and single-file project, use default.
     * If not provided and multi-file project, return empty string (caller should error).
     * @param aArgs Tool arguments (may contain "filename").
     * @param aProjectDir Project directory path.
     * @param aDefaultFilePath Default file path to use for single-file projects.
     * @param outIsMultiFile Set to true if project has multiple trace_sch files.
     * @return Resolved file path, or empty string if filename required but not provided.
     */
    std::string resolveFilePath( const nlohmann::json& aArgs,
                                 const std::string&    aProjectDir,
                                 const std::string&    aDefaultFilePath,
                                 bool&                 outIsMultiFile );

    /**
     * Copy file headers from source trace_sch file.
     * Extracts kicad_ver, kicad_gen, kicad_gen_ver, and paper lines.
     * @return Header content string (without file_uid, which should be regenerated).
     */
    std::string copyFileHeaders( const std::string& aSourceFilePath );

    /**
     * Validate that a file path is safe to access.
     * 
     * Security checks:
     * 1. Resolve to absolute path (prevents path traversal like ../../etc/passwd)
     * 2. Check file extension is in allowed list
     * 3. Check path is within allowed project directories
     * 
     * @param aFilePath Path to validate.
     * @param aOperation Operation type for logging ("read", "write", "delete").
     * @return Pair of (is_valid, error_message). If valid, error_message is empty.
     */
    std::pair<bool, std::string> validateFilePath( const std::string& aFilePath,
                                                    const std::string& aOperation = "access" );

    /**
     * Get the canonical (resolved) path for a file.
     * Resolves symlinks and removes . and .. components.
     */
    std::string getCanonicalPath( const std::string& aFilePath );

    /**
     * Check if a path is within a directory.
     */
    bool isPathWithinDirectory( const std::string& aPath, const std::string& aDirectory );

    /**
     * Extract symbol UUIDs from trace_sch content and add to modified set.
     * Parses the content looking for symbol statements and extracts their UUIDs.
     * @param aContent The trace_sch content to parse.
     */
    void extractAndTrackSymbolUUIDs( const std::string& aContent );

    std::string                                m_appType;
    std::string                                m_conversationId;
    std::function<void( const std::string& )>  m_fileModifiedCallback;
    std::function<std::future<bool>( const std::string& )> m_confirmationCallback;
    
    // DRC/ERC callbacks for direct access to editor violations
    std::function<nlohmann::json()> m_drcCallback;
    std::function<nlohmann::json()> m_ercCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_annotateCallback;
    
    // Gerber/Drill callbacks for direct access to editor generation
    std::function<nlohmann::json( const nlohmann::json& )> m_gerberCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_drillCallback;
    
    // Autoroute callback for direct access to PCB autorouting
    std::function<nlohmann::json( const nlohmann::json& )> m_autorouteCallback;
    
    // Hierarchy callbacks for schematic sheet navigation
    std::function<nlohmann::json()> m_hierarchyCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_switchSheetCallback;
    
    // Layer callbacks for PCB layer navigation
    std::function<nlohmann::json( const nlohmann::json& )> m_switchLayerCallback;
    std::function<nlohmann::json()> m_layersCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_layerPresetCallback;
    std::function<nlohmann::json( const nlohmann::json& )> m_layerVisibilityCallback;
    
    // Fetch dimensions callback for footprint size queries
    std::function<nlohmann::json( const nlohmann::json& )> m_fetchDimensionsCallback;
    
    // Snapshot callback for direct snapshot generation
    std::function<std::string()> m_snapshotCallback;
    
    // Security: Allowed project directories for file operations
    std::vector<std::string> m_allowedProjectDirs;
    
    // Conversion debouncing (prevent memory spikes from rapid tool calls)
    std::mutex                   m_conversionMutex;        ///< Protects conversion queue
    std::atomic<bool>            m_conversionPending;      ///< True if conversion queued
    std::string                  m_pendingConversionTrace; ///< Queued trace file path
    std::string                  m_pendingConversionKicad; ///< Queued KiCad file path
    std::chrono::steady_clock::time_point m_lastConversionRequest; ///< Time of last request
    static constexpr int         CONVERSION_DEBOUNCE_MS = 200; ///< Debounce delay
    
    // Conversion result tracking
    std::atomic<bool>            m_lastConversionSucceeded; ///< True if last conversion succeeded
    std::string                  m_lastConversionError;     ///< Error message from last failed conversion
    mutable std::mutex           m_conversionResultMutex;   ///< Protects m_lastConversionError
    
    // Modified symbol tracking for autoplace
    std::set<std::string>        m_modifiedSymbolUUIDs;     ///< UUIDs of symbols modified in this session
    mutable std::mutex           m_modifiedSymbolsMutex;    ///< Protects m_modifiedSymbolUUIDs
    
    // Security: Allowed file extensions for read/write operations
    static const std::set<std::string> s_allowedExtensions;

    // =========================================================================
    // Concurrent Editing Support (like Cursor)
    // =========================================================================
    
    /**
     * File-level locks for concurrent access.
     * Uses shared_mutex to allow multiple readers OR one writer.
     * Key is canonical file path.
     */
    static std::map<std::string, std::shared_mutex> s_fileLocks;
    static std::mutex s_fileLocksMapMutex;  ///< Protects s_fileLocks map itself
    
    /**
     * Get or create a shared_mutex for a file.
     * Thread-safe.
     */
    std::shared_mutex& getFileLock( const std::string& aCanonicalPath );
    
    /**
     * Compute a hash of file content for version checking.
     * Uses fast hash for change detection (not cryptographic).
     */
    std::string computeFileHash( const std::string& aContent );
    
    /**
     * Read file content with its hash (for optimistic concurrency).
     * Acquires shared (read) lock during read.
     * @return Pair of (content, hash).
     */
    std::pair<std::string, std::string> readFileWithHash( const std::string& aFilePath );
    
    /**
     * Write file only if it hasn't changed since we read it.
     * Acquires exclusive (write) lock, re-reads file, checks hash.
     * If hash matches expected, writes new content.
     * If hash doesn't match, returns false (conflict detected).
     * 
     * @param aFilePath Path to file.
     * @param aNewContent Content to write.
     * @param aExpectedHash Hash from when we read the file.
     * @param aConflictContent If conflict detected, filled with current file content.
     * @return True if write succeeded, false if conflict detected.
     */
    bool writeFileIfUnchanged( const std::string& aFilePath,
                               const std::string& aNewContent,
                               const std::string& aExpectedHash,
                               std::string&       aConflictContent );
};

#endif // AI_TOOL_EXECUTOR_H

