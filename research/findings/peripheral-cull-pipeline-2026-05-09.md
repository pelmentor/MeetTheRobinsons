# Peripheral cull pipeline — full RE (2026-05-09)

Date: 2026-05-09
Status: static investigation complete; runtime probe pending.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## TL;DR

The "circular pattern at screen corners" symptom is produced by **`cull_aabb_corners_vs_global_frustum`** at `0x4E0370`, reading from a **single global frustum struct `g_cull_frustum` at `0x726498`** that mtr-asi has *not* been overriding. All prior fixes targeted the per-camera projection cache at `outer+0xD4`, which is a *different* struct used by D3D's clipping pipeline — not by the engine's per-object cull.

`g_cull_frustum` is populated somewhere we have not yet identified statically (writes go via register-indirect addressing, so direct xrefs are empty). The runtime cull-probe will (a) read the live plane data, (b) count rejects per pipeline stage, and (c) optionally short-circuit the cull to confirm the gate.

## The full per-object cull pipeline

```
scene_tree_walk_with_cull (0x4E4E90)            ← recursive scene-tree visitor
└── per node, dispatch by *(node+5) bits[0:1]:
    ├── 00 → cull_dispatch_with_sphere_thunk (0x4E0B80)
    │   └── cull_dispatch_with_sphere_and_corners (0x4E0AD0)
    │       ├── cull_aabb_vs_aabb           (0x4CA970)  fast reject
    │       ├── cull_sphere_vs_global_frustum (0x4DFF20) sphere/center reject
    │       │   └── increments g_cull_count_sphere_reject (0x724EC4)
    │       └── cull_aabb_corners_vs_global_frustum (0x4E0370) ← THE GATE
    │           └── increments g_cull_count_corner_reject (0x724EC8)
    ├── 01 → cull_dispatch_corners_thunk     (0x4E0A80)
    │   └── cull_dispatch_corners_only       (0x4E0970) — same pipeline, no sphere test
    └── 10 → always-pass (no cull, sets unk_726E00 = 1)
```

Per-object render dispatch from a **separate** entry: `sub_4C1470` (0x4C1470) calls `cull_dispatch_with_sphere_thunk` at `0x4c1537` for *each* renderable object in the scene tree before invoking the per-object render machinery.

## g_cull_frustum struct layout (0x726498..)

| Offset | Field | Notes |
|--------|-------|-------|
| +0 | active scene-scope ptr | set by `global_frustum_set_scene_scope` (0x4DF1E0); read by getter (0x4DF1D0) |
| +128..+140 | plane 0 (vec4: nx, ny, nz, d) | always tested |
| +144..+156 | plane 1 (vec4) | skip flag at +456 |
| +160..+172 | plane 2 (vec4) | always tested |
| +176..+188 | plane 3 (vec4) | always tested |
| +192..+204 | plane 4 (vec4) | always tested |
| +208..+220 | plane 5 (vec4) | always tested |
| +224..+236 | plane 6 (vec4) | skip flag at +457 |
| +400..+424 | alt slot A (24 bytes) | bbox min/max for "non-shadow" test |
| +424..+448 | alt slot B (24 bytes) | bbox min/max for "shadow" test |
| +448 | persisted skip flag for plane 6 | restored on non-shadow path |
| +452 | active alt-slot ptr | points to +400 or +424 depending on shadow flag |
| +456 | skip flag plane 1 | set to 1 on shadow path |
| +457 | skip flag plane 6 | cleared on shadow path |

Plane test math (matches `0x4E0370`):

```
dot(plane.normal, corner.xyz) + plane.d <= 0   →   corner is OUTSIDE that plane
```

If **all 8 transformed AABB corners** of an object fall outside any single plane → object culled.

## How object corners enter the test

1. `sub_4C1470` calls `cull_dispatch_with_sphere_thunk(obj+64, obj+80, ...)`. Argument `obj+64` is the object's **bbox min/max** (24 bytes). Argument `obj+80` is also bbox-shaped.
2. `cull_dispatch_with_sphere_and_corners` runs the AABB-vs-AABB fast reject, then sphere-vs-frustum, then corner-vs-frustum.
3. Inside `cull_aabb_corners_vs_global_frustum`, the bbox min/max from `a2[0..5]` is unrolled into 8 corner points written to `g_cull_object_corners_xform` (0x724D38..0x724D9C) — a 24-float (8×vec3) global. Layout:

```
unk_724D38 = a2[0];    // min.x
unk_724D3C = a2[1];    // min.y
unk_724D40 = a2[2];    // min.z (corner 0: min)
unk_724D44 = a2[0];    // min.x
unk_724D48 = a2[1];    // min.y
unk_724D4C = a2[5];    // max.z (corner 1)
... etc, 8 corners total
```

4. The plane test loops over those 8 vec3 corners (stride 12 bytes) and dot-products each against each plane.

These corners are **already in camera-frustum space** by virtue of the object's bbox being in world coords AND the planes in `g_cull_frustum[+128..+236]` being in world coords. Validity of this assumption depends on which space the planes are stored in — verify via runtime probe.

## Where the planes come from (open question)

Direct data-xref scan for writes to `0x726498+128`, `+144`, etc. → **all empty**. That's because writes happen via register-indirect addressing:

```asm
mov reg, offset g_cull_frustum         ; load base
movups [reg+0x80], xmm0                ; write plane 0
...
```

IDA tracks this as *one* xref at the `lea`/`mov` site; the offsetted writes don't show up as separate xrefs. Byte-pattern scan for `98 64 72 00` in code yielded:

