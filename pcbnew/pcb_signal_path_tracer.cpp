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

#include "pcb_signal_path_tracer.h"

#include <board.h>
#include <footprint.h>
#include <pad.h>
#include <netinfo.h>
#include <netclass.h>
#include <connectivity/connectivity_data.h>
#include <trace_helpers.h>

#include <deque>
#include <map>
#include <set>


PCB_SIGNAL_PATH_TRACER::PCB_SIGNAL_PATH_TRACER( BOARD* aBoard ) :
        m_board( aBoard )
{
    m_fallbackPowerNets = { wxT( "GND" ),  wxT( "VCC" ),  wxT( "VDD" ),  wxT( "VSS" ),
                            wxT( "+3V3" ), wxT( "+5V" ),  wxT( "+12V" ), wxT( "+1V8" ),
                            wxT( "+3.3V" ), wxT( "+2.5V" ) };
}


bool PCB_SIGNAL_PATH_TRACER::isPowerNet( const wxString& aNetName ) const
{
    if( !m_board )
        return false;

    NETINFO_ITEM* net = m_board->FindNet( aNetName );

    if( net )
    {
        NETCLASS* nc = net->GetNetClass();

        if( nc )
        {
            wxString ncName = nc->GetName().Upper();

            if( ncName.Contains( wxT( "POWER" ) ) )
                return true;
        }
    }

    // Fallback: check against well-known power net names
    wxString upper = aNetName.Upper();

    for( const wxString& term : m_fallbackPowerNets )
    {
        if( upper == term )
            return true;
    }

    return false;
}


nlohmann::json PCB_SIGNAL_PATH_TRACER::TraceFromComponent( const wxString& aComponent,
                                                             const wxString& aPad,
                                                             int aMaxHops )
{
    nlohmann::json result;
    result["success"] = false;

    if( !m_board )
    {
        result["error"] = "No board available";
        return result;
    }

    if( aComponent.IsEmpty() )
    {
        result["error"] = "Component reference is empty";
        return result;
    }

    FOOTPRINT* fp = m_board->FindFootprintByReference( aComponent );

    if( !fp )
    {
        result["error"] = std::string( "Component not found: " ) + aComponent.ToStdString();
        return result;
    }

    wxString netName;

    if( !aPad.IsEmpty() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNumber() == aPad && pad->GetNetCode() > 0 )
            {
                netName = pad->GetNetname();
                break;
            }
        }

        if( netName.IsEmpty() )
        {
            result["error"] = wxString::Format( wxT( "Pad %s not found on %s or has no net" ),
                                                aPad, aComponent ).ToStdString();
            return result;
        }
    }
    else
    {
        // Use the first pad that has a non-power net
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 && !isPowerNet( pad->GetNetname() ) )
            {
                netName = pad->GetNetname();
                break;
            }
        }

        if( netName.IsEmpty() )
        {
            result["error"] = wxString::Format( wxT( "No signal pads found on %s" ),
                                                aComponent ).ToStdString();
            return result;
        }
    }

    result["resolved_from"] = nlohmann::json{
        { "component", aComponent.ToStdString() },
        { "pad", aPad.IsEmpty() ? "(auto)" : aPad.ToStdString() },
        { "net_name", netName.ToStdString() }
    };

    nlohmann::json traceResult = TraceFromNet( netName, aMaxHops );

    // Merge resolved_from into the trace result
    if( traceResult.contains( "success" ) && traceResult["success"].get<bool>() )
    {
        traceResult["resolved_from"] = result["resolved_from"];
    }

    return traceResult;
}


