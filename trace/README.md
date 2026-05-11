# Trace Proprietary Modules

> **All Rights Reserved.** This directory is **not** covered by GPLv3.

The contents of this directory (`trace/` and all its subdirectories) are the
proprietary and confidential property of the Trace Developers Team. They are
included in this repository for reference only as part of the larger Trace
source tree.

## What's in here

Trace's proprietary AI-native design tooling:

- `common/` — shared Python helpers used by the AI pipeline
- `eeschema/` — schematic-side converters and bridges
- `pcbnew/` — PCB-side converters (e.g. `trace_json_to_sexp.py`)

These modules are what turn the AI's intermediate `.trace_sch` / `.trace_pcb`
format into KiCad-native `.kicad_sch` / `.kicad_pcb` s-expression files.

## License

See [`LICENSE`](LICENSE) in this directory for the full proprietary license
text.

You may **NOT**:

- copy, modify, merge, publish, distribute, sublicense, or sell this code,
- use it for any commercial or non-commercial purpose without explicit written
  permission,
- reverse engineer, decompile, or disassemble it, or
- create derivative works based on it.

For licensing inquiries, contact: **hello@buildwithtrace.com**

## Relationship to the rest of the repository

The rest of the Trace repository (everything outside `trace/`) is licensed
under GPLv3 or other open-source licenses as described in the root
[`LICENSE`](../LICENSE) and [`LICENSE.README`](../LICENSE.README) files.

Only the contents of this `trace/` directory are proprietary.
