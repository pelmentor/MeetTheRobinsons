# Cvar / property-registration system (2026-05-05)

End-to-end RE of the engine's typed cvar registration. Confirmed by hooking
the eight typed-X registration variants and dumping every (group, name,
address, caller) tuple at startup → `Game/mtr_cvars.txt` (~13.3k entries).

## Registration API (eight typed-X variants)

```
console_register_var_typed_a (0x5894D0)
console_register_var_typed_b (0x5895A0)
console_register_var_typed_c (0x589670)
console_register_var_typed_d (0x589740)
console_register_var_typed_e (0x589816)
console_register_var_typed_f (0x5898D0)
console_register_var_typed_g (0x589990)
console_register_var_typed_h (0x589A50)
```

All eight share a uniform prefix: `__thiscall(this, group, name, addr, ...)`.
Stack-arg counts differ between variants:

| typed | stack args | semantic |
|-------|-----------:|----------|
| a, b  | 8          | bool / variant of int |
| c     | 9          | float (most common) |
| d     | 10         | angle / float-with-range |
| e, h  | 6          | byte / byte-with-range |
| f, g  | 7          | int / int-with-range |

**Stack-cleanup hazard:** every variant is `__thiscall` (callee-cleanup via
`ret N`). Mtr-asi's cvar dumper learned this the hard way — initial unified
hook with one signature corrupted the caller frame on every registration
that didn't match its arg count, causing a delayed crash. Each hook MUST
declare the exact arg count of the variant it patches. See
`src/mtr-asi/src/cvar_dump.cpp` for the per-variant hook signatures.

## Wrappers

`sub_589CE0`, `sub_589D10`, `sub_589810`, `sub_589E20` are convenience
wrappers that route to one of the typed-X functions based on type info.
Hooking only the typed-X leaves catches every registration regardless of
which wrapper called it.

## Storage encoding (CRITICAL)

The `setter_fn` and `notifier_fn` callbacks passed to `console_register_var_typed_c`
(and friends) are NOT identity functions — many of them transform the value.

### Common "real" cvars (most distance/scalar fields)

```
sub_429D20(x) = x * x         // setter — squares the input on write
sub_401E50(x) = sqrt(x)       // getter — sqrt's the storage on read
```

**Implication:** every `"real"` cvar registered with these callbacks stores
its value as `x²` internally. The engine reads the squared value directly,
which is fast for distance comparisons (no sqrt per object). User-facing
console writes go through `sub_429D20` to square; pre-init writes from C++
code typically write the squared value directly.

This affects MOST cvars in the dump tagged with `c` type — including
`PeripheryRejectDist`, `MediumDist`, `HighDist`, View.* distances, etc.

### `MeshLOD.PeripheryRejectAngle` (special case)

```
sub_67FE70(deg) = cos²(deg * π/180)    // setter
sub_67FE90(stored) = acos(sqrt(stored)) * 180/π   // getter (via sub_456890)
```

Storage = `cos²(half_cone_deg)`. Default `0.39` ≈ `cos²(51°)`.

To **disable periphery angle rejection** write `0.0` (= cos²(90°) = full
hemisphere). My initial attempt wrote `3.14` (thinking radians for 180°),
which the engine reads as cos²-of-something — `dot² >= 3.14` is never true
→ everything rejected (the OPPOSITE of disabling).

## Discovered global cvars (sample)

From the dump — entries with literal-string groups (engine globals, vs
heap-pointer groups for per-entity cvars):

### `scene` group (block base 0x745240)

