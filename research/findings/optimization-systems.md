# Engine optimization systems — full catalog (2026-05-05)

Every cull / LOD / fade / fog system identified in Wilbur, with addresses,
storage encoding, override mechanism status, and runtime test results.

## Quick status

| System | Address | Status | mtr-asi knob |
|--------|---------|--------|---------|
| Frustum side planes | per-camera buffer @ outer+0xD4 | data-write works for far plane; **side planes have no observable effect** (something else culls corners) | `mtr::scene::side_cull_disabled` (kept as code, removed from UI) |
| Frustum far plane (draw distance) | per-camera buffer @ outer+0xD4 | **WORKS** — write extends draw distance | `mtr::draw_dist::set` |
| Fog | scene cvar BYTE @ 0x745279 + D3DRS_FOGENABLE filter | **WORKS** | `mtr::scene::set_fog_disabled` |
| MeshLOD.PeripheryRejectAngle | 0x745B58 (cos²(deg) encoding) | **DEAD** for user-reported corner-cull symptom (verified with corrected math) | `mtr::lod::set_periphery_reject_angle_deg` |
| MeshLOD.PeripheryRejectDist | 0x745B54 (likely dist² encoding) | **DEAD** for corner-cull (verified with 1e12 write) | `mtr::lod::set_periphery_reject_dist_squared` |
| MeshLOD.{FocusDist,HighDist,MediumDist} | 0x745B44/48/4C | not yet tested | `mtr::lod::set_focus_dist` etc. |
| ActorLOD.LODScale | 0x745B7C | not yet tested | `mtr::lod::set_lod_scale` |
| vis_test (sub_4E0B90) - 4 call sites | thunk → IAT @ 0xF92F34 | `force_vis` (call-site rewrite all 4) **DEAD** for corner-cull. `vis_test_probe` (IAT patch) deployed for runtime characterization but not yet tested. | `mtr::force_vis::set` + `mtr::vis_test_probe::set_force_pass` |
| `(scene+104) bit 0` per-scene flag | runtime, per-scene-tree node | **suspect** for upstream cull — when set, sub_4BC340 zeros visibility array AND sub_4BC890 skips work. Not yet investigated. | none |
| Sectors / PVS (DB_SECTORS) | unknown — SLNG-hashed strings, no static xrefs | not investigated | none |
| Occluder system (`occluder` string) | unknown — SLNG-hashed | not investigated | none |
| Per-entity InstanceSetFadeOutDistance | per-entity hashtable property | not investigated; not a global knob | none |

## Frustum side / far planes (cached buffer at outer+0xD4)

Engine builds the per-camera frustum once at scene init via
`game_camera_build_view_frustum` (0x4DF2C0) and caches it at the
camera's `outer+0xD4..outer+0x270` buffer. Hooking the build function
post-init has zero effect because it doesn't fire again.

Solution: data-level overwrite of the cached buffer per frame from
`hk_PerCameraApply`. Layout:

| offset (within outer+0xD4) | content |
|---------------------------:|---------|
| +0..15    | near plane (0,0,1, near) |
| +16..31   | far plane (0,0,-1, -far) ← extended for draw-distance |
| +32..47   | top plane    ← side cull disabled writes (0,0,0,1) |
| +48..63   | bottom plane ← same |
| +64..79   | left plane   ← same |
| +80..95   | right plane  ← same |
| +96..127  | optional extra clip (water/reflection) |
| +352..399 | 4 far corners (16 floats; orig hardcodes z=-10000) ← recomputed for new far |

**Key finding:** the far-plane override DOES extend draw distance. The
side-plane (0,0,0,1) override has NO observable effect — meaning corner
culling is NOT done via these frustum side planes. The cull function
must compute its own test independent of the buffer (or the planes are
re-derived from corners somewhere).

## Fog (works correctly)

Engine "scene" cvar block at 0x745240. `fogEnabled` is a BYTE at
0x745279. Writing 0 disables. To prevent drift, mtr-asi pumps the byte
each frame from `hk_PerCameraApply` AND filters `D3DRS_FOGENABLE` in
`hk_SetRenderState` as belt-and-braces.

This is the canonical example of a working data-level cvar override.

## MeshLOD periphery culling — RE'd but DEAD for the symptom

Cvar group registered by `meshlod_register_cvars` (0x67FED0). Struct
base 0x745B38:

