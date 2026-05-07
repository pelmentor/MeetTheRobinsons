# Investigation state — autonomous multi-session run (2026-05-05/06)

Snapshot of where the two open investigations stand after ~8 autonomous
sessions of static RE + diagnostic + override deployment. The user is
away from the PC; this doc + the testing checklist are what they read on
return to triage.

## TL;DR

Two threads were active at start: **UI aspect override** (HUD doesn't
respond to user-set aspect) and **corner culling** (geometry vanishes at
screen corners; vis_test bypass is dead).

**Update 2026-05-06 (later same day):** Picking + on-screen gizmo for
the per-element editor shipped. Direct manipulation: click a sprite in
the game, drag handles to translate / scale, layered cycling for atlas
overlays, drag-group, auto-group from path, auto-Specialize on first
edit. Mouse input under DirectInput-exclusive resolved by extending the
existing `WH_MOUSE_LL` hook from wheel-only to capturing every mouse
event when an mtr-asi UI is visible — the geometry was always there
(Phase A entry-CSV captures ruled out ext_positions / state_key=0
hypotheses); clicks just never reached the picker. See
[`sprite-picking-gizmo-architecture.md`](sprite-picking-gizmo-architecture.md)
+ [`sprite-batcher-entry-distribution-2026-05-06.md`](sprite-batcher-entry-distribution-2026-05-06.md).

Same day a separate finding: the sprite-batcher matrix pipeline is
**pre-multiply** (`top := translate × scale`) — auto/pass-override
factor must hook XformA (scale) only, never XformB (translate); the
reverse caused a visible right-shift bug on per-screen pillarbox rules.
See `memory/project_sprite_matrix_pipeline.md`.

- **UI aspect:** code path fully reverse-engineered. The HUD goes
  through `render_sprite_batcher` (sub_4E8D30) which builds projection +
  view via two distinct helpers (`transform_apply_scale_via_stack` +
  `transform_apply_translate_via_stack`) that bypass our existing
  `hk_BuildOrtho` hook. The matrix builders are now decoded
  (matrix4_make_scale + matrix4_make_translate). mtr-asi has a
  full override path with auto-pillarbox button + per-screen
  rules-driven auto-mode. **Pending only runtime confirmation.**

- **Corner culling:** `(scene+104) bit 0` ruled out as the cull
  driver — all 8 writers identified, none camera-driven. Top remaining
  suspect is the render-context list populator, which is statically
  untraceable (`g_render_context_list_head` has zero direct writes).
  Runtime-data dependent: `mtr::scene_vis_log` resolves the
  script-driven hypothesis with one camera-pan during gameplay.

## Investigation 1 — UI aspect override

### Question
Why does the user's UI aspect setting do nothing? `hk_BuildOrtho`
captured only 3 calls/session — all from `debug_overlay_draw_4x3_wireframe`,
not the HUD. So the HUD doesn't go through `build_ortho_matrix`.

### Static RE (resolved)
The HUD path is the per-frame **sprite batcher** at
`render_sprite_batcher` (sub_4E8D30, called once from
`render_frame_top_level` at 0x4D23BF, AFTER all 3D passes).

Each frame the batcher:
1. Selects transform stack 0 (PROJECTION), pushes, calls wrap_SetTransform_state
2. Selects transform stack 1 (VIEW), pushes, calls wrap_SetTransform_state
3. Calls `transform_apply_scale_via_stack(2.0, -2.0, 1.0)` —
   builds `diag(2, -2, 1, 1)` via `matrix4_make_scale` (0x63C4E3),
   routes through `dword_72E67C[156]` (slot 39 = MultiplyTransform-style)
4. Calls `transform_apply_translate_via_stack(-2.0, -2.0, 0.0)` —
   builds row-3 translation `(-2, -2, 0)` via `matrix4_make_translate`
   (0x63C573), same vtable slot
5. Walks `unk_7271E8` linked list, fills 3 vertex streams (pos/UV/color),
   draws via `render_draw_primitive_dispatch(5, ...)` = quads