nlohmann::json PCB_SIGNAL_PATH_TRACER::TraceFromNet( const wxString& aNetName, int aMaxHops )
{
    nlohmann::json result;
    result["success"] = false;

    if( !m_board )
    {
        result["error"] = "No board available";
        return result;
    }

    if( aNetName.IsEmpty() )
    {
        result["error"] = "Net name is empty";
        return result;
    }

    wxLogTrace( traceAiToolCall, wxT( "PCB_SIGNAL_PATH_TRACER::TraceFromNet net='%s' maxHops=%d" ),
                aNetName, aMaxHops );

    NETINFO_ITEM* startNet = m_board->FindNet( aNetName );

    if( !startNet )
    {
        result["error"] = std::string( "Net not found: " ) + aNetName.ToStdString();
        return result;
    }

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = m_board->GetConnectivity();

    if( !connectivity )
    {
        result["error"] = "Board connectivity data not available";
        return result;
    }

    struct BFS_ENTRY
    {
        int      netCode;
        wxString netName;
        int      hop;
    };

    std::deque<BFS_ENTRY>  frontier;
    std::set<int>          enqueuedNetCodes;  // nets already pushed onto frontier (cycle guard)
    std::vector<nlohmann::json> pathsJson;

    struct COMPONENT_ENTRY
    {
        nlohmann::json json;
        int            firstHop;
    };

    std::map<wxString, COMPONENT_ENTRY> componentMap;

    frontier.push_back( { startNet->GetNetCode(), aNetName, 0 } );
    enqueuedNetCodes.insert( startNet->GetNetCode() );

    auto toMm = []( int aIU ) -> double
    {
        return aIU / 1000000.0;
    };

    while( !frontier.empty() )
    {
        BFS_ENTRY current = frontier.front();
        frontier.pop_front();

        if( current.hop > aMaxHops )
            continue;

        const std::vector<BOARD_CONNECTED_ITEM*> netItems =
                connectivity->GetNetItems( current.netCode, { PCB_PAD_T } );

        for( BOARD_CONNECTED_ITEM* item : netItems )
        {
            if( item->Type() != PCB_PAD_T )
                continue;

            PAD* pad = static_cast<PAD*>( item );
            FOOTPRINT* fp = pad->GetParentFootprint();

            if( !fp )
                continue;

            wxString ref = fp->GetReference();
            wxString value = fp->GetValue();

            // Record component (first-seen hop wins)
            if( componentMap.find( ref ) == componentMap.end() )
            {
                nlohmann::json comp;
                comp["reference"] = ref.ToStdString();
                comp["value"] = value.ToStdString();
                comp["x_mm"] = toMm( fp->GetPosition().x );
                comp["y_mm"] = toMm( fp->GetPosition().y );
                comp["pads"] = nlohmann::json::array();
                comp["hop"] = current.hop;
                comp["is_source"] = ( current.hop == 0 );

                componentMap[ref] = { comp, current.hop };
            }

            // Add pad info (deduplicated)
            nlohmann::json padInfo;
            padInfo["number"] = pad->GetNumber().ToStdString();
            padInfo["net"] = current.netName.ToStdString();

            bool padAlreadyListed = false;

            for( const auto& existing : componentMap[ref].json["pads"] )
            {
                if( existing["number"] == padInfo["number"]
                    && existing["net"] == padInfo["net"] )
                {
                    padAlreadyListed = true;
                    break;
                }
            }

            if( !padAlreadyListed )
                componentMap[ref].json["pads"].push_back( padInfo );

            // Hop through footprint: find other pads on different nets
            for( PAD* otherPad : fp->Pads() )
            {
                if( otherPad == pad )
                    continue;

                int otherNetCode = otherPad->GetNetCode();

                if( otherNetCode <= 0 || otherNetCode == current.netCode )
                    continue;

                wxString otherNetName = otherPad->GetNetname();

                // Always emit a path entry (even if net was previously visited)
                nlohmann::json pathJson;
                pathJson["start_net"] = current.netName.ToStdString();
                pathJson["end_net"] = otherNetName.ToStdString();

                nlohmann::json nodesJson = nlohmann::json::array();

                nlohmann::json entryNode;
                entryNode["net"] = current.netName.ToStdString();
                entryNode["pad"] = pad->GetNumber().ToStdString();
                entryNode["component"] = ref.ToStdString();
                entryNode["component_value"] = value.ToStdString();
                entryNode["x_mm"] = toMm( pad->GetPosition().x );
                entryNode["y_mm"] = toMm( pad->GetPosition().y );
                nodesJson.push_back( entryNode );

                nlohmann::json exitNode;
                exitNode["net"] = otherNetName.ToStdString();
                exitNode["pad"] = otherPad->GetNumber().ToStdString();
                exitNode["component"] = ref.ToStdString();
                exitNode["component_value"] = value.ToStdString();
                exitNode["x_mm"] = toMm( otherPad->GetPosition().x );
                exitNode["y_mm"] = toMm( otherPad->GetPosition().y );
                nodesJson.push_back( exitNode );

                pathJson["nodes"] = nodesJson;
                pathJson["hop"] = current.hop;
                pathsJson.push_back( pathJson );

                // Only enqueue for BFS if not already queued (prevents cycles)
                if( !enqueuedNetCodes.count( otherNetCode )
                    && !isPowerNet( otherNetName )
                    && current.hop + 1 <= aMaxHops )
                {
                    frontier.push_back( { otherNetCode, otherNetName, current.hop + 1 } );
                    enqueuedNetCodes.insert( otherNetCode );
                }
            }
        }
    }

    nlohmann::json componentsJson = nlohmann::json::array();

    for( const auto& [ref, entry] : componentMap )
        componentsJson.push_back( entry.json );

    nlohmann::json visitedNetsJson = nlohmann::json::array();

    for( int netCode : enqueuedNetCodes )
    {
        NETINFO_ITEM* net = m_board->FindNet( netCode );

        if( net )
            visitedNetsJson.push_back( net->GetNetname().ToStdString() );
    }

    result["success"] = true;
    result["start_net"] = aNetName.ToStdString();
    result["max_hops"] = aMaxHops;
    result["paths"] = pathsJson;
    result["connected_components"] = componentsJson;
    result["nets_visited"] = visitedNetsJson;

    wxLogTrace( traceAiToolCall,
                wxT( "PCB_SIGNAL_PATH_TRACER::TraceFromNet completed: %zu paths, %zu components" ),
                pathsJson.size(), componentsJson.size() );

    return result;
}
