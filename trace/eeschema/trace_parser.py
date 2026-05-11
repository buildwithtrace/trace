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
Trace SCH Parser

A recursive descent parser for .trace_sch files based on the EBNF grammar
defined in trace_ebnf. Parses TOON-based schematic format into structured
dictionaries/lists.
"""

import os
import re
import uuid
from typing import List, Dict, Any, Optional, Set, Tuple, Union
from dataclasses import dataclass


# =============================================================================
# Constants
# =============================================================================

VALID_LABEL_SHAPES = {"input", "output", "bidirectional", "tri_state", "passive"}


# =============================================================================
# Exception Classes
# =============================================================================

class TraceParseError(Exception):
    """Raised when a parse error occurs."""
    
    def __init__(self, message: str, line: int = 0, column: int = 0):
        self.message = message
        self.line = line
        self.column = column
        super().__init__(f"Parse error at line {line}, column {column}: {message}")


class TraceParseWarning:
    """Represents a critical warning during parsing (non-fatal)."""
    
    def __init__(self, message: str, line: int = 0, column: int = 0):
        self.message = message
        self.line = line
        self.column = column
    
    def __repr__(self):
        return f"TraceParseWarning(line={self.line}, col={self.column}: {self.message})"
    
    def __str__(self):
        return f"Warning at line {self.line}, column {self.column}: {self.message}"


# =============================================================================
# Token Classes
# =============================================================================

@dataclass
class Token:
    """Represents a token with type, value, and position."""
    type: str
    value: Any
    line: int
    column: int
    
    def __repr__(self):
        return f"Token({self.type}, {self.value!r}, line={self.line}, col={self.column})"


# =============================================================================
# Lexer
# =============================================================================

class TraceLexer:
    """Tokenizes input string into tokens."""
    
    # Token types
    EOF = "EOF"
    IDENT = "IDENT"
    NUMBER = "NUMBER"
    STRING = "STRING"
    UUID = "UUID"
    COMMENT = "COMMENT"
    WS = "WS"
    
    # Operators and punctuation
    AT = "@"
    ARROW = "->"
    EQUALS = "="
    COLON = ":"
    COMMA = ","
    LBRACE = "{"
    RBRACE = "}"
    LPAREN = "("
    RPAREN = ")"
    LT = "<"
    GT = ">"
    DOT = "."
    
    # Keywords
    KEYWORDS = {
        "comp", "net", "wire", "cwire", "cwiredef", "label", "glabel", "hier", "sheet",
        "text", "junction", "noconnect", "bus", "polyline", "inst",
        "rot", "mirror", "props", "pins", "shape", "file", "size", "ins", "outs",
        "project", "path", "page", "uid", "prop",
        "rectangle", "arc", "bezier", "circle", "text_box", "bus_entry",
        "start", "end", "mid", "center", "radius", "margins", "effects",
        "stroke", "fill", "width", "type", "color",
        "title_block", "title", "date", "rev", "company", "comment"
    }
    
    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.line = 1
        self.column = 1
        self.tokens: List[Token] = []
        self._tokenize()
    
    def _tokenize(self):
        """Tokenize the input text."""
        while self.pos < len(self.text):
            # Skip whitespace (but track it for line/column)
            if self._skip_whitespace():
                continue
            
            # Comments (only if # is followed by whitespace or newline)
            # Otherwise, # is part of an identifier like #PWR02
            if self._current_char() == '#':
                # Peek ahead to see if this is a comment or part of an identifier
                peek_pos = self.pos + 1
                if peek_pos >= len(self.text) or self.text[peek_pos].isspace():
                    # It's a comment
                    self._tokenize_comment()
                    continue
                # Otherwise, it's part of an identifier, fall through to identifier handling
            
            # Strings
            if self._current_char() == '"':
                self._tokenize_string()
                continue
            
            # Numbers (but check if it's actually part of an identifier like "10k" or a UUID)
            if self._current_char().isdigit() or (self._current_char() == '-' and 
                                                   self.pos + 1 < len(self.text) and 
                                                   self.text[self.pos + 1].isdigit()):
                # Check if this might be a UUID (starts with hex digit, has dashes in UUID pattern)
                # Peek ahead to see if it matches UUID pattern
                peek_pos = self.pos
                if self._current_char() == '-':
                    peek_pos += 1
                
                # Try to read what might be a UUID
                uuid_candidate = ""
                while peek_pos < len(self.text):
                    char = self.text[peek_pos]
                    if (char.isalnum() or char == '-'):
                        uuid_candidate += char
                        peek_pos += 1
                    else:
                        break
                
                # Check if it's a UUID
                if uuid_candidate and self._is_uuid(uuid_candidate):
                    # It's a UUID, tokenize as identifier (which will recognize it as UUID)
                    self._tokenize_ident()
                    continue
                
                # Check if this looks like an identifier (number followed by letter/underscore/colon)
                # by peeking ahead
                peek_pos = self.pos
                if self._current_char() == '-':
                    peek_pos += 1
                # Read digits
                while peek_pos < len(self.text) and (self.text[peek_pos].isdigit() or self.text[peek_pos] == '.'):
                    peek_pos += 1
                # Check if next char is letter/underscore/colon (making it an identifier)
                if peek_pos < len(self.text) and (self.text[peek_pos].isalpha() or self.text[peek_pos] == '_' or self.text[peek_pos] == ':'):
                    # It's an identifier, not a number
                    self._tokenize_ident()
                    continue
                else:
                    # It's a number
                    self._tokenize_number()
                    continue
            
            # Arrow operator (->)
            if self._current_char() == '-' and self.pos + 1 < len(self.text) and \
               self.text[self.pos + 1] == '>':
                self._add_token(self.ARROW, "->")
                self._advance(2)
                continue
            
            # Single character operators
            if self._current_char() == '@':
                self._add_token(self.AT, "@")
                self._advance()
                continue
            if self._current_char() == '=':
                self._add_token(self.EQUALS, "=")
                self._advance()
                continue
            if self._current_char() == ':':
                self._add_token(self.COLON, ":")
                self._advance()
                continue
            if self._current_char() == ',':
                self._add_token(self.COMMA, ",")
                self._advance()
                continue
            if self._current_char() == '{':
                self._add_token(self.LBRACE, "{")
                self._advance()
                continue
            if self._current_char() == '}':
                self._add_token(self.RBRACE, "}")
                self._advance()
                continue
            if self._current_char() == '(':
                self._add_token(self.LPAREN, "(")
                self._advance()
                continue
            if self._current_char() == ')':
                self._add_token(self.RPAREN, ")")
                self._advance()
                continue
            if self._current_char() == '<':
                self._add_token(self.LT, "<")
                self._advance()
                continue
            if self._current_char() == '>':
                self._add_token(self.GT, ">")
                self._advance()
                continue
            
            # Identifiers (including those starting with +, like +5V, or #, like #PWR02)
            if self._current_char().isalnum() or self._current_char() == '_' or self._current_char() == '+' or self._current_char() == '#' or self._current_char() == '~':
                self._tokenize_ident()
                continue
            
            # Unknown character
            raise TraceParseError(
                f"Unexpected character: {self._current_char()!r}",
                self.line, self.column
            )
        
        # Add EOF token
        self._add_token(self.EOF, None)
    
    def _current_char(self) -> str:
        """Get current character."""
        if self.pos >= len(self.text):
            return '\0'
        return self.text[self.pos]
    
    def _advance(self, n: int = 1):
        """Advance position by n characters."""
        for _ in range(n):
            if self.pos < len(self.text):
                if self.text[self.pos] == '\n':
                    self.line += 1
                    self.column = 1
                else:
                    self.column += 1
                self.pos += 1
    
    def _add_token(self, token_type: str, value: Any):
        """Add a token to the list."""
        self.tokens.append(Token(token_type, value, self.line, self.column))
    
    def _skip_whitespace(self) -> bool:
        """Skip whitespace characters. Returns True if whitespace was skipped."""
        if not self._current_char().isspace():
            return False
        
        start_pos = self.pos
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self._advance()
        
        # Only add WS token if we skipped something
        if self.pos > start_pos:
            self._add_token(self.WS, None)
        
        return True
    
    def _tokenize_comment(self):
        """Tokenize a comment (from # to end of line)."""
        start_line = self.line
        start_col = self.column
        start_pos = self.pos
        
        # Read until end of line
        while self.pos < len(self.text) and self.text[self.pos] != '\n':
            self._advance()
        
        # Include the newline if present
        if self.pos < len(self.text):
            self._advance()
        
        comment_text = self.text[start_pos:self.pos].rstrip('\n\r')
        self._add_token(self.COMMENT, comment_text)
    
    def _tokenize_string(self):
        """Tokenize a string literal."""
        start_line = self.line
        start_col = self.column
        start_pos = self.pos
        
        self._advance()  # Skip opening quote
        
        value = ""
        escape = False
        
        while self.pos < len(self.text):
            char = self._current_char()
            
            if escape:
                if char == 'n':
                    value += '\n'
                elif char == 't':
                    value += '\t'
                elif char == 'r':
                    value += '\r'
                elif char == '\\':
                    value += '\\'
                elif char == '"':
                    value += '"'
                else:
                    value += char
                escape = False
                self._advance()
                continue
            
            if char == '\\':
                escape = True
                self._advance()
                continue
            
            if char == '"':
                self._advance()  # Skip closing quote
                self._add_token(self.STRING, value)
                return
            
            value += char
            self._advance()
        
        raise TraceParseError(
            "Unterminated string literal",
            start_line, start_col
        )
    
    def _tokenize_number(self):
        """Tokenize a number (integer or float)."""
        start_line = self.line
        start_col = self.column
        start_pos = self.pos
        
        # Handle negative sign
        negative = False
        if self._current_char() == '-':
            negative = True
            self._advance()
        
        # Read digits
        value = ""
        has_dot = False
        
        while self.pos < len(self.text):
            char = self._current_char()
            if char.isdigit():
                value += char
                self._advance()
            elif char == '.' and not has_dot:
                value += char
                has_dot = True
                self._advance()
            else:
                break
        
        if not value or value == '.':
            raise TraceParseError(
                "Invalid number",
                start_line, start_col
            )
        
        num_value = float(value) if has_dot else int(value)
        if negative:
            num_value = -num_value
        
        self._add_token(self.NUMBER, num_value)
    
    def _tokenize_ident(self):
        """Tokenize an identifier."""
        start_pos = self.pos
        value = ""
        
        # Check if this might be a UUID (starts with hex digit)
        # UUIDs can contain dashes, so we need special handling
        if self._current_char().isalnum():
            # Try to read a potential UUID (allows dashes)
            while self.pos < len(self.text):
                char = self._current_char()
                if char.isalnum() or char == '_' or char == ':' or char == '-':
                    value += char
                    self._advance()
                elif char in ('~', '{', '}'):
                    value += char
                    self._advance()
                else:
                    break
            
            # Check if it's a UUID (format: 8-4-4-4-12 hex digits)
            if self._is_uuid(value):
                # Validate that it's not a nil UUID
                if self._is_nil_uuid(value):
                    raise TraceParseError(
                        f"Invalid nil UUID: {value}. UUIDs must not be all zeros.",
                        self.line, self.column
                    )
                self._add_token(self.UUID, value)
                return
            # Otherwise, it's a regular identifier (may contain +, dashes, colons, underscores, decimal points, #, ~{})
            # Continue reading to allow these characters
            while self.pos < len(self.text):
                char = self._current_char()
                if char.isalnum() or char in ('_', ':', '-', '+', '.', '#', '~', '{', '}'):
                    value += char
                    self._advance()
                else:
                    break
        else:
            # Regular identifier starting with +, _, :, -, #, or ~ (allows various special chars)
            while self.pos < len(self.text):
                char = self._current_char()
                if char.isalnum() or char in ('_', ':', '-', '+', '.', '#', '~', '{', '}'):
                    value += char
                    self._advance()
                else:
                    break
        
        if not value:
            raise TraceParseError(
                "Empty identifier",
                self.line, self.column
            )
        
        self._add_token(self.IDENT, value)
    
    def _is_uuid(self, value: str) -> bool:
        """Check if a value matches UUID format."""
        # UUID format: 8-4-4-4-12 hex digits
        uuid_pattern = r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$'
        return bool(re.match(uuid_pattern, value))
    
    def _is_nil_uuid(self, value: str) -> bool:
        """Check if a UUID is the nil UUID (all zeros)."""
        return value.lower() == "00000000-0000-0000-0000-000000000000"
    
    def get_tokens(self) -> List[Token]:
        """Get the list of tokens."""
        return self.tokens


# =============================================================================
# Parser
# =============================================================================

class TraceParser:
    """Parses tokens into statement dictionaries."""
    
    def __init__(self, tokens: List[Token], base_dir: Optional[str] = None):
        self.tokens = tokens
        self.pos = 0
        self.warnings: List[TraceParseWarning] = []
        self.base_dir = base_dir
    
    def _current_token(self) -> Token:
        """Get current token."""
        if self.pos >= len(self.tokens):
            return Token(TraceLexer.EOF, None, 0, 0)
        return self.tokens[self.pos]
    
    def _peek_token(self, offset: int = 1) -> Token:
        """Peek at token ahead."""
        idx = self.pos + offset
        if idx >= len(self.tokens):
            return Token(TraceLexer.EOF, None, 0, 0)
        return self.tokens[idx]
    
    def _advance(self):
        """Advance to next token."""
        if self.pos < len(self.tokens):
            self.pos += 1
    
    def _skip_ws(self):
        """Skip whitespace tokens."""
        while self._current_token().type == TraceLexer.WS:
            self._advance()
    
    def _expect(self, token_type: str, value: Any = None) -> Token:
        """Expect a specific token type (and optionally value)."""
        self._skip_ws()
        token = self._current_token()
        
        if token.type != token_type:
            raise TraceParseError(
                f"Expected {token_type}, got {token.type}",
                token.line, token.column
            )
        
        if value is not None and token.value != value:
            raise TraceParseError(
                f"Expected {token_type} with value {value!r}, got {token.value!r}",
                token.line, token.column
            )
        
        self._advance()
        return token
    
    def _optional(self, token_type: str, value: Any = None) -> bool:
        """Check if next token matches, and consume it if it does."""
        self._skip_ws()
        token = self._current_token()
        if token.type == token_type:
            if value is None or token.value == value:
                self._advance()  # Consume the token
                return True
        return False
    
    def _extract_uuid_from_statement(self, stmt: Dict[str, Any]) -> Optional[str]:
        """Extract UUID from a statement if it has one."""
        if not isinstance(stmt, dict):
            return None
        return stmt.get("uid")
    
    def _validate_uuid_uniqueness(self, statements: List[Dict[str, Any]]) -> None:
        """Validate that all UUIDs in statements are unique.
        
        Exception: Components with the same symbol and UUID but different units
        are allowed (e.g., U1 unit 1 and U1B unit 2 of the same component).
        """
        seen_uuids: Dict[str, List[Dict[str, Any]]] = {}
        
        for stmt in statements:
            uuid_val = self._extract_uuid_from_statement(stmt)
            if uuid_val:
                if uuid_val in seen_uuids:
                    # Check if this is a component with same symbol but different unit
                    existing_stmts = seen_uuids[uuid_val]
                    existing_stmt = existing_stmts[0]  # Get first occurrence
                    
                    # Allow duplicate UUIDs if both are components with same symbol but different units
                    if (stmt.get("type") == "component" and 
                        existing_stmt.get("type") == "component" and
                        stmt.get("symbol") == existing_stmt.get("symbol") and
                        stmt.get("unit") != existing_stmt.get("unit")):
                        # This is allowed - same component, different units
                        seen_uuids[uuid_val].append(stmt)
                        continue
                    
                    # Otherwise, it's a duplicate UUID error
                    existing_type = existing_stmt.get("type", "unknown")
                    current_type = stmt.get("type", "unknown")
                    
                    # Try to get line info from the statement if available
                    existing_ref = existing_stmt.get("ref", existing_stmt.get("name", ""))
                    current_ref = stmt.get("ref", stmt.get("name", ""))
                    
                    raise TraceParseError(
                        f"Duplicate UUID found: {uuid_val}. "
                        f"First occurrence: {existing_type} "
                        f"({existing_ref if existing_ref else 'no ref'}). "
                        f"Second occurrence: {current_type} "
                        f"({current_ref if current_ref else 'no ref'}).",
                        0, 0
                    )
                # Store as list to allow multiple components with same UUID
                seen_uuids[uuid_val] = [stmt]
    
    def parse(self) -> List[Dict[str, Any]]:
        """Parse the token stream into a list of statements."""
        statements = []
        
        while self.pos < len(self.tokens):
            self._skip_ws()
            
            if self._current_token().type == TraceLexer.EOF:
                break
            
            if self._current_token().type == TraceLexer.COMMENT:
                stmt = self._parse_comment()
                if stmt:
                    statements.append(stmt)
                continue
            
            stmt = self._parse_statement()
            if stmt:
                # Handle case where wire statement returns a list of wires
                if isinstance(stmt, list):
                    statements.extend(stmt)
                else:
                    statements.append(stmt)
        
        # Validate UUID uniqueness after parsing all statements
        self._validate_uuid_uniqueness(statements)
        
        # Auto-assign junction IDs to junctions that don't have them
        self._assign_junction_ids(statements)
        
        # Auto-assign cwire refs to cwires that don't have them
        self._assign_cwire_refs(statements)
        
        # Validate cwire endpoints (components/pins exist, nets match)
        self._validate_cwires(statements)
        
        # Validate sheet file existence and hier pin matching
        self._validate_sheets(statements)
        
        return statements
    
    def _assign_cwire_refs(self, statements: List[Dict[str, Any]]) -> None:
        """Auto-assign cwire references (CW1, CW2, etc.) to cwires without refs.
        
        This ensures all cwires have unique references that can be matched
        with cwiredef statements.
        """
        # First, collect existing cwire refs to avoid conflicts
        existing_refs = set()
        for stmt in statements:
            if stmt.get("type") == "cwire" and "ref" in stmt:
                existing_refs.add(stmt["ref"])
        
        # Find the highest existing CW number
        max_cw_num = 0
        for cw_ref in existing_refs:
            if cw_ref.startswith("CW"):
                try:
                    num = int(cw_ref[2:])
                    max_cw_num = max(max_cw_num, num)
                except ValueError:
                    pass
        
        # Assign refs to cwires without them
        next_cw_num = max_cw_num + 1
        for stmt in statements:
            if stmt.get("type") == "cwire" and "ref" not in stmt:
                stmt["ref"] = f"CW{next_cw_num}"
                next_cw_num += 1
    
    def _assign_junction_ids(self, statements: List[Dict[str, Any]]) -> None:
        """Auto-assign junction IDs (JUNC1, JUNC2, etc.) to junctions without IDs.
        
        This ensures all junctions have unique identifiers that can be referenced
        by cwire statements.
        """
        # First, collect existing junction IDs to avoid conflicts
        existing_ids = set()
        for stmt in statements:
            if stmt.get("type") == "junction" and "id" in stmt:
                existing_ids.add(stmt["id"])
        
        # Find the highest existing JUNC number
        max_junc_num = 0
        for junc_id in existing_ids:
            if junc_id.startswith("JUNC"):
                try:
                    num = int(junc_id[4:])
                    max_junc_num = max(max_junc_num, num)
                except ValueError:
                    pass
        
        # Assign IDs to junctions without them
        next_junc_num = max_junc_num + 1
        for stmt in statements:
            if stmt.get("type") == "junction" and "id" not in stmt:
                stmt["id"] = f"JUNC{next_junc_num}"
                next_junc_num += 1
    
    def _validate_cwires(self, statements: List[Dict[str, Any]]) -> None:
        """Validate cwire endpoints after all statements have been parsed.
        
        Checks:
        1. Pin endpoints: the component or sheet ref must exist and the pin must
           exist on that component/sheet. Raises TraceParseError if not.
        2. Pin-to-pin connections: both pins must belong to the same net.
           Adds a TraceParseWarning (critical warning, not error) if they don't.
        3. Junction endpoints (pin-junction, junction-junction): skipped for now
           since determining the net for a junction is non-trivial.
        """
        # Build index: component ref -> { pin_number -> net_name }
        comp_pins: Dict[str, Dict[Union[str, int], str]] = {}
        for stmt in statements:
            if stmt.get("type") == "component":
                ref = stmt["ref"]
                pins = stmt.get("pins", {})
                if ref in comp_pins:
                    # Merge pins from multi-unit components (same ref, different units)
                    comp_pins[ref].update(pins)
                else:
                    comp_pins[ref] = dict(pins)
        
        # Build index: sheet name -> { pin_name -> net_name }
        sheet_pins: Dict[str, Dict[str, str]] = {}
        for stmt in statements:
            if stmt.get("type") == "sheet":
                name = stmt.get("name", "")
                pins: Dict[str, str] = {}
                for pin_name, pin_value in stmt.get("ins", {}).items():
                    net = pin_value.get("net", pin_name) if isinstance(pin_value, dict) else (pin_value if isinstance(pin_value, str) else pin_name)
                    pins[pin_name] = net
                for pin_name, pin_value in stmt.get("outs", {}).items():
                    net = pin_value.get("net", pin_name) if isinstance(pin_value, dict) else (pin_value if isinstance(pin_value, str) else pin_name)
                    pins[pin_name] = net
                sheet_pins[name] = pins
        
        # Build index: junction IDs that exist
        junction_ids: set = set()
        for stmt in statements:
            if stmt.get("type") == "junction" and "id" in stmt:
                junction_ids.add(stmt["id"])
        
        # Validate each cwire
        for stmt in statements:
            if stmt.get("type") != "cwire":
                continue
            
            cwire_ref = stmt.get("ref", "<unknown>")
            from_ep = stmt["from"]
            to_ep = stmt["to"]
            
            from_net = self._validate_cwire_endpoint(from_ep, comp_pins, junction_ids, cwire_ref, "from", sheet_pins)
            to_net = self._validate_cwire_endpoint(to_ep, comp_pins, junction_ids, cwire_ref, "to", sheet_pins)
            
            # Check 2: if both endpoints are pins, verify they share the same net
            if (from_ep["type"] == "pin" and to_ep["type"] == "pin"
                    and from_net is not None and to_net is not None):
                # Ignore if either net is NONE/<NO NET> (unassigned)
                if from_net != "NONE" and to_net != "NONE" and from_net != to_net:
                    self.warnings.append(TraceParseWarning(
                        f"cwire {cwire_ref}: pins {from_ep['ref']}.{from_ep['pin']} (net "
                        f"{from_net}) and {to_ep['ref']}.{to_ep['pin']} (net {to_net}) "
                        f"belong to different nets."
                    ))
    
    def _validate_cwire_endpoint(
        self,
        endpoint: Dict[str, Any],
        comp_pins: Dict[str, Dict[Union[str, int], str]],
        junction_ids: set,
        cwire_ref: str,
        direction: str,
        sheet_pins: Optional[Dict[str, Dict[str, str]]] = None
    ) -> Optional[str]:
        """Validate a single cwire endpoint and return its net name (or None).
        
        For pin endpoints: raises TraceParseError if the component/sheet or pin doesn't exist.
        For junction endpoints: validation is skipped for now.
        
        Returns the net name for pin endpoints, or None for junctions.
        """
        if endpoint["type"] == "junction":
            return None
        
        if endpoint["type"] in ("label", "glabel", "hier"):
            return None
        
        # Pin endpoint
        ref = endpoint["ref"]
        pin = endpoint["pin"]
        
        # Check component exists
        if ref in comp_pins:
            # Check pin exists on the component
            pins = comp_pins[ref]
            if pin not in pins:
                # Also try string/int conversion since pin numbers may be stored
                # as int in one place and str in another
                alt_pin = str(pin) if isinstance(pin, int) else None
                if alt_pin is None:
                    try:
                        alt_pin = int(pin)
                    except (ValueError, TypeError):
                        alt_pin = None
                
                if alt_pin is not None and alt_pin in pins:
                    return pins[alt_pin]
                
                available = ", ".join(str(p) for p in pins.keys())
                raise TraceParseError(
                    f"cwire {cwire_ref}: {direction} endpoint references pin "
                    f"'{pin}' on component '{ref}', but that pin does not exist. "
                    f"Available pins: [{available}]"
                )
            
            return pins[pin]
        
        # Check sheet exists (sheets are treated like components for cwire)
        if sheet_pins and ref in sheet_pins:
            pins = sheet_pins[ref]
            pin_str = str(pin)
            if pin_str in pins:
                return pins[pin_str]
            if pin in pins:
                return pins[pin]
            available = ", ".join(str(p) for p in pins.keys())
            raise TraceParseError(
                f"cwire {cwire_ref}: {direction} endpoint references pin "
                f"'{pin}' on sheet '{ref}', but that pin does not exist. "
                f"Available pins: [{available}]"
            )
        
        raise TraceParseError(
            f"cwire {cwire_ref}: {direction} endpoint references "
            f"'{ref}' which does not exist as a component or sheet."
        )
    
    def _validate_sheets(self, statements: List[Dict[str, Any]]) -> None:
        """Validate sheet file existence and hier pin matching."""
        if not self.base_dir:
            return

        for stmt in statements:
            if stmt.get("type") != "sheet":
                continue

            sheet_name = stmt.get("name", "")
            kicad_file = stmt.get("file", "")
            if not kicad_file:
                continue

            trace_sch_name = kicad_file.replace(".kicad_sch", ".trace_sch")
            trace_sch_path = os.path.join(self.base_dir, trace_sch_name)
            kicad_sch_path = os.path.join(self.base_dir, kicad_file)

            if not os.path.exists(trace_sch_path) and not os.path.exists(kicad_sch_path):
                raise TraceParseError(
                    f"sheet '{sheet_name}': referenced file '{kicad_file}' not found. "
                    f"Neither '{trace_sch_name}' nor '{kicad_file}' exists in {self.base_dir}."
                )

            subsheet_hier_pins = self._extract_hier_pins_from_subsheet(trace_sch_path, kicad_sch_path)
            if subsheet_hier_pins is None:
                continue

            all_declared = {**stmt.get("ins", {}), **stmt.get("outs", {})}
            for pin_name in all_declared:
                if pin_name not in subsheet_hier_pins:
                    available = ", ".join(sorted(subsheet_hier_pins))
                    self.warnings.append(TraceParseWarning(
                        f"sheet '{sheet_name}': pin '{pin_name}' does not exist as a "
                        f"hierarchical label in '{trace_sch_name}'. "
                        f"Available hier pins: [{available}]"
                    ))

    def _extract_hier_pins_from_subsheet(
        self, trace_sch_path: str, kicad_sch_path: str
    ) -> Optional[Set[str]]:
        """Extract hierarchical pin names from a subsheet file.
        
        Tries trace_sch first, falls back to kicad_sch.
        Returns a set of hier pin names, or None if neither can be parsed.
        """
        if os.path.exists(trace_sch_path):
            try:
                with open(trace_sch_path, 'r') as f:
                    content = f.read()
                sub_stmts, _ = parse_trace_sch(content)
                return {s["name"] for s in sub_stmts if s.get("type") == "hier"}
            except Exception:
                pass

        if os.path.exists(kicad_sch_path):
            try:
                import sys
                base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
                if base not in sys.path:
                    sys.path.insert(0, base)
                from common.sexp_parser import parse_sexp
                from common.sexp_helpers import find_elements, get_atom_value
                with open(kicad_sch_path, 'r') as f:
                    content = f.read()
                tree = parse_sexp(content)
                hier_labels = find_elements(tree, 'hierarchical_label')
                return {get_atom_value(hl, 1, None) for hl in hier_labels if get_atom_value(hl, 1, None)}
            except Exception:
                pass

        return None

    def _parse_statement(self) -> Optional[Dict[str, Any]]:
        """Parse a statement."""
        self._skip_ws()
        token = self._current_token()
        
        if token.type != TraceLexer.IDENT:
            return None
        
        keyword = token.value
        
        # Skip deprecated metadata statements - these are now hardcoded in the converter
        if keyword in ("kicad_ver", "kicad_gen", "kicad_gen_ver"):
            # Skip the keyword and its value
            self._advance()  # Skip keyword
            self._skip_ws()
            self._advance()  # Skip value (NUMBER or STRING)
            return None  # Return None to indicate no statement was parsed
        elif keyword == "file_uid":
            return self._parse_file_uid_stmt()
        elif keyword == "paper":
            return self._parse_paper_stmt()
        elif keyword == "title_block":
            return self._parse_title_block_stmt()
        elif keyword == "comp":
            return self._parse_component_stmt()
        elif keyword == "net":
            return self._parse_net_stmt()
        elif keyword == "wire":
            return self._parse_wire_stmt()
        elif keyword == "cwire":
            return self._parse_cwire_stmt()
        elif keyword == "cwiredef":
            return self._parse_cwiredef_stmt()
        elif keyword == "label":
            return self._parse_label_stmt()
        elif keyword == "glabel":
            return self._parse_glabel_stmt()
        elif keyword == "hier":
            return self._parse_hier_stmt()
        elif keyword == "sheet":
            return self._parse_sheet_stmt()
        elif keyword == "text":
            return self._parse_text_stmt()
        elif keyword == "junction":
            return self._parse_junction_stmt()
        elif keyword == "noconnect":
            return self._parse_noconnect_stmt()
        elif keyword == "bus":
            return self._parse_bus_stmt()
        elif keyword == "polyline":
            return self._parse_polyline_stmt()
        elif keyword == "rectangle":
            return self._parse_rectangle_stmt()
        elif keyword == "arc":
            return self._parse_arc_stmt()
        elif keyword == "bezier":
            return self._parse_bezier_stmt()
        elif keyword == "circle":
            return self._parse_circle_stmt()
        elif keyword == "text_box":
            return self._parse_text_box_stmt()
        elif keyword == "bus_entry":
            return self._parse_bus_entry_stmt()
        elif keyword == "inst":
            return self._parse_instance_meta_stmt()
        else:
            raise TraceParseError(
                f"Unknown statement type: {keyword}",
                token.line, token.column
            )
    
    def _parse_comment(self) -> Optional[Dict[str, Any]]:
        """Parse a comment statement."""
        if self._current_token().type != TraceLexer.COMMENT:
            return None
        
        token = self._current_token()
        self._advance()
        
        return {
            "type": "comment",
            "text": token.value
        }
    
    # Helper parsers
    
    def _parse_coord(self) -> Tuple[float, float]:
        """Parse a coordinate: NUMBER , NUMBER"""
        x = self._expect(TraceLexer.NUMBER).value
        self._expect(TraceLexer.COMMA)
        y = self._expect(TraceLexer.NUMBER).value
        return (float(x), float(y))
    
    def _parse_point(self) -> Tuple[float, float]:
        """Parse a point (which is just a coord)."""
        return self._parse_coord()
    
    def _parse_wire_points(self) -> List[Tuple[float, float]]:
        """Parse wire_points: point { WS? , "->" , WS? , point }"""
        points = [self._parse_point()]
        
        while True:
            self._skip_ws()
            if not self._optional(TraceLexer.ARROW):
                break
            
            self._skip_ws()
            points.append(self._parse_point())
        
        return points
    
    def _parse_property(self) -> Dict[str, str]:
        """Parse property: "prop" , WS , STRING , WS , ":" , WS , STRING"""
        self._expect(TraceLexer.IDENT, "prop")
        self._skip_ws()
        key_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        self._expect(TraceLexer.COLON)
        self._skip_ws()
        value_token = self._expect(TraceLexer.STRING)
        
        return {key_token.value: value_token.value}
    
    def _parse_prop_list(self) -> Dict[str, str]:
        """Parse prop_list: "{" , WS? , { property , [ WS? , "," , WS? ] } , WS? , "}" """
        self._expect(TraceLexer.LBRACE)
        self._skip_ws()
        
        props = {}
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RBRACE):
                break
            
            prop = self._parse_property()
            props.update(prop)
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RBRACE):
                break
        
        return props
    
    def _parse_pin_assign(self) -> Tuple[Union[str, int], str, Optional[Tuple[float, float]], Optional[int]]:
        """Parse pin_assign: ( IDENT | NUMBER ) , "=" , ident , [ WS , "@" , WS , coord ]"""
        self._skip_ws()
        
        # Pin number can be IDENT or NUMBER
        pin_token = self._current_token()
        if pin_token.type == TraceLexer.NUMBER:
            pin_num = int(pin_token.value)
            self._advance()
        elif pin_token.type == TraceLexer.IDENT:
            pin_num = pin_token.value
            self._advance()
        else:
            raise TraceParseError(
                f"Expected NUMBER or IDENT for pin number, got {pin_token.type}",
                pin_token.line, pin_token.column
            )
        
        self._skip_ws()
        self._expect(TraceLexer.EQUALS)
        self._skip_ws()
        
        # Check if net name is angle-bracketed (e.g., <NO NET>)
        net_name = None
        if self._optional(TraceLexer.LT):
            # Parse angle-bracketed identifier: <content>
            self._skip_ws()
            # Collect tokens until we find >
            angle_bracket_content = []
            while True:
                token = self._current_token()
                if token.type == TraceLexer.GT:
                    self._advance()  # Consume >
                    break
                elif token.type == TraceLexer.EOF:
                    raise TraceParseError(
                        "Unclosed angle bracket in net name",
                        token.line, token.column
                    )
                else:
                    if token.type == TraceLexer.WS:
                        angle_bracket_content.append(' ')
                    else:
                        angle_bracket_content.append(str(token.value))
                    self._advance()
            # Join and normalize: <NO NET> -> "NO NET", then normalize to "NONE"
            net_name = ''.join(angle_bracket_content).strip()
            # Normalize common variations
            if net_name.upper() == "NO NET":
                net_name = "NONE"
            self._skip_ws()
        else:
            # Regular identifier
            net_token = self._expect(TraceLexer.IDENT)
            net_name = net_token.value
            self._skip_ws()
        
        # Optional @ coord
        coord = None
        rot = None
        if self._optional(TraceLexer.AT):
            self._skip_ws()
            coord = self._parse_coord()
            self._skip_ws()
            # Optional rot after coord
            if self._optional(TraceLexer.IDENT, "rot"):
                self._skip_ws()
                rot_token = self._expect(TraceLexer.NUMBER)
                rot = int(rot_token.value)
        
        return (pin_num, net_name, coord, rot)
    
    def _parse_pin_list(self) -> Dict[Union[str, int], str]:
        """Parse pin_list: "(" , WS? , pin_assign , { WS? , "," , WS? , pin_assign } , WS? , ")" """
        self._expect(TraceLexer.LPAREN)
        self._skip_ws()
        
        pins = {}
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RPAREN):
                break
            
            pin_num, net_name, coord, rot = self._parse_pin_assign()
            # For component pins, ignore coordinates (they're calculated from component position)
            pins[pin_num] = net_name
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RPAREN):
                break
        
        return pins
    
    def _parse_pinmap(self) -> Dict[Union[str, int], Union[str, Dict[str, Any]]]:
        """Parse pinmap: "{" , WS? , { pin_assign , [ WS? , "," , WS? ] } , WS? , "}" """
        self._expect(TraceLexer.LBRACE)
        self._skip_ws()
        
        pins = {}
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RBRACE):
                break
            
            pin_num, net_name, coord, rot = self._parse_pin_assign()
            # For sheet pins, store coordinates if present
            if coord is not None:
                pin_info = {"net": net_name, "at": coord}
                if rot is not None:
                    pin_info["rot"] = rot
                pins[pin_num] = pin_info
            else:
                # No coordinates - use simple string format for backward compatibility
                pins[pin_num] = net_name
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RBRACE):
                break
        
        return pins
    
    # Statement parsers
    
    def _parse_component_stmt(self) -> Dict[str, Any]:
        """Parse component_stmt"""
        self._expect(TraceLexer.IDENT, "comp")
        self._skip_ws()
        
        ref_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        
        symbol_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        
        # Optional comp_value
        value = None
        if self._current_token().type in (TraceLexer.STRING, TraceLexer.IDENT, TraceLexer.NUMBER):
            value_token = self._current_token()
            value = value_token.value
            self._advance()
            self._skip_ws()
        
        # Optional @ coord
        coord = None
        if self._optional(TraceLexer.AT):
            self._skip_ws()
            coord = self._parse_coord()
            self._skip_ws()
        
        # Optional rot NUMBER
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = int(rot_token.value)
            self._skip_ws()
        
        # Optional mirror ("x" | "y")
        mirror = None
        if self._optional(TraceLexer.IDENT, "mirror"):
            self._skip_ws()
            mirror_token = self._expect(TraceLexer.IDENT)
            if mirror_token.value not in ("x", "y"):
                raise TraceParseError(f"mirror must be 'x' or 'y', got '{mirror_token.value}'", mirror_token)
            mirror = mirror_token.value
            self._skip_ws()
        
        # Optional unit NUMBER (defaults to 1)
        unit = 1
        if self._optional(TraceLexer.IDENT, "unit"):
            self._skip_ws()
            unit_token = self._expect(TraceLexer.NUMBER)
            unit = int(unit_token.value)
            if unit < 1:
                raise TraceParseError(f"unit must be >= 1, got {unit}", unit_token)
            self._skip_ws()
        
        # Optional body_style NUMBER (defaults to 1)
        body_style = 1
        if self._optional(TraceLexer.IDENT, "body_style"):
            self._skip_ws()
            body_style_token = self._expect(TraceLexer.NUMBER)
            body_style = int(body_style_token.value)
            if body_style < 1:
                raise TraceParseError(f"body_style must be >= 1, got {body_style}", body_style_token)
            self._skip_ws()
        
        # Optional props prop_list
        props = None
        if self._optional(TraceLexer.IDENT, "props"):
            self._skip_ws()
            props = self._parse_prop_list()
            self._skip_ws()
        
        # Required pins pin_list
        self._expect(TraceLexer.IDENT, "pins")
        self._skip_ws()
        pins = self._parse_pin_list()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "component",
            "ref": ref_token.value,
            "symbol": symbol_token.value,
            "pins": pins,
            "uid": uid_token.value,
            "unit": unit,
            "body_style": body_style
        }
        
        if value is not None:
            result["value"] = value
        if coord is not None:
            result["at"] = coord
        if rot is not None:
            result["rot"] = rot
        if mirror is not None:
            result["mirror"] = mirror
        if props is not None:
            result["props"] = props
        
        return result
    
    def _parse_net_stmt(self) -> List[Dict[str, Any]]:
        """Parse net_stmt: "net" , WS , ident , { WS , ident }"""
        self._expect(TraceLexer.IDENT, "net")
        self._skip_ws()
        
        nets = []
        # Parse first required identifier
        name_token = self._expect(TraceLexer.IDENT)
        nets.append({
            "type": "net",
            "name": name_token.value
        })
        
        # Parse additional optional identifiers
        while True:
            self._skip_ws()
            token = self._current_token()
            # Stop if not an IDENT, or if it's a keyword (start of next statement)
            if token.type != TraceLexer.IDENT or token.value in TraceLexer.KEYWORDS:
                break
            name_token = token
            self._advance()
            nets.append({
                "type": "net",
                "name": name_token.value
            })
        
        return nets
    
    def _parse_wire_stmt(self) -> Union[Dict[str, Any], List[Dict[str, Any]]]:
        """Parse wire_stmt: "wire" , WS , wire_points , WS , "uid" , WS , UUID
        
        If the wire has more than 2 points, splits it into multiple wires
        with 2 coordinates each (chain of connected wires) and adds junctions
        at the connection points to show they are connected.
        """
        self._expect(TraceLexer.IDENT, "wire")
        self._skip_ws()
        points = self._parse_wire_points()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        # If we have 2 or fewer points, return a single wire
        if len(points) <= 2:
            return {
                "type": "wire",
                "points": points,
                "uid": uid_token.value
            }
        
        # Split into multiple wires with 2 coordinates each
        statements = []
        for i in range(len(points) - 1):
            statements.append({
                "type": "wire",
                "points": [points[i], points[i + 1]],
                "uid": str(uuid.uuid4())
            })
        
        # Add junctions at connection points (all points except first and last)
        for i in range(1, len(points) - 1):
            statements.append({
                "type": "junction",
                "at": points[i],
                "uid": str(uuid.uuid4())
            })
        
        return statements
    
    def _parse_endpoint(self) -> Dict[str, Any]:
        """Parse endpoint: pin_ref | junction_ref | label_ref
        
        pin_ref = IDENT , "." , ( IDENT | NUMBER )   e.g., C1.1, U1.VCC
        junction_ref = "JUNC" , NUMBER               e.g., JUNC1, JUNC2
        label_ref = "LBL" , NUMBER                   e.g., LBL1, LBL2
        glabel_ref = "GLBL" , NUMBER                 e.g., GLBL1, GLBL2
        hier_ref = "HIER" , NUMBER                   e.g., HIER1, HIER2
        """
        self._skip_ws()
        token = self._current_token()
        
        if token.type != TraceLexer.IDENT:
            raise TraceParseError(
                f"Expected IDENT for endpoint, got {token.type}",
                token.line, token.column
            )
        
        # Check if this is a junction reference (starts with JUNC)
        if token.value.startswith("JUNC"):
            junction_id = token.value
            self._advance()
            return {
                "type": "junction",
                "id": junction_id
            }
        
        # Check if this is a label reference (starts with LBL but not GLBL)
        if token.value.startswith("LBL"):
            label_id = token.value
            self._advance()
            return {
                "type": "label",
                "id": label_id
            }
        
        # Check if this is a global label reference (starts with GLBL)
        if token.value.startswith("GLBL"):
            glabel_id = token.value
            self._advance()
            return {
                "type": "glabel",
                "id": glabel_id
            }
        
        # Check if this is a hierarchical label reference (starts with HIER)
        if token.value.startswith("HIER"):
            hier_id = token.value
            self._advance()
            return {
                "type": "hier",
                "id": hier_id
            }
        
        # Otherwise, it's a pin reference: REF.PIN
        ref = token.value
        self._advance()
        
        # Expect a dot - but the lexer may have included it in the identifier
        # Check if the ref contains a dot
        if "." in ref:
            # The lexer included the dot and pin in the identifier
            parts = ref.split(".", 1)
            ref = parts[0]
            pin = parts[1]
            # Try to convert pin to int if it's numeric
            try:
                pin = int(pin)
            except ValueError:
                pass  # Keep as string (pin name like VCC)
            return {
                "type": "pin",
                "ref": ref,
                "pin": pin
            }
        
        # If no dot in the identifier, we need to parse it separately
        # This shouldn't happen with the current lexer, but handle it anyway
        self._skip_ws()
        token = self._current_token()
        if token.type == TraceLexer.IDENT and token.value.startswith("."):
            # Dot was tokenized separately
            pin_str = token.value[1:]  # Remove the leading dot
            self._advance()
        else:
            raise TraceParseError(
                f"Expected '.' after component reference in pin_ref, got {token.type}",
                token.line, token.column
            )
        
        # Try to convert pin to int if it's numeric
        try:
            pin = int(pin_str)
        except ValueError:
            pin = pin_str  # Keep as string (pin name like VCC)
        
        return {
            "type": "pin",
            "ref": ref,
            "pin": pin
        }
    
    def _parse_cwire_stmt(self) -> Dict[str, Any]:
        """Parse cwire_stmt: "cwire" , WS , cwire_ref , WS , endpoint , WS , "->" , WS , endpoint , [ WS , "net" , WS , ident ]
        
        cwire connects pins and/or junctions. Examples:
        - cwire CW1 C1.1 -> R1.2                    (pin to pin)
        - cwire CW2 C1.1 -> JUNC1 net OUTPUT        (pin to junction)
        - cwire CW3 JUNC1 -> JUNC2 net OUTPUT       (junction to junction)
        """
        self._expect(TraceLexer.IDENT, "cwire")
        self._skip_ws()
        
        # Parse cwire_ref (CW1, CW2, etc.)
        cwire_ref = self._parse_cwire_ref()
        self._skip_ws()
        
        # Parse 'from' endpoint
        from_endpoint = self._parse_endpoint()
        self._skip_ws()
        
        # Expect arrow
        self._expect(TraceLexer.ARROW)
        self._skip_ws()
        
        # Parse 'to' endpoint
        to_endpoint = self._parse_endpoint()
        self._skip_ws()
        
        # Optional net clause (required if either endpoint is a junction)
        net_name = None
        if self._optional(TraceLexer.IDENT, "net"):
            self._skip_ws()
            net_token = self._expect(TraceLexer.IDENT)
            net_name = net_token.value
        
        result = {
            "type": "cwire",
            "ref": cwire_ref,
            "from": from_endpoint,
            "to": to_endpoint
        }
        
        if net_name:
            result["net"] = net_name
        
        return result
    
    def _parse_cwire_ref(self) -> str:
        """Parse cwire_ref: "CW" , NUMBER (e.g., CW1, CW2)"""
        token = self._current_token()
        
        if token.type != TraceLexer.IDENT:
            raise TraceParseError(
                f"Expected cwire reference (CW1, CW2, etc.), got {token.type}",
                token.line, token.column
            )
        
        if not token.value.startswith("CW"):
            raise TraceParseError(
                f"Expected cwire reference starting with 'CW', got {token.value}",
                token.line, token.column
            )
        
        cwire_ref = token.value
        self._advance()
        return cwire_ref
    
    def _parse_cwiredef_stmt(self) -> Dict[str, Any]:
        """Parse cwiredef_stmt: "cwiredef" , WS , cwire_ref , WS , wire_points
        
        cwiredef defines the wire path for a cwire. Example:
        - cwiredef CW1 100,50 -> 120,50 -> 120,80
        """
        self._expect(TraceLexer.IDENT, "cwiredef")
        self._skip_ws()
        
        # Parse cwire_ref (CW1, CW2, etc.)
        cwire_ref = self._parse_cwire_ref()
        self._skip_ws()
        
        # Parse wire points
        points = self._parse_wire_points()
        
        return {
            "type": "cwiredef",
            "ref": cwire_ref,
            "points": points
        }
    
    def _parse_label_stmt(self) -> Dict[str, Any]:
        """Parse label_stmt: "label" , WS , ( STRING | ident ) , WS , "@" , WS , coord"""
        self._expect(TraceLexer.IDENT, "label")
        self._skip_ws()
        
        # Label name can be STRING or IDENT
        name_token = self._current_token()
        if name_token.type == TraceLexer.STRING:
            self._advance()
        elif name_token.type == TraceLexer.IDENT:
            self._advance()
        else:
            raise TraceParseError(
                f"Expected STRING or IDENT for label name, got {name_token.type}",
                name_token.line, name_token.column
            )
        
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        
        self._skip_ws()
        label_id = None
        if self._optional(TraceLexer.IDENT, "id"):
            self._skip_ws()
            id_token = self._expect(TraceLexer.IDENT)
            label_id = id_token.value
        
        result = {
            "type": "label",
            "name": name_token.value,
            "at": coord
        }
        if label_id:
            result["id"] = label_id
        return result
    
    def _parse_glabel_stmt(self) -> Dict[str, Any]:
        """Parse glabel_stmt: "glabel" , WS , ident , WS , "@" , WS , coord , [ WS , "rot" , WS , NUMBER ] , [ WS , "shape" , WS , ident ] , [ WS , prop_list ]"""
        self._expect(TraceLexer.IDENT, "glabel")
        self._skip_ws()
        name_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        
        # Optional rot NUMBER
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = int(rot_token.value)
            self._skip_ws()
        
        # Optional shape ident
        shape = None
        if self._optional(TraceLexer.IDENT, "shape"):
            self._skip_ws()
            shape_token = self._expect(TraceLexer.IDENT)
            if shape_token.value not in VALID_LABEL_SHAPES:
                raise TraceParseError(
                    f"shape must be one of {sorted(VALID_LABEL_SHAPES)}, got '{shape_token.value}'",
                    shape_token.line, shape_token.column
                )
            shape = shape_token.value
            self._skip_ws()
        
        # Optional prop_list
        props = None
        if self._current_token().type == TraceLexer.LBRACE:
            props = self._parse_prop_list()
        
        self._skip_ws()
        glabel_id = None
        if self._optional(TraceLexer.IDENT, "id"):
            self._skip_ws()
            id_token = self._expect(TraceLexer.IDENT)
            glabel_id = id_token.value
        
        result = {
            "type": "glabel",
            "name": name_token.value,
            "at": coord
        }
        
        if rot is not None:
            result["rot"] = rot
        if shape is not None:
            result["shape"] = shape
        if props is not None:
            result["props"] = props
        if glabel_id:
            result["id"] = glabel_id
        
        return result
    
    def _parse_hier_stmt(self) -> Dict[str, Any]:
        """Parse hier_stmt: "hier" , WS , ident , WS , "@" , WS , coord , [ WS , "rot" , WS , NUMBER ] , [ WS , "shape" , WS , ident ] , [ WS , prop_list ]"""
        self._expect(TraceLexer.IDENT, "hier")
        self._skip_ws()
        name_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        
        # Optional rot NUMBER
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = int(rot_token.value)
            self._skip_ws()
        
        # Optional shape ident
        shape = None
        if self._optional(TraceLexer.IDENT, "shape"):
            self._skip_ws()
            shape_token = self._expect(TraceLexer.IDENT)
            if shape_token.value not in VALID_LABEL_SHAPES:
                raise TraceParseError(
                    f"shape must be one of {sorted(VALID_LABEL_SHAPES)}, got '{shape_token.value}'",
                    shape_token.line, shape_token.column
                )
            shape = shape_token.value
            self._skip_ws()
        
        # Optional prop_list
        props = None
        if self._current_token().type == TraceLexer.LBRACE:
            props = self._parse_prop_list()
        
        self._skip_ws()
        hier_id = None
        if self._optional(TraceLexer.IDENT, "id"):
            self._skip_ws()
            id_token = self._expect(TraceLexer.IDENT)
            hier_id = id_token.value
        
        result = {
            "type": "hier",
            "name": name_token.value,
            "at": coord
        }
        
        if rot is not None:
            result["rot"] = rot
        if shape is not None:
            result["shape"] = shape
        if props is not None:
            result["props"] = props
        if hier_id:
            result["id"] = hier_id
        
        return result
    
    def _parse_sheet_stmt(self) -> Dict[str, Any]:
        """Parse sheet_stmt: "sheet" , WS , ident , WS , "file" , WS , ( STRING | ident ) , [ WS , "@" , WS , coord ] , [ WS , "rot" , WS , NUMBER ] , [ WS , "size" , WS , coord ] , [ WS , "page" , WS , ( STRING | NUMBER ) ] , [ WS , prop_list ] , [ WS , "ins" , WS , pinmap ] , [ WS , "outs" , WS , pinmap ] , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "sheet")
        self._skip_ws()
        name_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "file")
        self._skip_ws()
        
        # file can be STRING or ident
        file_token = self._current_token()
        if file_token.type == TraceLexer.STRING:
            file_value = file_token.value
            self._advance()
        elif file_token.type == TraceLexer.IDENT:
            file_value = file_token.value
            self._advance()
        else:
            raise TraceParseError(
                f"Expected STRING or IDENT for file, got {file_token.type}",
                file_token.line, file_token.column
            )
        
        self._skip_ws()
        
        # Optional @ coord
        coord = None
        if self._optional(TraceLexer.AT):
            self._skip_ws()
            coord = self._parse_coord()
            self._skip_ws()
        
        # Optional rot NUMBER
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = int(rot_token.value)
            self._skip_ws()
        
        # Optional size coord
        size = None
        if self._optional(TraceLexer.IDENT, "size"):
            self._skip_ws()
            size = self._parse_coord()
            self._skip_ws()
        
        # Optional page (STRING or NUMBER) - page numbers can be "2A", "3B", etc.
        page = None
        if self._optional(TraceLexer.IDENT, "page"):
            self._skip_ws()
            page_token = self._current_token()
            if page_token.type == TraceLexer.STRING:
                page = page_token.value
                self._advance()
            elif page_token.type == TraceLexer.NUMBER:
                page = str(int(page_token.value))
                self._advance()
            else:
                raise TraceParseError(
                    f"Expected STRING or NUMBER for page, got {page_token.type}",
                    page_token.line, page_token.column
                )
            self._skip_ws()
        
        # Optional prop_list
        props = None
        if self._current_token().type == TraceLexer.LBRACE:
            props = self._parse_prop_list()
            self._skip_ws()
        
        # Optional ins pinmap
        ins = None
        if self._optional(TraceLexer.IDENT, "ins"):
            self._skip_ws()
            ins = self._parse_pinmap()
            self._skip_ws()
        
        # Optional outs pinmap
        outs = None
        if self._optional(TraceLexer.IDENT, "outs"):
            self._skip_ws()
            outs = self._parse_pinmap()
            self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "sheet",
            "name": name_token.value,
            "file": file_value,
            "uid": uid_token.value
        }
        
        if coord is not None:
            result["at"] = coord
        if rot is not None:
            result["rot"] = rot
        if size is not None:
            result["size"] = size
        if page is not None:
            result["page"] = page
        if props is not None:
            result["props"] = props
        if ins is not None:
            result["ins"] = ins
        if outs is not None:
            result["outs"] = outs
        
        return result
    
    def _parse_text_stmt(self) -> Dict[str, Any]:
        """Parse text_stmt: "text" , WS , STRING , [ WS , "@" , WS , coord ] , [ WS , prop_list ]"""
        self._expect(TraceLexer.IDENT, "text")
        self._skip_ws()
        string_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        
        coord = None
        if self._optional(TraceLexer.AT):
            self._skip_ws()
            coord = self._parse_coord()
            self._skip_ws()
        
        props = None
        if self._current_token().type == TraceLexer.LBRACE:
            props = self._parse_prop_list()
        
        result = {
            "type": "text",
            "string": string_token.value
        }
        
        if coord is not None:
            result["at"] = coord
        if props is not None:
            result["props"] = props
        
        return result
    
    def _parse_junction_stmt(self) -> Dict[str, Any]:
        """Parse junction_stmt: "junction" , WS , [ junction_ref , WS ] , "@" , WS , coord , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "junction")
        self._skip_ws()
        
        # Check for optional junction_ref (e.g., JUNC1)
        junction_id = None
        token = self._current_token()
        if token.type == TraceLexer.IDENT and token.value.startswith("JUNC"):
            junction_id = token.value
            self._advance()
            self._skip_ws()
        
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "junction",
            "at": coord,
            "uid": uid_token.value
        }
        
        if junction_id:
            result["id"] = junction_id
        
        return result
    
    def _parse_noconnect_stmt(self) -> Dict[str, Any]:
        """Parse noconnect_stmt: "noconnect" , WS , "@" , WS , coord , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "noconnect")
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "noconnect",
            "at": coord,
            "uid": uid_token.value
        }
    
    def _parse_bus_stmt(self) -> Dict[str, Any]:
        """Parse bus_stmt: "bus" , WS , wire_points , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "bus")
        self._skip_ws()
        points = self._parse_wire_points()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "bus",
            "points": points,
            "uid": uid_token.value
        }
    
    def _parse_polyline_stmt(self) -> Dict[str, Any]:
        """Parse polyline_stmt: "polyline" , WS , wire_points , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "polyline")
        self._skip_ws()
        points = self._parse_wire_points()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "polyline",
            "points": points,
            "uid": uid_token.value
        }
    
    # Note: _parse_kicad_ver_stmt, _parse_kicad_gen_stmt, _parse_kicad_gen_ver_stmt
    # have been removed - these metadata values are now hardcoded in the converter
    
    def _parse_file_uid_stmt(self) -> Dict[str, Any]:
        """Parse file_uid_stmt: "file_uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "file_uid")
        self._skip_ws()
        uuid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "file_uid",
            "value": uuid_token.value
        }
    
    def _parse_paper_stmt(self) -> Dict[str, Any]:
        """Parse paper_stmt: "paper" , WS , STRING"""
        self._expect(TraceLexer.IDENT, "paper")
        self._skip_ws()
        paper_token = self._expect(TraceLexer.STRING)
        
        return {
            "type": "paper",
            "value": paper_token.value
        }
    
    def _parse_title_block_stmt(self) -> Dict[str, Any]:
        """Parse title_block_stmt."""
        self._expect(TraceLexer.IDENT, "title_block")
        self._skip_ws()
        
        result = {"type": "title_block"}
        
        if self._optional(TraceLexer.IDENT, "title"):
            self._skip_ws()
            result["title"] = self._expect(TraceLexer.STRING).value
            self._skip_ws()
        
        if self._optional(TraceLexer.IDENT, "date"):
            self._skip_ws()
            result["date"] = self._expect(TraceLexer.STRING).value
            self._skip_ws()
        
        if self._optional(TraceLexer.IDENT, "rev"):
            self._skip_ws()
            result["rev"] = self._expect(TraceLexer.STRING).value
            self._skip_ws()
        
        if self._optional(TraceLexer.IDENT, "company"):
            self._skip_ws()
            result["company"] = self._expect(TraceLexer.STRING).value
            self._skip_ws()
        
        comments = {}
        while self._optional(TraceLexer.IDENT, "comment"):
            self._skip_ws()
            num_token = self._expect(TraceLexer.NUMBER)
            self._skip_ws()
            text_token = self._expect(TraceLexer.STRING)
            comments[int(num_token.value)] = text_token.value
            self._skip_ws()
        
        if comments:
            result["comments"] = comments
        
        return result
    
    def _parse_stroke(self) -> Dict[str, Any]:
        """Parse stroke: "stroke" , WS , "width" , WS , NUMBER , WS , "type" , WS , STRING"""
        self._expect(TraceLexer.IDENT, "stroke")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "width")
        self._skip_ws()
        width_token = self._expect(TraceLexer.NUMBER)
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "type")
        self._skip_ws()
        type_token = self._expect(TraceLexer.STRING)
        
        return {
            "width": float(width_token.value) if isinstance(width_token.value, float) else float(width_token.value),
            "type": type_token.value
        }
    
    def _parse_fill(self) -> Dict[str, Any]:
        """Parse fill: "fill" , WS , "type" , WS , STRING , [ WS , "color" , WS , NUMBER , WS , NUMBER , WS , NUMBER , WS , NUMBER ]"""
        self._expect(TraceLexer.IDENT, "fill")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "type")
        self._skip_ws()
        type_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        
        result = {
            "type": type_token.value
        }
        
        # Optional color
        if self._optional(TraceLexer.IDENT, "color"):
            self._skip_ws()
            r_token = self._expect(TraceLexer.NUMBER)
            self._skip_ws()
            g_token = self._expect(TraceLexer.NUMBER)
            self._skip_ws()
            b_token = self._expect(TraceLexer.NUMBER)
            self._skip_ws()
            a_token = self._expect(TraceLexer.NUMBER)
            result["color"] = [
                float(r_token.value) if isinstance(r_token.value, float) else float(r_token.value),
                float(g_token.value) if isinstance(g_token.value, float) else float(g_token.value),
                float(b_token.value) if isinstance(b_token.value, float) else float(b_token.value),
                float(a_token.value) if isinstance(a_token.value, float) else float(a_token.value)
            ]
        
        return result
    
    def _parse_rectangle_stmt(self) -> Dict[str, Any]:
        """Parse rectangle_stmt: "rectangle" , WS , "start" , WS , coord , WS , "end" , WS , coord , WS , stroke , WS , fill , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "rectangle")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start_coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end_coord = self._parse_coord()
        self._skip_ws()
        stroke = self._parse_stroke()
        self._skip_ws()
        fill = self._parse_fill()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "rectangle",
            "start": start_coord,
            "end": end_coord,
            "stroke": stroke,
            "fill": fill,
            "uid": uid_token.value
        }
    
    def _parse_arc_stmt(self) -> Dict[str, Any]:
        """Parse arc_stmt: "arc" , WS , "start" , WS , coord , WS , "mid" , WS , coord , WS , "end" , WS , coord , WS , stroke , WS , fill , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "arc")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start_coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "mid")
        self._skip_ws()
        mid_coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end_coord = self._parse_coord()
        self._skip_ws()
        stroke = self._parse_stroke()
        self._skip_ws()
        fill = self._parse_fill()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "arc",
            "start": start_coord,
            "mid": mid_coord,
            "end": end_coord,
            "stroke": stroke,
            "fill": fill,
            "uid": uid_token.value
        }
    
    def _parse_bezier_stmt(self) -> Dict[str, Any]:
        """Parse bezier_stmt: "bezier" , WS , wire_points , WS , stroke , WS , fill , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "bezier")
        self._skip_ws()
        points = self._parse_wire_points()
        self._skip_ws()
        stroke = self._parse_stroke()
        self._skip_ws()
        fill = self._parse_fill()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "bezier",
            "points": points,
            "stroke": stroke,
            "fill": fill,
            "uid": uid_token.value
        }
    
    def _parse_circle_stmt(self) -> Dict[str, Any]:
        """Parse circle_stmt: "circle" , WS , "center" , WS , coord , WS , "radius" , WS , NUMBER , WS , stroke , WS , fill , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "circle")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "center")
        self._skip_ws()
        center_coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "radius")
        self._skip_ws()
        radius_token = self._expect(TraceLexer.NUMBER)
        self._skip_ws()
        stroke = self._parse_stroke()
        self._skip_ws()
        fill = self._parse_fill()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "circle",
            "center": center_coord,
            "radius": float(radius_token.value) if isinstance(radius_token.value, float) else float(radius_token.value),
            "stroke": stroke,
            "fill": fill,
            "uid": uid_token.value
        }
    
    def _parse_text_box_stmt(self) -> Dict[str, Any]:
        """Parse text_box_stmt: "text_box" , WS , STRING , WS , "@" , WS , coord , [ WS , "rot" , WS , NUMBER ] , WS , "size" , WS , coord , [ WS , "margins" , WS , margins ] , WS , stroke , WS , fill , [ WS , "effects" , WS , prop_list ] , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "text_box")
        self._skip_ws()
        text_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        at_coord = self._parse_coord()
        self._skip_ws()
        
        # Optional rot
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = int(rot_token.value)
            self._skip_ws()
        
        # Required size
        self._expect(TraceLexer.IDENT, "size")
        self._skip_ws()
        size_coord = self._parse_coord()
        self._skip_ws()
        
        # Optional margins
        margins = None
        if self._optional(TraceLexer.IDENT, "margins"):
            self._skip_ws()
            margins = self._parse_margins()
            self._skip_ws()
        
        stroke = self._parse_stroke()
        self._skip_ws()
        fill = self._parse_fill()
        self._skip_ws()
        
        # Optional effects
        effects = None
        if self._optional(TraceLexer.IDENT, "effects"):
            self._skip_ws()
            effects = self._parse_prop_list()
            self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "text_box",
            "text": text_token.value,
            "at": at_coord,
            "size": size_coord,
            "stroke": stroke,
            "fill": fill,
            "uid": uid_token.value
        }
        
        if rot is not None:
            result["rot"] = rot
        if margins is not None:
            result["margins"] = margins
        if effects is not None:
            result["effects"] = effects
        
        return result
    
    def _parse_bus_entry_stmt(self) -> Dict[str, Any]:
        """Parse bus_entry_stmt: "bus_entry" , WS , "@" , WS , coord , WS , "size" , WS , coord , WS , stroke , WS , "uid" , WS , UUID"""
        self._expect(TraceLexer.IDENT, "bus_entry")
        self._skip_ws()
        self._expect(TraceLexer.AT)
        self._skip_ws()
        at_coord = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "size")
        self._skip_ws()
        size_coord = self._parse_coord()
        self._skip_ws()
        stroke = self._parse_stroke()
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "bus_entry",
            "at": at_coord,
            "size": size_coord,
            "stroke": stroke,
            "uid": uid_token.value
        }
    
    def _parse_margins(self) -> List[float]:
        """Parse margins: NUMBER , "," , NUMBER , "," , NUMBER , "," , NUMBER"""
        top_token = self._expect(TraceLexer.NUMBER)
        self._skip_ws()
        self._expect(TraceLexer.COMMA)
        self._skip_ws()
        right_token = self._expect(TraceLexer.NUMBER)
        self._skip_ws()
        self._expect(TraceLexer.COMMA)
        self._skip_ws()
        bottom_token = self._expect(TraceLexer.NUMBER)
        self._skip_ws()
        self._expect(TraceLexer.COMMA)
        self._skip_ws()
        left_token = self._expect(TraceLexer.NUMBER)
        
        return [
            float(top_token.value) if isinstance(top_token.value, float) else float(top_token.value),
            float(right_token.value) if isinstance(right_token.value, float) else float(right_token.value),
            float(bottom_token.value) if isinstance(bottom_token.value, float) else float(bottom_token.value),
            float(left_token.value) if isinstance(left_token.value, float) else float(left_token.value)
        ]
    
    def _parse_instance_meta_stmt(self) -> Dict[str, Any]:
        """Parse instance_meta_stmt: "inst" , WS , "project" , WS , STRING , WS , "path" , WS , STRING , [ WS , "page" , WS , NUMBER ]"""
        self._expect(TraceLexer.IDENT, "inst")
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "project")
        self._skip_ws()
        project_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        self._expect(TraceLexer.IDENT, "path")
        self._skip_ws()
        path_token = self._expect(TraceLexer.STRING)
        self._skip_ws()
        
        page = None
        if self._optional(TraceLexer.IDENT, "page"):
            self._skip_ws()
            page_token = self._expect(TraceLexer.NUMBER)
            page = int(page_token.value)
        
        result = {
            "type": "instance_meta",
            "project": project_token.value,
            "path": path_token.value
        }
        
        if page is not None:
            result["page"] = page
        
        return result


# =============================================================================
# Public API
# =============================================================================

def parse_trace_sch(content: str, base_dir: Optional[str] = None) -> Tuple[List[Dict[str, Any]], List[TraceParseWarning]]:
    """
    Parse a .trace_sch file content into a list of statement dictionaries.
    
    Args:
        content: The file content as a string
        base_dir: Optional directory path for resolving subsheet file references.
                  When provided, enables validation of sheet file existence and
                  hier pin matching. When None, sheet validation is skipped.
        
    Returns:
        Tuple of (statements, warnings):
          - statements: List of dictionaries, each representing a statement
          - warnings: List of TraceParseWarning for critical (non-fatal) issues
        
    Raises:
        TraceParseError: If parsing fails due to a fatal error
    """
    lexer = TraceLexer(content)
    parser = TraceParser(lexer.get_tokens(), base_dir=base_dir)
    statements = parser.parse()
    return statements, parser.warnings


# =============================================================================
# Testing
# =============================================================================

if __name__ == "__main__":
    import sys
    import json
    if len(sys.argv) != 2:
        print("Usage: python trace_parser.py <filename>")
        sys.exit(1)
    filename = sys.argv[1]
    with open(filename, "r") as file:
        test_content = file.read()
    
    try:
        statements, warnings = parse_trace_sch(test_content)
        print(json.dumps(statements, indent=2))
        if warnings:
            print("\n--- WARNINGS ---", file=sys.stderr)
            for w in warnings:
                print(f"  {w}", file=sys.stderr)
    except TraceParseError as e:
        print(f"Parse error: {e}")
