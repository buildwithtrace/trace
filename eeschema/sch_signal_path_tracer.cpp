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

#include "sch_signal_path_tracer.h"

#include <schematic.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <sch_connection.h>
#include <connection_graph.h>
#include <trace_helpers.h>

#include <deque>
#include <map>
#include <set>


SCH_SIGNAL_PATH_TRACER::SCH_SIGNAL_PATH_TRACER( SCHEMATIC* aSchematic ) :
        m_schematic( aSchematic ),
        m_connGraph( aSchematic ? aSchematic->ConnectionGraph() : nullptr )
{
}


bool SCH_SIGNAL_PATH_TRACER::isPowerNet( const wxString& aNetName ) const
{
    if( !m_connGraph )
        return false;

    CONNECTION_SUBGRAPH* sg = m_connGraph->FindFirstSubgraphByName( aNetName );

    if( sg )
    {
        CONNECTION_SUBGRAPH::PRIORITY priority = sg->GetDriverPriority();

        if( priority == CONNECTION_SUBGRAPH::PRIORITY::GLOBAL_POWER_PIN
            || priority == CONNECTION_SUBGRAPH::PRIORITY::LOCAL_POWER_PIN )
        {
            return true;
        }
    }

    return false;
}


wxString SCH_SIGNAL_PATH_TRACER::resolveNetFromComponent( const wxString& aComponent,
                                                           const wxString& aPinName,
                                                           nlohmann::json& aResult )
{
    SCH_SHEET_LIST sheetList = m_schematic->Hierarchy();

    for( const SCH_SHEET_PATH& sheet : sheetList )
    {
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
            wxString ref = symbol->GetRef( &sheet, false );

            if( ref != aComponent )
                continue;

            std::vector<SCH_PIN*> pins = symbol->GetPins( &sheet );

            for( SCH_PIN* pin : pins )
            {
                bool nameMatch = pin->GetShownName().IsSameAs( aPinName, false );
                bool numberMatch = pin->GetNumber().IsSameAs( aPinName, false );

                if( nameMatch || numberMatch )
                {
                    SCH_CONNECTION* conn = pin->Connection( &sheet );

                    if( conn && !conn->Name().IsEmpty() )
                    {
                        aResult["resolved_from"] = nlohmann::json{
                            { "component", aComponent.ToStdString() },
                            { "pin_name", pin->GetShownName().ToStdString() },
                            { "pin_number", pin->GetNumber().ToStdString() },
                            { "net_name", conn->Name().ToStdString() },
                            { "sheet", sheet.GetPageNumber().ToStdString() }
                        };
                        return conn->Name();
                    }
                }
            }
        }
    }

    return wxEmptyString;
}


nlohmann::json SCH_SIGNAL_PATH_TRACER::TraceFromComponent( const wxString& aComponent,
                                                             const wxString& aPinName,
                                                             int aMaxHops )
{
    nlohmann::json result;
    result["success"] = false;

    if( !m_schematic || !m_connGraph )
    {
        result["error"] = "No schematic or connection graph available";
        return result;
    }

    if( aComponent.IsEmpty() )
    {
        result["error"] = "Component reference is empty";
        return result;
    }

    wxString netName = resolveNetFromComponent( aComponent, aPinName, result );

    if( netName.IsEmpty() )
    {
        wxString pinDesc = aPinName.IsEmpty() ? wxString( wxT( "(any)" ) ) : aPinName;
        result["error"] = wxString::Format( wxT( "Could not resolve net for %s pin %s" ),
                                            aComponent, pinDesc ).ToStdString();
        return result;
    }

    nlohmann::json traceResult = TraceFromNet( netName, aMaxHops );

    if( traceResult.contains( "success" ) && traceResult["success"].get<bool>()
        && result.contains( "resolved_from" ) )
    {
        traceResult["resolved_from"] = result["resolved_from"];
    }

    return traceResult;
}


