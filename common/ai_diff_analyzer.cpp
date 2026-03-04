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

#include "ai_diff_analyzer.h"

#include <sstream>
#include <algorithm>
#include <cmath>
#include <regex>
#include <iostream>


std::string TRACE_ELEMENT::GetKey() const
{
    // Use uid if available
    if( !uid.empty() )
        return uid;

    // Fallback: composite key based on type
    // Schematic elements
    if( type == "component" && !ref.empty() )
        return "comp:" + ref;
    else if( type == "wire" )
        return "wire:" + std::to_string( atX ) + ":" + std::to_string( atY );
    else if( type == "label" && !name.empty() )
        return "label:" + name + ":" + std::to_string( atX ) + ":" + std::to_string( atY );
    else if( type == "glabel" && !name.empty() )
        return "glabel:" + name;
    else if( type == "net" && !name.empty() )
        return "net:" + name;
    else if( type == "junction" )
        return "junction:" + std::to_string( atX ) + ":" + std::to_string( atY );
    else if( type == "noconnect" )
        return "noconnect:" + std::to_string( atX ) + ":" + std::to_string( atY );
    // PCB elements
    else if( type == "footprint" && !ref.empty() )
        return "fp:" + ref;
    else if( type == "track" )
        return "track:" + layer + ":" + std::to_string( atX ) + ":" + std::to_string( atY );
    else if( type == "via" )
        return "via:" + std::to_string( atX ) + ":" + std::to_string( atY );
    else if( type == "zone" && !name.empty() )
        return "zone:" + name + ":" + layer;
    else if( type == "gr_line" || type == "gr_rect" || type == "gr_circle" || type == "gr_arc" )
        return type + ":" + layer + ":" + std::to_string( atX ) + ":" + std::to_string( atY );

    // Last resort: type + raw content hash
    std::hash<std::string> hasher;
    return type + ":" + std::to_string( hasher( rawContent ) );
}


