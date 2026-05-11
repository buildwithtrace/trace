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
S-Expression Parser

Fast custom recursive parser for KiCad S-expression files.
Used by both eeschema and pcbnew converters.
"""

from typing import List, Union, Any


str_delimiters = '\'"'


def pairwise(iterable):
    """Iterate in pairs, such that [a, b, c, d, ...] becomes [(a, b), (c, d), ...]"""
    i = iter(iterable)
    return zip(i, i)


def to_number(string):
    try:
        n = int(string)
    except ValueError:
        n = float(string)
    return n


def from_s_atom(obj):
    """Convert S-expression atom to Python value."""
    if not isinstance(obj, str):
        return obj
    
    if not obj:
        return obj
    
    first = obj[0]
    if first.isdigit() or first == '.' or first == '-':
        # Is a number
        try:
            return to_number(obj)
        except ValueError:
            return obj
    elif first in str_delimiters:
        # Is a string literal
        return obj.strip(first)
    else:
        # Regular identifier/atom
        return obj


def from_s_data(obj):
    """Recursively convert S-expression data structure."""
    if isinstance(obj, list):
        # Convert list elements
        return [from_s_data(item) for item in obj]
    else:
        return from_s_atom(obj)


class SexpParser:
    """Fast custom recursive S-expression parser."""
    
    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.length = len(text)
    
    def skip_whitespace(self):
        """Skip whitespace characters."""
        while self.pos < self.length and self.text[self.pos].isspace():
            self.pos += 1
    
    def parse(self) -> Any:
        """Parse the S-expression string."""
        self.skip_whitespace()
        if self.pos >= self.length:
            return None
        
        if self.text[self.pos] == '(':
            return self.parse_list()
        else:
            return self.parse_atom()
    
    def parse_list(self) -> List:
        """Parse a list (parenthesized expression)."""
        self.pos += 1  # Skip '('
        result = []
        
        while True:
            self.skip_whitespace()
            
            if self.pos >= self.length:
                raise ValueError("Unclosed parenthesis")
            
            if self.text[self.pos] == ')':
                self.pos += 1
                break
            
            if self.text[self.pos] == '(':
                result.append(self.parse_list())
            else:
                result.append(self.parse_atom())
        
        return result
    
    def parse_atom(self) -> Union[str, int, float]:
        """Parse an atom (string, number, or identifier)."""
        self.skip_whitespace()
        
        if self.pos >= self.length:
            return ""
        
        # Handle quoted strings
        if self.text[self.pos] == '"':
            return self.parse_quoted_string()
        
        # Handle numbers and identifiers - read until whitespace or closing paren
        start = self.pos
        
        while self.pos < self.length:
            char = self.text[self.pos]
            
            # Stop on whitespace or closing paren
            if char.isspace() or char == ')':
                break
            
            self.pos += 1
        
        atom = self.text[start:self.pos]
        return from_s_atom(atom)
    
    def parse_quoted_string(self) -> str:
        """Parse a quoted string with escape sequences."""
        self.pos += 1  # Skip opening quote
        result = []
        
        while self.pos < self.length:
            char = self.text[self.pos]
            
            if char == '"':
                # Check if it's escaped
                if self.pos > 0 and self.text[self.pos - 1] == '\\':
                    result.append('"')
                    self.pos += 1
                    continue
                else:
                    self.pos += 1
                    break
            
            if char == '\\' and self.pos + 1 < self.length:
                next_char = self.text[self.pos + 1]
                if next_char == 'n':
                    result.append('\n')
                    self.pos += 2
                    continue
                elif next_char == 't':
                    result.append('\t')
                    self.pos += 2
                    continue
                elif next_char == 'r':
                    result.append('\r')
                    self.pos += 2
                    continue
                elif next_char == '\\':
                    result.append('\\')
                    self.pos += 2
                    continue
                elif next_char == '"':
                    result.append('"')
                    self.pos += 2
                    continue
            
            result.append(char)
            self.pos += 1
        
        return ''.join(result)


def parse_sexp(string: str) -> Any:
    """
    Parse a S-expression string into Python objects.
    Uses a fast custom recursive parser.
    """
    parser = SexpParser(string)
    result = parser.parse()
    
    # The custom parser returns the structure directly
    return result
