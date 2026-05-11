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

#ifndef PCB_SIGNAL_PATH_TRACER_H
#define PCB_SIGNAL_PATH_TRACER_H

#include <json_common.h>
#include <wx/string.h>
#include <set>
#include <vector>

class BOARD;
class PAD;
class FOOTPRINT;

/**
 * Traces signal paths on the PCB by following pad-to-pad connectivity through
 * footprints. Uses BFS: starting from a net, it finds all pads on that net,
 * crosses through each pad's parent footprint to the footprint's other pads
 * (on different nets), and continues.
 */
class PCB_SIGNAL_PATH_TRACER
{
public:
    explicit PCB_SIGNAL_PATH_TRACER( BOARD* aBoard );

    /**
     * Trace all signal paths reachable from a given net.
     * @param aNetName Starting net name.
     * @param aMaxHops Maximum number of component hops (default 3).
     * @return JSON object with paths, connected components, and metadata.
     */
    nlohmann::json TraceFromNet( const wxString& aNetName, int aMaxHops = 3 );

    /**
     * Resolve a component reference + optional pad number to a net, then trace.
     * @param aComponent Component reference designator (e.g. "U7").
     * @param aPad Pad number (e.g. "4"). If empty, uses first signal-type pad.
     * @param aMaxHops Maximum number of component hops (default 3).
     * @return JSON object with paths, connected components, and metadata.
     */
    nlohmann::json TraceFromComponent( const wxString& aComponent, const wxString& aPad,
                                       int aMaxHops = 3 );

private:
    bool isPowerNet( const wxString& aNetName ) const;

    BOARD*             m_board;
    std::set<wxString> m_fallbackPowerNets;
};

#endif // PCB_SIGNAL_PATH_TRACER_H
