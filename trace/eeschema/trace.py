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
Main conversion interface for trace_sch, trace_json, and kicad_sch formats.

Provides a unified API for converting between:
- trace_sch <--> trace_json
- trace_json <-- kicad_sch
- trace_json --> kicad_sch (TODO)
"""

from typing import List, Dict, Any, Optional, Tuple
import json
import re
import sys
import os

# Exit codes used by the CLI to distinguish lint levels for the C++ caller.
EXIT_TRACE_SCH_PARSE_ERROR = 1        # Level 1: trace_sch content is invalid
EXIT_KICAD_SCH_VALIDATION_ERROR = 2   # Level 2: generated kicad_sch failed round-trip validation


class KicadSchValidationError(Exception):
    """Raised when the generated kicad_sch fails round-trip validation."""

    def __init__(self, message: str, detail: str = "", snippet: str = "", error_line: int = 0):
        self.message = message
        self.detail = detail
        self.snippet = snippet
        self.error_line = error_line
        super().__init__(message)


def _split_lib_paths(raw: str) -> List[str]:
    """Split a colon-or-semicolon-separated path list, respecting Windows drive letters."""
    if ';' in raw:
        return [p.strip() for p in raw.split(';') if p.strip()]
    parts = raw.split(':')
    result: List[str] = []
    i = 0
    while i < len(parts):
        p = parts[i]
        if len(p) == 1 and p.isalpha() and i + 1 < len(parts):
            result.append(f"{p}:{parts[i + 1]}".strip())
            i += 2
        else:
            stripped = p.strip()
            if stripped:
                result.append(stripped)
            i += 1
    return result

# Handle both module import and direct script execution
try:
    from .trace_parser import parse_trace_sch
    from .trace_converter import convert_to_trace_sch
    from .sexp_to_trace_json import sexp_to_trace_json
    from .trace_json_to_sexp import trace_json_to_sexp
except (ImportError, ValueError):
    # Fallback for direct script execution - add current directory to path
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    from trace_parser import parse_trace_sch
    from trace_converter import convert_to_trace_sch
    from sexp_to_trace_json import sexp_to_trace_json
    from trace_json_to_sexp import trace_json_to_sexp


class TraceConverter:
    """
    Unified converter for trace_sch, trace_json, and kicad_sch formats.
    
    trace_json format is a list of statement dictionaries (Python objects).
    trace_sch format is a string representation.
    kicad_sch format is KiCad's S-expression format (string).
    """
    
    @staticmethod
    def trace_sch_to_trace_json(trace_sch_content: str) -> Tuple[List[Dict[str, Any]], List]:
        """
        Convert trace_sch string to trace_json (list of statement dicts).
        
        Args:
            trace_sch_content: The .trace_sch file content as a string
            
        Returns:
            Tuple of (statements, warnings):
              - statements: List of dictionaries, each representing a statement
              - warnings: List of TraceParseWarning for critical (non-fatal) issues
            
        Raises:
            TraceParseError: If parsing fails
        """
        return parse_trace_sch(trace_sch_content)
    
    @staticmethod
    def trace_json_to_trace_sch(trace_json: List[Dict[str, Any]]) -> str:
        """
        Convert trace_json (list of statement dicts) to trace_sch string.
        
        Args:
            trace_json: List of statement dictionaries
            
        Returns:
            Formatted trace_sch content as string
        """
        return convert_to_trace_sch(trace_json)
    
    @staticmethod
    def kicad_sch_to_trace_json(kicad_sch_content: str) -> List[Dict[str, Any]]:
        """
        Convert kicad_sch string to trace_json (list of statement dicts).
        
        Args:
            kicad_sch_content: The .kicad_sch file content as a string
            
        Returns:
            List of dictionaries, each representing a statement
        """
        return sexp_to_trace_json(kicad_sch_content)
    
    @staticmethod
    def trace_json_to_kicad_sch(trace_json: List[Dict[str, Any]], existing_sch_content: Optional[str] = None, symbol_paths: List[str] = None) -> str:
        """
        Convert trace_json (list of statement dicts) to kicad_sch string.
        
        Args:
            trace_json: List of statement dictionaries
            existing_sch_content: Optional content of existing kicad_sch file to merge with.
                                 If None, generates from scratch (backward compatible).
            symbol_paths: Optional list of symbol library directory paths to search
        
        Returns:
            Formatted kicad_sch content as string
        """
        return trace_json_to_sexp(trace_json, existing_sch_content=existing_sch_content, symbol_paths=symbol_paths)
        
    
    # Convenience methods for file operations
    
    @staticmethod
    def trace_sch_file_to_trace_json_file(trace_sch_path: str, trace_json_path: str):
        """
        Convert a trace_sch file to a trace_json file.
        
        Args:
            trace_sch_path: Path to input .trace_sch file
            trace_json_path: Path to output JSON file
        """
        with open(trace_sch_path, "r") as f:
            content = f.read()
        
        base_dir = os.path.dirname(os.path.abspath(trace_sch_path))
        trace_json, warnings = parse_trace_sch(content, base_dir=base_dir)
        
        if warnings:
            import sys as _sys
            for w in warnings:
                print(f"  {w}", file=_sys.stderr)
        
        import json
        with open(trace_json_path, "w") as f:
            json.dump(trace_json, f, indent=2)
    
    @staticmethod
    def trace_json_file_to_trace_sch_file(trace_json_path: str, trace_sch_path: str):
        """
        Convert a trace_json file to a trace_sch file.
        
        Args:
            trace_json_path: Path to input JSON file
            trace_sch_path: Path to output .trace_sch file
        """
        import json
        with open(trace_json_path, "r") as f:
            trace_json = json.load(f)
        
        trace_sch_content = TraceConverter.trace_json_to_trace_sch(trace_json)
        
        with open(trace_sch_path, "w") as f:
            f.write(trace_sch_content)
    
    @staticmethod
    def kicad_sch_file_to_trace_json_file(kicad_sch_path: str, trace_json_path: str):
        """
        Convert a kicad_sch file to a trace_json file.
        
        Args:
            kicad_sch_path: Path to input .kicad_sch file
            trace_json_path: Path to output JSON file
        """
        with open(kicad_sch_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.kicad_sch_to_trace_json(content)
        
        import json
        with open(trace_json_path, "w") as f:
            json.dump(trace_json, f, indent=2)
    
    @staticmethod
    def _make_snippet(content: str, error_line: int, context: int = 10) -> str:
        """Return numbered lines around *error_line* (1-based).

        Format: ``{line_number} | {line_content}``
        Shows *context* lines before and after the error line.
        Falls back to the first ``2*context+1`` lines when *error_line* is 0.
        """
        lines = content.splitlines()
        total = len(lines)
        if error_line <= 0:
            error_line = min(context + 1, total)
        start = max(0, error_line - 1 - context)
        end = min(total, error_line + context)
        width = len(str(end))
        parts: List[str] = []
        for idx in range(start, end):
            parts.append(f"{idx + 1:>{width}} | {lines[idx]}")
        return "\n".join(parts)

    @staticmethod
    def _validate_kicad_output(kicad_sch_content: str) -> None:
        """Validate generated kicad_sch by attempting to parse it back.

        Raises KicadSchValidationError if the round-trip parse fails, meaning
        KiCad's C++ parser would also reject the file.
        """
        try:
            sexp_to_trace_json(kicad_sch_content)
        except Exception as exc:
            detail = str(exc)
            # Try to extract a line number from the exception message
            error_line = 0
            m = re.search(r'line\s+(\d+)', detail, re.IGNORECASE)
            if m:
                error_line = int(m.group(1))
            snippet = TraceConverter._make_snippet(kicad_sch_content, error_line)
            raise KicadSchValidationError(
                "Generated kicad_sch failed round-trip validation",
                detail=detail,
                snippet=snippet,
                error_line=error_line,
            ) from exc

    @staticmethod
    def trace_json_file_to_kicad_sch_file(trace_json_path: str, kicad_sch_path: str, existing_sch_path: Optional[str] = None, symbol_paths: List[str] = None):
        """
        Convert a trace_json file to a kicad_sch file.
        
        Args:
            trace_json_path: Path to input JSON file
            kicad_sch_path: Path to output .kicad_sch file
            existing_sch_path: Optional path to existing .kicad_sch file to merge with.
                               If None, generates from scratch (backward compatible).
            symbol_paths: Optional list of symbol library directory paths to search
        """
        import json as _json
        with open(trace_json_path, "r") as f:
            trace_json = _json.load(f)
        
        existing_sch_content = None
        if existing_sch_path and os.path.exists(existing_sch_path):
            with open(existing_sch_path, "r") as f:
                existing_sch_content = f.read()
        
        kicad_sch_content = TraceConverter.trace_json_to_kicad_sch(trace_json, existing_sch_content=existing_sch_content, symbol_paths=symbol_paths)
        TraceConverter._validate_kicad_output(kicad_sch_content)
        
        with open(kicad_sch_path, "w") as f:
            f.write(kicad_sch_content)
    
    @staticmethod
    def trace_sch_file_to_kicad_sch_file(trace_sch_path: str, kicad_sch_path: str, existing_sch_path: Optional[str] = None, symbol_paths: List[str] = None):
        """
        Convert a trace_sch file to a kicad_sch file.
        
        Args:
            trace_sch_path: Path to input .trace_sch file
            kicad_sch_path: Path to output .kicad_sch file
            existing_sch_path: Optional path to existing .kicad_sch file to merge with.
                               If None, generates from scratch (backward compatible).
            symbol_paths: Optional list of symbol library directory paths to search
        """
        with open(trace_sch_path, "r") as f:
            content = f.read()
        
        base_dir = os.path.dirname(os.path.abspath(trace_sch_path))
        trace_json, warnings = parse_trace_sch(content, base_dir=base_dir)
        
        if warnings:
            import sys as _sys
            for w in warnings:
                print(f"  {w}", file=_sys.stderr)
        
        existing_sch_content = None
        if existing_sch_path and os.path.exists(existing_sch_path):
            with open(existing_sch_path, "r") as f:
                existing_sch_content = f.read()
        
        kicad_sch_content = TraceConverter.trace_json_to_kicad_sch(trace_json, existing_sch_content=existing_sch_content, symbol_paths=symbol_paths)
        TraceConverter._validate_kicad_output(kicad_sch_content)
        
        with open(kicad_sch_path, "w") as f:
            f.write(kicad_sch_content)
            
    @staticmethod
    def kicad_sch_file_to_trace_sch_file(kicad_sch_path: str, trace_sch_path: str):
        """
        Convert a kicad_sch file to a trace_sch file.
        
        Args:
            kicad_sch_path: Path to input .kicad_sch file
            trace_sch_path: Path to output .trace_sch file
        """
        with open(kicad_sch_path, "r") as f:
            content = f.read()
        
        trace_json = TraceConverter.kicad_sch_to_trace_json(content)
        
        trace_sch_content = TraceConverter.trace_json_to_trace_sch(trace_json)
        
        with open(trace_sch_path, "w") as f:
            f.write(trace_sch_content)
    
# =============================================================================
# Command-line interface
# =============================================================================

if __name__ == "__main__":
    import sys
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Convert between trace_sch, trace_json, and kicad_sch formats"
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
        choices=["trace_sch", "trace_json", "kicad_sch"],
        required=True,
        help="Input format"
    )
    parser.add_argument(
        "-t", "--to",
        dest="to_format",
        choices=["trace_sch", "trace_json", "kicad_sch"],
        required=True,
        help="Output format"
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
    parser.add_argument(
        "--existing-sch",
        dest="existing_sch_path",
        help="Path to existing .kicad_sch file to merge with (only for trace_sch/trace_json -> kicad_sch conversions)"
    )
    
    args = parser.parse_args()
    
    # Parse library paths if provided
    symbol_paths = None
    if args.symbol_paths:
        # Handle both colon and semicolon separators
        symbol_paths = _split_lib_paths(args.symbol_paths)
    
    converter = TraceConverter()

    def _emit_lint_error(lint_level: str, error_msg: str, detail: str = "",
                         line: int = 0, column: int = 0, snippet: str = ""):
        """Write a structured JSON lint error to stderr for the C++ caller to parse."""
        err = {"lint_level": lint_level, "error": error_msg, "detail": detail,
               "line": line, "column": column, "snippet": snippet}
        print(f"[LINT_ERROR] {json.dumps(err)}", file=sys.stderr)

    try:
        from trace_parser import TraceParseError
    except ImportError:
        from .trace_parser import TraceParseError

    try:
        # Determine conversion method
        if args.from_format == "trace_sch" and args.to_format == "trace_json":
            converter.trace_sch_file_to_trace_json_file(args.input_file, args.output_file)
        elif args.from_format == "trace_json" and args.to_format == "trace_sch":
            converter.trace_json_file_to_trace_sch_file(args.input_file, args.output_file)
        elif args.from_format == "kicad_sch" and args.to_format == "trace_json":
            converter.kicad_sch_file_to_trace_json_file(args.input_file, args.output_file)
        elif args.from_format == "trace_json" and args.to_format == "kicad_sch":
            converter.trace_json_file_to_kicad_sch_file(args.input_file, args.output_file, existing_sch_path=args.existing_sch_path, symbol_paths=symbol_paths)
        elif args.from_format == "kicad_sch" and args.to_format == "trace_sch":
            converter.kicad_sch_file_to_trace_sch_file(args.input_file, args.output_file)
        elif args.from_format == "trace_sch" and args.to_format == "kicad_sch":
            converter.trace_sch_file_to_kicad_sch_file(args.input_file, args.output_file, existing_sch_path=args.existing_sch_path, symbol_paths=symbol_paths)
        else:
            print(f"Error: Conversion from {args.from_format} to {args.to_format} is not supported.", file=sys.stderr)
            sys.exit(EXIT_TRACE_SCH_PARSE_ERROR)
        
        print(f"Successfully converted {args.input_file} ({args.from_format}) to {args.output_file} ({args.to_format})")

    except TraceParseError as e:
        _emit_lint_error("trace_sch", e.message, line=e.line, column=e.column)
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(EXIT_TRACE_SCH_PARSE_ERROR)
    except KicadSchValidationError as e:
        _emit_lint_error("kicad_sch", e.message, detail=e.detail,
                         line=e.error_line, snippet=e.snippet)
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(EXIT_KICAD_SCH_VALIDATION_ERROR)
    except NotImplementedError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(EXIT_TRACE_SCH_PARSE_ERROR)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(EXIT_TRACE_SCH_PARSE_ERROR)
