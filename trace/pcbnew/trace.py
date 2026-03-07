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
Main conversion interface for trace_pcb, trace_json, and kicad_pcb formats.

Provides a unified API for converting between:
- trace_pcb <--> trace_json
- trace_json <--> kicad_pcb
- trace_pcb <--> kicad_pcb
"""

from typing import List, Dict, Any
import sys
import os

# Handle both module import and direct script execution
try:
    from .trace_parser import parse_trace_pcb
    from .trace_converter import convert_to_trace_pcb
    from .sexp_to_trace_json import sexp_to_trace_json
    from .trace_json_to_sexp import trace_json_to_sexp
except (ImportError, ValueError):
    # Fallback for direct script execution - add current directory to path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    from trace_parser import parse_trace_pcb
    from trace_converter import convert_to_trace_pcb
    from sexp_to_trace_json import sexp_to_trace_json
    from trace_json_to_sexp import trace_json_to_sexp


class TraceConverter:
    """
    Unified converter for trace_pcb, trace_json, and kicad_pcb formats.
    
    trace_json format is a list of statement dictionaries (Python objects).
    trace_pcb format is a string representation.
    kicad_pcb format is KiCad's S-expression format (string).
    """
    
    @staticmethod
    def trace_pcb_to_trace_json(trace_pcb_content: str) -> List[Dict[str, Any]]:
        """
        Convert trace_pcb string to trace_json (list of statement dicts).
        
        Args:
            trace_pcb_content: The .trace_pcb file content as a string
            
        Returns:
            List of dictionaries, each representing a statement
            
        Raises:
            TraceParseError: If parsing fails
        """
        return parse_trace_pcb(trace_pcb_content)
    
    @staticmethod
    def trace_json_to_trace_pcb(trace_json: List[Dict[str, Any]]) -> str:
        """
        Convert trace_json (list of statement dicts) to trace_pcb string.
        
        Args:
            trace_json: List of statement dictionaries
            
        Returns:
            Formatted trace_pcb content as string
        """
        return convert_to_trace_pcb(trace_json)
    
    @staticmethod
    def kicad_pcb_to_trace_json(kicad_pcb_content: str) -> List[Dict[str, Any]]:
        """
        Convert kicad_pcb string to trace_json (list of statement dicts).
        
        Args:
            kicad_pcb_content: The .kicad_pcb file content as a string
            
        Returns:
            List of dictionaries, each representing a statement
        """
        return sexp_to_trace_json(kicad_pcb_content)
    
    @staticmethod
    def trace_json_to_kicad_pcb(trace_json: List[Dict[str, Any]], 
                                 existing_pcb_content: str = None,
                                 kicad_sch_content: str = None,
                                 footprint_paths: List[str] = None) -> str:
        """
        Convert trace_json (list of statement dicts) to kicad_pcb string.
        
        Args:
            trace_json: List of statement dictionaries
            existing_pcb_content: Optional content of existing kicad_pcb file to merge with.
                                  If None, a minimal PCB structure will be created.
            kicad_sch_content: Optional content of corresponding kicad_sch file for footprint-symbol mapping.
                              Required if existing_pcb_content is provided.
            footprint_paths: Optional list of footprint library directory paths to search
        
        Returns:
            Formatted kicad_pcb content as string
            
        Raises:
            ValueError: If existing_pcb_content is provided but kicad_sch_content is not
        """
        if existing_pcb_content is not None:
            if kicad_sch_content is None:
                raise ValueError("kicad_sch_content is required when existing_pcb_content is provided")
            return trace_json_to_sexp(trace_json, existing_pcb_content, kicad_sch_content, footprint_paths=footprint_paths)
        else:
            # Create minimal PCB structure
            # This is a fallback - ideally users should provide existing_pcb_content
            minimal_pcb = """(kicad_pcb
  (version 20251101)
  (generator "pcbnew")
  (generator_version "9.99")
  (general
    (thickness 1.6)
    (legacy_teardrops no)
  )
  (layers
    (0 "F.Cu" signal)
    (2 "B.Cu" signal)
    (31 "F.SilkS" user "F.Silkscreen")
    (29 "B.SilkS" user "B.Silkscreen")
    (25 "Edge.Cuts" user)
  )
  (setup
    (stackup
      (layer "F.Cu"
        (type copper)
        (thickness 0.035)
      )
      (layer "B.Cu"
        (type copper)
        (thickness 0.035)
      )
    )
  )
  (embedded_fonts no)
)"""
            # Use minimal PCB as base
            return trace_json_to_sexp(trace_json, minimal_pcb, kicad_sch_content or "", footprint_paths=footprint_paths)
        
    
    # Convenience methods for file operations
    
    @staticmethod
    def trace_pcb_file_to_trace_json_file(trace_pcb_path: str, trace_json_path: str):
        """
        Convert a trace_pcb file to a trace_json file.
        
        Args:
            trace_pcb_path: Path to input .trace_pcb file
            trace_json_path: Path to output JSON file
        """
        with open(trace_pcb_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.trace_pcb_to_trace_json(content)
        
        import json
        with open(trace_json_path, "w") as f:
            json.dump(trace_json, f, indent=2)
    
    @staticmethod
    def trace_json_file_to_trace_pcb_file(trace_json_path: str, trace_pcb_path: str):
        """
        Convert a trace_json file to a trace_pcb file.
        
        Args:
            trace_json_path: Path to input JSON file
            trace_pcb_path: Path to output .trace_pcb file
        """
        import json
        with open(trace_json_path, "r") as f:
            trace_json = json.load(f)
        
        trace_pcb_content = TraceConverter.trace_json_to_trace_pcb(trace_json)
        
        with open(trace_pcb_path, "w") as f:
            f.write(trace_pcb_content)
    
    @staticmethod
    def kicad_pcb_file_to_trace_json_file(kicad_pcb_path: str, trace_json_path: str):
        """
        Convert a kicad_pcb file to a trace_json file.
        
        Args:
            kicad_pcb_path: Path to input .kicad_pcb file
            trace_json_path: Path to output JSON file
        """
        with open(kicad_pcb_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.kicad_pcb_to_trace_json(content)
        
        import json
        with open(trace_json_path, "w") as f:
            json.dump(trace_json, f, indent=2)
    
    @staticmethod
    def trace_json_file_to_kicad_pcb_file(trace_json_path: str, kicad_pcb_path: str,
                                          existing_pcb_path: str = None,
                                          kicad_sch_path: str = None,
                                          footprint_paths: List[str] = None):
        """
        Convert a trace_json file to a kicad_pcb file.
        
        Args:
            trace_json_path: Path to input JSON file
            kicad_pcb_path: Path to output .kicad_pcb file
            existing_pcb_path: Optional path to existing kicad_pcb file to merge with
            kicad_sch_path: Optional path to corresponding kicad_sch file for footprint-symbol mapping
                           (required if existing_pcb_path is provided)
            footprint_paths: Optional list of footprint library directory paths to search
        """
        import json
        with open(trace_json_path, "r") as f:
            trace_json = json.load(f)
        
        existing_pcb_content = None
        kicad_sch_content = None
        
        if existing_pcb_path:
            with open(existing_pcb_path, "r") as f:
                existing_pcb_content = f.read()
        
        if kicad_sch_path:
            with open(kicad_sch_path, "r") as f:
                kicad_sch_content = f.read()
        
        kicad_pcb_content = TraceConverter.trace_json_to_kicad_pcb(
            trace_json, existing_pcb_content, kicad_sch_content, footprint_paths=footprint_paths
        )
        
        with open(kicad_pcb_path, "w") as f:
            f.write(kicad_pcb_content)
    
    @staticmethod
    def trace_pcb_file_to_kicad_pcb_file(trace_pcb_path: str, kicad_pcb_path: str,
                                          existing_pcb_path: str = None,
                                          kicad_sch_path: str = None,
                                          footprint_paths: List[str] = None):
        """
        Convert a trace_pcb file to a kicad_pcb file.
        
        Args:
            trace_pcb_path: Path to input .trace_pcb file
            kicad_pcb_path: Path to output .kicad_pcb file
            existing_pcb_path: Optional path to existing kicad_pcb file to merge with
            kicad_sch_path: Optional path to corresponding kicad_sch file for footprint-symbol mapping
                           (required if existing_pcb_path is provided)
            footprint_paths: Optional list of footprint library directory paths to search
        """
        with open(trace_pcb_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.trace_pcb_to_trace_json(content)
        
        existing_pcb_content = None
        kicad_sch_content = None
        
        if existing_pcb_path:
            with open(existing_pcb_path, "r") as f:
                existing_pcb_content = f.read()
        
        if kicad_sch_path:
            with open(kicad_sch_path, "r") as f:
                kicad_sch_content = f.read()
        
        kicad_pcb_content = TraceConverter.trace_json_to_kicad_pcb(
            trace_json, existing_pcb_content, kicad_sch_content, footprint_paths=footprint_paths
        )
        
        with open(kicad_pcb_path, "w") as f:
            f.write(kicad_pcb_content)
            
    @staticmethod
    def kicad_pcb_file_to_trace_pcb_file(kicad_pcb_path: str, trace_pcb_path: str):
        """
        Convert a kicad_pcb file to a trace_pcb file.
        
        Args:
            kicad_pcb_path: Path to input .kicad_pcb file
            trace_pcb_path: Path to output .trace_pcb file
        """
        with open(kicad_pcb_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.kicad_pcb_to_trace_json(content)
        
        trace_pcb_content = TraceConverter.trace_json_to_trace_pcb(trace_json)
        
        with open(trace_pcb_path, "w") as f:
            f.write(trace_pcb_content)
    
# =============================================================================
# Command-line interface
# =============================================================================

if __name__ == "__main__":
    import sys
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Convert between trace_pcb, trace_json, and kicad_pcb formats"
    )
    parser.add_argument(
        "input_file",
        help="Input file path"
    )
    parser.add_argument(
        "output_file",
        help="Output file path"
    )
    parser.add_argument(
        "-f", "--from", 
        dest="from_format",
        choices=["trace_pcb", "trace_json", "kicad_pcb"],
        required=True,
        help="Input format"
    )
    parser.add_argument(
        "-t", "--to",
        dest="to_format",
        choices=["trace_pcb", "trace_json", "kicad_pcb"],
        required=True,
        help="Output format"
    )
    parser.add_argument(
        "--existing-pcb",
        dest="existing_pcb_path",
        help="Path to existing kicad_pcb file (required for trace_json/trace_pcb -> kicad_pcb conversion)"
    )
    parser.add_argument(
        "--kicad-sch",
        dest="kicad_sch_path",
        help="Path to corresponding kicad_sch file (required when --existing-pcb is provided)"
    )
    parser.add_argument(
        "--symbol-paths",
        dest="symbol_paths",
        help="Colon or semicolon-separated list of symbol library directories"
    )
    parser.add_argument(
        "--footprint-paths",
        dest="footprint_paths",
        help="Colon or semicolon-separated list of footprint library directories"
    )
    
    args = parser.parse_args()
    
    # Parse library paths if provided
    footprint_paths = None
    if args.footprint_paths:
        # Handle both colon and semicolon separators
        footprint_paths = [p.strip() for p in args.footprint_paths.replace(';', ':').split(':') if p.strip()]
    
    converter = TraceConverter()
    
    try:
        # Determine conversion method
        if args.from_format == "trace_pcb" and args.to_format == "trace_json":
            converter.trace_pcb_file_to_trace_json_file(args.input_file, args.output_file)
        elif args.from_format == "trace_json" and args.to_format == "trace_pcb":
            converter.trace_json_file_to_trace_pcb_file(args.input_file, args.output_file)
        elif args.from_format == "kicad_pcb" and args.to_format == "trace_json":
            converter.kicad_pcb_file_to_trace_json_file(args.input_file, args.output_file)
        elif args.from_format == "trace_json" and args.to_format == "kicad_pcb":
            converter.trace_json_file_to_kicad_pcb_file(
                args.input_file, args.output_file,
                args.existing_pcb_path, args.kicad_sch_path, footprint_paths
            )
        elif args.from_format == "kicad_pcb" and args.to_format == "trace_pcb":
            converter.kicad_pcb_file_to_trace_pcb_file(args.input_file, args.output_file)
        elif args.from_format == "trace_pcb" and args.to_format == "kicad_pcb":
            converter.trace_pcb_file_to_kicad_pcb_file(
                args.input_file, args.output_file,
                args.existing_pcb_path, args.kicad_sch_path, footprint_paths
            )
        else:
            print(f"Error: Conversion from {args.from_format} to {args.to_format} is not supported.", file=sys.stderr)
            sys.exit(1)
        
        print(f"Successfully converted {args.input_file} ({args.from_format}) to {args.output_file} ({args.to_format})")
    
    except NotImplementedError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
