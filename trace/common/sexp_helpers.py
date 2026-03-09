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
S-Expression Helper Functions

Common helper functions for extracting data from parsed S-expressions.
Used by both eeschema and pcbnew converters.
"""

from typing import List, Optional, Tuple, Any


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
        # Remove quotes if present
        if value.startswith('"') and value.endswith('"'):
            return value[1:-1]
        if value.startswith("'") and value.endswith("'"):
            return value[1:-1]
    return value


def extract_coord(at: List) -> Optional[Tuple[float, float]]:
    """Extract coordinate from (at x y [rot]) format."""
    if not isinstance(at, list) or len(at) < 3:
        return None
    try:
        x = float(at[1])
        y = float(at[2])
        return (x, y)
    except (ValueError, TypeError, IndexError):
        return None


def extract_rotation(at: List) -> Optional[int]:
    """Extract rotation from (at x y rot) format."""
    if not isinstance(at, list) or len(at) < 4:
        return None
    try:
        rot = int(float(at[3]))
        return rot
    except (ValueError, TypeError, IndexError):
        return None
