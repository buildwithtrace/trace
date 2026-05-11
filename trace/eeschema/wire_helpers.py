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
Wire connectivity helper functions.

Provides functions for determining wire connectivity and pin-wire relationships.
These are separated from net_router.py to avoid circular import issues.
"""

import math
from typing import List, Dict, Any, Optional, Tuple
from collections import defaultdict


def euclidean_distance(p1: Tuple[float, float], p2: Tuple[float, float]) -> float:
    """Calculate Euclidean distance between two points."""
    dx = p1[0] - p2[0]
    dy = p1[1] - p2[1]
    return math.sqrt(dx * dx + dy * dy)


def point_on_segment(point: Tuple[float, float],
                    seg_start: Tuple[float, float],
                    seg_end: Tuple[float, float],
                    tolerance: float = 0.01) -> bool:
    """
    Check if a point lies on a line segment (within tolerance).
    
    Args:
        point: (x, y) point to check
        seg_start, seg_end: Segment endpoints
        tolerance: Distance tolerance in mm
    
    Returns:
        True if point is on the segment within tolerance
    """
    # Project point onto segment line
    seg_vec = (seg_end[0] - seg_start[0], seg_end[1] - seg_start[1])
    point_vec = (point[0] - seg_start[0], point[1] - seg_start[1])
    
    seg_len_sq = seg_vec[0] * seg_vec[0] + seg_vec[1] * seg_vec[1]
    
    if seg_len_sq < 1e-10:
        # Degenerate segment (zero length) - check if point matches endpoint
        return euclidean_distance(point, seg_start) < tolerance
    
    # Project point onto segment
    t = (point_vec[0] * seg_vec[0] + point_vec[1] * seg_vec[1]) / seg_len_sq
    
    # Check if projection is within segment bounds [0, 1]
    if t < 0 or t > 1:
        return False
    
    # Calculate projected point
    proj_point = (seg_start[0] + t * seg_vec[0], seg_start[1] + t * seg_vec[1])
    
    # Check distance from point to projected point
    return euclidean_distance(point, proj_point) < tolerance


def segment_intersection(seg1_start: Tuple[float, float],
                        seg1_end: Tuple[float, float],
                        seg2_start: Tuple[float, float],
                        seg2_end: Tuple[float, float],
                        tolerance: float = 0.01) -> Optional[Tuple[float, float]]:
    """
    Find the intersection point of two line segments.
    
    Args:
        seg1_start, seg1_end: First segment endpoints
        seg2_start, seg2_end: Second segment endpoints
        tolerance: Distance tolerance in mm
    
    Returns:
        Intersection point (x, y) if segments intersect, None otherwise
    """
    x1, y1 = seg1_start
    x2, y2 = seg1_end
    x3, y3 = seg2_start
    x4, y4 = seg2_end
    
    # Calculate denominators
    denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    
    if abs(denom) < 1e-10:
        # Segments are parallel
        return None
    
    # Calculate intersection point
    t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / denom
    u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / denom
    
    # Check if intersection is within both segments
    if 0 <= t <= 1 and 0 <= u <= 1:
        # Calculate intersection point
        x = x1 + t * (x2 - x1)
        y = y1 + t * (y2 - y1)
        return (x, y)
    
    return None


def junction_at_position(pos: Tuple[float, float],
                         junctions: List[Dict[str, Any]],
                         tolerance: float = 0.1) -> bool:
    """
    Check if there's a junction at a given position.
    
    Args:
        pos: (x, y) position to check
        junctions: List of junction dictionaries with 'at': [x, y]
        tolerance: Distance tolerance in mm
    
    Returns:
        True if there's a junction at the position within tolerance
    """
    for junction in junctions:
        junction_pos = tuple(junction.get('at', []))
        if len(junction_pos) == 2:
            if euclidean_distance(pos, junction_pos) < tolerance:
                return True
    return False


def wires_are_connected(wire1: Dict[str, Any],
                        wire2: Dict[str, Any],
                        junctions: List[Dict[str, Any]],
                        tolerance: float = 0.01) -> bool:
    """
    Check if two wires are connected (endpoints touch OR they intersect with a junction).
    
    Args:
        wire1, wire2: Wire dictionaries with 'points' list
        junctions: List of junction dictionaries
        tolerance: Distance tolerance in mm
    
    Returns:
        True if wires are connected
    """
    points1 = wire1.get('points', [])
    points2 = wire2.get('points', [])
    
    if len(points1) < 2 or len(points2) < 2:
        return False
    
    seg1_start = tuple(points1[0])
    seg1_end = tuple(points1[-1])
    seg2_start = tuple(points2[0])
    seg2_end = tuple(points2[-1])
    
    # Check if endpoints touch
    if (euclidean_distance(seg1_start, seg2_start) < tolerance or
        euclidean_distance(seg1_start, seg2_end) < tolerance or
        euclidean_distance(seg1_end, seg2_start) < tolerance or
        euclidean_distance(seg1_end, seg2_end) < tolerance):
        return True
    
    # Check if wires intersect AND there's a junction at the intersection
    intersection = segment_intersection(seg1_start, seg1_end, seg2_start, seg2_end, tolerance)
    if intersection and junction_at_position(intersection, junctions, tolerance=0.1):
        return True
    
    return False


def wire_touches_pin(wire: Dict[str, Any],
                     pin_pos: Tuple[float, float],
                     junctions: List[Dict[str, Any]],
                     tolerance: float = 0.01) -> bool:
    """
    Check if wire touches a pin (endpoint touches pin OR wire crosses pin with junction).
    
    Args:
        wire: Wire dictionary with 'points' list
        pin_pos: (x, y) pin position
        junctions: List of junction dictionaries
        tolerance: Distance tolerance in mm
    
    Returns:
        True if wire touches the pin
    """
    wire_points = wire.get('points', [])
    if len(wire_points) < 2:
        return False
    
    start_point = tuple(wire_points[0])
    end_point = tuple(wire_points[-1])
    
    # Case 1: Wire endpoint touches pin
    if (euclidean_distance(pin_pos, start_point) < tolerance or
        euclidean_distance(pin_pos, end_point) < tolerance):
        return True
    
    # Case 3: Wire crosses over pin position AND there's a junction at that position
    if point_on_segment(pin_pos, start_point, end_point, tolerance):
        if junction_at_position(pin_pos, junctions, tolerance=0.1):
            return True
    
    return False


def find_wire_islands(wires: List[Dict[str, Any]],
                      junctions: List[Dict[str, Any]],
                      tolerance: float = 0.01) -> List[List[Dict[str, Any]]]:
    """
    Group wires into connected islands using union-find.
    
    Wires are connected if:
    - Their endpoints touch (within tolerance), OR
    - They intersect AND there's a junction at the intersection point
    
    Args:
        wires: List of wire dictionaries
        junctions: List of junction dictionaries
        tolerance: Distance tolerance in mm
    
    Returns:
        List of wire islands, where each island is a list of connected wires
    """
    if not wires:
        return []
    
    # Union-find data structure
    parent = list(range(len(wires)))
    
    def find(x: int) -> int:
        if parent[x] != x:
            parent[x] = find(parent[x])
        return parent[x]
    
    def union(x: int, y: int):
        root_x = find(x)
        root_y = find(y)
        if root_x != root_y:
            parent[root_x] = root_y
    
    # Check all pairs of wires for connections
    for i in range(len(wires)):
        for j in range(i + 1, len(wires)):
            if wires_are_connected(wires[i], wires[j], junctions, tolerance):
                union(i, j)
    
    # Group wires by their root
    islands_dict: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    for i, wire in enumerate(wires):
        root = find(i)
        islands_dict[root].append(wire)
    
    # Return as list of lists
    return list(islands_dict.values())
