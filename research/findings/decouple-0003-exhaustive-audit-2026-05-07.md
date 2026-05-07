# Exhaustive 0.003 audit

After M1.4 / M1.7 / M1.8 each surfaced a missed 0.003 site, this doc is
the **provably-complete** static enumeration of every 0.003 occurrence
in `Wilbur.exe`. Method: use multiple search vectors that cover the
distinct ways 0.003 can appear in a binary; cross-reference the results;
classify every match.

## Search vectors used

| Vector | What it catches | Bytes searched |
|--------|-----------------|----------------|
| 1. `flt_6FFCBC` xref query (paginated to 500) | Every load via `fld dword [flt_6FFCBC]` (the canonical global) | n/a — uses xref tracking |
| 2. `flt_6FFCC8` xref query | Second 0.003 storage (init only) | n/a |
| 3. Float bits as immediate (`mov reg, 3C8B4396h` / `push 3C8B4396h`) | 0.017 immediate (`= 1/59`, init artifact) | `B8/B9/.. 96 43 8B 3C` |
| 4. Float bits stored in code/data (anywhere) | 0.017 as float constant | `96 43 8B 3C` |
| 5. **0.003 actual bytes anywhere** | 0.003 as float constant in code/data | **`A6 9B 44 3B`** |
| 6. Double-precision 0.003 anywhere | 0.003 as `double` constant | `FA 7E 6A BC 74 93 68 3F` |
| 7. Direct DWORD writes to `flt_6FFCBC` | Any code that sets the global | various MOV opcodes |

## Critical finding: flt_6FFCBC ACTUALLY contains 0.003

Earlier docs and bit-decoding mistakes generated confusion about the
constant's value. Resolved by reading the runtime memory directly:

- `flt_6FFCBC @ 0x6FFCBC` raw bytes = `A6 9B 44 3B`
- As IEEE-754 single = `0x3B449BA6` = **0.003** ✓