nlohmann::json SCH_SIGNAL_PATH_TRACER::TraceFromNet( const wxString& aNetName, int aMaxHops )
{
    nlohmann::json result;
    result["success"] = false;

    if( !m_schematic || !m_connGraph )
    {
        result["error"] = "No schematic or connection graph available";
        return result;
    }

    if( aNetName.IsEmpty() )
    {
        result["error"] = "Net name is empty";
        return result;
    }

    wxLogTrace( traceAiToolCall, wxT( "SCH_SIGNAL_PATH_TRACER::TraceFromNet net='%s' maxHops=%d" ),
                aNetName, aMaxHops );

    const std::vector<CONNECTION_SUBGRAPH*>& startSubgraphs =
            m_connGraph->GetAllSubgraphs( aNetName );

    if( startSubgraphs.empty() )
    {
        result["error"] = std::string( "Net not found: " ) + aNetName.ToStdString();
        return result;
    }

    struct BFS_ENTRY
    {
        wxString netName;
        int      hop;
    };

    std::deque<BFS_ENTRY>  frontier;
    std::set<wxString>     enqueuedNets;  // nets already pushed onto the frontier (cycle guard)
    std::vector<nlohmann::json> pathsJson;

    // reference -> { json component info, first_hop }
    struct COMPONENT_ENTRY
    {
        nlohmann::json json;
        int            firstHop;
    };

    std::map<wxString, COMPONENT_ENTRY> componentMap;

    frontier.push_back( { aNetName, 0 } );
    enqueuedNets.insert( aNetName );

    while( !frontier.empty() )
    {
        BFS_ENTRY current = frontier.front();
        frontier.pop_front();

        if( current.hop > aMaxHops )
            continue;

        const std::vector<CONNECTION_SUBGRAPH*>& subgraphs =
                m_connGraph->GetAllSubgraphs( current.netName );

        for( const CONNECTION_SUBGRAPH* sg : subgraphs )
        {
            const SCH_SHEET_PATH& sheet = sg->GetSheet();

            for( SCH_ITEM* item : sg->GetItems() )
            {
                if( item->Type() != SCH_PIN_T )
                    continue;

                SCH_PIN* pin = static_cast<SCH_PIN*>( item );
                SYMBOL* parentSym = pin->GetParentSymbol();

                if( !parentSym )
                    continue;

                SCH_SYMBOL* symbol = dynamic_cast<SCH_SYMBOL*>( parentSym );

                if( !symbol )
                    continue;

                wxString ref = symbol->GetRef( &sheet, false );
                wxString value = symbol->GetValue( false, &sheet, false );

                // Record this component (first-seen hop wins)
                if( componentMap.find( ref ) == componentMap.end() )
                {
                    nlohmann::json comp;
                    comp["reference"] = ref.ToStdString();
                    comp["value"] = value.ToStdString();
                    comp["pins"] = nlohmann::json::array();
                    comp["hop"] = current.hop;
                    comp["is_source"] = ( current.hop == 0 );

                    componentMap[ref] = { comp, current.hop };
                }

                // Add pin info (deduplicated)
                nlohmann::json pinInfo;
                pinInfo["number"] = pin->GetNumber().ToStdString();
                pinInfo["name"] = pin->GetShownName().ToStdString();
                pinInfo["net"] = current.netName.ToStdString();

                bool pinAlreadyListed = false;

                for( const auto& existing : componentMap[ref].json["pins"] )
                {
                    if( existing["number"] == pinInfo["number"]
                        && existing["net"] == pinInfo["net"] )
                    {
                        pinAlreadyListed = true;
                        break;
                    }
                }

                if( !pinAlreadyListed )
                    componentMap[ref].json["pins"].push_back( pinInfo );

                // Hop through this symbol: find other pins on different nets
                std::vector<SCH_PIN*> symbolPins = symbol->GetPins( &sheet );

                for( SCH_PIN* otherPin : symbolPins )
                {
                    if( otherPin == pin )
                        continue;

                    SCH_CONNECTION* conn = otherPin->Connection( &sheet );

                    if( !conn )
                        continue;

                    wxString otherNet = conn->Name();

                    if( otherNet.IsEmpty() || otherNet == current.netName )
                        continue;

                    // Always emit a path entry (even if net was already visited via another route)
                    nlohmann::json pathJson;
                    pathJson["start_net"] = current.netName.ToStdString();
                    pathJson["end_net"] = otherNet.ToStdString();

                    nlohmann::json nodesJson = nlohmann::json::array();

                    nlohmann::json entryNode;
                    entryNode["net"] = current.netName.ToStdString();
                    entryNode["pin"] = pin->GetNumber().ToStdString();
                    entryNode["pin_name"] = pin->GetShownName().ToStdString();
                    entryNode["component"] = ref.ToStdString();
                    entryNode["component_value"] = value.ToStdString();
                    entryNode["sheet"] = sheet.GetPageNumber().ToStdString();
                    nodesJson.push_back( entryNode );

                    nlohmann::json exitNode;
                    exitNode["net"] = otherNet.ToStdString();
                    exitNode["pin"] = otherPin->GetNumber().ToStdString();
                    exitNode["pin_name"] = otherPin->GetShownName().ToStdString();
                    exitNode["component"] = ref.ToStdString();
                    exitNode["component_value"] = value.ToStdString();
                    exitNode["sheet"] = sheet.GetPageNumber().ToStdString();
                    nodesJson.push_back( exitNode );

                    pathJson["nodes"] = nodesJson;
                    pathJson["hop"] = current.hop;
                    pathsJson.push_back( pathJson );

                    // Only enqueue for BFS if not already queued (prevents cycles)
                    if( !enqueuedNets.count( otherNet )
                        && !isPowerNet( otherNet )
                        && current.hop + 1 <= aMaxHops )
                    {
                        frontier.push_back( { otherNet, current.hop + 1 } );
                        enqueuedNets.insert( otherNet );
                    }
                }
            }
        }
    }

    nlohmann::json componentsJson = nlohmann::json::array();

    for( const auto& [ref, entry] : componentMap )
        componentsJson.push_back( entry.json );

    result["success"] = true;
    result["start_net"] = aNetName.ToStdString();
    result["max_hops"] = aMaxHops;
    result["paths"] = pathsJson;
    result["connected_components"] = componentsJson;
    result["nets_visited"] = nlohmann::json::array();

    for( const wxString& net : enqueuedNets )
        result["nets_visited"].push_back( net.ToStdString() );

    wxLogTrace( traceAiToolCall,
                wxT( "SCH_SIGNAL_PATH_TRACER::TraceFromNet completed: %zu paths, %zu components" ),
                pathsJson.size(), componentsJson.size() );

    return result;
}
