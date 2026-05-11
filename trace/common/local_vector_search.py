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
Local Vector Search for KiCad Symbols and Footprints

Provides semantic search over the user's local symbol/footprint libraries using:
- all-MiniLM-L6-v2 via ONNX Runtime for embeddings (384 dims)
- SQLite for persistent vector storage
- Brute-force cosine similarity (fast enough for ~20K vectors)
- mtime-based freshness checks for incremental re-indexing

Takes a list of queries (batched in a single embedding + matrix multiply).

CLI usage:
    python local_vector_search.py \\
        --mode symbol \\
        --queries '["LDO", "capacitor", "LED"]' \\
        --lib-paths "/path/to/symbols" \\
        --top-k 5 \\
        --model-path "/path/to/all-MiniLM-L6-v2.onnx" \\
        --tokenizer-path "/path/to/tokenizer/" \\
        --db-path "~/.trace/vector_index.db"
"""

import argparse
import json
import logging
import math
import os
import re
import sqlite3
import struct
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s.%(msecs)03d [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S',
    stream=sys.stderr,
)
logger = logging.getLogger(__name__)

EMBEDDING_DIM = 384
DB_SCHEMA_VERSION = 1


# ---------------------------------------------------------------------------
# ONNX Embedder
# ---------------------------------------------------------------------------

class ONNXEmbedder:
    """Generates embeddings using all-MiniLM-L6-v2 via ONNX Runtime."""

    def __init__(self, model_path: str, tokenizer_path: str):
        import onnxruntime as ort
        from tokenizers import Tokenizer

        t0 = time.perf_counter()
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = min(os.cpu_count() or 1, 4)
        opts.inter_op_num_threads = 1
        self.session = ort.InferenceSession(
            model_path,
            sess_options=opts,
            providers=['CPUExecutionProvider'],
        )
        t1 = time.perf_counter()
        logger.debug(f"ONNX model loaded in {(t1-t0)*1000:.1f}ms  (threads={opts.intra_op_num_threads}, {model_path})")

        tok_file = os.path.join(tokenizer_path, 'tokenizer.json')
        self.tokenizer = Tokenizer.from_file(tok_file)
        self.tokenizer.enable_truncation(max_length=128)
        self.tokenizer.enable_padding(length=128)
        t2 = time.perf_counter()
        logger.debug(f"Tokenizer loaded in {(t2-t1)*1000:.1f}ms  ({tok_file})")

    def embed(self, texts: List[str]) -> np.ndarray:
        """Embed a batch of texts. Returns (N, 384) float32 array, L2-normalized."""
        if not texts:
            return np.empty((0, EMBEDDING_DIM), dtype=np.float32)

        t0 = time.perf_counter()
        encodings = self.tokenizer.encode_batch(texts)
        input_ids = np.array([e.ids for e in encodings], dtype=np.int64)
        attention_mask = np.array([e.attention_mask for e in encodings], dtype=np.int64)
        token_type_ids = np.zeros_like(input_ids)
        t1 = time.perf_counter()
        logger.debug(f"Tokenized {len(texts)} texts in {(t1-t0)*1000:.1f}ms")

        outputs = self.session.run(
            None,
            {
                'input_ids': input_ids,
                'attention_mask': attention_mask,
                'token_type_ids': token_type_ids,
            },
        )
        t2 = time.perf_counter()
        logger.debug(f"ONNX inference for {len(texts)} texts in {(t2-t1)*1000:.1f}ms")

        # Mean pooling over token embeddings, masked by attention
        token_embeddings = outputs[0]  # (batch, seq_len, hidden)
        mask_expanded = attention_mask[:, :, np.newaxis].astype(np.float32)
        summed = (token_embeddings * mask_expanded).sum(axis=1)
        counts = mask_expanded.sum(axis=1).clip(min=1e-9)
        embeddings = summed / counts

        # L2 normalize
        norms = np.linalg.norm(embeddings, axis=1, keepdims=True).clip(min=1e-9)
        embeddings = embeddings / norms
        t3 = time.perf_counter()
        logger.debug(f"Pooling + normalize in {(t3-t2)*1000:.1f}ms  -> ({embeddings.shape[0]}, {embeddings.shape[1]})")
        return embeddings.astype(np.float32)


# ---------------------------------------------------------------------------
# SQLite Vector Store
# ---------------------------------------------------------------------------

def _init_db(conn: sqlite3.Connection):
    t0 = time.perf_counter()
    conn.execute('PRAGMA journal_mode=WAL')
    conn.execute('PRAGMA synchronous=NORMAL')
    conn.execute('''
        CREATE TABLE IF NOT EXISTS components (
            id TEXT PRIMARY KEY,
            lib_type TEXT NOT NULL,
            library TEXT NOT NULL,
            name TEXT NOT NULL,
            metadata_json TEXT NOT NULL,
            embedding BLOB NOT NULL,
            source_file TEXT NOT NULL,
            file_mtime REAL NOT NULL,
            indexed_at REAL NOT NULL
        )
    ''')
    conn.execute('CREATE INDEX IF NOT EXISTS idx_lib_type ON components(lib_type)')
    conn.execute('CREATE INDEX IF NOT EXISTS idx_source_file ON components(source_file)')
    conn.execute('''
        CREATE TABLE IF NOT EXISTS file_index (
            source_file TEXT PRIMARY KEY,
            lib_type TEXT NOT NULL,
            file_mtime REAL NOT NULL,
            component_count INTEGER NOT NULL DEFAULT 0
        )
    ''')
    conn.execute('''
        CREATE TABLE IF NOT EXISTS meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    ''')
    conn.execute(
        'INSERT OR IGNORE INTO meta (key, value) VALUES (?, ?)',
        ('schema_version', str(DB_SCHEMA_VERSION)),
    )
    conn.commit()
    logger.debug(f"DB schema initialized in {(time.perf_counter()-t0)*1000:.1f}ms")


def _pack_embedding(vec: np.ndarray) -> bytes:
    return vec.astype(np.float32).tobytes()


def _unpack_embedding(blob: bytes) -> np.ndarray:
    return np.frombuffer(blob, dtype=np.float32).copy()


# ---------------------------------------------------------------------------
# Symbol Parser (adapted from Pinecone backend)
# ---------------------------------------------------------------------------

def _count_parens(line: str) -> int:
    """Count paren balance, ignoring parens inside quoted strings."""
    clean = re.sub(r'"[^"]*"', '""', line)
    return clean.count('(') - clean.count(')')


def _extract_symbol_block(lines: List[str], start_idx: int) -> Tuple[str, int]:
    depth = 0
    end_line = start_idx
    for j in range(start_idx, len(lines)):
        depth += _count_parens(lines[j])
        if depth == 0 and j > start_idx:
            end_line = j
            break
    return '\n'.join(lines[start_idx:end_line + 1]), end_line


def _build_symbol_lookup(content: str) -> Dict[str, str]:
    symbol_lookup = {}
    lines = content.split('\n')
    i = 0
    while i < len(lines):
        if re.match(r'^\t\(symbol "([^"]+)"', lines[i]):
            match = re.search(r'\(symbol "([^"]+)"', lines[i])
            name = match.group(1)
            symbol_content, end_line = _extract_symbol_block(lines, i)
            symbol_lookup[name] = symbol_content
            i = end_line + 1
        else:
            i += 1
    return symbol_lookup


def _extract_pins(symbol_content: str) -> List[Dict]:
    pins = []
    for m in re.finditer(
        r'\(pin (\w+) (\w+)\s+\(at [0-9.-]+ [0-9.-]+ \d+\)\s+\(length [0-9.-]+\).*?\(name "([^"]*)".*?\(number "([^"]*)"',
        symbol_content, re.DOTALL,
    ):
        pins.append({'type': m.group(1), 'name': m.group(3), 'number': m.group(4)})
    return pins


def _resolve_symbol_pins(name: str, content: str, lookup: Dict[str, str], visited=None) -> List[Dict]:
    if visited is None:
        visited = set()
    if name in visited:
        return []
    visited.add(name)
    extends = re.search(r'\(extends "([^"]+)"\)', content)
    if extends:
        parent = extends.group(1)
        if parent in lookup:
            return _resolve_symbol_pins(parent, lookup[parent], lookup, visited)
        return []
    return _extract_pins(content)


def parse_symbols(content: str, lib_name: str) -> List[Dict]:
    """Parse a .kicad_sym file into a list of component metadata dicts."""
    symbols = []
    lib = lib_name.replace('.kicad_sym', '')
    lines = content.split('\n')
    lookup = _build_symbol_lookup(content)

    i = 0
    while i < len(lines):
        if re.match(r'^\t\(symbol "([^"]+)"', lines[i]):
            match = re.search(r'\(symbol "([^"]+)"', lines[i])
            name = match.group(1)

            # Skip internal unit symbols like "SymbolName_1_1"
            if re.match(r'.+_\d+_\d+$', name):
                i += 1
                continue

            sym_content, end_line = _extract_symbol_block(lines, i)

            props = {}
            for pm in re.finditer(r'\(property "([^"]+)" "([^"]*)"', sym_content):
                props[pm.group(1)] = pm.group(2)

            pins = _resolve_symbol_pins(name, sym_content, lookup)
            description = props.get('Description', '') or f"{name} component from {lib} library"

            symbols.append({
                'name': name,
                'library': lib,
                'description': description,
                'reference': props.get('Reference', ''),
                'value': props.get('Value', ''),
                'keywords': props.get('ki_keywords', ''),
                'footprint': props.get('Footprint', ''),
                'datasheet': props.get('Datasheet', ''),
                'pin_count': len(pins),
                'pins': pins,
            })
            i = end_line + 1
        else:
            i += 1
    return symbols


# ---------------------------------------------------------------------------
# Footprint Parser (adapted from Pinecone backend)
# ---------------------------------------------------------------------------

def parse_footprint(content: str, filename: str, library: str) -> Dict:
    """Parse a single .kicad_mod file into a metadata dict."""
    name = filename.replace('.kicad_mod', '')

    descr_match = re.search(r'\(descr "([^"]*)"', content)
    prop_desc_match = re.search(r'\(property "Description" "([^"]*)"', content)
    description = ' '.join(filter(None, [
        descr_match.group(1) if descr_match else '',
        prop_desc_match.group(1) if prop_desc_match else '',
    ])).strip() or f"{name} footprint from {library} library"

    tags_match = re.search(r'\(tags "([^"]*)"', content)
    tags = tags_match.group(1) if tags_match else ''

    mounting_type = ''
    if re.search(r'\(attr smd\)', content):
        mounting_type = 'smd'
    elif re.search(r'\(attr through_hole\)', content):
        mounting_type = 'through_hole'

    reference = ''
    ref_match = re.search(r'\(property "Reference" "([^"]*)"', content)
    if ref_match:
        reference = re.sub(r'\*+$', '', ref_match.group(1))

    datasheet_match = re.search(r'\(property "Datasheet" "([^"]*)"', content)
    datasheet = datasheet_match.group(1) if datasheet_match else ''

    pad_count = len(re.findall(
        r'\(pad "[^"]+" (?:smd|thru_hole)',
        content,
    ))

    pad_types = set()
    for pm in re.finditer(r'\(pad "[^"]+" (smd|thru_hole)', content):
        pad_types.add(pm.group(1))

    return {
        'name': name,
        'library': library,
        'description': description,
        'tags': tags,
        'mounting_type': mounting_type,
        'reference': reference,
        'datasheet': datasheet,
        'pad_count': pad_count,
        'pad_types': ','.join(sorted(pad_types)),
    }


# ---------------------------------------------------------------------------
# Embedding text builders
# ---------------------------------------------------------------------------

def _symbol_embed_text(sym: Dict) -> str:
    return f"{sym['name']} {sym['library']} {sym['description']} {sym.get('keywords', '')}"


def _footprint_embed_text(fp: Dict) -> str:
    return f"{fp['name']} {fp['library']} {fp['description']} {fp.get('tags', '')} {fp.get('mounting_type', '')}"


# ---------------------------------------------------------------------------
# LocalVectorIndex
# ---------------------------------------------------------------------------

class LocalVectorIndex:
    """SQLite-backed local vector index with mtime-based freshness."""

    BATCH_SIZE = 256

    def __init__(self, db_path: str, embedder: ONNXEmbedder):
        t0 = time.perf_counter()
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        self.conn = sqlite3.connect(db_path, timeout=1)
        _init_db(self.conn)
        self.embedder = embedder
        logger.debug(f"LocalVectorIndex opened in {(time.perf_counter()-t0)*1000:.1f}ms  (db={db_path})")

    def close(self):
        self.conn.close()
        logger.debug("DB connection closed")

    # -- freshness --------------------------------------------------------

    def check_freshness(self, lib_dirs: List[str], lib_type: str) -> List[str]:
        """Fast freshness check using the file_index table.

        Returns the list of file paths that need re-indexing (new or modified).
        Also removes stale entries for deleted files.
        """
        t_start = time.perf_counter()
        logger.debug(f"check_freshness: lib_type={lib_type}, dirs={lib_dirs}")

        t0 = time.perf_counter()
        if lib_type == 'symbol':
            disk_files = self._scan_symbol_dirs(lib_dirs)
        else:
            disk_files = self._scan_footprint_dirs(lib_dirs)
        logger.debug(f"  Disk scan found {len(disk_files)} files in {(time.perf_counter()-t0)*1000:.1f}ms")

        t0 = time.perf_counter()
        db_files: Dict[str, float] = {}
        for row in self.conn.execute(
            'SELECT source_file, file_mtime FROM file_index WHERE lib_type=?',
            (lib_type,),
        ):
            db_files[row[0]] = row[1]
        logger.debug(f"  file_index has {len(db_files)} entries (queried in {(time.perf_counter()-t0)*1000:.1f}ms)")

        stale_files = []
        for fpath, mtime in disk_files.items():
            if fpath not in db_files or mtime > db_files[fpath]:
                stale_files.append(fpath)

        deleted = set(db_files.keys()) - set(disk_files.keys())
        if deleted:
            logger.debug(f"  Removing {len(deleted)} deleted file entries")
            self.conn.executemany(
                'DELETE FROM components WHERE source_file=? AND lib_type=?',
                [(sf, lib_type) for sf in deleted],
            )
            self.conn.executemany(
                'DELETE FROM file_index WHERE source_file=? AND lib_type=?',
                [(sf, lib_type) for sf in deleted],
            )
            self.conn.commit()

        if stale_files:
            logger.debug(f"  {len(stale_files)} files need re-indexing")
        else:
            logger.debug("  Index is up-to-date, nothing to re-index")

        logger.debug(f"  check_freshness total: {(time.perf_counter()-t_start)*1000:.1f}ms")
        return stale_files

    def reindex_files(self, file_paths: List[str], lib_type: str):
        """Re-index a list of specific files (the heavy/slow operation)."""
        t_start = time.perf_counter()
        logger.info(f"reindex_files: {len(file_paths)} {lib_type} files")

        for i, fpath in enumerate(file_paths):
            t_file = time.perf_counter()
            mtime = os.path.getmtime(fpath) if os.path.exists(fpath) else 0
            self._reindex_file(fpath, lib_type, mtime)
            logger.debug(f"  [{i+1}/{len(file_paths)}] {os.path.basename(fpath)} "
                         f"in {(time.perf_counter()-t_file)*1000:.1f}ms")

        logger.debug(f"reindex_files total: {(time.perf_counter()-t_start)*1000:.1f}ms")

    def _scan_symbol_dirs(self, lib_dirs: List[str]) -> Dict[str, float]:
        result = {}
        for d in lib_dirs:
            if not os.path.isdir(d):
                continue
            for fname in os.listdir(d):
                if fname.endswith('.kicad_sym'):
                    fpath = os.path.join(d, fname)
                    result[fpath] = os.path.getmtime(fpath)
        return result

    def _scan_footprint_dirs(self, lib_dirs: List[str]) -> Dict[str, float]:
        result = {}
        for d in lib_dirs:
            if not os.path.isdir(d):
                continue
            for pretty_dir in os.listdir(d):
                if not pretty_dir.endswith('.pretty'):
                    continue
                pretty_path = os.path.join(d, pretty_dir)
                if not os.path.isdir(pretty_path):
                    continue
                for fname in os.listdir(pretty_path):
                    if fname.endswith('.kicad_mod'):
                        fpath = os.path.join(pretty_path, fname)
                        result[fpath] = os.path.getmtime(fpath)
        return result

    def _reindex_file(self, fpath: str, lib_type: str, mtime: float):
        """Parse, embed, and upsert all components from a single file."""
        try:
            t0 = time.perf_counter()
            with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read()
            logger.debug(f"      Read {len(content)} bytes in {(time.perf_counter()-t0)*1000:.1f}ms")
        except OSError as e:
            logger.warning(f"Cannot read {fpath}: {e}")
            return

        # Delete old entries for this file
        self.conn.execute(
            'DELETE FROM components WHERE source_file=? AND lib_type=?',
            (fpath, lib_type),
        )

        now = time.time()

        t0 = time.perf_counter()
        if lib_type == 'symbol':
            lib_name = os.path.basename(fpath)
            components = parse_symbols(content, lib_name)
            texts = [_symbol_embed_text(c) for c in components]
        else:
            library = os.path.basename(os.path.dirname(fpath)).replace('.pretty', '')
            comp = parse_footprint(content, os.path.basename(fpath), library)
            components = [comp]
            texts = [_footprint_embed_text(comp)]
        logger.debug(f"      Parsed {len(components)} components in {(time.perf_counter()-t0)*1000:.1f}ms")

        if not components:
            self.conn.commit()
            return

        # Batch embed
        total_embed_ms = 0.0
        for batch_start in range(0, len(components), self.BATCH_SIZE):
            batch_comps = components[batch_start:batch_start + self.BATCH_SIZE]
            batch_texts = texts[batch_start:batch_start + self.BATCH_SIZE]
            t0 = time.perf_counter()
            embeddings = self.embedder.embed(batch_texts)
            total_embed_ms += (time.perf_counter() - t0) * 1000

            rows = []
            for comp, emb in zip(batch_comps, embeddings):
                norm = np.linalg.norm(emb)
                if norm < 1e-10:
                    logger.debug(f"      Skipping zero-vector embedding for {comp.get('name', '?')}")
                    continue
                comp_id = f"{comp['library']}:{comp['name']}"
                meta = {k: v for k, v in comp.items()}
                rows.append((
                    comp_id,
                    lib_type,
                    comp['library'],
                    comp['name'],
                    json.dumps(meta),
                    _pack_embedding(emb),
                    fpath,
                    mtime,
                    now,
                ))

            self.conn.executemany(
                '''INSERT OR REPLACE INTO components
                   (id, lib_type, library, name, metadata_json, embedding,
                    source_file, file_mtime, indexed_at)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)''',
                rows,
            )
        self.conn.commit()
        logger.debug(f"      Embedded + upserted {len(components)} components (embed={total_embed_ms:.1f}ms)")

        # Update file_index tracking table
        self.conn.execute(
            'INSERT OR REPLACE INTO file_index (source_file, lib_type, file_mtime, component_count) VALUES (?, ?, ?, ?)',
            (fpath, lib_type, mtime, len(components)),
        )
        self.conn.commit()

    # -- search -----------------------------------------------------------

    def search(self, queries: List[str], lib_type: str, top_k: int = 5) -> Dict[str, List[Dict]]:
        """Brute-force cosine similarity search for a list of queries.

        Embeds all queries in a single batch and computes scores against the
        shared embedding matrix once, so N queries cost barely more than 1.
        Returns a dict mapping each query to its top-k results.
        """
        t_start = time.perf_counter()
        logger.debug(f"search: {len(queries)} queries, lib_type={lib_type}, top_k={top_k}")

        t0 = time.perf_counter()
        rows = self.conn.execute(
            'SELECT id, metadata_json, embedding FROM components WHERE lib_type=?',
            (lib_type,),
        ).fetchall()
        logger.debug(f"  Fetched {len(rows)} rows from DB in {(time.perf_counter()-t0)*1000:.1f}ms")

        if not rows:
            logger.debug("  No indexed components, returning empty results")
            return {q: [] for q in queries}

        t0 = time.perf_counter()
        metas = [r[1] for r in rows]
        mat = np.stack([_unpack_embedding(r[2]) for r in rows])  # (N, 384)
        logger.debug(f"  Unpacked {mat.shape[0]} embeddings in {(time.perf_counter()-t0)*1000:.1f}ms  -> matrix {mat.shape}")

        # Filter out zero-vector embeddings to prevent matmul divide-by-zero
        norms = np.linalg.norm(mat, axis=1)
        valid_mask = norms > 1e-10
        if not np.all(valid_mask):
            n_invalid = int(np.sum(~valid_mask))
            logger.debug(f"  Filtering out {n_invalid} zero-vector embeddings")
            mat = mat[valid_mask]
            metas = [m for m, v in zip(metas, valid_mask) if v]

        if mat.shape[0] == 0:
            logger.debug("  All embeddings were zero-vectors, returning empty results")
            return {q: [] for q in queries}

        t0 = time.perf_counter()
        query_vecs = self.embedder.embed(queries)  # (Q, 384)
        logger.debug(f"  Query embeddings computed in {(time.perf_counter()-t0)*1000:.1f}ms")

        t0 = time.perf_counter()
        all_scores = mat @ query_vecs.T  # (N, Q)
        logger.debug(f"  Cosine similarity matrix ({mat.shape[0]}x{len(queries)}) in {(time.perf_counter()-t0)*1000:.1f}ms")

        t0 = time.perf_counter()
        results_map: Dict[str, List[Dict]] = {}
        for qi, query in enumerate(queries):
            scores = all_scores[:, qi]
            top_indices = np.argsort(scores)[::-1][:top_k]
            results = []
            for idx in top_indices:
                meta = json.loads(metas[idx])
                meta['score'] = round(float(scores[idx]), 4)
                results.append(meta)
            results_map[query] = results
            logger.debug(f"  Query '{query}': top score={results[0]['score'] if results else 'N/A'}")
        logger.debug(f"  Ranking + JSON parse in {(time.perf_counter()-t0)*1000:.1f}ms")

        logger.debug(f"  search total: {(time.perf_counter()-t_start)*1000:.1f}ms")
        return results_map

    def count(self, lib_type: str) -> int:
        row = self.conn.execute(
            'SELECT COUNT(*) FROM components WHERE lib_type=?', (lib_type,)
        ).fetchone()
        return row[0] if row else 0


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def _add_common_args(parser: argparse.ArgumentParser):
    parser.add_argument('--mode', required=True, choices=['symbol', 'footprint'])
    parser.add_argument('--lib-paths', required=True, help='Colon-separated library dirs')
    parser.add_argument('--model-path', required=True, help='Path to ONNX model file')
    parser.add_argument('--tokenizer-path', required=True, help='Path to tokenizer directory')
    parser.add_argument('--db-path', required=True, help='Path to SQLite database file')


def main():
    t_wall = time.perf_counter()

    parser = argparse.ArgumentParser(description='Local vector search for KiCad libraries')
    sub = parser.add_subparsers(dest='command')

    # --- search command (default behavior when --queries is present) ---
    search_parser = sub.add_parser('search', help='Search the index')
    _add_common_args(search_parser)
    search_parser.add_argument('--queries', required=True, help='JSON array of search queries')
    search_parser.add_argument('--top-k', type=int, default=5)

    # --- reindex-only command ---
    reindex_parser = sub.add_parser('reindex', help='Re-index specific files (background)')
    _add_common_args(reindex_parser)
    reindex_parser.add_argument('--files', required=True, help='JSON array of file paths to re-index')

    # --- build-index command ---
    build_parser = sub.add_parser('build-index', help='Build a full index from scratch (build-time)')
    _add_common_args(build_parser)

    # --- fixup-paths command (remap build-time paths to installed paths) ---
    fixup_parser = sub.add_parser('fixup-paths', help='Remap source_file paths after DB copy')
    fixup_parser.add_argument('--db-path', required=True, help='Path to SQLite database file')
    fixup_parser.add_argument('--symbol-paths', help='Colon-separated symbol library dirs')
    fixup_parser.add_argument('--footprint-paths', help='Colon-separated footprint library dirs')

    # Support legacy flat args (no subcommand) for backward compat
    parser.add_argument('--mode', choices=['symbol', 'footprint'])
    parser.add_argument('--queries', help='JSON array of search queries')
    parser.add_argument('--lib-paths', help='Colon-separated library dirs')
    parser.add_argument('--top-k', type=int, default=5)
    parser.add_argument('--model-path', help='Path to ONNX model file')
    parser.add_argument('--tokenizer-path', help='Path to tokenizer directory')
    parser.add_argument('--db-path', help='Path to SQLite database file')
    parser.add_argument('--files', help='JSON array of file paths (for reindex)')
    parser.add_argument('--build-index', action='store_true', help='Build full index')
    parser.add_argument('--reindex-only', action='store_true', help='Reindex specific files')

    args = parser.parse_args()

    # Resolve the command from subcommand or legacy flags
    command = args.command
    if not command:
        if getattr(args, 'build_index', False):
            command = 'build-index'
        elif getattr(args, 'reindex_only', False):
            command = 'reindex'
        elif getattr(args, 'queries', None):
            command = 'search'
        else:
            parser.error('Must specify a subcommand (search, reindex, build-index, fixup-paths) or --queries')

    # --- fixup-paths: remap build-time paths, then exit early ---
    if command == 'fixup-paths':
        db_path = os.path.expanduser(args.db_path)
        logger.debug("=" * 60)
        logger.debug(f"local_vector_search  command=fixup-paths")
        logger.debug(f"  db: {db_path}")
        logger.debug("=" * 60)

        conn = sqlite3.connect(db_path, timeout=10)
        remapped = 0

        for lib_type, paths_arg, scan_fn_key in [
            ('symbol', getattr(args, 'symbol_paths', None), 'symbol'),
            ('footprint', getattr(args, 'footprint_paths', None), 'footprint'),
        ]:
            if not paths_arg:
                continue
            new_dirs = [p for p in paths_arg.split(':') if p]
            if not new_dirs:
                continue

            if lib_type == 'symbol':
                disk_map: Dict[str, str] = {}
                for d in new_dirs:
                    if not os.path.isdir(d):
                        continue
                    for fname in os.listdir(d):
                        if fname.endswith('.kicad_sym'):
                            disk_map[fname] = os.path.join(d, fname)
            else:
                disk_map = {}
                for d in new_dirs:
                    if not os.path.isdir(d):
                        continue
                    for pretty_dir in os.listdir(d):
                        if not pretty_dir.endswith('.pretty'):
                            continue
                        pretty_path = os.path.join(d, pretty_dir)
                        if not os.path.isdir(pretty_path):
                            continue
                        for fname in os.listdir(pretty_path):
                            if fname.endswith('.kicad_mod'):
                                rel_key = os.path.join(pretty_dir, fname)
                                disk_map[rel_key] = os.path.join(pretty_path, fname)

            old_entries = conn.execute(
                'SELECT source_file FROM file_index WHERE lib_type=?',
                (lib_type,),
            ).fetchall()
            logger.debug(f"  {lib_type}: {len(old_entries)} DB entries, {len(disk_map)} disk files")

            for (old_path,) in old_entries:
                if lib_type == 'symbol':
                    key = os.path.basename(old_path)
                else:
                    key = os.path.join(
                        os.path.basename(os.path.dirname(old_path)),
                        os.path.basename(old_path),
                    )

                new_path = disk_map.get(key)
                if not new_path or new_path == old_path:
                    continue

                new_mtime = os.path.getmtime(new_path)
                conn.execute(
                    'UPDATE file_index SET source_file=?, file_mtime=? WHERE source_file=? AND lib_type=?',
                    (new_path, new_mtime, old_path, lib_type),
                )
                conn.execute(
                    'UPDATE components SET source_file=? WHERE source_file=? AND lib_type=?',
                    (new_path, old_path, lib_type),
                )
                remapped += 1

        conn.commit()
        conn.close()

        logger.debug(f"  Remapped {remapped} paths")
        logger.debug(f"TOTAL wall time: {(time.perf_counter()-t_wall)*1000:.1f}ms")
        print(json.dumps({'remapped': remapped}))
        return

    db_path = os.path.expanduser(args.db_path)
    lib_dirs = [p for p in args.lib_paths.split(':') if p]

    logger.debug("=" * 60)
    logger.debug(f"local_vector_search  command={command}  mode={args.mode}")
    logger.debug(f"  lib-paths: {lib_dirs}")
    logger.debug(f"  db:        {db_path}")
    logger.debug("=" * 60)

    try:
        t0 = time.perf_counter()
        embedder = ONNXEmbedder(args.model_path, args.tokenizer_path)
        logger.debug(f"Embedder ready in {(time.perf_counter()-t0)*1000:.1f}ms")

        t0 = time.perf_counter()
        index = LocalVectorIndex(db_path, embedder)
        logger.debug(f"Index ready in {(time.perf_counter()-t0)*1000:.1f}ms")

        if command == 'search':
            queries = json.loads(args.queries)
            logger.debug(f"--- Freshness check ---")
            t0 = time.perf_counter()
            stale_files = index.check_freshness(lib_dirs, args.mode)
            logger.debug(f"Freshness done in {(time.perf_counter()-t0)*1000:.1f}ms")

            logger.debug(f"--- Search ---")
            t0 = time.perf_counter()
            results_map = index.search(queries, args.mode, args.top_k)
            logger.debug(f"Search done in {(time.perf_counter()-t0)*1000:.1f}ms")

            total = index.count(args.mode)
            index.close()

            output = {
                'results': results_map,
                'index_status': 'fresh' if not stale_files else 'stale',
                'stale_files': stale_files,
                'total_indexed': total,
            }
            print(json.dumps(output))

        elif command == 'reindex':
            file_paths = json.loads(args.files)
            logger.debug(f"--- Reindex {len(file_paths)} files ---")
            index.reindex_files(file_paths, args.mode)
            total = index.count(args.mode)
            index.close()

            output = {
                'reindexed': len(file_paths),
                'total_indexed': total,
            }
            print(json.dumps(output))

        elif command == 'build-index':
            logger.debug(f"--- Build full index ---")
            if args.mode == 'symbol':
                disk_files = index._scan_symbol_dirs(lib_dirs)
            else:
                disk_files = index._scan_footprint_dirs(lib_dirs)
            logger.debug(f"Found {len(disk_files)} files to index")

            index.reindex_files(list(disk_files.keys()), args.mode)
            total = index.count(args.mode)
            index.close()

            output = {
                'indexed': total,
                'files': len(disk_files),
            }
            print(json.dumps(output))

        logger.debug("=" * 60)
        logger.debug(f"TOTAL wall time: {(time.perf_counter()-t_wall)*1000:.1f}ms")
        logger.debug("=" * 60)

    except Exception as e:
        logger.error(f"FATAL: {e}", exc_info=True)
        error_output = {
            'error': str(e),
            'results': {},
            'index_status': 'error',
            'total_indexed': 0,
        }
        print(json.dumps(error_output), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