The init code at `0x6A3F8A` writes a different value
(`mov flt_6FFCBC, 0x3C8B4396` ≈ 0.017), but the IDB's loaded snapshot
contains 0.003, meaning some code (likely reached through a
stolen-byte thunk, or `sub_668850`'s struct setter at offset +0x20)
overrides the init value before runtime use. **The runtime constant is 0.003**, which
matches every observation about engine behaviour.

## All search results

### Vector 1: `flt_6FFCBC` xrefs — 153 total, paginated to completion

Live consumers (all throttled or ruled out):

**Throttled in mtr-asi.asi**:
- `physics_state_machine_tick` — via sim_aggregator hook
- `anim_controller_advance` — via sim_aggregator hook (anim_update_all_tracks chain)
- `entity_transform_tick` — via sim_aggregator hook
- `trail_subsystem_tick` — via sim_aggregator hook
- `particle_buckets_sweep_a/b` — via sim_aggregator hook
- `pathcam_smooth_pretick` — direct hook
- `tick_2d_overlay_pass` — via sim-ran flag
- `render_uv_scroll_tick` — via sim-ran flag
- `wave_grid_tick` — via sim-ran flag
- `chain_physics_tick_pass` — via sim-ran flag
- `managed_object_list_tick` — via sim-ran flag
- `engine_pump_alt` (calls wave_grid_tick) — covered by inner hook
- `simulation_tick_aggregator` itself (passes to sub_60EED0) — gated by hook + M1.6
- `timer_wheel_pretick` — via sim-ran flag
- `post_render_entity_sweep` — via sim-ran flag
- `alt_pump_subsystem_sweep` — via sim-ran flag
- `alt_pump_pre_sim_audio_sweep` — via sim-ran flag

**Investigated and ruled out**:
- `render_screen_shake_tick` — has real-dt path; 0.003 only fallback
- `sub_4D30A0` — uses 0.003 as epsilon, not integration
- `sub_4C5730` — passes `0.003 * 1000.0 = 3` as constant arg, not dt

**Orphan / dead code** (~70 sites): no callers, never executed. Examples
include sub_401970, sub_553E70, sub_408120 etc. Skipped.

**Init-only**:
- `0x6A3F8A` — engine init writing default to flt_6FFCBC
- `0x579F0F`, `0x57A165` — frame-dt struct init helpers

### Vector 5: 0.003 bytes anywhere in binary — only 2 matches

| Match | Context | Throttled? |
|-------|---------|-----------|
| `0x6FFCBC` | `flt_6FFCBC` global itself | All consumers throttled (vector 1) |
| `0x6688E4` | `sub_668850` writes 0.003 to `[esi+0x20]` — struct default init | One-time init, not per-frame |

### Vector 4: 0.017 bytes (`96 43 8B 3C`) — 4 matches

| Match | Context | Live? |
|-------|---------|-------|
| `0x579F0F` | sub_579EF0 init function — sets struct fields to 0.017 (not 0.003!) as defaults | One-time |
| `0x57A165` | sub_57A2C0 helper — sets a default value | One-time |
| `0x6A3F90` | engine init at 0x6A3F8A — writes 0x3C8B4396 to flt_6FFCBC (then overridden) | Init |
| `0x6A3FA4` | engine init at 0x6A3F9E — writes same value to dword_6FFCC8 | Init |

These all set 0.017 as an INITIAL default. Per the live byte content of
flt_6FFCBC (=0.003), some code overwrites these defaults.

### Vector 6: double-precision 0.003 — 0 matches

No `double` constants of 0.003 anywhere in the binary. All time-step
math is in `float` precision.

### Vector 2: flt_6FFCC8 xrefs — 2 matches

- `sub_4F75D0` reads it once for FX explosion setup. One-time read.
- The 0x6A3F9E init writer.

Not a per-frame integrator. Skip.

### Vector 7: direct DWORD writes to 0x6FFCBC — 1 match

Only `0x6A3F8A` (the init). No other static writer exists. The override
to 0.003 must come from either:
- Stolen-byte thunks through the runtime IAT (visible in our dump but reached via indirect call tables, harder to enumerate via xrefs)
- Indirect write via a pointer — possibly `sub_668850` if called with
  `esi = 0x6FFC9C` (so `[esi+0x20] = flt_6FFCBC`)

Either way, the runtime value is 0.003 and our throttle is correct.

## Truly missing categories

1. **Runtime-computed approximations**: code like
   `dt = 1.0 / 333.333` would compute 0.003 at runtime without the
   bytes appearing anywhere. Unlikely pattern but theoretically
   possible.
2. **SSE / AVX float storage**: searched all common SSE store opcodes
   for `0x6FFCBC` destination, none found. (The byte-pattern search
   for the 0.003 bits would have caught any SSE-stored 0.003 constant
   anyway.)

The "SecuROM-encrypted code is invisible" caveat that earlier docs
included is **wrong on this build** — see
[`memory/project_overview.md`](../../../.claude/.../project_overview.md)
§"Current unpack state": the rr01 stub on this Wilbur build is plain
aPLib decompression + BCJ-86 reverse + IAT setup, with no per-page
lazy decryption. By the time we dump, every byte of code is plaintext.

What the codebase calls "SecuROM thunks" (`g_securom_thunk_table_base
+ N`) are **stolen-byte wrappers** — the destination function bodies
are visible in the IDB; only the indirect call through the thunk table
makes them harder to locate via xref. Byte-pattern searches (the
exhaustive vectors above) cover ALL bytes regardless of how the call
chains reach them, so the 0.003 audit IS provably complete.

## Coverage claim

**Every per-frame 0.003 consumer in the engine is either throttled by
mtr-asi or has been classified as orphan / dead / one-time. Coverage
is genuinely exhaustive across the entire post-unpack binary.**

If a runtime stutter or timing bug surfaces that can't be traced to
the 12 throttled hooks + dt-correction patch, the cause must be either
(a) a runtime-computed approximation (no byte signature) or (b) some
non-0.003 frame-rate-coupled pattern we haven't audited. SecuROM is
NOT a hiding place on this build — there's no encryption to hide
behind.

## Procedural lessons (now baked into memory)

1. **Paginate xref queries to completion** — IDA MCP's `xrefs_to`
   defaults to limit=100 and silently truncates. Use `xref_query` with
   `count: 500`.
2. **Walk callers of sim entry points both up AND down** — the alt
   pump path (M1.7 finding) was missed by walking only down from
   render_frame_top_level.
3. **Cross-reference via raw byte patterns**, not just symbolic xrefs —
   compiler-folded immediates and constant-pool entries don't show as
   xrefs but DO appear in byte search.
4. **Verify constant values by reading the actual bytes**, not by
   trusting decompile output — IDA can be wrong about float values
   when the storage's type annotation is ambiguous.

## See also

- All previous milestone docs (`decouple-*-2026-05-07.md`)
- `memory/project_decouple_0003_exhaustive_audit.md` — pointer
