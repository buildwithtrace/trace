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
A* Net Router for trace_sch

Implements intelligent routing for cwires using A* pathfinding algorithm.
Routes connections between pins while avoiding component collisions.
"""

import math
import uuid
from typing import List, Dict, Any, Optional, Tuple, Set
import heapq
from wire_helpers import euclidean_distance, point_on_segment


# Grid size in mm (1.27mm = 50 mils)
GRID_SIZE = 1.27


def manhattan_distance(p1: Tuple[float, float], p2: Tuple[float, float]) -> float:
    """Calculate Manhattan distance between two points."""
    return abs(p1[0] - p2[0]) + abs(p1[1] - p2[1])


def point_in_rect(point: Tuple[float, float], 
                  min_x: float, min_y: float, 
                  max_x: float, max_y: float) -> bool:
    """Check if point is inside rectangle."""
    x, y = point
    return min_x <= x <= max_x and min_y <= y <= max_y


def segment_intersects_rect(seg_start: Tuple[float, float],
                            seg_end: Tuple[float, float],
                            min_x: float, min_y: float,
                            max_x: float, max_y: float) -> bool:
    """
    Check if line segment intersects rectangle.
    Uses Liang-Barsky algorithm for efficiency.
    """
    x0, y0 = seg_start
    x1, y1 = seg_end
    
    dx = x1 - x0
    dy = y1 - y0
    
    # Check if segment is completely outside bounding box
    if dx == 0 and dy == 0:
        # Point segment - check if point is in rect
        return point_in_rect(seg_start, min_x, min_y, max_x, max_y)
    
    # Parameter ranges
    t0 = 0.0
    t1 = 1.0
    
    # Check each edge
    for edge in range(4):
        if edge == 0:   # Left edge
            p = -dx
            q = x0 - min_x
        elif edge == 1:  # Right edge
            p = dx
            q = max_x - x0
        elif edge == 2:  # Bottom edge
            p = -dy
            q = y0 - min_y
        else:  # Top edge
            p = dy
            q = max_y - y0
        
        if abs(p) < 1e-10:
            # Parallel to edge
            if q < 0:
                return False
        else:
            r = q / p
            if p < 0:
                if r > t1:
                    return False
                elif r > t0:
                    t0 = r
            else:
                if r < t0:
                    return False
                elif r < t1:
                    t1 = r
    
    return t0 < t1


def calculate_component_bbox(symbol_def, pins, unit, body_style, symbol_pos, symbol_rot, transform_pin_coordinate_func, extract_pin_info_from_symbol_func) -> Optional[Tuple[float, float, float, float]]:
    """
    Calculate component bounding box from pin positions and component position.
    
    Args:
        symbol_def: Symbol definition from library
        pins: Dictionary of pin numbers to net names
        unit: Unit of the component
        body_style: Body style of the component
        symbol_pos: Position of the component
        symbol_rot: Rotation of the component
        transform_pin_coordinate_func: Function to transform pin coordinates
    
    Returns:
        (min_x, min_y, max_x, max_y) or None if no pins
    """
    # Collect all positions: component position and pin positions
    all_positions = []
    
    # Add component position (symbol position)
    symbol_pos_tuple = tuple(symbol_pos)
    all_positions.append(symbol_pos_tuple)
    
    # Add pin positions
    for pin_num in pins.keys():
        pin_info = extract_pin_info_from_symbol_func(symbol_def, pin_num, unit=unit, body_style=body_style)
        if pin_info:
            x_off, y_off, pin_rot = pin_info
            pin_pos = transform_pin_coordinate_func((x_off, y_off, pin_rot), 
                                                    symbol_pos_tuple, 
                                                    symbol_rot)
            all_positions.append(pin_pos)
    
    if not all_positions:
        return None
    
    # Find bounding box of component position and all pins
    min_x = min(p[0] for p in all_positions)
    min_y = min(p[1] for p in all_positions)
    max_x = max(p[0] for p in all_positions)
    max_y = max(p[1] for p in all_positions)
    
    return (min_x, min_y, max_x, max_y)


def calculate_sheet_bbox(sheet_pos: Tuple[float, float], sheet_size: Tuple[float, float]) -> Tuple[float, float, float, float]:
    """
    Calculate sheet bounding box from position and size.
    
    Args:
        sheet_pos: Sheet position (x, y)
        sheet_size: Sheet size (width, height)
    
    Returns:
        (min_x, min_y, max_x, max_y) bounding box
    """
    x, y = sheet_pos
    width, height = sheet_size
    
    # Sheet position is typically the top-left corner
    min_x = x
    min_y = y
    max_x = x + width
    max_y = y + height
    
    return (min_x, min_y, max_x, max_y)


def point_in_component(point: Tuple[float, float], 
                      component_bboxes: List[Tuple[float, float, float, float]]) -> bool:
    """Check if point is inside any component bounding box."""
    x, y = point
    for min_x, min_y, max_x, max_y in component_bboxes:
        if point_in_rect(point, min_x, min_y, max_x, max_y):
            return True
    return False


def segment_intersects_component(seg_start: Tuple[float, float],
                                 seg_end: Tuple[float, float],
                                 component_bboxes: List[Tuple[float, float, float, float]]) -> bool:
    """Check if line segment intersects any component bounding box."""
    for min_x, min_y, max_x, max_y in component_bboxes:
        if segment_intersects_rect(seg_start, seg_end, min_x, min_y, max_x, max_y):
            return True
    return False


def is_horizontal_segment(seg_start: Tuple[float, float], seg_end: Tuple[float, float], 
                          tolerance: float = 0.01) -> bool:
    """Check if a segment is horizontal (same Y coordinate)."""
    return abs(seg_start[1] - seg_end[1]) < tolerance


def is_vertical_segment(seg_start: Tuple[float, float], seg_end: Tuple[float, float],
                        tolerance: float = 0.01) -> bool:
    """Check if a segment is vertical (same X coordinate)."""
    return abs(seg_start[0] - seg_end[0]) < tolerance


def segments_are_parallel(seg1_start: Tuple[float, float], seg1_end: Tuple[float, float],
                          seg2_start: Tuple[float, float], seg2_end: Tuple[float, float],
                          tolerance: float = 0.01) -> bool:
    """
    Check if two segments are parallel (both horizontal or both vertical).
    
    Args:
        seg1_start, seg1_end: First segment endpoints
        seg2_start, seg2_end: Second segment endpoints
        tolerance: Coordinate tolerance in mm
    
    Returns:
        True if both segments are parallel (both horizontal or both vertical)
    """
    seg1_horiz = is_horizontal_segment(seg1_start, seg1_end, tolerance)
    seg1_vert = is_vertical_segment(seg1_start, seg1_end, tolerance)
    seg2_horiz = is_horizontal_segment(seg2_start, seg2_end, tolerance)
    seg2_vert = is_vertical_segment(seg2_start, seg2_end, tolerance)
    
    return (seg1_horiz and seg2_horiz) or (seg1_vert and seg2_vert)


def ranges_overlap(a_min: float, a_max: float, b_min: float, b_max: float, 
                   tolerance: float = 0.01) -> bool:
    """
    Check if two 1D ranges overlap.
    
    Args:
        a_min, a_max: First range (will be sorted internally)
        b_min, b_max: Second range (will be sorted internally)
        tolerance: Overlap tolerance
    
    Returns:
        True if ranges overlap by more than tolerance
    """
    # Ensure min <= max
    if a_min > a_max:
        a_min, a_max = a_max, a_min
    if b_min > b_max:
        b_min, b_max = b_max, b_min
    
    # Check for overlap - ranges overlap if one doesn't end before the other starts
    # We use tolerance to avoid false positives from touching endpoints
    return a_max > b_min + tolerance and b_max > a_min + tolerance


def segments_overlap_parallel(seg1_start: Tuple[float, float], seg1_end: Tuple[float, float],
                              seg2_start: Tuple[float, float], seg2_end: Tuple[float, float],
                              tolerance: float = 0.1) -> bool:
    """
    Check if two parallel segments overlap (share common length on the same line).
    
    This detects the forbidden case where two wires run parallel on top of each other.
    Segments must be on the same line (not just parallel) and share overlapping length.
    
    Args:
        seg1_start, seg1_end: First segment endpoints
        seg2_start, seg2_end: Second segment endpoints
        tolerance: Distance tolerance for considering segments on same line
    
    Returns:
        True if segments overlap parallel (forbidden in routing)
    """
    seg1_horiz = is_horizontal_segment(seg1_start, seg1_end, tolerance)
    seg1_vert = is_vertical_segment(seg1_start, seg1_end, tolerance)
    seg2_horiz = is_horizontal_segment(seg2_start, seg2_end, tolerance)
    seg2_vert = is_vertical_segment(seg2_start, seg2_end, tolerance)
    
    # Both horizontal - check if on same Y and X ranges overlap
    if seg1_horiz and seg2_horiz:
        # Check if on same horizontal line (same Y coordinate)
        if abs(seg1_start[1] - seg2_start[1]) < tolerance:
            # Check if X ranges overlap
            return ranges_overlap(seg1_start[0], seg1_end[0], 
                                  seg2_start[0], seg2_end[0], tolerance)
    
    # Both vertical - check if on same X and Y ranges overlap
    if seg1_vert and seg2_vert:
        # Check if on same vertical line (same X coordinate)
        if abs(seg1_start[0] - seg2_start[0]) < tolerance:
            # Check if Y ranges overlap
            return ranges_overlap(seg1_start[1], seg1_end[1],
                                  seg2_start[1], seg2_end[1], tolerance)
    
    return False


def segment_overlaps_any_wire(seg_start: Tuple[float, float], seg_end: Tuple[float, float],
                              existing_wires: List[Tuple[Tuple[float, float], Tuple[float, float]]],
                              tolerance: float = 0.1) -> bool:
    """
    Check if a segment would overlap parallel with any existing wire.
    
    Args:
        seg_start, seg_end: The segment to check
        existing_wires: List of (start, end) tuples representing existing wire segments
        tolerance: Distance tolerance
    
    Returns:
        True if the segment overlaps parallel with any existing wire (forbidden)
    """
    for wire_start, wire_end in existing_wires:
        if segments_overlap_parallel(seg_start, seg_end, wire_start, wire_end, tolerance):
            return True
    return False


def is_orthogonal_crossing(seg1_start: Tuple[float, float], seg1_end: Tuple[float, float],
                           seg2_start: Tuple[float, float], seg2_end: Tuple[float, float],
                           tolerance: float = 0.01) -> bool:
    """
    Check if two segments cross orthogonally (one horizontal, one vertical).
    
    This is the allowed case where wires can cross over each other.
    
    Args:
        seg1_start, seg1_end: First segment endpoints
        seg2_start, seg2_end: Second segment endpoints
        tolerance: Coordinate tolerance
    
    Returns:
        True if segments cross at 90 degrees (allowed in routing)
    """
    seg1_horiz = is_horizontal_segment(seg1_start, seg1_end, tolerance)
    seg1_vert = is_vertical_segment(seg1_start, seg1_end, tolerance)
    seg2_horiz = is_horizontal_segment(seg2_start, seg2_end, tolerance)
    seg2_vert = is_vertical_segment(seg2_start, seg2_end, tolerance)
    
    # One horizontal and one vertical = orthogonal
    return (seg1_horiz and seg2_vert) or (seg1_vert and seg2_horiz)


def find_point_on_same_net_wire(
    point: Tuple[float, float],
    same_net_wires: List[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any]]],
    tolerance: float = 0.1
) -> Optional[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any]]]:
    """
    Check if a point lies on any wire from the same net.
    
    Args:
        point: (x, y) point to check
        same_net_wires: List of (start, end, wire_dict) tuples for wires on the same net
        tolerance: Distance tolerance for point-on-segment check
    
    Returns:
        The (start, end, wire_dict) tuple of the wire the point lies on, or None
    """
    for wire_start, wire_end, wire_dict in same_net_wires:
        if point_on_segment(point, wire_start, wire_end, tolerance):
            return (wire_start, wire_end, wire_dict)
    return None


def split_wire_at_point(
    wire_dict: Dict[str, Any],
    split_point: Tuple[float, float]
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """
    Split a wire into two segments at a given point.
    
    Args:
        wire_dict: The original wire dictionary with 'points' and 'uid'
        split_point: The point at which to split the wire
    
    Returns:
        Tuple of (wire1, wire2) - two new wire dictionaries
    """
    points = wire_dict.get('points', [])
    if len(points) < 2:
        # Can't split a degenerate wire
        return wire_dict, None
    
    wire_start = tuple(points[0]) if isinstance(points[0], list) else points[0]
    wire_end = tuple(points[-1]) if isinstance(points[-1], list) else points[-1]
    
    # Round split point for consistency
    split_point_rounded = (round(split_point[0], 3), round(split_point[1], 3))
    
    # Create two new wires
    wire1 = {
        'type': 'wire',
        'points': [list(wire_start), list(split_point_rounded)],
        'uid': str(uuid.uuid4())
    }
    
    wire2 = {
        'type': 'wire',
        'points': [list(split_point_rounded), list(wire_end)],
        'uid': str(uuid.uuid4())
    }
    
    return wire1, wire2


def snap_to_grid(point: Tuple[float, float], grid_size: float = GRID_SIZE) -> Tuple[float, float]:
    """Snap a point to the nearest grid position."""
    return (
        round(point[0] / grid_size) * grid_size,
        round(point[1] / grid_size) * grid_size
    )


def get_neighbors(grid_pos: Tuple[int, int], goal_grid: Optional[Tuple[int, int]] = None) -> List[Tuple[int, int]]:
    """
    Get 4-directional neighbors of a grid position.
    If goal is provided, prioritize moves toward the goal.
    """
    x, y = grid_pos
    neighbors = [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    
    # If goal is provided, sort neighbors to prioritize moves toward goal
    if goal_grid:
        gx, gy = goal_grid
        def dist_to_goal(pos):
            return abs(pos[0] - gx) + abs(pos[1] - gy)
        neighbors.sort(key=dist_to_goal)
    
    return neighbors


def grid_to_world(grid_pos: Tuple[int, int], grid_size: float = GRID_SIZE) -> Tuple[float, float]:
    """Convert grid position to world coordinates."""
    return (grid_pos[0] * grid_size, grid_pos[1] * grid_size)


def world_to_grid(world_pos: Tuple[float, float], grid_size: float = GRID_SIZE) -> Tuple[int, int]:
    """Convert world coordinates to grid position."""
    return (int(round(world_pos[0] / grid_size)), int(round(world_pos[1] / grid_size)))


def astar_pathfind(start: Tuple[float, float],
                  goal: Tuple[float, float],
                  component_bboxes: List[Tuple[float, float, float, float]],
                  existing_wires: List[Tuple[Tuple[float, float], Tuple[float, float]]] = None,
                  same_net_wires: List[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any]]] = None,
                  pin_positions: Dict[Tuple[float, float], Optional[str]] = None,
                  current_net: Optional[str] = None,
                  grid_size: float = GRID_SIZE,
                  max_iterations: int = 50000) -> Tuple[Optional[List[Tuple[float, float]]], Optional[Dict[str, Any]]]:
    """
    A* pathfinding algorithm for routing cwires with net-aware early termination.
    
    Routes from start to goal while avoiding component bounding boxes and
    avoiding parallel overlap with existing wires. If the route reaches a point
    on a wire belonging to the same net, it terminates early at that point.
    Also avoids pins that are not the start/end pins, unless the pin is on the
    same net (in which case it terminates early at that pin).
    
    Args:
        start: Start point (x, y) in world coordinates
        goal: Goal point (x, y) in world coordinates
        component_bboxes: List of component bounding boxes (min_x, min_y, max_x, max_y)
        existing_wires: List of (start, end) tuples representing existing wire segments
                       to avoid parallel overlap with
        same_net_wires: List of (start, end, wire_dict) tuples for wires on the same net
                       that can be used for early termination
        pin_positions: Dict mapping pin coordinates to their net names (None if no net assigned)
                      Used to avoid routing over other pins
        current_net: The net name of the current route (for same-net pin detection)
        grid_size: Grid size in mm
        max_iterations: Maximum iterations to prevent infinite loops
    
    Returns:
        Tuple of (path, early_termination_info):
        - path: List of points forming the path, or None if no path found
        - early_termination_info: Dict with 'wire' and 'point' if terminated early on same-net wire,
                                  or Dict with 'pin' and 'point' if terminated early on same-net pin,
                                  or None if reached the goal normally
    """
    if existing_wires is None:
        existing_wires = []
    if same_net_wires is None:
        same_net_wires = []
    if pin_positions is None:
        pin_positions = {}
        
    start_grid = world_to_grid(start, grid_size)
    goal_grid = world_to_grid(goal, grid_size)
    
    # If start and goal are the same, return trivial path
    if start_grid == goal_grid:
        return [start, goal], None
    
    # Store start and goal positions
    start_world = start
    goal_world = goal
    
    # Priority queue: (f_score, g_score, tie_breaker, position, path)
    tie_breaker = 0
    open_set = [(0, 0, tie_breaker, start_grid, [start])]
    closed_set: Set[Tuple[int, int]] = set()
    
    iterations = 0
    
    def is_pin_position(grid_pos: Tuple[int, int]) -> bool:
        """Check if grid position is start or goal (pin positions)."""
        return grid_pos == start_grid or grid_pos == goal_grid
    
    while open_set and iterations < max_iterations:
        iterations += 1
        
        current_f, current_g, current_tie, current_pos, current_path = heapq.heappop(open_set)
        
        if current_pos in closed_set:
            continue
        
        closed_set.add(current_pos)
        
        current_world = grid_to_world(current_pos, grid_size)
        
        # Check if we reached the goal
        if current_pos == goal_grid:
            path = current_path + [goal]
            return path, None
        
        # Check if current position is on a same-net wire (early termination)
        # Skip this check for the start position to avoid immediately terminating
        if current_pos != start_grid and same_net_wires:
            hit_wire = find_point_on_same_net_wire(current_world, same_net_wires, tolerance=grid_size * 0.6)
            if hit_wire:
                # Found a same-net wire! Terminate early at this point
                wire_start, wire_end, wire_dict = hit_wire
                # Snap the termination point to the grid for clean routing
                termination_point = snap_to_grid(current_world, grid_size)
                path = current_path  # Path already includes current_world from previous iteration
                return path, {
                    'wire': wire_dict,
                    'wire_start': wire_start,
                    'wire_end': wire_end,
                    'point': termination_point
                }
        
        # Check neighbors (prioritize moves toward goal)
        for neighbor in get_neighbors(current_pos, goal_grid):
            if neighbor in closed_set:
                continue
            
            neighbor_world = grid_to_world(neighbor, grid_size)
            
            # Allow start and goal positions even if in components (they're pin positions)
            neighbor_is_pin = is_pin_position(neighbor)
            current_is_pin = is_pin_position(current_pos)
            
            # Check if neighbor is in a component (but allow pin positions and nearby positions)
            dist_to_start = abs(neighbor_world[0] - start_world[0]) + abs(neighbor_world[1] - start_world[1])
            dist_to_goal = abs(neighbor_world[0] - goal_world[0]) + abs(neighbor_world[1] - goal_world[1])
            near_pin = dist_to_start < 2.0 or dist_to_goal < 2.0
            
            if not (neighbor_is_pin or current_is_pin or near_pin):
                if point_in_component(neighbor_world, component_bboxes):
                    continue
            
            # Check if path to neighbor crosses a component
            if current_is_pin:
                # First step from pin - allow even if segment intersects component
                pass
            elif neighbor_is_pin:
                # Moving to pin - allow
                pass
            else:
                # Normal case - check if segment intersects component
                if segment_intersects_component(current_world, neighbor_world, component_bboxes):
                    continue
            
            # Check if segment would overlap parallel with any existing wire
            # Orthogonal crossings are allowed, but parallel overlaps are forbidden
            if existing_wires and segment_overlaps_any_wire(current_world, neighbor_world, existing_wires):
                continue
            
            # Check if neighbor is at a pin position (not start/end of current route)
            # If so, either block it (different net) or terminate early (same net)
            if pin_positions and not neighbor_is_pin:
                pin_collision = False
                for pin_pos, pin_net in pin_positions.items():
                    dist_to_pin = euclidean_distance(neighbor_world, pin_pos)
                    if dist_to_pin < grid_size * 0.6:  # Within tolerance of a pin
                        if current_net and pin_net == current_net:
                            # Same net - terminate early at this pin
                            termination_point = pin_pos
                            path = current_path + [termination_point]
                            return path, {
                                'pin': pin_pos,
                                'pin_net': pin_net,
                                'point': termination_point
                            }
                        else:
                            # Different net or no net - block this position
                            pin_collision = True
                            break
                if pin_collision:
                    # Skip this neighbor - can't route over a pin on a different net
                    continue
            
            # Calculate g_score (cost from start)
            # Simple cost: just the grid size, with slight preference for straight paths
            move_cost = grid_size
            
            # Check if this is a direction change (bend) - add small penalty
            if len(current_path) > 1:
                last_dir = (current_world[0] - current_path[-2][0], 
                           current_world[1] - current_path[-2][1])
                current_dir = (neighbor_world[0] - current_world[0],
                               neighbor_world[1] - current_world[1])
                
                # If direction changed, add small penalty to prefer straight paths
                if abs(last_dir[0] - current_dir[0]) > 0.01 or abs(last_dir[1] - current_dir[1]) > 0.01:
                    move_cost += grid_size * 0.1  # 10% penalty for bends
            
            # Add bonus for moving toward same-net wires (makes early termination more likely)
            if same_net_wires:
                for wire_start, wire_end, _ in same_net_wires:
                    # Check distance to wire segment
                    # Simple heuristic: distance to midpoint of wire
                    wire_mid = ((wire_start[0] + wire_end[0]) / 2, (wire_start[1] + wire_end[1]) / 2)
                    dist_to_wire = manhattan_distance(neighbor_world, wire_mid)
                    if dist_to_wire < manhattan_distance(current_world, wire_mid):
                        # Moving closer to a same-net wire - small bonus
                        move_cost -= grid_size * 0.05
                        break
            
            g_score = current_g + move_cost
            
            # Calculate h_score (heuristic to goal) - Manhattan distance
            h_score = manhattan_distance(neighbor_world, goal)
            
            # Calculate f_score
            f_score = g_score + h_score
            
            # Use tie_breaker for deterministic ordering
            tie_breaker += 1
            
            # Add to open set
            heapq.heappush(open_set, (f_score, g_score, tie_breaker, neighbor, current_path + [neighbor_world]))
    
    # No path found
    return None, None


def simplify_path(path: List[Tuple[float, float]], tolerance: float = 0.01) -> List[Tuple[float, float]]:
    """
    Simplify path by merging consecutive segments that are on the same orthogonal line.
    
    IMPORTANT: This function enforces strict orthogonality - only merges points that
    are on the same horizontal line (same Y) or same vertical line (same X).
    Diagonal segments are NOT allowed in KiCad schematic wires.
    
    Args:
        path: List of (x, y) points from A* pathfinding
        tolerance: Coordinate tolerance for considering points on same line
    
    Returns:
        Simplified path with only orthogonal segments (horizontal or vertical)
    """
    if len(path) <= 2:
        return path
    
    simplified = [path[0]]
    i = 1
    
    while i < len(path):
        p_start = simplified[-1]
        p_current = path[i]
        
        # Determine if we're on a horizontal or vertical line from p_start
        is_horiz = abs(p_start[1] - p_current[1]) < tolerance  # Same Y = horizontal
        is_vert = abs(p_start[0] - p_current[0]) < tolerance   # Same X = vertical
        
        if not is_horiz and not is_vert:
            # This shouldn't happen with proper A* (4-directional), but handle it
            # by adding the point and continuing
            simplified.append(p_current)
            i += 1
            continue
        
        # Look ahead to find the furthest point on the same orthogonal line
        best_idx = i
        
        for j in range(i + 1, len(path)):
            p_check = path[j]
            
            if is_horiz:
                # Check if p_check is still on the same horizontal line
                if abs(p_start[1] - p_check[1]) < tolerance:
                    best_idx = j
                else:
                    break
            elif is_vert:
                # Check if p_check is still on the same vertical line
                if abs(p_start[0] - p_check[0]) < tolerance:
                    best_idx = j
                else:
                    break
        
        # Add the furthest point on this orthogonal line
        simplified.append(path[best_idx])
        i = best_idx + 1
    
    # Ensure the last point is included
    if simplified[-1] != path[-1]:
        simplified.append(path[-1])
    
    return simplified


def split_path_into_wires(path: List[Tuple[float, float]], 
                         pin_positions: Optional[Set[Tuple[float, float]]] = None) -> List[Dict[str, Any]]:
    """
    Split a path into multiple 2-point wire segments.
    The path is already simplified, so we just need to split it.
    Note: We don't merge here because the path is already simplified.
    
    Args:
        path: List of (x, y) points (should already be simplified)
        pin_positions: Optional set of exact pin positions to preserve without rounding
    
    Returns:
        List of wire dictionaries, each with exactly 2 points
    """
    if len(path) < 2:
        return []
    
    if pin_positions is None:
        pin_positions = set()
    
    # Create 2-point wires from the simplified path
    # Ensure all consecutive wires connect at their endpoints with exact coordinates
    wires = []
    for i in range(len(path) - 1):
        start_point = path[i]
        end_point = path[i + 1]
        
        # Ensure points are properly connected (snap to same position if very close)
        if i > 0:
            # This wire's start should match previous wire's end exactly
            prev_wire_end = tuple(wires[-1]['points'][1])
            dist = euclidean_distance(start_point, prev_wire_end)
            if dist < 0.1:  # If very close, use exact same coordinates
                # Use exact previous wire's end point for perfect connection
                start_point = prev_wire_end
        
        # For pin positions, use exact coordinates to avoid rounding errors
        # that could cause wires from different nets to appear connected
        # For intermediate points, round to avoid floating point precision issues
        # Use a tighter tolerance (0.005mm) to ensure we only match exact pin positions
        start_is_pin = False
        start_pin_pos = None
        for pin_pos in pin_positions:
            if euclidean_distance(start_point, pin_pos) < 0.005:
                start_is_pin = True
                start_pin_pos = pin_pos
                break
        
        end_is_pin = False
        end_pin_pos = None
        for pin_pos in pin_positions:
            if euclidean_distance(end_point, pin_pos) < 0.005:
                end_is_pin = True
                end_pin_pos = pin_pos
                break
        
        if start_is_pin and start_pin_pos:
            # Use exact pin position to prevent wires from different nets connecting
            start_final = start_pin_pos
        else:
            # Round intermediate points to avoid floating point precision issues
            # Use higher precision (3 decimals = 0.001mm) for better accuracy
            start_final = (round(start_point[0], 3), round(start_point[1], 3))
        
        if end_is_pin and end_pin_pos:
            # Use exact pin position to prevent wires from different nets connecting
            end_final = end_pin_pos
        else:
            # Round intermediate points to avoid floating point precision issues
            # Use higher precision (3 decimals = 0.001mm) for better accuracy
            end_final = (round(end_point[0], 3), round(end_point[1], 3))
        
        wire = {
            'type': 'wire',
            'points': [list(start_final), list(end_final)],
            'uid': str(uuid.uuid4())
        }
        wires.append(wire)
    
    return wires


def route_cwires(
    cwire_routing_pairs: List[Tuple[Tuple[float, float], Tuple[float, float], Optional[str]]],
    trace_json: List[Dict[str, Any]],
    lib_symbols_cache: Dict[str, List],
    transform_pin_coordinate_func,
    extract_pin_info_from_symbol_func
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]], List[str]]:
    """
    Route cwires that don't have valid cwiredefs using A* pathfinding.
    
    This function performs net-aware routing. If a route reaches a point on an
    existing wire belonging to the same net, it terminates early at that point,
    creates a junction, and splits the intersected wire.
    
    Args:
        cwire_routing_pairs: List of (from_coord, to_coord, net_name) tuples to route
                            These are cwires that need to be autorouted (no valid cwiredef)
        trace_json: Full trace JSON for extracting components/sheets as obstacles
        lib_symbols_cache: Cache of loaded library symbols
        transform_pin_coordinate_func: Function to transform pin coordinates
        extract_pin_info_from_symbol_func: Function to extract pin info from symbol
    
    Returns:
        Tuple of (routed_wires, routing_junctions, wires_to_remove):
        - routed_wires: List of wire dicts generated by routing (includes split wires)
        - routing_junctions: List of junction dicts created at early termination points
        - wires_to_remove: List of wire UIDs that were split and should be removed
    """
    # If no cwires to route, return empty results
    if not cwire_routing_pairs:
        return [], [], []
    
    # Extract components, sheets, and wires from trace_json
    components = []
    sheets = []
    labels = []
    glabels = []
    # Track existing wires with their dict for potential splitting
    # Format: List of (start, end, wire_dict)
    existing_wire_dicts: List[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any]]] = []
    # Simple list for collision detection (no dict needed)
    existing_wires: List[Tuple[Tuple[float, float], Tuple[float, float]]] = []
    
    for item in trace_json:
        item_type = item.get('type')
        if item_type == 'component':
            components.append(item)
        elif item_type == 'sheet':
            sheets.append(item)
        elif item_type == 'label':
            labels.append(item)
        elif item_type == 'glabel':
            glabels.append(item)
        elif item_type == 'wire':
            # Extract wire segments with their dict for potential splitting
            points = item.get('points', [])
            if len(points) >= 2:
                start_pt = tuple(points[0]) if isinstance(points[0], list) else points[0]
                end_pt = tuple(points[-1]) if isinstance(points[-1], list) else points[-1]
                existing_wire_dicts.append((start_pt, end_pt, item))
                existing_wires.append((start_pt, end_pt))
    
    # Build wire-to-net mapping
    # We'll infer nets from:
    # 1. Labels at wire endpoints
    # 2. Component pins that wires connect to
    # 3. The net from cwires being routed (wires we create inherit the cwire's net)
    wire_to_net: Dict[str, Optional[str]] = {}
    
    # Map wire UIDs to nets from labels at their endpoints
    for wire_start, wire_end, wire_dict in existing_wire_dicts:
        wire_uid = wire_dict.get('uid')
        if not wire_uid:
            continue
        
        # Check if any label is at a wire endpoint
        for label in labels:
            label_pos = label.get('at', [])
            if len(label_pos) >= 2:
                label_pt = (label_pos[0], label_pos[1])
                label_text = label.get('text', '')
                if label_text:
                    dist_to_start = euclidean_distance(label_pt, wire_start)
                    dist_to_end = euclidean_distance(label_pt, wire_end)
                    if dist_to_start < 0.1 or dist_to_end < 0.1:
                        wire_to_net[wire_uid] = label_text
                        break
        
        # Check global labels too
        if wire_uid not in wire_to_net:
            for glabel in glabels:
                glabel_pos = glabel.get('at', [])
                if len(glabel_pos) >= 2:
                    glabel_pt = (glabel_pos[0], glabel_pos[1])
                    glabel_text = glabel.get('text', '')
                    if glabel_text:
                        dist_to_start = euclidean_distance(glabel_pt, wire_start)
                        dist_to_end = euclidean_distance(glabel_pt, wire_end)
                        if dist_to_start < 0.1 or dist_to_end < 0.1:
                            wire_to_net[wire_uid] = glabel_text
                            break
    
    # Map wire UIDs to nets from component pin connections
    for comp in components:
        pins = comp.get('pins', {})
        comp_uid = comp.get('uid')
        lib_id = comp.get('symbol')
        
        if not comp_uid or not lib_id or lib_id not in lib_symbols_cache:
            continue
        
        symbol_def = lib_symbols_cache[lib_id]
        symbol_pos = comp.get('at', [0, 0])
        symbol_rot = comp.get('rot', 0)
        unit = comp.get('unit', 1)
        body_style = comp.get('body_style', 1)
        
        for pin_num, net_name in pins.items():
            if net_name in ('NONE', 'DNC', None):
                continue
            
            # Get pin position
            pin_info = extract_pin_info_from_symbol_func(symbol_def, pin_num, unit=unit, body_style=body_style)
            if pin_info:
                x_off, y_off, pin_rot = pin_info
                pin_pos = transform_pin_coordinate_func((x_off, y_off, pin_rot), 
                                                        tuple(symbol_pos), 
                                                        symbol_rot)
                
                # Check if any wire endpoint matches this pin
                for wire_start, wire_end, wire_dict in existing_wire_dicts:
                    wire_uid = wire_dict.get('uid')
                    if not wire_uid or wire_uid in wire_to_net:
                        continue
                    
                    dist_to_start = euclidean_distance(pin_pos, wire_start)
                    dist_to_end = euclidean_distance(pin_pos, wire_end)
                    if dist_to_start < 0.1 or dist_to_end < 0.1:
                        wire_to_net[wire_uid] = net_name
    
    # Calculate component bounding boxes (obstacles)
    component_bboxes: List[Tuple[float, float, float, float]] = []
    
    # Add component bounding boxes
    for comp in components:
        comp_uid = comp.get('uid')
        lib_id = comp.get('symbol')
        if not comp_uid or not lib_id or lib_id not in lib_symbols_cache:
            continue
        
        symbol_def = lib_symbols_cache[lib_id]
        symbol_pos = comp.get('at', [0, 0])
        symbol_rot = comp.get('rot', 0)
        unit = comp.get('unit', 1)
        body_style = comp.get('body_style', 1)
        pins = comp.get('pins', {})
        
        bbox = calculate_component_bbox(
            symbol_def, pins, unit, body_style, symbol_pos, symbol_rot,
            transform_pin_coordinate_func, extract_pin_info_from_symbol_func
        )
        if bbox:
            component_bboxes.append(bbox)
    
    # Add sheet bounding boxes
    for sheet in sheets:
        sheet_pos = sheet.get('at', [0, 0])
        sheet_size = sheet.get('size', [76.2, 50.8])  # Default size if not specified
        
        if sheet_pos and len(sheet_pos) >= 2 and sheet_size and len(sheet_size) >= 2:
            sheet_bbox = calculate_sheet_bbox(tuple(sheet_pos), tuple(sheet_size))
            component_bboxes.append(sheet_bbox)
    
    # Build map of all pin positions with their net names
    # This is used to avoid routing over pins (unless they're on the same net)
    all_pin_positions: Dict[Tuple[float, float], Optional[str]] = {}
    
    for comp in components:
        pins = comp.get('pins', {})
        comp_uid = comp.get('uid')
        lib_id = comp.get('symbol')
        
        if not comp_uid or not lib_id or lib_id not in lib_symbols_cache:
            continue
        
        symbol_def = lib_symbols_cache[lib_id]
        symbol_pos = comp.get('at', [0, 0])
        symbol_rot = comp.get('rot', 0)
        unit = comp.get('unit', 1)
        body_style = comp.get('body_style', 1)
        
        for pin_num, net_name in pins.items():
            # Get pin position
            pin_info = extract_pin_info_from_symbol_func(symbol_def, pin_num, unit=unit, body_style=body_style)
            if pin_info:
                x_off, y_off, pin_rot = pin_info
                pin_pos = transform_pin_coordinate_func((x_off, y_off, pin_rot), 
                                                        tuple(symbol_pos), 
                                                        symbol_rot)
                # Store pin position with its net (None if NONE/DNC)
                pin_net = net_name if net_name not in ('NONE', 'DNC', None) else None
                all_pin_positions[pin_pos] = pin_net
    
    # Route each cwire with net-aware early termination
    all_routed_wires: List[Dict[str, Any]] = []
    all_junctions: List[Dict[str, Any]] = []
    wires_to_remove: List[str] = []
    
    # Track wires we've created during routing (for same-net detection in later routes)
    routed_wire_dicts: List[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any], Optional[str]]] = []
    
    for from_coord, to_coord, net_name in cwire_routing_pairs:
        # Build list of same-net wires for early termination
        same_net_wires: List[Tuple[Tuple[float, float], Tuple[float, float], Dict[str, Any]]] = []
        
        if net_name:
            # Add existing wires on the same net
            for wire_start, wire_end, wire_dict in existing_wire_dicts:
                wire_uid = wire_dict.get('uid')
                if wire_uid and wire_to_net.get(wire_uid) == net_name:
                    same_net_wires.append((wire_start, wire_end, wire_dict))
            
            # Add previously routed wires on the same net
            for wire_start, wire_end, wire_dict, wire_net in routed_wire_dicts:
                if wire_net == net_name:
                    same_net_wires.append((wire_start, wire_end, wire_dict))
        
        # Use A* to find path from start to end, with potential early termination
        # Build pin_positions excluding the start and end pins of this route
        route_pin_positions = {}
        for pin_pos, pin_net in all_pin_positions.items():
            # Exclude start and end pins from collision detection
            dist_to_start = euclidean_distance(pin_pos, from_coord)
            dist_to_end = euclidean_distance(pin_pos, to_coord)
            if dist_to_start > 0.1 and dist_to_end > 0.1:
                route_pin_positions[pin_pos] = pin_net
        
        path, early_term_info = astar_pathfind(
            from_coord, 
            to_coord, 
            component_bboxes,
            existing_wires=existing_wires,
            same_net_wires=same_net_wires,
            pin_positions=route_pin_positions,
            current_net=net_name,
            grid_size=GRID_SIZE
        )
        
        if path:
            # Handle early termination on same-net wire or same-net pin
            if early_term_info:
                termination_point = early_term_info['point']
                
                if 'wire' in early_term_info:
                    # Route terminated early on a same-net wire
                    intersected_wire = early_term_info['wire']
                    wire_start = early_term_info['wire_start']
                    wire_end = early_term_info['wire_end']
                    
                    # Update path to end at termination point
                    path[-1] = termination_point
                    
                    # Create a junction at the termination point
                    junction = {
                        'type': 'junction',
                        'at': [termination_point[0], termination_point[1]],
                        'uid': str(uuid.uuid4())
                    }
                    all_junctions.append(junction)
                    
                    # Split the intersected wire into two segments
                    intersected_uid = intersected_wire.get('uid')
                    if intersected_uid:
                        # Check if the termination point is at an endpoint (no split needed)
                        dist_to_start = euclidean_distance(termination_point, wire_start)
                        dist_to_end = euclidean_distance(termination_point, wire_end)
                        
                        if dist_to_start > 0.1 and dist_to_end > 0.1:
                            # Point is in the middle - need to split
                            wire1, wire2 = split_wire_at_point(intersected_wire, termination_point)
                            if wire1 and wire2:
                                # Mark original wire for removal
                                wires_to_remove.append(intersected_uid)
                                
                                # Add split wires to results
                                all_routed_wires.append(wire1)
                                all_routed_wires.append(wire2)
                                
                                # Update existing_wires for collision detection
                                wire1_pts = wire1.get('points', [])
                                wire2_pts = wire2.get('points', [])
                                if len(wire1_pts) >= 2:
                                    existing_wires.append((tuple(wire1_pts[0]), tuple(wire1_pts[-1])))
                                if len(wire2_pts) >= 2:
                                    existing_wires.append((tuple(wire2_pts[0]), tuple(wire2_pts[-1])))
                                
                                # Add to routed_wire_dicts for same-net tracking
                                routed_wire_dicts.append((tuple(wire1_pts[0]), tuple(wire1_pts[-1]), wire1, net_name))
                                routed_wire_dicts.append((tuple(wire2_pts[0]), tuple(wire2_pts[-1]), wire2, net_name))
                
                elif 'pin' in early_term_info:
                    # Route terminated early on a same-net pin
                    # No junction or wire splitting needed - just end at the pin
                    path[-1] = termination_point
            
            # Simplify the path
            path = simplify_path(path)
            
            # Ensure path starts at exact coordinate
            if len(path) > 0:
                path[0] = from_coord
                # For early termination, path[-1] is already set to termination_point
                if not early_term_info:
                    path[-1] = to_coord
            
            # Split path into wire segments
            pin_positions = {from_coord}
            if early_term_info:
                pin_positions.add(early_term_info['point'])
            else:
                pin_positions.add(to_coord)
            path_wires = split_path_into_wires(path, pin_positions=pin_positions)
            
            # Add wires to results
            all_routed_wires.extend(path_wires)
            
            # Add newly routed wire segments to existing_wires so subsequent
            # routes will avoid overlapping with them
            for wire in path_wires:
                wire_points = wire.get('points', [])
                if len(wire_points) >= 2:
                    start_pt = tuple(wire_points[0])
                    end_pt = tuple(wire_points[-1])
                    existing_wires.append((start_pt, end_pt))
                    # Track for same-net detection in later routes
                    routed_wire_dicts.append((start_pt, end_pt, wire, net_name))
    
    return all_routed_wires, all_junctions, wires_to_remove
