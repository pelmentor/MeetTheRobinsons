# Offsets — Wilbur.exe (post-dump)

Single source of truth for known function VAs / data RVAs in the dumped
Wilbur.exe. The ASI mod's `include/mtr/offsets.h` is generated from this file.

> All addresses are virtual addresses (VA) at the default ImageBase of `0x00400000`.
> Subtract `0x00400000` for RVAs.

> **M1 is done.** Wilbur.exe was unpacked via PE-sieve `/imp 3` against a running process.
> Dump at `ida/dumps/process_22276/400000.Wilbur.exe`; analyzed IDA database at `ida/400000.Wilbur.exe.i64` (12,555 functions, Hex-Rays works).
> See [../research/findings/unpack-state.md](../research/findings/unpack-state.md) for the procedure and the known incompleteness (menu-time dump, in-game-only code paths missing).
>
> Rich symbol cross-reference (this is now the authoritative table): [../research/findings/symbol-table.md](../research/findings/symbol-table.md). Per-feature writeups in `../research/findings/`. This `offsets.md` is preserved as a brief landing page; canonical addresses live in `symbol-table.md`.

---

## Build identification

| Field | Value |
|---|---|
| `Wilbur.exe` SHA256 (protected) | `269C8FEFD5B8A571B3F24EA068F1432F0D3A80FEBA597024BA18043C9F6C0A50` |
| `Wilbur_dumped_SCY.exe` SHA256 | _(to be filled in once dumped)_ |
| Original OEP                     | _(to be filled in)_ |
| ImageBase                        | `0x00400000` |

---

## Functions

| VA | Name | What it does | Notes |
|---|---|---|---|
| _(empty)_ | | | |

## Globals

| VA | Type | Name | What it is |
|---|---|---|---|
| _(empty)_ | | | |

## Constants in `.rdata`

| VA | Bytes | Decoded | Source |
|---|---|---|---|
| _(empty — but [WSGF widescreen hack](../docs/prior-art/wsgf-widescreen-hack.md) tells us `AB AA AA 3F` (= `1.333333f`) lives somewhere in `.rdata`)_ | | | |

---

## Workflow

When you discover a new function/global worth tracking:

1. Annotate it inside IDA (rename + comment).
2. Add a row here with the VA, the name, and a one-line description.
3. If it's used by the ASI mod: also add it to
   `src/mtr-asi/include/mtr/offsets.h` (single header so changes propagate).