6. Pops stacks 0 and 1

The composed transform on a vertex (x, y, z) is
`(sx·x + tx, sy·y + ty, sz·z + tz)`. Default = (2x − 2, −2y − 2, z).

### Override path (deployed)
mtr-asi `mtr::sprite_matrix` namespace:
- Off-by-default toggle, passthrough at 1.0 multipliers = zero risk
- Per-arg multipliers (sx/sy/sz, tx/ty/tz) for fine control
- "Auto-pillarbox to 4:3" / "Auto-pillarbox to 16:10" buttons —
  one-click compute factor `target / screen` and apply to sx + tx
- "Drive sx/tx from ui_aspect_rules (per-screen, auto)" mode — each
  frame, look up current top-screen name in the rules table; apply the
  matched aspect's factor automatically. Lets the user define
  `{PauseMenu → 4:3, MainMenu → 4:3, Loading → 16:9}` and have
  pillarboxing track screen navigation.

### Pending runtime confirmation
Tests 7b/7c/7d in [testing-checklist.md](testing-checklist.md):
- 7b: log lines confirm builder hooks fire on HUD-visible state
- 7c: Auto-pillarbox button → HUD pillarboxes to 4:3 in 16:9
- 7d: per-screen rules + auto-mode → HUD aspect tracks screen changes

If 7c works, the feature is shippable in the current form. If 7b shows
the builders fire but 7c doesn't pillarbox, the math derived from static
RE is wrong (worth reviewing manually). If 7b shows builders DON'T fire
on HUD, the actual HUD path is elsewhere — fallback hypotheses are
D3D-level XYZRHW pre-transformed vertices or 3D-near-camera HUD elements
(neither uses the matrix builders).

## Investigation 2 — Corner culling

### Question
User reports a "circular pattern at screen corners" where geometry is
culled. `force_vis` (5-byte call-site rewrite at all 4 sites of
sub_4E0B90) doesn't fix it. `vis_test_probe` (IAT-slot patch with
force-pass) doesn't fix it. So the cull is NOT vis_test.

### Static RE on `(scene+104) bit 0`
This flag short-circuits both `render_context_run_vis_test` (sub_4BC340,
the upstream visibility-list builder that fills the byte array at +152)
and `render_per_context_depth_pass` (sub_4BC890). Hypothesis was that
something camera-driven sets bit 0 for off-screen scenes.

**All 8 writers identified and decoded** (renamed in IDB):

| Function | Role | Camera-driven? |
|---|---|---|
| `scene_set_visible` (0x4AABC0) | Explicit (scene, on) API | No |
| `anim_evaluate_track` (0x4E4370) | Animation channel-0 ≤ 0.5 → hide | No (animation time) |
| `script_set_instance_hidden` (0x5E3DC0) | Reads `instance_hidden` script prop | No (script ctor) |
| `script_init_subset_hidden` (0x5E4530) | Init-time subset hide | No |
| `script_set_visible_with_fade` (0x5E6740) | Script API for fade in/out | No |
| `entity_set_child_visible` (0x4FC540) | Hash-table entity child visibility | No |
| `script_visibility_command_handler` (0x5CE060) | Script command iterator | No |
| `render_reflection_probe` (0x4E6A20) | TRANSIENT self-hide | No |
| `world_grid_spawn_scenes` (0x4168F0) | Level-load (only enables) | No |
| `scene_instance_create_multi` (0x506300) | 240-B scene ctor (only enables) | No |

**Conclusion: `(scene+104) bit 0` is NOT the corner-cull driver.**

### Runtime confirmation diagnostic (deployed)
`mtr::scene_vis_log` hooks the two writers scripts can drive per-frame
(`scene_set_visible` + `script_set_instance_hidden`). Per-frame
counters: hides, shows, script_calls, script_hides, script_shows.
Sticky log of last 8 distinct scene addresses hidden this frame.

