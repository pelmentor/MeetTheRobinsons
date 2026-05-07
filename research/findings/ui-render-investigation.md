# UI / HUD render path investigation (2026-05-05)

Status: **partial — actual HUD render path not yet identified.** This
documents what we tried, what failed, and what remains.

## The user-facing problem

User reports "UI aspect ratio is dead" — wants the option to choose what
aspect ratio different UI elements (HUD, menus, dialogs) render at, even
granularly per-screen. None of the override mechanisms I deployed made
the HUD respond.

## What we know works (fog, draw-distance overrides)

`mtr::scene::fog_disabled` and `mtr::draw_dist::set` work because they
write to addresses that the engine reads each frame. The failed UI
overrides write to addresses or call paths that turn out NOT to be the
ones the HUD goes through.

## Path attempted #1: `hk_BuildProjMatrix` FOV<10° heuristic — DEAD

In `d3d9_hook.cpp`, when sub_562B20 (build_proj_matrix) is called with
FOV < 10°, classify as "UI/HUD" and substitute aspect:

```c
const bool is_ui = (fov_deg < 10.0f);
if (is_ui && mtr::aspect_ui::has_override()) {
    aspect = mtr::aspect_ui::current();
}
```

**Why dead:** the narrow-FOV (~3.5°) projection IS observed at runtime
(diagnostic log: `m00=18.16 m11=32.29 caller=004BC9AC`), but it's
**inside `sub_4BC890`'s shadow-caster loop**, not the HUD path. Each
shadow caster gets its own narrow-FOV projection set. Substituting the
aspect there has no visible effect on HUD.

The FOV<10° pattern matches the wrong thing. Crutch.

## Path attempted #2: `hk_BuildOrtho` on `sub_562B70` — DEAD

`sub_562B70(l, r, t, b, n, f)` is the engine's actual ortho matrix
builder — calls `matrix4_make_ortho` (sub_63CB8B) and routes the matrix
through `dword_72E67C` vtable[156].

Static xrefs show two callers:
- `render_2d_overlay_pass` (sub_4A9CE0) → `(0, 1, 0, 1, -0.01, 1.0)`
- `sub_563300` → `(0, 1, 1, 0, -1, 1)` — Y-inverted post-fx screen quad

mtr-asi hook `hk_BuildOrtho` matches the HUD signature `(0, 1, 0, 1, ...)`
and pillarboxes by widening L/R bounds:

```
factor = ui_target_aspect / screen_aspect
L_new = 0.5 - 0.5/factor
R_new = 0.5 + 0.5/factor
```

Math is correct (verified algebraically and via vertex test).

**Why dead:** at runtime, the diagnostic log captured `BuildOrtho`
firing only 3 times in a session — all from caller `0x41C7B8` (now
identified as `debug_overlay_draw_4x3_wireframe`, sub_41C788) with
hardcoded `(-4, 4, -3, 3, -1, 1)` bounds. This is a debug overlay that
runs at scene transitions, NOT the HUD.

`render_2d_overlay_pass` is **gated by `unk_71D2AC`** which was 0 in
all observed sessions, so the (0,1,0,1) ortho path is dormant.

## What's the actual HUD path?

Unknown. Hypotheses:

### Hypothesis A: XYZRHW pre-transformed vertices

D3D's `D3DFVF_XYZRHW` (= 0x4) format flag tells the GPU "these vertices
are already in screen space; don't apply any transform". Many older
games use this for HUD/menus — vertex positions are computed in screen
space directly, no projection / view matrix needed.

If Wilbur's HUD uses XYZRHW, our projection-level overrides have no
effect because the HUD doesn't use the projection at all.

**To verify:** hook `IDirect3DDevice9::SetFVF` and `SetVertexShader`,
log FVF flags around the HUD render time. Or hook DrawPrimitive and
inspect the bound stream descriptor.

**To override:** intercept the vertex buffer at lock/unlock time and
scale the screen-space X coords. This is doable but invasive.

### Hypothesis B: 3D-near-camera HUD elements

HUD elements rendered as regular 3D objects positioned at fixed offset
from camera (z=very-small). They use the regular perspective projection
+ view matrix.

**Implication:** changing world aspect ALREADY affects them (we override
that in hk_BuildProjMatrix). The user might already see HUD scale with
world aspect — but they want a SEPARATE aspect for HUD vs world.

For separate HUD aspect, would need to identify the per-HUD-object
projection setup (probably has a different projection setup we haven't
found).

### Hypothesis C: Dedicated 2D-fonts/sprites pipeline (D3DXSprite-like)

Some games use D3DXSprite or a custom sprite batcher for HUD. We saw
the engine has a sprite renderer at `sub_4E8D30` (walks `unk_7271E8`
list, uses transform stack 2 = D3DTS_TEXTURE0).

**To verify:** hook sub_4E8D30 entry and log when it fires, what
matrices are active, what FVF.

