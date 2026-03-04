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
Trace SCH Converter

Converts parsed statement dictionaries back into .trace_sch file format.
"""

from typing import List, Dict, Any, Union, Tuple


class TraceConverter:
    """Converts statement dictionaries to trace_sch format."""
    
    def __init__(self):
        self.lines: List[str] = []
    
    def convert(self, statements: List[Dict[str, Any]]) -> str:
        """
        Convert a list of statements to trace_sch format.
        
        Args:
            statements: List of statement dictionaries from parser
            
        Returns:
            Formatted trace_sch content as string
        """
        self.lines = []
        
        # Separate metadata statements from other statements
        # Note: kicad_ver, kicad_gen, kicad_gen_ver are no longer output - they are hardcoded in the converter
        metadata_statements = []
        other_statements = []
        
        for stmt in statements:
            stmt_type = stmt.get("type")
            if stmt_type in ("file_uid", "paper"):
                metadata_statements.append(stmt)
            elif stmt_type in ("kicad_ver", "kicad_gen", "kicad_gen_ver"):
                # Skip these - they are hardcoded in the converter
                pass
            else:
                other_statements.append(stmt)
        
        # Group consecutive net statements
        grouped_statements = []
        i = 0
        while i < len(other_statements):
            if other_statements[i].get("type") == "net":
                # Collect all consecutive net statements
                net_group = []
                while i < len(other_statements) and other_statements[i].get("type") == "net":
                    net_group.append(other_statements[i])
                    i += 1
                # Add as a single grouped net statement
                grouped_statements.append({"type": "net_group", "nets": net_group})
            else:
                grouped_statements.append(other_statements[i])
                i += 1
        
        # Output metadata statements first
        for stmt in metadata_statements:
            stmt_type = stmt.get("type")
            if stmt_type == "file_uid":
                self._convert_file_uid(stmt)
            elif stmt_type == "paper":
                self._convert_paper(stmt)
        
        # Output other statements
        for stmt in grouped_statements:
            stmt_type = stmt.get("type")
            
            if stmt_type == "comment":
                self._convert_comment(stmt)
            elif stmt_type == "component":
                self._convert_component(stmt)
            elif stmt_type == "net_group":
                self._convert_net_group(stmt)
            elif stmt_type == "net":
                self._convert_net(stmt)
            elif stmt_type == "wire":
                self._convert_wire(stmt)
            elif stmt_type == "cwire":
                self._convert_cwire(stmt)
            elif stmt_type == "cwiredef":
                self._convert_cwiredef(stmt)
            elif stmt_type == "label":
                self._convert_label(stmt)
            elif stmt_type == "glabel":
                self._convert_glabel(stmt)
            elif stmt_type == "hier":
                self._convert_hier(stmt)
            elif stmt_type == "sheet":
                self._convert_sheet(stmt)
            elif stmt_type == "text":
                self._convert_text(stmt)
            elif stmt_type == "junction":
                self._convert_junction(stmt)
            elif stmt_type == "noconnect":
                self._convert_noconnect(stmt)
            elif stmt_type == "bus":
                self._convert_bus(stmt)
            elif stmt_type == "polyline":
                self._convert_polyline(stmt)
            elif stmt_type == "rectangle":
                self._convert_rectangle(stmt)
            elif stmt_type == "arc":
                self._convert_arc(stmt)
            elif stmt_type == "bezier":
                self._convert_bezier(stmt)
            elif stmt_type == "circle":
                self._convert_circle(stmt)
            elif stmt_type == "text_box":
                self._convert_text_box(stmt)
            elif stmt_type == "bus_entry":
                self._convert_bus_entry(stmt)
            elif stmt_type == "instance_meta":
                self._convert_instance_meta(stmt)
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
            if any(c in value for c in [' ', '\t', '\n', ':', '=', '@', '{', '}', '(', ')', ',']):
                return f'"{self._escape_string(value)}"'
            return value
        elif isinstance(value, (int, float)):
            # Format as integer if whole number
            if isinstance(value, float) and value.is_integer():
                return str(int(value))
            return str(value)
        else:
            return str(value)
    
    def _format_prop_list(self, props: Dict[str, str]) -> str:
        """Format a property list as '{ prop "key":"value", ... }'."""
        if not props:
            return "{}"
        
        prop_parts = []
        for key, value in props.items():
            escaped_key = self._escape_string(key)
            escaped_value = self._escape_string(value)
            prop_parts.append(f'prop "{escaped_key}":"{escaped_value}"')
        
        return "{ " + " , ".join(prop_parts) + " }"
    
    def _format_pin_list(self, pins: Dict[Union[str, int], str]) -> str:
        """Format a pin list as '(pin=net,pin=net)'."""
        if not pins:
            return "()"
        
        pin_parts = []
        for pin_num, net_name in pins.items():
            pin_parts.append(f"{pin_num}={net_name}")
        
        return "(" + ",".join(pin_parts) + ")"
    
    def _format_pinmap(self, pins: Dict[Union[str, int], Union[str, Dict[str, Any]]]) -> str:
        """Format a pinmap as '{ pin=net, pin=net @ x,y rot N }'."""
        if not pins:
            return "{}"
        
        pin_parts = []
        for pin_num, pin_value in pins.items():
            if isinstance(pin_value, dict):
                # Pin has coordinates: format as "pin=net @ x,y rot N"
                net_name = pin_value.get("net", "")
                coord = pin_value.get("at")
                rot = pin_value.get("rot")
                if coord:
                    coord_str = self._format_coord(coord)
                    pin_str = f"{pin_num}={net_name} @ {coord_str}"
                    if rot is not None:
                        pin_str += f" rot {int(rot)}"
                    pin_parts.append(pin_str)
                else:
                    # Fallback to simple format if no coordinates
                    pin_parts.append(f"{pin_num}={net_name}")
            else:
                # Simple string format (backward compatibility)
                pin_parts.append(f"{pin_num}={pin_value}")
        
        return "{ " + " , ".join(pin_parts) + " }"
    
    def _format_wire_points(self, points: List[Union[Tuple[float, float], List[float]]]) -> str:
        """Format wire points as 'coord -> coord -> coord'."""
        if not points:
            return ""
        
        coord_strs = [self._format_coord(p) for p in points]
        return " -> ".join(coord_strs)
    
    # Statement converters
    
    def _convert_comment(self, stmt: Dict[str, Any]):
        """Convert a comment statement."""
        text = stmt.get("text", "")
        # Preserve the comment text, adding # if not present
        if text.startswith("#"):
            self.lines.append(text)
        else:
            self.lines.append(f"# {text}")
    
    def _convert_component(self, stmt: Dict[str, Any]):
        """Convert a component statement."""
        parts = ["comp"]
        parts.append(stmt["ref"])
        parts.append(stmt["symbol"])
        
        # Optional value
        if "value" in stmt:
            parts.append(self._format_string_value(stmt["value"]))
        
        # Optional @ coord
        if "at" in stmt:
            parts.append("@")
            parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Optional unit (only output if not 1, the default)
        unit = stmt.get("unit", 1)
        if unit != 1:
            parts.append("unit")
            parts.append(str(int(unit)))
        
        # Optional body_style (only output if not 1, the default)
        body_style = stmt.get("body_style", 1)
        if body_style != 1:
            parts.append("body_style")
            parts.append(str(int(body_style)))
        
        # Optional props
        if "props" in stmt:
            parts.append("props")
            parts.append(self._format_prop_list(stmt["props"]))
        
        # Required pins
        parts.append("pins")
        parts.append(self._format_pin_list(stmt["pins"]))
        
        # Required uid
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_net(self, stmt: Dict[str, Any]):
        """Convert a net statement."""
        self.lines.append(f"net {stmt['name']}")
    
    def _convert_net_group(self, stmt: Dict[str, Any]):
        """Convert a grouped net statement (multiple nets combined)."""
        nets = stmt.get("nets", [])
        if nets:
            net_names = [net_stmt["name"] for net_stmt in nets]
            self.lines.append(f"net {' '.join(net_names)}")
    
    def _convert_wire(self, stmt: Dict[str, Any]):
        """Convert a wire statement."""
        parts = ["wire"]
        parts.append(self._format_wire_points(stmt["points"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        self.lines.append(" ".join(parts))
    
    def _format_endpoint(self, endpoint: Dict[str, Any]) -> str:
        """Format a cwire endpoint (pin_ref or junction_ref)."""
        endpoint_type = endpoint.get("type")
        
        if endpoint_type == "pin":
            ref = endpoint.get("ref", "")
            pin = endpoint.get("pin", "")
            return f"{ref}.{pin}"
        elif endpoint_type == "junction":
            junction_id = endpoint.get("id", "JUNC?")
            return junction_id
        else:
            return "?"
    
    def _convert_cwire(self, stmt: Dict[str, Any]):
        """Convert a cwire statement.
        
        Format: cwire CW_REF endpoint -> endpoint [net NET_NAME]
        """
        parts = ["cwire"]
        
        # cwire reference (CW1, CW2, etc.)
        cwire_ref = stmt.get("ref", "CW?")
        parts.append(cwire_ref)
        
        # From endpoint
        from_endpoint = stmt.get("from", {})
        parts.append(self._format_endpoint(from_endpoint))
        
        # Arrow
        parts.append("->")
        
        # To endpoint
        to_endpoint = stmt.get("to", {})
        parts.append(self._format_endpoint(to_endpoint))
        
        # Optional net clause
        if "net" in stmt:
            parts.append("net")
            parts.append(stmt["net"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_cwiredef(self, stmt: Dict[str, Any]):
        """Convert a cwiredef statement.
        
        Format: cwiredef CW_REF x1,y1 -> x2,y2 -> x3,y3
        """
        parts = ["cwiredef"]
        
        # cwire reference (CW1, CW2, etc.)
        cwire_ref = stmt.get("ref", "CW?")
        parts.append(cwire_ref)
        
        # Wire points
        points = stmt.get("points", [])
        parts.append(self._format_wire_points(points))
        
        self.lines.append(" ".join(parts))
    
    def _convert_label(self, stmt: Dict[str, Any]):
        """Convert a label statement."""
        parts = ["label"]
        parts.append(stmt["name"])
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        self.lines.append(" ".join(parts))
    
    def _convert_glabel(self, stmt: Dict[str, Any]):
        """Convert a glabel statement."""
        parts = ["glabel"]
        parts.append(stmt["name"])
        
        # Required @ coord
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Optional shape
        if "shape" in stmt:
            parts.append("shape")
            parts.append(stmt["shape"])
        
        # Optional props
        if "props" in stmt:
            parts.append(self._format_prop_list(stmt["props"]))
        
        self.lines.append(" ".join(parts))
    
    def _convert_hier(self, stmt: Dict[str, Any]):
        """Convert a hier statement."""
        parts = ["hier"]
        parts.append(stmt["name"])
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Optional shape
        if "shape" in stmt:
            parts.append("shape")
            parts.append(stmt["shape"])
        
        # Optional props
        if "props" in stmt:
            parts.append(self._format_prop_list(stmt["props"]))
        
        self.lines.append(" ".join(parts))
    
    def _convert_sheet(self, stmt: Dict[str, Any]):
        """Convert a sheet statement."""
        parts = ["sheet"]
        parts.append(stmt["name"])
        parts.append("file")
        
        # File can be string or ident
        file_value = stmt["file"]
        if isinstance(file_value, str):
            # Check if it needs quoting
            if any(c in file_value for c in [' ', '\t', '\n', ':', '=', '@', '{', '}', '(', ')', ',']):
                parts.append(f'"{self._escape_string(file_value)}"')
            else:
                parts.append(file_value)
        else:
            parts.append(str(file_value))
        
        # Optional @ coord
        if "at" in stmt:
            parts.append("@")
            parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Optional size
        if "size" in stmt:
            parts.append("size")
            parts.append(self._format_coord(stmt["size"]))
        
        # Optional props
        if "props" in stmt:
            parts.append(self._format_prop_list(stmt["props"]))
        
        # Optional ins
        if "ins" in stmt:
            parts.append("ins")
            parts.append(self._format_pinmap(stmt["ins"]))
        
        # Optional outs
        if "outs" in stmt:
            parts.append("outs")
            parts.append(self._format_pinmap(stmt["outs"]))
        
        # Required uid
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_text(self, stmt: Dict[str, Any]):
        """Convert a text statement."""
        parts = ["text"]
        parts.append(f'"{self._escape_string(stmt["string"])}"')
        
        # Optional @ coord
        if "at" in stmt:
            parts.append("@")
            parts.append(self._format_coord(stmt["at"]))
        
        # Optional props
        if "props" in stmt:
            parts.append(self._format_prop_list(stmt["props"]))
        
        self.lines.append(" ".join(parts))
    
    def _convert_junction(self, stmt: Dict[str, Any]):
        """Convert a junction statement.
        
        Format: junction [JUNC_ID] @ x,y uid UUID
        """
        parts = ["junction"]
        
        # Optional junction ID (e.g., JUNC1)
        if "id" in stmt:
            parts.append(stmt["id"])
        
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        self.lines.append(" ".join(parts))
    
    def _convert_noconnect(self, stmt: Dict[str, Any]):
        """Convert a noconnect statement."""
        parts = ["noconnect"]
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        self.lines.append(" ".join(parts))
    
    def _convert_bus(self, stmt: Dict[str, Any]):
        """Convert a bus statement."""
        parts = ["bus"]
        parts.append(self._format_wire_points(stmt["points"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_polyline(self, stmt: Dict[str, Any]):
        """Convert a polyline statement."""
        parts = ["polyline"]
        parts.append(self._format_wire_points(stmt["points"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _format_stroke(self, stroke: Dict[str, Any]) -> str:
        """Format a stroke as 'stroke width N type "TYPE"'."""
        width = stroke.get("width", 0)
        if isinstance(width, float) and width.is_integer():
            width = int(width)
        stroke_type = stroke.get("type", "default")
        return f'stroke width {width} type "{self._escape_string(stroke_type)}"'
    
    def _format_fill(self, fill: Dict[str, Any]) -> str:
        """Format a fill as 'fill type "TYPE" [color R G B A]'."""
        fill_type = fill.get("type", "none")
        parts = [f'fill type "{self._escape_string(fill_type)}"']
        if "color" in fill:
            color = fill["color"]
            if len(color) >= 4:
                r = int(color[0]) if isinstance(color[0], float) and color[0].is_integer() else color[0]
                g = int(color[1]) if isinstance(color[1], float) and color[1].is_integer() else color[1]
                b = int(color[2]) if isinstance(color[2], float) and color[2].is_integer() else color[2]
                a = int(color[3]) if isinstance(color[3], float) and color[3].is_integer() else color[3]
                parts.append(f"color {r} {g} {b} {a}")
        return " ".join(parts)
    
    def _convert_rectangle(self, stmt: Dict[str, Any]):
        """Convert a rectangle statement."""
        parts = ["rectangle"]
        parts.append("start")
        parts.append(self._format_coord(stmt["start"]))
        parts.append("end")
        parts.append(self._format_coord(stmt["end"]))
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append(self._format_fill(stmt["fill"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_arc(self, stmt: Dict[str, Any]):
        """Convert an arc statement."""
        parts = ["arc"]
        parts.append("start")
        parts.append(self._format_coord(stmt["start"]))
        parts.append("mid")
        parts.append(self._format_coord(stmt["mid"]))
        parts.append("end")
        parts.append(self._format_coord(stmt["end"]))
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append(self._format_fill(stmt["fill"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_bezier(self, stmt: Dict[str, Any]):
        """Convert a bezier statement."""
        parts = ["bezier"]
        parts.append(self._format_wire_points(stmt["points"]))
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append(self._format_fill(stmt["fill"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_circle(self, stmt: Dict[str, Any]):
        """Convert a circle statement."""
        parts = ["circle"]
        parts.append("center")
        parts.append(self._format_coord(stmt["center"]))
        parts.append("radius")
        radius = stmt["radius"]
        if isinstance(radius, float) and radius.is_integer():
            radius = int(radius)
        parts.append(str(radius))
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append(self._format_fill(stmt["fill"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_text_box(self, stmt: Dict[str, Any]):
        """Convert a text_box statement."""
        parts = ["text_box"]
        parts.append(f'"{self._escape_string(stmt["text"])}"')
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        
        # Optional rot
        if "rot" in stmt:
            parts.append("rot")
            parts.append(str(int(stmt["rot"])))
        
        # Required size
        parts.append("size")
        parts.append(self._format_coord(stmt["size"]))
        
        # Optional margins
        if "margins" in stmt:
            parts.append("margins")
            margins = stmt["margins"]
            margin_strs = []
            for m in margins:
                if isinstance(m, float) and m.is_integer():
                    margin_strs.append(str(int(m)))
                else:
                    margin_strs.append(str(m))
            parts.append(",".join(margin_strs))
        
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append(self._format_fill(stmt["fill"]))
        
        # Optional effects
        if "effects" in stmt:
            parts.append("effects")
            parts.append(self._format_prop_list(stmt["effects"]))
        
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    def _convert_bus_entry(self, stmt: Dict[str, Any]):
        """Convert a bus_entry statement."""
        parts = ["bus_entry"]
        parts.append("@")
        parts.append(self._format_coord(stmt["at"]))
        parts.append("size")
        parts.append(self._format_coord(stmt["size"]))
        parts.append(self._format_stroke(stmt["stroke"]))
        parts.append("uid")
        parts.append(stmt["uid"])
        
        self.lines.append(" ".join(parts))
    
    # Note: _convert_kicad_ver, _convert_kicad_gen, _convert_kicad_gen_ver
    # have been removed - these metadata values are now hardcoded in the converter
    
    def _convert_file_uid(self, stmt: Dict[str, Any]):
        """Convert a file_uid statement."""
        value = stmt.get("value", "")
        self.lines.append(f"file_uid {value}")
    
    def _convert_paper(self, stmt: Dict[str, Any]):
        """Convert a paper statement."""
        value = stmt.get("value", "")
        self.lines.append(f'paper "{self._escape_string(value)}"')
    
    def _convert_instance_meta(self, stmt: Dict[str, Any]):
        """Convert an instance_meta statement."""
        parts = ["inst"]
        parts.append("project")
        parts.append(f'"{self._escape_string(stmt["project"])}"')
        parts.append("path")
        parts.append(f'"{self._escape_string(stmt["path"])}"')
        
        # Optional page
        if "page" in stmt:
            parts.append("page")
            parts.append(str(int(stmt["page"])))
        
        self.lines.append(" ".join(parts))


# Public API

def convert_to_trace_sch(statements: List[Dict[str, Any]]) -> str:
    """
    Convert a list of statement dictionaries to trace_sch format.
    
    Args:
        statements: List of statement dictionaries from parser
        
    Returns:
        Formatted trace_sch content as string
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
    
    trace_sch_content = convert_to_trace_sch(statements)
    with open("output.trace_sch", "w") as file:
        file.write(trace_sch_content)
