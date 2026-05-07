# Render pipeline — top-down call flow (2026-05-05/06)

End-to-end map of how Wilbur renders one frame. Built from static RE +
runtime probes; cross-references the cull / LOD / projection systems.

**Key function names (renamed in IDB):**
- `render_frame_top_level` (sub_4D22D0) — entry
- `render_depth_prepass` (sub_4BD0C0) — pre-camera shadow/depth pass
- `render_context_list_begin_frame` (sub_4BC720) — per-frame init (no static xref)
- `render_context_run_vis_test` (sub_4BC340) — populates +152 visibility byte array
- `render_per_context_depth_pass` (sub_4BC890) — per-node depth render
- `render_context_draw_visible_slots` (sub_4BCBB0) — visible-slot drawer
- `render_2d_overlay_pass` (sub_4A9CE0) — gated by unk_71D2AC, dormant in normal play
- `tick_2d_overlay_pass` (sub_4A9F10) — overlay queue tick (also gated)
- `init_2d_overlay_mesh` (sub_4A92A0) — wobble/shimmer effect (NOT general HUD)
- `render_particle_systems` (sub_4B5180) — particle effects (vtable[1] dispatch)
- `render_sprite_batcher` (sub_4E8D30) — main 2D/sprite renderer (HUD candidate)
- `render_draw_primitive_dispatch` (sub_5692A0) — DrawPrimitive selector

## Top-level entry — `render_frame_top_level` (sub_4D22D0)

Two callers in the binary:

| caller | when |
|--------|------|
| `sub_572040` | main game loop — runs while `a1[170]` (game-active flag) is true |
| `sub_682010` | loading-screen render path — gated by `*(scene_cvar+2580)` |

Both go through `render_frame_top_level`.

### Per-frame sequence inside `render_frame_top_level`

```
render_frame_begin (sub_562D10)        -- presentation/state setup
render_depth_prepass (sub_4BD0C0)       -- shadow caster setup; walks
                                          g_render_context_list_head (0x724E18)
                                          calling render_per_context_depth_pass
                                          (sub_4BC890) per node
nullsub_2                               -- empty hook slot
[optional]  sub_562FE0                  -- post-fx pre-pass setup
camera_apply_all_active (sub_4C1E40)    -- iterate g_active_camera_array
                                          per cam: sub_4C1BA0 (per-camera apply)
                                                     view→g_d3d_view_matrix_global (0x724C10)
                                                     world→0x724C50
                                                     [mtr-asi hook: freecam, draw-dist,
                                                      side-cull, fog enforcement]
sub_4C4110(0x724BE0)                    -- main render dispatcher; iterates the active
                                          render-target list, calls sub_4C3790
                                          per-target. sub_4C3790 is the per-object
                                          render loop (vis_test → draw)
[optional]  sub_563300                  -- post-fx screen-fill quad (gated by
                                          dword_6FBD2C); calls build_ortho_matrix
                                          (sub_562B70) with (0,1,1,0,-1,1)
tick_2d_overlay_pass (sub_4A9F10)       -- 2D overlay queue tick (priority arrays
                                          unk_71D2C8/D0/D8/E0). Gated by unk_71D2AC.
render_2d_overlay_pass (sub_4A9CE0)     -- 2D overlay render — calls
                                          build_ortho_matrix(0,1,0,1,-0.01,1) +
                                          render_2d_overlay_layers (sub_4A9890)
                                          ** GATED by unk_71D2AC; this is the
                                             init_2d_overlay_mesh effect overlay,
                                             NOT general HUD **
sub_58D9F0(0)                           -- ?
sub_4B61B0                              -- post-process thunk (SecuROM)
render_particle_systems (sub_4B5180)    -- iterates unk_724AF8 array of particle
                                          systems; calls vtable[1] on each
                                          (NOT the HUD path — particle-only)
[deferred draw queues]
sub_4C9CF0..4C9D00                      -- post-process passes (thunks, mostly
                                          SecuROM-protected)
render_sprite_batcher (sub_4E8D30)      -- per-frame sprite/2D renderer; walks
                                          unk_7271E8 list. Sets per-frame proj+view
                                          via transform_apply_scale_via_stack +
                                          transform_apply_translate_via_stack
                                          (NOT through build_ortho_matrix).
                                          ** Strongest HUD path candidate **
sub_563220(0, 1)                        -- frame end / present
```

## Per-object render — `sub_4C3790`

Iterates `*(this+128)` (object array, `*(this+132)` count). For each
object `v11`:

