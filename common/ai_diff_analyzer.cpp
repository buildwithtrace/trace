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
#include <cstdio>
#include <functional>


static std::string FormatCoord( double aVal )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%.3f", aVal );
    return buf;
}


std::string TRACE_ELEMENT::GetKey() const
{
    if( !uid.empty() )
        return uid;

    std::hash<std::string> hasher;

    // Schematic elements
    if( type == "component" && !ref.empty() )
        return "comp:" + ref;
    else if( type == "wire" )
        return "wire:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "label" && !name.empty() )
        return "label:" + name + ":" + FormatCoord( atX ) + ":" + FormatCoord( atY );
    else if( type == "glabel" && !name.empty() )
        return "glabel:" + name;
    else if( type == "hier" && !name.empty() )
        return "hier:" + name + ":" + FormatCoord( atX ) + ":" + FormatCoord( atY );
    else if( type == "net" && !name.empty() )
        return "net:" + name;
    else if( type == "junction" )
        return "junction:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "noconnect" )
        return "noconnect:" + FormatCoord( atX ) + ":" + FormatCoord( atY );
    else if( type == "bus" )
        return "bus:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "polyline" )
        return "polyline:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "rectangle" )
        return "rectangle:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "arc" )
        return "arc:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "bezier" )
        return "bezier:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "circle" )
        return "circle:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "text" )
        return "text:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "text_box" )
        return "text_box:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "bus_entry" )
        return "bus_entry:" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "cwire" && !ref.empty() )
        return "cwire:" + ref;
    else if( type == "cwiredef" && !ref.empty() )
        return "cwiredef:" + ref;
    else if( type == "sheet" && !name.empty() )
        return "sheet:" + name;
    else if( type == "file_uid" )
        return "file_uid:" + name;
    else if( type == "paper" )
        return "paper:" + name;
    else if( type == "title_block" )
        return "title_block:#" + std::to_string( hasher( rawContent ) );
    else if( type == "inst" )
        return "inst:" + name + ":" + value;
    // PCB elements
    else if( type == "footprint" && !ref.empty() )
        return "fp:" + ref;
    else if( type == "track" )
        return "track:" + layer + ":" + FormatCoord( atX ) + ":" + FormatCoord( atY )
               + "#" + std::to_string( hasher( rawContent ) );
    else if( type == "via" )
        return "via:" + FormatCoord( atX ) + ":" + FormatCoord( atY );
    else if( type == "zone" && !name.empty() )
        return "zone:" + name + ":" + layer;
    else if( type == "gr_line" || type == "gr_rect" || type == "gr_circle" || type == "gr_arc" )
        return type + ":" + layer + ":" + FormatCoord( atX ) + ":" + FormatCoord( atY );

    return type + ":#" + std::to_string( hasher( rawContent ) );
}