bool TRACE_ELEMENT::Equals( const TRACE_ELEMENT& aOther ) const
{
    if( type != aOther.type )
        return false;

    // Schematic elements
    // For components, compare key fields
    if( type == "component" )
    {
        return ref == aOther.ref && symbol == aOther.symbol &&
               std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               rotation == aOther.rotation && value == aOther.value;
    }
    // For wires, compare position
    else if( type == "wire" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    // For labels, compare name and position
    else if( type == "label" )
    {
        return name == aOther.name && std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    // For glabels and nets, compare name
    else if( type == "glabel" || type == "net" )
    {
        return name == aOther.name;
    }
    // For junctions and noconnects, compare position
    else if( type == "junction" || type == "noconnect" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    // PCB elements
    // For footprints, compare ref, symbol, position, rotation
    else if( type == "footprint" )
    {
        return ref == aOther.ref && symbol == aOther.symbol &&
               std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               rotation == aOther.rotation && layer == aOther.layer;
    }
    // For tracks, compare position, layer, width, net
    else if( type == "track" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               layer == aOther.layer &&
               std::abs( width - aOther.width ) < 0.001 &&
               net == aOther.net;
    }
    // For vias, compare position and net
    else if( type == "via" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               net == aOther.net;
    }
    // For zones, compare name and layer
    else if( type == "zone" )
    {
        return name == aOther.name && layer == aOther.layer;
    }
    // For graphic elements, compare position and layer
    else if( type == "gr_line" || type == "gr_rect" || type == "gr_circle" || type == "gr_arc" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               layer == aOther.layer;
    }

    // For other types, compare raw content
    return rawContent == aOther.rawContent;
}


std::vector<TRACE_ELEMENT> AI_DIFF_ANALYZER::ParseTraceSchContent( const std::string& aContent )
{
    std::vector<TRACE_ELEMENT> elements;
    std::istringstream         stream( aContent );
    std::string                line;

    while( std::getline( stream, line ) )
    {
        auto element = ParseLine( line );
        if( element.has_value() )
            elements.push_back( element.value() );
    }

    return elements;
}


std::optional<TRACE_ELEMENT> AI_DIFF_ANALYZER::ParseLine( const std::string& aLine )
{
    // Skip empty lines and comments
    std::string trimmed = aLine;
    size_t      start = trimmed.find_first_not_of( " \t" );
    if( start == std::string::npos )
        return std::nullopt;
    trimmed = trimmed.substr( start );

    if( trimmed.empty() || trimmed[0] == '#' )
        return std::nullopt;

    TRACE_ELEMENT element;
    element.rawContent = aLine;

    // Detect element type by prefix
    // trace_sch format: component ref="R1" symbol="Device:R" at=[100, 50] ...
    if( trimmed.rfind( "component ", 0 ) == 0 )
    {
        element.type = "component";
        element.ref = ExtractQuotedValue( trimmed, "ref=" );
        element.symbol = ExtractQuotedValue( trimmed, "symbol=" );
        element.value = ExtractQuotedValue( trimmed, "value=" );
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
        element.rotation = static_cast<int>( ExtractNumericValue( trimmed, "rot=", 0 ) );
    }
    else if( trimmed.rfind( "wire ", 0 ) == 0 )
    {
        element.type = "wire";
        element.uid = ExtractUid( trimmed );
        // Wire has points, use first point as position
        element.atX = ExtractNumericValue( trimmed, "points=", 0 );
        element.atY = ExtractNumericValue( trimmed, "points=", 1 );
    }
    else if( trimmed.rfind( "junction ", 0 ) == 0 )
    {
        element.type = "junction";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
    }
    else if( trimmed.rfind( "noconnect ", 0 ) == 0 )
    {
        element.type = "noconnect";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
    }
    else if( trimmed.rfind( "label ", 0 ) == 0 )
    {
        element.type = "label";
        element.name = ExtractQuotedValue( trimmed, "name=" );
        if( element.name.empty() )
            element.name = ExtractQuotedValue( trimmed, "label " );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
    }
    else if( trimmed.rfind( "glabel ", 0 ) == 0 )
    {
        element.type = "glabel";
        element.name = ExtractQuotedValue( trimmed, "name=" );
        if( element.name.empty() )
            element.name = ExtractQuotedValue( trimmed, "glabel " );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
    }
    else if( trimmed.rfind( "net ", 0 ) == 0 )
    {
        element.type = "net";
        element.name = ExtractQuotedValue( trimmed, "name=" );
        if( element.name.empty() )
            element.name = ExtractQuotedValue( trimmed, "net " );
    }
    else if( trimmed.rfind( "sheet ", 0 ) == 0 )
    {
        element.type = "sheet";
        element.name = ExtractQuotedValue( trimmed, "name=" );
        element.uid = ExtractUid( trimmed );
    }
    // PCB elements (trace_pcb format)
    else if( trimmed.rfind( "footprint ", 0 ) == 0 )
    {
        element.type = "footprint";
        element.ref = ExtractQuotedValue( trimmed, "ref=" );
        element.symbol = ExtractQuotedValue( trimmed, "footprint=" );
        if( element.symbol.empty() )
            element.symbol = ExtractQuotedValue( trimmed, "lib=" );
        element.value = ExtractQuotedValue( trimmed, "value=" );
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
        element.rotation = static_cast<int>( ExtractNumericValue( trimmed, "rot=", 0 ) );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else if( trimmed.rfind( "track ", 0 ) == 0 )
    {
        element.type = "track";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "start=", 0 );
        element.atY = ExtractNumericValue( trimmed, "start=", 1 );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
        element.width = ExtractNumericValue( trimmed, "width=", 0 );
        element.net = ExtractQuotedValue( trimmed, "net=" );
    }
    else if( trimmed.rfind( "via ", 0 ) == 0 )
    {
        element.type = "via";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "at=", 0 );
        element.atY = ExtractNumericValue( trimmed, "at=", 1 );
        element.net = ExtractQuotedValue( trimmed, "net=" );
    }
    else if( trimmed.rfind( "zone ", 0 ) == 0 )
    {
        element.type = "zone";
        element.uid = ExtractUid( trimmed );
        element.name = ExtractQuotedValue( trimmed, "net=" );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else if( trimmed.rfind( "gr_line ", 0 ) == 0 )
    {
        element.type = "gr_line";
        element.atX = ExtractNumericValue( trimmed, "start=", 0 );
        element.atY = ExtractNumericValue( trimmed, "start=", 1 );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else if( trimmed.rfind( "gr_rect ", 0 ) == 0 )
    {
        element.type = "gr_rect";
        element.atX = ExtractNumericValue( trimmed, "start=", 0 );
        element.atY = ExtractNumericValue( trimmed, "start=", 1 );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else if( trimmed.rfind( "gr_circle ", 0 ) == 0 )
    {
        element.type = "gr_circle";
        element.atX = ExtractNumericValue( trimmed, "center=", 0 );
        element.atY = ExtractNumericValue( trimmed, "center=", 1 );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else if( trimmed.rfind( "gr_arc ", 0 ) == 0 )
    {
        element.type = "gr_arc";
        element.atX = ExtractNumericValue( trimmed, "start=", 0 );
        element.atY = ExtractNumericValue( trimmed, "start=", 1 );
        element.layer = ExtractQuotedValue( trimmed, "layer=" );
    }
    else
    {
        // Not an element we track
        return std::nullopt;
    }

    return element;
}


std::string AI_DIFF_ANALYZER::ExtractQuotedValue( const std::string& aLine,
                                                   const std::string& aPrefix )
{
    size_t prefixPos = aLine.find( aPrefix );
    if( prefixPos == std::string::npos )
        return "";

    size_t startPos = prefixPos + aPrefix.length();

    // Skip whitespace
    while( startPos < aLine.length() && ( aLine[startPos] == ' ' || aLine[startPos] == '\t' ) )
        startPos++;

    if( startPos >= aLine.length() )
        return "";

    // Check for quote
    char quoteChar = aLine[startPos];
    if( quoteChar == '"' || quoteChar == '\'' )
    {
        startPos++;
        size_t endPos = aLine.find( quoteChar, startPos );
        if( endPos == std::string::npos )
            return "";
        return aLine.substr( startPos, endPos - startPos );
    }

    // No quotes - read until whitespace or special char
    size_t endPos = aLine.find_first_of( " \t,]}", startPos );
    if( endPos == std::string::npos )
        endPos = aLine.length();
    return aLine.substr( startPos, endPos - startPos );
}


double AI_DIFF_ANALYZER::ExtractNumericValue( const std::string& aLine, const std::string& aPrefix,
                                               int aIndex )
{
    size_t prefixPos = aLine.find( aPrefix );
    if( prefixPos == std::string::npos )
        return 0.0;

    size_t startPos = prefixPos + aPrefix.length();

    // Skip to opening bracket if present
    size_t bracketPos = aLine.find( '[', startPos );
    if( bracketPos != std::string::npos && bracketPos < startPos + 5 )
        startPos = bracketPos + 1;

    // Find all numbers in the sequence
    std::regex  numberRegex( "-?[0-9]+\\.?[0-9]*" );
    std::string searchStr = aLine.substr( startPos );
    auto        numbersBegin = std::sregex_iterator( searchStr.begin(), searchStr.end(), numberRegex );
    auto        numbersEnd = std::sregex_iterator();

    int idx = 0;
    for( auto it = numbersBegin; it != numbersEnd; ++it )
    {
        if( idx == aIndex )
        {
            try
            {
                return std::stod( it->str() );
            }
            catch( ... )
            {
                return 0.0;
            }
        }
        idx++;
    }

    return 0.0;
}


std::string AI_DIFF_ANALYZER::ExtractUid( const std::string& aLine )
{
    // Look for uid="..." or uuid="..."
    std::string uid = ExtractQuotedValue( aLine, "uid=" );
    if( !uid.empty() )
        return uid;

    return ExtractQuotedValue( aLine, "uuid=" );
}


DIFF_RESULT AI_DIFF_ANALYZER::AnalyzeFileDiff( const std::string& aOldContent,
                                                const std::string& aNewContent )
{
    DIFF_RESULT result;

    try
    {
        // Parse both contents
        auto oldElements = ParseTraceSchContent( aOldContent );
        auto newElements = ParseTraceSchContent( aNewContent );

        // Build maps by key
        std::map<std::string, TRACE_ELEMENT> oldMap;
        std::map<std::string, TRACE_ELEMENT> newMap;

        for( const auto& elem : oldElements )
        {
            std::string key = elem.GetKey();
            if( !key.empty() )
                oldMap[key] = elem;
        }

        for( const auto& elem : newElements )
        {
            std::string key = elem.GetKey();
            if( !key.empty() )
                newMap[key] = elem;
        }

        // Find added elements (in new but not in old)
        for( const auto& [key, elem] : newMap )
        {
            if( oldMap.find( key ) == oldMap.end() )
                result.added.push_back( elem );
        }

        // Find removed elements (in old but not in new)
        for( const auto& [key, elem] : oldMap )
        {
            if( newMap.find( key ) == newMap.end() )
                result.removed.push_back( elem );
        }

        // Find modified elements (in both but different)
        for( const auto& [key, oldElem] : oldMap )
        {
            auto it = newMap.find( key );
            if( it != newMap.end() )
            {
                if( !oldElem.Equals( it->second ) )
                {
                    ELEMENT_MODIFICATION mod;
                    mod.oldElement = oldElem;
                    mod.newElement = it->second;
                    result.modified.push_back( mod );
                }
            }
        }

        // Classify complexity
        ClassifyComplexity( result );
    }
    catch( const std::exception& e )
    {
        result.isSimple = false;
        result.complexityReason = std::string( "Error during diff analysis: " ) + e.what();
    }

    return result;
}


void AI_DIFF_ANALYZER::ClassifyComplexity( DIFF_RESULT& aDiff )
{
    size_t numAdded = aDiff.added.size();
    size_t numRemoved = aDiff.removed.size();
    size_t numModified = aDiff.modified.size();
    size_t totalChanges = numAdded + numRemoved + numModified;

    // No changes - simple
    if( totalChanges == 0 )
    {
        aDiff.isSimple = true;
        aDiff.complexityReason = "No changes";
        return;
    }

    // Single element change - simple
    if( totalChanges == 1 )
    {
        aDiff.isSimple = true;
        aDiff.complexityReason = "Single element change";
        return;
    }

    // Too many changes - complex
    if( totalChanges > 5 )
    {
        aDiff.isSimple = false;
        aDiff.complexityReason = "Too many changes (" + std::to_string( totalChanges ) + ")";
        return;
    }

    // Count component changes
    size_t componentChanges = 0;
    for( const auto& elem : aDiff.added )
    {
        if( elem.type == "component" )
            componentChanges++;
    }
    for( const auto& elem : aDiff.removed )
    {
        if( elem.type == "component" )
            componentChanges++;
    }
    for( const auto& mod : aDiff.modified )
    {
        if( mod.oldElement.type == "component" || mod.newElement.type == "component" )
            componentChanges++;
    }

    if( componentChanges > 2 )
    {
        aDiff.isSimple = false;
        aDiff.complexityReason =
                "Multiple component changes (" + std::to_string( componentChanges ) + ")";
        return;
    }

    // Count wire changes
    size_t wireChanges = 0;
    for( const auto& elem : aDiff.added )
    {
        if( elem.type == "wire" )
            wireChanges++;
    }
    for( const auto& elem : aDiff.removed )
    {
        if( elem.type == "wire" )
            wireChanges++;
    }
    for( const auto& mod : aDiff.modified )
    {
        if( mod.oldElement.type == "wire" || mod.newElement.type == "wire" )
            wireChanges++;
    }

    if( wireChanges > 1 )
    {
        aDiff.isSimple = false;
        aDiff.complexityReason =
                "Multiple wire changes (" + std::to_string( wireChanges ) + ") - may affect connectivity";
        return;
    }

    // Check for sheet changes - always complex
    for( const auto& elem : aDiff.added )
    {
        if( elem.type == "sheet" )
        {
            aDiff.isSimple = false;
            aDiff.complexityReason = "Hierarchical sheet changes require full reload";
            return;
        }
    }
    for( const auto& elem : aDiff.removed )
    {
        if( elem.type == "sheet" )
        {
            aDiff.isSimple = false;
            aDiff.complexityReason = "Hierarchical sheet changes require full reload";
            return;
        }
    }

    // Check if all modifications are property-only changes (value, ref changes but same position)
    bool allPropertyChanges = true;
    for( const auto& mod : aDiff.modified )
    {
        if( mod.oldElement.type == "component" && mod.newElement.type == "component" )
        {
            // Check if position and symbol are the same (just property changes)
            if( mod.oldElement.symbol != mod.newElement.symbol ||
                std::abs( mod.oldElement.atX - mod.newElement.atX ) > 0.001 ||
                std::abs( mod.oldElement.atY - mod.newElement.atY ) > 0.001 ||
                mod.oldElement.rotation != mod.newElement.rotation )
            {
                allPropertyChanges = false;
                break;
            }
        }
        else
        {
            allPropertyChanges = false;
            break;
        }
    }

    if( allPropertyChanges && numAdded == 0 && numRemoved == 0 )
    {
        aDiff.isSimple = true;
        aDiff.complexityReason = "Property-only changes";
        return;
    }

    // Default: moderate changes are simple
    aDiff.isSimple = true;
    aDiff.complexityReason = "Moderate changes (" + std::to_string( totalChanges ) + " elements)";
}

