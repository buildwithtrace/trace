#!/usr/bin/env python3
#
# This program source code file is part of Trace, an AI-native PCB design application.
#
# Copyright (C) 2025-2026 Trace Developers Team
# Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
S-Expression to TRACE-JSON Converter

Converts KiCad .kicad_sch S-expression files to trace_sch-compatible JSON format.
Uses a fast custom recursive parser to parse S-expressions, then transforms
the data to match the trace_sch format structure.
"""

import json
import math
import re
import sys
import os
from typing import List, Dict, Any, Optional, Tuple, Union, Set

# Handle both module import and direct script execution
try:
    from .wire_helpers import find_wire_islands, wire_touches_pin
except (ImportError, ValueError):
    # Fallback for direct script execution - add current directory to path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    from wire_helpers import find_wire_islands, wire_touches_pin

# Import common S-expression parsing utilities
try:
    from ..common.sexp_parser import parse_sexp
    from ..common.sexp_helpers import (
        find_element, find_elements, get_atom_value,
        extract_coord, extract_rotation
    )
except (ImportError, ValueError):
    # Fallback for direct script execution
    import sys
    import os
    common_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common')
    if common_dir not in sys.path:
        sys.path.insert(0, common_dir)
    from sexp_parser import parse_sexp
    from sexp_helpers import find_element, find_elements, get_atom_value, extract_coord, extract_rotation


def extract_wire_points(pts: List) -> List[Tuple[float, float]]:
    """Extract points from (pts (xy x y) (xy x y) ...) format."""
    points = []
    if not isinstance(pts, list) or len(pts) < 2:
        return points
    
    # pts is like: ['pts', ['xy', x1, y1], ['xy', x2, y2], ...]
    for item in pts[1:]:
        if isinstance(item, list) and len(item) >= 3 and item[0] == 'xy':
            try:
                x = float(item[1])
                y = float(item[2])
                points.append((x, y))
            except (ValueError, TypeError, IndexError):
                continue
    
    return points


def extract_properties(symbol: List) -> Dict[str, str]:
    """Extract properties from symbol element."""
    props = {}
    property_elements = find_elements(symbol, 'property')
    
    for prop in property_elements:
        if len(prop) >= 3:
            prop_name = get_atom_value(prop, 1, '')
            prop_value = get_atom_value(prop, 2, '')
            if prop_name:
                props[prop_name] = prop_value
    
    return props


def is_power_symbol_for_net(lib_id: str) -> bool:
    """
    Check if symbol is a power symbol that should define a net name.
    
    Power symbols (from the "power" library) define net names based on their Value property.
    PWR_FLAG is excluded because it's only for ERC purposes and doesn't define a net.
    
    Args:
        lib_id: Library symbol ID in format "LibraryName:SymbolName"
    
    Returns:
        True if the symbol is a power symbol that defines a net (not PWR_FLAG)
    """
    if not lib_id:
        return False
    # Power symbols are in the "power" library
    if ':' in lib_id:
        library_name = lib_id.split(':', 1)[0].lower()
        symbol_name = lib_id.split(':', 1)[1].upper()
        return 'power' in library_name and 'PWR_FLAG' not in symbol_name
    return False


def extract_pins(symbol: List) -> List[Dict[str, Any]]:
    """Extract pin information from symbol element."""
    pins = []
    pin_elements = find_elements(symbol, 'pin')
    
    for pin in pin_elements:
        pin_info = {}
        # Pin number is usually the first atom after 'pin'
        pin_number = get_atom_value(pin, 1, None)
        if pin_number:
            pin_info['number'] = pin_number
        
        # Pin UUID
        uuid_elem = find_element(pin, 'uuid')
        if uuid_elem:
            pin_info['uuid'] = get_atom_value(uuid_elem, 1, None)
        
        # Pin position (at) - may not be present in instance, only in library
        at_elem = find_element(pin, 'at')
        if at_elem:
            pin_info['at'] = extract_coord(at_elem)
            pin_info['rot'] = extract_rotation(at_elem)
        
        pins.append(pin_info)
    
    return pins


def extract_library_symbols(sexp_data: List) -> Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]]:
    """
    Extract library symbol definitions and build pin offset map.
    
    Returns a dictionary mapping:
    lib_id -> (unit, body_style) -> pin_number -> (x_offset, y_offset, pin_rotation)
    """
    lib_symbols_map = {}
    
    if not isinstance(sexp_data, list) or len(sexp_data) == 0:
        return lib_symbols_map
    
    # Find kicad_sch root
    if sexp_data[0] != 'kicad_sch':
        return lib_symbols_map
    
    # Find lib_symbols section
    lib_symbols_elem = find_element(sexp_data, 'lib_symbols')
    if not lib_symbols_elem:
        return lib_symbols_map
    
    # Iterate through library symbols
    for item in lib_symbols_elem[1:]:
        if not isinstance(item, list) or len(item) == 0 or item[0] != 'symbol':
            continue
        
        # Get library symbol ID (e.g., "Amplifier_Operational:TL072")
        lib_id = get_atom_value(item, 1, None)
        if not lib_id:
            continue
        
        # Initialize entry for this lib_id
        if lib_id not in lib_symbols_map:
            lib_symbols_map[lib_id] = {}
        
        # Find nested symbol definitions (units/alternates)
        # These are like (symbol "TL072_1_1" ...) where format is "{name}_{unit}_{body_style}"
        nested_symbols = find_elements(item, 'symbol')
        
        for nested_symbol in nested_symbols:
            # Get nested symbol name (e.g., "TL072_1_1")
            nested_name = get_atom_value(nested_symbol, 1, None)
            if not nested_name:
                continue
            
            # Parse unit and body_style from name (format: "{name}_{unit}_{body_style}")
            # Try to extract unit and body_style
            parts = nested_name.split('_')
            unit = 1
            body_style = 1
            
            # Try to parse unit and body_style from the name
            # Format is typically "{base}_{unit}_{body_style}" or just "{base}_{unit}"
            if len(parts) >= 3:
                try:
                    unit = int(parts[-2])
                    body_style = int(parts[-1])
                except (ValueError, IndexError):
                    # Fallback: try last two parts
                    try:
                        unit = int(parts[-1])
                        body_style = 1
                    except (ValueError, IndexError):
                        pass
            elif len(parts) >= 2:
                try:
                    unit = int(parts[-1])
                    body_style = 1
                except (ValueError, IndexError):
                    pass
            
            # Extract pins from this nested symbol
            pin_elements = find_elements(nested_symbol, 'pin')
            pin_map = {}
            
            for pin in pin_elements:
                # Get pin number
                pin_number_elem = find_element(pin, 'number')
                if not pin_number_elem:
                    continue
                
                pin_number = get_atom_value(pin_number_elem, 1, None)
                if not pin_number:
                    continue
                
                # Get pin position (at x y rot)
                at_elem = find_element(pin, 'at')
                if not at_elem:
                    continue
                
                pin_coord = extract_coord(at_elem)
                pin_rot = extract_rotation(at_elem)
                
                if pin_coord:
                    pin_map[str(pin_number)] = (pin_coord[0], pin_coord[1], pin_rot if pin_rot is not None else 0)
            
            # Store pin map for this unit/body_style combination
            if pin_map:
                lib_symbols_map[lib_id][(unit, body_style)] = pin_map
    
    return lib_symbols_map


def _get_pins_with_inheritance(lib_symbol_pins: Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]], 
                                unit: int, body_style: int) -> Dict[str, Tuple[float, float, int]]:
    """
    Get pins with KiCad inheritance fallback.
    
    KiCad uses an inheritance model for symbol definitions:
    - SymbolName_0_0: Global elements (inherited by all units and styles)
    - SymbolName_0_style: Shared by all units, specific to style
    - SymbolName_unit_0: Specific to unit, shared by all styles
    - SymbolName_unit_style: Specific to both unit and style
    
    Pins are merged from the inheritance chain, with more specific definitions
    overriding less specific ones.
    
    Args:
        lib_symbol_pins: Dict mapping (unit, body_style) -> pin_number -> (x, y, rot)
        unit: Target unit number (must be >= 1)
        body_style: Target body style (must be >= 1)
    
    Returns:
        Merged pin dictionary with inheritance applied
    """
    # Merge pins from inheritance chain (later entries override earlier)
    # Order: global -> unit-specific shared style -> shared unit specific style -> exact match
    merged_pins = {}
    for search_unit, search_style in [(0, 0), (unit, 0), (0, body_style), (unit, body_style)]:
        pins = lib_symbol_pins.get((search_unit, search_style), {})
        merged_pins.update(pins)
    return merged_pins


def transform_pin_coordinate(pin_offset: Tuple[float, float, int], symbol_pos: Tuple[float, float], 
                             symbol_rot: Optional[int], mirror: Optional[str] = None) -> Tuple[float, float]:
    """
    Transform pin offset by symbol position, mirror, and rotation.
    
    Args:
        pin_offset: (x_offset, y_offset, pin_rotation) from library symbol
        symbol_pos: (x, y) position of symbol instance
        symbol_rot: Rotation angle in degrees (0, 90, 180, 270)
        mirror: Optional mirror axis - "x" or "y"
    
    Returns:
        Transformed (x, y) coordinate
    """
    x_off, y_off, pin_rot = pin_offset
    x_pos, y_pos = symbol_pos
    
    # Normalize pin rotation to 0-360 range
    pin_rot_norm = pin_rot % 360
    if pin_rot_norm < 0:
        pin_rot_norm += 360
    
    # Apply pin rotation transformation to get the actual connection point
    y_off = -y_off
    
    # Apply mirror transformation before rotation
    if mirror == 'x':
        y_off = -y_off
    elif mirror == 'y':
        x_off = -x_off
    
    # Normalize symbol rotation to 0-360 range
    rot = symbol_rot if symbol_rot is not None else 0
    rot = rot % 360
    if rot < 0:
        rot += 360
    
    # Rotate pin offset around origin by symbol rotation
    # KiCad uses screen coordinates (Y increases downward)
    if rot == 0:
        x_rot, y_rot = x_off, y_off
    elif rot == 90:
        # 90° CCW in screen coords: (x, y) → (y, -x)
        x_rot, y_rot = y_off, -x_off
    elif rot == 180:
        # 180° CCW in screen coords: (x, y) → (-x, -y)
        x_rot, y_rot = -x_off, -y_off
    elif rot == 270:
        # 270° CCW in screen coords: (x, y) → (-y, x)
        x_rot, y_rot = -y_off, x_off
    else:
        # For non-cardinal angles, use rotation matrix for screen coordinates
        rad = math.radians(rot)
        cos_r = math.cos(rad)
        sin_r = math.sin(rad)
        x_rot = x_off * cos_r + y_off * sin_r
        y_rot = -x_off * sin_r + y_off * cos_r
    
    # Translate by symbol position
    return (x_rot + x_pos, y_rot + y_pos)


def find_wires_near_point(point: Tuple[float, float], wires: List[List], 
                          threshold: float = 2.54) -> List[Tuple[float, float]]:
    """Find wire endpoints near a given point."""
    nearby_points = []
    for wire in wires:
        pts_elem = find_element(wire, 'pts')
        if pts_elem:
            points = extract_wire_points(pts_elem)
            for p in points:
                dx = abs(p[0] - point[0])
                dy = abs(p[1] - point[1])
                if dx < threshold and dy < threshold:
                    nearby_points.append(p)
    return nearby_points


# =============================================================================
# Connectivity Analysis
# =============================================================================

def convert_wires_to_dict_format(wires: List[List]) -> List[Dict[str, Any]]:
    """Convert S-expression wire format to dict format with 'points' key."""
    wire_dicts = []
    for wire in wires:
        pts_elem = find_element(wire, 'pts')
        if pts_elem:
            points = extract_wire_points(pts_elem)
            if len(points) >= 2:
                # Convert tuples to lists for JSON compatibility
                wire_dict = {
                    'points': [list(p) for p in points]
                }
                wire_dicts.append(wire_dict)
    return wire_dicts


def convert_junctions_to_dict_format(junctions: List[List]) -> List[Dict[str, Any]]:
    """Convert S-expression junction format to dict format with 'at' key."""
    junction_dicts = []
    for junction in junctions:
        at_elem = find_element(junction, 'at')
        if at_elem:
            junc_pos = extract_coord(at_elem)
            if junc_pos:
                junction_dict = {
                    'at': list(junc_pos)  # Convert tuple to list
                }
                junction_dicts.append(junction_dict)
    return junction_dicts


def assign_nets_to_wire_islands(wire_islands: List[List[Dict[str, Any]]],
                                 symbols: List[List], wires: List[List],
                                 labels: List[List], glabels: List[List],
                                 lib_symbols_map: Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]],
                                 junctions: List[List]) -> Dict[Tuple[float, float], str]:
    """
    Assign net names to wire islands based on labels, power symbols, and pin connections.
    
    Priority order for net assignment:
    1. Labels/global labels on wires or pins
    2. Power symbol Value property (e.g., GND, VCC, +5V) - excludes PWR_FLAG
    3. Auto-generated net name (NET1, NET2, etc.) if connected to any pin
    4. No net assignment if island has no pins connected
    
    Args:
        wire_islands: List of wire islands (groups of connected wires)
        symbols: List of symbol S-expressions
        wires: List of wire S-expressions
        labels: List of label S-expressions
        glabels: List of global label S-expressions
        lib_symbols_map: Map of library symbols for pin position lookup
        junctions: List of junction S-expressions
    
    Returns:
        Dictionary mapping wire points to net names
    """
    point_to_net = {}
    tolerance = 0.1
    pin_tolerance = 0.01  # Tighter tolerance for pin-wire connections
    
    # Convert junctions for wire_touches_pin
    junction_dicts = convert_junctions_to_dict_format(junctions)
    
    # Build a map of island index to net name
    island_to_net = {}
    
    # Track which islands have pins connected (for auto-net generation)
    island_has_pins = {}
    
    # Counter for auto-generated net names
    auto_net_counter = 1
    
    # For each island, find pins that touch wires in it
    for island_idx, island in enumerate(wire_islands):
        island_net = None
        island_has_pins[island_idx] = False
        power_symbol_value = None  # Track power symbol value for this island
        
        # First pass: Check all symbols for labels and power symbols
        for symbol in symbols:
            # Get symbol position and rotation
            at_elem = find_element(symbol, 'at')
            symbol_pos = extract_coord(at_elem) if at_elem else None
            symbol_rot = extract_rotation(at_elem) if at_elem else None
            
            if not symbol_pos:
                continue
            
            # Get lib_id
            lib_id_elem = find_element(symbol, 'lib_id')
            lib_id = get_atom_value(lib_id_elem, 1, None) if lib_id_elem else None
            if not lib_id:
                lib_name_elem = find_element(symbol, 'lib_name')
                lib_id = get_atom_value(lib_name_elem, 1, None) if lib_name_elem else None
            
            if not lib_id or lib_id not in lib_symbols_map:
                continue
            
            # Get unit and body_style
            unit_elem = find_element(symbol, 'unit')
            unit = int(get_atom_value(unit_elem, 1, 1)) if unit_elem else 1
            body_style_elem = find_element(symbol, 'body_style')
            body_style = int(get_atom_value(body_style_elem, 1, 1)) if body_style_elem else 1
            
            # Look up library symbol definition with inheritance
            lib_pins = _get_pins_with_inheritance(lib_symbols_map[lib_id], unit, body_style)
            
            if not lib_pins:
                continue
            
            # Check if this is a power symbol (not PWR_FLAG)
            is_power = is_power_symbol_for_net(lib_id)
            
            # Get mirror
            mirror_elem = find_element(symbol, 'mirror')
            symbol_mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
            
            # Get symbol properties for power symbol value
            symbol_value = None
            if is_power:
                properties = extract_properties(symbol)
                symbol_value = properties.get('Value', None)
            
            # Check each pin
            for pin_num_str, pin_offset in lib_pins.items():
                pin_x_off, pin_y_off, pin_rot = pin_offset
                pin_coord = transform_pin_coordinate((pin_x_off, pin_y_off, pin_rot), symbol_pos, symbol_rot, symbol_mirror)
                pin_pos = tuple(pin_coord)
                
                # Check if this pin touches any wire in the island
                pin_touches_island = False
                for wire in island:
                    if wire_touches_pin(wire, pin_pos, junction_dicts, tolerance=pin_tolerance):
                        pin_touches_island = True
                        break
                
                if not pin_touches_island:
                    continue
                
                # This pin touches the island - mark that island has pins
                island_has_pins[island_idx] = True
                
                # If this is a power symbol, record its value
                if is_power and symbol_value and not power_symbol_value:
                    power_symbol_value = symbol_value
                
                # Check if this pin has a label (highest priority)
                pin_label = None
                
                # Check regular labels
                for label in labels:
                    at_elem = find_element(label, 'at')
                    if at_elem:
                        label_pos = extract_coord(at_elem)
                        if label_pos:
                            dx = abs(label_pos[0] - pin_coord[0])
                            dy = abs(label_pos[1] - pin_coord[1])
                            if dx < tolerance and dy < tolerance:
                                label_text = get_atom_value(label, 1, None)
                                if label_text:
                                    pin_label = label_text
                                    break
                
                # Check global labels if no regular label found
                if not pin_label:
                    for glabel in glabels:
                        at_elem = find_element(glabel, 'at')
                        if at_elem:
                            glabel_pos = extract_coord(at_elem)
                            if glabel_pos:
                                dx = abs(glabel_pos[0] - pin_coord[0])
                                dy = abs(glabel_pos[1] - pin_coord[1])
                                if dx < tolerance and dy < tolerance:
                                    glabel_text = get_atom_value(glabel, 1, None)
                                    if glabel_text:
                                        pin_label = glabel_text
                                        break
                
                # If pin has a label, use it as the net for this island (highest priority)
                if pin_label:
                    island_net = pin_label
                    break  # Found a pin with label, use it
            
            # If we found a label, stop checking other symbols
            if island_net:
                break
        
        # Priority 2: If no label found but power symbol touches island, use power symbol value
        if not island_net and power_symbol_value:
            island_net = power_symbol_value
        
        # Priority 3: If island has pins but no label/power symbol, generate auto-net name
        if not island_net and island_has_pins[island_idx]:
            island_net = f"NET{auto_net_counter}"
            auto_net_counter += 1
        
        # Store net for this island
        island_to_net[island_idx] = island_net
        
        # Extract all points from wires in this island and map them to the net
        for wire in island:
            wire_points = wire.get('points', [])
            for point in wire_points:
                point_tuple = tuple(point) if isinstance(point, list) else point
                if island_net:
                    point_to_net[point_tuple] = island_net
                # If island_net is None, don't assign anything (leave it unassigned)
    
    # Also assign net names from labels/glabels that are directly on wires (not just pins)
    for label in labels:
        at_elem = find_element(label, 'at')
        if at_elem:
            label_pos = extract_coord(at_elem)
            label_text = get_atom_value(label, 1, None)
            if label_pos and label_text:
                # Find which island this label belongs to by checking if it's close to any wire point
                for island_idx, island in enumerate(wire_islands):
                    for wire in island:
                        wire_points = wire.get('points', [])
                        for point in wire_points:
                            point_tuple = tuple(point) if isinstance(point, list) else point
                            dx = abs(point_tuple[0] - label_pos[0])
                            dy = abs(point_tuple[1] - label_pos[1])
                            if dx < tolerance and dy < tolerance:
                                # Assign this net name to all points in the island
                                island_net = label_text
                                island_to_net[island_idx] = island_net
                                for w in island:
                                    for p in w.get('points', []):
                                        pt = tuple(p) if isinstance(p, list) else p
                                        point_to_net[pt] = island_net
                                break
    
    # Assign names from global labels
    for glabel in glabels:
        at_elem = find_element(glabel, 'at')
        if at_elem:
            glabel_pos = extract_coord(at_elem)
            glabel_text = get_atom_value(glabel, 1, None)
            if glabel_pos and glabel_text:
                for island_idx, island in enumerate(wire_islands):
                    for wire in island:
                        wire_points = wire.get('points', [])
                        for point in wire_points:
                            point_tuple = tuple(point) if isinstance(point, list) else point
                            dx = abs(point_tuple[0] - glabel_pos[0])
                            dy = abs(point_tuple[1] - glabel_pos[1])
                            if dx < tolerance and dy < tolerance:
                                island_net = glabel_text
                                island_to_net[island_idx] = island_net
                                for w in island:
                                    for p in w.get('points', []):
                                        pt = tuple(p) if isinstance(p, list) else p
                                        point_to_net[pt] = island_net
                                break
    
    return point_to_net


def find_labels_at_pin_positions(symbols: List[List], labels: List[List],
                                  lib_symbols_map: Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]]) -> Set[Tuple[float, float]]:
    """
    Find labels that are at pin positions.
    
    Returns a set of label positions (tuples) that are on component pins.
    """
    pin_label_positions = set()
    tolerance = 0.1
    
    for symbol in symbols:
        lib_id_elem = find_element(symbol, 'lib_id')
        lib_id = get_atom_value(lib_id_elem, 1, None) if lib_id_elem else None
        if not lib_id:
            lib_name_elem = find_element(symbol, 'lib_name')
            lib_id = get_atom_value(lib_name_elem, 1, None) if lib_name_elem else None
        
        if not lib_id:
            continue
        
        # Get symbol position and rotation
        at_elem = find_element(symbol, 'at')
        symbol_pos = extract_coord(at_elem) if at_elem else None
        symbol_rot = extract_rotation(at_elem) if at_elem else None
        
        if not symbol_pos:
            continue
        
        # Get unit and body_style
        unit_elem = find_element(symbol, 'unit')
        unit = int(get_atom_value(unit_elem, 1, 1)) if unit_elem else 1
        
        body_style_elem = find_element(symbol, 'body_style')
        body_style = int(get_atom_value(body_style_elem, 1, 1)) if body_style_elem else 1
        
        # Look up library symbol definition
        if lib_id not in lib_symbols_map:
            continue
        
        # Use inheritance for pin lookup
        lib_pins = _get_pins_with_inheritance(lib_symbols_map[lib_id], unit, body_style)
        
        if not lib_pins:
            continue
        
        # Get mirror
        mirror_elem = find_element(symbol, 'mirror')
        symbol_mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
        
        # Extract pins from symbol
        pins = extract_pins(symbol)
        
        # Calculate pin positions and check if labels are at those positions
        for pin in pins:
            pin_num = pin.get('number')
            if pin_num is None:
                continue
            
            pin_num_str = str(pin_num)
            
            # Get pin offset from library definition
            if pin_num_str not in lib_pins:
                continue
            
            pin_offset = lib_pins[pin_num_str]
            pin_x_off, pin_y_off, pin_rot = pin_offset
            
            # Transform pin offset by symbol position and rotation
            pin_coord = transform_pin_coordinate((pin_x_off, pin_y_off, pin_rot), symbol_pos, symbol_rot, symbol_mirror)
            
            # Check if any label is at this pin position
            for label in labels:
                at_elem = find_element(label, 'at')
                if at_elem:
                    label_pos = extract_coord(at_elem)
                    if label_pos:
                        dx = abs(label_pos[0] - pin_coord[0])
                        dy = abs(label_pos[1] - pin_coord[1])
                        if dx < tolerance and dy < tolerance:
                            # This label is at a pin position
                            pin_label_positions.add(label_pos)
    
    return pin_label_positions


def find_labels_at_sheet_pin_positions(sheets: List[List], labels: List[List]) -> Tuple[Set[Tuple[float, float]], Dict[Tuple[str, str], str]]:
    """
    Find labels that are at sheet pin positions.
    
    Returns:
        Tuple of:
        - Set of label positions (tuples) that are on sheet pins
        - Dict mapping (sheet_uuid, pin_name) -> label_text for net name resolution
    """
    sheet_pin_label_positions: Set[Tuple[float, float]] = set()
    sheet_pin_net_map: Dict[Tuple[str, str], str] = {}
    tolerance = 0.1
    
    for sheet in sheets:
        if not isinstance(sheet, list) or len(sheet) == 0 or sheet[0] != 'sheet':
            continue
        
        uuid_elem = find_element(sheet, 'uuid')
        sheet_uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
        if not sheet_uuid:
            continue
        
        pin_elements = find_elements(sheet, 'pin')
        for pin in pin_elements:
            if len(pin) < 3:
                continue
            pin_name = get_atom_value(pin, 1, None)
            if not pin_name:
                continue
            
            pin_at_elem = find_element(pin, 'at')
            if not pin_at_elem:
                continue
            pin_coord = extract_coord(pin_at_elem)
            if not pin_coord:
                continue
            
            for label in labels:
                label_at_elem = find_element(label, 'at')
                if label_at_elem:
                    label_pos = extract_coord(label_at_elem)
                    if label_pos:
                        dx = abs(label_pos[0] - pin_coord[0])
                        dy = abs(label_pos[1] - pin_coord[1])
                        if dx < tolerance and dy < tolerance:
                            label_text = get_atom_value(label, 1, None)
                            if label_text:
                                sheet_pin_label_positions.add(label_pos)
                                sheet_pin_net_map[(sheet_uuid, pin_name)] = label_text
    
    return sheet_pin_label_positions, sheet_pin_net_map


def find_noconnects_at_pin_positions(symbols: List[List], noconnects: List[List],
                                      lib_symbols_map: Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]]) -> Set[Tuple[float, float]]:
    """
    Find noconnects that are at pin positions.
    
    Returns a set of noconnect positions (tuples) that are on component pins.
    """
    pin_noconnect_positions = set()
    tolerance = 0.1
    
    for symbol in symbols:
        # Power symbols are now treated as normal components
        lib_id_elem = find_element(symbol, 'lib_id')
        lib_id = get_atom_value(lib_id_elem, 1, None) if lib_id_elem else None
        if not lib_id:
            lib_name_elem = find_element(symbol, 'lib_name')
            lib_id = get_atom_value(lib_name_elem, 1, None) if lib_name_elem else None
        
        if not lib_id:
            continue
        
        # Get symbol position and rotation
        at_elem = find_element(symbol, 'at')
        symbol_pos = extract_coord(at_elem) if at_elem else None
        symbol_rot = extract_rotation(at_elem) if at_elem else None
        
        if not symbol_pos:
            continue
        
        # Get unit and body_style
        unit_elem = find_element(symbol, 'unit')
        unit = int(get_atom_value(unit_elem, 1, 1)) if unit_elem else 1
        
        body_style_elem = find_element(symbol, 'body_style')
        body_style = int(get_atom_value(body_style_elem, 1, 1)) if body_style_elem else 1
        
        # Look up library symbol definition
        if lib_id not in lib_symbols_map:
            continue
        
        # Use inheritance for pin lookup
        lib_pins = _get_pins_with_inheritance(lib_symbols_map[lib_id], unit, body_style)
        
        if not lib_pins:
            continue
        
        # Get mirror
        mirror_elem = find_element(symbol, 'mirror')
        symbol_mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
        
        # Extract pins from symbol
        pins = extract_pins(symbol)
        
        # Calculate pin positions and check if noconnects are at those positions
        for pin in pins:
            pin_num = pin.get('number')
            if pin_num is None:
                continue
            
            pin_num_str = str(pin_num)
            
            # Get pin offset from library definition
            if pin_num_str not in lib_pins:
                continue
            
            pin_offset = lib_pins[pin_num_str]
            pin_x_off, pin_y_off, pin_rot = pin_offset
            
            # Transform pin offset by symbol position and rotation
            pin_coord = transform_pin_coordinate((pin_x_off, pin_y_off, pin_rot), symbol_pos, symbol_rot, symbol_mirror)
            
            # Check if any noconnect is at this pin position
            for noconnect in noconnects:
                at_elem = find_element(noconnect, 'at')
                if at_elem:
                    nc_pos = extract_coord(at_elem)
                    if nc_pos:
                        dx = abs(nc_pos[0] - pin_coord[0])
                        dy = abs(nc_pos[1] - pin_coord[1])
                        if dx < tolerance and dy < tolerance:
                            # This noconnect is at a pin position
                            pin_noconnect_positions.add(nc_pos)
    
    return pin_noconnect_positions


def map_pins_to_nets(symbol: List, wires: List[List], junctions: List[List],
                     labels: List[List], glabels: List[List],
                     point_to_net: Dict[Tuple[float, float], str],
                     lib_symbols_map: Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]],
                     noconnects: Optional[List[Tuple[float, float]]] = None) -> Dict[Union[str, int], str]:
    """
    Map pin numbers to net names using library pin offsets and coordinate transformation.
    
    Logic:
    1. First check: If pin has a label (labels/glabels at pin position), use that label
    2. Second check: If pin is on a wire in an island (use wire_touches_pin), use that island's net from point_to_net
    3. Third check: If pin has a noconnect symbol, use "DNC"
    4. Else: Return "NONE"
    """
    pins = extract_pins(symbol)
    pin_map = {}
    
    # Get symbol position and rotation
    at_elem = find_element(symbol, 'at')
    symbol_pos = extract_coord(at_elem) if at_elem else None
    symbol_rot = extract_rotation(at_elem) if at_elem else None
    
    if not symbol_pos:
        return pin_map
    
    # Get lib_id
    lib_id_elem = find_element(symbol, 'lib_id')
    lib_id = get_atom_value(lib_id_elem, 1, None) if lib_id_elem else None
    if not lib_id:
        lib_name_elem = find_element(symbol, 'lib_name')
        lib_id = get_atom_value(lib_name_elem, 1, None) if lib_name_elem else None
    
    if not lib_id:
        return pin_map
    
    # Get unit and body_style
    unit_elem = find_element(symbol, 'unit')
    unit = int(get_atom_value(unit_elem, 1, 1)) if unit_elem else 1
    
    body_style_elem = find_element(symbol, 'body_style')
    body_style = int(get_atom_value(body_style_elem, 1, 1)) if body_style_elem else 1
    
    # Look up library symbol definition
    if lib_id not in lib_symbols_map:
        return pin_map
    
    # Use inheritance for pin lookup
    lib_pins = _get_pins_with_inheritance(lib_symbols_map[lib_id], unit, body_style)
    
    if not lib_pins:
        return pin_map
    
    # Get mirror
    mirror_elem = find_element(symbol, 'mirror')
    symbol_mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
    
    # Convert wires and junctions to dict format for wire_touches_pin
    wire_dicts = convert_wires_to_dict_format(wires)
    junction_dicts = convert_junctions_to_dict_format(junctions)
    
    # Tolerance for point matching
    tolerance = 0.1
    pin_tolerance = 0.01  # Tighter tolerance for pin-wire connections
    
    # Map each pin to a net
    for pin in pins:
        pin_num = pin.get('number')
        if pin_num is None:
            continue
        
        pin_num_str = str(pin_num)
        
        # Get pin offset from library definition
        if pin_num_str not in lib_pins:
            # Pin not found in library - assign "NONE"
            try:
                pin_num_int = int(pin_num)
                pin_map[pin_num_int] = "NONE"
            except (ValueError, TypeError):
                pin_map[pin_num_str] = "NONE"
            continue
        
        pin_offset = lib_pins[pin_num_str]
        pin_x_off, pin_y_off, pin_rot = pin_offset
        
        # Transform pin offset by symbol position and rotation
        pin_coord = transform_pin_coordinate((pin_x_off, pin_y_off, pin_rot), symbol_pos, symbol_rot, symbol_mirror)
        pin_pos = tuple(pin_coord)
        
        # Look up net for this pin coordinate
        net_name = None
        
        # First check: Does this pin have a label?
        for label in labels:
            at_elem = find_element(label, 'at')
            if at_elem:
                label_pos = extract_coord(at_elem)
                if label_pos:
                    dx = abs(label_pos[0] - pin_coord[0])
                    dy = abs(label_pos[1] - pin_coord[1])
                    if dx < tolerance and dy < tolerance:
                        label_text = get_atom_value(label, 1, None)
                        if label_text:
                            net_name = label_text
                            break
        
        # If no label found, check global labels
        if not net_name:
            for glabel in glabels:
                at_elem = find_element(glabel, 'at')
                if at_elem:
                    glabel_pos = extract_coord(at_elem)
                    if glabel_pos:
                        dx = abs(glabel_pos[0] - pin_coord[0])
                        dy = abs(glabel_pos[1] - pin_coord[1])
                        if dx < tolerance and dy < tolerance:
                            glabel_text = get_atom_value(glabel, 1, None)
                            if glabel_text:
                                net_name = glabel_text
                                break
        
        # Second check: If pin is on a wire, use that wire's net from point_to_net
        if not net_name:
            for wire in wire_dicts:
                if wire_touches_pin(wire, pin_pos, junction_dicts, tolerance=pin_tolerance):
                    # Pin touches this wire - find the net for points on this wire
                    wire_points = wire.get('points', [])
                    for point in wire_points:
                        point_tuple = tuple(point) if isinstance(point, list) else point
                        if point_tuple in point_to_net:
                            net_name = point_to_net[point_tuple]
                            break
                    if net_name:
                        break
        
        # Third check: If pin has a noconnect symbol, use "DNC"
        if not net_name and noconnects:
            for nc_pos in noconnects:
                dx = abs(nc_pos[0] - pin_coord[0])
                dy = abs(nc_pos[1] - pin_coord[1])
                if dx < tolerance and dy < tolerance:
                    net_name = "DNC"
                    break
        
        # If no net found, use "NONE"
        if not net_name:
            net_name = "NONE"
        
        # Store mapping
        try:
            pin_num_int = int(pin_num)
            pin_map[pin_num_int] = net_name
        except (ValueError, TypeError):
            pin_map[pin_num_str] = net_name
    return pin_map


# =============================================================================
# Element Converters
# =============================================================================

def convert_symbol(symbol: List, wires: List[List], junctions: List[List],
                   labels: List[List], glabels: List[List],
                   point_to_net: Dict[Tuple[float, float], str],
                   lib_symbols_map: Dict[str, Dict[Tuple[int, int], Dict[str, Tuple[float, float, int]]]],
                   noconnects: Optional[List[List]] = None) -> Optional[Dict[str, Any]]:
    """Convert KiCad symbol to component statement."""
    if not isinstance(symbol, list) or len(symbol) == 0 or symbol[0] != 'symbol':
        return None
    
    # Extract lib_id
    lib_id_elem = find_element(symbol, 'lib_id')
    lib_id = get_atom_value(lib_id_elem, 1, None) if lib_id_elem else None
    if not lib_id:
        lib_name_elem = find_element(symbol, 'lib_name')
        lib_id = get_atom_value(lib_name_elem, 1, None) if lib_name_elem else None
    
    if not lib_id:
        return None
    
    # Extract properties
    properties = extract_properties(symbol)
    ref = properties.get('Reference', '')
    value = properties.get('Value', '')
    
    # Extract position and rotation
    at_elem = find_element(symbol, 'at')
    coord = None
    rot = None
    if at_elem:
        coord = extract_coord(at_elem)
        rot = extract_rotation(at_elem)
    
    # Extract mirror
    mirror_elem = find_element(symbol, 'mirror')
    mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
    
    # Extract UUID
    uuid_elem = find_element(symbol, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    # Extract unit and body_style (default to 1 if not present)
    unit_elem = find_element(symbol, 'unit')
    unit = int(get_atom_value(unit_elem, 1, 1)) if unit_elem else 1
    
    body_style_elem = find_element(symbol, 'body_style')
    body_style = int(get_atom_value(body_style_elem, 1, 1)) if body_style_elem else 1
    
    # Map pins to nets
    # Extract no-connect coordinates for pin mapping
    noconnect_coords = []
    if noconnects:
        for nc in noconnects:
            at_elem = find_element(nc, 'at')
            nc_coord = extract_coord(at_elem) if at_elem else None
            if nc_coord:
                noconnect_coords.append(nc_coord)
    pins = map_pins_to_nets(symbol, wires, junctions, labels, glabels, point_to_net, lib_symbols_map, noconnect_coords)
    
    # Extract footprint and other properties for props
    props = {}
    if 'Footprint' in properties and properties['Footprint']:
        props['footprint'] = properties['Footprint']
    
    result = {
        'type': 'component',
        'ref': ref,
        'symbol': lib_id,
        'pins': pins,
        'uid': uuid,
        'unit': unit,
        'body_style': body_style
    }
    
    if value:
        result['value'] = value
    if coord:
        result['at'] = list(coord)  # Convert tuple to list for JSON
    if rot is not None:
        result['rot'] = rot
    if mirror:
        result['mirror'] = mirror
    if props:
        result['props'] = props
    
    return result


def convert_wire(wire: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad wire to wire statement."""
    if not isinstance(wire, list) or len(wire) == 0 or wire[0] != 'wire':
        return None
    
    # Extract points
    pts_elem = find_element(wire, 'pts')
    if not pts_elem:
        return None
    
    points = extract_wire_points(pts_elem)
    if len(points) < 2:
        return None
    
    # Extract UUID
    uuid_elem = find_element(wire, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    if not uuid:
        return None
    
    return {
        'type': 'wire',
        'points': [list(p) for p in points],  # Convert tuples to lists for JSON
        'uid': uuid
    }


def convert_label(label: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad label to label statement."""
    if not isinstance(label, list) or len(label) == 0 or label[0] != 'label':
        return None
    
    # Extract label text
    label_text = get_atom_value(label, 1, None)
    if not label_text:
        return None
    
    # Extract position
    at_elem = find_element(label, 'at')
    coord = extract_coord(at_elem) if at_elem else None
    
    if not coord:
        return None
    
    return {
        'type': 'label',
        'name': label_text,
        'at': list(coord)  # Convert tuple to list for JSON
    }


def convert_glabel(glabel: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad global_label to glabel statement."""
    if not isinstance(glabel, list) or len(glabel) == 0 or glabel[0] != 'global_label':
        return None
    
    # Extract label text
    label_text = get_atom_value(glabel, 1, None)
    if not label_text:
        return None
    
    # Extract position and rotation (required)
    at_elem = find_element(glabel, 'at')
    coord = None
    rot = None
    if at_elem:
        coord = extract_coord(at_elem)
        rot = extract_rotation(at_elem)
    
    if not coord:
        return None
    
    # Extract UUID
    uuid_elem = find_element(glabel, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    # Extract shape (optional)
    shape_elem = find_element(glabel, 'shape')
    shape = get_atom_value(shape_elem, 1, None) if shape_elem else None
    
    result = {
        'type': 'glabel',
        'name': label_text,
        'at': list(coord)  # Convert tuple to list for JSON
    }
    
    if uuid_value:
        result['uid'] = uuid_value
    if rot is not None:
        result['rot'] = rot
    if shape:
        result['shape'] = shape
    
    return result


def convert_junction(junction: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad junction to junction statement."""
    if not isinstance(junction, list) or len(junction) == 0 or junction[0] != 'junction':
        return None
    
    # Extract position
    at_elem = find_element(junction, 'at')
    coord = extract_coord(at_elem) if at_elem else None
    if not coord:
        return None
    
    # Extract UUID
    uuid_elem = find_element(junction, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    if not uuid:
        return None
    
    return {
        'type': 'junction',
        'at': list(coord),  # Convert tuple to list for JSON
        'uid': uuid
    }


def convert_noconnect(noconnect: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad no_connect to noconnect statement."""
    if not isinstance(noconnect, list) or len(noconnect) == 0 or noconnect[0] != 'no_connect':
        return None
    
    # Extract position
    at_elem = find_element(noconnect, 'at')
    coord = extract_coord(at_elem) if at_elem else None
    if not coord:
        return None
    
    # Extract UUID
    uuid_elem = find_element(noconnect, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    if not uuid:
        return None
    
    return {
        'type': 'noconnect',
        'at': list(coord),  # Convert tuple to list for JSON
        'uid': uuid
    }


def convert_text(text: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad text to text statement."""
    if not isinstance(text, list) or len(text) == 0 or text[0] != 'text':
        return None
    
    # Extract text string
    text_str = get_atom_value(text, 1, None)
    if not text_str:
        return None
    
    # Extract position (optional)
    at_elem = find_element(text, 'at')
    coord = extract_coord(at_elem) if at_elem else None
    
    result = {
        'type': 'text',
        'string': text_str
    }
    
    if coord:
        result['at'] = list(coord)  # Convert tuple to list for JSON
    
    return result


def extract_stroke(shape: List) -> Dict[str, Any]:
    """Extract stroke properties from shape element."""
    stroke_elem = find_element(shape, 'stroke')
    if not stroke_elem:
        return {'width': 0, 'type': 'default'}
    
    width_elem = find_element(stroke_elem, 'width')
    width = float(get_atom_value(width_elem, 1, 0)) if width_elem else 0
    
    type_elem = find_element(stroke_elem, 'type')
    stroke_type = get_atom_value(type_elem, 1, 'default') if type_elem else 'default'
    
    return {
        'width': width,
        'type': stroke_type
    }


def extract_fill(shape: List) -> Dict[str, Any]:
    """Extract fill properties from shape element."""
    fill_elem = find_element(shape, 'fill')
    if not fill_elem:
        return {'type': 'none'}
    
    type_elem = find_element(fill_elem, 'type')
    fill_type = get_atom_value(type_elem, 1, 'none') if type_elem else 'none'
    
    result = {'type': fill_type}
    
    # Check for color
    color_elem = find_element(fill_elem, 'color')
    if color_elem and len(color_elem) >= 5:
        # color is like ['color', r, g, b, a]
        try:
            r = float(color_elem[1]) if len(color_elem) > 1 else 0
            g = float(color_elem[2]) if len(color_elem) > 2 else 0
            b = float(color_elem[3]) if len(color_elem) > 3 else 0
            a = float(color_elem[4]) if len(color_elem) > 4 else 0
            result['color'] = [r, g, b, a]
        except (ValueError, TypeError, IndexError):
            pass
    
    return result


def convert_bus(bus: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad bus to bus statement."""
    if not isinstance(bus, list) or len(bus) == 0 or bus[0] != 'bus':
        return None
    
    # Extract points
    pts_elem = find_element(bus, 'pts')
    if not pts_elem:
        return None
    
    points = extract_wire_points(pts_elem)
    if len(points) < 2:
        return None
    
    # Extract UUID
    uuid_elem = find_element(bus, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    result = {
        'type': 'bus',
        'points': [list(p) for p in points],  # Convert tuples to lists for JSON
        'uid': uuid_value
    }
    
    return result


def convert_polyline(polyline: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad polyline to polyline statement."""
    if not isinstance(polyline, list) or len(polyline) == 0 or polyline[0] != 'polyline':
        return None
    
    # Extract points from pts
    pts_elem = find_element(polyline, 'pts')
    if not pts_elem:
        return None
    
    points = extract_wire_points(pts_elem)
    if len(points) < 2:
        return None
    
    # Extract UUID
    uuid_elem = find_element(polyline, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    result = {
        'type': 'polyline',
        'points': [list(p) for p in points],  # Convert tuples to lists for JSON
        'uid': uuid_value
    }
    
    return result


def convert_rectangle(rectangle: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad rectangle to rectangle statement."""
    if not isinstance(rectangle, list) or len(rectangle) == 0 or rectangle[0] != 'rectangle':
        return None
    
    # Extract start
    start_elem = find_element(rectangle, 'start')
    start = extract_coord(start_elem) if start_elem else None
    if not start:
        return None
    
    # Extract end
    end_elem = find_element(rectangle, 'end')
    end = extract_coord(end_elem) if end_elem else None
    if not end:
        return None
    
    # Extract stroke and fill
    stroke = extract_stroke(rectangle)
    fill = extract_fill(rectangle)
    
    # Extract UUID
    uuid_elem = find_element(rectangle, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    return {
        'type': 'rectangle',
        'start': list(start),
        'end': list(end),
        'stroke': stroke,
        'fill': fill,
        'uid': uuid_value
    }


def convert_arc(arc: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad arc to arc statement."""
    if not isinstance(arc, list) or len(arc) == 0 or arc[0] != 'arc':
        return None
    
    # Extract start
    start_elem = find_element(arc, 'start')
    start = extract_coord(start_elem) if start_elem else None
    if not start:
        return None
    
    # Extract mid
    mid_elem = find_element(arc, 'mid')
    mid = extract_coord(mid_elem) if mid_elem else None
    if not mid:
        return None
    
    # Extract end
    end_elem = find_element(arc, 'end')
    end = extract_coord(end_elem) if end_elem else None
    if not end:
        return None
    
    # Extract stroke and fill
    stroke = extract_stroke(arc)
    fill = extract_fill(arc)
    
    # Extract UUID
    uuid_elem = find_element(arc, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    return {
        'type': 'arc',
        'start': list(start),
        'mid': list(mid),
        'end': list(end),
        'stroke': stroke,
        'fill': fill,
        'uid': uuid_value
    }


def convert_bezier(bezier: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad bezier to bezier statement."""
    if not isinstance(bezier, list) or len(bezier) == 0 or bezier[0] != 'bezier':
        return None
    
    # Extract points from pts
    pts_elem = find_element(bezier, 'pts')
    if not pts_elem:
        return None
    
    points = extract_wire_points(pts_elem)
    if len(points) < 2:
        return None
    
    # Extract stroke and fill
    stroke = extract_stroke(bezier)
    fill = extract_fill(bezier)
    
    # Extract UUID
    uuid_elem = find_element(bezier, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    return {
        'type': 'bezier',
        'points': [list(p) for p in points],
        'stroke': stroke,
        'fill': fill,
        'uid': uuid_value
    }


def convert_circle(circle: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad circle to circle statement."""
    if not isinstance(circle, list) or len(circle) == 0 or circle[0] != 'circle':
        return None
    
    # Extract center
    center_elem = find_element(circle, 'center')
    center = extract_coord(center_elem) if center_elem else None
    if not center:
        return None
    
    # Extract radius
    radius_elem = find_element(circle, 'radius')
    radius = float(get_atom_value(radius_elem, 1, 0)) if radius_elem else 0
    if radius == 0:
        return None
    
    # Extract stroke and fill
    stroke = extract_stroke(circle)
    fill = extract_fill(circle)
    
    # Extract UUID
    uuid_elem = find_element(circle, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    return {
        'type': 'circle',
        'center': list(center),
        'radius': radius,
        'stroke': stroke,
        'fill': fill,
        'uid': uuid_value
    }


def convert_text_box(text_box: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad text_box to text_box statement."""
    if not isinstance(text_box, list) or len(text_box) == 0 or text_box[0] != 'text_box':
        return None
    
    # Extract text (first string after 'text_box')
    text_value = get_atom_value(text_box, 1, None)
    if not text_value:
        return None
    
    # Extract at (position with rotation)
    at_elem = find_element(text_box, 'at')
    at_coord = extract_coord(at_elem) if at_elem else None
    rot = extract_rotation(at_elem) if at_elem else None
    if not at_coord:
        return None
    
    # Combine coord and rotation
    at_result = list(at_coord)
    if rot is not None:
        at_result.append(rot)
    
    # Extract size
    size_elem = find_element(text_box, 'size')
    size = extract_coord(size_elem) if size_elem else None
    if not size:
        return None
    
    # Extract margins (optional)
    margins_elem = find_element(text_box, 'margins')
    margins = None
    if margins_elem and len(margins_elem) >= 5:
        # margins is like ['margins', top, right, bottom, left]
        try:
            margins = [
                float(margins_elem[1]) if len(margins_elem) > 1 else 0,
                float(margins_elem[2]) if len(margins_elem) > 2 else 0,
                float(margins_elem[3]) if len(margins_elem) > 3 else 0,
                float(margins_elem[4]) if len(margins_elem) > 4 else 0
            ]
        except (ValueError, TypeError, IndexError):
            pass
    
    # Extract stroke and fill
    stroke = extract_stroke(text_box)
    fill = extract_fill(text_box)
    
    # Extract effects (optional) - convert to prop_list format
    effects = None
    effects_elem = find_element(text_box, 'effects')
    if effects_elem:
        # Convert effects to a simple dictionary representation
        # This is a simplified conversion - effects structure is complex
        effects = {}
        font_elem = find_element(effects_elem, 'font')
        if font_elem:
            size_elem = find_element(font_elem, 'size')
            if size_elem and len(size_elem) >= 3:
                effects['font_size'] = str(get_atom_value(size_elem, 1, 1.27))
        
        justify_elem = find_element(effects_elem, 'justify')
        if justify_elem and len(justify_elem) >= 3:
            effects['justify'] = f"{get_atom_value(justify_elem, 1, 'left')} {get_atom_value(justify_elem, 2, 'top')}"
    
    # Extract UUID
    uuid_elem = find_element(text_box, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    result = {
        'type': 'text_box',
        'text': text_value,
        'at': at_result,
        'size': list(size),
        'stroke': stroke,
        'fill': fill,
        'uid': uuid_value
    }
    
    if rot is not None:
        result['rot'] = rot
    if margins is not None:
        result['margins'] = margins
    if effects is not None:
        result['effects'] = effects
    
    return result


def convert_bus_entry(bus_entry: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad bus_entry to bus_entry statement."""
    if not isinstance(bus_entry, list) or len(bus_entry) == 0 or bus_entry[0] != 'bus_entry':
        return None
    
    # Extract at
    at_elem = find_element(bus_entry, 'at')
    at_coord = extract_coord(at_elem) if at_elem else None
    if not at_coord:
        return None
    
    # Extract size
    size_elem = find_element(bus_entry, 'size')
    size = extract_coord(size_elem) if size_elem else None
    if not size:
        return None
    
    # Extract stroke (no fill for bus_entry)
    stroke = extract_stroke(bus_entry)
    
    # Extract UUID
    uuid_elem = find_element(bus_entry, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid_value:
        return None
    
    return {
        'type': 'bus_entry',
        'at': list(at_coord),
        'size': list(size),
        'stroke': stroke,
        'uid': uuid_value
    }


def convert_sheet(sheet: List, sheet_pin_net_map: Optional[Dict[Tuple[str, str], str]] = None) -> Optional[Dict[str, Any]]:
    """Convert KiCad sheet to sheet statement.
    
    Args:
        sheet: KiCad sheet S-expression list
        sheet_pin_net_map: Optional mapping of (sheet_uuid, pin_name) -> label_text
            for resolving net names from labels at pin positions
    """
    if not isinstance(sheet, list) or len(sheet) == 0 or sheet[0] != 'sheet':
        return None
    
    # Extract UUID (required)
    uuid_elem = find_element(sheet, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    # Extract sheet name and file (from properties)
    properties = extract_properties(sheet)
    name = properties.get('Sheetname', properties.get('Sheet name', 'SHEET'))
    
    # Extract file from Sheetfile property (not a direct 'file' element)
    file_value = properties.get('Sheetfile', None)
    if not file_value:
        # Fallback: try to find a 'file' element (for backwards compatibility)
        file_elem = find_element(sheet, 'file')
        file_value = get_atom_value(file_elem, 1, None) if file_elem else None
    if not file_value:
        return None
    
    # Extract position and rotation (optional)
    at_elem = find_element(sheet, 'at')
    coord = None
    rot = None
    if at_elem:
        coord = extract_coord(at_elem)
        rot = extract_rotation(at_elem)
    
    # Extract size (optional)
    size_elem = find_element(sheet, 'size')
    size = None
    if size_elem:
        size = extract_coord(size_elem)
    
    # Extract sheet pins (ins and outs) with coordinates
    ins = {}
    outs = {}
    pin_elements = find_elements(sheet, 'pin')
    for pin in pin_elements:
        if len(pin) >= 3:
            pin_name = get_atom_value(pin, 1, None)
            pin_type = get_atom_value(pin, 2, None)  # input, output, bidirectional, etc.
            if pin_name:
                # Extract pin coordinates and rotation
                pin_at_elem = find_element(pin, 'at')
                pin_coord = None
                pin_rot = None
                if pin_at_elem:
                    pin_coord = extract_coord(pin_at_elem)
                    pin_rot = extract_rotation(pin_at_elem)
                
                # Use label at pin position as net name if available, else fall back to pin name
                if sheet_pin_net_map and (uuid, pin_name) in sheet_pin_net_map:
                    net_name = sheet_pin_net_map[(uuid, pin_name)]
                else:
                    net_name = pin_name
                
                if pin_coord is not None:
                    # Store with coordinates
                    pin_info = {
                        "net": net_name,
                        "at": list(pin_coord)  # Convert tuple to list for JSON
                    }
                    if pin_rot is not None:
                        pin_info["rot"] = pin_rot
                else:
                    # No coordinates - use simple string format
                    pin_info = net_name
                
                # Map pin to appropriate ins/outs based on type
                if pin_type == 'input':
                    ins[pin_name] = pin_info
                elif pin_type == 'output':
                    outs[pin_name] = pin_info
                else:
                    # Default to outs for bidirectional or unknown
                    outs[pin_name] = pin_info
    
    # Extract page number from instances section
    # Structure: (instances (project "" (path "/uuid" (page "X"))))
    page_number = None
    instances_elem = find_element(sheet, 'instances')
    if instances_elem:
        for item in instances_elem[1:]:
            if isinstance(item, list) and len(item) > 0 and item[0] == 'project':
                # Find path element within project
                for path_item in item[1:]:
                    if isinstance(path_item, list) and len(path_item) > 0 and path_item[0] == 'path':
                        # Find page element within path
                        page_elem = find_element(path_item, 'page')
                        if page_elem and len(page_elem) > 1:
                            page_number = get_atom_value(page_elem, 1, None)
                            break
                if page_number:
                    break
    
    result = {
        'type': 'sheet',
        'name': name,
        'file': file_value,
        'uid': uuid
    }
    
    if coord:
        result['at'] = list(coord)  # Convert tuple to list for JSON
    if rot is not None:
        result['rot'] = rot
    if size:
        result['size'] = list(size)  # Convert tuple to list for JSON
    if page_number:
        result['page'] = page_number
    if ins:
        result['ins'] = ins
    if outs:
        result['outs'] = outs
    
    # Extract properties (excluding Sheetname and Sheetfile which are already extracted)
    props = {}
    for key, value in properties.items():
        if key not in ('Sheetname', 'Sheet name', 'Sheetfile'):
            props[key.lower()] = value
    if props:
        result['props'] = props
    
    return result


def convert_hier_label(hier_label: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad hierarchical_label to hier statement."""
    if not isinstance(hier_label, list) or len(hier_label) == 0 or hier_label[0] != 'hierarchical_label':
        return None
    
    # Extract label text
    label_text = get_atom_value(hier_label, 1, None)
    if not label_text:
        return None
    
    # Extract position and rotation (required)
    at_elem = find_element(hier_label, 'at')
    coord = None
    rot = None
    if at_elem:
        coord = extract_coord(at_elem)
        rot = extract_rotation(at_elem)
    
    if not coord:
        return None
    
    # Extract UUID
    uuid_elem = find_element(hier_label, 'uuid')
    uuid_value = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    
    # Extract shape (optional)
    shape_elem = find_element(hier_label, 'shape')
    shape = get_atom_value(shape_elem, 1, None) if shape_elem else None
    
    result = {
        'type': 'hier',
        'name': label_text,
        'at': list(coord)  # Convert tuple to list for JSON
    }
    
    if uuid_value:
        result['uid'] = uuid_value
    if rot is not None:
        result['rot'] = rot
    if shape:
        result['shape'] = shape
    
    return result


# =============================================================================
# Junction ID Assignment and cwire Generation
# =============================================================================

def assign_junction_ids(statements: List[Dict[str, Any]]) -> None:
    """
    Assign junction IDs (JUNC1, JUNC2, etc.) to all junctions in statements.
    
    Modifies statements in place.
    """
    junction_counter = 1
    for stmt in statements:
        if stmt.get('type') == 'junction' and 'id' not in stmt:
            stmt['id'] = f'JUNC{junction_counter}'
            junction_counter += 1


def assign_label_ids(statements: List[Dict[str, Any]]) -> None:
    """
    Assign unique IDs to all label, glabel, and hier statements.
    
    - label  -> LBL1, LBL2, ...
    - glabel -> GLBL1, GLBL2, ...
    - hier   -> HIER1, HIER2, ...
    
    Modifies statements in place.
    """
    label_counter = 1
    glabel_counter = 1
    hier_counter = 1
    for stmt in statements:
        stmt_type = stmt.get('type')
        if stmt_type == 'label' and 'id' not in stmt:
            stmt['id'] = f'LBL{label_counter}'
            label_counter += 1
        elif stmt_type == 'glabel' and 'id' not in stmt:
            stmt['id'] = f'GLBL{glabel_counter}'
            glabel_counter += 1
        elif stmt_type == 'hier' and 'id' not in stmt:
            stmt['id'] = f'HIER{hier_counter}'
            hier_counter += 1


def find_pin_at_position(pos: Tuple[float, float], 
                         components: List[Dict[str, Any]], 
                         tolerance: float = 0.1) -> Optional[Dict[str, Any]]:
    """
    Find a component pin at the given position.
    
    Returns:
        Dict with 'ref' and 'pin' keys if found, None otherwise
    """
    for comp in components:
        pins = comp.get('pins', {})
        comp_at = comp.get('at')
        if not comp_at:
            continue
        
        # For each pin, we need to check if the position matches
        # This is a simplified check - in practice we'd need to transform pin positions
        # based on component position and rotation
        # For now, we'll rely on the pin positions being stored in the component
        for pin_num, net_name in pins.items():
            # We don't have direct pin positions here, so we can't do exact matching
            # This would require access to library symbols
            pass
    
    return None


def find_junction_at_position(pos: Tuple[float, float], 
                              junctions: List[Dict[str, Any]], 
                              tolerance: float = 0.1) -> Optional[Dict[str, Any]]:
    """
    Find a junction at the given position.
    
    Returns:
        Junction dict if found, None otherwise
    """
    for junction in junctions:
        junc_at = junction.get('at', [])
        if len(junc_at) >= 2:
            dx = abs(junc_at[0] - pos[0])
            dy = abs(junc_at[1] - pos[1])
            if dx < tolerance and dy < tolerance:
                return junction
    return None


def analyze_wire_chains(statements: List[Dict[str, Any]], 
                        lib_symbols_map: Dict[str, Dict]) -> Tuple[List[Dict[str, Any]], Set[str], Set[str]]:
    """
    Analyze wire connectivity and generate cwire and cwiredef statements where possible.
    
    A cwire can be generated when:
    1. A chain of wires connects two component pins
    2. A chain of wires connects a component pin to a junction
    3. A chain of wires connects two junctions
    4. A chain of wires connects a component pin to a label/glabel/hier
    5. A chain of wires connects a junction to a label/glabel/hier
    6. A chain of wires connects two labels
    
    For each cwire, a corresponding cwiredef is generated with the wire path coordinates.
    
    Args:
        statements: List of trace_sch statements
        lib_symbols_map: Map of library symbols for pin position lookup
    
    Returns:
        Tuple of:
        - List of cwire and cwiredef statements to add
        - Set of wire UIDs that were consolidated into cwires (should be removed from output)
        - Set of label IDs that became cwire endpoints (should be filtered from standalone labels)
    """
    # Extract relevant statements
    components = [s for s in statements if s.get('type') == 'component']
    wires = [s for s in statements if s.get('type') == 'wire']
    junctions = [s for s in statements if s.get('type') == 'junction']
    all_labels = [s for s in statements if s.get('type') in ('label', 'glabel', 'hier')]
    
    results = []  # Will contain both cwires and cwiredefs
    cwire_counter = 1  # For generating CW1, CW2, etc.
    label_ids_used_as_endpoints = set()  # Track which label IDs became cwire endpoints
    
    # Build junction position map
    junction_pos_map: Dict[Tuple[float, float], Dict[str, Any]] = {}
    for junction in junctions:
        junc_at = junction.get('at', [])
        if len(junc_at) >= 2:
            # Round to avoid floating point issues
            pos = (round(junc_at[0], 2), round(junc_at[1], 2))
            junction_pos_map[pos] = junction
    
    # Build component pin position map
    # This maps (x, y) -> {'ref': ref, 'pin': pin_num, 'net': net_name}
    pin_pos_map: Dict[Tuple[float, float], Dict[str, Any]] = {}
    
    # For each component, we need to calculate pin positions
    # This requires the library symbol definitions
    for comp in components:
        ref = comp.get('ref')
        symbol = comp.get('symbol')
        comp_at = comp.get('at', [0, 0])
        comp_rot = comp.get('rot', 0)
        comp_mirror = comp.get('mirror')
        unit = comp.get('unit', 1)
        body_style = comp.get('body_style', 1)
        pins = comp.get('pins', {})
        
        if not symbol or symbol not in lib_symbols_map:
            continue
        
        lib_symbol = lib_symbols_map[symbol]
        
        # Get pins with inheritance applied
        lib_pins = _get_pins_with_inheritance(lib_symbol, unit, body_style)
        
        # Get pin positions from library symbol
        for pin_num, net_name in pins.items():
            # Look up pin position in library symbol (inheritance already applied)
            pin_info = lib_pins.get(str(pin_num))
            
            if pin_info:
                x_off, y_off, pin_rot = pin_info
                # Transform pin position
                pin_pos = transform_pin_coordinate((x_off, y_off, pin_rot), 
                                                   tuple(comp_at), 
                                                   comp_rot, comp_mirror)
                pos = (round(pin_pos[0], 2), round(pin_pos[1], 2))
                pin_pos_map[pos] = {
                    'ref': ref,
                    'pin': pin_num,
                    'net': net_name
                }
    
    # Add sheet pin positions to pin_pos_map (sheets are treated like components)
    sheet_stmts = [s for s in statements if s.get('type') == 'sheet']
    for sheet_stmt in sheet_stmts:
        sheet_name = sheet_stmt.get('name')
        for pin_dict_key in ('ins', 'outs'):
            for pin_name, pin_value in sheet_stmt.get(pin_dict_key, {}).items():
                if isinstance(pin_value, dict):
                    pin_at = pin_value.get('at', [])
                    net = pin_value.get('net', pin_name)
                else:
                    continue
                if len(pin_at) >= 2:
                    pos = (round(pin_at[0], 2), round(pin_at[1], 2))
                    pin_pos_map[pos] = {
                        'ref': sheet_name,
                        'pin': pin_name,
                        'net': net
                    }
    
    # Build label position map (label, glabel, hier)
    # Maps (x, y) -> {type, name, id}
    label_pos_map: Dict[Tuple[float, float], Dict[str, Any]] = {}
    for lbl in all_labels:
        lbl_at = lbl.get('at', [])
        if len(lbl_at) >= 2:
            pos = (round(lbl_at[0], 2), round(lbl_at[1], 2))
            label_pos_map[pos] = {
                'type': lbl.get('type'),
                'name': lbl.get('name', ''),
                'id': lbl.get('id', '')
            }
    
    # Build wire endpoint map
    # Maps position -> list of wires that have an endpoint there
    # Wire endpoints are snapped to nearby pin/junction/label positions (tolerance-based)
    wire_endpoint_map: Dict[Tuple[float, float], List[Dict[str, Any]]] = {}
    snap_tolerance = 0.02
    all_snap_positions = set(pin_pos_map.keys()) | set(junction_pos_map.keys()) | set(label_pos_map.keys())
    
    def snap_position(pos):
        """Snap a position to a known endpoint if within tolerance."""
        for snap_pos in all_snap_positions:
            if abs(pos[0] - snap_pos[0]) < snap_tolerance and abs(pos[1] - snap_pos[1]) < snap_tolerance:
                return snap_pos
        return pos
    
    def round_and_snap(x, y):
        """Round to 2 decimal places then snap to nearest known position."""
        return snap_position((round(x, 2), round(y, 2)))
    
    for wire in wires:
        points = wire.get('points', [])
        if len(points) >= 2:
            start = round_and_snap(points[0][0], points[0][1])
            end = round_and_snap(points[-1][0], points[-1][1])
            
            if start not in wire_endpoint_map:
                wire_endpoint_map[start] = []
            wire_endpoint_map[start].append(wire)
            
            if end not in wire_endpoint_map:
                wire_endpoint_map[end] = []
            wire_endpoint_map[end].append(wire)
    
    def extract_chain_points(chain: List[Dict[str, Any]], start_pos: Tuple[float, float]) -> List[List[float]]:
        """Extract ordered points from a wire chain starting from start_pos."""
        if not chain:
            return []
        
        all_points = [list(start_pos)]
        current_pos = start_pos
        
        for wire in chain:
            points = wire.get('points', [])
            if len(points) >= 2:
                wire_start = round_and_snap(points[0][0], points[0][1])
                wire_end = round_and_snap(points[-1][0], points[-1][1])
                
                # Determine which end connects to current_pos
                if abs(wire_start[0] - current_pos[0]) < 0.1 and abs(wire_start[1] - current_pos[1]) < 0.1:
                    # Wire starts at current_pos, add end point
                    all_points.append(list(wire_end))
                    current_pos = wire_end
                else:
                    # Wire ends at current_pos, add start point
                    all_points.append(list(wire_start))
                    current_pos = wire_start
        
        return all_points
    
    # Find wire chains that connect pins/junctions
    # A chain starts at a pin or junction and ends at another pin or junction
    processed_wires = set()
    
    # Start from each pin position
    for pin_pos, pin_info in pin_pos_map.items():
        if pin_pos not in wire_endpoint_map:
            continue
        
        # Follow wire chain from this pin
        for start_wire in wire_endpoint_map[pin_pos]:
            wire_uid = start_wire.get('uid', '')
            if wire_uid in processed_wires:
                continue
            
            # Follow the chain
            chain = [start_wire]
            current_pos = pin_pos
            visited_wires = {wire_uid}
            
            # Get the other end of the first wire
            points = start_wire.get('points', [])
            if len(points) >= 2:
                start = round_and_snap(points[0][0], points[0][1])
                end = round_and_snap(points[-1][0], points[-1][1])
                current_pos = end if start == pin_pos else start
            
            # Follow connected wires
            while True:
                # Check if we've reached a destination (pin or junction)
                if current_pos in pin_pos_map and current_pos != pin_pos:
                    # Found a pin-to-pin connection
                    dest_pin = pin_pos_map[current_pos]
                    
                    # Generate cwire ref
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    # Create cwire
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': 'pin', 'ref': pin_info['ref'], 'pin': pin_info['pin']},
                        'to': {'type': 'pin', 'ref': dest_pin['ref'], 'pin': dest_pin['pin']}
                    }
                    
                    # Add net if both pins have the same net
                    if pin_info.get('net') and pin_info['net'] == dest_pin.get('net'):
                        if pin_info['net'] not in ('NONE', 'DNC'):
                            cwire['net'] = pin_info['net']
                    
                    results.append(cwire)
                    
                    # Create cwiredef with the wire path
                    chain_points = extract_chain_points(chain, pin_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    # Mark all wires in chain as processed
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    break
                
                if current_pos in junction_pos_map:
                    # Found a pin-to-junction connection
                    dest_junction = junction_pos_map[current_pos]
                    
                    # Generate cwire ref
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    # Create cwire
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': 'pin', 'ref': pin_info['ref'], 'pin': pin_info['pin']},
                        'to': {'type': 'junction', 'id': dest_junction.get('id', 'JUNC?')}
                    }
                    
                    # Add net from pin
                    if pin_info.get('net') and pin_info['net'] not in ('NONE', 'DNC'):
                        cwire['net'] = pin_info['net']
                    
                    results.append(cwire)
                    
                    # Create cwiredef with the wire path
                    chain_points = extract_chain_points(chain, pin_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    # Mark all wires in chain as processed
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    break
                
                if current_pos in label_pos_map:
                    dest_label = label_pos_map[current_pos]
                    
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': 'pin', 'ref': pin_info['ref'], 'pin': pin_info['pin']},
                        'to': {'type': dest_label['type'], 'id': dest_label['id']}
                    }
                    
                    net_name = dest_label.get('name') or pin_info.get('net')
                    if net_name and net_name not in ('NONE', 'DNC'):
                        cwire['net'] = net_name
                    
                    results.append(cwire)
                    
                    chain_points = extract_chain_points(chain, pin_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    label_ids_used_as_endpoints.add(dest_label['id'])
                    break
                
                # Find next wire in chain
                next_wires = wire_endpoint_map.get(current_pos, [])
                found_next = False
                for next_wire in next_wires:
                    next_uid = next_wire.get('uid', '')
                    if next_uid not in visited_wires:
                        chain.append(next_wire)
                        visited_wires.add(next_uid)
                        
                        # Get the other end
                        points = next_wire.get('points', [])
                        if len(points) >= 2:
                            start = round_and_snap(points[0][0], points[0][1])
                            end = round_and_snap(points[-1][0], points[-1][1])
                            current_pos = end if start == current_pos else start
                        
                        found_next = True
                        break
                
                if not found_next:
                    # Dead end - no cwire possible
                    break
    
    # Also check junction-to-junction connections
    for junc_pos, junction in junction_pos_map.items():
        if junc_pos not in wire_endpoint_map:
            continue
        
        for start_wire in wire_endpoint_map[junc_pos]:
            wire_uid = start_wire.get('uid', '')
            if wire_uid in processed_wires:
                continue
            
            # Follow the chain
            chain = [start_wire]
            current_pos = junc_pos
            visited_wires = {wire_uid}
            
            # Get the other end of the first wire
            points = start_wire.get('points', [])
            if len(points) >= 2:
                start = round_and_snap(points[0][0], points[0][1])
                end = round_and_snap(points[-1][0], points[-1][1])
                current_pos = end if start == junc_pos else start
            
            # Follow connected wires
            while True:
                # Check if we've reached another junction
                if current_pos in junction_pos_map and current_pos != junc_pos:
                    dest_junction = junction_pos_map[current_pos]
                    
                    # Generate cwire ref
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    # Create cwire (junction to junction)
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': 'junction', 'id': junction.get('id', 'JUNC?')},
                        'to': {'type': 'junction', 'id': dest_junction.get('id', 'JUNC?')}
                    }
                    
                    # Junction-to-junction needs a net name
                    # Try to infer from connected pins
                    # For now, skip if we can't determine the net
                    
                    results.append(cwire)
                    
                    # Create cwiredef with the wire path
                    chain_points = extract_chain_points(chain, junc_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    # Mark all wires in chain as processed
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    break
                
                # Check if we've reached a pin
                if current_pos in pin_pos_map:
                    # This would be a junction-to-pin, but we already handle pin-to-junction
                    # Skip to avoid duplicates
                    break
                
                if current_pos in label_pos_map:
                    dest_label = label_pos_map[current_pos]
                    
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': 'junction', 'id': junction.get('id', 'JUNC?')},
                        'to': {'type': dest_label['type'], 'id': dest_label['id']}
                    }
                    
                    net_name = dest_label.get('name')
                    if net_name and net_name not in ('NONE', 'DNC'):
                        cwire['net'] = net_name
                    
                    results.append(cwire)
                    
                    chain_points = extract_chain_points(chain, junc_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    label_ids_used_as_endpoints.add(dest_label['id'])
                    break
                
                # Find next wire in chain
                next_wires = wire_endpoint_map.get(current_pos, [])
                found_next = False
                for next_wire in next_wires:
                    next_uid = next_wire.get('uid', '')
                    if next_uid not in visited_wires:
                        chain.append(next_wire)
                        visited_wires.add(next_uid)
                        
                        # Get the other end
                        points = next_wire.get('points', [])
                        if len(points) >= 2:
                            start = round_and_snap(points[0][0], points[0][1])
                            end = round_and_snap(points[-1][0], points[-1][1])
                            current_pos = end if start == current_pos else start
                        
                        found_next = True
                        break
                
                if not found_next:
                    break
    
    # Phase 3: Label-start chains (label-to-pin, label-to-junction, label-to-label)
    for lbl_pos, lbl_info in label_pos_map.items():
        if lbl_pos not in wire_endpoint_map:
            continue
        
        for start_wire in wire_endpoint_map[lbl_pos]:
            wire_uid = start_wire.get('uid', '')
            if wire_uid in processed_wires:
                continue
            
            chain = [start_wire]
            current_pos = lbl_pos
            visited_wires = {wire_uid}
            
            points = start_wire.get('points', [])
            if len(points) >= 2:
                start = round_and_snap(points[0][0], points[0][1])
                end = round_and_snap(points[-1][0], points[-1][1])
                current_pos = end if start == lbl_pos else start
            
            while True:
                if current_pos in pin_pos_map:
                    dest_pin = pin_pos_map[current_pos]
                    
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': lbl_info['type'], 'id': lbl_info['id']},
                        'to': {'type': 'pin', 'ref': dest_pin['ref'], 'pin': dest_pin['pin']}
                    }
                    
                    net_name = lbl_info.get('name') or dest_pin.get('net')
                    if net_name and net_name not in ('NONE', 'DNC'):
                        cwire['net'] = net_name
                    
                    results.append(cwire)
                    
                    chain_points = extract_chain_points(chain, lbl_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    label_ids_used_as_endpoints.add(lbl_info['id'])
                    break
                
                if current_pos in junction_pos_map:
                    dest_junction = junction_pos_map[current_pos]
                    
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': lbl_info['type'], 'id': lbl_info['id']},
                        'to': {'type': 'junction', 'id': dest_junction.get('id', 'JUNC?')}
                    }
                    
                    net_name = lbl_info.get('name')
                    if net_name and net_name not in ('NONE', 'DNC'):
                        cwire['net'] = net_name
                    
                    results.append(cwire)
                    
                    chain_points = extract_chain_points(chain, lbl_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    label_ids_used_as_endpoints.add(lbl_info['id'])
                    break
                
                if current_pos in label_pos_map and current_pos != lbl_pos:
                    dest_label = label_pos_map[current_pos]
                    
                    cwire_ref = f"CW{cwire_counter}"
                    cwire_counter += 1
                    
                    cwire = {
                        'type': 'cwire',
                        'ref': cwire_ref,
                        'from': {'type': lbl_info['type'], 'id': lbl_info['id']},
                        'to': {'type': dest_label['type'], 'id': dest_label['id']}
                    }
                    
                    net_name = lbl_info.get('name') or dest_label.get('name')
                    if net_name and net_name not in ('NONE', 'DNC'):
                        cwire['net'] = net_name
                    
                    results.append(cwire)
                    
                    chain_points = extract_chain_points(chain, lbl_pos)
                    if len(chain_points) >= 2:
                        cwiredef = {
                            'type': 'cwiredef',
                            'ref': cwire_ref,
                            'points': chain_points
                        }
                        results.append(cwiredef)
                    
                    for w in chain:
                        processed_wires.add(w.get('uid', ''))
                    label_ids_used_as_endpoints.add(lbl_info['id'])
                    label_ids_used_as_endpoints.add(dest_label['id'])
                    break
                
                next_wires = wire_endpoint_map.get(current_pos, [])
                found_next = False
                for next_wire in next_wires:
                    next_uid = next_wire.get('uid', '')
                    if next_uid not in visited_wires:
                        chain.append(next_wire)
                        visited_wires.add(next_uid)
                        
                        points = next_wire.get('points', [])
                        if len(points) >= 2:
                            start = round_and_snap(points[0][0], points[0][1])
                            end = round_and_snap(points[-1][0], points[-1][1])
                            current_pos = end if start == current_pos else start
                        
                        found_next = True
                        break
                
                if not found_next:
                    break
    
    return results, processed_wires, label_ids_used_as_endpoints


def transform_pin_coordinate(pin_offset: Tuple[float, float, int], 
                             symbol_pos: Tuple[float, float], 
                             symbol_rot: Optional[int], mirror: Optional[str] = None) -> Tuple[float, float]:
    """
    Transform pin offset by symbol position, mirror, and rotation.
    
    Args:
        pin_offset: (x_offset, y_offset, pin_rotation) from library symbol
        symbol_pos: (x, y) position of symbol instance
        symbol_rot: Rotation angle in degrees (0, 90, 180, 270)
        mirror: Optional mirror axis - "x" or "y"
    
    Returns:
        Transformed (x, y) coordinate
    """
    import math
    
    x_off, y_off, pin_rot = pin_offset
    x_pos, y_pos = symbol_pos
    
    # Apply pin rotation transformation to get the actual connection point
    y_off = -y_off
    
    # Apply mirror transformation before rotation
    if mirror == 'x':
        y_off = -y_off
    elif mirror == 'y':
        x_off = -x_off
    
    # Normalize symbol rotation to 0-360 range
    rot = symbol_rot if symbol_rot is not None else 0
    rot = rot % 360
    if rot < 0:
        rot += 360
    
    # Rotate pin offset around origin by symbol rotation
    if rot == 0:
        x_rot, y_rot = x_off, y_off
    elif rot == 90:
        x_rot, y_rot = y_off, -x_off
    elif rot == 180:
        x_rot, y_rot = -x_off, -y_off
    elif rot == 270:
        x_rot, y_rot = -y_off, x_off
    else:
        rad = math.radians(rot)
        cos_r = math.cos(rad)
        sin_r = math.sin(rad)
        x_rot = x_off * cos_r + y_off * sin_r
        y_rot = -x_off * sin_r + y_off * cos_r
    
    return (x_rot + x_pos, y_rot + y_pos)


# =============================================================================
# Main Extraction Function
# =============================================================================

def extract_trace_elements(sexp_data: List) -> List[Dict[str, Any]]:
    """Extract and convert all trace_sch elements from parsed S-expression."""
    statements = []
    
    if not isinstance(sexp_data, list) or len(sexp_data) == 0:
        return statements
    
    # Find kicad_sch root
    if sexp_data[0] != 'kicad_sch':
        return statements
    
    # Extract metadata from kicad_sch root and add as statements at the beginning
    # Note: kicad_ver, kicad_gen, kicad_gen_ver are no longer extracted - they are hardcoded in the converter
    metadata_statements = []
    
    # Extract uuid (file_uid)
    uuid_elem = find_element(sexp_data, 'uuid')
    if uuid_elem:
        uuid_value = get_atom_value(uuid_elem, 1, None)
        if uuid_value:
            metadata_statements.append({
                'type': 'file_uid',
                'value': uuid_value
            })
    
    # Extract paper
    paper_elem = find_element(sexp_data, 'paper')
    if paper_elem:
        paper_value = get_atom_value(paper_elem, 1, None)
        if paper_value:
            metadata_statements.append({
                'type': 'paper',
                'value': paper_value
            })
    
    # Extract title_block
    tb_elem = find_element(sexp_data, 'title_block')
    if tb_elem:
        tb_stmt = {'type': 'title_block'}
        title_elem = find_element(tb_elem, 'title')
        if title_elem:
            val = get_atom_value(title_elem, 1, None)
            if val:
                tb_stmt['title'] = val
        date_elem = find_element(tb_elem, 'date')
        if date_elem:
            val = get_atom_value(date_elem, 1, None)
            if val:
                tb_stmt['date'] = val
        rev_elem = find_element(tb_elem, 'rev')
        if rev_elem:
            val = get_atom_value(rev_elem, 1, None)
            if val:
                tb_stmt['rev'] = val
        company_elem = find_element(tb_elem, 'company')
        if company_elem:
            val = get_atom_value(company_elem, 1, None)
            if val:
                tb_stmt['company'] = val
        comments = {}
        for sub in tb_elem[1:]:
            if isinstance(sub, list) and len(sub) >= 3 and sub[0] == 'comment':
                try:
                    comment_num = int(sub[1])
                    comment_text = sub[2]
                    if isinstance(comment_text, str):
                        comments[comment_num] = comment_text
                except (ValueError, TypeError, IndexError):
                    pass
        if comments:
            tb_stmt['comments'] = comments
        if len(tb_stmt) > 1:
            metadata_statements.append(tb_stmt)
    
    # Add metadata statements at the beginning
    statements.extend(metadata_statements)
    
    # Get all elements from kicad_sch
    all_elements = sexp_data[1:] if len(sexp_data) > 1 else []
    
    # Extract library symbol definitions first
    lib_symbols_map = extract_library_symbols(sexp_data)
    
    # Separate elements by type
    symbols = find_elements(all_elements, 'symbol')
    wires = find_elements(all_elements, 'wire')
    labels = find_elements(all_elements, 'label')
    glabels = find_elements(all_elements, 'global_label')
    hier_labels = find_elements(all_elements, 'hierarchical_label')
    junctions = find_elements(all_elements, 'junction')
    noconnects = find_elements(all_elements, 'no_connect')
    texts = find_elements(all_elements, 'text')
    buses = find_elements(all_elements, 'bus')
    polylines = find_elements(all_elements, 'polyline')
    rectangles = find_elements(all_elements, 'rectangle')
    arcs = find_elements(all_elements, 'arc')
    beziers = find_elements(all_elements, 'bezier')
    circles = find_elements(all_elements, 'circle')
    text_boxes = find_elements(all_elements, 'text_box')
    bus_entries = find_elements(all_elements, 'bus_entry')
    sheets = find_elements(all_elements, 'sheet')
    
    # Convert wires and junctions to dict format
    wire_dicts = convert_wires_to_dict_format(wires)
    junction_dicts = convert_junctions_to_dict_format(junctions)
    
    # Find wire islands using the more accurate algorithm
    wire_islands = find_wire_islands(wire_dicts, junction_dicts, tolerance=0.01)
    
    # Assign net names to islands based on pin labels and wire connections
    point_to_net = assign_nets_to_wire_islands(wire_islands, symbols, wires, labels, glabels, lib_symbols_map, junctions)
    
    # Find labels that are at pin positions (these should not be converted to separate labels)
    pin_label_positions = find_labels_at_pin_positions(symbols, labels, lib_symbols_map)
    
    # Find labels at sheet pin positions (absorb into sheet ins/outs as net names)
    sheet_pin_label_positions, sheet_pin_net_map = find_labels_at_sheet_pin_positions(sheets, labels)
    pin_label_positions |= sheet_pin_label_positions
    
    # Find noconnects that are at pin positions (these should not be converted to separate noconnects)
    pin_noconnect_positions = find_noconnects_at_pin_positions(symbols, noconnects, lib_symbols_map)
    
    # Convert symbols to components (including power symbols - they're treated as normal components now)
    for symbol in symbols:
        comp = convert_symbol(symbol, wires, junctions, labels, glabels, point_to_net, lib_symbols_map, noconnects)
        if comp:
            statements.append(comp)
    
    # Extract nets (from point_to_net mapping and labels/global labels)
    nets = set(point_to_net.values())
    for label in labels:
        label_text = get_atom_value(label, 1, None)
        if label_text:
            nets.add(label_text)
    for glabel in glabels:
        glabel_text = get_atom_value(glabel, 1, None)
        if glabel_text:
            nets.add(glabel_text)
    
    # Add net statements
    for net_name in sorted(nets):
        statements.append({
            'type': 'net',
            'name': net_name
        })
    
    # Convert wires
    for wire in wires:
        wire_stmt = convert_wire(wire)
        if wire_stmt:
            statements.append(wire_stmt)
    
    # Convert labels (skip labels at pin positions ONLY if no wire connects there)
    # Labels at pin positions that also connect to wires are handled by cwire chain analysis
    tolerance = 0.1
    wire_positions: set = set()
    for wire in wires:
        wire_stmt_temp = convert_wire(wire)
        if wire_stmt_temp:
            pts = wire_stmt_temp.get('points', [])
            for p in pts:
                wire_positions.add((round(p[0], 1), round(p[1], 1)))
    
    for label in labels:
        at_elem = find_element(label, 'at')
        if at_elem:
            label_pos = extract_coord(at_elem)
            if label_pos:
                is_pin_label = False
                for pin_pos in pin_label_positions:
                    dx = abs(pin_pos[0] - label_pos[0])
                    dy = abs(pin_pos[1] - label_pos[1])
                    if dx < tolerance and dy < tolerance:
                        is_pin_label = True
                        break
                if is_pin_label:
                    continue
        label_stmt = convert_label(label)
        if label_stmt:
            statements.append(label_stmt)
    
    # Convert global labels
    for glabel in glabels:
        glabel_stmt = convert_glabel(glabel)
        if glabel_stmt:
            statements.append(glabel_stmt)
    
    # Convert hierarchical labels
    for hier_label in hier_labels:
        hier_stmt = convert_hier_label(hier_label)
        if hier_stmt:
            statements.append(hier_stmt)
    
    # Convert junctions
    for junction in junctions:
        junction_stmt = convert_junction(junction)
        if junction_stmt:
            statements.append(junction_stmt)
    
    # Convert no_connects (skip noconnects that are at pin positions)
    tolerance = 0.1
    for noconnect in noconnects:
        at_elem = find_element(noconnect, 'at')
        if at_elem:
            nc_pos = extract_coord(at_elem)
            if nc_pos:
                # Check if this noconnect is at a pin position (with tolerance)
                is_pin_noconnect = False
                for pin_pos in pin_noconnect_positions:
                    dx = abs(pin_pos[0] - nc_pos[0])
                    dy = abs(pin_pos[1] - nc_pos[1])
                    if dx < tolerance and dy < tolerance:
                        is_pin_noconnect = True
                        break
                if is_pin_noconnect:
                    continue
        noconnect_stmt = convert_noconnect(noconnect)
        if noconnect_stmt:
            statements.append(noconnect_stmt)
    
    # Convert texts
    for text in texts:
        text_stmt = convert_text(text)
        if text_stmt:
            statements.append(text_stmt)
    
    # Convert buses
    for bus in buses:
        bus_stmt = convert_bus(bus)
        if bus_stmt:
            statements.append(bus_stmt)
    
    # Convert polylines
    for polyline in polylines:
        polyline_stmt = convert_polyline(polyline)
        if polyline_stmt:
            statements.append(polyline_stmt)
    
    # Convert rectangles
    for rectangle in rectangles:
        rectangle_stmt = convert_rectangle(rectangle)
        if rectangle_stmt:
            statements.append(rectangle_stmt)
    
    # Convert arcs
    for arc in arcs:
        arc_stmt = convert_arc(arc)
        if arc_stmt:
            statements.append(arc_stmt)
    
    # Convert beziers
    for bezier in beziers:
        bezier_stmt = convert_bezier(bezier)
        if bezier_stmt:
            statements.append(bezier_stmt)
    
    # Convert circles
    for circle in circles:
        circle_stmt = convert_circle(circle)
        if circle_stmt:
            statements.append(circle_stmt)
    
    # Convert text_boxes
    for text_box in text_boxes:
        text_box_stmt = convert_text_box(text_box)
        if text_box_stmt:
            statements.append(text_box_stmt)
    
    # Convert bus_entries
    for bus_entry in bus_entries:
        bus_entry_stmt = convert_bus_entry(bus_entry)
        if bus_entry_stmt:
            statements.append(bus_entry_stmt)
    
    # Convert sheets
    for sheet in sheets:
        sheet_stmt = convert_sheet(sheet, sheet_pin_net_map)
        if sheet_stmt:
            statements.append(sheet_stmt)
    
    # Post-processing: Assign junction IDs
    assign_junction_ids(statements)
    
    # Post-processing: Assign label IDs (LBL1, GLBL1, HIER1, etc.)
    assign_label_ids(statements)
    
    # Post-processing: Generate cwires from wire chains
    # This analyzes wire connectivity and creates cwire statements
    # where wires form chains connecting pins and/or junctions
    cwire_statements, processed_wire_uids, cwire_label_ids = analyze_wire_chains(statements, lib_symbols_map)
    
    # Remove wires that were consolidated into cwires
    if processed_wire_uids:
        statements = [s for s in statements if not (s.get('type') == 'wire' and s.get('uid') in processed_wire_uids)]
    
    # Labels that became cwire endpoints are kept as standalone statements
    # (they carry position/name info needed by the reverse conversion).
    # The cwire references them by ID.
    
    # Add cwire and cwiredef statements
    statements.extend(cwire_statements)
    
    return statements


# =============================================================================
# Main Entry Point
# =============================================================================

def sexp_to_trace_json(content: str) -> List[Dict[str, Any]]:
    """
    Convert KiCad S-expression content to trace_sch JSON format.
    
    Args:
        content: The .kicad_sch file content as a string
        
    Returns:
        List of dictionaries, each representing a trace_sch statement
    """
    # Parse S-expression
    sexp_data = parse_sexp(content)
    
    # Extract and convert to trace_sch format
    statements = extract_trace_elements(sexp_data)
    
    return statements


# =============================================================================
# Testing
# =============================================================================

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) != 2:
        print("Usage: python sexp_to_trace_json.py <kicad_sch_file>")
        sys.exit(1)
    
    filename = sys.argv[1]
    with open(filename, "r") as file:
        content = file.read()
    
    try:
        statements = sexp_to_trace_json(content)
        with open("output.json", "w") as file:
            json.dump(statements, file, indent=2)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