bool TRACE_ELEMENT::Equals( const TRACE_ELEMENT& aOther ) const
{
    if( type != aOther.type )
        return false;

    // Schematic elements
    if( type == "component" )
    {
        return ref == aOther.ref && symbol == aOther.symbol &&
               std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               rotation == aOther.rotation && value == aOther.value;
    }
    else if( type == "wire" || type == "bus" || type == "polyline" || type == "bezier" )
    {
        return rawContent == aOther.rawContent;
    }
    else if( type == "label" )
    {
        return name == aOther.name && std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    else if( type == "glabel" || type == "hier" )
    {
        return name == aOther.name && std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               rotation == aOther.rotation;
    }
    else if( type == "net" )
    {
        return name == aOther.name;
    }
    else if( type == "junction" || type == "noconnect" || type == "bus_entry" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    else if( type == "text" || type == "text_box" )
    {
        return name == aOther.name && std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001;
    }
    else if( type == "rectangle" )
    {
        return rawContent == aOther.rawContent;
    }
    else if( type == "arc" )
    {
        return rawContent == aOther.rawContent;
    }
    else if( type == "circle" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               std::abs( width - aOther.width ) < 0.001;
    }
    else if( type == "cwire" || type == "cwiredef" )
    {
        return rawContent == aOther.rawContent;
    }
    else if( type == "sheet" )
    {
        return name == aOther.name && rawContent == aOther.rawContent;
    }
    else if( type == "file_uid" || type == "paper" || type == "title_block" || type == "inst" )
    {
        return rawContent == aOther.rawContent;
    }
    // PCB elements
    else if( type == "footprint" )
    {
        return ref == aOther.ref && symbol == aOther.symbol &&
               std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               rotation == aOther.rotation && layer == aOther.layer;
    }
    else if( type == "track" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               layer == aOther.layer &&
               std::abs( width - aOther.width ) < 0.001 &&
               net == aOther.net;
    }
    else if( type == "via" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               net == aOther.net;
    }
    else if( type == "zone" )
    {
        return name == aOther.name && layer == aOther.layer;
    }
    else if( type == "gr_line" || type == "gr_rect" || type == "gr_circle" || type == "gr_arc" )
    {
        return std::abs( atX - aOther.atX ) < 0.001 &&
               std::abs( atY - aOther.atY ) < 0.001 &&
               layer == aOther.layer;
    }

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
    std::string trimmed = aLine;
    size_t      start = trimmed.find_first_not_of( " \t" );
    if( start == std::string::npos )
        return std::nullopt;
    trimmed = trimmed.substr( start );

    if( trimmed.empty() || trimmed[0] == '#' )
        return std::nullopt;

    TRACE_ELEMENT element;
    element.rawContent = aLine;

    // Grammar keyword is "comp", internal type remains "component" for downstream compat
    if( trimmed.rfind( "comp ", 0 ) == 0 )
    {
        element.type = "component";
        element.ref = ExtractQuotedValue( trimmed, "ref=" );
        element.symbol = ExtractQuotedValue( trimmed, "symbol=" );
        element.value = ExtractQuotedValue( trimmed, "value=" );
        element.uid = ExtractUid( trimmed );
        // Grammar uses @ coord syntax: comp R1 Device:R @ 100,50
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
        element.rotation = static_cast<int>( ExtractNumericValue( trimmed, "rot ", 0 ) );
    }
    else if( trimmed.rfind( "wire ", 0 ) == 0 )
    {
        element.type = "wire";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "wire ", 0 );
        element.atY = ExtractNumericValue( trimmed, "wire ", 1 );
    }
    else if( trimmed.rfind( "cwire ", 0 ) == 0 )
    {
        element.type = "cwire";
        element.ref = ExtractFirstToken( trimmed, "cwire " );
        element.rawContent = aLine;
    }
    else if( trimmed.rfind( "cwiredef ", 0 ) == 0 )
    {
        element.type = "cwiredef";
        element.ref = ExtractFirstToken( trimmed, "cwiredef " );
        element.atX = ExtractNumericValue( trimmed, "cwiredef ", 1 );
        element.atY = ExtractNumericValue( trimmed, "cwiredef ", 2 );
    }
    else if( trimmed.rfind( "junction ", 0 ) == 0 )
    {
        element.type = "junction";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "noconnect ", 0 ) == 0 )
    {
        element.type = "noconnect";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "label ", 0 ) == 0 )
    {
        element.type = "label";
        element.name = ExtractQuotedValue( trimmed, "label " );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "glabel ", 0 ) == 0 )
    {
        element.type = "glabel";
        element.name = ExtractFirstToken( trimmed, "glabel " );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
        element.rotation = static_cast<int>( ExtractNumericValue( trimmed, "rot ", 0 ) );
    }
    else if( trimmed.rfind( "hier ", 0 ) == 0 )
    {
        element.type = "hier";
        element.name = ExtractFirstToken( trimmed, "hier " );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
        element.rotation = static_cast<int>( ExtractNumericValue( trimmed, "rot ", 0 ) );
    }
    else if( trimmed.rfind( "net ", 0 ) == 0 )
    {
        element.type = "net";
        element.name = ExtractFirstToken( trimmed, "net " );
    }
    else if( trimmed.rfind( "text ", 0 ) == 0 )
    {
        element.type = "text";
        element.name = ExtractQuotedValue( trimmed, "text " );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "text_box ", 0 ) == 0 )
    {
        element.type = "text_box";
        element.name = ExtractQuotedValue( trimmed, "text_box " );
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "bus ", 0 ) == 0 )
    {
        element.type = "bus";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "bus ", 0 );
        element.atY = ExtractNumericValue( trimmed, "bus ", 1 );
    }
    else if( trimmed.rfind( "bus_entry ", 0 ) == 0 )
    {
        element.type = "bus_entry";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "@", 0 );
        element.atY = ExtractNumericValue( trimmed, "@", 1 );
    }
    else if( trimmed.rfind( "polyline ", 0 ) == 0 )
    {
        element.type = "polyline";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "polyline ", 0 );
        element.atY = ExtractNumericValue( trimmed, "polyline ", 1 );
    }
    else if( trimmed.rfind( "rectangle ", 0 ) == 0 )
    {
        element.type = "rectangle";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "start ", 0 );
        element.atY = ExtractNumericValue( trimmed, "start ", 1 );
    }
    else if( trimmed.rfind( "arc ", 0 ) == 0 )
    {
        element.type = "arc";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "start ", 0 );
        element.atY = ExtractNumericValue( trimmed, "start ", 1 );
    }
    else if( trimmed.rfind( "bezier ", 0 ) == 0 )
    {
        element.type = "bezier";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "bezier ", 0 );
        element.atY = ExtractNumericValue( trimmed, "bezier ", 1 );
    }
    else if( trimmed.rfind( "circle ", 0 ) == 0 )
    {
        element.type = "circle";
        element.uid = ExtractUid( trimmed );
        element.atX = ExtractNumericValue( trimmed, "center ", 0 );
        element.atY = ExtractNumericValue( trimmed, "center ", 1 );
        element.width = ExtractNumericValue( trimmed, "radius ", 0 );
    }
    else if( trimmed.rfind( "sheet ", 0 ) == 0 )
    {
        element.type = "sheet";
        element.name = ExtractFirstToken( trimmed, "sheet " );
        element.uid = ExtractUid( trimmed );
    }
    else if( trimmed.rfind( "inst ", 0 ) == 0 )
    {
        element.type = "inst";
        element.name = ExtractQuotedValue( trimmed, "project " );
        element.value = ExtractQuotedValue( trimmed, "path " );
    }
    else if( trimmed.rfind( "file_uid ", 0 ) == 0 )
    {
        element.type = "file_uid";
        element.name = ExtractFirstToken( trimmed, "file_uid " );
    }
    else if( trimmed.rfind( "paper ", 0 ) == 0 )
    {
        element.type = "paper";
        element.name = ExtractQuotedValue( trimmed, "paper " );
    }
    else if( trimmed.rfind( "title_block", 0 ) == 0 )
    {
        element.type = "title_block";
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

    while( startPos < aLine.length() && ( aLine[startPos] == ' ' || aLine[startPos] == '\t' ) )
        startPos++;

    if( startPos >= aLine.length() )
        return "";

    char quoteChar = aLine[startPos];
    if( quoteChar == '"' || quoteChar == '\'' )
    {
        startPos++;
        size_t endPos = aLine.find( quoteChar, startPos );
        if( endPos == std::string::npos )
            return "";
        return aLine.substr( startPos, endPos - startPos );
    }

    size_t endPos = aLine.find_first_of( " \t,]}", startPos );
    if( endPos == std::string::npos )
        endPos = aLine.length();
    return aLine.substr( startPos, endPos - startPos );
}


