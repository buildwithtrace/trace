#!/usr/bin/env python3
"""
Compile Python source files to bytecode (.pyc) for distribution.

This script:
1. Compiles all .py files in a directory tree to .pyc
2. Optionally removes the .py source files (for distribution)
3. Creates a __pycache__ directory structure for bytecode

Usage:
    python compile_python_to_bytecode.py <source_dir> [--remove-source]
"""

import py_compile
import sys
import os
import argparse
from pathlib import Path
import shutil


def compile_directory(source_dir: Path, remove_source: bool = False) -> tuple[int, int]:
    """
    Compile all Python files in a directory tree to bytecode.
    
    Args:
        source_dir: Root directory containing Python files
        remove_source: If True, remove .py files after compilation
        
    Returns:
        Tuple of (compiled_count, error_count)
    """
    compiled_count = 0
    error_count = 0
    
    # Find all .py files
    py_files = list(source_dir.rglob("*.py"))
    
    print(f"Found {len(py_files)} Python files to compile in {source_dir}")
    
    for py_file in py_files:
        try:
            # Skip __pycache__ directories
            if "__pycache__" in py_file.parts:
                continue
            
            # Compile to bytecode
            py_compile.compile(
                str(py_file),
                doraise=True,
                optimize=2  # Maximum optimization (-OO)
            )
            
            compiled_count += 1
            print(f"  ✓ Compiled: {py_file.relative_to(source_dir)}")
            
            # Remove source file if requested
            if remove_source:
                py_file.unlink()
                print(f"    Removed source: {py_file.relative_to(source_dir)}")
                
        except Exception as e:
            error_count += 1
            print(f"  ✗ Error compiling {py_file.relative_to(source_dir)}: {e}", file=sys.stderr)
    
    return compiled_count, error_count


def main():
    parser = argparse.ArgumentParser(
        description="Compile Python source files to bytecode (.pyc)"
    )
    parser.add_argument(
        "source_dir",
        type=Path,
        help="Directory containing Python files to compile"
    )
    parser.add_argument(
        "--remove-source",
        action="store_true",
        help="Remove .py source files after compilation (use for distribution)"
    )
    
    args = parser.parse_args()
    
    if not args.source_dir.exists():
        print(f"Error: Directory not found: {args.source_dir}", file=sys.stderr)
        return 1
    
    if not args.source_dir.is_dir():
        print(f"Error: Not a directory: {args.source_dir}", file=sys.stderr)
        return 1
    
    print(f"Compiling Python files in: {args.source_dir}")
    if args.remove_source:
        print("Warning: Source files will be removed after compilation")
    
    compiled, errors = compile_directory(args.source_dir, args.remove_source)
    
    print(f"\nCompilation complete:")
    print(f"  Compiled: {compiled} files")
    print(f"  Errors: {errors} files")
    
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