```
v64 = (v11[26] >> 9) & 0xFFFFFF01;  -- flags from object's bit 9 of [+26]

if (*(this + 149)) {
    main = camera_select_first_main();
    if (vis_test(main, v11+16, v11+20, 0, 0, v64)) {  // ← cull gate
        ...preprocess (occlusion / scale matrix)...
        sub_4C2890(this, v11);                         // ← actual draw
    }
}
else if ((v11[26] & 1) == 0 && sub_4C4760(v11)) {
    // alternate path: object processed earlier this frame
    sub_4C2890(this, v11);
}
```

The flag at `*(this + 149)` selects between "do strict vis-test" and
"trust upstream visibility" paths.

## Object structure (`v11`)

Per-object record consumed by `sub_4C3790`:

| offset (DWORDs) | content |
|----------------:|---------|
| 0..15           | 4x4 world matrix |
| 16..19          | bounding sphere (center.xyz, radius) — first arg to vis_test |
| 20..25          | AABB (min.xyz, max.xyz) — second arg to vis_test |
| 26              | flags |
| 28              | flags2 |
| 31              | ptr to mesh data |
| 37, 38          | counters |
| 41              | ptr to mesh-array (instances) |
| 43              | ptr to render obj |
| 174             | scratch |

## Visibility byte array — sub_4BC340 → `*((BYTE*)v2 + idx + 152)`

`sub_4BC340` is the **upstream visibility list update**. Called from
`sub_4C4420` and `sub_4C5730` (the top-level render-dispatch paths
called by `sub_4C4110`). It walks the linked list at
`g_render_context_list_head` (0x724E18) and:

```
for each node v2 in list:
    v3 = v2[1]                                 // scene ptr
    if (*(v3+104) & 1) || v3 == a1:            // ← scene-flag short-circuit
        for i in 0..v2[8]:
            *((BYTE*)v2 + 152 + i) = 0         // ZERO entire visibility array
        v2[37] = 0
    else:
        for each item:
            v6 = vis_test(...)                 // ← call 1 of 4
            *((BYTE*)v2 + 152 + i) = v6        // store visibility per object
            if (v6) v2[37]++
        unk_724E34 += v2[37]
```

Downstream rendering reads from the byte array at `+152`. **This is why
patching only the downstream `sub_4C3790` vis_test call site (0x4C385D)
was insufficient** — the upstream call at 0x4BC406 already filtered
the object set, and downstream just consults the byte array.

`(*(v3+104) & 1) != 0` short-circuits the entire scene tree node — this
is a HIGHER-LEVEL gate than vis_test itself.

**Static-RE conclusion (2026-05-06):** all 8 writers of `(scene+104) bit 0`
were identified and decoded. None are camera-position-driven:
`scene_set_visible` (explicit API), `anim_evaluate_track` (animation
channel-0 ≤ 0.5), three `script_*_hidden` paths, `render_reflection_probe`
(transient self-hide), `world_grid_spawn_scenes` (level-load), and
`scene_instance_create_multi` (constructor). So this flag CANNOT be the
corner-cull driver. The scene-vis tracker (`mtr::scene_vis_log` in mtr-asi)
runs runtime counters on the two writers scripts can drive per-frame —
if counters spike during corner-pan, the cull is script-driven; if they
stay flat, the cull is in the render-context list populator (currently
untraceable statically — `g_render_context_list_head` has zero direct
writes).

## Scene-tree walker — `sub_4BC890`

Called by `sub_4BD0C0` per node in `g_render_context_list_head`. Has the
SAME early-reject:

```
if ((*(a1[1] + 104) & 1) == 0) {  // ← same +104 bit 0 check
    ...do work...
}
```

Inside, iterates per-item:
- Computes per-item position offset
- Calls `matrix4_lookat_rh` to build a per-item view matrix
- `game_d3d_select_transform_stack(0)` → state = D3DTS_PROJECTION
- `wrap_SetTransform(*(v7-4)+16)` → sets that matrix as projection
  (this is the m00=18, m11=32 narrow-FOV projection observed by the
  WrapSetTransform diagnostic; it's per-item shadow-caster / light-view
  setup, NOT the HUD)
- Calls `sub_4C4630` recursively

## D3D transform-stack table

| stack index | D3D state         | usage |
|------------:|-------------------|-------|
| 0           | D3DTS_PROJECTION  | main camera projection, per-item shadow projection |
| 1           | D3DTS_VIEW        | view matrix |
| 2           | D3DTS_TEXTURE0    | texture matrix 0 |
| 3           | D3DTS_TEXTURE1    | texture matrix 1 |
| 4           | D3DTS_TEXTURE2    | texture matrix 2 |
| 5           | D3DTS_TEXTURE3    | texture matrix 3 |

`D3DTS_WORLD` (256) is handled separately — not in the table.

`game_d3d_select_transform_stack(N)` writes `dword_6FBD40[N]` into
`g_d3d_transform_state` (0x6FBD58). Subsequent `wrap_SetTransform(M)`
calls write `M` to that state.

