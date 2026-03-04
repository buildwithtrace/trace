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
Trace PCB Converter

Converts parsed statement dictionaries back into .trace_pcb file format.
"""

from typing import List, Dict, Any, Union, Tuple


class TraceConverter:
    """Converts statement dictionaries to trace_pcb format."""
    
    def __init__(self):
        self.lines: List[str] = []
    
    def convert(self, statements: List[Dict[str, Any]]) -> str:
        """
        Convert a list of statements to trace_pcb format.
        
        Args:
            statements: List of statement dictionaries from parser
            
        Returns:
            Formatted trace_pcb content as string
        """
        self.lines = []
        
        # Note: kicad_ver, kicad_gen, kicad_gen_ver are no longer output - they are hardcoded in the converter
        # Filter out any metadata statements that might still be in the input
        other_statements = []
        
        for stmt in statements:
            stmt_type = stmt.get("type")
            if stmt_type in ("kicad_ver", "kicad_gen", "kicad_gen_ver"):
                # Skip these - they are hardcoded in the converter
                pass
            else:
                other_statements.append(stmt)
        
        # Output statements
        for stmt in other_statements:
            stmt_type = stmt.get("type")
            
            if stmt_type == "footprint":
                self._convert_footprint(stmt)
            elif stmt_type == "segment":
                self._convert_segment(stmt)
            elif stmt_type == "via":
                self._convert_via(stmt)
            elif stmt_type == "zone":
                self._convert_zone(stmt)
            elif stmt_type == "edge":
                self._convert_edge(stmt)
            elif stmt_type == "text":
                self._convert_text(stmt)
            else:
                # Unknown type, skip
                continue
        
        return "\n".join(self.lines)
    
    # Formatting helpers
    
    def _format_coord(self, coord: Union[Tuple[float, float], List[float]]) -> str:
        """Format a coordinate as 'x,y'."""
        x, y = coord[0], coord[1]
        # Format as integer if whole number, otherwise float
        if isinstance(x, float) and x.is_integer():
            x = int(x)
        if isinstance(y, float) and y.is_integer():
            y = int(y)
        return f"{x},{y}"
    
    def _escape_string(self, s: str) -> str:
        """Escape a string for output."""
        # Escape backslashes and quotes
        s = s.replace("\\", "\\\\")
        s = s.replace('"', '\\"')
        return s
    
    def _format_string_value(self, value: Any) -> str:
        """Format a value that could be string, number, or identifier."""
        if isinstance(value, str):
            # Check if it needs quoting (contains spaces or special chars)
            if any(c in value for c in [' ', '\t', '\n', ':', '=', '@', '{', '}', '(', ')', ',', '.']):
                return f'"{self._escape_string(value)}"'
            return value
        elif isinstance(value, (int, float)):
            # Format as integer if whole number
            if isinstance(value, float) and value.is_integer():
                return str(int(value))
            return str(value)
        else:
            return str(value)
    
    def _format_layer_name(self, layer: str) -> str:
        """Format a layer name - always quote layer names."""
        return f'"{self._escape_string(layer)}"'
    
    def _format_footprint_name(self, lib_path: str) -> str:
        """Format a footprint name - quote if it contains a comma."""
        if ',' in lib_path:
            return f'"{self._escape_string(lib_path)}"'
        return lib_path
    
    def _format_pad_list(self, pads: Dict[Union[str, int], str]) -> str:
        """Format a pad list as '(pad_num=net, pad_num=net)'."""
        if not pads:
            return "()"
        
        pad_parts = []
        for pad_num, net_name in pads.items():
            pad_parts.append(f"{pad_num}={net_name}")
        
        return "(" + ", ".join(pad_parts) + ")"
    
    def _format_layer_list(self, layers: List[str]) -> str:
        """Format a layer list as '("layer1", "layer2")'."""
        if not layers:
            return "()"
        
        layer_parts = []
        for layer in layers:
            # Always quote layer names
            layer_parts.append(f'"{self._escape_string(layer)}"')
        
        return "(" + ", ".join(layer_parts) + ")"
    
    def _format_polygon_points(self, points: List[Union[Tuple[float, float], List[float]]]) -> str:
        """Format polygon points as '(x1,y1, x2,y2, ...)'."""
        if not points:
            return "()"
        
        coord_strs = [self._format_coord(p) for p in points]
        return "(" + ", ".join(coord_strs) + ")"
    
    def _format_edge_points(self, points: List[Union[Tuple[float, float], List[float]]]) -> str:
        """Format edge points as 'coord -> coord -> coord'."""
        if not points:
            return ""
        
        coord_strs = [self._format_coord(p) for p in points]
        return " -> ".join(coord_strs)
    
    # Statement converters
    # Note: _convert_kicad_ver, _convert_kicad_gen, _convert_kicad_gen_ver
    # have been removed - these metadata values are now hardcoded in the converter
    
    def _convert_footprint(self, stmt: Dict[str, Any]):
        """Convert a footprint statement."""
        parts = ["footprint"]
        parts.append(stmt["ref"])
        parts.append(self._format_footprint_name(stmt["lib_path"]))
        
        # Optional @ coord
        if "at" in stmt:
            parts.append("@")
            parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            rot_value = stmt["rot"]
            if isinstance(rot_value, float) and rot_value.is_integer():
                rot_value = int(rot_value)
            parts.append(str(rot_value))
        
        # Required layer
        parts.append("layer")
        parts.append(self._format_layer_name(stmt["layer"]))
        
        # Required pads
        parts.append("pads")
        parts.append(self._format_pad_list(stmt["pads"]))
        
        # Required uid
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_segment(self, stmt: Dict[str, Any]):
        """Convert a segment statement."""
        parts = ["segment"]
        parts.append("start")
        print(stmt["start"])
        print(self._format_coord(stmt["start"]))
        parts.append(self._format_coord(stmt["start"]))
        parts.append("end")
        parts.append(self._format_coord(stmt["end"]))
        parts.append("width")
        
        # Format width
        width = stmt["width"]
        if isinstance(width, float) and width.is_integer():
            width = int(width)
        parts.append(str(width))
        
        parts.append("layer")
        parts.append(self._format_layer_name(str(stmt["layer"])))
        parts.append("net")
        parts.append(str(stmt["net"]))
        parts.append("uid")
        parts.append(str(stmt["uid"]))
        
        self.lines.append(" ".join(parts))
    
    def _convert_via(self, stmt: Dict[str, Any]):
        """Convert a via statement."""
        parts = ["via"]
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        parts.append("size")
        
        # Format size
        size = stmt["size"]
        if isinstance(size, float) and size.is_integer():
            size = int(size)
        parts.append(str(size))
        
        parts.append("drill")
        
        # Format drill
        drill = stmt["drill"]
        if isinstance(drill, float) and drill.is_integer():
            drill = int(drill)
        parts.append(str(drill))
        
        parts.append("layers")
        parts.append(self._format_layer_list(stmt["layers"]))
        
        # Optional net
        if "net" in stmt:
            parts.append("net")
            parts.append(str(stmt["net"]))
        
        parts.append("uid")
        parts.append(str(stmt["uid"]))
        
        self.lines.append(" ".join(parts))
    
    def _convert_zone(self, stmt: Dict[str, Any]):
        """Convert a zone statement."""
        parts = ["zone"]
        parts.append("net")
        parts.append(str(stmt["net"]))
        
        # Handle layers - support both 'layers' (array) and 'layer' (single, for backward compatibility)
        layers = stmt.get("layers", [])
        layer = stmt.get("layer")
        
        if layers and len(layers) > 1:
            # Multi-layer zone: use 'layers' keyword
            parts.append("layers")
            parts.append(self._format_layer_list(layers))
        elif layers and len(layers) == 1:
            # Single layer in 'layers' array
            parts.append("layer")
            parts.append(self._format_layer_name(layers[0]))
        elif layer:
            # Backward compatibility: single 'layer' key
            parts.append("layer")
            parts.append(self._format_layer_name(layer))
        else:
            # Default to F.Cu
            parts.append("layer")
            parts.append(self._format_layer_name("F.Cu"))
        
        parts.append("polygon")
        parts.append(self._format_polygon_points(stmt["polygon"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_edge(self, stmt: Dict[str, Any]):
        """Convert an edge statement."""
        parts = ["edge"]
        parts.append(self._format_edge_points(stmt["points"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_text(self, stmt: Dict[str, Any]):
        """Convert a text statement."""
        parts = ["text"]
        # Text content (STRING)
        parts.append(f'"{stmt["text"]}"')
        
        # Required @ coord
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        
        # Required layer
        parts.append("layer")
        parts.append(self._format_layer_name(stmt["layer"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Optional font_size
        if "font_size" in stmt:
            parts.append("font_size")
            parts.append(self._format_coord(stmt["font_size"]))
        
        # Optional font_thickness
        if "font_thickness" in stmt:
            parts.append("font_thickness")
            thickness = stmt["font_thickness"]
            if isinstance(thickness, float) and thickness.is_integer():
                thickness = int(thickness)
            parts.append(str(thickness))
        
        # Optional justify
        if "justify" in stmt:
            parts.append("justify")
            justify = stmt["justify"]
            if isinstance(justify, list) and len(justify) >= 2:
                parts.append(justify[0])
                parts.append(justify[1])
            else:
                # Fallback if not a list
                parts.append(str(justify))
        
        # Required uid
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))


# Public API

def convert_to_trace_pcb(statements: List[Dict[str, Any]]) -> str:
    """
    Convert a list of statement dictionaries to trace_pcb format.
    
    Args:
        statements: List of statement dictionaries from parser
        
    Returns:
        Formatted trace_pcb content as string
    """
    converter = TraceConverter()
    return converter.convert(statements)


# Testing

if __name__ == "__main__":
    import sys
    import json
    
    if len(sys.argv) != 2:
        print("Usage: python trace_converter.py <json_file>")
        sys.exit(1)
    
    filename = sys.argv[1]
    with open(filename, "r") as file:
        statements = json.load(file)
    
    trace_pcb_content = convert_to_trace_pcb(statements)
    with open("output.trace_pcb", "w") as file:
        file.write(trace_pcb_content)
    
    print("Converted to output.trace_pcb")