| VA | Function | Action |
|----|----------|--------|
| 0x40B437 | `global_frustum_set_scene_scope` (0x4DF1E0) | writes `g_cull_frustum[+0]` only |
| 0x4DF1D1 | `global_frustum_get_scene_scope` (0x4DF1D0) | reads `g_cull_frustum[+0]` |
| 0x479A60 | `scene_scope_set_then_cull_dispatch_with_sphere` | tail-call wrapper |
| 0x4BA890 | `scene_scope_set_then_cull_dispatch_corners_only` | tail-call wrapper |
| 0x4E09xx, 0x4E0Axx, 0x4E0Bxx | dispatch funcs | read +452/+456/+457 setup |

**None of these writes the planes.** The writer must compute the planes from the camera (or scene) state and store them via a register-indirect `lea/mov ; movaps [reg+offset], xmm0` sequence — either during render-frame setup (probably called from `render_frame_top_level` or earlier) or per-render-context.

The runtime probe will (a) read the live plane values per frame, (b) snapshot before/after specific calls (`render_depth_prepass`, `camera_apply_all_active`), narrowing the writer to a region of code we can then disassemble.

## Why prior overrides did nothing

mtr-asi's frustum-side-plane override (in `freecam.cpp`'s `hk_PerCameraApply`) writes to `outer+0xD4` — the per-camera projection cache. That cache is consumed by:

- D3D's hardware clipping pipeline (when transform is set via `IDirect3DDevice9::SetTransform`).
- `game_camera_build_view_frustum` (0x4DF2C0), which only fires ~5 times at scene init (per camera-subsystem RE) — confirmed via diagnostic log.

It is **not** consumed by `cull_aabb_corners_vs_global_frustum`. That function reads `g_cull_frustum`, which is a separate global. Therefore:

- `force_vis` (call-site rewrite of `vis_test`) → dead because `vis_test` (`sub_4E0B90`) is *not* on the corner-cull path. Per-object cull goes through `cull_dispatch_with_sphere_thunk` at 0x4E0B80, *not* `vis_test` at 0x4E0B90.
- `vis_test_probe` (IAT-slot patch + force-pass) → dead for the same reason.
- `MeshLOD.PeripheryRejectAngle` cvar → unrelated; that controls a different LOD-distance check, not the frustum cull.
- Frustum side-plane override → dead because it writes to `outer+0xD4`, not `g_cull_frustum`.

## What the runtime cull-probe will measure

Module: `src/mtr-asi/src/peripheral_cull_probe.cpp` (to be built).

- **Per-frame counters**: read `g_cull_count_sphere_reject` and `g_cull_count_corner_reject`, compute deltas, expose as "objects culled this frame" by stage. When the user pans the camera so corner objects vanish, the corner-reject counter should spike — confirming the gate.
- **Live plane readout**: every frame, read `g_cull_frustum[+128..+236]` (7×vec4 = 112 bytes) and display the values + a derived "horizontal half-FOV" angle from planes 4/5 (left/right). If aspect ratio is wrong, this tells us by exactly how much.
- **Optional force-pass**: hook entry of `cull_aabb_corners_vs_global_frustum` and short-circuit it to return 1. If corners suddenly stop disappearing → cull confirmed at this exact function. (Keep behind a checkbox; expensive to leave on if it makes the GPU draw 10× more geometry.)
- **Optional dispatch hook**: hook entry of `cull_dispatch_with_sphere_and_corners` (0x4E0AD0) to count total per-object cull dispatches per frame. Useful for sanity-checking what fraction of objects are being culled.

Once the probe identifies which plane has the wrong values for the current 16:9 viewport, we can either:

1. Find the plane writer (via memory-write breakpoint mechanic — set guard page on `g_cull_frustum+128` and catch the access violation) and patch its inputs (most likely the source matrix is the per-camera projection at `outer+0x40` or `outer+0xD4`).
2. Override `g_cull_frustum[+128..+236]` per frame from our hook (if writes happen at a predictable point in the frame).

Either path is a proper root-cause fix per RULE №1.

## Renamed in IDB

- `g_cull_frustum` (0x726498) — was `unk_726498`
- `g_cull_count_sphere_reject` (0x724EC4) — was `unk_724EC4`
- `g_cull_count_corner_reject` (0x724EC8) — was `unk_724EC8`
- `g_cull_object_corners_xform` (0x724D38) — was `unk_724D38`
- `cull_aabb_corners_vs_global_frustum` (0x4E0370) — was `sub_4E0370`
- `cull_sphere_vs_global_frustum` (0x4DFF20) — was `sub_4DFF20`
- `cull_aabb_vs_aabb` (0x4CA970) — was `sub_4CA970`
- `cull_dispatch_with_sphere_and_corners` (0x4E0AD0) — was `sub_4E0AD0`
- `cull_dispatch_corners_only` (0x4E0970) — was `sub_4E0970`
- `cull_dispatch_with_sphere_thunk` (0x4E0B80) — was a `j_` thunk
- `cull_dispatch_corners_thunk` (0x4E0A80) — was a `j_` thunk
- `scene_tree_walk_with_cull` (0x4E4E90) — was `sub_4E4E90`
- `build_frustum_in_object_local_space` (0x4DF6E0) — was `sub_4DF6E0` (peer of the corner test, transforms frustum into per-object local space)
- `plane_test_point_dot_gt0` (0x4D7A40) — was `sub_4D7A40`
- `global_frustum_get_scene_scope` (0x4DF1D0), `global_frustum_set_scene_scope` (0x4DF1E0)
- `frustum_aabb_init_inf` (0x4CA5D0), `frustum_aabb_accumulate` (0x4CA7E0)

Comments at `0x4E0370`, `0x4DFF20`, `0x4E0AD0`, `0x4E0970`, `0x726498`, `0x724EC4`, `0x724EC8`, `0x724D38`, `0x4C1470`, `0x4E4E90`. IDB saved.
