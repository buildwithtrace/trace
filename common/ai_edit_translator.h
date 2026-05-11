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

#ifndef AI_EDIT_TRANSLATOR_H
#define AI_EDIT_TRANSLATOR_H

#include <kicommon.h>
#include <kiid.h>
#include <string>
#include <vector>
#include <map>
#include <memory>


/**
 * Types of operations produced by the translator.
 */
enum class AI_EDIT_OP_TYPE
{
    ADD,
    REMOVE,
    MODIFY
};

/**
 * A single item-level operation produced by diffing before/after KiCad s-expression trees.
 *
 * For ADD: newSexp contains the s-expression for the new item.
 * For REMOVE: itemUuid identifies the item to remove.
 * For MODIFY: itemUuid identifies the item; newSexp contains its updated s-expression.
 */
struct KICOMMON_API AI_EDIT_OP
{
    AI_EDIT_OP_TYPE type;
    KIID            itemUuid;
    std::string     newSexp;     ///< S-expression for the new/modified item
    std::string     oldSexp;     ///< S-expression for the original item (MODIFY only, for diagnostics)
};

/**
 * Result of translating AI edits into item-level operations.
 */
struct KICOMMON_API AI_EDIT_TRANSLATION
{
    bool                    success;
    std::string             errorMessage;
    std::vector<AI_EDIT_OP> ops;

    AI_EDIT_TRANSLATION() : success( false ) {}

    bool HasChanges() const { return !ops.empty(); }
};

/**
 * Translates AI tool edits into concrete item-level operations by diffing
 * before/after trace file content through the Python converter and KiCad's
 * native s-expression parsers.
 *
 * The translation pipeline:
 *   1. Convert both before/after .trace_sch content to .kicad_sch s-expressions
 *      via the existing Python converter (syncTraceToKicad, run in-memory via temp files).
 *   2. Parse both s-expression outputs to extract per-item UUID-keyed blocks.
 *   3. Diff the two maps to produce ADD/REMOVE/MODIFY operations.
 *
 * Callers then feed these ops to AI_COMMIT which applies them through
 * BOARD_COMMIT / SCH_COMMIT for native undo/redo support.
 */
class KICOMMON_API AI_EDIT_TRANSLATOR
{
public:
    /**
     * @param aAppType "eeschema" or "pcbnew"
     */
    explicit AI_EDIT_TRANSLATOR( const std::string& aAppType );

    /**
     * Translate a before/after pair of trace file contents into item-level operations.
     *
     * Writes both contents to temporary files, runs the Python converter on each,
     * then diffs the resulting KiCad s-expression files by UUID.
     *
     * @param aBeforeTraceContent The trace file content before the AI edit.
     * @param aAfterTraceContent  The trace file content after the AI edit.
     * @return Translation result with a vector of AI_EDIT_OP.
     */
    AI_EDIT_TRANSLATION Translate( const std::string& aBeforeTraceContent,
                                   const std::string& aAfterTraceContent );

    /**
     * Translate using a pre-converted KiCad file for the "after" state.
     *
     * When the tool executor has already run syncTraceToKicad(), the resulting
     * .kicad_sch/.kicad_pcb file is on disk. This overload reads that file directly
     * for the "after" s-expression content, only running the converter on the
     * "before" trace content. This avoids running the converter twice per edit.
     *
     * @param aBeforeTraceContent The trace file content before the AI edit.
     * @param aAfterKicadFilePath Path to the already-converted KiCad file on disk.
     * @param aAfterTraceContent  The trace file content after the AI edit (used only
     *                            if the KiCad file cannot be read from disk).
     * @return Translation result with a vector of AI_EDIT_OP.
     */
    AI_EDIT_TRANSLATION TranslateWithKicadFile(
            const std::string& aBeforeTraceContent,
            const std::string& aAfterKicadFilePath,
            const std::string& aAfterTraceContent );

private:
    /**
     * Run the Python converter on a trace file to produce KiCad s-expression output.
     *
     * @param aTraceFilePath  Path to the temporary trace file.
     * @param aKicadFilePath  Path where the KiCad s-expression output should be written.
     * @return True if conversion succeeded.
     */
    bool runConverter( const std::string& aTraceFilePath,
                       const std::string& aKicadFilePath );

    /**
     * Parse a KiCad s-expression file and extract top-level items keyed by UUID.
     *
     * Each top-level element (symbol, wire, junction, label, footprint, track, via, etc.)
     * is extracted as a raw s-expression string, keyed by its uuid field.
     *
     * @param aKicadContent The KiCad s-expression file content.
     * @return Map of UUID string -> raw s-expression block for each item.
     */
    std::map<std::string, std::string> extractItemsByUuid( const std::string& aKicadContent );

    /**
     * Extract the UUID from a KiCad s-expression block.
     * Looks for (uuid "...") or (uuid ...) patterns.
     *
     * @param aSexp The s-expression block to search.
     * @return The UUID string, or empty if not found.
     */
    std::string extractUuidFromSexp( const std::string& aSexp );

    /**
     * Extract all top-level item blocks from a KiCad s-expression file.
     * Handles nested parentheses to correctly delimit each item.
     *
     * @param aContent The full KiCad s-expression file content.
     * @return Vector of individual item s-expression blocks.
     */
    std::vector<std::string> extractTopLevelItems( const std::string& aContent );

    /**
     * Core diffing logic: given before and after KiCad s-expression content,
     * produce the list of ADD/REMOVE/MODIFY operations.
     */
    AI_EDIT_TRANSLATION diffKicadContent( const std::string& aBeforeKicad,
                                          const std::string& aAfterKicad );

    std::string m_appType;
    std::string m_lastAfterKicadContent;  ///< Cached "after" KiCad content from previous edit
};

#endif // AI_EDIT_TRANSLATOR_H
