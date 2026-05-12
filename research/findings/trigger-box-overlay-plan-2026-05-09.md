# Trigger box overlay — 3D-projected debug visualization plan

**Date:** 2026-05-09
**Revision:** v2 (post-audit — architecture audit by `a96cd28b`, engineering audit by `aa720b80`)
**Governing rule:** RULE №1 — proper fix, no shortcuts. Weeks/months OK; no quick wins that get reverted.

**v2 changes from v1:**
- Projection capture: switched from "VA read at 0x724C90" to "snapshot in `hk_BuildProjMatrix` POST" (the existing hook already filters RT-probe projections via aspect ~ 1.0 guard).
- Projection-matrix candidate VAs: `0x00745AA0` (already named as `kHaloProjMatrixVA` in [interp_halo.cpp:96](../../src/mtr-asi/src/interp/interp_halo.cpp#L96)) is the primary candidate; 0x724C90 was a guess.
- Phase 0 reordered to 0A → 0C → 0D → 0B (`triggerSize` semantics depend on having property accessor + an entity to test on).
- Phase 0A bootstrapped from existing logs ([sprite_hooks.cpp:88-169](../../src/mtr-asi/src/d3d9_hook/sprite_hooks.cpp#L88-L169) already logs every D3DTS_PROJECTION write).
- Phase 0C uses [interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100) `walk_transform_list` as the safety template (SEH + cap + offset traversal); the new walker is for `entity_manager`'s list (static triggers), not the transform list (dynamic actors).
- Phase 1 acceptance includes SEH guards on matrix reads (was deferred to Phase 5; raised to Phase 1 per RULE №1).
- Phase 2 algorithm corrected: **homogeneous parametric clip** in clip space (pre-divide), not Liang-Barsky / Cohen-Sutherland. Lines crossing `w=0` are clipped at `t = w_a / (w_a - w_b)`, not dropped.
- Projection math made explicit: D3D9 row-vector convention, Y-axis flip on NDC→screen.
- ImGui sequencing: `trigger_overlay::tick()` runs INSIDE `on_end_scene` (after `ImGui::NewFrame()`), not before — `GetForegroundDrawList()` requires a live frame.
- Terminology: "SecuROM stubs" renamed to "stolen-byte IAT thunks" per [feedback_securom_terminology_2026-05-08.md](../../memory/feedback_securom_terminology_2026-05-08.md).
- OQ1, OQ3, OQ4 closed.

---

## TL;DR

Render an in-game debug overlay showing the engine's trigger volumes as 3D-projected wireframe AABBs. Driven by an ImGui-foreground-drawlist projection of each trigger entity's bounding box through the engine's current view + projection matrices. Toggle in Insert → Debug.

Useful for: understanding mission flow (trigger volumes drive `mission_*_start/end` script callbacks), investigating why a mission step doesn't fire, marking spots for screenshots/videos, and (later) extending to other named entity classes for a generic "show entities of class X" debug view.

---

## Why we want it

- Wilbur is mission/script-driven. Strings `mission_orphans_start`, `mission_jacoby_end`, `mission_HavocGloves_start` etc. are .sx-script callbacks bound by name to script logic. Most mission state transitions are gated on player crossing a `trigger_volume` / `triggerbox` / `triggeraoe` AABB region.
- Without a debug overlay, finding where a trigger lives requires reading the level's `.sc` files (text), guessing at world coordinates, and walking around. With an overlay, the trigger geometry is directly visible.
- The engine ships built-in `DrawDebugLine` / `DrawDebugSphere` / `DrawDebugPoint` script callbacks plus a `show_trigger` debug toggle. **All wrapped in SecuROM stolen-byte stubs** — calling them directly would require decoding those stubs (hours of RE). Drawing the overlay ourselves via ImGui sidesteps that entirely.

---

## What we know already

### Confirmed
- View matrix at `0x00724C10` (16 floats, written by `sub_4C1BA0` per-camera apply, read by everything downstream of the apply). Already used by [freecam.cpp](../../src/mtr-asi/src/freecam.cpp) and [interp_view.cpp](../../src/mtr-asi/src/interp/interp_view.cpp).
- World matrix at `0x00724C50` (inverse of view; same writer).
- Engine uses `game_PerspectiveFovRH` (right-handed), Y-up. Verified by freecam basis math.
- Entity manager singleton pointer at `0x007425AC`. Per-entity layout: `+0x58` = vec3 game-logic pos, `+0x70` = 3x3 rotation, `+0x48` = render sub-component.
- Property accessor `entity_get_kv` at `0x004B8F00` (150+ callers, reached via stolen-byte IAT thunk; destination decompilable per memory).
- Trigger entity classes exist as named types: `triggerbox`, `trigger_volume`, `triggeraoe`. Each has properties including `triggerSize`, `triggerCenter`, `triggerDuration`, `triggerMessage`.
- Engine `DrawDebugLine/Sphere/Point` and `show_trigger` callbacks registered at startup via `sub_581420`. Implementations behind stolen-byte IAT thunks at 0x57DAB0 / 0x57DB20 / etc — bytes are runtime-patched by the unpacker; static disasm shows obfuscated dwords. Out of scope for this feature.
- **Projection matrix candidate VA: `0x00745AA0`** (named `kHaloProjMatrixVA` in [interp_halo.cpp:96](../../src/mtr-asi/src/interp/interp_halo.cpp#L96)). The halo follow-fix uses this address as the engine's "current projection" — strong evidence this IS the projection global. Phase 0A confirms by sampling.
- Existing projection-matrix logger: [sprite_hooks.cpp:88-169](../../src/mtr-asi/src/d3d9_hook/sprite_hooks.cpp#L88-L169) `hk_WrapSetTransform` already filters perspective projections via `m32 != 0 && m33 == 0` and logs every distinct caller + matrix value. Phase 0A starts by reading existing `mtr-asi.log` for `WrapSetTransform[PROJECTION]` lines.
- Existing entity walker pattern: [interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100) `walk_transform_list` walks `dword_724DE4` (transform-list head) with SEH guard + 8K-iteration cap + offset-based `next` traversal. Reusable as the safety template for any list walker.

### Unverified — Phase 0 will close
- **Projection matrix capture method.** Two options, both viable:
  - **Read VA `0x00745AA0` directly** (the `kHaloProjMatrixVA` already used by [interp_halo.cpp:96](../../src/mtr-asi/src/interp/interp_halo.cpp#L96)). Cheapest if it's stable across all main-camera writes.
  - **Snapshot in `hk_BuildProjMatrix` POST** (already hooked at [camera_hooks.cpp:59](../../src/mtr-asi/src/d3d9_hook/camera_hooks.cpp#L59) with the existing aspect ~ 1.0 filter at line 60 that suppresses RT-probe / shadow-pass projections). This is the SAFE option — guaranteed to capture only main-camera perspective projections.
  - Phase 0A picks one based on cross-validation.
- **`triggerSize` semantics.** Unknown whether it's `vec3 half_extents`, `vec3 full_extents`, `float radius`, or something else. Phase 0 reads sample values from a known trigger volume (e.g. one of the `R*_DeathTrigger*` from string table at 0x6A6DEA-0x6A6E10) and matches against visible behavior.
- **Trigger orientation.** Are triggers axis-aligned (just position+extents) or oriented (position+rotation+extents)? Affects whether we draw an AABB or an OBB. The entity's `+0x70` 3x3 rotation might apply, or might be ignored for triggers.
- **Trigger entity enumeration.** Walking the entity manager's list to find all trigger-class entities. The entity manager (at `0x7425AC`) has an internal list head — exact offset and traversal pattern needs RE.
- **Property accessor calling convention.** `entity_get_kv` at `0x4B8F00` is `__thiscall` per memory but exact signature for reading vec3 / float properties needs verification.

---

## Architecture

```
                 EndScene PRE hook (already exists in d3d9_hook.cpp)
                                  │
                                  ▼
                      mtr::menu::on_end_scene(dev)
                      ImGui::NewFrame()                          ◄── frame begun
                      ImGui windows render
                      ┌──────────────────────────────────────┐
                      │  mtr::trigger_overlay::tick(dev)     │  ◄── INSIDE on_end_scene,
                      └──────────────────────────────────────┘     after NewFrame, before
                                  │                                ImGui::Render()
                  ┌───────────────┴───────────────┐
                  │                               │
                  ▼                               ▼
       Read view + cached proj         Walk entity_manager list,
       (SEH-guarded). View at          filter to trigger classes,
       0x724C10; proj cached by        snapshot pos + extents +
       hk_BuildProjMatrix POST         rotation + name. Use the
       (filtered to aspect ~ 1.0       interp_npc.cpp::walk_transform_list
       main-camera writes).            safety pattern (SEH + 8K cap
                                       + offset-based next traversal).
                  │                               │
                  └───────────────┬───────────────┘
                                  ▼
                    For each trigger:
                       - Compute 8 world-space corners (rotation matrix from +0x70 if
                         Phase 0B confirms triggers are oriented; else axis-aligned)
                       - Build clip-space points: clip = world_pos_row_vec * View * Proj
                         (D3D9 row-vector convention)
                       - HOMOGENEOUS PARAMETRIC CLIP per edge in clip space
                         against the 6 frustum planes:
                           w + x >= 0,  w - x >= 0
                           w + y >= 0,  w - y >= 0
                                z >= 0,  w - z >= 0     (D3D9 has z in [0, w])
                       - For edges crossing w = 0: clip at t = w_a / (w_a - w_b),
                         lerp the 4D homogeneous endpoints, NEVER divide an unclipped
                         w<=0 endpoint
                       - Whole-box reject: skip if all 8 corners share an outside
                         half-space for any one frustum plane
                       - For surviving (clipped) endpoints: divide by w → NDC
                       - NDC → screen with D3D9 Y-flip:
                           screen_x = (ndc_x * 0.5 + 0.5) * viewport_width
                           screen_y = (1.0 - (ndc_y * 0.5 + 0.5)) * viewport_height
                       - ImGui::GetForegroundDrawList()->AddLine() × 12 edges
                       - Optional: AddText() with trigger name at AABB center
                                  ▼
                       ImGui::Render() pass
                       (in on_end_scene's tail)
                       — foreground draws sit on top of the 3D scene
                         and the engine's HUD; menu windows draw later
                         in the same Render() call so overlay sits
                         BELOW any open menu.
```

### Module layout

- `src/mtr-asi/src/trigger_overlay.cpp` — new module, ~400 LOC
- `include/mtr/trigger_overlay.h` — public API: `enabled()/set_enabled()/tick(dev)/cache_proj(...)`
- **Call site:** INSIDE `mtr::menu::on_end_scene(dev)`, after `ImGui::NewFrame()`, before `ImGui::Render()`. NOT directly in `hk_EndScene` (because `GetForegroundDrawList()` needs a live ImGui frame and the menu module owns NewFrame/EndFrame/Render lifecycle).
- **Projection capture site:** `hk_BuildProjMatrix` POST in [camera_hooks.cpp:59](../../src/mtr-asi/src/d3d9_hook/camera_hooks.cpp#L59). Apply the existing aspect ~ 1.0 guard before caching so we only snapshot main-camera perspective projections, not RT-probe / shadow-pass / minimap projections. New extern: `mtr::trigger_overlay::cache_proj(const D3DMATRIX*)`.
- UI toggle added to [tab_debug.cpp](../../src/mtr-asi/src/menu/tab_debug.cpp), section "Trigger box overlay"

### Public API sketch

```cpp
namespace mtr::trigger_overlay {

bool     enabled();
void     set_enabled(bool v);
int      visible_box_count();   // last-frame count, for UI status

// Per-class show toggles — user can hide noisy class while debugging another.
bool     show_class(const char* name);          // "triggerbox" / "trigger_volume" / "triggeraoe"
void     set_show_class(const char* name, bool v);

// Color overrides (default: per-class hashed color).
void     set_class_color(const char* name, uint32_t abgr);

// Called from EndScene PRE. Performance-bounded internally —
// auto-skips if too many entities or projection invalid.
void     tick(IDirect3DDevice9* dev);

} // namespace mtr::trigger_overlay
```

---

## Phase plan

Each phase is a standalone deployable build. If a phase fails its acceptance criteria, abort and document — RULE №1 says no half-finished implementations.

### Phase 0 — Fact-finding (cap: 2 hr direction-eval, real time can extend)

Acceptance: every "Unverified" item above has a written outcome in this doc. Sub-phases ordered by data dependency (each one consumes the output of the previous).

#### 0A — Projection matrix capture (cap: 30 min)
- **Bootstrap from existing artifacts** (per audit feedback):
  1. Grep `Game/mtr-asi.log` for `WrapSetTransform[PROJECTION]` lines from any prior session. The hook at [sprite_hooks.cpp:88-169](../../src/mtr-asi/src/d3d9_hook/sprite_hooks.cpp#L88-L169) logs every distinct (caller, m00, m11) tuple. If lines exist, we know the matrix shape + which functions write it.
  2. Read 16 floats at `0x00745AA0` during a frame and compare to the most recent `WrapSetTransform[PROJECTION]` log entry.
  3. If matching: confirm `0x00745AA0` as the global. Done.
  4. If not matching: snapshot in `hk_BuildProjMatrix` POST (already hooked at camera_hooks.cpp:59 with `aspect ~ 1.0` filter at line 60). New: `cache_proj(matrix*)` writes to `g_cached_proj[16]` after the orig completes AND the existing aspect filter passes.
- **Output:** a confirmed VA OR a documented decision to cache via the existing hook. `mtr::trigger_overlay::current_proj(out_matrix)` is the single accessor.

#### 0C — Entity enumeration (cap: 1.5 hr)
- **Bootstrap:** read [interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100) `walk_transform_list`. That walks `dword_724DE4` (transform-list head) — works for entities with active transform nodes (animated actors / NPCs). Triggers are static and may NOT have transform nodes — that walker may miss them.
- The entity_manager singleton at `0x007425AC` has its own list (the one [entity_lookup_by_name_retry](../../src/mtr-asi/src/freecam.cpp#L120) walks looking for "player"). RE the list head + next-offset by reading `entity_lookup_by_name_retry` (0x5AC8F0) decompile.
- Apply the same safety idiom as `walk_transform_list`: SEH guard around every pointer read, hard cap iteration count (8192), validate next-pointer is heap-shaped before deref.
- **Output:** `for_each_entity(callback)` walking entity_manager's list. Callback receives `void* entity_ptr` per node.

#### 0D — Property accessor signature (cap: 30 min)
- Decompile `0x4B8F00` (the IAT-thunked `entity_get_kv`). Confirm exact ABI: arg list, return type, how it signals not-found.
- Build typed wrappers:
  - `bool get_property_vec3(void* entity, const char* key, float out[3])`
  - `bool get_property_float(void* entity, const char* key, float* out)`
  - `bool get_property_string(void* entity, const char* key, char* out, size_t cap)`
- Test on a known entity (e.g. read `triggerSize` from any one trigger from Phase 0C's enumeration).
- **Output:** typed accessor module + verified read of one trigger's `triggerSize`.

#### 0B — `triggerSize` semantics (cap: 30 min)
- For one captured trigger from Phase 0C+0D: read its position (`+0x58`), rotation (`+0x70`), and `triggerSize` property.
- Walk to the trigger in-game; record the boundary at which the trigger's script callback fires:
  - If trigger fires when `|player.x - entity.pos.x| < size.x`: **half-extents**.
  - If trigger fires when `0 < player.x - entity.pos.x < size.x`: **full-extents from corner**.
  - If radial fire condition: **sphere**.
- Test rotation: face the trigger from a 45° angle and check whether the entity-space half-extents are in player-space or world-space (= whether the rotation applies).
- **Output:** documented semantics for each of the three trigger classes (`triggerbox`, `trigger_volume`, `triggeraoe`). Determines AABB vs OBB rendering in Phase 1.

### Phase 1 — Projection scaffold (cap: 2 hr)

Acceptance: a hardcoded AABB at world `(0,0,0)` with extents `(10,10,10)` renders as a wireframe box overlay on screen. Box stays attached to that world location as the camera moves. No flicker, no crash, no FPS impact > 1%. **All matrix reads + projection math are SEH-wrapped from this phase forward (raised from Phase 5 per audit feedback).**

- Implement `mtr::trigger_overlay::tick()` minimally.
- Read view from `0x00724C10` and proj via `current_proj()` accessor from Phase 0A. **Wrap both reads in `__try / __except (EXCEPTION_EXECUTE_HANDLER)`** with a clean abort path (no draw if either read faults). View matrix global is normally stable but the cost of the guard is one cycle per frame; benefit is correctness under any future engine churn.
- Project the 8 corners of the hardcoded AABB using D3D9 row-vector math:
  - `clip[i] = world_corner_row * View_matrix * Proj_matrix` (4-component row vector left-multiplied)
  - For initial scaffold, draw a SOLID 12-edge wireframe — no clipping yet (Phase 2 adds clipping). Just `clip /= clip.w` blindly. Boxes near the camera will tear; that's expected and confirms the projection pipeline is live.
- Use `ImGui::GetForegroundDrawList()->AddLine()` × 12 for the edges. NDC → screen with explicit Y-flip:
  - `screen_x = (ndc_x * 0.5f + 0.5f) * viewport_width`
  - `screen_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * viewport_height` (D3D9 convention; Y axis inverts vs OpenGL)
- Add menu toggle in tab_debug.
- **Validation gate:** user confirms visually that the box stays anchored at world origin as the camera moves around it.

### Phase 2 — Homogeneous parametric clip (cap: 1.5 hr)

Acceptance: AABBs entirely behind the camera don't draw at all. AABBs partially behind (any corner has `w <= 0` or `|x| > w` etc.) draw the visible portion correctly with no tearing, no NaN, no Inf, no impossible diagonal lines crossing the screen. Edge from in-frustum corner to out-of-frustum corner is clipped at the exact frustum boundary.

- **Algorithm:** homogeneous parametric clip (Cyrus-Beck variant), in clip space, BEFORE perspective divide. NOT Liang-Barsky (2D NDC algorithm) and NOT Cohen-Sutherland (region-code algorithm — wrong primitive). The textbook reference is the "homogeneous space clipping" section of *Computer Graphics: Principles and Practice* (Foley/van Dam) or the equivalent in any GPU pipeline reference.
- **Six clip planes** for D3D9:
  - `w + x >= 0` (left)
  - `w - x >= 0` (right)
  - `w + y >= 0` (bottom)
  - `w - y >= 0` (top)
  - `z >= 0` (near — D3D9 uses `[0, w]` for z, NOT `[-w, w]` like OpenGL)
  - `w - z >= 0` (far)
- **Per-edge clip:** for each of the 12 AABB edges with endpoints `A`, `B` in clip space, iterate the 6 planes. For each plane, compute distances `d_a = A · plane_eq`, `d_b = B · plane_eq`. Cases:
  - Both `d_a >= 0` and `d_b >= 0`: edge is inside this plane, continue.
  - Both `d_a < 0` and `d_b < 0`: edge is fully outside this plane, **discard** entire edge.
  - Mixed: compute `t = d_a / (d_a - d_b)`, clip the outside endpoint to `lerp(A, B, t)` (4-component lerp on the homogeneous coordinates — keep w!). Continue with the clipped edge.
- **Whole-box reject:** before per-edge work, check if all 8 corners share an outside half-space for any one of the 6 planes. If yes, skip the box entirely (cheap early-out — typical for off-screen triggers).
- **Critical safety:** never divide by `w` until after the clip survives all 6 planes. The bug to avoid is "skip endpoint with `w <= 0`" — that loses visible portions of any edge crossing the near plane. Use the parametric clip math above instead.
- **Numerical edge cases to test:**
  - Camera exactly at an AABB corner (one corner with `w == 0`): use `w <= epsilon` as the test, treat as outside.
  - Edge parallel to a clip plane (`d_a == d_b`): both should be on same side; if not, divide-by-zero is a real bug — fall back to "discard edge."
  - NaN inputs (would only happen if matrices are corrupt; SEH catches the matrix read but not arithmetic NaN). Defensive: NaN-check the clip values before drawing; skip edge if any.
- Reuse a 50-LOC reference implementation; this is well-trodden ground.

### Phase 3 — Entity walker (cap: 3 hr — depends on Phase 0C output)

Acceptance: `mtr::trigger_overlay::tick()` enumerates every active entity in the current level, filters to trigger classes, and draws each one's projected AABB. Validation: in a known mission level (e.g. Robinson manor), the number of visible boxes matches the count from the corresponding `.sc` file.

- Plug `for_each_entity(callback)` from Phase 0C into `tick()`.
- For each trigger entity: read `triggerSize` + position via property accessor (Phase 0D).
- Build the 8 corners using the rotation matrix at `+0x70` if Phase 0B says triggers are oriented; otherwise axis-aligned.
- Filter by per-class show toggles.

### Phase 4 — UX polish (cap: 2 hr)

Acceptance: per-class color picker works, per-class show/hide works, overlay shows trigger name at AABB centroid, FPS overlay shows "trigger overlay: N visible / M total".

- Color pickers in tab_debug.
- Centroid-projected text labels (small, semi-transparent so they don't clutter).
- Status line.

### Phase 5 — Stress + edge cases (cap: 1 hr)

(SEH guards already in place from Phase 1 + Phase 0C — this phase just exercises the system in adversarial conditions.)

- Test in 5+ different levels including mini-games and intermission screens.
- Confirm overlay handles level transitions cleanly (entity list churn during the load window). If the engine zeros the entity-manager list pointer mid-load, our SEH-guarded walker should bail; verify no crash. If false positives appear (overlay flickering during loads), gate the walker on `screen_push` events (skip enumeration during the screen transition window — one bool atomic).
- Confirm overlay handles freecam mode (per audit: should "just work" because we read from the live view-matrix global at 0x724C10 which freecam writes through). Verify visually.
- Document any classes that have weird behavior (e.g. nested triggers, dynamic-size triggers, event-spawned triggers that appear mid-mission).

---

## Risks

- **R1: Projection matrix isn't where Phase 0A first looks.** Mitigation tiered: 0x745AA0 (likely correct, per interp_halo.cpp evidence) → existing log lines from sprite_hooks.cpp → fall back to caching in hk_BuildProjMatrix POST. All three options are cheap; one will work.
- **R2: triggerSize is something other than half-extents.** Phase 0B catches this. We render the correct shape once we know the semantics.
- **R3: Entity walker corrupts on level transition.** Mitigation: SEH wrap; Phase 5 stress-tests transitions. Worst case: skip enumeration during the engine's scene-load window (we can detect via a screen-push event).
- **R4: Trigger classes have rotated AABBs (OBBs).** If `+0x70` rotation applies, we draw an OBB. Phase 0B confirms — implementation cost is the same (transform 8 corners by rotation before world-projection).
- **R5: Frustum clipping is mathematically wrong.** Use a known-good Liang-Barsky reference; unit-test with synthetic camera + box positions.
- **R6: Performance — 100+ trigger entities × 12 edges projected per frame at 240Hz.** Per-frame cost: ~100 × 8 matmuls = ~800 4×4 matrix-vector products = ~32K float ops. Trivial. Should be < 0.1ms.
- **R7: ImGui foreground draw list interaction with the engine's later draws.** Foreground draws AFTER the engine's 3D scene but BEFORE other ImGui windows. Overlay sits on top of the 3D world but under any open menu — that's the desired layering.
- **R8: AABB overlap during clipping produces visible "tearing."** Any two adjacent edges at a frustum boundary need to clip consistently. Risk is low; verified by Phase 2 acceptance.
- **R9: Entity property reads thread-safe?** Engine is single-threaded for sim/render per memory; EndScene runs on the same thread. Safe to read entity properties without lock.
- **R10: Multiple entity lists per level (active vs spawned vs dormant).** Mission-gated triggers might be inactive (not in main entity list) until their mission starts. Plan: walk the main list; if some triggers are missing, look for a secondary "spawn pool" list during Phase 5 stress-testing.

---

## Alternatives considered

- **A1: Hook the engine's `DrawDebugLine` script callback and call it ourselves.** Blocked: `DrawDebugLine` impl is referenced from a stolen-byte IAT thunk at 0x57DAB0; the static-disasm bytes are obfuscated and only resolve to real instructions after the unpacker runs. Calling the thunk directly requires runtime decoding which we haven't done. Could be done as a standalone RE project but isn't the bottleneck.
- **A2: Render the overlay via a custom D3D9 line list.** Possible. ImGui is simpler — already integrated, draws on top of everything, no per-frame device-state bookkeeping. ImGui wins.
- **A3: Use a 2D minimap projection (top-down).** Useful but doesn't match "with 3D projection" request. Could be a follow-up feature ("minimap mode") but not the primary path.
- **A4: Ship the engine's built-in `show_trigger` toggle by setting whichever cvar/script-flag enables it.** Blocked: `show_trigger` registration runs through a stolen-byte IAT thunk; can't easily flip it on without RE'ing the registration target. Same RE cost as A1.
- **A5: Read trigger boxes from .sc text files at boot rather than from runtime entities.** Pros: no entity walker needed. Cons: doesn't reflect dynamic state (despawned/triggered triggers); requires .sc parser. Worse complexity tradeoff than runtime entity walking.
- **A6: Render only the trigger CURRENTLY closest to the player.** Less visual clutter but loses overview. Could be a per-class option later.

---

## Open questions

(OQ1, OQ3, OQ4 closed by the audit pass.)

- **OQ1 — closed.** Projection matrix is most likely at `0x00745AA0` (already named `kHaloProjMatrixVA` in [interp_halo.cpp:96](../../src/mtr-asi/src/interp/interp_halo.cpp#L96)). Phase 0A confirms; if 0x745AA0 doesn't hold the main-camera matrix consistently, fall back to caching in `hk_BuildProjMatrix` POST with the existing aspect ~ 1.0 filter ([camera_hooks.cpp:60](../../src/mtr-asi/src/d3d9_hook/camera_hooks.cpp#L60)).
- **OQ2 — open.** Are mission-gated triggers spawned upfront or on-demand? Phase 5 stress-testing across 5+ levels will surface this.
- **OQ3 — closed.** Engine's HUD draws DURING the engine's render pass (before EndScene), so it's underneath when EndScene fires. Our ImGui foreground overlay draws after that, on top of HUD. Menu windows draw later in the same `ImGui::Render()` call so they sit on top of the overlay. This is the desired layering.
- **OQ4 — closed.** Freecam works "for free" because the overlay reads from the live view-matrix global at `0x00724C10` which freecam writes through in its `apply_to_globals` POST hook ([freecam.cpp:809](../../src/mtr-asi/src/freecam.cpp#L809)). Same projection global reading. Phase 5 acceptance verifies visually.
- **OQ5 — out of scope.** Trigger property editing in-game (drag-to-move, resize handles). Documented for a future feature; not part of this plan.
- **OQ6 (NEW from architecture audit) — design call:** if the overlay is later extended to non-static entities (mobile NPCs / actors), reading `+0x58` during EndScene pulls the live-modified position from M4's interp save/write/restore fence ([interp_player.cpp](../../src/mtr-asi/src/interp/interp_player.cpp)) — the interpolated render position, NOT the sim-tick position. For static triggers this is moot. For future entity overlays, decide whether to read the sim position (pre-interp) or the render position (interpolated). Either is defensible.

---

## Audit history

- **v1** (2026-05-09): original draft.
- **v2** (2026-05-09, this revision): incorporated independent architecture audit (`a96cd28b`) and engineering audit (`aa720b80`). Both audits independently caught: (1) projection matrix location is already known at 0x745AA0 (interp_halo.cpp), don't reinvent; (2) entity walker pattern already exists in interp_npc.cpp. Engineering audit additionally caught a critical correctness bug in the v1 clipping description (skip-w<=0 was wrong; correct algorithm is homogeneous parametric clip with `t = w_a / (w_a - w_b)` lerp at the near plane crossing). Architecture audit caught a sequencing bug (overlay tick must run inside `on_end_scene` after `ImGui::NewFrame`, not before).

---

## Phase 0 outcome (empty until executed)

(Populated as Phase 0 progresses.)
