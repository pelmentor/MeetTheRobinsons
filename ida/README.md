# `ida/` — IDA Pro working directory

This folder holds the IDA Pro databases (`.i64`), the PE-sieve dumps used as IDA inputs, and any IDA Python / IDAPython scripts (`scripts/`).

## What's here

```
ida/
├── README.md                       (this file)
├── Launcher.exe.i64                (analyzed PE32; usable today)
├── 400000.Wilbur.exe.i64           (analyzed PE32 from PE-sieve dump; usable today)
├── dumps/
│   └── process_22276/              (PE-sieve output — see research/findings/unpack-state.md)
│       ├── 400000.Wilbur.exe       (45 MB unpacked image; what the .i64 above was built from)
│       ├── 400000.Wilbur.exe.imports.txt
│       ├── dump_report.json
│       └── scan_report.json
└── scripts/                        (IDA Python scripts; commit these)
```

## Status of `400000.Wilbur.exe.i64`

Built from the PE-sieve unpack of a running `Wilbur.exe`. 12,555 functions auto-recognized, Hex-Rays works, all the analysis from `research/findings/` lives in this database (renames, comments, struct layouts).

**Caveat:** the dump was taken at the main menu. Code paths that only run during in-game 3D scenes (notably DirectInput init) are still in their pre-trigger SecuROM lazy-decrypt state and do not appear as instructions in the image. See [`research/findings/unpack-state.md`](../research/findings/unpack-state.md) §"Known incompleteness in current dump" and [`research/findings/lessons-learned.md`](../research/findings/lessons-learned.md) §L2 for impact and recovery procedure (re-dump from in-game).

## Why .i64 files are gitignored

- They're **per-user license artifacts** — metadata is tied to the IDA install that produced them.
- They're **large** (`400000.Wilbur.exe.i64` is currently ~25–40 MB and grows with annotations).
- They're **regenerable** — anyone can rebuild from the source binary in `dumps/` plus the narrative notes in `research/findings/`. The narrative is the real work product; the .i64 is its IDE state.
- They evolve continuously — git diffs would be huge unreviewable binary blobs.

If you want to back up your databases, use IDA's `File → Save as…` to a separate location, or push to personal cloud — not this repo.

## Where the binaries themselves live

- The protected on-disk `Wilbur.exe` lives in `Game/` (gitignored — copyrighted binary).
- The PE-sieve unpacked dump lives in `ida/dumps/process_<pid>/400000.Wilbur.exe` (gitignored — derivative of a copyrighted binary).
- IDA stores absolute paths to the source binary in the `.i64`. If you move the dump or the live `Game/Wilbur.exe`, IDA prompts to re-link on open.

## Working with these databases

- Per-session setup of IDA + `ida-pro-mcp` + Claude Code: [`../docs/ida-workflow.md`](../docs/ida-workflow.md).
- Naming / comment conventions used in both databases: [`../docs/REVERSE_ENGINEERING.md`](../docs/REVERSE_ENGINEERING.md).
- Symbol cross-reference (original `sub_…` ↔ our names ↔ addresses): [`../research/findings/symbol-table.md`](../research/findings/symbol-table.md).

## scripts/

Commit any IDAPython / idalib scripts you write here. Examples we'll likely accumulate:

- `dump_string_xrefs.py` — list every xref to every string in a section, useful for finding code paths near interesting strings.
- `find_d3d_calls.py` — locate all `IDirect3D9::CreateDevice` and `IDirect3DDevice9::Reset` call sites.
- `apply_offsets.py` — bulk-apply VAs from `../research/offsets.md` to a freshly built database (lets you re-bootstrap an .i64 from a clean dump quickly).
