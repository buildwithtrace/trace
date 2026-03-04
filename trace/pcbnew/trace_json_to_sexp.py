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
TRACE-JSON to KiCad PCB S-Expression Converter

Converts trace_pcb JSON format back to KiCad .kicad_pcb S-expression files.
Merges trace_pcb data with existing kicad_pcb file, updating elements by UUID
and generating new ones from footprint libraries and schematic data.
"""

import json
import os
import uuid
import copy
from typing import List, Dict, Any, Optional, Tuple, Union, Set
import logging
import sys

# Industry-standard console-only logging for CLI tools
# Robust, zero permission issues, cross-platform compatible
try:
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        stream=sys.stderr,  # Log to stderr (standard for CLI tools)
        force=True  # Override any existing config
    )
except Exception as e:
    # Ultra-robust fallback - if even stderr logging fails, continue silently
    # (though this should never happen in normal circumstances)
    pass

logger = logging.getLogger(__name__)

# Import common S-expression parsing utilities
import sys
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


# =============================================================================
# S-Expression Formatter
# =============================================================================

# Fields whose values should be quoted in S-expressions
QUOTED_VALUE_FIELDS = {'name', 'number', 'property', 'symbol', 'uuid', 'label', 'path', 'generator', 'paper', 'lib_id', 'lib_name', 'project', 'reference', 'page', 'layer', 'net', 'lib_path', 'sheetname', 'sheetfile', 'generator_version', 'material', 'copper_finish', 'outputdirectory', 'pins'}


def escape_string(value: str) -> str:
    """
    Clean a string for S-expression formatting.
    
    Removes any quotes from inside the string. Only the outer delimiters
    should be quotes - no quotes should appear in the content itself.
    
    Args:
        value: String to clean
    
    Returns:
        Cleaned string with internal quotes removed
    """
    if not isinstance(value, str):
        return value
    cleaned = value.replace('\\"', '').replace('"', '').replace('\\', '')
    return cleaned


def format_sexp_value(value: Any, field_name: str = '', parent_field: str = '', indent: int = 0) -> str:
    """
    Format a value as an S-expression atom or structure.
    
    Args:
        value: The value to format
        field_name: Name of the field (for quoting rules)
        parent_field: Parent field name (for context)
        indent: Current indentation level
    
    Returns:
        Formatted S-expression string
    """
    if isinstance(value, list):
        # Format as list
        if len(value) == 0:
            return '()'
        
        # Check if first element is a field name that determines quoting
        first_elem = value[0] if len(value) > 0 else ''
        
        indent_str = '\t' * indent
        next_indent_str = '\t' * (indent + 1)
        
        # Separate atoms (simple values) from nested lists
        atoms = []  # Simple values that go on the same line as keyword
        nested_lists = []  # Nested lists that go on separate lines
        
        for i, item in enumerate(value):
            if i == 0:
                # First element is the keyword - never quote it
                atoms.append(str(item))
            elif isinstance(item, list):
                # Nested list - format it and add to nested_lists
                formatted_nested = format_sexp_value(item, str(item[0]) if len(item) > 0 and isinstance(item[0], str) else '', first_elem, indent + 1)
                nested_lists.append(formatted_nested)
            else:
                # Atom value - format and add to atoms
                # Handle None values for quoted fields
                if item is None and i == 1 and first_elem in QUOTED_VALUE_FIELDS:
                    atoms.append('""')
                elif i == 1 and first_elem == 'type':
                    # Context-aware quoting for 'type' field
                    # Quote in stackup context, don't quote in stroke context
                    if parent_field == 'stackup' or parent_field == 'layer':
                        # In stackup or stackup layer, quote type values (e.g., "copper", "Top Silk Screen")
                        atoms.append(f'"{escape_string(str(item))}"')
                    elif parent_field == 'stroke':
                        # In stroke context, don't quote type values (e.g., solid, default)
                        atoms.append(str(item))
                    else:
                        # For other contexts, quote if contains spaces or special characters
                        if isinstance(item, str) and (' ' in item or '\n' in item or '\t' in item or '*' in item or '?' in item or '"' in item):
                            atoms.append(f'"{escape_string(str(item))}"')
                        else:
                            atoms.append(str(item))
                elif i == 1 and first_elem in QUOTED_VALUE_FIELDS:
                    # Value immediately after a quoted field keyword should be quoted
                    atoms.append(f'"{escape_string(str(item))}"')
                elif i == 2 and first_elem == 'property':
                    # Second value after 'property' (the property value) should also be quoted
                    atoms.append(f'"{escape_string(str(item))}"')
                elif parent_field == 'layers' and isinstance(item, str):
                    # In layers section, quote layer name (index 1) and description (index 3), but not type (index 2)
                    # Layer entries are like: (0 "F.Cu" signal) or (9 "F.Adhes" user "F.Adhesive")
                    # Index 0: number (never quoted)
                    # Index 1: layer name (always quoted)
                    # Index 2: type like "signal" or "user" (never quoted)
                    # Index 3: optional description (always quoted if present)
                    if i == 1 or i == 3:
                        atoms.append(f'"{escape_string(str(item))}"')
                    else:
                        atoms.append(str(item))
                elif isinstance(item, str) and item == '':
                    # Always quote empty strings
                    atoms.append('""')
                else:
                    # Format as atom
                    formatted_atom = format_sexp_value(item, str(item) if isinstance(item, str) else '', first_elem, indent)
                    atoms.append(formatted_atom)
        
        # Build the formatted string
        # First line: (keyword atom1 atom2 ...)
        # If the first element is a quoted field and we only have the keyword, add empty quoted string
        if len(atoms) == 1 and first_elem in QUOTED_VALUE_FIELDS:
            atoms.append('""')
        first_line = '(' + ' '.join(atoms)
        
        # If there are nested lists, we need multi-line format
        if nested_lists:
            lines = [first_line]
            for nested in nested_lists:
                # Each nested list goes on its own line with proper indentation
                if '\n' in nested:
                    # Multi-line nested list
                    nested_lines = nested.split('\n')
                    base_indent_tabs = indent + 1
                    
                    for nested_line in nested_lines:
                        if nested_line.strip():  # Skip empty lines
                            # Count existing tabs
                            existing_tabs = len(nested_line) - len(nested_line.lstrip())
                            # Calculate relative indent (beyond the base)
                            relative_tabs = max(0, existing_tabs - base_indent_tabs)
                            # Add our base indent plus the relative indent
                            lines.append(next_indent_str + '\t' * relative_tabs + nested_line.lstrip())
                else:
                    # Single-line nested list
                    lines.append(next_indent_str + nested)
            lines.append(indent_str + ')')
            return '\n'.join(lines)
        else:
            # All atoms - single line
            return first_line + ')'
    
    elif isinstance(value, (int, float)):
        # Format number
        if isinstance(value, float):
            # Format with appropriate precision
            if value.is_integer():
                return str(int(value))
            return f'{value:.10g}'.rstrip('0').rstrip('.')
        return str(value)
    
    elif isinstance(value, str):
        # Format string - quote if parent field requires quoting or contains spaces/special chars
        if parent_field in QUOTED_VALUE_FIELDS:
            return f'"{escape_string(value)}"'
        # Always quote empty strings
        if value == '':
            return '""'
        # Quote if contains spaces or special characters
        if ' ' in value or '\n' in value or '\t' in value or '*' in value or '?' in value or '"' in value:
            return f'"{escape_string(value)}"'
        return value
    
    elif isinstance(value, bool):
        return 'yes' if value else 'no'
    
    elif value is None:
        # Return empty quoted string if parent field requires quoting
        if parent_field in QUOTED_VALUE_FIELDS:
            return '""'
        return ''
    
    else:
        return str(value)


def format_sexp(data: Any, indent: int = 0) -> str:
    """
    Format Python data structure as S-expression string.
    
    Args:
        data: Python data structure (list, dict, etc.)
        indent: Current indentation level
    
    Returns:
        Formatted S-expression string
    """
    if isinstance(data, list):
        if len(data) == 0:
            return '()'
        
        # Use format_sexp_value which handles the proper formatting
        return format_sexp_value(data, '', '', indent)
    
    elif isinstance(data, dict):
        # Convert dict to list format: (key value ...)
        items = []
        for key, value in data.items():
            items.append(key)
            items.append(value)
        return format_sexp(items, indent)
    
    else:
        return format_sexp_value(data, '', '', indent)


# =============================================================================
# Footprint Library Loader
# =============================================================================

KICAD_FOOTPRINT_PATH = ""

# Cache for parsed footprint files (lib_path -> parsed_footprint_data)
_footprint_cache = {}


def load_footprint_from_library(lib_path: str, footprint_paths: Union[str, List[str]] = None) -> Optional[List]:
    """
    Load footprint definition from KiCad library file.
    Uses caching to avoid re-parsing the same footprint file multiple times.
    Searches across multiple footprint directories if provided.
    
    Args:
        lib_path: Footprint library path in format "LibraryName:FootprintName" (e.g., "Package_TO_SOT_SMD:SOT-23-5")
        footprint_paths: Path(s) to KiCad footprints directory(ies). Can be a single string or list of strings.
                        If None, uses default KICAD_FOOTPRINT_PATH or environment variable.
    
    Returns:
        Footprint definition as S-expression list, or None if not found
    """
    if ':' not in lib_path:
        return None
    
    # Normalize footprint_paths to a list
    if footprint_paths is None:
        # Check environment variable first
        env_paths = os.environ.get('KICAD_FOOTPRINT_DIR', '')
        if env_paths:
            # Handle both colon and semicolon separators
            search_paths = [p.strip() for p in env_paths.replace(';', ':').split(':') if p.strip()]
        else:
            search_paths = [KICAD_FOOTPRINT_PATH]
    elif isinstance(footprint_paths, str):
        search_paths = [footprint_paths]
    else:
        search_paths = footprint_paths
    
    library_name, footprint_name = lib_path.split(':', 1)
    
    # Search across all provided paths
    for footprint_path in search_paths:
        if not footprint_path or not os.path.isdir(footprint_path):
            continue
        
        # Footprint files are in subdirectories: footprints/LibraryName.pretty/FootprintName.kicad_mod
        footprint_file = os.path.join(footprint_path, f'{library_name}.pretty', f'{footprint_name}.kicad_mod')
        
        if os.path.exists(footprint_file):
            break
    else:
        # No footprint found in any path
        logger.warning(f"Warning: Footprint file not found in any search path: {lib_path}")
        return None
    
    try:
        # Check cache first
        if lib_path not in _footprint_cache:
            with open(footprint_file, 'r') as f:
                content = f.read()
            
            # Parse the footprint file
            footprint_data = parse_sexp(content)
            
            if not isinstance(footprint_data, list) or len(footprint_data) == 0 or footprint_data[0] != 'footprint':
                return None
            
            # Cache the parsed footprint
            _footprint_cache[lib_path] = footprint_data
        
        # Return cached footprint
        return _footprint_cache[lib_path]
    
    except Exception as e:
        logger.error(f"Error loading footprint from {footprint_file}: {e}")
        return None


# =============================================================================
# Schematic Symbol Mapper
# =============================================================================

def parse_schematic_symbols(kicad_sch_content: str) -> Dict[str, Dict[str, Any]]:
    """
    Parse kicad_sch file and build symbol lookup by Reference.
    
    Args:
        kicad_sch_content: Content of kicad_sch file as string
    
    Returns:
        Dictionary mapping Reference -> symbol info (path, sheetname, sheetfile, properties)
    """
    symbol_map = {}
    
    try:
        sch_data = parse_sexp(kicad_sch_content)
        
        if not isinstance(sch_data, list) or len(sch_data) == 0 or sch_data[0] != 'kicad_sch':
            return symbol_map
        
        # Find all symbol instances
        symbols = find_elements(sch_data, 'symbol')
        
        for symbol in symbols:
            # Extract Reference property
            properties = {}
            property_elements = find_elements(symbol, 'property')
            
            for prop in property_elements:
                if len(prop) >= 3:
                    prop_name = get_atom_value(prop, 1, '')
                    prop_value = get_atom_value(prop, 2, '')
                    if prop_name:
                        properties[prop_name] = prop_value
            
            ref = properties.get('Reference', '')
            if not ref:
                continue
            
            # Extract path, sheetname, sheetfile from instances
            instances_elem = find_element(symbol, 'instances')
            path = ''
            sheetname = ''
            sheetfile = ''
            
            if instances_elem:
                project_elem = find_element(instances_elem, 'project')
                if project_elem:
                    path_elem = find_element(project_elem, 'path')
                    if path_elem:
                        path = get_atom_value(path_elem, 1, '')
                    
                    sheetname_elem = find_element(project_elem, 'sheetname')
                    if sheetname_elem:
                        sheetname = get_atom_value(sheetname_elem, 1, '')
                    
                    sheetfile_elem = find_element(project_elem, 'sheetfile')
                    if sheetfile_elem:
                        sheetfile = get_atom_value(sheetfile_elem, 1, '')
            
            symbol_map[ref] = {
                'path': path,
                'sheetname': sheetname,
                'sheetfile': sheetfile,
                'properties': properties
            }
    
    except Exception as e:
        logger.error(f"Error parsing schematic: {e}")
    
    return symbol_map


# =============================================================================
# Element Mergers (Update Existing Elements)
# =============================================================================

def merge_footprint(existing_fp: List, trace_fp: Dict[str, Any]) -> List:
    """
    Merge trace_pcb footprint data into existing footprint.
    Updates only changed fields, preserves all other elements.
    
    Args:
        existing_fp: Existing footprint as S-expression list
        trace_fp: Footprint data from trace_pcb JSON
    
    Returns:
        Updated footprint as S-expression list
    """
    # Deep copy the footprint
    result = copy.deepcopy(existing_fp)
    
    # Update position (at)
    if 'at' in trace_fp:
        at_elem = find_element(result, 'at')
        if at_elem:
            # Update existing at element
            at_idx = result.index(at_elem)
            rot = trace_fp.get('rot', extract_rotation(at_elem) if at_elem else 0)
            result[at_idx] = ['at', trace_fp['at'][0], trace_fp['at'][1], rot]
        else:
            # Insert new at element after layer
            layer_idx = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'layer':
                    layer_idx = i
                    break
            if layer_idx is not None:
                rot = trace_fp.get('rot', 0)
                result.insert(layer_idx + 1, ['at', trace_fp['at'][0], trace_fp['at'][1], rot])
    
    # Update rotation if provided separately
    if 'rot' in trace_fp:
        at_elem = find_element(result, 'at')
        if at_elem and len(at_elem) >= 4:
            at_idx = result.index(at_elem)
            result[at_idx][3] = trace_fp['rot']
    
    # Update layer
    if 'layer' in trace_fp:
        layer_elem = find_element(result, 'layer')
        if layer_elem:
            layer_idx = result.index(layer_elem)
            result[layer_idx] = ['layer', trace_fp['layer']]
    
    # Update pad nets
    if 'pads' in trace_fp:
        pads_dict = trace_fp['pads']
        pad_elements = find_elements(result, 'pad')
        
        for pad in pad_elements:
            pad_num = get_atom_value(pad, 1, None)
            if pad_num is None:
                continue
            
            # Convert pad_num to string for comparison
            pad_num_str = str(pad_num)
            if pad_num_str in pads_dict:
                net_name = pads_dict[pad_num_str]
                # Update or add net element
                net_elem = find_element(pad, 'net')
                if net_elem:
                    net_idx = pad.index(net_elem)
                    pad[net_idx] = ['net', net_name]
                else:
                    # Insert net element before uuid
                    uuid_idx = None
                    for i, item in enumerate(pad):
                        if isinstance(item, list) and len(item) > 0 and item[0] == 'uuid':
                            uuid_idx = i
                            break
                    if uuid_idx is not None:
                        pad.insert(uuid_idx, ['net', net_name])
                    else:
                        pad.append(['net', net_name])
    
    return result


def merge_segment(existing_seg: List, trace_seg: Dict[str, Any]) -> List:
    """Merge trace_pcb segment data into existing segment."""
    result = copy.deepcopy(existing_seg)
    
    # Update start
    if 'start' in trace_seg:
        start_elem = find_element(result, 'start')
        if start_elem:
            start_idx = result.index(start_elem)
            result[start_idx] = ['start', trace_seg['start'][0], trace_seg['start'][1]]
    
    # Update end
    if 'end' in trace_seg:
        end_elem = find_element(result, 'end')
        if end_elem:
            end_idx = result.index(end_elem)
            result[end_idx] = ['end', trace_seg['end'][0], trace_seg['end'][1]]
    
    # Update width
    if 'width' in trace_seg:
        width_elem = find_element(result, 'width')
        if width_elem:
            width_idx = result.index(width_elem)
            result[width_idx] = ['width', trace_seg['width']]
    
    # Update layer
    if 'layer' in trace_seg:
        layer_elem = find_element(result, 'layer')
        if layer_elem:
            layer_idx = result.index(layer_elem)
            result[layer_idx] = ['layer', trace_seg['layer']]
    
    # Update net
    if 'net' in trace_seg:
        net_elem = find_element(result, 'net')
        if net_elem:
            net_idx = result.index(net_elem)
            result[net_idx] = ['net', trace_seg['net']]
    
    return result


def merge_via(existing_via: List, trace_via: Dict[str, Any]) -> List:
    """Merge trace_pcb via data into existing via."""
    result = copy.deepcopy(existing_via)
    
    # Update at
    if 'at' in trace_via:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            result[at_idx] = ['at', trace_via['at'][0], trace_via['at'][1]]
    
    # Update size
    if 'size' in trace_via:
        size_elem = find_element(result, 'size')
        if size_elem:
            size_idx = result.index(size_elem)
            result[size_idx] = ['size', trace_via['size']]
    
    # Update drill
    if 'drill' in trace_via:
        drill_elem = find_element(result, 'drill')
        if drill_elem:
            drill_idx = result.index(drill_elem)
            result[drill_idx] = ['drill', trace_via['drill']]
    
    # Update layers
    if 'layers' in trace_via:
        layers_elem = find_element(result, 'layers')
        if layers_elem:
            layers_idx = result.index(layers_elem)
            layers_list = ['layers'] + trace_via['layers']
            result[layers_idx] = layers_list
    
    # Update net
    if 'net' in trace_via:
        net_elem = find_element(result, 'net')
        if net_elem:
            net_idx = result.index(net_elem)
            result[net_idx] = ['net', trace_via['net']]
        else:
            # Insert net before uuid
            uuid_idx = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'uuid':
                    uuid_idx = i
                    break
            if uuid_idx is not None:
                result.insert(uuid_idx, ['net', trace_via['net']])
    
    return result


def merge_zone(existing_zone: List, trace_zone: Dict[str, Any]) -> List:
    """Merge trace_pcb zone data into existing zone."""
    result = copy.deepcopy(existing_zone)
    
    # Update net
    if 'net' in trace_zone:
        net_elem = find_element(result, 'net')
        if net_elem:
            net_idx = result.index(net_elem)
            result[net_idx] = ['net', trace_zone['net']]
    
    # Update layer(s) - support both 'layers' (array) and 'layer' (single)
    layers = trace_zone.get('layers', [])
    layer = trace_zone.get('layer')
    
    if layers or layer:
        # Find existing layer or layers element
        existing_layer_elem = find_element(result, 'layer')
        existing_layers_elem = find_element(result, 'layers')
        
        # Determine the new layer element
        if layers and len(layers) > 1:
            # Multi-layer zone: use 'layers' keyword
            new_layer_elem = ['layers'] + layers
        elif layers and len(layers) == 1:
            # Single layer in 'layers' array
            new_layer_elem = ['layer', layers[0]]
        elif layer:
            # Backward compatibility: single 'layer' key
            new_layer_elem = ['layer', layer]
        else:
            new_layer_elem = None
        
        if new_layer_elem:
            # Replace existing layer/layers element
            if existing_layers_elem:
                layers_idx = result.index(existing_layers_elem)
                result[layers_idx] = new_layer_elem
            elif existing_layer_elem:
                layer_idx = result.index(existing_layer_elem)
                result[layer_idx] = new_layer_elem
    
    # Update polygon
    if 'polygon' in trace_zone:
        polygon_elem = find_element(result, 'polygon')
        if polygon_elem:
            polygon_idx = result.index(polygon_elem)
            # Build polygon structure: (polygon (pts (xy x1 y1) (xy x2 y2) ...))
            pts = ['pts']
            for point in trace_zone['polygon']:
                pts.append(['xy', point[0], point[1]])
            result[polygon_idx] = ['polygon', pts]
    
    return result


def merge_edge(existing_edge: List, trace_edge: Dict[str, Any]) -> List:
    """Merge trace_pcb edge data into existing gr_line."""
    # Only merge if it's on Edge.Cuts layer
    layer_elem = find_element(existing_edge, 'layer')
    if layer_elem:
        layer_name = get_atom_value(layer_elem, 1, None)
        if layer_name != 'Edge.Cuts':
            return copy.deepcopy(existing_edge)
    
    result = copy.deepcopy(existing_edge)
    
    # Update start
    if 'points' in trace_edge and len(trace_edge['points']) > 0:
        start_elem = find_element(result, 'start')
        if start_elem:
            start_idx = result.index(start_elem)
            result[start_idx] = ['start', trace_edge['points'][0][0], trace_edge['points'][0][1]]
    
    # Update end
    if 'points' in trace_edge and len(trace_edge['points']) > 1:
        end_elem = find_element(result, 'end')
        if end_elem:
            end_idx = result.index(end_elem)
            result[end_idx] = ['end', trace_edge['points'][-1][0], trace_edge['points'][-1][1]]
    
    return result


def merge_text(existing_text: List, trace_text: Dict[str, Any]) -> List:
    """Merge trace_pcb text data into existing gr_text."""
    result = copy.deepcopy(existing_text)
    
    # Update text content (second element)
    if 'text' in trace_text and len(result) >= 2:
        result[1] = trace_text['text']
    
    # Update position (at)
    if 'at' in trace_text:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            at_coord = trace_text['at']
            rot = trace_text.get('rot', 0)
            if rot != 0:
                result[at_idx] = ['at', at_coord[0], at_coord[1], rot]
            else:
                result[at_idx] = ['at', at_coord[0], at_coord[1]]
        else:
            # Insert at element after text content
            at_coord = trace_text['at']
            rot = trace_text.get('rot', 0)
            at_elem = ['at', at_coord[0], at_coord[1]]
            if rot != 0:
                at_elem.append(rot)
            result.insert(2, at_elem)
    
    # Update layer
    if 'layer' in trace_text:
        layer_elem = find_element(result, 'layer')
        if layer_elem:
            layer_idx = result.index(layer_elem)
            result[layer_idx] = ['layer', trace_text['layer']]
    
    # Update effects (font and justify)
    if 'font_size' in trace_text or 'font_thickness' in trace_text or 'justify' in trace_text:
        effects_elem = find_element(result, 'effects')
        if effects_elem:
            effects_idx = result.index(effects_elem)
            
            # Rebuild effects
            effects_parts = []
            
            # Font information
            font_parts = []
            font_size = trace_text.get('font_size')
            if font_size:
                font_parts.append(['size', font_size[0], font_size[1]])
            else:
                # Try to get from existing
                font_elem = find_element(effects_elem, 'font')
                if font_elem:
                    size_elem = find_element(font_elem, 'size')
                    if size_elem and len(size_elem) >= 3:
                        font_parts.append(['size', size_elem[1], size_elem[2]])
                    else:
                        font_parts.append(['size', 1, 1])
                else:
                    font_parts.append(['size', 1, 1])
            
            font_thickness = trace_text.get('font_thickness')
            if font_thickness is not None:
                font_parts.append(['thickness', font_thickness])
            else:
                # Try to get from existing
                font_elem = find_element(effects_elem, 'font')
                if font_elem:
                    thickness_elem = find_element(font_elem, 'thickness')
                    if thickness_elem:
                        font_parts.append(['thickness', thickness_elem[1]])
                    else:
                        font_parts.append(['thickness', 0.1])
                else:
                    font_parts.append(['thickness', 0.1])
            
            effects_parts.append(['font'] + font_parts)
            
            # Justify
            justify = trace_text.get('justify')
            if justify and isinstance(justify, list) and len(justify) >= 2:
                effects_parts.append(['justify', justify[0], justify[1]])
            else:
                # Try to get from existing
                justify_elem = find_element(effects_elem, 'justify')
                if justify_elem and len(justify_elem) >= 3:
                    effects_parts.append(['justify', justify_elem[1], justify_elem[2]])
                else:
                    effects_parts.append(['justify', 'left', 'bottom'])
            
            result[effects_idx] = ['effects'] + effects_parts
    
    return result


# =============================================================================
# Element Generators (Create New Elements)
# =============================================================================

def generate_footprint(trace_fp: Dict[str, Any], symbol_info: Optional[Dict[str, Any]] = None, footprint_paths: Union[str, List[str]] = None) -> Optional[List]:
    """
    Generate a new footprint from trace_pcb data and footprint library.
    
    Args:
        trace_fp: Footprint data from trace_pcb JSON
        symbol_info: Optional symbol info from schematic (path, sheetname, sheetfile, properties)
        footprint_paths: Path(s) to KiCad footprints directory(ies). Can be a single string or list of strings.
    
    Returns:
        Footprint as S-expression list, or None if generation fails
    """
    lib_path = trace_fp.get('lib_path')
    if not lib_path:
        return None
    
    # Load footprint definition from library
    footprint_def = load_footprint_from_library(lib_path, footprint_paths)
    if not footprint_def:
        logger.warning(f"Warning: Could not load footprint {lib_path}, creating minimal footprint")
        # Create minimal footprint structure
        footprint_def = ['footprint', lib_path]
    
    # Create footprint instance
    result = ['footprint', lib_path]
    
    # Add layer
    layer = trace_fp.get('layer', 'F.Cu')
    result.append(['layer', layer])
    
    # Add position
    at = trace_fp.get('at', [0, 0])
    rot = trace_fp.get('rot', 0)
    result.append(['at', at[0], at[1], rot])
    
    # Add UUID
    fp_uid = trace_fp.get('uid')
    if not fp_uid:
        fp_uid = str(uuid.uuid4())
    result.append(['uuid', fp_uid])
    
    # Copy elements from footprint definition (skip 'footprint' and name)
    if len(footprint_def) > 2:
        for elem in footprint_def[2:]:
            # Skip certain elements that will be set from trace_pcb or symbol
            if isinstance(elem, list) and len(elem) > 0:
                elem_type = elem[0]
                if elem_type in ('at', 'layer', 'uuid', 'path', 'sheetname', 'sheetfile'):
                    continue
                # Skip properties that will be set from symbol
                if elem_type == 'property' and len(elem) > 1:
                    prop_name = get_atom_value(elem, 1, '')
                    if prop_name in ('Reference', 'Value', 'Footprint', 'Datasheet', 'Description'):
                        continue
            result.append(elem)
    
    # Add properties from symbol or trace_pcb
    properties = {}
    if symbol_info and 'properties' in symbol_info:
        properties = symbol_info['properties'].copy()
    
    # Override with Reference from trace_pcb if available
    ref = trace_fp.get('ref', properties.get('Reference', ''))
    if ref:
        properties['Reference'] = ref
    
    # Add properties
    for prop_name, prop_value in properties.items():
        if prop_name in ('Reference', 'Value', 'Footprint', 'Datasheet', 'Description'):
            # Find position for property (after uuid, before other elements)
            insert_idx = len(result)
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'uuid':
                    insert_idx = i + 1
                    break
            
            # Create property element with default formatting
            prop_elem = ['property', prop_name, prop_value]
            # Add default property attributes
            prop_elem.append(['at', at[0], at[1], 0])
            prop_elem.append(['layer', 'F.SilkS' if prop_name == 'Reference' else 'F.Fab'])
            prop_elem.append(['hide', 'yes' if prop_name in ('Footprint', 'Datasheet', 'Description') else 'no'])
            prop_elem.append(['uuid', str(uuid.uuid4())])
            prop_elem.append(['effects', ['font', ['size', 1.27, 1.27]]])
            
            result.insert(insert_idx, prop_elem)
    
    # Add path, sheetname, sheetfile from symbol
    if symbol_info:
        if symbol_info.get('path'):
            result.append(['path', symbol_info['path']])
        if symbol_info.get('sheetname'):
            result.append(['sheetname', symbol_info['sheetname']])
        if symbol_info.get('sheetfile'):
            result.append(['sheetfile', symbol_info['sheetfile']])
    
    # Update pad nets from trace_pcb
    if 'pads' in trace_fp:
        pads_dict = trace_fp['pads']
        pad_elements = find_elements(result, 'pad')
        
        for pad in pad_elements:
            pad_num = get_atom_value(pad, 1, None)
            if pad_num is None:
                continue
            
            pad_num_str = str(pad_num)
            if pad_num_str in pads_dict:
                net_name = pads_dict[pad_num_str]
                # Update or add net element
                net_elem = find_element(pad, 'net')
                if net_elem:
                    net_idx = pad.index(net_elem)
                    pad[net_idx] = ['net', net_name]
                else:
                    # Insert net element before uuid
                    uuid_idx = None
                    for i, item in enumerate(pad):
                        if isinstance(item, list) and len(item) > 0 and item[0] == 'uuid':
                            uuid_idx = i
                            break
                    if uuid_idx is not None:
                        pad.insert(uuid_idx, ['net', net_name])
                    else:
                        pad.append(['net', net_name])
    
    return result


def generate_segment(trace_seg: Dict[str, Any]) -> Optional[List]:
    """Generate a new segment from trace_pcb data."""
    if 'start' not in trace_seg or 'end' not in trace_seg:
        return None
    
    seg_uid = trace_seg.get('uid')
    if not seg_uid:
        seg_uid = str(uuid.uuid4())
    
    result = ['segment',
              ['start', trace_seg['start'][0], trace_seg['start'][1]],
              ['end', trace_seg['end'][0], trace_seg['end'][1]],
              ['width', trace_seg.get('width', 0.2)],
              ['layer', trace_seg.get('layer', 'F.Cu')],
              ['net', trace_seg.get('net', '')],
              ['uuid', seg_uid]]
    
    return result


def generate_via(trace_via: Dict[str, Any]) -> Optional[List]:
    """Generate a new via from trace_pcb data."""
    if 'at' not in trace_via or 'size' not in trace_via or 'drill' not in trace_via:
        return None
    
    via_uid = trace_via.get('uid')
    if not via_uid:
        via_uid = str(uuid.uuid4())
    
    result = ['via',
              ['at', trace_via['at'][0], trace_via['at'][1]],
              ['size', trace_via['size']],
              ['drill', trace_via['drill']],
              ['layers'] + trace_via.get('layers', ['F.Cu', 'B.Cu']),
              ['uuid', via_uid]]
    
    # Add optional net
    if 'net' in trace_via:
        result.insert(-1, ['net', trace_via['net']])
    
    # Add default via properties
    result.insert(-1, ['free', 'yes'])
    result.insert(-1, ['tenting', ['front', 'none'], ['back', 'none']])
    result.insert(-1, ['capping', 'none'])
    result.insert(-1, ['covering', ['front', 'none'], ['back', 'none']])
    result.insert(-1, ['plugging', ['front', 'none'], ['back', 'none']])
    result.insert(-1, ['filling', 'none'])
    
    return result


def generate_zone(trace_zone: Dict[str, Any]) -> Optional[List]:
    """Generate a new zone from trace_pcb data."""
    if 'polygon' not in trace_zone or not trace_zone['polygon']:
        return None
    
    zone_uid = trace_zone.get('uid')
    if not zone_uid:
        zone_uid = str(uuid.uuid4())
    
    # Build polygon structure
    pts = ['pts']
    for point in trace_zone['polygon']:
        pts.append(['xy', point[0], point[1]])
    
    # Handle layers - support both 'layers' (array) and 'layer' (single, for backward compatibility)
    layers = trace_zone.get('layers', [])
    layer = trace_zone.get('layer')
    
    if layers and len(layers) > 1:
        # Multi-layer zone: use 'layers' keyword
        layer_elem = ['layers'] + layers
    elif layers and len(layers) == 1:
        # Single layer in 'layers' array
        layer_elem = ['layer', layers[0]]
    elif layer:
        # Backward compatibility: single 'layer' key
        layer_elem = ['layer', layer]
    else:
        # Default to F.Cu
        layer_elem = ['layer', 'F.Cu']
    
    result = ['zone',
              ['net', trace_zone.get('net', '')],
              layer_elem,
              ['uuid', zone_uid],
              ['hatch', 'edge', 0.5],
              ['connect_pads', ['clearance', 0.5]],
              ['min_thickness', 0.25],
              ['fill', 'yes',
               ['thermal_gap', 0.5],
               ['thermal_bridge_width', 0.5],
               ['island_removal_mode', 0]],
              ['polygon', pts]]
    
    return result


def generate_edge(trace_edge: Dict[str, Any]) -> Optional[List]:
    """Generate a new gr_line (edge) from trace_pcb data."""
    if 'points' not in trace_edge or len(trace_edge['points']) < 2:
        return None
    
    edge_uid = trace_edge.get('uid')
    if not edge_uid:
        edge_uid = str(uuid.uuid4())
    
    # For now, handle only two-point edges (start -> end)
    # Multi-point edges would need to be split into multiple gr_lines
    points = trace_edge['points']
    
    result = ['gr_line',
              ['start', points[0][0], points[0][1]],
              ['end', points[-1][0], points[-1][1]],
              ['stroke', ['width', 0.05], ['type', 'default']],
              ['layer', 'Edge.Cuts'],
              ['uuid', edge_uid]]
    
    return result


def generate_text(trace_text: Dict[str, Any]) -> Optional[List]:
    """Generate a new gr_text from trace_pcb data."""
    text_content = trace_text.get('text')
    if not text_content:
        return None
    
    text_uid = trace_text.get('uid')
    if not text_uid:
        text_uid = str(uuid.uuid4())
    
    at_coord = trace_text.get('at', [0, 0])
    layer = trace_text.get('layer', 'F.SilkS')
    rot = trace_text.get('rot', 0)
    
    # Build at element with rotation
    at_elem = ['at', at_coord[0], at_coord[1]]
    if rot != 0:
        at_elem.append(rot)
    
    result = ['gr_text', text_content, at_elem, ['layer', layer]]
    
    # Build effects element
    effects_parts = []
    
    # Font information
    font_parts = []
    font_size = trace_text.get('font_size')
    if font_size:
        font_parts.append(['size', font_size[0], font_size[1]])
    else:
        font_parts.append(['size', 1, 1])
    
    font_thickness = trace_text.get('font_thickness')
    if font_thickness is not None:
        font_parts.append(['thickness', font_thickness])
    else:
        font_parts.append(['thickness', 0.1])
    
    effects_parts.append(['font'] + font_parts)
    
    # Justify
    justify = trace_text.get('justify')
    if justify and isinstance(justify, list) and len(justify) >= 2:
        effects_parts.append(['justify', justify[0], justify[1]])
    else:
        effects_parts.append(['justify', 'left', 'bottom'])
    
    result.append(['effects'] + effects_parts)
    result.append(['uuid', text_uid])
    
    return result


# =============================================================================
# Main Conversion Function
# =============================================================================

def trace_json_to_sexp(trace_json: List[Dict[str, Any]],
                       existing_pcb_content: str,
                       kicad_sch_content: str,
                       footprint_paths: Union[str, List[str]] = None) -> str:
    """
    Convert trace JSON format to KiCad PCB S-expression format.
    
    Args:
        trace_json: List of trace JSON statements
        existing_pcb_content: Content of existing kicad_pcb file
        kicad_sch_content: Content of corresponding kicad_sch file
        footprint_paths: Path(s) to KiCad footprints directory(ies). Can be a single string or list of strings.
                        If None, uses default KICAD_FOOTPRINT_PATH or environment variable.
    
    Returns:
        Complete kicad_pcb S-expression string
    """
    import time
    start_time = time.time()
    
    # Parse existing PCB file
    existing_pcb_data = parse_sexp(existing_pcb_content)
    if not isinstance(existing_pcb_data, list) or len(existing_pcb_data) == 0 or existing_pcb_data[0] != 'kicad_pcb':
        raise ValueError("Invalid kicad_pcb file format")
    
    # Parse schematic for symbol mapping
    symbol_map = parse_schematic_symbols(kicad_sch_content)
    
    # Build element maps by UUID for fast lookup
    element_maps = {
        'footprint': {},
        'segment': {},
        'via': {},
        'zone': {},
        'gr_line': {},  # edges
        'gr_text': {}   # text
    }
    
    all_elements = existing_pcb_data[1:] if len(existing_pcb_data) > 1 else []
    
    for elem in all_elements:
        if not isinstance(elem, list) or len(elem) == 0:
            continue
        
        elem_type = elem[0]
        uuid_elem = find_element(elem, 'uuid')
        if uuid_elem:
            uuid_val = get_atom_value(uuid_elem, 1, None)
            if uuid_val:
                # For gr_line, only index if it's on Edge.Cuts layer
                if elem_type == 'gr_line':
                    layer_elem = find_element(elem, 'layer')
                    if layer_elem:
                        layer_name = get_atom_value(layer_elem, 1, None)
                        if layer_name == 'Edge.Cuts':
                            element_maps[elem_type][uuid_val] = elem
                elif elem_type in element_maps:
                    element_maps[elem_type][uuid_val] = elem
    
    # Hardcoded metadata values for pcbnew
    # These are no longer read from trace_json
    PCBNEW_VERSION = 20251101
    PCBNEW_GENERATOR = 'pcbnew'
    PCBNEW_GENERATOR_VERSION = 9.99
    
    # Separate trace JSON statements by type
    # Note: kicad_ver, kicad_gen, kicad_gen_ver are ignored - we use hardcoded values
    trace_footprints = []
    trace_segments = []
    trace_vias = []
    trace_zones = []
    trace_edges = []
    trace_texts = []
    
    for item in trace_json:
        item_type = item.get('type')
        if item_type in ('kicad_ver', 'kicad_gen', 'kicad_gen_ver'):
            # Skip these - we use hardcoded values
            pass
        elif item_type == 'footprint':
            trace_footprints.append(item)
        elif item_type == 'segment':
            trace_segments.append(item)
        elif item_type == 'via':
            trace_vias.append(item)
        elif item_type == 'zone':
            trace_zones.append(item)
        elif item_type == 'edge':
            trace_edges.append(item)
        elif item_type == 'text':
            trace_texts.append(item)
    
    # Start building result PCB structure
    result_pcb = ['kicad_pcb']
    
    # Use hardcoded metadata values
    result_pcb.append(['version', PCBNEW_VERSION])
    result_pcb.append(['generator', PCBNEW_GENERATOR])
    result_pcb.append(['generator_version', PCBNEW_GENERATOR_VERSION])
    
    # Copy general, layers, setup sections from existing PCB
    for section_name in ('general', 'layers', 'setup'):
        section_elem = find_element(existing_pcb_data, section_name)
        if section_elem:
            result_pcb.append(section_elem)
    
    # Copy paper if present
    paper_elem = find_element(existing_pcb_data, 'paper')
    if paper_elem:
        result_pcb.append(paper_elem)
    
    # Process footprints
    processed_uuids = set()
    
    for trace_fp in trace_footprints:
        fp_uid = trace_fp.get('uid')
        if not fp_uid:
            continue
        
        processed_uuids.add(fp_uid)
        
        if fp_uid in element_maps['footprint']:
            # Update existing footprint
            existing_fp = element_maps['footprint'][fp_uid]
            updated_fp = merge_footprint(existing_fp, trace_fp)
            result_pcb.append(updated_fp)
        else:
            # Generate new footprint
            ref = trace_fp.get('ref', '')
            symbol_info = symbol_map.get(ref) if ref else None
            new_fp = generate_footprint(trace_fp, symbol_info, footprint_paths)
            if new_fp:
                result_pcb.append(new_fp)
    
    # Process segments
    processed_uuids = set()
    
    for trace_seg in trace_segments:
        seg_uid = trace_seg.get('uid')
        if not seg_uid:
            continue
        
        processed_uuids.add(seg_uid)
        
        if seg_uid in element_maps['segment']:
            # Update existing segment
            existing_seg = element_maps['segment'][seg_uid]
            updated_seg = merge_segment(existing_seg, trace_seg)
            result_pcb.append(updated_seg)
        else:
            # Generate new segment
            new_seg = generate_segment(trace_seg)
            if new_seg:
                result_pcb.append(new_seg)
    
    # Process vias
    processed_uuids = set()
    
    for trace_via in trace_vias:
        via_uid = trace_via.get('uid')
        if not via_uid:
            continue
        
        processed_uuids.add(via_uid)
        
        if via_uid in element_maps['via']:
            # Update existing via
            existing_via = element_maps['via'][via_uid]
            updated_via = merge_via(existing_via, trace_via)
            result_pcb.append(updated_via)
        else:
            # Generate new via
            new_via = generate_via(trace_via)
            if new_via:
                result_pcb.append(new_via)
    
    # Process zones
    processed_uuids = set()
    
    for trace_zone in trace_zones:
        zone_uid = trace_zone.get('uid')
        if not zone_uid:
            continue
        
        processed_uuids.add(zone_uid)
        
        if zone_uid in element_maps['zone']:
            # Update existing zone
            existing_zone = element_maps['zone'][zone_uid]
            updated_zone = merge_zone(existing_zone, trace_zone)
            result_pcb.append(updated_zone)
        else:
            # Generate new zone
            new_zone = generate_zone(trace_zone)
            if new_zone:
                result_pcb.append(new_zone)
    
    # Process edges (gr_line on Edge.Cuts)
    processed_uuids = set()
    
    for trace_edge in trace_edges:
        edge_uid = trace_edge.get('uid')
        if not edge_uid:
            continue
        
        processed_uuids.add(edge_uid)
        
        if edge_uid in element_maps['gr_line']:
            # Update existing edge
            existing_edge = element_maps['gr_line'][edge_uid]
            updated_edge = merge_edge(existing_edge, trace_edge)
            result_pcb.append(updated_edge)
        else:
            # Generate new edge
            new_edge = generate_edge(trace_edge)
            if new_edge:
                result_pcb.append(new_edge)
    
    # Process texts (gr_text)
    processed_uuids = set()
    
    for trace_text in trace_texts:
        text_uid = trace_text.get('uid')
        if not text_uid:
            continue
        
        processed_uuids.add(text_uid)
        
        if text_uid in element_maps['gr_text']:
            # Update existing text
            existing_text = element_maps['gr_text'][text_uid]
            updated_text = merge_text(existing_text, trace_text)
            result_pcb.append(updated_text)
        else:
            # Generate new text
            new_text = generate_text(trace_text)
            if new_text:
                result_pcb.append(new_text)
    
    # Add any other elements from existing PCB (embedded_fonts, etc.)
    # Collect all element types we've already processed
    processed_types = {'footprint', 'segment', 'via', 'zone', 'gr_line', 'gr_text', 'version', 'generator', 'generator_version', 'general', 'layers', 'setup', 'paper'}
    
    # Track which single-instance elements we've added
    added_single_elements = set()
    
    for elem in all_elements:
        if isinstance(elem, list) and len(elem) > 0:
            elem_type = elem[0]
            if elem_type not in processed_types:
                # For single-instance elements (like embedded_fonts), only add once
                if elem_type in ('embedded_fonts',):
                    if elem_type not in added_single_elements:
                        result_pcb.append(elem)
                        added_single_elements.add(elem_type)
                else:
                    # For other elements, add them
                    result_pcb.append(elem)
    
    # Format as S-expression string
    result = format_sexp(result_pcb)
    
    end_time = time.time()
    print(f"Conversion complete in {end_time - start_time} seconds")
    return result


# =============================================================================
# Command-Line Interface
# =============================================================================

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) < 4:
        print("Usage: python trace_json_to_sexp.py <trace_json_file> <existing_kicad_pcb> <kicad_sch_file> [--output <output_file>]")
        sys.exit(1)
    
    trace_json_file = sys.argv[1]
    existing_pcb_file = sys.argv[2]
    kicad_sch_file = sys.argv[3]
    
    output_file = None
    if '--output' in sys.argv:
        output_idx = sys.argv.index('--output')
        if output_idx + 1 < len(sys.argv):
            output_file = sys.argv[output_idx + 1]
    
    try:
        # Load trace JSON
        with open(trace_json_file, "r") as file:
            trace_json = json.load(file)
        
        # Load existing PCB
        with open(existing_pcb_file, "r") as file:
            existing_pcb_content = file.read()
        
        # Load schematic
        with open(kicad_sch_file, "r") as file:
            kicad_sch_content = file.read()
        
        # Convert
        sexp_output = trace_json_to_sexp(trace_json, existing_pcb_content, kicad_sch_content)
        
        # Write output
        if output_file:
            output_filename = output_file
        else:
            output_filename = existing_pcb_file.replace('.kicad_pcb', '_converted.kicad_pcb')
            if output_filename == existing_pcb_file:
                output_filename = 'output.kicad_pcb'
        
        with open(output_filename, "w") as file:
            file.write(sexp_output)
        
        print(f"Conversion complete. Output written to {output_filename}")
    
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
