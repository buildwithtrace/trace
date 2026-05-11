#!/usr/bin/env python3
"""
Update copyright headers for Trace-modified files.

For NEW files (created by Trace): Replace entire header with Trace header
For MODIFIED files (existed in KiCad): Add Trace copyright line after KiCad copyright
"""

import subprocess
import os
import re

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORK_COMMIT = "313194f63b406ab13a5a7c7eb90b63e4878b7dad"

# Copyright patterns
KICAD_COPYRIGHT = "Copyright The KiCad Developers, see AUTHORS.txt for contributors."
TRACE_COPYRIGHT = "Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors."

# Old KiCad header (for new files)
KICAD_HEADER = """/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */"""

# New Trace header (for new files)
TRACE_HEADER = """/*
 * This program source code file is part of Trace, a free EDA CAD application.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */"""

def get_new_files():
    """Get files that were ADDED (created) since the fork."""
    result = subprocess.run(
        ["git", "diff", "--diff-filter=A", "--name-only", f"{FORK_COMMIT}..HEAD", "--", "*.cpp", "*.h"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True
    )
    return [f for f in result.stdout.strip().split('\n') if f]

def get_modified_files():
    """Get files that were MODIFIED (not added) since the fork."""
    result = subprocess.run(
        ["git", "diff", "--diff-filter=M", "--name-only", f"{FORK_COMMIT}..HEAD", "--", "*.cpp", "*.h"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True
    )
    return [f for f in result.stdout.strip().split('\n') if f]

def update_new_file(filepath):
    """For new files: Replace entire header with Trace header."""
    full_path = os.path.join(REPO_ROOT, filepath)
    if not os.path.exists(full_path):
        print(f"  SKIP (not found): {filepath}")
        return False
    
    with open(full_path, 'r') as f:
        content = f.read()
    
    # Check if already has correct Trace header
    if "part of Trace, a free EDA" in content:
        print(f"  SKIP (already has Trace header): {filepath}")
        return False
    
    # Replace "part of KiCad" with "part of Trace" for new files
    if "part of KiCad, a free EDA" in content:
        new_content = content.replace("part of KiCad, a free EDA", "part of Trace, a free EDA")
        with open(full_path, 'w') as f:
            f.write(new_content)
        print(f"  UPDATED (new file header): {filepath}")
        return True
    else:
        print(f"  SKIP (no KiCad header found): {filepath}")
        return False

def update_modified_file(filepath):
    """For modified files: Add Trace copyright after KiCad copyright."""
    full_path = os.path.join(REPO_ROOT, filepath)
    if not os.path.exists(full_path):
        print(f"  SKIP (not found): {filepath}")
        return False
    
    with open(full_path, 'r') as f:
        content = f.read()
    
    # Check if already has Trace copyright
    if TRACE_COPYRIGHT in content:
        print(f"  SKIP (already has Trace copyright): {filepath}")
        return False
    
    # Add Trace copyright after KiCad copyright
    if KICAD_COPYRIGHT in content:
        # Add Trace copyright on the next line
        new_content = content.replace(
            KICAD_COPYRIGHT,
            f"{KICAD_COPYRIGHT}\n * {TRACE_COPYRIGHT}"
        )
        with open(full_path, 'w') as f:
            f.write(new_content)
        print(f"  UPDATED (modified file): {filepath}")
        return True
    else:
        print(f"  SKIP (no KiCad copyright found): {filepath}")
        return False

def main():
    os.chdir(REPO_ROOT)
    
    print("=" * 60)
    print("Updating Trace Copyright Headers")
    print("=" * 60)
    
    # Get file lists
    new_files = get_new_files()
    modified_files = get_modified_files()
    
    print(f"\nFound {len(new_files)} NEW files (created by Trace)")
    print(f"Found {len(modified_files)} MODIFIED files (from KiCad)")
    
    updated_count = 0
    
    # Process new files
    print("\n--- Processing NEW files (replace KiCad -> Trace) ---")
    for filepath in new_files:
        if update_new_file(filepath):
            updated_count += 1
    
    # Process modified files
    print("\n--- Processing MODIFIED files (add Trace after KiCad) ---")
    for filepath in modified_files:
        if update_modified_file(filepath):
            updated_count += 1
    
    print("\n" + "=" * 60)
    print(f"Done! Updated {updated_count} files.")
    print("=" * 60)

if __name__ == "__main__":
    main()
