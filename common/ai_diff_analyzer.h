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

#ifndef AI_DIFF_ANALYZER_H
#define AI_DIFF_ANALYZER_H

#include <kicommon.h>
#include <string>
#include <vector>
#include <map>
#include <optional>

/**
 * Represents a parsed element from a trace_sch or trace_pcb file.
 */
struct KICOMMON_API TRACE_ELEMENT
{
    std::string type;           ///< Element type: component, wire, junction, footprint, track, etc.
    std::string uid;            ///< Unique ID (uuid or composite key)
    std::string ref;            ///< Reference designator (for components/footprints)
    std::string symbol;         ///< Symbol name (for components) or footprint library
    std::string name;           ///< Name (for labels, nets)
    double      atX = 0.0;      ///< X position
    double      atY = 0.0;      ///< Y position
    int         rotation = 0;   ///< Rotation in degrees
    std::string value;          ///< Component/footprint value
    std::string rawContent;     ///< Original line content for comparison

    // PCB-specific fields
    std::string layer;          ///< Layer name (for PCB elements)
    std::string net;            ///< Net name (for tracks, vias, pads)
    double      width = 0.0;    ///< Width (for tracks)

    /**
     * Get a unique key for this element.
     */
    std::string GetKey() const;

    /**
     * Check equality with another element (ignoring metadata).
     */
    bool Equals( const TRACE_ELEMENT& aOther ) const;
};

/**
 * Represents a modification between two versions of an element.
 */
struct KICOMMON_API ELEMENT_MODIFICATION
{
    TRACE_ELEMENT oldElement;
    TRACE_ELEMENT newElement;
};

/**
 * Result of analyzing differences between two trace_sch contents.
 */
struct KICOMMON_API DIFF_RESULT
{
    std::vector<TRACE_ELEMENT>       added;      ///< Elements added in new version
    std::vector<TRACE_ELEMENT>       removed;    ///< Elements removed from old version
    std::vector<ELEMENT_MODIFICATION> modified;  ///< Elements that changed

    bool        isSimple = true;           ///< True if incremental update is safe
    std::string complexityReason;          ///< Explanation of complexity classification

    /**
     * Get total number of changes.
     */
    size_t GetTotalChanges() const
    {
        return added.size() + removed.size() + modified.size();
    }

    /**
     * Check if there are any changes.
     */
    bool HasChanges() const { return GetTotalChanges() > 0; }
};

/**
 * Analyzes differences between trace_sch file contents.
 *
 * Parses trace_sch content using a lightweight line-based parser
 * and computes differences to determine if incremental updates are safe.
 */
class KICOMMON_API AI_DIFF_ANALYZER
{
public:
    AI_DIFF_ANALYZER() = default;

    /**
     * Parse trace_sch content into a list of elements.
     *
     * @param aContent The trace_sch file content.
     * @return Vector of parsed elements.
     */
    std::vector<TRACE_ELEMENT> ParseTraceSchContent( const std::string& aContent );

    /**
     * Analyze differences between old and new trace_sch content.
     *
     * @param aOldContent The original trace_sch content.
     * @param aNewContent The new trace_sch content.
     * @return Diff result with added/removed/modified elements.
     */
    DIFF_RESULT AnalyzeFileDiff( const std::string& aOldContent,
                                  const std::string& aNewContent );

    /**
     * Classify the complexity of a diff result.
     *
     * Determines if the diff is simple enough for incremental update
     * or requires a full reload.
     *
     * @param aDiff The diff result to classify.
     */
    void ClassifyComplexity( DIFF_RESULT& aDiff );

private:
    /**
     * Parse a single line from trace_sch content.
     *
     * @param aLine The line to parse.
     * @return Parsed element, or nullopt if not an element line.
     */
    std::optional<TRACE_ELEMENT> ParseLine( const std::string& aLine );

    /**
     * Extract a quoted string value from a line.
     *
     * @param aLine The line to search.
     * @param aPrefix The prefix before the quoted value (e.g., "ref=").
     * @return The extracted value, or empty string if not found.
     */
    std::string ExtractQuotedValue( const std::string& aLine, const std::string& aPrefix );

    /**
     * Extract a numeric value from a line.
     *
     * @param aLine The line to search.
     * @param aPrefix The prefix before the value (e.g., "at=").
     * @param aIndex Which number to extract (0-indexed).
     * @return The extracted value, or 0.0 if not found.
     */
    double ExtractNumericValue( const std::string& aLine, const std::string& aPrefix,
                                int aIndex = 0 );

    /**
     * Extract uid from a line.
     */
    std::string ExtractUid( const std::string& aLine );
};

#endif // AI_DIFF_ANALYZER_H

