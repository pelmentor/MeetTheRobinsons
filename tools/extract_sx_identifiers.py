#!/usr/bin/env python3
"""Extract printable identifier-shaped tokens from .sx bytecode files.

Phase 0C (coop multiplayer plan) catalog tool: walks Game/data/scripts/*.sx
(SLNG bytecode), extracts every printable ASCII run that looks like a C-style
identifier, and aggregates per-file-count + per-occurrence-count.

The output drives the Replicate/Cosmetic/Forbidden classification that
gates Phase 5 of the coop plan.

Usage:
    python3 tools/extract_sx_identifiers.py                # default repo paths
    python3 tools/extract_sx_identifiers.py --out catalog.txt
"""

import argparse
import os
import re
import sys
from collections import Counter

DEFAULT_DIR = r'd:/Projects/Programming/MeetTheRobinsons/Game/data/scripts'

IDENT_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]{2,49}$')

# Paths and build-system artifacts to drop.
SKIP_SUBSTR = ['dev_Wilbur', 'GameCommon', 'Scripts', '.sla', '.sx']
# Token-level: drop tokens that contain a path separator (single backslash
# or forward slash) — defined here to avoid PowerShell heredoc escaping pain.
PATH_CHARS = ('\x5c', '/')


def looks_identifier(s: str) -> bool:
    if any(sub in s for sub in SKIP_SUBSTR):
        return False
    if any(ch in s for ch in PATH_CHARS):
        return False
    return bool(IDENT_RE.match(s))


def extract(path: str):
    with open(path, 'rb') as f:
        data = f.read()
    return [m.decode('ascii', errors='ignore')
            for m in re.findall(rb'[\x20-\x7e]{3,}', data)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=DEFAULT_DIR)
    ap.add_argument('--out', default=None,
                    help='write detail to this file; stdout always gets summary')
    ap.add_argument('--top', type=int, default=200,
                    help='print top N most frequent identifiers')
    args = ap.parse_args()

    per_file_idents: dict[str, set[str]] = {}
    file_freq = Counter()
    occ_freq = Counter()

    files = sorted(fn for fn in os.listdir(args.dir) if fn.endswith('.sx'))
    if not files:
        print(f'no .sx files in {args.dir}', file=sys.stderr)
        return 1

    for fn in files:
        idents_in_file = set()
        for token in extract(os.path.join(args.dir, fn)):
            if looks_identifier(token):
                idents_in_file.add(token)
                occ_freq[token] += 1
        per_file_idents[fn] = idents_in_file
        for ident in idents_in_file:
            file_freq[ident] += 1

    print(f'# .sx files scanned: {len(files)}')
    print(f'# distinct identifier-shaped tokens: {len(file_freq)}')
    print()
    print(f'# Top {args.top} by per-file frequency (n = number of .sx files containing it):')
    print(f'# {"n":>5}  {"occ":>6}  identifier')
    for ident, n in file_freq.most_common(args.top):
        print(f'  {n:>5}  {occ_freq[ident]:>6}  {ident}')

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as out:
            out.write(f'# .sx identifier catalog — {len(files)} files, '
                      f'{len(file_freq)} distinct tokens\n\n')
            for ident, n in file_freq.most_common():
                out.write(f'{n:>5}\t{occ_freq[ident]:>6}\t{ident}\n')
        print(f'\nwrote full catalog to {args.out}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