| name                       | address     | notes |
|---------------------------:|-------------|-------|
| forceClear                 | 0x00745250  | byte |
| clipNear                   | 0x0074525C  | float (squared) |
| clipFar                    | 0x00745260  | float (squared) |
| fadeSpan                   | 0x00745264  | float |
| defaultShutdownDistance    | 0x00745268  | float (squared) |
| deathPlaneY                | 0x0074526C  | float |
| fallToDeathDistance        | 0x00745274  | float (squared) |
| **fogEnabled**             | 0x00745279  | byte (we write 0 here) |
| fogDensity                 | 0x00745284  | float |
| fogNear                    | 0x0074527C  | float |
| fogFar                     | 0x00745280  | float |
| baseFOV                    | 0x00745298  | float |

### `MeshLOD` group (struct base 0x745B38, registered by sub_67FED0)

| name                  | offset | address    | default | notes |
|----------------------:|:------:|-----------:|---------|-------|
| FocusDist             | +0x0C  | 0x00745B44 | 100.0   | dist² candidate |
| HighDist              | +0x10  | 0x00745B48 | 250.0   | dist² candidate |
| MediumDist            | +0x14  | 0x00745B4C | 500.0   | dist² candidate |
| PeripheryAcceptDist   | +0x18  | 0x00745B50 | 100.0   | dist² candidate |
| PeripheryRejectDist   | +0x1C  | 0x00745B54 | 1500.0  | dist² candidate |
| **PeripheryRejectAngle** | +0x20 | 0x00745B58 | 0.39 = cos²(51°) | `cos²(deg)` storage |

### `ActorLOD` group

| name          | address    | notes |
|--------------:|-----------:|-------|
| LODScale      | 0x00745B7C | global LOD multiplier |
| ONCAMERA      | 0x00745B80 | LOD distance band |
| NEARCAMERA    | 0x00745B84 | |
| MEDIUMCAMERA  | 0x00745B88 | |
| FARCAMERA     | 0x00745B8C | |
| OFFCAMERA     | 0x00745B90 | |

### `lightbloom`

```
ShiftPixels   0x00724F7C
OffsetPixels  0x00724F80
Brightness    0x00724F84
Iterations    0x00724F88   (typed_a — bool/int)
FullScreen    0x00724F8C   (typed_e — byte)
Threshold     0x00724F8D   (typed_b)
```

### `defproj` (default projection — heap, not static)

```
hither            0x197C2828    (per-camera defaults; per-camera state
yon               0x197C282C     copies these at init, so live edits
hitherYonFilter   0x197C2830     don't propagate)
fov               0x197C2834
FOVFilter         0x197C2838
aspectRatio       0x197C283C
apectRatioFilter  0x197C2840  [sic, typo in engine]
```

`defproj` was on the heap — meaning each camera class has its own
`defproj` instance. Live writes to these specific addresses won't affect
other camera classes' defproj copies.

## Discovery infrastructure

`src/mtr-asi/src/cvar_dump.cpp` hooks all eight typed-X functions with
per-variant signatures, records (type, group, name, address, caller) into
a fixed vector. UI button writes the dump to `Game/mtr_cvars.txt`.

To dump again: Insert → Tools → Cvar dump → "Dump cvars to mtr_cvars.txt".

## Known limitations

1. **SLNG-hashed strings have no static xrefs.** The engine accesses cvars
   by hashed name lookups (sub_5D3E30), not by direct address. So `xrefs_to`
   on a cvar address returns empty. Have to find consumers indirectly.

2. **Per-entity cvars dominate the dump** (groups shown as raw heap ptrs).
   Most rows are entity-attached properties (water surfaces, lights, FX,
   etc.). The interesting GLOBAL knobs are the minority — filter by
   `grep -vE "ptr=0x"` to see them.

3. **Some cvars are registered multiple times.** The same name appears for
   different per-instance contexts. The dump is a flat log, not deduped.

4. **Pre-init writes bypass the setter callback.** When initialization C++
   code writes a default value directly (e.g., `*(this+8) = 0.39f` for
   PeripheryRejectAngle), it writes the FINAL storage value (the cos²-encoded
   one), bypassing sub_67FE70. So the immediate constants in disasm reflect
   the squared / cos²-encoded form.
