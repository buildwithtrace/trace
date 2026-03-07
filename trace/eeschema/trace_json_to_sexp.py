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
TRACE-JSON to KiCad S-Expression Converter

Converts trace_sch JSON format back to KiCad .kicad_sch S-expression files.
Loads symbol definitions from KiCad library files and generates proper S-expression output.
"""

import json
import math
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

# No external dependencies - using custom fast parser

# Import net router for intelligent routing
import sys
import os

# Handle both module import and direct script execution
try:
    from .net_router import route_cwires
except (ImportError, ValueError):
    # Fallback for direct script execution - add current directory to path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    try:
        from net_router import route_cwires
    except ImportError:
        logger.error("Error: net_router module not found")
        raise


# =============================================================================
# Constants
# =============================================================================

VALID_LABEL_SHAPES = {"input", "output", "bidirectional", "tri_state", "passive"}


# =============================================================================
# S-Expression Parser (reused from sexp_to_trace_json.py)
# =============================================================================

def from_s_atom(obj):
    """Convert S-expression atom to Python value."""
    if not isinstance(obj, str):
        return obj
    
    if not obj:
        return obj
    
    first = obj[0]
    if first.isdigit() or first == '.' or first == '-':
        try:
            return int(obj) if '.' not in obj else float(obj)
        except ValueError:
            return obj
    elif first in '\'"':
        return obj.strip(first)
    else:
        return obj


def from_s_data(obj):
    """Recursively convert S-expression data structure."""
    if isinstance(obj, list):
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


# =============================================================================
# S-Expression Formatter
# =============================================================================

# Fields whose values should be quoted in S-expressions
# These are the keywords where the value immediately following should be quoted
QUOTED_VALUE_FIELDS = {'name', 'number', 'property', 'symbol', 'uuid', 'label', 'global_label', 'hierarchical_label', 'path', 'generator', 'generator_version', 'paper', 'lib_id', 'lib_name', 'project', 'reference', 'page'}


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
    # Remove all quotes from the string content
    # Also remove escaped quotes (\" becomes empty)
    # Remove backslashes that were used for escaping quotes
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
                if i == 1 and first_elem in QUOTED_VALUE_FIELDS:
                    # Value immediately after a quoted field keyword should be quoted
                    atoms.append(f'"{escape_string(item)}"')
                elif i == 1 and first_elem == 'pin':
                    # Determine pin context:
                    # 1. Sheet pins: (pin "A" input ...) - third element is input/output/bidirectional → quote
                    # 2. Library symbol pins: (pin power_out line ...) - third element is line/inverted/etc. → don't quote
                    # 3. Symbol instance pins: (pin 1 (uuid ...)) - third element is a list or missing → quote
                    is_sheet_pin = False
                    is_library_symbol_pin = False
                    
                    if len(value) > 2:
                        third_elem = value[2]
                        if isinstance(third_elem, str):
                            third_elem_lower = third_elem.lower()
                            if third_elem_lower in ('input', 'output', 'bidirectional'):
                                is_sheet_pin = True
                            elif third_elem_lower in ('line', 'inverted', 'clock', 'inverted_clock', 
                                                      'input_low', 'output_low', 'failing_edge', 
                                                      'non_logic', 'power_in', 'power_out', 
                                                      'passive', 'tri_state', 'open_collector',
                                                      'open_emitter', 'unconnected'):
                                is_library_symbol_pin = True
                        # If third element is a list (like ['uuid', ...]), it's a symbol instance pin
                    
                    if is_sheet_pin:
                        # Sheet pin names should be quoted
                        atoms.append(f'"{escape_string(item)}"')
                    elif is_library_symbol_pin:
                        # Library symbol pin types should NOT be quoted
                        atoms.append(str(item))
                    else:
                        # Symbol instance pins (pin numbers) should be quoted
                        atoms.append(f'"{escape_string(item)}"')
                elif i == 2 and first_elem == 'property':
                    # Second value after 'property' (the property value) should also be quoted
                    atoms.append(f'"{escape_string(item)}"')
                elif i >= 3 and first_elem == 'property' and isinstance(item, str):
                    # Defensive: Skip any extra string arguments after property name and value
                    # Properties should only have 2 strings (name and value), then attributes
                    # This should never happen if properties are created correctly via create_property_element,
                    # but serves as a safety net for malformed data from old files or edge cases
                    continue
                else:
                    # Format as atom
                    formatted_atom = format_sexp_value(item, str(item) if isinstance(item, str) else '', first_elem, indent)
                    atoms.append(formatted_atom)
        
        # Build the formatted string
        # First line: (keyword atom1 atom2 ...)
        first_line = '(' + ' '.join(atoms)
        
        # If there are nested lists, we need multi-line format
        if nested_lists:
            lines = [first_line]
            for nested in nested_lists:
                # Each nested list goes on its own line with proper indentation
                if '\n' in nested:
                    # Multi-line nested list
                    # The nested list was formatted with indent+1, so it has base indent of (indent+1) tabs
                    # We want to add it with base indent of next_indent_str (which is also indent+1 tabs)
                    # So we need to: remove the base indent from the nested list and add next_indent_str
                    nested_lines = nested.split('\n')
                    base_indent_tabs = indent + 1  # The indent level the nested list was formatted with
                    
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
        # Quote if contains spaces or special characters
        if ' ' in value or '\n' in value or '\t' in value or '*' in value or '?' in value or '"' in value:
            return f'"{escape_string(value)}"'
        return value
    
    elif isinstance(value, bool):
        return 'yes' if value else 'no'
    
    elif value is None:
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
# Symbol Library Loader
# =============================================================================

def _find_kicad_symbol_path() -> str:
    """Find KiCad symbols directory by checking multiple common locations."""
    from pathlib import Path
    import sys
    import os
    
    # Windows: Check install and build directories first (where CMake copies libraries)
    if sys.platform == 'win32':
        # Get script directory and navigate to install/build roots
        script_dir = Path(__file__).resolve().parent
        # Navigate up from trace/eeschema to project root
        project_root = script_dir.parent.parent
        
        # Check install directory first (for distributed builds)
        install_candidates = [
            # Relative to executable (most common for installed builds)
            Path(sys.executable).parent.parent / 'share' / 'trace' / 'symbols',
            # Absolute paths for development
            project_root / 'build' / 'install' / 'msvc-win64-release' / 'share' / 'trace' / 'symbols',
            project_root / 'build' / 'install' / 'msvc-win64-debug' / 'share' / 'trace' / 'symbols',
            project_root / 'out' / 'install' / 'x64-Release' / 'share' / 'trace' / 'symbols',
            project_root / 'out' / 'install' / 'x64-Debug' / 'share' / 'trace' / 'symbols',
        ]
        
        for install_path in install_candidates:
            if install_path.is_dir():
                return str(install_path)
        
        # Check build directory (for development builds via FetchContent)
        build_candidates = [
            project_root / 'build' / '_deps' / 'kicad_symbols-src',
            project_root / 'build' / 'msvc-win64-release' / '_deps' / 'kicad_symbols-src',
            project_root / 'build' / 'msvc-win64-debug' / '_deps' / 'kicad_symbols-src',
            project_root / 'out' / 'build' / 'x64-Release' / '_deps' / 'kicad_symbols-src',
            project_root / 'out' / 'build' / 'x64-Debug' / '_deps' / 'kicad_symbols-src',
        ]
        
        for build_path in build_candidates:
            if build_path.is_dir():
                return str(build_path)
    
    # List of common KiCad symbol paths (in order of preference)
    possible_paths = [
        # Developer-specific path (relative to Documents folder, not hardcoded username)
        str(Path.home() / 'Documents' / 'Trace' / 'Trace_packages' / 'kicad-symbols'),
        # macOS system-wide install
        '/Library/Application Support/kicad/symbols',
        # macOS app bundle (various versions)
        '/Applications/Trace.app/Contents/SharedSupport/symbols',
        '/Applications/KiCad.app/Contents/SharedSupport/symbols',
        '/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols',
        # Linux
        '/usr/share/kicad/symbols',
        '/usr/local/share/kicad/symbols',
        # User local (macOS/Linux)
        str(Path.home() / '.local/share/kicad/symbols'),
        str(Path.home() / 'Library/Application Support/kicad/symbols'),
        # Windows - system install (with version subdirectories)
        'C:/Program Files/KiCad/9.0/share/kicad/symbols',
        'C:/Program Files/KiCad/8.0/share/kicad/symbols',
        'C:/Program Files/KiCad/7.0/share/kicad/symbols',
        'C:/Program Files/KiCad/share/kicad/symbols',
        'C:/Program Files (x86)/KiCad/9.0/share/kicad/symbols',
        'C:/Program Files (x86)/KiCad/8.0/share/kicad/symbols',
        'C:/Program Files (x86)/KiCad/share/kicad/symbols',
    ]
    
    # Check KICAD_SYMBOL_DIR environment variable first
    env_path = os.environ.get('KICAD_SYMBOL_DIR')
    if env_path and os.path.isdir(env_path):
        return env_path
    
    # Try each path
    for path in possible_paths:
        if os.path.isdir(path):
            return path
    
    # Fallback to default (will likely fail but matches old behavior)
    return str(Path.home() / 'Documents' / 'Trace' / 'Trace_packages' / 'kicad-symbols')

KICAD_SYMBOL_PATH = _find_kicad_symbol_path()

# Cache for parsed library files (library_name -> parsed_lib_data)
_library_cache = {}


def find_element(sexp_list: List, name: str) -> Optional[List]:
    """Find first element with given name in S-expression list."""
    if not isinstance(sexp_list, list) or len(sexp_list) == 0:
        return None
    for item in sexp_list:
        if isinstance(item, list) and len(item) > 0 and item[0] == name:
            return item
    return None


def find_elements(sexp_list: List, name: str) -> List[List]:
    """Find all elements with given name in S-expression list."""
    results = []
    if not isinstance(sexp_list, list):
        return []
    for item in sexp_list:
        if isinstance(item, list) and len(item) > 0 and item[0] == name:
            results.append(item)
    return results


def get_atom_value(sexp_list: List, index: int = 1, default: Any = None) -> Any:
    """Get atom value from S-expression list at given index."""
    if not isinstance(sexp_list, list) or len(sexp_list) <= index:
        return default
    value = sexp_list[index]
    if isinstance(value, str):
        if value.startswith('"') and value.endswith('"'):
            return value[1:-1]
        if value.startswith("'") and value.endswith("'"):
            return value[1:-1]
    return value


def extract_properties_from_lib_symbol(symbol_def: List) -> Dict[str, str]:
    """
    Extract properties from library symbol definition.
    
    Args:
        symbol_def: Library symbol definition as S-expression list
    
    Returns:
        Dictionary mapping property names to property values
    """
    props = {}
    property_elements = find_elements(symbol_def, 'property')
    
    for prop in property_elements:
        if len(prop) >= 3:
            prop_name = get_atom_value(prop, 1, '')
            prop_value = get_atom_value(prop, 2, '')
            # Only use first 2 strings (name and value) - ignore any extra strings
            # Properties should only have 2 strings, then attributes
            if prop_name:
                props[prop_name] = prop_value
    
    return props


def rename_nested_symbol(nested_symbol: List, base_name: str, extending_name: str) -> List:
    """
    Rename a nested symbol by replacing base name prefix with extending name prefix.
    
    For example, "TSV912IQ2T_1_1" becomes "LM358_DFN_1_1" when base_name="TSV912IQ2T"
    and extending_name="LM358_DFN".
    
    Args:
        nested_symbol: Nested symbol definition as S-expression list
        base_name: Base symbol name to replace
        extending_name: Extending symbol name to use as replacement
    
    Returns:
        Nested symbol with renamed symbol name
    """
    if not isinstance(nested_symbol, list) or len(nested_symbol) < 2:
        return nested_symbol
    
    # Get the nested symbol name (second element)
    nested_name = get_atom_value(nested_symbol, 1, '')
    if not nested_name:
        return nested_symbol
    
    # Replace base name prefix with extending name prefix
    # Format is typically "{BASE_NAME}_{unit}_{body_style}" or "{BASE_NAME}_{unit}"
    if nested_name.startswith(base_name + '_'):
        # Replace the prefix
        suffix = nested_name[len(base_name):]  # Get everything after base_name
        new_name = extending_name + suffix
        # Create a new nested symbol with the renamed symbol name
        result = ['symbol', new_name] + nested_symbol[2:]
        return result
    
    return nested_symbol


def merge_symbol_with_base(base_symbol: List, extending_symbol: List) -> List:
    """
    Merge base symbol fields into extending symbol.
    
    Fields from the extending symbol take precedence over base symbol fields.
    For property elements, matching is done by property name (second element).
    For other elements, matching is done by element type (first element).
    Nested symbol names are renamed to use the extending symbol's name prefix.
    
    Args:
        base_symbol: Base symbol definition as S-expression list
        extending_symbol: Extending symbol definition as S-expression list
    
    Returns:
        Merged symbol definition as S-expression list
    """
    if not isinstance(base_symbol, list) or len(base_symbol) < 2:
        return extending_symbol
    if not isinstance(extending_symbol, list) or len(extending_symbol) < 2:
        return extending_symbol
    
    # Get symbol names for renaming nested symbols
    # Extract just the symbol name part (not the lib_id format)
    base_name_full = get_atom_value(base_symbol, 1, '')
    extending_name_full = get_atom_value(extending_symbol, 1, '')
    
    # Extract symbol name from lib_id format (Library:SymbolName -> SymbolName)
    base_name = base_name_full.split(':', 1)[-1] if ':' in base_name_full else base_name_full
    extending_name = extending_name_full.split(':', 1)[-1] if ':' in extending_name_full else extending_name_full
    
    # Preserve the extending symbol's name (first two elements: 'symbol' and name)
    result = ['symbol', extending_symbol[1]]
    
    # Build a map of extending symbol elements for quick lookup
    # For properties, key by property name; for others, key by element type
    extending_elements_map = {}
    
    # Process extending symbol elements (skip 'symbol' and name, skip 'extends')
    for elem in extending_symbol[2:]:
        if not isinstance(elem, list) or len(elem) == 0:
            continue
        
        elem_type = elem[0]
        
        # Skip the 'extends' element - we don't want it in the final result
        if elem_type == 'extends':
            continue
        
        # For properties, use property name as key
        if elem_type == 'property' and len(elem) > 1:
            prop_name = get_atom_value(elem, 1, '')
            extending_elements_map[('property', prop_name)] = elem
        else:
            # For other elements, use element type as key
            # Note: Some elements like nested 'symbol' can appear multiple times,
            # so we'll collect all of them
            if elem_type == 'symbol':
                # Nested symbols can appear multiple times, collect them all
                if elem_type not in extending_elements_map:
                    extending_elements_map[elem_type] = []
                extending_elements_map[elem_type].append(elem)
            else:
                # Single element types - extending symbol overrides base
                extending_elements_map[elem_type] = elem
    
    # Process base symbol elements (skip 'symbol' and name)
    base_elements_map = {}
    for elem in base_symbol[2:]:
        if not isinstance(elem, list) or len(elem) == 0:
            continue
        
        elem_type = elem[0]
        
        # Skip 'extends' from base symbol too
        if elem_type == 'extends':
            continue
        
        # For properties, use property name as key
        if elem_type == 'property' and len(elem) > 1:
            prop_name = get_atom_value(elem, 1, '')
            base_elements_map[('property', prop_name)] = elem
        else:
            # For other elements, use element type as key
            if elem_type == 'symbol':
                # Nested symbols can appear multiple times, collect them all
                if elem_type not in base_elements_map:
                    base_elements_map[elem_type] = []
                base_elements_map[elem_type].append(elem)
            else:
                base_elements_map[elem_type] = elem
    
    # Merge: start with base elements, override with extending elements
    merged_elements = []
    
    # First, add all base elements that aren't overridden
    for key, elem in base_elements_map.items():
        if key not in extending_elements_map:
            if isinstance(key, tuple):
                # Property
                merged_elements.append(elem)
            elif key == 'symbol' and isinstance(elem, list):
                # Nested symbols list - elem is a list of symbol elements
                # Rename each nested symbol to use extending symbol name prefix
                renamed_symbols = [rename_nested_symbol(sym, base_name, extending_name) 
                                  for sym in elem]
                merged_elements.extend(renamed_symbols)
            else:
                merged_elements.append(elem)
    
    # Then, add all extending elements (these override base elements)
    for key, elem in extending_elements_map.items():
        if isinstance(key, tuple):
            # Property
            merged_elements.append(elem)
        elif key == 'symbol' and isinstance(elem, list):
            # Nested symbols list - elem is a list of symbol elements
            merged_elements.extend(elem)
        else:
            merged_elements.append(elem)
    
    # Add merged elements to result
    result.extend(merged_elements)
    
    return result


def load_symbol_from_library(lib_id: str, symbol_paths: Union[str, List[str]] = None, visited: Optional[Set[str]] = None) -> Optional[List]:
    """
    Load symbol definition from KiCad library file.
    Uses caching to avoid re-parsing the same library file multiple times.
    Handles symbol inheritance via the 'extends' field.
    Searches across multiple symbol directories if provided.
    
    Args:
        lib_id: Library symbol ID in format "LibraryName:SymbolName" (e.g., "Device:C")
        symbol_paths: Path(s) to KiCad symbols directory(ies). Can be a single string or list of strings.
                     If None, uses default KICAD_SYMBOL_PATH or environment variable.
        visited: Set of lib_ids already visited (to prevent circular references)
    
    Returns:
        Symbol definition as S-expression list, or None if not found
    """
    if ':' not in lib_id:
        return None
    
    # Initialize visited set if not provided
    if visited is None:
        visited = set()
    
    # Check for circular references
    if lib_id in visited:
        logger.warning(f"Warning: Circular symbol inheritance detected for {lib_id}")
        return None
    
    # Add current lib_id to visited set
    visited.add(lib_id)
    
    # Normalize symbol_paths to a list
    if symbol_paths is None:
        # Check environment variable first
        env_paths = os.environ.get('KICAD_SYMBOL_DIR', '')
        if env_paths:
            # Handle both colon and semicolon separators
            search_paths = [p.strip() for p in env_paths.replace(';', ':').split(':') if p.strip()]
        else:
            search_paths = [KICAD_SYMBOL_PATH]
    elif isinstance(symbol_paths, str):
        search_paths = [symbol_paths]
    else:
        search_paths = symbol_paths
    
    library_name, symbol_name = lib_id.split(':', 1)
    
    # Search across all provided paths
    library_file = None
    symbol_path = None
    for path in search_paths:
        if not path or not os.path.isdir(path):
            continue
        candidate_file = os.path.join(path, f'{library_name}.kicad_sym')
        if os.path.exists(candidate_file):
            library_file = candidate_file
            symbol_path = path
            break
    
    if not library_file:
        logger.warning(f"Warning: Library file not found in any search path: {library_name}.kicad_sym")
        visited.remove(lib_id)
        return None
    
    try:
        # Check cache first
        if library_name not in _library_cache:
            with open(library_file, 'r') as f:
                content = f.read()
            
            # Parse the library file
            lib_data = parse_sexp(content)
            
            if not isinstance(lib_data, list) or len(lib_data) == 0 or lib_data[0] != 'kicad_symbol_lib':
                visited.remove(lib_id)
                return None
            
            # Cache the parsed library
            _library_cache[library_name] = lib_data
        
        # Get cached library data
        lib_data = _library_cache[library_name]
        
        # Find all symbol definitions
        symbols = find_elements(lib_data, 'symbol')
        
        for symbol in symbols:
            # Get symbol name (second element)
            sym_name = get_atom_value(symbol, 1, None)
            if sym_name == symbol_name:
                # Found the symbol - create result with lib_id prefix
                result = ['symbol', lib_id] + symbol[2:]
                
                # Check if this symbol extends another symbol
                extends_elem = find_element(result, 'extends')
                if extends_elem:
                    # Get the base symbol name
                    base_symbol_name = get_atom_value(extends_elem, 1, None)
                    if base_symbol_name:
                        # Base symbol is in the same library
                        base_lib_id = f'{library_name}:{base_symbol_name}'
                        
                        # Recursively load the base symbol (using same search paths)
                        base_symbol = load_symbol_from_library(base_lib_id, symbol_paths, visited.copy())
                        
                        if base_symbol:
                            # Merge base symbol into extending symbol
                            result = merge_symbol_with_base(base_symbol, result)
                        else:
                            logger.warning(f"Warning: Base symbol '{base_symbol_name}' not found for extending symbol '{lib_id}'")
                            # Continue with extending symbol as-is (without extends field)
                            # Remove the extends element
                            result = ['symbol', lib_id]
                            for elem in symbol[2:]:
                                if isinstance(elem, list) and len(elem) > 0 and elem[0] != 'extends':
                                    result.append(elem)
                
                visited.remove(lib_id)
                return result
        
        logger.warning(f"Warning: Symbol '{symbol_name}' not found in library '{library_name}'")
        visited.remove(lib_id)
        return None
    
    except Exception as e:
        logger.error(f"Error loading symbol from {library_file}: {e}")
        if lib_id in visited:
            visited.remove(lib_id)
        return None


def transform_pin_coordinate(pin_offset: Tuple[float, float, int], 
                             symbol_pos: Tuple[float, float], 
                             symbol_rot: Optional[int], mirror: Optional[str] = None) -> Tuple[float, float]:
    """
    Transform pin offset by symbol position, mirror, and rotation (forward transform).
    Same as in sexp_to_trace_json.py for consistency.
    """
    x_off, y_off, pin_rot = pin_offset
    x_pos, y_pos = symbol_pos
    
    # Normalize pin rotation
    pin_rot_norm = pin_rot % 360
    if pin_rot_norm < 0:
        pin_rot_norm += 360
    # Apply pin rotation transformation
    y_off = -y_off
    
    # Apply mirror transformation before rotation
    if mirror == 'x':
        y_off = -y_off
    elif mirror == 'y':
        x_off = -x_off
    
    # Normalize symbol rotation
    rot = symbol_rot if symbol_rot is not None else 0
    rot = rot % 360
    if rot < 0:
        rot += 360
    
    # Rotate pin offset
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
    
    # Translate by symbol position
    return (x_rot + x_pos, y_rot + y_pos)


def _extract_pin_from_subsymbol(symbol_def: List, pin_number: str, target_unit: int, target_body_style: int) -> Optional[Tuple[float, float, int]]:
    """
    Extract pin position from a specific sub-symbol (exact unit/body_style match).
    
    Args:
        symbol_def: Symbol definition from library
        pin_number: Pin number as string
        target_unit: Target unit number to match
        target_body_style: Target body style to match
    
    Returns:
        (x_offset, y_offset, rotation) or None if not found
    """
    nested_symbols = find_elements(symbol_def, 'symbol')
    
    for nested_symbol in nested_symbols:
        # Get nested symbol name (format: "{name}_{unit}_{body_style}")
        nested_name = get_atom_value(nested_symbol, 1, None)
        if not nested_name:
            continue
        
        # Parse unit and body_style from name
        parts = nested_name.split('_')
        sym_unit = 1
        sym_body_style = 1
        
        if len(parts) >= 3:
            try:
                sym_unit = int(parts[-2])
                sym_body_style = int(parts[-1])
            except (ValueError, IndexError):
                try:
                    sym_unit = int(parts[-1])
                    sym_body_style = 1
                except (ValueError, IndexError):
                    pass
        elif len(parts) >= 2:
            try:
                sym_unit = int(parts[-1])
                sym_body_style = 1
            except (ValueError, IndexError):
                pass
        
        # Check if this is the target unit/body_style
        if sym_unit != target_unit or sym_body_style != target_body_style:
            continue
        
        # Find pins in this nested symbol
        pin_elements = find_elements(nested_symbol, 'pin')
        
        for pin in pin_elements:
            # Get pin number
            number_elem = find_element(pin, 'number')
            if not number_elem:
                continue
            
            pin_num = get_atom_value(number_elem, 1, None)
            if str(pin_num) != str(pin_number):
                continue
            
            # Get pin position (at x y rot)
            at_elem = find_element(pin, 'at')
            if not at_elem:
                continue
            
            try:
                x = float(at_elem[1])
                y = float(at_elem[2])
                rot = int(float(at_elem[3])) if len(at_elem) > 3 else 0
                return (x, y, rot)
            except (ValueError, TypeError, IndexError):
                continue
    
    return None


def extract_pin_info_from_symbol(symbol_def: List, pin_number: str, unit: int = 1, body_style: int = 1) -> Optional[Tuple[float, float, int]]:
    """
    Extract pin position and rotation from symbol definition with inheritance fallback.
    
    KiCad uses an inheritance model for symbol definitions:
    - SymbolName_unit_style: Specific to both unit and style (checked first)
    - SymbolName_0_style: Shared by all units, specific to style
    - SymbolName_unit_0: Specific to unit, shared by all styles
    - SymbolName_0_0: Global elements (inherited by all)
    
    Args:
        symbol_def: Symbol definition from library
        pin_number: Pin number as string
        unit: Symbol unit (default 1, must be >= 1)
        body_style: Body style (default 1, must be >= 1)
    
    Returns:
        (x_offset, y_offset, rotation) or None if not found
    """
    # Search order implements KiCad's inheritance model
    # More specific definitions override less specific ones
    search_order = [
        (unit, body_style),      # Exact match (most specific)
        (0, body_style),         # Shared unit, specific style
        (unit, 0),               # Specific unit, shared style
        (0, 0),                  # Global (least specific)
    ]
    
    for search_unit, search_style in search_order:
        result = _extract_pin_from_subsymbol(symbol_def, pin_number, search_unit, search_style)
        if result:
            return result
    
    return None


def get_valid_unit_body_style_combinations(symbol_def: List) -> List[Tuple[int, int]]:
    """
    Extract all valid (unit, body_style) combinations from a symbol definition
    that are valid for schematic instances.
    
    Note: In KiCad library definitions, unit=0 or body_style=0 means "shared/inherited"
    and is only valid in library files. Schematic instances must use unit >= 1 and
    body_style >= 1. Pins defined in _0_x sub-symbols are inherited by all units.
    
    Args:
        symbol_def: Symbol definition from library
    
    Returns:
        List of (unit, body_style) tuples valid for schematic instances (unit >= 1)
    """
    all_combinations = set()
    units_with_subsymbols = set()  # Track all units that have any sub-symbol defined
    body_styles_with_subsymbols = set()  # Track all body_styles that have any sub-symbol defined
    inherited_pins_body_styles = set()  # Body styles that have pins in _0_x (inherited by all units)
    has_global_pins = False  # Track if _0_0 has pins (inherited by everything)
    
    nested_symbols = find_elements(symbol_def, 'symbol')
    
    for nested_symbol in nested_symbols:
        nested_name = get_atom_value(nested_symbol, 1, None)
        if not nested_name:
            continue
        
        # Parse unit and body_style from name (format: {name}_{unit}_{body_style})
        parts = nested_name.split('_')
        sym_unit = 1
        sym_body_style = 1
        
        if len(parts) >= 3:
            try:
                sym_unit = int(parts[-2])
                sym_body_style = int(parts[-1])
            except (ValueError, IndexError):
                try:
                    sym_unit = int(parts[-1])
                    sym_body_style = 1
                except (ValueError, IndexError):
                    pass
        elif len(parts) >= 2:
            try:
                sym_unit = int(parts[-1])
                sym_body_style = 1
            except (ValueError, IndexError):
                pass
        
        # Track all units and body_styles that have sub-symbols
        if sym_unit >= 1:
            units_with_subsymbols.add(sym_unit)
        if sym_body_style >= 1:
            body_styles_with_subsymbols.add(sym_body_style)
        
        # Check if this sub-symbol has pins
        pin_elements = find_elements(nested_symbol, 'pin')
        if pin_elements:
            if sym_unit == 0 and sym_body_style == 0:
                # _0_0 has pins - inherited by all units and body_styles
                has_global_pins = True
            elif sym_unit == 0 and sym_body_style >= 1:
                # _0_style has pins - inherited by all units for this body_style
                inherited_pins_body_styles.add(sym_body_style)
            elif sym_unit >= 1:
                # Explicit unit with pins
                all_combinations.add((sym_unit, sym_body_style))
    
    # Now build the valid combinations considering inheritance
    # If there are global pins (_0_0), all unit/body_style combinations are valid
    # If there are inherited pins for a body_style (_0_style), all units with that body_style are valid
    
    if has_global_pins:
        # All units with all body_styles are valid
        units = units_with_subsymbols if units_with_subsymbols else {1}
        body_styles = body_styles_with_subsymbols if body_styles_with_subsymbols else {1}
        for u in units:
            for b in body_styles:
                all_combinations.add((u, b))
    
    if inherited_pins_body_styles:
        # For each body_style with inherited pins, all units are valid
        units = units_with_subsymbols if units_with_subsymbols else {1}
        for b in inherited_pins_body_styles:
            for u in units:
                all_combinations.add((u, b))
    
    # Filter to only schematic-valid combinations (unit >= 1, body_style >= 1)
    valid_for_schematic = [(u, b) for u, b in all_combinations if u >= 1 and b >= 1]
    
    # If still empty but there are inherited pins, default to (1, 1)
    if not valid_for_schematic and (has_global_pins or inherited_pins_body_styles):
        valid_for_schematic = [(1, 1)]
    
    return sorted(set(valid_for_schematic))


def validate_unit_body_style(symbol_def: List, lib_id: str, unit: int, body_style: int) -> None:
    """
    Validate that the unit/body_style combination is valid for a schematic instance.
    
    In KiCad, unit and body_style must be >= 1 for schematic instances.
    Values of 0 are only valid in library definitions for inheritance.
    
    Args:
        symbol_def: Symbol definition from library
        lib_id: Library symbol ID for error message
        unit: Unit number to validate (must be >= 1)
        body_style: Body style number to validate (must be >= 1)
    
    Raises:
        ValueError: If unit/body_style < 1 or combination doesn't exist
    """
    # First check: unit and body_style must be >= 1 for schematic instances
    if unit < 1:
        raise ValueError(f"unit must be >= 1 for symbol '{lib_id}', got {unit}")
    if body_style < 1:
        raise ValueError(f"body_style must be >= 1 for symbol '{lib_id}', got {body_style}")
    
    # Then check if combination is valid (has pins, considering inheritance)
    valid_combos = get_valid_unit_body_style_combinations(symbol_def)
    
    # If no valid combos, the symbol has no pins (e.g., mounting holes) - skip validation
    if not valid_combos:
        return
    
    if (unit, body_style) not in valid_combos:
        valid_str = ", ".join(f"(unit={u}, body_style={b})" for u, b in valid_combos)
        raise ValueError(
            f"Invalid unit/body_style for symbol '{lib_id}': "
            f"unit={unit}, body_style={body_style}. "
            f"Valid combinations with pins: {valid_str}"
        )


def get_all_pin_numbers(symbol_def: List) -> Set[str]:
    """
    Extract all valid pin numbers from a symbol definition across all sub-symbols.
    
    This function collects pins from ALL sub-symbols including inherited ones
    (_0_0, _0_style, _unit_0) since pins defined in these are inherited by
    all unit/body_style combinations.
    
    Args:
        symbol_def: Symbol definition from library
    
    Returns:
        Set of pin numbers (as strings) that exist in the symbol (including inherited)
    """
    pin_numbers = set()
    nested_symbols = find_elements(symbol_def, 'symbol')
    
    for nested_symbol in nested_symbols:
        pin_elements = find_elements(nested_symbol, 'pin')
        for pin in pin_elements:
            number_elem = find_element(pin, 'number')
            if number_elem:
                pin_num = get_atom_value(number_elem, 1, None)
                if pin_num is not None:
                    pin_numbers.add(str(pin_num))
    
    return pin_numbers


def validate_pin_numbers(symbol_def: List, lib_id: str, comp_ref: str, pins: Dict[str, Any]) -> None:
    """
    Validate that all pin numbers used exist in the symbol definition.
    
    This validation considers inheritance - pins defined in _0_0, _0_style, etc.
    are valid for all unit/body_style combinations since they are inherited.
    
    Raises ValueError with helpful message if invalid pins are found.
    
    Args:
        symbol_def: Symbol definition from library
        lib_id: Library symbol ID for error message
        comp_ref: Component reference for error message
        pins: Dictionary of {pin_number: net_name} from trace JSON
    """
    valid_pins = get_all_pin_numbers(symbol_def)
    invalid_pins = [p for p in pins.keys() if str(p) not in valid_pins]
    
    if invalid_pins:
        valid_str = ", ".join(sorted(valid_pins, key=lambda x: (len(x), x)))
        raise ValueError(
            f"Invalid pin number(s) for symbol '{lib_id}' (ref: {comp_ref}): "
            f"{', '.join(str(p) for p in invalid_pins)}. "
            f"Valid pin numbers: {valid_str}"
        )


# =============================================================================
# Power Symbol Detection
# =============================================================================

def is_power_symbol(lib_id: str) -> bool:
    """
    Check if a symbol is a power symbol by checking if the library name contains "power".
    
    Args:
        lib_id: Library symbol ID in format "LibraryName:SymbolName"
    
    Returns:
        True if the symbol is a power symbol, False otherwise
    """
    if not lib_id or ':' not in lib_id:
        return False
    
    library_name = lib_id.split(':', 1)[0].lower()
    return 'power' in library_name


# =============================================================================
# Element Mergers (Update Existing Elements)
# =============================================================================

def merge_symbol(existing_symbol: List, trace_comp: Dict[str, Any], lib_symbols_cache: Dict[str, List] = None) -> List:
    """
    Merge trace_sch component data into existing symbol.
    Updates only changed fields, preserves all other elements.
    If optional properties (Value, Footprint, Datasheet, Description) are not provided
    in trace_json, copies them from the symbol library definition.
    
    Args:
        existing_symbol: Existing symbol as S-expression list
        trace_comp: Component data from trace_sch JSON
        lib_symbols_cache: Cache of loaded library symbols (optional, for copying library properties)
    
    Returns:
        Updated symbol as S-expression list
    """
    result = copy.deepcopy(existing_symbol)
    
    # Update position (at) - symbol position should be updated when changed in trace_sch
    if 'at' in trace_comp:
        at_elem = find_element(result, 'at')
        if at_elem:
            rot = trace_comp.get('rot', 0)
            if len(at_elem) >= 3:
                at_idx = result.index(at_elem)
                result[at_idx] = ['at', trace_comp['at'][0], trace_comp['at'][1], rot]
    
    # Update rotation if provided separately
    if 'rot' in trace_comp:
        at_elem = find_element(result, 'at')
        if at_elem and len(at_elem) >= 4:
            at_idx = result.index(at_elem)
            result[at_idx][3] = trace_comp['rot']
    
    # Update mirror
    existing_mirror = find_element(result, 'mirror')
    if 'mirror' in trace_comp:
        new_mirror = trace_comp['mirror']
        if existing_mirror:
            mirror_idx = result.index(existing_mirror)
            result[mirror_idx] = ['mirror', new_mirror]
        else:
            at_elem = find_element(result, 'at')
            insert_idx = result.index(at_elem) + 1 if at_elem else 2
            result.insert(insert_idx, ['mirror', new_mirror])
    elif existing_mirror:
        result.remove(existing_mirror)
    
    # Update unit
    if 'unit' in trace_comp:
        unit_elem = find_element(result, 'unit')
        if unit_elem:
            unit_idx = result.index(unit_elem)
            result[unit_idx] = ['unit', trace_comp['unit']]
        else:
            # Insert after lib_id or at
            insert_idx = 1
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] in ('lib_id', 'at'):
                    insert_idx = i + 1
                    break
            result.insert(insert_idx, ['unit', trace_comp['unit']])
    
    # Update body_style
    if 'body_style' in trace_comp:
        body_style_elem = find_element(result, 'body_style')
        if body_style_elem:
            body_style_idx = result.index(body_style_elem)
            result[body_style_idx] = ['body_style', trace_comp['body_style']]
        else:
            # Insert after unit
            unit_idx = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'unit':
                    unit_idx = i
                    break
            if unit_idx is not None:
                result.insert(unit_idx + 1, ['body_style', trace_comp['body_style']])
    
    # Get library properties if available and check if this is a power symbol
    lib_props = {}
    is_power = False
    lib_id = None
    if lib_symbols_cache:
        lib_id_elem = find_element(result, 'lib_id')
        if lib_id_elem:
            lib_id = get_atom_value(lib_id_elem, 1, None)
            if lib_id:
                is_power = is_power_symbol(lib_id)
                if lib_id in lib_symbols_cache:
                    symbol_def = lib_symbols_cache[lib_id]
                    lib_props = extract_properties_from_lib_symbol(symbol_def)
                    
                    # Validate unit/body_style combination if either was updated
                    if 'unit' in trace_comp or 'body_style' in trace_comp:
                        # Get current values from result
                        unit_elem = find_element(result, 'unit')
                        body_style_elem = find_element(result, 'body_style')
                        unit_val = get_atom_value(unit_elem, 1, 1) if unit_elem else 1
                        body_style_val = get_atom_value(body_style_elem, 1, 1) if body_style_elem else 1
                        validate_unit_body_style(symbol_def, lib_id, unit_val, body_style_val)
    
    # Validate power symbol reference starts with '#'
    comp_ref = trace_comp.get('ref', '')
    if is_power and comp_ref and not comp_ref.startswith('#'):
        raise ValueError(f"Power symbol '{lib_id}' reference '{comp_ref}' must start with '#' to be excluded from layout")
    
    # Helper function to update or add property with hide flag
    def update_or_add_property(prop_name: str, prop_value: str, x_offset: float, y_offset: float, hide: bool = True):
        """Update existing property or add new one with hide flag.
        
        NOTE: When updating existing properties, we preserve the original 'at' coordinates
        from the existing schematic. We only set coordinates when adding NEW properties.
        This is intentional - KiCad uses absolute coordinates for properties, and the
        existing schematic has the correct positions.
        """
        prop_found = False
        for i, item in enumerate(result):
            if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                existing_prop_name = get_atom_value(item, 1, '')
                if existing_prop_name == prop_name:
                    # Update existing property value only - preserve 'at' coordinates
                    result[i][2] = prop_value
                    # Update hide flag
                    hide_elem = find_element(item, 'hide')
                    if hide:
                        if hide_elem:
                            # Update existing hide element
                            hide_idx = item.index(hide_elem)
                            item[hide_idx] = ['hide', 'yes']
                        else:
                            # Insert hide element before effects
                            effects_idx = None
                            for j, elem in enumerate(item):
                                if isinstance(elem, list) and len(elem) > 0 and elem[0] == 'effects':
                                    effects_idx = j
                                    break
                            if effects_idx is not None:
                                item.insert(effects_idx, ['hide', 'yes'])
                    else:
                        # Remove hide element if present
                        if hide_elem:
                            item.remove(hide_elem)
                    prop_found = True
                    break
        
        # NOTE: We intentionally do NOT add new properties here.
        # The existing schematic should already have all necessary properties with
        # correct coordinates. Adding new properties with calculated coordinates
        # can cause misalignment issues.
    
    # Update properties (match by property name)
    if 'props' in trace_comp or 'ref' in trace_comp or 'value' in trace_comp:
        comp_props = trace_comp.get('props', {})
        comp_ref = trace_comp.get('ref', '')
        comp_value = trace_comp.get('value', '')
        
        # Handle Reference property - hide for power symbols
        if comp_ref:
            update_or_add_property('Reference', comp_ref, 0, 2.54, hide=is_power)
        
        # Handle Value property - use trace_json value if provided, otherwise copy from library
        # Value is never hidden, even for power symbols
        value_to_use = None
        if 'value' in trace_comp:
            value_to_use = comp_value
        elif lib_props:
            # Not provided in trace_json, copy from library
            value_to_use = lib_props.get('Value', '')
        
        if value_to_use:
            value_prop = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                    prop_name = get_atom_value(item, 1, '')
                    if prop_name == 'Value':
                        value_prop = item
                        value_idx = i
                        break
            
            if value_prop:
                # Update existing Value property value - preserve 'at' coordinates
                result[value_idx][2] = value_to_use
                # Ensure Value is never hidden
                hide_elem = find_element(value_prop, 'hide')
                if hide_elem:
                    value_prop.remove(hide_elem)
            # NOTE: We intentionally do NOT add new Value property here.
            # The existing schematic should already have the Value property with
            # correct coordinates.
        
        # Handle Footprint property - use trace_json value if provided, otherwise copy from library
        # Always hide Footprint (for both regular and power symbols)
        footprint_to_use = None
        if 'footprint' in comp_props:
            footprint_to_use = comp_props['footprint']
        elif lib_props:
            # Not provided in trace_json, copy from library
            footprint_to_use = lib_props.get('Footprint', '')
        
        if footprint_to_use is not None:
            update_or_add_property('Footprint', str(footprint_to_use), 0, 0, hide=True)
        
        # Handle Datasheet property - use trace_json value if provided, otherwise copy from library
        # Always hide Datasheet (for both regular and power symbols)
        datasheet_to_use = None
        if 'datasheet' in comp_props:
            datasheet_to_use = comp_props['datasheet']
        elif lib_props:
            # Not provided in trace_json, copy from library
            datasheet_to_use = lib_props.get('Datasheet', '')
        
        if datasheet_to_use is not None:
            update_or_add_property('Datasheet', str(datasheet_to_use), 0, 0, hide=True)
        
        # Handle Description property - use trace_json value if provided, otherwise copy from library
        # Always hide Description (for both regular and power symbols)
        description_to_use = None
        if 'description' in comp_props:
            description_to_use = comp_props['description']
        elif lib_props:
            # Not provided in trace_json, copy from library
            description_to_use = lib_props.get('Description', '')
        
        if description_to_use is not None:
            update_or_add_property('Description', str(description_to_use), 0, 0, hide=True)
        
        # Update other properties from comp_props (match by property name)
        # Always hide all other properties (for both regular and power symbols)
        standard_props = {'reference', 'value', 'footprint', 'datasheet', 'description'}
        for prop_name, prop_value in comp_props.items():
            prop_name_lower = prop_name.lower()
            if prop_name_lower in standard_props:
                continue  # Already handled above
            
            # Find existing property with this name
            prop_found = False
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                    existing_prop_name = get_atom_value(item, 1, '')
                    if existing_prop_name == prop_name:
                        # Update existing property value - preserve 'at' coordinates
                        result[i][2] = str(prop_value)
                        # Always hide other properties (for both regular and power symbols)
                        hide_elem = find_element(item, 'hide')
                        if not hide_elem:
                            # Insert hide element before effects
                            effects_idx = None
                            for j, elem in enumerate(item):
                                if isinstance(elem, list) and len(elem) > 0 and elem[0] == 'effects':
                                    effects_idx = j
                                    break
                            if effects_idx is not None:
                                item.insert(effects_idx, ['hide', 'yes'])
                        prop_found = True
                        break
            
            # NOTE: We intentionally do NOT add new properties here.
            # The existing schematic should already have all necessary properties with
            # correct coordinates.
    
    return result


def merge_wire(existing_wire: List, trace_wire: Dict[str, Any]) -> List:
    """Merge trace_sch wire data into existing wire."""
    result = copy.deepcopy(existing_wire)
    
    # Update wire points
    if 'points' in trace_wire and len(trace_wire['points']) >= 2:
        pts_elem = find_element(result, 'pts')
        if pts_elem:
            pts_idx = result.index(pts_elem)
            # Build new pts structure
            pts = ['pts']
            for point in trace_wire['points']:
                pts.append(['xy', point[0], point[1]])
            result[pts_idx] = pts
    
    # Update stroke (width, type) if present
    if 'stroke' in trace_wire:
        stroke_elem = find_element(result, 'stroke')
        if stroke_elem:
            stroke_idx = result.index(stroke_elem)
            # Build stroke element (default values if not specified)
            width = trace_wire.get('stroke_width', 0)
            stroke_type = trace_wire.get('stroke_type', 'default')
            result[stroke_idx] = ['stroke', ['width', width], ['type', stroke_type]]
    
    return result


def merge_junction(existing_junction: List, trace_junction: Dict[str, Any]) -> List:
    """Merge trace_sch junction data into existing junction."""
    result = copy.deepcopy(existing_junction)
    
    # Update position (at)
    if 'at' in trace_junction:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            result[at_idx] = ['at', trace_junction['at'][0], trace_junction['at'][1]]
    
    # Update diameter
    if 'diameter' in trace_junction:
        diameter_elem = find_element(result, 'diameter')
        if diameter_elem:
            diameter_idx = result.index(diameter_elem)
            result[diameter_idx] = ['diameter', trace_junction['diameter']]
    
    # Update color
    if 'color' in trace_junction:
        color_elem = find_element(result, 'color')
        if color_elem:
            color_idx = result.index(color_elem)
            color = trace_junction['color']
            if isinstance(color, list) and len(color) >= 4:
                result[color_idx] = ['color'] + color[:4]
            else:
                result[color_idx] = ['color', 0, 0, 0, 0]
    
    return result


def merge_label(existing_label: List, trace_label: Dict[str, Any]) -> List:
    """Merge trace_sch label data into existing label."""
    result = copy.deepcopy(existing_label)
    
    # Update label name (second element)
    if 'name' in trace_label and len(result) >= 2:
        result[1] = trace_label['name']
    
    # Update position (at)
    if 'at' in trace_label:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            at_coord = trace_label['at']
            if len(at_coord) >= 2:
                if len(at_elem) >= 4:
                    result[at_idx] = ['at', at_coord[0], at_coord[1], at_elem[3]]
                else:
                    result[at_idx] = ['at', at_coord[0], at_coord[1], 0]
    
    # Update effects (font, justify) if present
    if 'effects' in trace_label or 'font_size' in trace_label or 'justify' in trace_label:
        effects_elem = find_element(result, 'effects')
        if effects_elem:
            effects_idx = result.index(effects_elem)
            # Rebuild effects (preserve existing values if not specified)
            font_parts = []
            justify_parts = []
            
            # Get existing font info or use defaults
            font_elem = find_element(effects_elem, 'font')
            if font_elem:
                size_elem = find_element(font_elem, 'size')
                if size_elem and len(size_elem) >= 3:
                    font_parts.append(['size', size_elem[1], size_elem[2]])
                else:
                    font_parts.append(['size', 1.27, 1.27])
            else:
                font_parts.append(['size', 1.27, 1.27])
            
            # Get existing justify or use defaults
            justify_elem = find_element(effects_elem, 'justify')
            if justify_elem and len(justify_elem) >= 3:
                justify_parts = [justify_elem[1], justify_elem[2]]
            else:
                justify_parts = ['left', 'bottom']
            
            # Override with trace_label values if present
            if 'font_size' in trace_label and isinstance(trace_label['font_size'], list) and len(trace_label['font_size']) >= 2:
                font_parts[0] = ['size', trace_label['font_size'][0], trace_label['font_size'][1]]
            
            if 'justify' in trace_label and isinstance(trace_label['justify'], list) and len(trace_label['justify']) >= 2:
                justify_parts = trace_label['justify'][:2]
            
            result[effects_idx] = ['effects',
                                  ['font'] + font_parts,
                                  ['justify', justify_parts[0], justify_parts[1]]]
    
    return result


def merge_noconnect(existing_noconnect: List, trace_noconnect: Dict[str, Any]) -> List:
    """Merge trace_sch noconnect data into existing no_connect."""
    result = copy.deepcopy(existing_noconnect)
    
    # Update position (at)
    if 'at' in trace_noconnect:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            result[at_idx] = ['at', trace_noconnect['at'][0], trace_noconnect['at'][1]]
    
    return result


def merge_glabel(existing_glabel: List, trace_glabel: Dict[str, Any]) -> List:
    """Merge trace_sch glabel data into existing global_label."""
    result = copy.deepcopy(existing_glabel)
    
    # Update label name (second element)
    if 'name' in trace_glabel and len(result) >= 2:
        result[1] = trace_glabel['name']
    
    # Update shape
    if 'shape' in trace_glabel:
        shape = trace_glabel['shape']
        if shape not in VALID_LABEL_SHAPES:
            raise ValueError(
                f"Invalid glabel shape '{shape}' for label '{trace_glabel.get('name', '?')}'. "
                f"Must be one of {sorted(VALID_LABEL_SHAPES)}"
            )
        shape_elem = find_element(result, 'shape')
        if shape_elem:
            shape_idx = result.index(shape_elem)
            result[shape_idx] = ['shape', shape]
    
    # Update position (at)
    if 'at' in trace_glabel:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            at_coord = trace_glabel['at']
            rot = trace_glabel.get('rot', 0)
            if len(at_coord) >= 2:
                result[at_idx] = ['at', at_coord[0], at_coord[1], rot]
    
    # Update rotation if provided separately
    if 'rot' in trace_glabel:
        at_elem = find_element(result, 'at')
        if at_elem and len(at_elem) >= 4:
            at_idx = result.index(at_elem)
            result[at_idx][3] = trace_glabel['rot']
    
    # Update properties (match by property name, especially Intersheetrefs)
    if 'props' in trace_glabel:
        props = trace_glabel['props']
        if 'intersheetrefs' in props:
            # Find Intersheetrefs property
            intersheetrefs_prop = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                    prop_name = get_atom_value(item, 1, '')
                    if prop_name == 'Intersheetrefs':
                        intersheetrefs_prop = item
                        prop_idx = i
                        break
            
            if intersheetrefs_prop:
                # Update existing Intersheetrefs property value
                result[prop_idx][2] = props['intersheetrefs']
            else:
                # Add new Intersheetrefs property before uuid
                uuid_idx = None
                for i, item in enumerate(result):
                    if isinstance(item, list) and len(item) > 0 and item[0] == 'uuid':
                        uuid_idx = i
                        break
                if uuid_idx is not None:
                    at_elem = find_element(result, 'at')
                    pos = [0, 0]
                    if at_elem and len(at_elem) >= 3:
                        pos = [at_elem[1], at_elem[2]]
                    result.insert(uuid_idx, ['property', 'Intersheetrefs', props['intersheetrefs'],
                                           ['at', pos[0] + 6.8557, pos[1], 0],
                                           ['hide', 'yes'],
                                           ['show_name', 'no'],
                                           ['do_not_autoplace', 'no'],
                                           ['effects',
                                            ['font', ['size', 1.27, 1.27]],
                                            ['justify', 'left']]])
    
    return result


def merge_hier_label(existing_hier: List, trace_hier: Dict[str, Any]) -> List:
    """Merge trace_sch hier label data into existing hierarchical_label."""
    result = copy.deepcopy(existing_hier)
    
    # Update label name (second element)
    if 'name' in trace_hier and len(result) >= 2:
        result[1] = trace_hier['name']
    
    # Update shape
    if 'shape' in trace_hier:
        shape = trace_hier['shape']
        if shape not in VALID_LABEL_SHAPES:
            raise ValueError(
                f"Invalid hier label shape '{shape}' for label '{trace_hier.get('name', '?')}'. "
                f"Must be one of {sorted(VALID_LABEL_SHAPES)}"
            )
        shape_elem = find_element(result, 'shape')
        if shape_elem:
            shape_idx = result.index(shape_elem)
            result[shape_idx] = ['shape', shape]
    
    # Update position (at)
    if 'at' in trace_hier:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            at_coord = trace_hier['at']
            rot = trace_hier.get('rot', 0)
            if len(at_coord) >= 2:
                result[at_idx] = ['at', at_coord[0], at_coord[1], rot]
    
    # Update rotation if provided separately
    if 'rot' in trace_hier:
        at_elem = find_element(result, 'at')
        if at_elem and len(at_elem) >= 4:
            at_idx = result.index(at_elem)
            result[at_idx][3] = trace_hier['rot']
    
    return result


def merge_sheet(existing_sheet: List, trace_sheet: Dict[str, Any]) -> List:
    """Merge trace_sch sheet data into existing sheet."""
    result = copy.deepcopy(existing_sheet)
    
    # Update position (at)
    if 'at' in trace_sheet:
        at_elem = find_element(result, 'at')
        if at_elem:
            at_idx = result.index(at_elem)
            result[at_idx] = ['at', trace_sheet['at'][0], trace_sheet['at'][1]]
    
    # Update size
    if 'size' in trace_sheet:
        size_elem = find_element(result, 'size')
        if size_elem:
            size_idx = result.index(size_elem)
            size = trace_sheet['size']
            if len(size) >= 2:
                result[size_idx] = ['size', size[0], size[1]]
    
    # Update properties (Sheetname, Sheetfile)
    if 'props' in trace_sheet or 'name' in trace_sheet or 'file' in trace_sheet:
        props = trace_sheet.get('props', {})
        sheet_name = trace_sheet.get('name', props.get('sheetname', ''))
        file_value = trace_sheet.get('file', props.get('sheetfile', ''))
        
        # Update Sheetname property
        if sheet_name:
            sheetname_prop = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                    prop_name = get_atom_value(item, 1, '')
                    if prop_name == 'Sheetname':
                        sheetname_prop = item
                        prop_idx = i
                        break
            
            if sheetname_prop:
                result[prop_idx][2] = sheet_name
        
        # Update Sheetfile property
        if file_value:
            sheetfile_prop = None
            for i, item in enumerate(result):
                if isinstance(item, list) and len(item) > 0 and item[0] == 'property':
                    prop_name = get_atom_value(item, 1, '')
                    if prop_name == 'Sheetfile':
                        sheetfile_prop = item
                        prop_idx = i
                        break
            
            if sheetfile_prop:
                result[prop_idx][2] = file_value
    
    # Update sheet pins (ins and outs) - match by pin name
    if 'ins' in trace_sheet:
        ins = trace_sheet['ins']
        # Find existing input pins and update or add
        existing_pins = find_elements(result, 'pin')
        for pin_name, pin_value in ins.items():
            # Find pin by name
            pin_found = False
            for i, pin_elem in enumerate(existing_pins):
                if len(pin_elem) >= 2 and get_atom_value(pin_elem, 1, '') == pin_name:
                    # Check if it's an input pin (third element should be 'input')
                    if len(pin_elem) >= 3 and pin_elem[2] == 'input':
                        # Update pin position if provided
                        if isinstance(pin_value, dict) and 'at' in pin_value:
                            at_elem = find_element(pin_elem, 'at')
                            if at_elem:
                                pin_idx = None
                                for j, item in enumerate(result):
                                    if item == pin_elem:
                                        pin_idx = j
                                        break
                                if pin_idx is not None:
                                    at_idx = result[pin_idx].index(at_elem)
                                    pin_coord = pin_value['at']
                                    rot = pin_value.get('rot', 180)
                                    if len(pin_coord) >= 2:
                                        result[pin_idx][at_idx] = ['at', pin_coord[0], pin_coord[1], rot]
                        pin_found = True
                        break
            
            # If pin not found, it will be added by convert_sheet if needed
            # For now, we skip adding new pins in merge (they should be in trace_sheet)
    
    if 'outs' in trace_sheet:
        outs = trace_sheet['outs']
        # Find existing output pins and update
        existing_pins = find_elements(result, 'pin')
        for pin_name, pin_value in outs.items():
            # Find pin by name
            for i, pin_elem in enumerate(existing_pins):
                if len(pin_elem) >= 2 and get_atom_value(pin_elem, 1, '') == pin_name:
                    # Check if it's an output pin (third element should be 'output')
                    if len(pin_elem) >= 3 and pin_elem[2] == 'output':
                        # Update pin position if provided
                        if isinstance(pin_value, dict) and 'at' in pin_value:
                            at_elem = find_element(pin_elem, 'at')
                            if at_elem:
                                pin_idx = None
                                for j, item in enumerate(result):
                                    if item == pin_elem:
                                        pin_idx = j
                                        break
                                if pin_idx is not None:
                                    at_idx = result[pin_idx].index(at_elem)
                                    pin_coord = pin_value['at']
                                    rot = pin_value.get('rot', 0)
                                    if len(pin_coord) >= 2:
                                        result[pin_idx][at_idx] = ['at', pin_coord[0], pin_coord[1], rot]
                        break
    
    # Handle page number in sheet instances
    if 'page' in trace_sheet:
        instances_elem = find_element(result, 'instances')
        if instances_elem:
            for item in instances_elem:
                if isinstance(item, list) and len(item) > 0 and item[0] == 'project':
                    for path_item in item:
                        if isinstance(path_item, list) and len(path_item) > 0 and path_item[0] == 'path':
                            # Find or create page element
                            page_elem = find_element(path_item, 'page')
                            if page_elem:
                                page_idx = path_item.index(page_elem)
                                path_item[page_idx] = ['page', str(trace_sheet['page'])]
                            else:
                                path_item.append(['page', str(trace_sheet['page'])])
    
    return result


# =============================================================================
# Element Converters
# =============================================================================

def convert_component(comp: Dict[str, Any], lib_symbols_cache: Dict[str, List], instance_path: str = None, symbol_paths: Union[str, List[str]] = None) -> Tuple[Optional[List], List[Dict[str, Any]], List[Dict[str, Any]]]:
    """
    Convert component from trace JSON to KiCad symbol instance.
    
    Args:
        comp: Component dictionary from trace JSON
        lib_symbols_cache: Cache of loaded library symbols
        instance_path: Consistent path to use for all instances (defaults to comp_uid if not provided)
        symbol_paths: Path(s) to KiCad symbols directory(ies) for loading symbols
    
    Returns:
        Tuple of (symbol_instance, list_of_labels_for_pins, list_of_no_connects_for_pins)
    """
    lib_id = comp.get('symbol')
    if not lib_id:
        return None, [], []
    
    # Load symbol definition if not cached
    if lib_id not in lib_symbols_cache:
        symbol_def = load_symbol_from_library(lib_id, symbol_paths)
        if symbol_def:
            lib_symbols_cache[lib_id] = symbol_def
        else:
            print(f"Warning: Could not load symbol {lib_id}")
            return None, [], []
    
    # Create symbol instance
    symbol_pos = comp.get('at', [0, 0])
    symbol_rot = comp.get('rot', 0)
    comp_ref = comp.get('ref', '')
    comp_value = comp.get('value', '')
    comp_uid = comp.get('uid')
    if not comp_uid:
        comp_uid = str(uuid.uuid4())
    
    # Extract unit and body_style (default to 1 if not present)
    unit = comp.get('unit', 1)
    body_style = comp.get('body_style', 1)
    
    # Validate unit/body_style combination exists with pins
    symbol_def = lib_symbols_cache[lib_id]
    validate_unit_body_style(symbol_def, lib_id, unit, body_style)
    
    # Build symbol instance
    symbol_instance = ['symbol']
    symbol_instance.append(['lib_id', lib_id])
    symbol_instance.append(['at', symbol_pos[0], symbol_pos[1], symbol_rot])
    mirror = comp.get('mirror')
    if mirror:
        symbol_instance.append(['mirror', mirror])
    symbol_instance.append(['unit', unit])
    symbol_instance.append(['body_style', body_style])
    symbol_instance.append(['exclude_from_sim', 'no'])
    symbol_instance.append(['in_bom', 'yes'])
    symbol_instance.append(['on_board', 'yes'])
    symbol_instance.append(['dnp', 'no'])
    symbol_instance.append(['fields_autoplaced', 'yes'])
    symbol_instance.append(['uuid', comp_uid])
    
    # Extract properties from library symbol definition
    symbol_def = lib_symbols_cache[lib_id]
    lib_props = extract_properties_from_lib_symbol(symbol_def)
    
    # Check if this is a power symbol
    is_power = is_power_symbol(lib_id)
    
    # Validate power symbol reference starts with '#'
    if is_power and comp_ref and not comp_ref.startswith('#'):
        raise ValueError(f"Power symbol '{lib_id}' reference '{comp_ref}' must start with '#' to be excluded from layout")
    
    # Get component props
    comp_props = comp.get('props', {})
    
    # Helper function to create property element with proper formatting
    def create_property_element(prop_name: str, prop_value: str, x_offset: float, y_offset: float, hide: bool = True) -> List:
        """Create a property element with proper formatting."""
        prop_elem = ['property', prop_name, prop_value,
                    ['at', symbol_pos[0] + x_offset, symbol_pos[1] + y_offset, 0],
                    ['show_name', 'no'],
                    ['do_not_autoplace', 'no'],
                    ['effects', ['font', ['size', 1.27, 1.27]]]]
        if hide:
            prop_elem.insert(-1, ['hide', 'yes'])
        return prop_elem
    
    # Add properties
    # For power symbols, hide Reference property
    if comp_ref:
        hide_ref = is_power
        ref_elem = ['property', 'Reference', comp_ref,
                   ['at', symbol_pos[0], symbol_pos[1] + 2.54, 0],
                   ['show_name', 'no'],
                   ['do_not_autoplace', 'no'],
                   ['effects', ['font', ['size', 1.27, 1.27]]]]
        if hide_ref:
            ref_elem.insert(-1, ['hide', 'yes'])
        symbol_instance.append(ref_elem)
    
    # Add Value property - use trace_json value if provided, otherwise copy from library
    # For power symbols, Value is always visible (not hidden)
    if 'value' in comp:
        value_to_use = comp_value
    else:
        # Not provided in trace_json, copy from library
        value_to_use = lib_props.get('Value', '')
    
    if value_to_use:
        # Value is never hidden, even for power symbols
        symbol_instance.append(['property', 'Value', value_to_use,
                               ['at', symbol_pos[0], symbol_pos[1] - 2.54, 0],
                               ['show_name', 'no'],
                               ['do_not_autoplace', 'no'],
                               ['effects', ['font', ['size', 1.27, 1.27]]]])
    
    # Add Footprint property - use trace_json value if provided, otherwise copy from library
    # Always hide Footprint (for both regular and power symbols)
    if 'footprint' in comp_props:
        footprint = comp_props['footprint']
    else:
        # Not provided in trace_json, copy from library
        footprint = lib_props.get('Footprint', '')
    symbol_instance.append(create_property_element('Footprint', footprint, 0, 0, hide=True))
    
    # Add Datasheet property - use trace_json value if provided, otherwise copy from library
    # Always hide Datasheet (for both regular and power symbols)
    if 'datasheet' in comp_props:
        datasheet = comp_props['datasheet']
    else:
        # Not provided in trace_json, copy from library
        datasheet = lib_props.get('Datasheet', '')
    symbol_instance.append(create_property_element('Datasheet', datasheet, 0, 0, hide=True))
    
    # Add Description property - use trace_json value if provided, otherwise copy from library
    # Always hide Description (for both regular and power symbols)
    if 'description' in comp_props:
        description = comp_props['description']
    else:
        # Not provided in trace_json, copy from library
        description = lib_props.get('Description', '')
    symbol_instance.append(create_property_element('Description', description, 0, 0, hide=True))
    
    # Add all other properties from trace_sch file (comp_props)
    # Skip standard properties we've already added (Reference, Value, Footprint, Datasheet, Description)
    # Also skip library-only/internal properties that shouldn't be in user schematics
    # Always hide all other properties (for both regular and power symbols)
    standard_props = {'reference', 'value', 'footprint', 'datasheet', 'description'}
    library_only_props = {'private'}  # Properties that are library-internal and shouldn't be copied
    for prop_name, prop_value in comp_props.items():
        prop_name_lower = prop_name.lower()
        if (prop_name_lower not in standard_props and 
            prop_name_lower not in library_only_props and 
            prop_value):
            # Only add properties that are in the trace_sch file (user's schematic)
            # Don't add library-only properties like "private"
            # Always hide all other properties (for both regular and power symbols)
            symbol_instance.append(create_property_element(prop_name, str(prop_value), 0, 0, hide=True))
    
    # Add pins
    pins = comp.get('pins', {})
    
    # Validate pin numbers exist in symbol
    if pins:
        validate_pin_numbers(symbol_def, lib_id, comp_ref, pins)
    
    for pin_num in pins.keys():
        pin_uid = str(uuid.uuid4())
        symbol_instance.append(['pin', pin_num, ['uuid', pin_uid]])
    
    # Add instances section
    # Use consistent path for all instances (either provided instance_path or comp_uid as fallback)
    path_value = instance_path if instance_path is not None else f'/{comp_uid}'
    instances = ['instances',
                 ['project', '',
                  ['path', path_value,
                   ['reference', comp_ref],
                   ['unit', unit]]]]
    symbol_instance.append(instances)
    
    # Generate labels for pins with net assignments
    labels = []
    no_connects = []  # For DNC pins
    # symbol_def already assigned above when extracting properties
    
    for pin_num, net_name in pins.items():
        # Skip pins with net "NONE" (unconnected)
        if net_name == "NONE":
            continue
        
        # Get pin info from symbol definition
        pin_info = extract_pin_info_from_symbol(symbol_def, pin_num, unit=unit, body_style=body_style)
        if pin_info:
            x_off, y_off, pin_rot = pin_info
            # Transform pin position
            
            pin_pos = transform_pin_coordinate((x_off, y_off, pin_rot), 
                                               tuple(symbol_pos), 
                                               symbol_rot, mirror)
            # For DNC pins, create a no_connect element instead of a label
            if net_name == "DNC":
                no_connect = {
                    'type': 'noconnect',
                    'at': list(pin_pos),
                    'uid': str(uuid.uuid4())
                }
                no_connects.append(no_connect)
            else:
                # Create label at pin position for normal nets
                label = {
                    'type': 'label',
                    'name': net_name,
                    'at': list(pin_pos)
                }
                labels.append(label)
    
    return symbol_instance, labels, no_connects


def convert_wire(wire: Dict[str, Any]) -> Optional[List]:
    """Convert wire from trace JSON to KiCad wire element."""
    points = wire.get('points', [])
    if len(points) < 2:
        return None
    
    wire_uid = wire.get('uid')
    if not wire_uid:
        wire_uid = str(uuid.uuid4())
    
    # Build pts structure
    pts = ['pts']
    for point in points:
        pts.append(['xy', point[0], point[1]])
    
    wire_elem = ['wire',
                 pts,
                 ['stroke', ['width', 0], ['type', 'default']],
                 ['uuid', wire_uid]]
    
    return wire_elem


def convert_junction(junction: Dict[str, Any]) -> Optional[List]:
    """Convert junction from trace JSON to KiCad junction element."""
    pos = junction.get('at')
    if not pos or len(pos) < 2:
        return None
    
    junc_uid = junction.get('uid')
    if not junc_uid:
        junc_uid = str(uuid.uuid4())
    
    junction_elem = ['junction',
                    ['at', pos[0], pos[1]],
                    ['diameter', 0],
                    ['color', 0, 0, 0, 0],
                    ['uuid', junc_uid]]
    
    return junction_elem


def convert_label(label: Dict[str, Any]) -> Optional[List]:
    """Convert label from trace JSON to KiCad label element."""
    label_name = label.get('name')
    if not label_name:
        return None
    
    pos = label.get('at')
    if not pos or len(pos) < 2:
        return None
    
    label_uid = str(uuid.uuid4())
    label_elem = ['label', label_name,
                  ['at', pos[0], pos[1], 0],
                  ['effects',
                   ['font', ['size', 1.27, 1.27]],
                   ['justify', 'left', 'bottom']],
                  ['uuid', label_uid]]
    
    return label_elem


def convert_noconnect(noconnect: Dict[str, Any]) -> Optional[List]:
    """Convert noconnect from trace JSON to KiCad no_connect element."""
    pos = noconnect.get('at')
    if not pos or len(pos) < 2:
        return None
    
    noconnect_uid = noconnect.get('uid')
    if not noconnect_uid:
        noconnect_uid = str(uuid.uuid4())
    
    noconnect_elem = ['no_connect',
                      ['at', pos[0], pos[1]],
                      ['uuid', noconnect_uid]]
    
    return noconnect_elem


def convert_glabel(glabel: Dict[str, Any]) -> Optional[List]:
    """Convert glabel from trace JSON to KiCad global_label element."""
    label_name = glabel.get('name')
    if not label_name:
        return None
    
    pos = glabel.get('at')
    if not pos or len(pos) < 2:
        return None
    
    rot = glabel.get('rot', 0)
    shape = glabel.get('shape', 'input')
    if shape not in VALID_LABEL_SHAPES:
        raise ValueError(
            f"Invalid glabel shape '{shape}' for label '{label_name}'. "
            f"Must be one of {sorted(VALID_LABEL_SHAPES)}"
        )
    
    # Use stored UUID if present, otherwise generate new one
    glabel_uid = glabel.get('uid')
    if not glabel_uid:
        glabel_uid = str(uuid.uuid4())
    
    glabel_elem = ['global_label', label_name,
                   ['shape', shape],
                   ['at', pos[0], pos[1], rot],
                   ['fields_autoplaced', 'yes'],
                   ['effects',
                    ['font', ['size', 1.27, 1.27]],
                    ['justify', 'left']],
                   ['uuid', glabel_uid]]
    
    # Add Intersheetrefs property if present
    props = glabel.get('props', {})
    if props:
        intersheetrefs = props.get('intersheetrefs', '${INTERSHEET_REFS}')
        glabel_elem.append(['property', 'Intersheetrefs', intersheetrefs,
                           ['at', pos[0] + 6.8557, pos[1], 0],
                           ['hide', 'yes'],
                           ['show_name', 'no'],
                           ['do_not_autoplace', 'no'],
                           ['effects',
                            ['font', ['size', 1.27, 1.27]],
                            ['justify', 'left']]])
    
    return glabel_elem


def convert_hier_label(hier: Dict[str, Any]) -> Optional[List]:
    """Convert hier from trace JSON to KiCad hierarchical_label element."""
    label_name = hier.get('name')
    if not label_name:
        return None
    
    pos = hier.get('at')
    if not pos or len(pos) < 2:
        return None
    
    rot = hier.get('rot', 0)
    shape = hier.get('shape', 'input')
    if shape not in VALID_LABEL_SHAPES:
        raise ValueError(
            f"Invalid hier label shape '{shape}' for label '{label_name}'. "
            f"Must be one of {sorted(VALID_LABEL_SHAPES)}"
        )
    
    # Use stored UUID if present, otherwise generate new one
    hier_uid = hier.get('uid')
    if not hier_uid:
        hier_uid = str(uuid.uuid4())
    
    hier_elem = ['hierarchical_label', label_name,
                 ['shape', shape],
                 ['at', pos[0], pos[1], rot],
                 ['effects',
                  ['font', ['size', 1.27, 1.27]],
                  ['justify', 'left']],
                 ['uuid', hier_uid]]
    
    return hier_elem


def build_stroke(stroke: Dict[str, Any]) -> List:
    """Build stroke S-expression from trace JSON."""
    width = stroke.get('width', 0)
    stroke_type = stroke.get('type', 'default')
    return ['stroke', ['width', width], ['type', stroke_type]]


def build_fill(fill: Dict[str, Any]) -> List:
    """Build fill S-expression from trace JSON."""
    fill_type = fill.get('type', 'none')
    fill_elem = ['fill', ['type', fill_type]]
    
    if 'color' in fill:
        color = fill['color']
        if len(color) >= 4:
            fill_elem.append(['color', color[0], color[1], color[2], color[3]])
    
    return fill_elem


def convert_bus(bus: Dict[str, Any]) -> Optional[List]:
    """Convert bus from trace JSON to KiCad bus element."""
    points = bus.get('points', [])
    if len(points) < 2:
        return None
    
    bus_uid = bus.get('uid')
    if not bus_uid:
        bus_uid = str(uuid.uuid4())
    
    # Build pts structure
    pts = ['pts']
    for point in points:
        pts.append(['xy', point[0], point[1]])
    
    bus_elem = ['bus',
                pts,
                build_stroke(bus.get('stroke', {'width': 0, 'type': 'default'})),
                ['uuid', bus_uid]]
    
    return bus_elem


def convert_polyline(polyline: Dict[str, Any]) -> Optional[List]:
    """Convert polyline from trace JSON to KiCad polyline element."""
    points = polyline.get('points', [])
    if len(points) < 2:
        return None
    
    polyline_uid = polyline.get('uid')
    if not polyline_uid:
        polyline_uid = str(uuid.uuid4())
    
    # Build pts structure
    pts = ['pts']
    for point in points:
        pts.append(['xy', point[0], point[1]])
    
    polyline_elem = ['polyline',
                    pts,
                    build_stroke(polyline.get('stroke', {'width': 0, 'type': 'default'})),
                    ['uuid', polyline_uid]]
    
    return polyline_elem


def convert_rectangle(rectangle: Dict[str, Any]) -> Optional[List]:
    """Convert rectangle from trace JSON to KiCad rectangle element."""
    start = rectangle.get('start')
    end = rectangle.get('end')
    if not start or not end or len(start) < 2 or len(end) < 2:
        return None
    
    rect_uid = rectangle.get('uid')
    if not rect_uid:
        rect_uid = str(uuid.uuid4())
    
    rect_elem = ['rectangle',
                ['start', start[0], start[1]],
                ['end', end[0], end[1]],
                build_stroke(rectangle.get('stroke', {'width': 0, 'type': 'default'})),
                build_fill(rectangle.get('fill', {'type': 'none'})),
                ['uuid', rect_uid]]
    
    return rect_elem


def convert_arc(arc: Dict[str, Any]) -> Optional[List]:
    """Convert arc from trace JSON to KiCad arc element."""
    start = arc.get('start')
    mid = arc.get('mid')
    end = arc.get('end')
    if not start or not mid or not end or len(start) < 2 or len(mid) < 2 or len(end) < 2:
        return None
    
    arc_uid = arc.get('uid')
    if not arc_uid:
        arc_uid = str(uuid.uuid4())
    
    arc_elem = ['arc',
                ['start', start[0], start[1]],
                ['mid', mid[0], mid[1]],
                ['end', end[0], end[1]],
                build_stroke(arc.get('stroke', {'width': 0, 'type': 'default'})),
                build_fill(arc.get('fill', {'type': 'none'})),
                ['uuid', arc_uid]]
    
    return arc_elem


def convert_bezier(bezier: Dict[str, Any]) -> Optional[List]:
    """Convert bezier from trace JSON to KiCad bezier element."""
    points = bezier.get('points', [])
    if len(points) < 2:
        return None
    
    bezier_uid = bezier.get('uid')
    if not bezier_uid:
        bezier_uid = str(uuid.uuid4())
    
    # Build pts structure
    pts = ['pts']
    for point in points:
        pts.append(['xy', point[0], point[1]])
    
    bezier_elem = ['bezier',
                  pts,
                  build_stroke(bezier.get('stroke', {'width': 0, 'type': 'default'})),
                  build_fill(bezier.get('fill', {'type': 'none'})),
                  ['uuid', bezier_uid]]
    
    return bezier_elem


def convert_circle(circle: Dict[str, Any]) -> Optional[List]:
    """Convert circle from trace JSON to KiCad circle element."""
    center = circle.get('center')
    radius = circle.get('radius')
    if not center or len(center) < 2 or radius is None:
        return None
    
    circle_uid = circle.get('uid')
    if not circle_uid:
        circle_uid = str(uuid.uuid4())
    
    circle_elem = ['circle',
                  ['center', center[0], center[1]],
                  ['radius', radius],
                  build_stroke(circle.get('stroke', {'width': 0, 'type': 'default'})),
                  build_fill(circle.get('fill', {'type': 'none'})),
                  ['uuid', circle_uid]]
    
    return circle_elem


def convert_text_box(text_box: Dict[str, Any]) -> Optional[List]:
    """Convert text_box from trace JSON to KiCad text_box element."""
    text = text_box.get('text')
    at = text_box.get('at')
    size = text_box.get('size')
    if not text or not at or not size or len(at) < 2 or len(size) < 2:
        return None
    
    text_box_uid = text_box.get('uid')
    if not text_box_uid:
        text_box_uid = str(uuid.uuid4())
    
    # Build at with optional rotation
    at_elem = ['at', at[0], at[1]]
    if len(at) >= 3:
        at_elem.append(at[2])
    else:
        at_elem.append(0)
    
    text_box_elem = ['text_box', text,
                    ['exclude_from_sim', 'no'],
                    at_elem,
                    ['size', size[0], size[1]]]
    
    # Optional margins
    if 'margins' in text_box and len(text_box['margins']) >= 4:
        margins = text_box['margins']
        text_box_elem.append(['margins', margins[0], margins[1], margins[2], margins[3]])
    
    text_box_elem.append(build_stroke(text_box.get('stroke', {'width': 0, 'type': 'solid'})))
    text_box_elem.append(build_fill(text_box.get('fill', {'type': 'none'})))
    
    # Optional effects
    if 'effects' in text_box and text_box['effects']:
        effects_elem = ['effects']
        effects = text_box['effects']
        
        # Build font effects
        if 'font_size' in effects:
            font_size = float(effects['font_size']) if isinstance(effects['font_size'], str) else effects['font_size']
            effects_elem.append(['font', ['size', font_size, font_size]])
        
        # Build justify effects
        if 'justify' in effects:
            justify_parts = effects['justify'].split()
            if len(justify_parts) >= 2:
                effects_elem.append(['justify', justify_parts[0], justify_parts[1]])
            elif len(justify_parts) >= 1:
                effects_elem.append(['justify', justify_parts[0], 'top'])
        
        if len(effects_elem) > 1:  # If we added any effects
            text_box_elem.append(effects_elem)
        else:
            # Default effects
            text_box_elem.append(['effects',
                                ['font', ['size', 1.27, 1.27]],
                                ['justify', 'left', 'top']])
    else:
        # Default effects
        text_box_elem.append(['effects',
                            ['font', ['size', 1.27, 1.27]],
                            ['justify', 'left', 'top']])
    
    text_box_elem.append(['uuid', text_box_uid])
    
    return text_box_elem


def convert_bus_entry(bus_entry: Dict[str, Any]) -> Optional[List]:
    """Convert bus_entry from trace JSON to KiCad bus_entry element."""
    at = bus_entry.get('at')
    size = bus_entry.get('size')
    if not at or not size or len(at) < 2 or len(size) < 2:
        return None
    
    bus_entry_uid = bus_entry.get('uid')
    if not bus_entry_uid:
        bus_entry_uid = str(uuid.uuid4())
    
    bus_entry_elem = ['bus_entry',
                     ['at', at[0], at[1]],
                     ['size', size[0], size[1]],
                     build_stroke(bus_entry.get('stroke', {'width': 0, 'type': 'default'})),
                     ['uuid', bus_entry_uid]]
    
    return bus_entry_elem


def convert_sheet(sheet: Dict[str, Any], instance_path: str = '/') -> Tuple[Optional[List], List[Dict[str, Any]]]:
    """Convert sheet from trace JSON to KiCad sheet element and pin labels.
    
    Returns:
        Tuple of (sheet_elem, pin_labels) where pin_labels are label dicts
        generated at sheet pin positions for their net names (analogous to
        component pin label generation).
    """
    sheet_name = sheet.get('name', 'SHEET')
    file_value = sheet.get('file')
    if not file_value:
        return None, []
    
    sheet_uid = sheet.get('uid')
    if not sheet_uid:
        sheet_uid = str(uuid.uuid4())
    
    pos = sheet.get('at', [0, 0])
    size = sheet.get('size', [76.2, 50.8])
    
    # Build sheet element
    sheet_elem = ['sheet',
                  ['at', pos[0], pos[1]],
                  ['size', size[0], size[1]],
                  ['exclude_from_sim', 'no'],
                  ['in_bom', 'yes'],
                  ['on_board', 'yes'],
                  ['dnp', 'no'],
                  ['fields_autoplaced', 'yes'],
                  ['stroke',
                   ['width', 0.1524],
                   ['type', 'solid']],
                  ['fill',
                   ['color', 0, 0, 0, 0]],
                  ['uuid', sheet_uid]]
    
    # Add properties
    props = sheet.get('props', {})
    sheetname_prop = props.get('sheetname', sheet_name)
    sheetfile_prop = props.get('sheetfile', file_value)
    
    # Add Sheetname property
    sheet_elem.append(['property', 'Sheetname', sheetname_prop,
                      ['at', pos[0], pos[1] - 0.7116, 0],
                      ['show_name', 'no'],
                      ['do_not_autoplace', 'no'],
                      ['effects',
                       ['font', ['size', 1.27, 1.27]],
                       ['justify', 'left', 'bottom']]])
    
    # Add Sheetfile property
    sheet_elem.append(['property', 'Sheetfile', sheetfile_prop,
                      ['at', pos[0], pos[1] + size[1] / 2, 0],
                      ['show_name', 'no'],
                      ['do_not_autoplace', 'no'],
                      ['effects',
                       ['font', ['size', 1.27, 1.27]],
                       ['justify', 'left', 'top']]])
    
    # Add sheet pins (ins and outs)
    ins = sheet.get('ins', {})
    outs = sheet.get('outs', {})
    
    # Calculate sheet edges
    left_edge = pos[0]
    right_edge = pos[0] + size[0]
    top_edge = pos[1]
    bottom_edge = pos[1] + size[1]
    
    # Helper function to validate and fix pin placement based on rotation
    # IMPORTANT: In KiCad, the rotation angle determines which side the pin is on:
    #   rot 0   → RIGHT side (x = right_edge)
    #   rot 90  → TOP side (y = top_edge)
    #   rot 180 → LEFT side (x = left_edge)
    #   rot 270 → BOTTOM side (y = bottom_edge)
    def validate_and_fix_pin_placement(pin_name, pin_x, pin_y, explicit_rot, default_rot, pin_type):
        """
        Validate pin placement and fix coordinates to match rotation.
        Rotation takes priority over coordinates - if they conflict, warn and snap to correct edge.
        
        Args:
            pin_name: Name of the pin (for warning messages)
            pin_x, pin_y: Specified coordinates
            explicit_rot: Rotation value from trace_sch (None if not specified)
            default_rot: Default rotation if not specified (180 for ins, 0 for outs)
            pin_type: 'input' or 'output' (for warning messages)
        
        Returns:
            (final_x, final_y, final_rot, warnings) - corrected coordinates and rotation
        """
        warnings = []
        tolerance = 2.54  # 100 mils tolerance for edge detection
        
        # Determine which edge the coordinates suggest
        coord_suggests_edge = None
        if abs(pin_x - left_edge) < tolerance:
            coord_suggests_edge = 'left'
            coord_expected_rot = 180
        elif abs(pin_x - right_edge) < tolerance:
            coord_suggests_edge = 'right'
            coord_expected_rot = 0
        elif abs(pin_y - top_edge) < tolerance:
            coord_suggests_edge = 'top'
            coord_expected_rot = 90
        elif abs(pin_y - bottom_edge) < tolerance:
            coord_suggests_edge = 'bottom'
            coord_expected_rot = 270
        else:
            coord_suggests_edge = None
            coord_expected_rot = None
        
        # If explicit rotation is provided, it takes priority
        if explicit_rot is not None:
            final_rot = explicit_rot
            
            # Check if coordinates conflict with the explicit rotation
            rot_to_edge = {0: 'right', 90: 'top', 180: 'left', 270: 'bottom'}
            rot_edge = rot_to_edge.get(explicit_rot % 360, None)
            
            if coord_suggests_edge is not None and coord_suggests_edge != rot_edge:
                # Conflict! Coordinates suggest one edge, rotation says another
                warnings.append(
                    f"WARNING: Sheet pin '{pin_name}' ({pin_type}): coordinates ({pin_x}, {pin_y}) suggest "
                    f"{coord_suggests_edge} edge (rot {coord_expected_rot}), but rot {explicit_rot} specifies "
                    f"{rot_edge} edge. Using rot {explicit_rot} and snapping to {rot_edge} edge."
                )
            
            # Snap coordinates to match the rotation
            if explicit_rot == 180:  # LEFT edge
                final_x = left_edge
                final_y = pin_y
            elif explicit_rot == 0:  # RIGHT edge
                final_x = right_edge
                final_y = pin_y
            elif explicit_rot == 90:  # TOP edge
                final_x = pin_x
                final_y = top_edge
            elif explicit_rot == 270:  # BOTTOM edge
                final_x = pin_x
                final_y = bottom_edge
            else:
                # Non-standard rotation, use coordinates as-is
                final_x = pin_x
                final_y = pin_y
        else:
            # No explicit rotation - infer from coordinates or use default
            if coord_suggests_edge is not None:
                final_rot = coord_expected_rot
                # Snap to exact edge
                if coord_suggests_edge == 'left':
                    final_x = left_edge
                    final_y = pin_y
                elif coord_suggests_edge == 'right':
                    final_x = right_edge
                    final_y = pin_y
                elif coord_suggests_edge == 'top':
                    final_x = pin_x
                    final_y = top_edge
                elif coord_suggests_edge == 'bottom':
                    final_x = pin_x
                    final_y = bottom_edge
            else:
                # Coordinates don't clearly indicate an edge, use default
                final_rot = default_rot
                # Snap to default edge
                if default_rot == 180:  # LEFT edge (default for inputs)
                    final_x = left_edge
                    final_y = pin_y
                elif default_rot == 0:  # RIGHT edge (default for outputs)
                    final_x = right_edge
                    final_y = pin_y
                else:
                    final_x = pin_x
                    final_y = pin_y
                
                warnings.append(
                    f"WARNING: Sheet pin '{pin_name}' ({pin_type}): coordinates ({pin_x}, {pin_y}) don't clearly "
                    f"indicate a sheet edge. Using default rot {default_rot} and snapping to "
                    f"{'left' if default_rot == 180 else 'right'} edge."
                )
        
        return final_x, final_y, final_rot, warnings
    
    pin_labels = []
    
    # Convert ins to input pins
    for pin_name, pin_value in ins.items():
        # Handle both dict format (with coordinates) and string format (backward compatibility)
        if isinstance(pin_value, dict):
            net_name = pin_value.get('net', pin_name)
            pin_coord = pin_value.get('at', [left_edge, pos[1] + 10.16])
            pin_x = pin_coord[0]
            pin_y = pin_coord[1] if len(pin_coord) > 1 else pos[1] + 10.16
            explicit_rot = pin_value.get('rot', None)  # None if not specified
            
            # Validate and fix pin placement - rotation takes priority
            final_x, final_y, pin_rot, warnings = validate_and_fix_pin_placement(
                pin_name, pin_x, pin_y, explicit_rot, default_rot=180, pin_type='input'
            )
            
            # Log any warnings
            for warning in warnings:
                logger.warning(warning)
            
            pin_coord = [final_x, final_y]
        else:
            net_name = pin_value if isinstance(pin_value, str) else pin_name
            # Backward compatibility: use default position on left edge
            # rot 180 places pin on LEFT side in KiCad
            pin_coord = [left_edge, pos[1] + 10.16]
            pin_rot = 180
        
        pin_elem = ['pin', pin_name, 'input',
                   ['at', pin_coord[0], pin_coord[1], pin_rot],
                   ['uuid', str(uuid.uuid4())],
                   ['effects',
                    ['font', ['size', 1.27, 1.27]],
                    ['justify', 'left']]]
        sheet_elem.append(pin_elem)
        
        if net_name and net_name not in ("NONE", "DNC"):
            pin_labels.append({
                'type': 'label',
                'name': net_name,
                'at': list(pin_coord)
            })
    
    # Convert outs to output pins
    for pin_name, pin_value in outs.items():
        # Handle both dict format (with coordinates) and string format (backward compatibility)
        if isinstance(pin_value, dict):
            net_name = pin_value.get('net', pin_name)
            pin_coord = pin_value.get('at', [right_edge, pos[1] + 10.16])
            pin_x = pin_coord[0]
            pin_y = pin_coord[1] if len(pin_coord) > 1 else pos[1] + 10.16
            explicit_rot = pin_value.get('rot', None)  # None if not specified
            
            # Validate and fix pin placement - rotation takes priority
            final_x, final_y, pin_rot, warnings = validate_and_fix_pin_placement(
                pin_name, pin_x, pin_y, explicit_rot, default_rot=0, pin_type='output'
            )
            
            # Log any warnings
            for warning in warnings:
                logger.warning(warning)
            
            pin_coord = [final_x, final_y]
        else:
            net_name = pin_value if isinstance(pin_value, str) else pin_name
            # Backward compatibility: use default position on right edge
            # rot 0 places pin on RIGHT side in KiCad
            pin_coord = [right_edge, pos[1] + 10.16]
            pin_rot = 0
        
        pin_elem = ['pin', pin_name, 'output',
                   ['at', pin_coord[0], pin_coord[1], pin_rot],
                   ['uuid', str(uuid.uuid4())],
                   ['effects',
                    ['font', ['size', 1.27, 1.27]],
                    ['justify', 'right']]]
        sheet_elem.append(pin_elem)
        
        if net_name and net_name not in ("NONE", "DNC"):
            pin_labels.append({
                'type': 'label',
                'name': net_name,
                'at': list(pin_coord)
            })
    
    # Add instances
    # For now, use a simple instance path - in a real implementation, this would track hierarchy
    # Check for page in top-level sheet dict first, then fall back to props
    page_num = sheet.get('page', props.get('page', '1'))
    sheet_elem.append(['instances',
                      ['project', '',
                       ['path', instance_path,
                        ['page', str(page_num)]]]])
    
    return sheet_elem, pin_labels


# =============================================================================
# cwire Processing
# =============================================================================

def resolve_cwire_endpoint(endpoint: Dict[str, Any], 
                           components: List[Dict[str, Any]], 
                           junctions: List[Dict[str, Any]],
                           lib_symbols_cache: Dict[str, List],
                           all_labels: Optional[List[Dict[str, Any]]] = None,
                           sheets: Optional[List[Dict[str, Any]]] = None) -> Optional[Tuple[float, float, str]]:
    """
    Resolve a cwire endpoint to coordinates.
    
    Args:
        endpoint: Endpoint dict with 'type' ('pin', 'junction', 'label', 'glabel', 'hier') and relevant fields
        components: List of component dicts
        junctions: List of junction dicts
        lib_symbols_cache: Cache of loaded library symbols
        all_labels: List of label/glabel/hier dicts from trace_json
        sheets: List of sheet dicts (for resolving sheet pin endpoints like XOR1.OUT_SUM)
    
    Returns:
        (x, y, net_name) tuple or None if endpoint cannot be resolved
    """
    endpoint_type = endpoint.get('type')
    
    if endpoint_type == 'junction':
        junction_id = endpoint.get('id')
        for junction in junctions:
            if junction.get('id') == junction_id:
                at = junction.get('at', [0, 0])
                return (at[0], at[1], None)
        return None
    
    elif endpoint_type in ('label', 'glabel', 'hier'):
        label_id = endpoint.get('id')
        if all_labels:
            for lbl in all_labels:
                if lbl.get('id') == label_id and lbl.get('type') == endpoint_type:
                    at = lbl.get('at', [0, 0])
                    net_name = lbl.get('name')
                    return (at[0], at[1], net_name)
        return None
    
    elif endpoint_type == 'pin':
        ref = endpoint.get('ref')
        pin = endpoint.get('pin')
        
        # Try components first
        for comp in components:
            if comp.get('ref') == ref:
                lib_id = comp.get('symbol')
                if not lib_id or lib_id not in lib_symbols_cache:
                    continue
                
                symbol_def = lib_symbols_cache[lib_id]
                symbol_pos = comp.get('at', [0, 0])
                symbol_rot = comp.get('rot', 0)
                comp_mirror = comp.get('mirror')
                unit = comp.get('unit', 1)
                body_style = comp.get('body_style', 1)
                
                pin_info = extract_pin_info_from_symbol(symbol_def, pin, unit=unit, body_style=body_style)
                if pin_info:
                    x_off, y_off, pin_rot = pin_info
                    pin_pos = transform_pin_coordinate((x_off, y_off, pin_rot), 
                                                       tuple(symbol_pos), 
                                                       symbol_rot, comp_mirror)
                    pins = comp.get('pins', {})
                    net_name = pins.get(pin) or pins.get(str(pin))
                    return (pin_pos[0], pin_pos[1], net_name)
        
        # Fall back to sheets: treat sheet pins like component pins
        if sheets:
            pin_str = str(pin)
            for s in sheets:
                if s.get('name') == ref:
                    for pin_dict_key in ('ins', 'outs'):
                        pin_map = s.get(pin_dict_key, {})
                        pin_value = pin_map.get(pin_str) or pin_map.get(pin)
                        if pin_value is not None:
                            if isinstance(pin_value, dict):
                                pin_at = pin_value.get('at')
                                net_name = pin_value.get('net', pin_str)
                            else:
                                pin_at = None
                                net_name = pin_value if isinstance(pin_value, str) else pin_str
                            if pin_at and len(pin_at) >= 2:
                                return (pin_at[0], pin_at[1], net_name)
        return None
    
    return None


def process_cwires(cwires: List[Dict[str, Any]], 
                   cwiredefs: List[Dict[str, Any]],
                   components: List[Dict[str, Any]], 
                   junctions: List[Dict[str, Any]],
                   lib_symbols_cache: Dict[str, List],
                   all_labels: Optional[List[Dict[str, Any]]] = None,
                   sheets: Optional[List[Dict[str, Any]]] = None) -> Tuple[List[Tuple[Tuple[float, float], Tuple[float, float], Optional[str]]], List[Dict[str, Any]], List[Dict[str, Any]], Set[Tuple[str, Any]]]:
    """
    Process cwire statements and resolve them to coordinate pairs for routing.
    
    Also collects label/glabel/hier elements that need to be generated from cwire endpoints.
    
    Returns:
        Tuple of:
        - List of (from_coord, to_coord, net_name) tuples to route
        - List of wire dicts generated from cwiredefs (bypass autorouting)
        - List of label dicts to generate in kicad_sch from cwire label endpoints
    """
    cwiredef_map: Dict[str, Dict[str, Any]] = {}
    for cwiredef in cwiredefs:
        ref = cwiredef.get('ref')
        if ref:
            cwiredef_map[ref] = cwiredef
    
    cwire_routing_pairs: List[Tuple[Tuple[float, float], Tuple[float, float], Optional[str]]] = []
    cwiredef_wires: List[Dict[str, Any]] = []
    cwire_labels: List[Dict[str, Any]] = []
    pins_with_label_cwires: Set[Tuple[str, Any]] = set()
    
    if all_labels is None:
        all_labels = []
    
    for cwire in cwires:
        cwire_ref = cwire.get('ref')
        from_endpoint = cwire.get('from', {})
        to_endpoint = cwire.get('to', {})
        
        cwiredef = cwiredef_map.get(cwire_ref) if cwire_ref else None
        
        # Collect label endpoints that need to be generated in kicad_sch
        for ep in (from_endpoint, to_endpoint):
            ep_type = ep.get('type')
            if ep_type in ('label', 'glabel', 'hier'):
                ep_id = ep.get('id')
                for lbl in all_labels:
                    if lbl.get('id') == ep_id and lbl.get('type') == ep_type:
                        cwire_labels.append(lbl)
                        break
        
        # Track which pins have label cwires (to suppress duplicate label generation)
        for ep, other_ep in ((from_endpoint, to_endpoint), (to_endpoint, from_endpoint)):
            if ep.get('type') == 'pin' and other_ep.get('type') in ('label', 'glabel', 'hier'):
                pins_with_label_cwires.add((ep.get('ref'), ep.get('pin')))
        
        if cwiredef:
            points = cwiredef.get('points', [])
            if len(points) >= 2:
                for i in range(len(points) - 1):
                    wire = {
                        'type': 'wire',
                        'points': [list(points[i]), list(points[i + 1])],
                        'uid': str(uuid.uuid4())
                    }
                    cwiredef_wires.append(wire)
                continue
        
        from_resolved = resolve_cwire_endpoint(from_endpoint, components, junctions, lib_symbols_cache, all_labels, sheets)
        to_resolved = resolve_cwire_endpoint(to_endpoint, components, junctions, lib_symbols_cache, all_labels, sheets)
        
        if not from_resolved or not to_resolved:
            continue
        
        from_x, from_y, from_net = from_resolved
        to_x, to_y, to_net = to_resolved
        
        net_name = cwire.get('net')
        if not net_name:
            if from_net and from_net not in ('NONE', 'DNC'):
                net_name = from_net
            elif to_net and to_net not in ('NONE', 'DNC'):
                net_name = to_net
        
        cwire_routing_pairs.append(((from_x, from_y), (to_x, to_y), net_name))
    
    return cwire_routing_pairs, cwiredef_wires, cwire_labels, pins_with_label_cwires


# =============================================================================
# Main Conversion Function
# =============================================================================

def trace_json_to_sexp(trace_json: List[Dict[str, Any]], 
                       existing_sch_content: Optional[str] = None,
                       symbol_paths: Union[str, List[str]] = None) -> str:
    """
    Convert trace JSON format to KiCad S-expression format.
    
    Args:
        trace_json: List of trace JSON statements
        existing_sch_content: Optional content of existing kicad_sch file to merge with.
                             If None, generates from scratch (backward compatible).
        symbol_paths: Path(s) to KiCad symbols directory(ies). Can be a single string or list of strings.
                     If None, uses default KICAD_SYMBOL_PATH or environment variable.
    
    Returns:
        Complete kicad_sch S-expression string
    """
    import time
    start_time = time.time()
    
    # Parse existing kicad_sch if provided
    existing_sch_data = None
    element_maps = {
        'symbol': {},
        'wire': {},
        'junction': {},
        'label': {},
        'no_connect': {},
        'global_label': {},
        'hierarchical_label': {},
        'sheet': {}
    }
    preserved_elements = []  # Elements not in trace_sch.ebnf
    existing_lib_symbols = {}  # Existing lib_symbols from kicad_sch
    
    # Elements defined in trace_sch.ebnf that we should track
    trace_sch_element_types = {'symbol', 'wire', 'junction', 'label', 'no_connect',
                               'global_label', 'hierarchical_label', 'sheet', 'lib_symbols',
                               'sheet_instances', 'version', 'generator', 'generator_version',
                               'uuid', 'paper', 'title_block', 'embedded_fonts', 'bus', 'polyline',
                               'rectangle', 'arc', 'bezier', 'circle', 'text_box', 'bus_entry'}
    
    if existing_sch_content:
        try:
            existing_sch_data = parse_sexp(existing_sch_content)
            if isinstance(existing_sch_data, list) and len(existing_sch_data) > 0 and existing_sch_data[0] == 'kicad_sch':
                # Build element maps by UUID for trace_sch elements
                all_elements = existing_sch_data[1:] if len(existing_sch_data) > 1 else []
                
                for elem in all_elements:
                    if not isinstance(elem, list) or len(elem) == 0:
                        continue
                    
                    elem_type = elem[0]
                    uuid_elem = find_element(elem, 'uuid')
                    if uuid_elem:
                        uuid_val = get_atom_value(uuid_elem, 1, None)
                        if uuid_val:
                            if elem_type in element_maps:
                                element_maps[elem_type][uuid_val] = elem
                            elif elem_type not in trace_sch_element_types:
                                # Preserve elements not in trace_sch.ebnf (text_box, table, bitmap, shapes, etc.)
                                preserved_elements.append(elem)
                
                # Extract existing lib_symbols
                lib_symbols_elem = find_element(existing_sch_data, 'lib_symbols')
                if lib_symbols_elem:
                    for lib_symbol_elem in lib_symbols_elem[1:]:
                        if isinstance(lib_symbol_elem, list) and len(lib_symbol_elem) >= 2:
                            lib_id = get_atom_value(lib_symbol_elem, 1, None)
                            if lib_id:
                                existing_lib_symbols[lib_id] = lib_symbol_elem
        except Exception as e:
            logger.warning(f"Warning: Could not parse existing kicad_sch file: {e}")
            existing_sch_data = None
    
    # Default metadata values for eeschema (used when no existing_sch_content)
    # When merging, version/generator/generator_version are taken from existing file.
    EESCHEMA_VERSION = 20260101
    EESCHEMA_GENERATOR = 'eeschema'
    EESCHEMA_GENERATOR_VERSION = '10.0'

    # Extract other metadata from trace_json (file_uid, paper only)
    metadata = {}
    other_statements = []

    for item in trace_json:
        item_type = item.get('type')
        if item_type == 'file_uid':
            metadata['uuid'] = item.get('value')
        elif item_type == 'paper':
            metadata['paper'] = item.get('value', 'A4')
        elif item_type == 'title_block':
            metadata['title_block'] = item
        elif item_type in ('kicad_ver', 'kicad_gen', 'kicad_gen_ver'):
            # Ignore these - we use existing file or default values
            pass
        else:
            other_statements.append(item)
    
    # Use metadata values - prefer trace_json, fallback to existing if merging
    out_version = EESCHEMA_VERSION
    out_generator = EESCHEMA_GENERATOR
    out_generator_version = EESCHEMA_GENERATOR_VERSION

    if existing_sch_data:
        existing_uuid_elem = find_element(existing_sch_data, 'uuid')
        if existing_uuid_elem and 'uuid' not in metadata:
            metadata['uuid'] = get_atom_value(existing_uuid_elem, 1, None)
        
        paper_elem = find_element(existing_sch_data, 'paper')
        if paper_elem and 'paper' not in metadata:
            metadata['paper'] = get_atom_value(paper_elem, 1, 'A4')

        version_elem = find_element(existing_sch_data, 'version')
        if version_elem is not None:
            val = get_atom_value(version_elem, 1, None)
            if val is not None:
                out_version = int(val) if isinstance(val, str) and val.isdigit() else val
        generator_elem = find_element(existing_sch_data, 'generator')
        if generator_elem is not None:
            val = get_atom_value(generator_elem, 1, None)
            if val is not None:
                out_generator = str(val)
        generator_version_elem = find_element(existing_sch_data, 'generator_version')
        if generator_version_elem is not None:
            val = get_atom_value(generator_version_elem, 1, None)
            if val is not None:
                out_generator_version = str(val)
    
    # Use metadata values or defaults
    doc_uuid = metadata.get('uuid', str(uuid.uuid4()))
    # Use the document UUID as the consistent path for all instances
    instance_path = f'/{doc_uuid}'
    kicad_sch = ['kicad_sch',
                 ['version', out_version],
                 ['generator', out_generator],
                 ['generator_version', out_generator_version],
                 ['uuid', doc_uuid],
                 ['paper', metadata.get('paper', 'A4')]]
    
    # Add title_block if present (from trace_json or existing file)
    tb_data = metadata.get('title_block')
    if not tb_data and existing_sch_data:
        existing_tb = find_element(existing_sch_data, 'title_block')
        if existing_tb:
            kicad_sch.append(existing_tb)
    elif tb_data:
        tb_sexp = ['title_block']
        if tb_data.get('title'):
            tb_sexp.append(['title', tb_data['title']])
        if tb_data.get('date'):
            tb_sexp.append(['date', tb_data['date']])
        if tb_data.get('rev'):
            tb_sexp.append(['rev', tb_data['rev']])
        if tb_data.get('company'):
            tb_sexp.append(['company', tb_data['company']])
        comments = tb_data.get('comments', {})
        for num in sorted(comments.keys()):
            tb_sexp.append(['comment', int(num), comments[num]])
        if len(tb_sexp) > 1:
            kicad_sch.append(tb_sexp)
    
    # Collect unique lib_ids from components
    lib_symbols_cache = {}
    components = []
    wires = []
    junctions = []
    labels = []
    noconnects = []  # Standalone noconnect statements
    net_labels = []  # Labels generated from pin nets
    
    # Separate elements by type (from other_statements, not trace_json)
    glabels = []
    hier_labels = []
    sheets = []
    buses = []
    polylines = []
    rectangles = []
    arcs = []
    beziers = []
    circles = []
    text_boxes = []
    bus_entries = []
    cwires = []  # Component wires (high-level connectivity)
    cwiredefs = []  # Wire path definitions for cwires
    for item in other_statements:
        item_type = item.get('type')
        if item_type == 'component':
            components.append(item)
        elif item_type == 'wire':
            wires.append(item)
        elif item_type == 'cwire':
            cwires.append(item)
        elif item_type == 'cwiredef':
            cwiredefs.append(item)
        elif item_type == 'junction':
            junctions.append(item)
        elif item_type == 'label':
            labels.append(item)
        elif item_type == 'noconnect':
            noconnects.append(item)
        elif item_type == 'glabel':
            glabels.append(item)
        elif item_type == 'hier':
            hier_labels.append(item)
        elif item_type == 'sheet':
            sheets.append(item)
        elif item_type == 'bus':
            buses.append(item)
        elif item_type == 'polyline':
            polylines.append(item)
        elif item_type == 'rectangle':
            rectangles.append(item)
        elif item_type == 'arc':
            arcs.append(item)
        elif item_type == 'bezier':
            beziers.append(item)
        elif item_type == 'circle':
            circles.append(item)
        elif item_type == 'text_box':
            text_boxes.append(item)
        elif item_type == 'bus_entry':
            bus_entries.append(item)
        # Skip 'net' type as per requirements
    
    # Load all required symbol definitions (merge with existing)
    for comp in components:
        lib_id = comp.get('symbol')
        if lib_id:
            if lib_id in existing_lib_symbols:
                lib_symbols_cache[lib_id] = existing_lib_symbols[lib_id]
            elif lib_id not in lib_symbols_cache:
                symbol_def = load_symbol_from_library(lib_id, symbol_paths)
                if symbol_def:
                    lib_symbols_cache[lib_id] = symbol_def
    
    # Merge existing lib_symbols with new ones
    for lib_id, symbol_def in existing_lib_symbols.items():
        if lib_id not in lib_symbols_cache:
            lib_symbols_cache[lib_id] = symbol_def
    
    # Build lib_symbols section
    if lib_symbols_cache:
        lib_symbols = ['lib_symbols']
        for lib_id, symbol_def in lib_symbols_cache.items():
            lib_symbols.append(symbol_def)
        kicad_sch.append(lib_symbols)
    
    # Process components - merge or generate
    processed_uuids = set()
    symbol_instances = []
    no_connects_from_pins = []  # No-connects from DNC pins
    
    for comp in components:
        comp_uid = comp.get('uid')
        if comp_uid and comp_uid in element_maps['symbol']:
            # Merge existing symbol
            existing_symbol = element_maps['symbol'][comp_uid]
            updated_symbol = merge_symbol(existing_symbol, comp, lib_symbols_cache)
            symbol_instances.append(updated_symbol)
            processed_uuids.add(comp_uid)
            
            # Still need to generate pin labels for merged components for routing
            # Get lib_id from the merged symbol
            lib_id = None
            lib_id_elem = find_element(updated_symbol, 'lib_id')
            if lib_id_elem:
                lib_id = get_atom_value(lib_id_elem, 1, None)
            
            if lib_id and lib_id in lib_symbols_cache:
                symbol_def = lib_symbols_cache[lib_id]
                # Get position, unit, body_style from updated symbol
                at_elem = find_element(updated_symbol, 'at')
                unit_elem = find_element(updated_symbol, 'unit')
                body_style_elem = find_element(updated_symbol, 'body_style')
                
                symbol_pos = [0, 0]
                symbol_rot = 0
                unit = 1
                body_style = 1
                
                if at_elem and len(at_elem) >= 3:
                    symbol_pos = [at_elem[1], at_elem[2]]
                    if len(at_elem) >= 4:
                        symbol_rot = at_elem[3]
                if unit_elem:
                    unit = get_atom_value(unit_elem, 1, 1)
                if body_style_elem:
                    body_style = get_atom_value(body_style_elem, 1, 1)
                
                # Get mirror from updated symbol
                mirror_elem = find_element(updated_symbol, 'mirror')
                merge_mirror = get_atom_value(mirror_elem, 1, None) if mirror_elem else None
                
                # Generate labels for pins with net assignments
                pins = comp.get('pins', {})
                for pin_num, net_name in pins.items():
                    if net_name == "NONE":
                        continue
                    
                    pin_info = extract_pin_info_from_symbol(symbol_def, pin_num, unit=unit, body_style=body_style)
                    if pin_info:
                        x_off, y_off, pin_rot = pin_info
                        # Transform pin position
                        pin_pos = transform_pin_coordinate((x_off, y_off, pin_rot), 
                                                          tuple(symbol_pos), 
                                                          symbol_rot, merge_mirror)
                        # For DNC pins, create a no_connect element instead of a label
                        if net_name == "DNC":
                            no_connect = {
                                'type': 'noconnect',
                                'at': list(pin_pos),
                                'uid': str(uuid.uuid4())
                            }
                            no_connects_from_pins.append(no_connect)
                        else:
                            # Create label at pin position for normal nets
                            label = {
                                'type': 'label',
                                'name': net_name,
                                'at': list(pin_pos)
                            }
                            net_labels.append(label)
        else:
            # Generate new symbol instance
            symbol_instance, pin_labels, pin_no_connects = convert_component(comp, lib_symbols_cache, instance_path, symbol_paths)
            if symbol_instance:
                symbol_instances.append(symbol_instance)
            net_labels.extend(pin_labels)
            no_connects_from_pins.extend(pin_no_connects)
    
    # Add symbol instances
    kicad_sch.extend(symbol_instances)
    
    # Process cwires: resolve endpoints to coordinates for routing
    # cwires are high-level connectivity declarations that get autorouted
    # unless they have a matching cwiredef with valid endpoints
    # Net information is included for net-aware routing
    all_trace_labels = labels + glabels + hier_labels
    cwire_routing_pairs, cwiredef_wires, cwire_labels, pins_with_label_cwires = process_cwires(
        cwires, cwiredefs, components, junctions, lib_symbols_cache, all_trace_labels, sheets
    )
    
    # Add wires from cwiredefs directly (these bypass autorouting)
    # Deduplicate: multiple cwires may share the same wire segment
    seen_segments: set = set()
    for w in cwiredef_wires:
        pts = w.get('points', [])
        if len(pts) == 2:
            p1 = (round(pts[0][0], 2), round(pts[0][1], 2))
            p2 = (round(pts[1][0], 2), round(pts[1][1], 2))
            seg = (min(p1, p2), max(p1, p2))
            if seg in seen_segments:
                continue
            seen_segments.add(seg)
        wires.append(w)
    
    # Route cwires using A* algorithm (cwires without valid cwiredefs are autorouted)
    # Net-aware routing: routes can terminate early when reaching same-net wires
    routed_wires, routing_junctions, wires_to_remove = route_cwires(
        cwire_routing_pairs, trace_json, lib_symbols_cache,
        transform_pin_coordinate, extract_pin_info_from_symbol
    )
    
    # Remove wires that were split during routing
    if wires_to_remove:
        wires_to_remove_set = set(wires_to_remove)
        wires = [w for w in wires if w.get('uid') not in wires_to_remove_set]
    
    # Merge routed wires with existing wires
    wires.extend(routed_wires)
    # Add junctions from routing (created at early termination points)
    junctions.extend(routing_junctions)
    
    # Process junctions - merge or generate
    processed_uuids = set()
    for junction in junctions:
        junc_uid = junction.get('uid')
        if junc_uid and junc_uid in element_maps['junction']:
            # Merge existing junction
            existing_junction = element_maps['junction'][junc_uid]
            updated_junction = merge_junction(existing_junction, junction)
            kicad_sch.append(updated_junction)
            processed_uuids.add(junc_uid)
        else:
            # Generate new junction
            junc_elem = convert_junction(junction)
            if junc_elem:
                kicad_sch.append(junc_elem)
    
    # Process wires - merge or generate
    # Note: wires_to_remove contains UIDs of wires that were split during routing
    # These should not be merged from element_maps, as they've been replaced by split wires
    processed_uuids = set()
    wires_to_remove_set = set(wires_to_remove) if wires_to_remove else set()
    for wire in wires:
        wire_uid = wire.get('uid')
        if wire_uid and wire_uid in wires_to_remove_set:
            # Skip wires that were split - they've been replaced by split wires in routed_wires
            continue
        if wire_uid and wire_uid in element_maps['wire']:
            # Merge existing wire
            existing_wire = element_maps['wire'][wire_uid]
            updated_wire = merge_wire(existing_wire, wire)
            kicad_sch.append(updated_wire)
            processed_uuids.add(wire_uid)
        else:
            # Generate new wire
            wire_elem = convert_wire(wire)
            if wire_elem:
                kicad_sch.append(wire_elem)
    
    # Process labels (from trace JSON and generated from nets)
    # cwire_labels are NOT added here since they already exist as standalone
    # label/glabel/hier statements in the trace_json (they were kept during forward conversion)
    processed_uuids = set()
    all_labels_output = labels + net_labels
    for label in all_labels_output:
        label_uid = label.get('uid')
        if label_uid and label_uid in element_maps['label']:
            # Merge existing label
            existing_label = element_maps['label'][label_uid]
            updated_label = merge_label(existing_label, label)
            kicad_sch.append(updated_label)
            processed_uuids.add(label_uid)
        else:
            # Generate new label
            label_elem = convert_label(label)
            if label_elem:
                kicad_sch.append(label_elem)
    
    # Process no-connects (from standalone noconnect statements and from DNC pins)
    processed_uuids = set()
    all_no_connects = noconnects + no_connects_from_pins
    for noconnect in all_no_connects:
        nc_uid = noconnect.get('uid')
        if nc_uid and nc_uid in element_maps['no_connect']:
            # Merge existing noconnect
            existing_noconnect = element_maps['no_connect'][nc_uid]
            updated_noconnect = merge_noconnect(existing_noconnect, noconnect)
            kicad_sch.append(updated_noconnect)
            processed_uuids.add(nc_uid)
        else:
            # Generate new noconnect
            nc_elem = convert_noconnect(noconnect)
            if nc_elem:
                kicad_sch.append(nc_elem)
    
    # Process global labels - merge or generate
    processed_uuids = set()
    for glabel in glabels:
        glabel_uid = glabel.get('uid')
        if glabel_uid and glabel_uid in element_maps['global_label']:
            # Merge existing glabel
            existing_glabel = element_maps['global_label'][glabel_uid]
            updated_glabel = merge_glabel(existing_glabel, glabel)
            kicad_sch.append(updated_glabel)
            processed_uuids.add(glabel_uid)
        else:
            # Generate new glabel
            glabel_elem = convert_glabel(glabel)
            if glabel_elem:
                kicad_sch.append(glabel_elem)
    
    # Process hierarchical labels - merge or generate
    processed_uuids = set()
    for hier in hier_labels:
        hier_uid = hier.get('uid')
        if hier_uid and hier_uid in element_maps['hierarchical_label']:
            # Merge existing hier label
            existing_hier = element_maps['hierarchical_label'][hier_uid]
            updated_hier = merge_hier_label(existing_hier, hier)
            kicad_sch.append(updated_hier)
            processed_uuids.add(hier_uid)
        else:
            # Generate new hier label
            hier_elem = convert_hier_label(hier)
            if hier_elem:
                kicad_sch.append(hier_elem)
    
    # Process sheets - merge or generate
    # Auto-assign page numbers to new sheets (starting from 2, since main sheet is page 1)
    # For merged sheets, page numbers are preserved from the existing kicad_sch
    processed_uuids = set()
    
    # Count existing sheets to determine next page number for new sheets
    next_page_number = 2  # Start from 2 (main sheet is 1)
    for sheet_uid in element_maps['sheet']:
        # Find the page number of existing sheets to avoid conflicts
        existing_sheet = element_maps['sheet'][sheet_uid]
        instances = find_element(existing_sheet, 'instances')
        if instances:
            for item in instances:
                if isinstance(item, list) and len(item) > 0 and item[0] == 'project':
                    path_elem = find_element(item, 'path')
                    if path_elem:
                        page_elem = find_element(path_elem, 'page')
                        if page_elem and len(page_elem) >= 2:
                            try:
                                existing_page = int(page_elem[1])
                                if existing_page >= next_page_number:
                                    next_page_number = existing_page + 1
                            except (ValueError, TypeError):
                                pass
    
    for sheet in sheets:
        sheet_uid = sheet.get('uid')
        
        if sheet_uid and sheet_uid in element_maps['sheet']:
            # Merge existing sheet - preserves page number from kicad_sch
            existing_sheet = element_maps['sheet'][sheet_uid]
            updated_sheet = merge_sheet(existing_sheet, sheet)
            kicad_sch.append(updated_sheet)
            processed_uuids.add(sheet_uid)
            
            # Generate labels for sheet pins (analogous to component pin labels in merge path)
            for pin_name, pin_value in {**sheet.get('ins', {}), **sheet.get('outs', {})}.items():
                if isinstance(pin_value, dict):
                    pin_net = pin_value.get('net', pin_name)
                    pin_at = pin_value.get('at')
                else:
                    pin_net = pin_value if isinstance(pin_value, str) else pin_name
                    pin_at = None
                if pin_net and pin_net not in ("NONE", "DNC") and pin_at:
                    net_labels.append({
                        'type': 'label',
                        'name': pin_net,
                        'at': list(pin_at)
                    })
        else:
            # Generate new sheet with auto-assigned page number
            # Set the page number in the sheet dict before conversion
            if 'page' not in sheet:
                sheet['page'] = str(next_page_number)
                next_page_number += 1
            sheet_elem, sheet_pin_labels = convert_sheet(sheet, instance_path)
            if sheet_elem:
                kicad_sch.append(sheet_elem)
            net_labels.extend(sheet_pin_labels)
    
    # Process buses - generate (no merging needed as they're just graphical)
    for bus in buses:
        bus_elem = convert_bus(bus)
        if bus_elem:
            kicad_sch.append(bus_elem)
    
    # Process polylines - generate (no merging needed as they're just graphical)
    for polyline in polylines:
        polyline_elem = convert_polyline(polyline)
        if polyline_elem:
            kicad_sch.append(polyline_elem)
    
    # Process rectangles - generate (no merging needed as they're just graphical)
    for rectangle in rectangles:
        rectangle_elem = convert_rectangle(rectangle)
        if rectangle_elem:
            kicad_sch.append(rectangle_elem)
    
    # Process arcs - generate (no merging needed as they're just graphical)
    for arc in arcs:
        arc_elem = convert_arc(arc)
        if arc_elem:
            kicad_sch.append(arc_elem)
    
    # Process beziers - generate (no merging needed as they're just graphical)
    for bezier in beziers:
        bezier_elem = convert_bezier(bezier)
        if bezier_elem:
            kicad_sch.append(bezier_elem)
    
    # Process circles - generate (no merging needed as they're just graphical)
    for circle in circles:
        circle_elem = convert_circle(circle)
        if circle_elem:
            kicad_sch.append(circle_elem)
    
    # Process text_boxes - generate (no merging needed as they're just graphical)
    for text_box in text_boxes:
        text_box_elem = convert_text_box(text_box)
        if text_box_elem:
            kicad_sch.append(text_box_elem)
    
    # Process bus_entries - generate (no merging needed as they're just graphical)
    for bus_entry in bus_entries:
        bus_entry_elem = convert_bus_entry(bus_entry)
        if bus_entry_elem:
            kicad_sch.append(bus_entry_elem)
    
    # Add preserved elements (not in trace_sch.ebnf)
    kicad_sch.extend(preserved_elements)
    
    # Handle sheet_instances - preserve from existing or generate new
    # Note: sheet_instances only contains the root path "/" with page "1"
    # Sub-sheet page numbers are stored in the sheet element's instances section
    if existing_sch_data:
        existing_sheet_instances = find_element(existing_sch_data, 'sheet_instances')
        if existing_sheet_instances:
            kicad_sch.append(existing_sheet_instances)
        else:
            # Generate new sheet_instances - only root path needed
            sheet_instances = ['sheet_instances']
            sheet_instances.append(['path', '/', ['page', "1"]])
            kicad_sch.append(sheet_instances)
    else:
        # Generate new sheet_instances - only root path needed
        sheet_instances = ['sheet_instances']
        sheet_instances.append(['path', '/', ['page', "1"]])
        kicad_sch.append(sheet_instances)
    
    # Handle embedded_fonts - preserve from existing or use default
    if existing_sch_data:
        existing_embedded_fonts = find_element(existing_sch_data, 'embedded_fonts')
        if existing_embedded_fonts:
            kicad_sch.append(existing_embedded_fonts)
        else:
            kicad_sch.append(['embedded_fonts', 'no'])
    else:
        kicad_sch.append(['embedded_fonts', 'no'])
    
    # Format as S-expression string
    result = format_sexp(kicad_sch)
    
    end_time = time.time()
    print(f"Conversion complete in {end_time - start_time} seconds")
    return result


# =============================================================================
# Command-Line Interface
# =============================================================================

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print("Usage: python trace_json_to_sexp.py <trace_json_file> [existing_kicad_sch]")
        sys.exit(1)
    
    filename = sys.argv[1]
    existing_sch_file = sys.argv[2] if len(sys.argv) == 3 else None
    
    try:
        with open(filename, "r") as file:
            trace_json = json.load(file)
        
        existing_sch_content = None
        if existing_sch_file:
            with open(existing_sch_file, "r") as file:
                existing_sch_content = file.read()
        
        sexp_output = trace_json_to_sexp(trace_json, existing_sch_content=existing_sch_content)
        
        # Write to output file
        output_filename = filename.replace('.json', '.kicad_sch')
        if output_filename == filename:
            output_filename = 'output.kicad_sch'
        
        with open(output_filename, "w") as file:
            file.write(sexp_output)
        
        print(f"Conversion complete. Output written to {output_filename}")
    
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