## Path attempted #3: per-screen aspect rules — INFRASTRUCTURE OK, GATE WRONG

We built rules infrastructure in `ui_aspect_rules.cpp`:
- `screen_push.cpp` mirrors the last successfully-pushed screen name
- `mtr::ui_aspect_rules` resolves an aspect from rules table given top
  screen name
- `hk_BuildOrtho` consults rules

The rules table works correctly. But since the ortho hook itself
doesn't fire on the actual HUD, the rules layer is moot until we find
the HUD render path.

UI is exposed in Insert → Display → "UI aspect — per-screen rules". The
"current top screen" indicator updates correctly when screens are
pushed (verified working).

## Static RE update (2026-05-05/06) — sprite-batcher path FULLY decoded

Static RE of `render_sprite_batcher` (was `sub_4E8D30`, renamed in IDB)
nailed down WHY `hk_BuildOrtho` doesn't fire on the HUD, even though the
batcher is plausibly the HUD path:

- `render_sprite_batcher` is called **once per frame** from
  `render_frame_top_level` at 0x4D23BF, AFTER all 3D passes.
- It pushes its own projection + view via two helper functions
  (renamed in IDB):
    - `transform_apply_scale_via_stack(2.0, -2.0, 1.0)` at 0x4E8DDA
    - `transform_apply_translate_via_stack(-2.0, -2.0, 0.0)` at 0x4E8DEB
- Both helpers build a 4×4 matrix via runtime function pointers in the
  table at `0x715B40` (statically initialized to point into rr01).
  After post-decryption, the targets ARE static-RE-decodable:
    - `dword_715B64` → `matrix4_make_scale(out, sx, sy, sz)` at 0x63C4E3
      — pure diagonal scale matrix `diag(sx, sy, sz, 1)`.
    - `dword_715B48` → `matrix4_make_translate(out, tx, ty, tz)` at
      0x63C573 — pure translation matrix with row 3 = (tx, ty, tz, 1).
- The matrix is routed through `dword_72E67C[156]` (slot 39 of the
  engine wrapper's vtable — `MultiplyTransform`-style: composes onto
  current transform stack).
- `sub_562B70` (the `BuildOrtho` we hook) is **not on the path**. The
  batcher uses an entirely separate matrix-build path. That is the
  definitive reason `hk_BuildOrtho` never fires on the HUD.

**Practical implication for HUD aspect override:**

The composed transform applied to a sprite vertex (x, y, z) is:
```
(x, y, z) → (sx·x + tx, sy·y + ty, sz·z + tz)
         = (2x − 2, −2y − 2, z + 0)   for default args
```