std::string AI_DIFF_ANALYZER::ExtractFirstToken( const std::string& aLine,
                                                  const std::string& aPrefix )
{
    size_t prefixPos = aLine.find( aPrefix );
    if( prefixPos == std::string::npos )
        return "";

    size_t startPos = prefixPos + aPrefix.length();

    while( startPos < aLine.length() && ( aLine[startPos] == ' ' || aLine[startPos] == '\t' ) )
        startPos++;

    if( startPos >= aLine.length() )
        return "";

    if( aLine[startPos] == '"' )
        return ExtractQuotedValue( aLine, aPrefix );

    size_t endPos = aLine.find_first_of( " \t", startPos );
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

    size_t bracketPos = aLine.find( '[', startPos );
    if( bracketPos != std::string::npos && bracketPos < startPos + 5 )
        startPos = bracketPos + 1;

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
    // Check uuid= first (longer prefix) to avoid "uid=" matching inside "uuid="
    std::string val = ExtractQuotedValue( aLine, "uuid=" );
    if( !val.empty() )
        return val;

    // Match " uid=" (with leading space) to prevent substring match inside other keywords
    val = ExtractQuotedValue( aLine, " uid " );
    if( !val.empty() )
        return val;

    return "";
}


DIFF_RESULT AI_DIFF_ANALYZER::AnalyzeFileDiff( const std::string& aOldContent,
                                                const std::string& aNewContent )
{
    DIFF_RESULT result;

    try
    {
        auto oldElements = ParseTraceSchContent( aOldContent );
        auto newElements = ParseTraceSchContent( aNewContent );

        // Build maps, disambiguating duplicate keys with counter suffixes
        std::map<std::string, TRACE_ELEMENT> oldMap;
        std::map<std::string, int>           oldKeyCounts;

        for( const auto& elem : oldElements )
        {
            std::string baseKey = elem.GetKey();
            if( baseKey.empty() )
                continue;

            int& count = oldKeyCounts[baseKey];
            std::string key = ( count == 0 ) ? baseKey : baseKey + "##" + std::to_string( count );
            count++;
            oldMap[key] = elem;
        }

        std::map<std::string, TRACE_ELEMENT> newMap;
        std::map<std::string, int>           newKeyCounts;

        for( const auto& elem : newElements )
        {
            std::string baseKey = elem.GetKey();
            if( baseKey.empty() )
                continue;

            int& count = newKeyCounts[baseKey];
            std::string key = ( count == 0 ) ? baseKey : baseKey + "##" + std::to_string( count );
            count++;
            newMap[key] = elem;
        }

        for( const auto& [key, elem] : newMap )
        {
            if( oldMap.find( key ) == oldMap.end() )
                result.added.push_back( elem );
        }

        for( const auto& [key, elem] : oldMap )
        {
            if( newMap.find( key ) == newMap.end() )
                result.removed.push_back( elem );
        }

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

    if( totalChanges == 0 )
    {
        aDiff.isSimple = true;
        aDiff.complexityReason = "No changes";
        return;
    }

    if( totalChanges == 1 )
    {
        aDiff.isSimple = true;
        aDiff.complexityReason = "Single element change";
        return;
    }

    if( totalChanges > 5 )
    {
        aDiff.isSimple = false;
        aDiff.complexityReason = "Too many changes (" + std::to_string( totalChanges ) + ")";
        return;
    }

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

    bool allPropertyChanges = true;
    for( const auto& mod : aDiff.modified )
    {
        if( mod.oldElement.type == "component" && mod.newElement.type == "component" )
        {
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

    aDiff.isSimple = true;
    aDiff.complexityReason = "Moderate changes (" + std::to_string( totalChanges ) + " elements)";
}