## 2D overlay queues (rarely active)

Four priority arrays at `unk_71D2C8`, `unk_71D2D0`, `unk_71D2D8`,
`unk_71D2E0` hold per-priority 2D rendering items. `tick_2d_overlay_pass`
(sub_4A9F10) ticks them; `render_2d_overlay_pass` (sub_4A9CE0) renders
them under an ortho projection (0, 1, 0, 1, -0.01, 1.0).

**However:** `sub_4A9CE0` is gated by `unk_71D2AC`. In all observed
gameplay sessions this flag was 0, so the ortho path never fired. The
runtime `BuildOrtho` log captures showed only 3 calls per session, all
from `debug_overlay_draw_4x3_wireframe` (0x41C788) with hardcoded
`(-4, 4, -3, 3, -1, 1)` bounds.

The 2D overlay system is also NOT the general HUD path — `init_2d_overlay_mesh`
(sub_4A92A0, the function that sets `unk_71D2AC = 1` to activate it)
constructs a `(rows+1)*(cols+1)` mesh grid with random per-vertex angles,
which is a SPECIFIC visual effect (wobble / shimmer / distortion overlay)
the engine activates only in scripted contexts. Force-enabling it would
display whatever effect was scripted, not the HUD.

## HUD render path — `render_sprite_batcher` (sub_4E8D30)

The strongest HUD path candidate (confirmation pending Test 7b runtime data).
Walks the linked list at `unk_7271E8` once per frame, called from
`render_frame_top_level` at 0x4D23BF.

```
sub_4E8D30:
    [walks unk_7271E8 to count active items, sets up state]
    if (any items) {
        [select transform stack 2 (TEXTURE0)] + wrap_SetTransform_state
        [select transform stack 0 (PROJECTION)] + push + wrap_SetTransform_state
        [select transform stack 1 (VIEW)]       + push + wrap_SetTransform_state
        transform_apply_scale_via_stack(2.0, -2.0, 1.0)      ← scale matrix
        transform_apply_translate_via_stack(-2.0, -2.0, 0.0) ← translate matrix
        [render-state setup: blend, alpha, fog, etc]
        [walks list AGAIN, fills 3 vertex streams (pos/UV/color), draws quads]
        render_draw_primitive_dispatch(5, ...)               ← case 5 = quads
    }
    sub_4C96A0(...)                                          ← restore states
```

**Matrix-builder details (decoded in IDA, unpacked in rr01):**

`transform_apply_scale_via_stack` (sub_562AA0) calls `matrix4_make_scale`
(sub_63C4E3) via `dword_715B64` thunk to build `diag(sx, sy, sz, 1)`,
then routes through `dword_72E67C[156]` (slot 39 = MultiplyTransform-style).

`transform_apply_translate_via_stack` (sub_562AE0) calls
`matrix4_make_translate` (sub_63C573) via `dword_715B48` thunk to build
identity-with-row-3-translation, then routes through the same vtable slot.

**Practical implication:** the composed transform on a vertex (x, y, z) is
`(sx·x + tx, sy·y + ty, sz·z + tz)`. For 4:3 pillarbox in 16:9, multiply
sx and tx by `4/3 / 16/9 = 0.75`. mtr-asi's `mtr::sprite_matrix` namespace
ships this as a one-click "Auto-pillarbox to 4:3" button in Insert →
Display, plus a per-screen auto-mode that drives factors from
`ui_aspect_rules` automatically.

**Why hk_BuildOrtho doesn't fire on HUD:** the sprite-batcher uses
`transform_apply_scale_via_stack` / `transform_apply_translate_via_stack`,
which bypass `build_ortho_matrix` (sub_562B70) entirely. They go through
the same downstream vtable slot but build different matrix shapes
(scale + translate, not ortho). That's the definitive answer to the
"hk_BuildOrtho only fires for debug overlay" puzzle.

## SecuROM thunks in this region

The render pipeline contains many 1-instruction `jmp [imported]` thunks:
- vis_test (sub_4E0B90) → `[0xF92F34]`
- post-fx thunks (sub_4C9970, sub_4C9F70, etc.) → `[byte_F5F876+...]`

MinHook on these is unstable (1-byte instruction → fragile trampoline).
**IAT-slot patching** (write our wrapper VA into the indirect target
slot) is the safe path — see `mtr::vis_test_probe` for the reference
implementation.

## See also

- [`camera-subsystem.md`](camera-subsystem.md) — camera classes, FreeCam, draw-distance
- [`cvar-system.md`](cvar-system.md) — typed cvar registration, storage encoding
- [`optimization-systems.md`](optimization-systems.md) — LOD, periphery, fog, vis_test catalog
- [`ui-render-investigation.md`](ui-render-investigation.md) — what we know about HUD rendering