So the X-extent in clip space is controlled by `sx` (matrix A's a1).
For pillarboxing the HUD into a narrower aspect:
- `target_aspect / screen_aspect = 4/3 / 16/9 = 0.75`
- Multiply `sx` by 0.75 → narrower X clip-space extent → pillarbox.
- Also multiply `tx` by 0.75 → keeps the narrower extent centered.

The mtr-asi menu (Insert → Display → "[experimental] sprite-batcher
matrix override") exposes:
- A toggle (off by default; passthrough at 1.0 = zero risk to normal play)
- Per-arg multipliers for both matrices (sx/sy/sz and tx/ty/tz)
- Two **Auto-pillarbox** buttons (4:3 and 16:10) that compute the
  correct factor automatically and enable the override in one click.

**Other static RE artifacts:**
- Vertex format is **NOT XYZRHW** — pos stream is 12 bytes/vert (XYZ
  only), separate streams for UV (8 B) and color (4 B).
- `render_draw_primitive_dispatch` (was `sub_5692A0`) is the actual
  D3D `DrawPrimitive` call. Case 5 (the case the batcher takes) draws
  quads (i+=4, prim type 6 = D3DPT_TRIANGLELIST, 2 prims/quad).
- The list walked is `unk_7271E8` (no static external xrefs — pushers
  use struct-relative addressing we can't surface statically).

### Corner-cull thread (2026-05-05) — `(scene+104) bit 0` is NOT it

Renamed the writers in IDB:

| Function (renamed) | What it does | Fires when |
|--------|--------------|------------|
| `scene_set_visible` (was sub_4AABC0) | Explicit `(scene, on/off)` API. SETs bit 0 to hide; CLEARs to show. | Script/level transitions |
| `anim_evaluate_track` (was sub_4E4370) | Animation visibility track. SETs bit 0 when channel-0 ≤ 0.5 OR parent hidden (cascade). CLEARs when channel-0 > 0.5. | Per-frame for active anim tracks |
| `script_set_instance_hidden` (was sub_5E3DC0) | Reads `instance_hidden` script property; toggles bit 0 to match. | Script update |
| `script_init_subset_hidden` (was sub_5E4530) | Hides "subset" scenes during entity init. | Init only |
| `script_set_visible_with_fade` (was sub_5E6740) | Script API for fade in/out. | Script call |
| `render_reflection_probe` (was sub_4E6A20) | TRANSIENT hide around reflection-probe render so scene doesn't appear in its own reflection. Sets bit 0, calls `sub_4C1BA0`, clears bit 0. | Per reflection probe |
| `world_grid_spawn_scenes` (was sub_4168F0) | Level-grid spawn — only ENABLES (clears bit 0); never hides. | Level load |
| `scene_instance_create_multi` (was sub_506300) | Allocates 240-B scene; init bit 0 = 0. | Spawn |

**None of these are camera-position-driven.** The user's "circular
pattern at screen corners" cannot be explained by any of these writers.
Conclusion: `(scene+104) bit 0` is NOT the corner-cull mechanism. The
cull is somewhere else entirely.

### What this leaves for the corner-cull

The user's symptom "circular pattern at screen corners" combined with
"force_vis on all 4 sites + force_pass via IAT both dead" + "(scene+104)
bit 0 ruled out" narrows the remaining suspects sharply:

1. **The render-context list itself is filtered before vis_test runs.**
   `render_context_run_vis_test` (sub_4BC340) iterates
   `g_render_context_list_head`; if a higher-level system removes
   contexts from this list based on camera angle, force_vis can't help
   because the contexts never reach vis_test. Need to find the LIST
   POPULATOR — what walks the scene tree and decides which nodes become
   render contexts each frame.
2. **Aspect-induced over-cull.** The widescreen aspect override
   (mtr::aspect, in BuildProjMatrix) might be widening the projection
   without widening the engine's INTERNAL frustum. The engine could be
   running its own aspect-locked test (e.g., using `defproj.aspectRatio`
   = 4:3 cached at init) that culls outside the original 4:3 horizontal
   FOV, producing a "vignette" effect at corners that LOOKS circular
   but is actually rectangular (the original 4:3 frame inside our 16:9).
3. **Sectors / occluders / PVS** — strings exist in the binary
   (`DB_SECTORS`, `occluder`) but consumer code is SLNG-hashed with no
   static xrefs. Cannot be surfaced statically without computing the
   hash for the strings and finding the code-ref.

Hypothesis #2 is now my top suspect. The widescreen aspect we set
overrides the projection matrix sent to D3D, but the engine's INTERNAL
visibility test might use `defproj.aspectRatio` directly (without going
through our hook). If the cvar still reads 4:3, the engine culls
everything outside 4:3 horizontal even though the GPU draws 16:9.

### Concrete next steps when investigation resumes

1. **Check what writes `defproj.aspectRatio`** (heap address
   `0x197C283C` per cvar dump). Is it cached per camera at init?
   Hook the writer if found and force it to match the user's display
   aspect.

2. **Find the render-context list populator.** Hooking writes to
   `g_render_context_list_head` (`0x724E18`) at runtime to log who
   adds/removes contexts each frame. Narrows the list-filter
   suspect.

3. **Hook D3DFVF / SetFVF** to log every FVF set during HUD-visible
   state. Confirms the sprite-batcher hypothesis and tells us whether
   to override at vertex stream level.

4. **Hook `matrix_set_via_xform_a` + `matrix_set_via_xform_b`** at
   0x562AA0 / 0x562AE0. These are the actual HUD projection/view
   builders. If we substitute aspect there, the HUD pillarboxes
   correctly.

5. **`screen_pop` hook** — currently `screen_push.cpp` only sees pushes.
   Without pop visibility, the "current top screen" goes stale when the
   user backs out of a menu. Pop is at `*(captured_manager+24)+52`
   vtable[2 or 3] — needs vtable RE.

## Per-entity context cvars from cvar dump

The cvar dump revealed many per-entity `aspectRatio` cvars:
- `(ptr=0x030FFD08) | aspectRatio | 0x1944xxxx` — these are per-entity
  light or FX instances, NOT global UI knobs. Not useful for HUD.

`defproj.aspectRatio` at `0x197C283C` is on the heap (per-camera-class).
Live writes don't propagate (per-camera-state caches it at init).

## Implementation status (currently shipping in mtr-asi.asi)

| File | Status |
|------|--------|
| `aspect_patch.cpp` `mtr::aspect_ui` | knob exposed, applied via hk_BuildOrtho when matched (currently never fires for HUD) |
| `d3d9_hook.cpp` `hk_BuildOrtho` | hooks sub_562B70, pillarbox math correct, never matches actual HUD ortho |
| `ui_aspect_rules.cpp` | rules table working; consulted by hk_BuildOrtho; moot until HUD ortho path found |
| `screen_push.cpp` `current_top_name()` | tracks last push; pop tracking missing (next iteration) |

When the HUD render path is identified, the rules-table layer will
"just work" because the hook can consult it the same way `hk_BuildOrtho`
already does.