| offset | name                     | default | encoding |
|-------:|--------------------------|---------|----------|
| +0x0C  | FocusDist                | 100.0   | possibly dist² (common "real" setter squares) |
| +0x10  | HighDist                 | 250.0   | dist² candidate |
| +0x14  | MediumDist               | 500.0   | dist² candidate |
| +0x18  | PeripheryAcceptDist      | 100.0   | dist² candidate |
| +0x1C  | PeripheryRejectDist      | 1500.0  | dist² candidate |
| +0x20  | **PeripheryRejectAngle** | 0.39    | **cos²(half_cone_deg)** — confirmed via setter `cvar_setter_deg_to_cos2` (0x67FE70) |

**Storage encoding gotcha** that initially misled debugging:

```
PeripheryRejectAngle setter (0x67FE70):
    sub_67FE70(deg) = cos²(deg * π/180)

→ default 0.39 ≈ cos²(51°) = ~51° half-cone

To DISABLE write 0.0 (= cos²(90°) = full hemisphere)

Earlier attempt with 3.14 (radians for 180°) was the OPPOSITE of disable:
the engine reads `dot² >= stored` and stored=3.14 makes the test never
satisfy → everything rejected.
```

Test result with corrected math: still no observable effect on the user's
"circular pattern at screen corners" symptom. Either the cvar is consumed
only at scene init and copied into per-camera state (we'd need to find
the cache), or this isn't the cull at all.

## ActorLOD distance bands

| address    | name         |
|-----------:|--------------|
| 0x745B7C   | LODScale     |
| 0x745B80   | ONCAMERA     |
| 0x745B84   | NEARCAMERA   |
| 0x745B88   | MEDIUMCAMERA |
| 0x745B8C   | FARCAMERA    |
| 0x745B90   | OFFCAMERA    |

Not yet runtime-tested. UI knob exposed via mtr::lod::set_lod_scale.

## vis_test (`sub_4E0B90`) — 4 call sites, both override mechanisms dead

SecuROM-protected 1-instruction thunk: `jmp [0xF92F34]`. The IAT slot is
resolved by SecuROM at runtime to the real impl in a decrypted segment.

Calling convention: `__cdecl` (caller-cleanup `add esp, 18h` at every
site = 6 args). Returns struct ptr; callers use only AL.

### Four call sites

| addr      | function         | role |
|-----------|------------------|------|
| 0x4BC406  | sub_4BC340       | **scene-tree visibility list update** (THE upstream cull — result stored in byte array at *((BYTE*)v2 + idx + 152), downstream reads from this) |
| 0x4C385D  | sub_4C3790       | main per-object render loop (downstream) |
| 0x4CBAC7  | unrecognized fn  | real CALL in unanalyzed code |
| 0x4E6A5A  | sub_4E6A20       | reflection probe path |

### Override mechanisms

1. **`mtr::force_vis`** — replaces each `call sub_4E0B90` (5 bytes) with
   `mov al, 1; nop*3` (5 bytes). Atomic per-site, restored from saved
   bytes on toggle off. All 4 sites verified as `0xE8 CALL` before patch.

2. **`mtr::vis_test_probe`** — IAT-slot patch at `0xF92F34`. Wrapper
   counts pass/fail per call site, optionally short-circuits to return
   1. Polled-install (up to 30s) since SecuROM resolves the slot at
   runtime. `frame_tick()` snapshot from EndScene hook.

Both **DEAD** for user-reported corner-cull symptom (verified 2026-05-05).

### Why MinHook on the thunk is wrong

Previous attempt to MinHook `sub_4E0B90` directly caused save-load
crashes. The thunk is one instruction (`jmp [imm32]`, 6 bytes); MinHook's
trampoline interaction with such a short function is fragile.

**IAT-slot patching is the safe path.** We don't touch the thunk or the
real impl — we just retarget the indirect jump.

## `(scene+104) bit 0` — RULED OUT for corner-cull (2026-05-05)

Both upstream functions short-circuit on this flag:

```c
// render_context_run_vis_test (sub_4BC340):
if ((*(BYTE*)(v3 + 104) & 1) != 0) {
    for (i in 0..count) *((BYTE*)v2 + 152 + i) = 0;  // zero ALL visibility
    v2[37] = 0;
} else {
    ...vis_test loop...
}

// render_per_context_depth_pass (sub_4BC890):
if ((*(BYTE*)(a1[1] + 104) & 1) == 0) {
    ...do work...
}  // else skip the entire node
```

**Static-RE'd all writers** (full byte-pattern sweep for
`or [reg+0x68], 1` and `and [reg+0x68], 0xFE`). Writers (renamed in IDB):

