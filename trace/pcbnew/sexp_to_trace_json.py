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
S-Expression to TRACE-JSON Converter for PCB

Converts KiCad .kicad_pcb S-expression files to trace_pcb-compatible JSON format.
Uses common S-expression parser to parse S-expressions, then transforms
the data to match the trace_pcb format structure.
"""

import json
import sys
import os
from typing import List, Dict, Any, Optional, Tuple, Union

# Import common S-expression parsing utilities
try:
    from ..common.sexp_parser import parse_sexp
    from ..common.sexp_helpers import (
        find_element, find_elements, get_atom_value,
        extract_coord, extract_rotation
    )
except (ImportError, ValueError):
    # Fallback for direct script execution
    common_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'common')
    if common_dir not in sys.path:
        sys.path.insert(0, common_dir)
    from sexp_parser import parse_sexp
    from sexp_helpers import find_element, find_elements, get_atom_value, extract_coord, extract_rotation

# Import progress reporter
try:
    from ..common.progress_reporter import ProgressReporter
except (ImportError, ValueError):
    from progress_reporter import ProgressReporter


# =============================================================================
# Helper Functions for PCB-specific Extraction
# =============================================================================

def extract_properties(footprint: List) -> Dict[str, str]:
    """Extract properties from footprint element."""
    props = {}
    property_elements = find_elements(footprint, 'property')
    
    for prop in property_elements:
        if len(prop) >= 3:
            prop_name = get_atom_value(prop, 1, '')
            prop_value = get_atom_value(prop, 2, '')
            if prop_name:
                props[prop_name] = prop_value
    
    return props


def extract_pad_number(pad: List) -> Optional[Union[str, int]]:
    """Extract pad number from pad element."""
    if not isinstance(pad, list) or len(pad) < 2:
        return None
    
    pad_num = pad[1]
    # Try to convert to int if it's numeric, otherwise keep as string
    if isinstance(pad_num, (int, float)):
        return int(pad_num)
    elif isinstance(pad_num, str):
        try:
            return int(pad_num)
        except ValueError:
            return pad_num
    return None


def extract_pad_net(pad: List) -> Optional[str]:
    """Extract net from pad element."""
    net_elem = find_element(pad, 'net')
    if net_elem:
        return get_atom_value(net_elem, 1, None)
    return None


def extract_footprint_pads(footprint: List) -> Dict[Union[str, int], str]:
    """Extract all pads from footprint and return pad_number -> net_name mapping."""
    pads = {}
    pad_elements = find_elements(footprint, 'pad')
    
    for pad in pad_elements:
        pad_num = extract_pad_number(pad)
        net_name = extract_pad_net(pad)
        
        if pad_num is not None and net_name is not None:
            pads[pad_num] = net_name
    
    return pads


def extract_polygon_points(polygon: List) -> List[Tuple[float, float]]:
    """Extract points from (polygon (pts (xy x1 y1) (xy x2 y2) ...)) format."""
    points = []
    if not isinstance(polygon, list) or len(polygon) < 2:
        return points
    
    # Find pts element
    pts_elem = find_element(polygon, 'pts')
    if not pts_elem:
        return points
    
    # pts is like: ['pts', ['xy', x1, y1], ['xy', x2, y2], ...]
    for item in pts_elem[1:]:
        if isinstance(item, list) and len(item) >= 3 and item[0] == 'xy':
            try:
                x = float(item[1])
                y = float(item[2])
                points.append((x, y))
            except (ValueError, TypeError, IndexError):
                continue
    
    return points


def extract_layer_list(layers_elem: List) -> List[str]:
    """Extract layer names from (layers "F.Cu" "B.Cu") format."""
    layers = []
    if not isinstance(layers_elem, list):
        return layers
    
    # Skip first element (should be 'layers')
    for item in layers_elem[1:]:
        if isinstance(item, str):
            # Remove quotes if present
            layer_name = item.strip('"\'')
            if layer_name:
                layers.append(layer_name)
    
    return layers


# =============================================================================
# Element Converters
# =============================================================================

def convert_footprint(footprint: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad footprint to footprint statement."""
    if not isinstance(footprint, list) or len(footprint) == 0 or footprint[0] != 'footprint':
        return None
    
    # Extract lib_path (footprint name - first atom after 'footprint')
    lib_path = get_atom_value(footprint, 1, None)
    if not lib_path:
        return None
    
    # Extract properties
    properties = extract_properties(footprint)
    ref = properties.get('Reference', '')
    
    # Extract position and rotation
    at_elem = find_element(footprint, 'at')
    coord = None
    rot = None
    if at_elem:
        coord = extract_coord(at_elem)
        rot = extract_rotation(at_elem)
    
    # Extract layer
    layer_elem = find_element(footprint, 'layer')
    layer_name = get_atom_value(layer_elem, 1, None) if layer_elem else None
    if not layer_name:
        return None
    
    # Extract pads
    pads = extract_footprint_pads(footprint)
    
    # Extract UUID
    uuid_elem = find_element(footprint, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    result = {
        'type': 'footprint',
        'ref': ref,
        'lib_path': lib_path,
        'layer': layer_name,
        'pads': pads,
        'uid': uuid
    }
    
    if coord:
        result['at'] = list(coord)  # Convert tuple to list for JSON
    if rot is not None:
        result['rot'] = rot
    
    return result


def convert_segment(segment: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad segment to segment statement."""
    if not isinstance(segment, list) or len(segment) == 0 or segment[0] != 'segment':
        return None
    
    # Extract start
    start_elem = find_element(segment, 'start')
    start_coord = extract_coord(start_elem) if start_elem else None
    if not start_coord:
        return None
    
    # Extract end
    end_elem = find_element(segment, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    # Extract width
    width_elem = find_element(segment, 'width')
    width = float(get_atom_value(width_elem, 1, 0)) if width_elem else None
    if width is None:
        return None
    
    # Extract layer
    layer_elem = find_element(segment, 'layer')
    layer_name = get_atom_value(layer_elem, 1, None) if layer_elem else None
    if not layer_name:
        return None
    
    # Extract net
    net_elem = find_element(segment, 'net')
    net_name = get_atom_value(net_elem, 1, None) if net_elem else None
    if not net_name:
        return None
    
    # Extract UUID
    uuid_elem = find_element(segment, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    return {
        'type': 'segment',
        'start': list(start_coord),  # Convert tuple to list for JSON
        'end': list(end_coord),
        'width': width,
        'layer': layer_name,
        'net': net_name,
        'uid': uuid
    }


def convert_via(via: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad via to via statement."""
    if not isinstance(via, list) or len(via) == 0 or via[0] != 'via':
        return None
    
    # Extract position
    at_elem = find_element(via, 'at')
    coord = extract_coord(at_elem) if at_elem else None
    if not coord:
        return None
    
    # Extract size
    size_elem = find_element(via, 'size')
    size = float(get_atom_value(size_elem, 1, 0)) if size_elem else None
    if size is None:
        return None
    
    # Extract drill
    drill_elem = find_element(via, 'drill')
    drill = float(get_atom_value(drill_elem, 1, 0)) if drill_elem else None
    if drill is None:
        return None
    
    # Extract layers
    layers_elem = find_element(via, 'layers')
    layers = extract_layer_list(layers_elem) if layers_elem else []
    if not layers:
        return None
    
    # Extract UUID
    uuid_elem = find_element(via, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    result = {
        'type': 'via',
        'at': list(coord),  # Convert tuple to list for JSON
        'size': size,
        'drill': drill,
        'layers': layers,
        'uid': uuid
    }
    
    # Extract net (optional)
    net_elem = find_element(via, 'net')
    if net_elem:
        net_name = get_atom_value(net_elem, 1, None)
        if net_name:
            result['net'] = net_name
    
    return result


def convert_zone(zone: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad zone to zone statement."""
    if not isinstance(zone, list) or len(zone) == 0 or zone[0] != 'zone':
        return None
    
    # Extract net
    net_elem = find_element(zone, 'net')
    net_name = get_atom_value(net_elem, 1, None) if net_elem else None
    if not net_name:
        return None
    
    # Extract layer(s) - support both 'layer' (single) and 'layers' (multiple)
    layers_elem = find_element(zone, 'layers')
    layer_elem = find_element(zone, 'layer')
    
    layers = []
    if layers_elem:
        # Multi-layer zone: extract all layer names from (layers "F.Cu" "B.Cu" ...)
        for i in range(1, len(layers_elem)):
            if isinstance(layers_elem[i], str):
                layers.append(layers_elem[i])
    elif layer_elem:
        # Single-layer zone: extract layer name from (layer "F.Cu")
        layer_name = get_atom_value(layer_elem, 1, None)
        if layer_name:
            layers.append(layer_name)
    
    if not layers:
        return None
    
    # Extract polygon
    polygon_elem = find_element(zone, 'polygon')
    polygon_points = extract_polygon_points(polygon_elem) if polygon_elem else []
    if not polygon_points:
        return None
    
    # Extract UUID
    uuid_elem = find_element(zone, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    return {
        'type': 'zone',
        'net': net_name,
        'layers': layers,  # Always use 'layers' array for consistency
        'polygon': [list(p) for p in polygon_points],  # Convert tuples to lists for JSON
        'uid': uuid
    }


def _extract_stroke_props(elem: List) -> Tuple[Optional[float], Optional[str]]:
    """Extract stroke width and type from an element's stroke sub-element."""
    stroke_elem = find_element(elem, 'stroke')
    stroke_width = None
    stroke_type = None
    if stroke_elem:
        width_elem = find_element(stroke_elem, 'width')
        if width_elem:
            val = get_atom_value(width_elem, 1, None)
            if val is not None:
                stroke_width = float(val)
        type_elem = find_element(stroke_elem, 'type')
        if type_elem:
            stroke_type = get_atom_value(type_elem, 1, None)
    return stroke_width, stroke_type


def _extract_fill(elem: List) -> Optional[str]:
    """Extract fill value (yes/no) from an element."""
    fill_elem = find_element(elem, 'fill')
    if fill_elem:
        val = get_atom_value(fill_elem, 1, None)
        if val is not None:
            return str(val)
    return None


def _extract_layer(elem: List) -> Optional[str]:
    """Extract layer name from an element."""
    layer_elem = find_element(elem, 'layer')
    return get_atom_value(layer_elem, 1, None) if layer_elem else None


def _build_graphic_result(gtype: str, base: Dict[str, Any], elem: List) -> Optional[Dict[str, Any]]:
    """Build a graphic element result dict with common optional fields extracted from sexp."""
    uuid_elem = find_element(elem, 'uuid')
    uid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uid:
        return None
    
    result = {'type': gtype, **base, 'uid': uid}
    
    layer = _extract_layer(elem)
    if layer:
        result['layer'] = layer
    
    stroke_width, stroke_type = _extract_stroke_props(elem)
    if stroke_width is not None:
        result['stroke_width'] = stroke_width
    if stroke_type is not None:
        result['stroke_type'] = stroke_type
    
    fill = _extract_fill(elem)
    if fill is not None:
        result['fill'] = fill
    
    return result


def convert_gr_line(gr_line: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_line to line statement (any layer)."""
    if not isinstance(gr_line, list) or len(gr_line) == 0 or gr_line[0] != 'gr_line':
        return None
    
    start_elem = find_element(gr_line, 'start')
    start_coord = extract_coord(start_elem) if start_elem else None
    if not start_coord:
        return None
    
    end_elem = find_element(gr_line, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    return _build_graphic_result('line', {
        'start': list(start_coord),
        'end': list(end_coord),
    }, gr_line)


def convert_gr_arc(gr_arc: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_arc to arc statement."""
    if not isinstance(gr_arc, list) or len(gr_arc) == 0 or gr_arc[0] != 'gr_arc':
        return None
    
    start_elem = find_element(gr_arc, 'start')
    start_coord = extract_coord(start_elem) if start_elem else None
    if not start_coord:
        return None
    
    mid_elem = find_element(gr_arc, 'mid')
    mid_coord = extract_coord(mid_elem) if mid_elem else None
    if not mid_coord:
        return None
    
    end_elem = find_element(gr_arc, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    return _build_graphic_result('arc', {
        'start': list(start_coord),
        'mid': list(mid_coord),
        'end': list(end_coord),
    }, gr_arc)


def convert_gr_circle(gr_circle: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_circle to circle statement."""
    if not isinstance(gr_circle, list) or len(gr_circle) == 0 or gr_circle[0] != 'gr_circle':
        return None
    
    center_elem = find_element(gr_circle, 'center')
    center_coord = extract_coord(center_elem) if center_elem else None
    if not center_coord:
        return None
    
    end_elem = find_element(gr_circle, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    return _build_graphic_result('circle', {
        'center': list(center_coord),
        'end': list(end_coord),
    }, gr_circle)


def convert_gr_rect(gr_rect: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_rect to rect statement."""
    if not isinstance(gr_rect, list) or len(gr_rect) == 0 or gr_rect[0] != 'gr_rect':
        return None
    
    start_elem = find_element(gr_rect, 'start')
    start_coord = extract_coord(start_elem) if start_elem else None
    if not start_coord:
        return None
    
    end_elem = find_element(gr_rect, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    return _build_graphic_result('rect', {
        'start': list(start_coord),
        'end': list(end_coord),
    }, gr_rect)


def convert_gr_curve(gr_curve: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_curve to curve statement."""
    if not isinstance(gr_curve, list) or len(gr_curve) == 0 or gr_curve[0] != 'gr_curve':
        return None
    
    pts_elem = find_element(gr_curve, 'pts')
    if not pts_elem:
        return None
    
    pts = []
    for item in pts_elem[1:]:
        if isinstance(item, list) and len(item) >= 3 and item[0] == 'xy':
            try:
                pts.append([float(item[1]), float(item[2])])
            except (ValueError, TypeError):
                continue
    
    if len(pts) != 4:
        return None
    
    return _build_graphic_result('curve', {'pts': pts}, gr_curve)


def convert_gr_poly(gr_poly: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_poly to poly statement."""
    if not isinstance(gr_poly, list) or len(gr_poly) == 0 or gr_poly[0] != 'gr_poly':
        return None
    
    pts_elem = find_element(gr_poly, 'pts')
    if not pts_elem:
        return None
    
    pts = []
    for item in pts_elem[1:]:
        if isinstance(item, list) and len(item) >= 3 and item[0] == 'xy':
            try:
                pts.append([float(item[1]), float(item[2])])
            except (ValueError, TypeError):
                continue
    
    if not pts:
        return None
    
    return _build_graphic_result('poly', {'pts': pts}, gr_poly)


def convert_track_arc(arc: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad track arc to track_arc statement."""
    if not isinstance(arc, list) or len(arc) == 0 or arc[0] != 'arc':
        return None
    
    start_elem = find_element(arc, 'start')
    start_coord = extract_coord(start_elem) if start_elem else None
    if not start_coord:
        return None
    
    mid_elem = find_element(arc, 'mid')
    mid_coord = extract_coord(mid_elem) if mid_elem else None
    if not mid_coord:
        return None
    
    end_elem = find_element(arc, 'end')
    end_coord = extract_coord(end_elem) if end_elem else None
    if not end_coord:
        return None
    
    width_elem = find_element(arc, 'width')
    width = float(get_atom_value(width_elem, 1, 0)) if width_elem else None
    if width is None:
        return None
    
    layer_elem = find_element(arc, 'layer')
    layer_name = get_atom_value(layer_elem, 1, None) if layer_elem else None
    if not layer_name:
        return None
    
    net_elem = find_element(arc, 'net')
    net_name = get_atom_value(net_elem, 1, None) if net_elem else None
    if not net_name:
        return None
    
    uuid_elem = find_element(arc, 'uuid')
    uid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uid:
        return None
    
    return {
        'type': 'track_arc',
        'start': list(start_coord),
        'mid': list(mid_coord),
        'end': list(end_coord),
        'width': width,
        'layer': layer_name,
        'net': net_name,
        'uid': uid
    }


def convert_text(gr_text: List) -> Optional[Dict[str, Any]]:
    """Convert KiCad gr_text to text statement."""
    if not isinstance(gr_text, list) or len(gr_text) == 0 or gr_text[0] != 'gr_text':
        return None
    
    # Extract text content (first atom after 'gr_text')
    if len(gr_text) < 2:
        return None
    text_content = get_atom_value(gr_text, 1, None)
    if text_content is None:
        return None
    
    # Extract position (at)
    at_elem = find_element(gr_text, 'at')
    if not at_elem:
        return None
    at_coord = extract_coord(at_elem)
    if not at_coord:
        return None
    
    # Extract rotation (optional, third element of 'at')
    rot = None
    if isinstance(at_elem, list) and len(at_elem) >= 4:
        rot_value = get_atom_value(at_elem, 3, None)
        if rot_value is not None:
            rot = float(rot_value)
    
    # Extract layer
    layer_elem = find_element(gr_text, 'layer')
    layer_name = get_atom_value(layer_elem, 1, None) if layer_elem else None
    if not layer_name:
        return None
    
    # Extract UUID
    uuid_elem = find_element(gr_text, 'uuid')
    uuid = get_atom_value(uuid_elem, 1, None) if uuid_elem else None
    if not uuid:
        return None
    
    # Extract effects (font and justify)
    effects_elem = find_element(gr_text, 'effects')
    font_size = None
    font_thickness = None
    justify = None
    
    if effects_elem:
        # Extract font information
        font_elem = find_element(effects_elem, 'font')
        if font_elem:
            # Extract size (width, height)
            size_elem = find_element(font_elem, 'size')
            if size_elem and len(size_elem) >= 3:
                width = get_atom_value(size_elem, 1, None)
                height = get_atom_value(size_elem, 2, None)
                if width is not None and height is not None:
                    font_size = (float(width), float(height))
            
            # Extract thickness
            thickness_elem = find_element(font_elem, 'thickness')
            if thickness_elem:
                thickness_value = get_atom_value(thickness_elem, 1, None)
                if thickness_value is not None:
                    font_thickness = float(thickness_value)
        
        # Extract justify (horizontal, vertical)
        justify_elem = find_element(effects_elem, 'justify')
        if justify_elem and len(justify_elem) >= 3:
            justify_h = get_atom_value(justify_elem, 1, None)
            justify_v = get_atom_value(justify_elem, 2, None)
            if justify_h and justify_v:
                justify = [justify_h, justify_v]
    
    result = {
        'type': 'text',
        'text': text_content,
        'at': list(at_coord),  # Convert tuple to list for JSON
        'layer': layer_name,
        'uid': uuid
    }
    
    if rot is not None:
        result['rot'] = rot
    if font_size is not None:
        result['font_size'] = list(font_size)  # Convert tuple to list
    if font_thickness is not None:
        result['font_thickness'] = font_thickness
    if justify is not None:
        result['justify'] = justify
    
    return result


# =============================================================================
# Main Extraction Function
# =============================================================================

def extract_trace_elements(sexp_data: List, progress=None) -> List[Dict[str, Any]]:
    """Extract and convert all trace_pcb elements from parsed S-expression."""
    statements = []
    
    if not isinstance(sexp_data, list) or len(sexp_data) == 0:
        return statements
    
    # Find kicad_pcb root
    if sexp_data[0] != 'kicad_pcb':
        return statements
    
    # Note: kicad_ver, kicad_gen, kicad_gen_ver are no longer extracted - they are hardcoded in the converter
    
    # Get all elements from kicad_pcb
    all_elements = sexp_data[1:] if len(sexp_data) > 1 else []
    
    # Separate elements by type
    if progress:
        progress.phase("Categorizing PCB elements")
    footprints = find_elements(all_elements, 'footprint')
    segments = find_elements(all_elements, 'segment')
    track_arcs = find_elements(all_elements, 'arc')
    vias = find_elements(all_elements, 'via')
    zones = find_elements(all_elements, 'zone')
    gr_lines = find_elements(all_elements, 'gr_line')
    gr_arcs = find_elements(all_elements, 'gr_arc')
    gr_circles = find_elements(all_elements, 'gr_circle')
    gr_rects = find_elements(all_elements, 'gr_rect')
    gr_curves = find_elements(all_elements, 'gr_curve')
    gr_polys = find_elements(all_elements, 'gr_poly')
    
    if progress:
        progress.phase(f"Found {len(footprints)} footprints, {len(segments)} segments, {len(vias)} vias")
    
    # Convert footprints
    if progress and footprints:
        progress.phase(f"Converting {len(footprints)} footprints")
    for footprint in footprints:
        fp_stmt = convert_footprint(footprint)
        if fp_stmt:
            statements.append(fp_stmt)
    
    # Convert segments
    if progress and segments:
        progress.phase(f"Converting {len(segments)} track segments")
    for segment in segments:
        seg_stmt = convert_segment(segment)
        if seg_stmt:
            statements.append(seg_stmt)
    
    # Convert track arcs
    for arc in track_arcs:
        arc_stmt = convert_track_arc(arc)
        if arc_stmt:
            statements.append(arc_stmt)
    
    # Convert vias
    for via in vias:
        via_stmt = convert_via(via)
        if via_stmt:
            statements.append(via_stmt)
    
    # Convert zones
    for zone in zones:
        zone_stmt = convert_zone(zone)
        if zone_stmt:
            statements.append(zone_stmt)
    
    # Convert graphical lines (all layers)
    for gr_line in gr_lines:
        line_stmt = convert_gr_line(gr_line)
        if line_stmt:
            statements.append(line_stmt)
    
    # Convert graphical arcs
    for gr_arc in gr_arcs:
        arc_stmt = convert_gr_arc(gr_arc)
        if arc_stmt:
            statements.append(arc_stmt)
    
    # Convert graphical circles
    for gr_circle in gr_circles:
        circle_stmt = convert_gr_circle(gr_circle)
        if circle_stmt:
            statements.append(circle_stmt)
    
    # Convert graphical rectangles
    for gr_rect in gr_rects:
        rect_stmt = convert_gr_rect(gr_rect)
        if rect_stmt:
            statements.append(rect_stmt)
    
    # Convert graphical curves (bezier)
    for gr_curve in gr_curves:
        curve_stmt = convert_gr_curve(gr_curve)
        if curve_stmt:
            statements.append(curve_stmt)
    
    # Convert graphical polygons
    for gr_poly in gr_polys:
        poly_stmt = convert_gr_poly(gr_poly)
        if poly_stmt:
            statements.append(poly_stmt)
    
    # Convert text (gr_text)
    gr_texts = find_elements(all_elements, 'gr_text')
    for gr_text in gr_texts:
        text_stmt = convert_text(gr_text)
        if text_stmt:
            statements.append(text_stmt)
    
    return statements


# =============================================================================
# Main Entry Point
# =============================================================================

def sexp_to_trace_json(content: str) -> List[Dict[str, Any]]:
    """
    Convert KiCad PCB S-expression content to trace_pcb JSON format.
    
    Args:
        content: The .kicad_pcb file content as a string
        
    Returns:
        List of dictionaries, each representing a trace_pcb statement
    """
    # Initialize progress reporter for stall detection
    progress = ProgressReporter()
    progress.phase("Starting pcbnew sexp-to-json conversion")
    
    # Parse S-expression
    progress.phase("Parsing S-expression")
    sexp_data = parse_sexp(content)
    
    # Extract and convert to trace_pcb format
    progress.phase("Extracting trace elements")
    statements = extract_trace_elements(sexp_data, progress)
    
    progress.phase("Conversion complete")
    return statements


# =============================================================================
# Testing
# =============================================================================

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) != 2:
        print("Usage: python sexp_to_trace_json.py <kicad_pcb_file>")
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
