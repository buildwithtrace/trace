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
Trace PCB Parser

A recursive descent parser for .trace_pcb files based on the EBNF grammar
defined in trace_pcb.ebnf. Parses TOON-based PCB format into structured
dictionaries/lists.
"""

import re
import uuid
from typing import List, Dict, Any, Optional, Tuple, Union
from dataclasses import dataclass


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
    
    # Keywords
    KEYWORDS = {
        "footprint", "segment", "track_arc", "via", "zone", "edge",
        "line", "arc", "circle", "rect", "curve", "poly",
        "start", "end", "mid", "center", "width", "layer", "layers",
        "net", "pads", "size", "drill", "polygon", "pts", "rot", "uid",
        "stroke", "fill",
        "kicad_ver", "kicad_gen", "kicad_gen_ver"
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
            
            # Allow - to start an identifier if followed by alphanumeric (e.g., -Pad9)
            if self._current_char() == '-' and self.pos + 1 < len(self.text) and \
               (self.text[self.pos + 1].isalnum() or self.text[self.pos + 1] == '_' or 
                self.text[self.pos + 1] == ':' or self.text[self.pos + 1] == '+' or 
                self.text[self.pos + 1] == '#' or self.text[self.pos + 1] == '/' or
                self.text[self.pos + 1] == '~'):
                self._tokenize_ident()
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
            
            # Identifiers (including those starting with +, like +5V, or #, like #PWR02, or /, like /N1)
            if self._current_char().isalnum() or self._current_char() == '_' or self._current_char() == '+' or self._current_char() == '#' or self._current_char() == '/':
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
            # Otherwise, it's a regular identifier (may contain +, dashes, colons, underscores, decimal points, #, /, ~)
            # Continue reading to allow these characters
            while self.pos < len(self.text):
                char = self._current_char()
                if char.isalnum() or char == '_' or char == ':' or char == '-' or char == '+' or char == '.' or char == '#' or char == '/' or char == '~':
                    value += char
                    self._advance()
                else:
                    break
        else:
            # Regular identifier starting with +, _, :, -, #, /, or ~ (allows +, dashes, colons, underscores, decimal points, #, /, ~)
            while self.pos < len(self.text):
                char = self._current_char()
                if char.isalnum() or char == '_' or char == ':' or char == '-' or char == '+' or char == '.' or char == '#' or char == '/' or char == '~':
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
    
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.pos = 0
    
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
        """Validate that all UUIDs in statements are unique."""
        seen_uuids: Dict[str, Dict[str, Any]] = {}
        
        for stmt in statements:
            uuid_val = self._extract_uuid_from_statement(stmt)
            if uuid_val:
                if uuid_val in seen_uuids:
                    # Find the line/column info for better error reporting
                    existing_stmt = seen_uuids[uuid_val]
                    existing_type = existing_stmt.get("type", "unknown")
                    current_type = stmt.get("type", "unknown")
                    
                    # Try to get line info from the statement if available
                    existing_ref = existing_stmt.get("ref", existing_stmt.get("net", ""))
                    current_ref = stmt.get("ref", stmt.get("net", ""))
                    
                    raise TraceParseError(
                        f"Duplicate UUID found: {uuid_val}. "
                        f"First occurrence: {existing_type} "
                        f"({existing_ref if existing_ref else 'no ref'}). "
                        f"Second occurrence: {current_type} "
                        f"({current_ref if current_ref else 'no ref'}).",
                        0, 0
                    )
                seen_uuids[uuid_val] = stmt
    
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
                statements.append(stmt)
        
        # Validate UUID uniqueness after parsing all statements
        self._validate_uuid_uniqueness(statements)
        
        return statements
    
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
        elif keyword == "footprint":
            return self._parse_footprint_stmt()
        elif keyword == "segment":
            return self._parse_segment_stmt()
        elif keyword == "track_arc":
            return self._parse_track_arc_stmt()
        elif keyword == "via":
            return self._parse_via_stmt()
        elif keyword == "zone":
            return self._parse_zone_stmt()
        elif keyword == "line":
            return self._parse_line_stmt()
        elif keyword == "arc":
            return self._parse_arc_stmt()
        elif keyword == "circle":
            return self._parse_circle_stmt()
        elif keyword == "rect":
            return self._parse_rect_stmt()
        elif keyword == "curve":
            return self._parse_curve_stmt()
        elif keyword == "poly":
            return self._parse_poly_stmt()
        elif keyword == "edge":
            return self._parse_edge_stmt()
        elif keyword == "text":
            return self._parse_text_stmt()
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
    
    def _parse_edge_points(self) -> List[Tuple[float, float]]:
        """Parse edge_points: point { WS? , "->" , WS? , point }"""
        points = [self._parse_point()]
        
        while True:
            self._skip_ws()
            if not self._optional(TraceLexer.ARROW):
                break
            
            self._skip_ws()
            points.append(self._parse_point())
        
        return points
    
    def _parse_pad_assign(self) -> Tuple[Union[str, int], str]:
        """Parse pad_assign: pad_number , "=" , ident"""
        self._skip_ws()
        
        # Pad number can be IDENT or NUMBER
        pad_token = self._current_token()
        if pad_token.type == TraceLexer.NUMBER:
            pad_num = int(pad_token.value)
            self._advance()
        elif pad_token.type == TraceLexer.IDENT:
            pad_num = pad_token.value
            self._advance()
        else:
            raise TraceParseError(
                f"Expected NUMBER or IDENT for pad number, got {pad_token.type}",
                pad_token.line, pad_token.column
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
            return (pad_num, net_name)
        
        # Parse net name - can contain special characters like ~, (, ), {, }
        # Read tokens until we hit a comma or closing paren
        net_name_parts = []
        paren_depth = 0
        brace_depth = 0
        
        while True:
            token = self._current_token()
            
            # Stop at comma or closing paren (if we're at the same depth level)
            if token.type == TraceLexer.COMMA and paren_depth == 0 and brace_depth == 0:
                break
            if token.type == TraceLexer.RPAREN and paren_depth == 0 and brace_depth == 0:
                break
            
            # Track nesting depth for parentheses and braces
            if token.type == TraceLexer.LPAREN:
                paren_depth += 1
                net_name_parts.append('(')
                self._advance()
            elif token.type == TraceLexer.RPAREN:
                paren_depth -= 1
                net_name_parts.append(')')
                self._advance()
            elif token.type == TraceLexer.LBRACE:
                brace_depth += 1
                net_name_parts.append('{')
                self._advance()
            elif token.type == TraceLexer.RBRACE:
                brace_depth -= 1
                net_name_parts.append('}')
                self._advance()
            elif token.type in (TraceLexer.IDENT, TraceLexer.NUMBER):
                # Include the token value
                net_name_parts.append(str(token.value))
                self._advance()
            elif token.type == TraceLexer.WS:
                # Skip whitespace within net names
                self._advance()
            elif token.type == TraceLexer.EOF:
                raise TraceParseError(
                    "Unexpected end of file while parsing net name",
                    token.line, token.column
                )
            else:
                # Include other single-character tokens (like ~ if it becomes a token, or other operators)
                # For now, treat unexpected tokens as part of the net name by converting to string
                if hasattr(token, 'value') and token.value:
                    net_name_parts.append(str(token.value))
                self._advance()
        
        net_name = ''.join(net_name_parts)
        if not net_name:
            raise TraceParseError(
                "Empty net name",
                token.line, token.column
            )
        
        return (pad_num, net_name)
    
    def _parse_pad_list(self) -> Dict[Union[str, int], str]:
        """Parse pad_list: "(" , WS? , pad_assign , { WS? , "," , WS? , pad_assign } , WS? , ")" """
        self._expect(TraceLexer.LPAREN)
        self._skip_ws()
        
        pads = {}
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RPAREN):
                break
            
            pad_num, net_name = self._parse_pad_assign()
            pads[pad_num] = net_name
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RPAREN):
                break
        
        return pads
    
    def _parse_net_name(self, stop_keywords: List[str] = None) -> str:
        """
        Parse a net name that can contain special characters like ~, (, ), {, }.
        Stops when it encounters one of the stop_keywords (like "uid", "layer").
        """
        if stop_keywords is None:
            stop_keywords = ["uid", "layer"]
        
        net_name_parts = []
        paren_depth = 0
        brace_depth = 0
        
        while True:
            # Peek ahead (skipping whitespace) to see if we hit a stop keyword
            peek_pos = self.pos
            while peek_pos < len(self.tokens) and self.tokens[peek_pos].type == TraceLexer.WS:
                peek_pos += 1
            
            if peek_pos < len(self.tokens):
                peek_token = self.tokens[peek_pos]
                if peek_token.type == TraceLexer.IDENT and peek_token.value in stop_keywords:
                    # Found a stop keyword, we're done
                    break
            
            token = self._current_token()
            
            if token.type == TraceLexer.EOF:
                raise TraceParseError(
                    "Unexpected end of file while parsing net name",
                    token.line, token.column
                )
            
            # Track nesting depth for parentheses and braces
            if token.type == TraceLexer.LPAREN:
                paren_depth += 1
                net_name_parts.append('(')
                self._advance()
            elif token.type == TraceLexer.RPAREN:
                paren_depth -= 1
                net_name_parts.append(')')
                self._advance()
            elif token.type == TraceLexer.LBRACE:
                brace_depth += 1
                net_name_parts.append('{')
                self._advance()
            elif token.type == TraceLexer.RBRACE:
                brace_depth -= 1
                net_name_parts.append('}')
                self._advance()
            elif token.type in (TraceLexer.IDENT, TraceLexer.NUMBER):
                # Include the token value
                net_name_parts.append(str(token.value))
                self._advance()
            elif token.type == TraceLexer.WS:
                # Skip whitespace within net names
                self._advance()
            else:
                # Include other tokens (like operators) as part of the net name
                if hasattr(token, 'value') and token.value:
                    net_name_parts.append(str(token.value))
                self._advance()
        
        net_name = ''.join(net_name_parts)
        if not net_name:
            token = self._current_token()
            raise TraceParseError(
                "Empty net name",
                token.line, token.column
            )
        
        return net_name
    
    def _parse_layer_name(self) -> str:
        """Parse layer_name: IDENT | STRING"""
        self._skip_ws()
        token = self._current_token()
        if token.type == TraceLexer.STRING:
            self._advance()
            return token.value
        elif token.type == TraceLexer.IDENT:
            self._advance()
            return token.value
        else:
            raise TraceParseError(
                f"Expected STRING or IDENT for layer name, got {token.type}",
                token.line, token.column
            )
    
    def _parse_layer_list(self) -> List[str]:
        """Parse layer_list: "(" , WS? , layer_name , { WS? , "," , WS? , layer_name } , WS? , ")" """
        self._expect(TraceLexer.LPAREN)
        self._skip_ws()
        
        layers = []
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RPAREN):
                break
            
            layer_name = self._parse_layer_name()
            layers.append(layer_name)
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RPAREN):
                break
        
        return layers
    
    def _parse_polygon_points(self) -> List[Tuple[float, float]]:
        """Parse polygon_points: "(" , WS? , coord , { WS? , "," , WS? , coord } , WS? , ")" """
        self._expect(TraceLexer.LPAREN)
        self._skip_ws()
        
        points = []
        
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RPAREN):
                break
            
            coord = self._parse_coord()
            points.append(coord)
            
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RPAREN):
                break
        
        return points
    
    # Statement parsers
    
    def _parse_footprint_stmt(self) -> Dict[str, Any]:
        """Parse footprint_stmt"""
        self._expect(TraceLexer.IDENT, "footprint")
        self._skip_ws()
        
        ref_token = self._expect(TraceLexer.IDENT)
        self._skip_ws()
        
        # Parse lib_path which can be either a STRING or an unquoted identifier
        # that can contain commas (e.g., "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2_1x02_P5.00mm_Horizontal")
        self._skip_ws()
        token = self._current_token()
        
        if token.type == TraceLexer.STRING:
            # Footprint name is a quoted string
            lib_path = token.value
            self._advance()
            self._skip_ws()
        else:
            # Footprint name is an unquoted identifier - read tokens until we find @ symbol
            lib_path_parts = []
            while True:
                self._skip_ws()
                # Check if next token is @ (which means we're done with lib_path)
                if self._current_token().type == TraceLexer.AT:
                    break
                # Read IDENT, NUMBER, or COMMA token (numbers can appear after commas in footprint names)
                token = self._current_token()
                if token.type == TraceLexer.IDENT:
                    lib_path_parts.append(str(token.value))
                    self._advance()
                elif token.type == TraceLexer.NUMBER:
                    lib_path_parts.append(str(token.value))
                    self._advance()
                elif token.type == TraceLexer.COMMA:
                    lib_path_parts.append(',')
                    self._advance()
                else:
                    raise TraceParseError(
                        f"Expected STRING, IDENT, NUMBER, or COMMA in footprint name, got {token.type}",
                        token.line, token.column
                    )
            
            lib_path = ''.join(lib_path_parts)
            if not lib_path:
                raise TraceParseError(
                    "Empty footprint name (lib_path)",
                    self._current_token().line, self._current_token().column
                )
            
            self._skip_ws()
        
        # Optional @ coord
        coord = None
        if self._optional(TraceLexer.AT):
            self._skip_ws()
            coord = self._parse_coord()
            self._skip_ws()
        
        # Optional rot NUMBER (can appear before or after layer)
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = float(rot_token.value)
            self._skip_ws()
        
        # Required layer layer_name
        self._expect(TraceLexer.IDENT, "layer")
        self._skip_ws()
        layer_name = self._parse_layer_name()
        self._skip_ws()
        
        # Optional rot NUMBER (if not already parsed, can appear after layer)
        if rot is None and self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = float(rot_token.value)
            self._skip_ws()
        
        # Required pads pad_list
        self._expect(TraceLexer.IDENT, "pads")
        self._skip_ws()
        pads = self._parse_pad_list()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "footprint",
            "ref": ref_token.value,
            "lib_path": lib_path,
            "layer": layer_name,
            "pads": pads,
            "uid": uid_token.value
        }
        
        if coord is not None:
            result["at"] = coord
        if rot is not None:
            result["rot"] = rot
        
        return result
    
    def _parse_segment_stmt(self) -> Dict[str, Any]:
        """Parse segment_stmt"""
        self._expect(TraceLexer.IDENT, "segment")
        self._skip_ws()
        
        # Required start coord
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start_coord = self._parse_coord()
        self._skip_ws()
        
        # Required end coord
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end_coord = self._parse_coord()
        self._skip_ws()
        
        # Required width NUMBER
        self._expect(TraceLexer.IDENT, "width")
        self._skip_ws()
        width_token = self._expect(TraceLexer.NUMBER)
        width = float(width_token.value)
        self._skip_ws()
        
        # Required layer layer_name
        self._expect(TraceLexer.IDENT, "layer")
        self._skip_ws()
        layer_name = self._parse_layer_name()
        self._skip_ws()
        
        # Required net ident
        self._expect(TraceLexer.IDENT, "net")
        self._skip_ws()
        net_name = self._parse_net_name(stop_keywords=["uid"])
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "segment",
            "start": start_coord,
            "end": end_coord,
            "width": width,
            "layer": layer_name,
            "net": net_name,
            "uid": uid_token.value
        }
    
    def _parse_via_stmt(self) -> Dict[str, Any]:
        """Parse via_stmt"""
        self._expect(TraceLexer.IDENT, "via")
        self._skip_ws()
        
        # Required @ coord
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        
        # Required size NUMBER
        self._expect(TraceLexer.IDENT, "size")
        self._skip_ws()
        size_token = self._expect(TraceLexer.NUMBER)
        size = float(size_token.value)
        self._skip_ws()
        
        # Required drill NUMBER
        self._expect(TraceLexer.IDENT, "drill")
        self._skip_ws()
        drill_token = self._expect(TraceLexer.NUMBER)
        drill = float(drill_token.value)
        self._skip_ws()
        
        # Required layers layer_list
        self._expect(TraceLexer.IDENT, "layers")
        self._skip_ws()
        layers = self._parse_layer_list()
        self._skip_ws()
        
        # Optional net ident
        net = None
        if self._optional(TraceLexer.IDENT, "net"):
            self._skip_ws()
            net = self._parse_net_name(stop_keywords=["uid"])
            self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "via",
            "at": coord,
            "size": size,
            "drill": drill,
            "layers": layers,
            "uid": uid_token.value
        }
        
        if net is not None:
            result["net"] = net
        
        return result
    
    def _parse_zone_stmt(self) -> Dict[str, Any]:
        """Parse zone_stmt"""
        self._expect(TraceLexer.IDENT, "zone")
        self._skip_ws()
        
        # Required net ident
        self._expect(TraceLexer.IDENT, "net")
        self._skip_ws()
        net_name = self._parse_net_name(stop_keywords=["layer", "layers"])
        self._skip_ws()
        
        # Required layer or layers
        token = self._current_token()
        if token and token.type == TraceLexer.IDENT and token.value == "layers":
            # Multi-layer zone: layers layer_list
            self._expect(TraceLexer.IDENT, "layers")
            self._skip_ws()
            layers = self._parse_layer_list()
        else:
            # Single-layer zone: layer layer_name
            self._expect(TraceLexer.IDENT, "layer")
            self._skip_ws()
            layer_name = self._parse_layer_name()
            layers = [layer_name]
        self._skip_ws()
        
        # Required polygon polygon_points
        self._expect(TraceLexer.IDENT, "polygon")
        self._skip_ws()
        polygon_points = self._parse_polygon_points()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "zone",
            "net": net_name,
            "layers": layers,  # Always use 'layers' array for consistency
            "polygon": polygon_points,
            "uid": uid_token.value
        }
    
    def _parse_edge_stmt(self) -> Dict[str, Any]:
        """Parse edge_stmt (backward-compatible alias for line on Edge.Cuts)."""
        self._expect(TraceLexer.IDENT, "edge")
        self._skip_ws()
        
        # Required edge_points
        points = self._parse_edge_points()
        self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        return {
            "type": "edge",
            "points": points,
            "uid": uid_token.value
        }
    
    # Shared optional-property helpers
    
    def _parse_optional_layer(self) -> Optional[str]:
        """Parse optional layer property. Returns layer name or None."""
        if self._optional(TraceLexer.IDENT, "layer"):
            self._skip_ws()
            return self._parse_layer_name()
        return None
    
    def _parse_optional_stroke_props(self) -> Tuple[Optional[float], Optional[str]]:
        """Parse optional stroke properties: [width NUMBER] [stroke IDENT]."""
        stroke_width = None
        stroke_type = None
        
        if self._optional(TraceLexer.IDENT, "width"):
            self._skip_ws()
            stroke_width = float(self._expect(TraceLexer.NUMBER).value)
            self._skip_ws()
        
        if self._optional(TraceLexer.IDENT, "stroke"):
            self._skip_ws()
            stroke_type = self._expect(TraceLexer.IDENT).value
            self._skip_ws()
        
        return stroke_width, stroke_type
    
    def _parse_optional_fill(self) -> Optional[str]:
        """Parse optional fill property: fill yes|no."""
        if self._optional(TraceLexer.IDENT, "fill"):
            self._skip_ws()
            fill_token = self._expect(TraceLexer.IDENT)
            return fill_token.value
        return None
    
    def _parse_point_list(self) -> List[Tuple[float, float]]:
        """Parse point_list: '(' coord {',' coord} ')'"""
        self._expect(TraceLexer.LPAREN)
        self._skip_ws()
        
        points = []
        while True:
            self._skip_ws()
            if self._optional(TraceLexer.RPAREN):
                break
            coord = self._parse_coord()
            points.append(coord)
            self._skip_ws()
            if self._optional(TraceLexer.COMMA):
                self._skip_ws()
                continue
            elif self._optional(TraceLexer.RPAREN):
                break
        
        return points
    
    def _build_graphic_result(self, gtype: str, base: Dict[str, Any],
                              layer: Optional[str],
                              stroke_width: Optional[float],
                              stroke_type: Optional[str],
                              fill: Optional[str],
                              uid: str) -> Dict[str, Any]:
        """Build a result dict for a graphical statement with common optional fields."""
        result = {"type": gtype, **base, "uid": uid}
        if layer is not None:
            result["layer"] = layer
        if stroke_width is not None:
            result["stroke_width"] = stroke_width
        if stroke_type is not None:
            result["stroke_type"] = stroke_type
        if fill is not None:
            result["fill"] = fill
        return result
    
    # New graphical-primitive parsers
    
    def _parse_line_stmt(self) -> Dict[str, Any]:
        """Parse line_stmt: line coord -> coord [layer] [stroke_props] uid UUID"""
        self._expect(TraceLexer.IDENT, "line")
        self._skip_ws()
        
        start = self._parse_coord()
        self._skip_ws()
        self._expect(TraceLexer.ARROW)
        self._skip_ws()
        end = self._parse_coord()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "line", {"start": start, "end": end},
            layer, stroke_width, stroke_type, None, uid)
    
    def _parse_arc_stmt(self) -> Dict[str, Any]:
        """Parse arc_stmt: arc start coord mid coord end coord [layer] [stroke_props] uid UUID"""
        self._expect(TraceLexer.IDENT, "arc")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "mid")
        self._skip_ws()
        mid = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end = self._parse_coord()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "arc", {"start": start, "mid": mid, "end": end},
            layer, stroke_width, stroke_type, None, uid)
    
    def _parse_circle_stmt(self) -> Dict[str, Any]:
        """Parse circle_stmt: circle center coord end coord [layer] [stroke_props] [fill] uid UUID"""
        self._expect(TraceLexer.IDENT, "circle")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "center")
        self._skip_ws()
        center = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end = self._parse_coord()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        fill = self._parse_optional_fill()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "circle", {"center": center, "end": end},
            layer, stroke_width, stroke_type, fill, uid)
    
    def _parse_rect_stmt(self) -> Dict[str, Any]:
        """Parse rect_stmt: rect start coord end coord [layer] [stroke_props] [fill] uid UUID"""
        self._expect(TraceLexer.IDENT, "rect")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end = self._parse_coord()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        fill = self._parse_optional_fill()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "rect", {"start": start, "end": end},
            layer, stroke_width, stroke_type, fill, uid)
    
    def _parse_curve_stmt(self) -> Dict[str, Any]:
        """Parse curve_stmt: curve pts (coord,coord,coord,coord) [layer] [stroke_props] uid UUID"""
        self._expect(TraceLexer.IDENT, "curve")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "pts")
        self._skip_ws()
        pts = self._parse_point_list()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "curve", {"pts": pts},
            layer, stroke_width, stroke_type, None, uid)
    
    def _parse_poly_stmt(self) -> Dict[str, Any]:
        """Parse poly_stmt: poly pts (coord,...) [layer] [stroke_props] [fill] uid UUID"""
        self._expect(TraceLexer.IDENT, "poly")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "pts")
        self._skip_ws()
        pts = self._parse_point_list()
        self._skip_ws()
        
        layer = self._parse_optional_layer()
        self._skip_ws()
        stroke_width, stroke_type = self._parse_optional_stroke_props()
        self._skip_ws()
        fill = self._parse_optional_fill()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return self._build_graphic_result(
            "poly", {"pts": pts},
            layer, stroke_width, stroke_type, fill, uid)
    
    def _parse_track_arc_stmt(self) -> Dict[str, Any]:
        """Parse track_arc_stmt: track_arc start coord mid coord end coord width NUMBER layer layer_name net ident uid UUID"""
        self._expect(TraceLexer.IDENT, "track_arc")
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "start")
        self._skip_ws()
        start = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "mid")
        self._skip_ws()
        mid = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "end")
        self._skip_ws()
        end = self._parse_coord()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "width")
        self._skip_ws()
        width = float(self._expect(TraceLexer.NUMBER).value)
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "layer")
        self._skip_ws()
        layer_name = self._parse_layer_name()
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "net")
        self._skip_ws()
        net_name = self._parse_net_name(stop_keywords=["uid"])
        self._skip_ws()
        
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid = self._expect(TraceLexer.UUID).value
        
        return {
            "type": "track_arc",
            "start": start,
            "mid": mid,
            "end": end,
            "width": width,
            "layer": layer_name,
            "net": net_name,
            "uid": uid
        }
    
    def _parse_text_stmt(self) -> Dict[str, Any]:
        """Parse text_stmt"""
        self._expect(TraceLexer.IDENT, "text")
        self._skip_ws()
        
        # Required text STRING
        text_token = self._expect(TraceLexer.STRING)
        text_value = text_token.value
        self._skip_ws()
        
        # Required @ coord
        self._expect(TraceLexer.AT)
        self._skip_ws()
        coord = self._parse_coord()
        self._skip_ws()
        
        # Required layer layer_name
        self._expect(TraceLexer.IDENT, "layer")
        self._skip_ws()
        layer_name = self._parse_layer_name()
        self._skip_ws()
        
        # Optional rot NUMBER
        rot = None
        if self._optional(TraceLexer.IDENT, "rot"):
            self._skip_ws()
            rot_token = self._expect(TraceLexer.NUMBER)
            rot = float(rot_token.value)
            self._skip_ws()
        
        # Optional font_size coord
        font_size = None
        if self._optional(TraceLexer.IDENT, "font_size"):
            self._skip_ws()
            font_size = self._parse_coord()
            self._skip_ws()
        
        # Optional font_thickness NUMBER
        font_thickness = None
        if self._optional(TraceLexer.IDENT, "font_thickness"):
            self._skip_ws()
            thickness_token = self._expect(TraceLexer.NUMBER)
            font_thickness = float(thickness_token.value)
            self._skip_ws()
        
        # Optional justify ident ident
        justify = None
        if self._optional(TraceLexer.IDENT, "justify"):
            self._skip_ws()
            justify_h_token = self._expect(TraceLexer.IDENT)
            justify_h = justify_h_token.value
            if justify_h not in ["left", "right"]:
                raise TraceParseError(
                    f"Invalid horizontal justify value: {justify_h}. Must be 'left' or 'right'",
                    justify_h_token.line, justify_h_token.column
                )
            self._skip_ws()
            justify_v_token = self._expect(TraceLexer.IDENT)
            justify_v = justify_v_token.value
            if justify_v not in ["top", "bottom"]:
                raise TraceParseError(
                    f"Invalid vertical justify value: {justify_v}. Must be 'top' or 'bottom'",
                    justify_v_token.line, justify_v_token.column
                )
            justify = [justify_h, justify_v]
            self._skip_ws()
        
        # Required uid UUID
        self._expect(TraceLexer.IDENT, "uid")
        self._skip_ws()
        uid_token = self._expect(TraceLexer.UUID)
        
        result = {
            "type": "text",
            "text": text_value,
            "at": coord,
            "layer": layer_name,
            "uid": uid_token.value
        }
        
        if rot is not None:
            result["rot"] = rot
        if font_size is not None:
            result["font_size"] = font_size
        if font_thickness is not None:
            result["font_thickness"] = font_thickness
        if justify is not None:
            result["justify"] = justify
        
        return result
    
    # Note: _parse_kicad_ver_stmt, _parse_kicad_gen_stmt, _parse_kicad_gen_ver_stmt
    # have been removed - these metadata values are now hardcoded in the converter


# =============================================================================
# Public API
# =============================================================================

def parse_trace_pcb(content: str) -> List[Dict[str, Any]]:
    """
    Parse a .trace_pcb file content into a list of statement dictionaries.
    
    Args:
        content: The file content as a string
        
    Returns:
        List of dictionaries, each representing a statement
        
    Raises:
        TraceParseError: If parsing fails
    """
    lexer = TraceLexer(content)
    parser = TraceParser(lexer.get_tokens())
    return parser.parse()


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
        statements = parse_trace_pcb(test_content)
        print(json.dumps(statements, indent=2))
    except TraceParseError as e:
        print(f"Parse error: {e}")
