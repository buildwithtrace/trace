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

#ifndef SCH_SIGNAL_PATH_TRACER_H
#define SCH_SIGNAL_PATH_TRACER_H

#include <json_common.h>
#include <wx/string.h>
#include <sch_sheet_path.h>
#include <set>
#include <vector>

class SCHEMATIC;
class SCH_PIN;
class SCH_SYMBOL;
class CONNECTION_GRAPH;

/**
 * Traces signal paths through the schematic by following electrical connectivity
 * across component pins. Uses BFS over the CONNECTION_GRAPH to hop through symbols:
 * starting from a net, it finds all pins on that net, crosses through each pin's
 * parent symbol to the symbol's other pins, and continues onto their nets.
 */
class SCH_SIGNAL_PATH_TRACER
{
public:
    explicit SCH_SIGNAL_PATH_TRACER( SCHEMATIC* aSchematic );

    /**
     * Trace all signal paths reachable from a given net.
     * @param aNetName Starting net name.
     * @param aMaxHops Maximum number of component hops (default 3).
     * @return JSON object with paths, connected components, and metadata.
     */
    nlohmann::json TraceFromNet( const wxString& aNetName, int aMaxHops = 3 );

    /**
     * Resolve a component reference + pin name to a net name, then trace from that net.
     * @param aComponent Component reference designator (e.g. "U7").
     * @param aPinName Pin name or number (e.g. "FB" or "4").
     * @param aMaxHops Maximum number of component hops (default 3).
     * @return JSON object with paths, connected components, and metadata.
     */
    nlohmann::json TraceFromComponent( const wxString& aComponent, const wxString& aPinName,
                                       int aMaxHops = 3 );

private:
    bool isPowerNet( const wxString& aNetName ) const;
    wxString resolveNetFromComponent( const wxString& aComponent, const wxString& aPinName,
                                      nlohmann::json& aResult );

    SCHEMATIC*        m_schematic;
    CONNECTION_GRAPH* m_connGraph;
};

#endif // SCH_SIGNAL_PATH_TRACER_H