Test 4b in [testing-checklist.md](testing-checklist.md): user pans
camera at corners while watching counters. If counters spike at the
symptom, scripts ARE driving (and the sticky list narrows the set of
scenes affected). If counters stay flat, `(scene+104) bit 0` is fully
ruled out — the cull is in the **render-context list populator**.

### What's left if scene_vis_log shows flat counters
The render-context list (`g_render_context_list_head` at 0x724E18) is
walked by every cull / depth path. **Static byte-pattern sweep found
ZERO direct writes to this address** — the pushers must use heap-
pointer-base + offset, or SecuROM-decrypted code. Cannot be surfaced
statically.

Remaining options at that point:
1. Hook the readers and log walk traces (per-frame, expensive but
   answers "what's in the list").
2. SLNG-hash the strings `DB_SECTORS`, `occluder`, `DB_LOD` and find
   them as immediates in code — unlocks the sectors/PVS subsystems.
3. RE-instrumented runtime trace via the `defproj.aspectRatio` cvar
   address (heap, captured by cvar dump) — find readers via a
   write-watch breakpoint.

## What ships in mtr-asi.asi (~420 KB)

| Module | Status | Purpose |
|---|---|---|
| `aspect_patch.cpp` (mtr::aspect, fov, draw_dist, scene, lod, force_vis, sprite_matrix) | All armed | World aspect, fog, draw distance, LOD scale, vis_test bypass, sprite-batcher matrix override |
| `d3d9_hook.cpp` | All armed | EndScene/Reset/SetTransform/BuildProjMatrix/BuildOrtho/PerCameraApply/MatrixSetXformA/B hooks |
| `freecam.cpp`, `cmdline_hook.cpp`, `dinput_hook.cpp` | All armed | FreeCam, native res, input suppression |
| `console.cpp` | Armed | Native engine console restored (F2) |
| `screen_push.cpp` | Armed (push + pop) | Screen-stack mirror with 16-deep tracking |
| `ui_aspect_rules.cpp` | Armed | Per-screen aspect rules table |
| `cvar_dump.cpp` | Armed | 13.3k cvar capture via 8 typed-X registrations |
| `vis_test_probe.cpp` | Armed | IAT-slot diagnostic for vis_test |
| `scene_vis_log.cpp` (NEW) | Armed | (scene+104) bit 0 writer tracker |

## Symbols renamed in IDB during this run

~30 functions, ~25 comments. Major additions:
- Render pipeline: `render_frame_top_level`, `render_depth_prepass`,
  `render_context_run_vis_test`, `render_per_context_depth_pass`,
  `render_context_draw_visible_slots`, `render_2d_overlay_pass`,
  `tick_2d_overlay_pass`, `render_sprite_batcher`,
  `render_draw_primitive_dispatch`, `render_particle_systems`,
  `render_reflection_probe`
- Matrix builders: `matrix4_make_scale`, `matrix4_make_translate`,
  `matrix4_make_ortho`, `transform_apply_scale_via_stack`,
  `transform_apply_translate_via_stack`, `build_ortho_matrix`
- Visibility writers: `scene_set_visible`, `anim_evaluate_track`,
  `anim_update_all_tracks`, `script_set_instance_hidden`,
  `script_init_subset_hidden`, `script_set_visible_with_fade`,
  `entity_set_child_visible`, `script_visibility_command_handler`,
  `world_grid_spawn_scenes`, `scene_instance_create_multi`
- Screen system: `screen_manager_pop_top`,
  `screen_stack_destroy_match`, `screen_manager_get_stack`,
  `screen_manager_pop_to_with_arg`, `particle_system_ctor`,
  `particle_system_render`
- Effects: `init_2d_overlay_mesh`

## See also

- [`ui-render-investigation.md`](ui-render-investigation.md) — full UI aspect findings
- [`optimization-systems.md`](optimization-systems.md) — full cull/LOD catalog
- [`render-pipeline.md`](render-pipeline.md) — top-down per-frame call flow
- [`testing-checklist.md`](testing-checklist.md) — Tests 4b, 7b, 7c, 7d resolve both threads
- [`symbol-table.md`](symbol-table.md) — authoritative IDB rename table