| Function | Role | Camera-driven? |
|----------|------|---------------|
| `scene_set_visible` (0x4AABC0) | Explicit `(scene, on)` API | No |
| `anim_evaluate_track` (0x4E4370) | Animation channel-0 ≤ 0.5 → hide | No (animation time) |
| `script_set_instance_hidden` (0x5E3DC0) | Reads `instance_hidden` script prop | No |
| `script_init_subset_hidden` (0x5E4530) | Init-time hide of subset scenes | No |
| `script_set_visible_with_fade` (0x5E6740) | Script API for fade in/out | No |
| `render_reflection_probe` (0x4E6A20) | TRANSIENT self-hide for reflection probe (sets bit 0, calls `sub_4C1BA0`, clears bit 0 — pattern at 0x4E6DF7/0x4E6E02) | No |
| `world_grid_spawn_scenes` (0x4168F0) | Level-grid spawn — only ENABLES | No |
| `scene_instance_create_multi` (0x506300) | 240-byte scene constructor — only ENABLES | No |

**Conclusion: the bit-0 flag is NOT camera-driven and cannot explain
corner culling.** The corner-cull mechanism is something else entirely.

### What corner-cull might actually be

With vis_test (force_vis 4-site + IAT-slot probe) AND `(scene+104) bit
0` both ruled out, the remaining suspects are:

1. **Render-context list populator filtering before vis_test runs.** The
   list head `g_render_context_list_head` (0x724E18) is iterated by
   `render_context_run_vis_test` and `render_per_context_depth_pass`,
   but who BUILDS the list each frame? If contexts are filtered by
   camera angle BEFORE being added, force_vis can't help. The
   list-builder is the next RE target. (`render_context_list_begin_frame`
   at 0x4BC720 has no static xrefs — likely called via function pointer.)

2. **`defproj.aspectRatio` cached at 4:3.** The cvar lives at heap
   address (0x197C283C) per cvar dump — the engine MAY use this for
   internal frustum tests, separate from the projection matrix the GPU
   uses. Our `aspect_patch` overrides the GPU matrix, but if the engine
   tests visibility against the original 4:3 cached value, anything
   outside the 4:3 horizontal FOV gets culled even though the GPU draws
   16:9 — producing an apparent "circular" pattern at corners.

3. **Sectors / PVS / occluders.** Strings exist in binary
   (`DB_SECTORS`, `occluder`, `DB_LOD`) but all SLNG-hashed with no
   static xrefs. Untraceable without hash-immediate search or runtime
   instrumentation.

Hypothesis #2 is the new top suspect. **Action**: trace
`defproj.aspectRatio` writers/readers to confirm/refute.

## Sectors / PVS / occluders (untested)

Strings discovered in the binary:
- `DB_SECTORS` @ 0x6BC3EC — **no static xrefs** (SLNG-hashed)
- `DB_LOD` @ 0x6BC934 — no xrefs
- `Sectors` @ 0x6BC3F8 — no xrefs
- `occluder` @ 0x6C4B58 — no xrefs

These systems exist but their consumer code is hash-indirected. Tracing
requires either:
1. Computing the SLNG hash and finding its immediate value as a code-ref
2. Hooking the runtime hash-lookup function (sub_5D3E30) to log lookups

Not investigated yet.

## Per-entity fade systems (per-instance, no global knob)

Strings present:
- `fadeIn`, `fadeOut`, `fFadeInThis`, `fFadeOutPrev` — referenced in
  `sub_5B40C0` (a script-message handler that reads them via property
  hash-lookup `sub_4B9050`/`sub_4B8F40`)
- `InstanceSetFadeOutDistance`, `MeshSetFadeOutDistance` — script API
  function names

These are **per-entity properties**, not engine globals. No mtr-asi-side
override possible without per-entity instrumentation.

## RE methodology notes

### Cvar dump

The most productive RE technique was hooking all 8 typed-X registration
functions and dumping (group, name, address) at startup → `mtr_cvars.txt`
(13.3k entries). This surfaced LOD / Periphery / scene cvars that have
**no static xrefs** because the engine accesses them via SLNG-hashed
runtime lookup.

See [`cvar-system.md`](cvar-system.md) for the dump infrastructure.

### Storage encoding traps

Many "real" cvars store value squared (setter `cvar_setter_square_real`
sub_429D20). PeripheryRejectAngle stores `cos²(half_cone_deg)`. Always
trace the setter callback before designing override values, or your
"disable" might be the OPPOSITE of disable.

### What didn't work

- MinHook on SecuROM thunks → save-load crashes
- Patching one call site of a multi-call function → upstream cull still applies
- Pattern-matching on FOV<10° to identify HUD → caught a debug overlay, missed the actual HUD

### What worked

- Data-level overwrites of cached engine state (frustum buffer, fog cvar)
- Hooking all leaf functions of a registration system at once
- IAT-slot patching for SecuROM thunks (clean indirect retarget)
