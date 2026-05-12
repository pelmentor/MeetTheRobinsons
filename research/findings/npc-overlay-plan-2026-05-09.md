# NPC visual debug overlay — projection of NPC data plan

**Date:** 2026-05-09 evening
**Revision:** v2 (post-audit; architecture audit `aaab9a06`, engineering audit `aea25030`)
**Governing rule:** RULE №1 — proper fix, no shortcuts. Reuse existing scaffolds where they exist; build new ones where they don't.

**v2 changes from v1:**
- **Walker offset corrected: `+0x5C` (NOT `+0x40`) is the entity pointer.** Both audits independently caught this. Evidence: `interp_npc.cpp:115-117` reads pos/rot through `inner = *(node + 0x5C)` using `+0x58`/`+0x70`; `freecam.cpp:188` defines `kTransformNodeSubjectOffset = 92 = 0x5C` and the comment says "subject (= entity ptr)". `+0x40` is used only as a player-handle for filtering, never as a target for pos/name reads. Phase 0A is now a verification of `+0x5C` (not a tie-breaker between two candidates).
- **Frustum culling: full 6-plane point test, not just `clip.w > epsilon`.** Off-screen NPCs (in front of camera, but outside the lateral or vertical frustum) would otherwise project to absurd screen coords and waste CPU on AddText. 6-plane test mirrors the trigger overlay's existing math.
- **`+0x50` name read needs stronger validation.** `is_safe_to_read` only checks page mapping. The full validator: dereference → check target page is COMMITTED + READ + not in .text/.rdata → first byte 0x20-0x7E printable → null terminator within 128 bytes. Mirrors the validator already in `widget_probe.cpp::extract_string`.
- **SEH guard wraps the entire per-NPC body, not just one read.** Mid-destruction entities can have valid pointer at `+0x48` but freed sub-component → fault on `*(+0x48)+0x10`. One try/except per iteration is the minimum safe scope.
- **Shared math header `include/mtr/overlay_math.h`** factors out `row_mul`, `mat_mul`, `plane_dist`, the 6-plane point-in-frustum test, and `ndc_to_screen` from `trigger_overlay.cpp` so `npc_overlay` and any future world overlay share one source of truth. Duplicate inline math in two TUs is RULE №1-violating; will silently diverge when one module fixes a sign and the other doesn't.
- **`*(entity+0x48)+0x10` is the PRIMARY pos source for character entities** (not a fallback). The renderer reads it for the visible model; M4/M5 interp writes it during throttle for visual-anchor consistency. `+0x58` is the secondary fallback only when `+0x48` sub is NULL (non-character entities).
- **`cam_pos` extraction from world matrix**: read row 3 directly (`world[12..14]`). World at `0x00724C50` is `inverse(view)` for rigid LookAt-RH; row-3 IS cam_pos. Avoids the view-rotation-transpose math.
- **`any_draw` flag (`menu.cpp:578`) must OR-in `npc_overlay::enabled()`** — same pattern as the trigger overlay shipment. ImGui::Render() doesn't run otherwise when the overlay is the only enabled UI.
- **`npc_overlay::tick()` placed between `trigger_overlay::tick()` and `ImGui::EndFrame()`** at `menu.cpp:1171-1173`. Same lifecycle constraint as trigger overlay (foreground draw list requires live frame).
- **Cross-system race resolved:** sim_decouple writes interp pos to entity `+0x58`/`+0x48+0x10` BEFORE `menu::on_end_scene` runs. When throttling, `npc_overlay::tick()` reads interp pos — which is the correct visually-consistent value. Not a race; documented as intentional ordering.
- **Phase 2 anim read needs class guard.** `+0x158` is verified for the Player class only. Other entity classes (props, particles, FX) don't have anim state at the same offset. Phase 2 gates the read on a vtable-whitelist or `+0x04` flags check.
- OQ3 closed (use world[12..14]), OQ4 closed (per the architecture audit, gate independently on `pick_mode()` flags, no shared dispatcher needed).

---

## TL;DR

Render an in-world debug overlay that shows per-NPC info (name, position, distance, animation state, registry properties) projected onto the screen as text labels at each NPC's world location. Sister overlay to the trigger-box overlay shipped earlier today; reuses the projection scaffold, the matrix capture, the autonomous-validation pipeline, and the SEH-guard idioms. Targets the existing transform-list walker pattern in [interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100).

Useful for: debugging NPC scripts (which AI state is each NPC in?), correlating mission triggers with the actors that drive them (which NPC's scripted callback fires when?), tuning camera follow targets (which NPC is the script attaching the camera to?), and finding the entity behind a visible model glitch (look at where the "glitched" position is vs the engine's reported position).

---

## Why we want it

- The engine is event-driven and script-heavy. Without an overlay, debugging "why did this NPC stop walking" or "why is the wrong character speaking" requires console logs that you have to correlate by hand.
- We already have the projection scaffold (`trigger_overlay`), the matrix capture (`0x724C10` view + `0x745AA0` proj), the homogeneous parametric clip, and the autonomous validation pattern shipped today. Adding the NPC channel is mostly composition, not new RE work.
- The transform-list walker pattern is already proven safe in production code ([interp_npc.cpp::walk_transform_list](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100)) — SEH-guarded, hard-iteration-cap, offset-based traversal.

---

## What we know already

### Transform-list walker (proven, in production)

Source: [interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100). Walks `dword_724DE4` (the transform list head). Per-node offsets:
- `node+0x04` → next pointer
- `node+0x40` → outer entity ptr
- `node+0x44` → flags (`0x10` = skip bit set by our `entity_transform_tick` hook for the player MMB-tp hold)
- `node+0x5C` → inner (sub-component the anim writes pos/rot into)

**For overlay purposes** the `node+0x40` "outer entity" is the right one — it's the logical NPC pointer with `name@+0x50`, `pos@+0x58`, `rotation@+0x70`. The `inner` (`+0x5C`) is the render sub-component and would be correct only if we want render-side interpolated pos.

### Entity layout (from research/findings/entity-system.md + player-entity-layout-2026-05-09.md)

Confirmed across multiple entity types:
- `entity+0x00` → vtable
- `entity+0x14` → entity_id (registry hash key in `dword_7427C0`)
- `entity+0x48` → render sub-component pointer (renderer reads `*(sub)+0x10` for pos)
- `entity+0x50` → **entity name string** (heap-allocated ASCII; e.g. `"player\0"`, `"Avatar\0"`)
- `entity+0x58` → **vec3 game-logic pos** (read by AI/scripts, written by `entity_transform_tick`)
- `entity+0x70` → float[9] rotation 3×3
- `entity+0x158` → animation state pointer (animated entities only — verified for player)
- `entity+0x410` → character metadata (character entities only)

The `+0x50` name is the killer feature for the overlay: it's a stable, human-readable string we can show next to each NPC.

### Property accessor (kv_get)

`entity_get_kv` at `0x4B8F00` — 150+ callers, reached via stolen-byte IAT thunk (destination decompilable per memory). Per memory: takes `(entity, name_hash)`, returns value-offset or NULL. Uses Jenkins lookup2 hash (same hash function as `sub_581420` script-callback registration uses).

### Animation system

[research/findings/animation-system.md](animation-system.md):
- `anim_update_all_tracks` at `0x4E4B70` — per-frame entry
- Per-controller layout: `+0x00` time, `+0x04` end, `+0x08` start, `+0x0C` rate, `+0x10` loop_mode, `+0x14` flags
- Track tree with parent-first recursion; channel 0 is hard-wired to scene visibility
- Hardcoded 0.003s step (333Hz)

What's NOT yet RE'd:
- The mapping from anim controller → "human-readable anim ID / name" (controllers are pool-stored at `unk_726D5C`)
- Per-NPC: which anim controller index is currently driving them?

This means Phase 1 can show pos/name immediately but anim state requires Phase 2 RE work.

### Existing reusable infrastructure (today's earlier shipments)

- **`trigger_overlay.cpp`** — projection scaffold + homogeneous parametric clip + SEH-guarded matrix reads + ImGui foreground draw + autonomous export path + UI toggle. The whole pipeline.
- **`tools/run-test.ps1`** + **`validate-overlay-frames.ps1`** — autonomous validation framework.
- **`test_harness.cpp`** — `tick_overlay_phase1_verify` scenario as a template; same pattern works for `tick_npc_overlay_phase1_verify`.

---

## Architecture

```
                    EndScene PRE hook
                            │
                            ▼
         menu::on_end_scene → ImGui::NewFrame()
                            │
                            ▼
             ┌──────────────────────────────────┐
             │  trigger_overlay::tick(dev)      │  (already shipped)
             │  npc_overlay::tick(dev)          │  (NEW — sibling module)
             └──────────────────────────────────┘
                            │
       ┌────────────────────┴────────────────────┐
       │                                         │
       ▼                                         ▼
  Read view + proj                  walk_transform_list:
  via SEH-guarded                     for each (node, entity):
  reads at fixed VAs.                   - read entity name (+0x50, char*)
                                        - read entity pos (+0x58, vec3)
                                        - read entity rot (+0x70, mat3)
                                        - (Phase 2) read anim state (+0x158)
                                        - (Phase 3) read kv properties via kv_get
       │                                         │
       └────────────────────┬────────────────────┘
                            ▼
                For each visible NPC:
                   - Project pos via row_vec * View * Proj (clip space)
                   - Whole-point reject if behind camera (w <= eps)
                   - NDC → screen with D3D9 Y-flip
                   - Draw label via ImGui::GetForegroundDrawList()->AddText()
                   - Optional: small dot/icon at the projected screen pos
                   - Optional: distance-based alpha fade
                            ▼
                  ImGui::Render() (in on_end_scene's tail)
```

### Module layout

- `src/mtr-asi/src/npc_overlay.cpp` — new module, ~400 LOC
- `include/mtr/npc_overlay.h` — public API: `enabled()/set_enabled()/tick(dev)` + per-field show toggles + autonomous export
- **Call site:** INSIDE `mtr::menu::on_end_scene(dev)`, alongside `trigger_overlay::tick(dev)`. Same lifecycle constraints (after NewFrame, before Render).
- **Matrix source:** read view + proj via the same fixed-VA path that `trigger_overlay` uses. (If `trigger_overlay` later moves to capture-via-hook, `npc_overlay` migrates with it.)
- UI toggle in [tab_debug.cpp](../../src/mtr-asi/src/menu/tab_debug.cpp), section "NPC overlay".

### Public API sketch

```cpp
namespace mtr::npc_overlay {

bool     enabled();
void     set_enabled(bool v);
int      visible_npc_count();   // last-frame count for UI status

// Per-field show toggles. Each field is rendered conditionally; cheap
// when off (skipped per-NPC at render time).
bool show_name();        void set_show_name(bool v);
bool show_pos();         void set_show_pos(bool v);
bool show_distance();    void set_show_distance(bool v);
bool show_anim_state();  void set_show_anim_state(bool v);  // Phase 2
bool show_kv_dump();     void set_show_kv_dump(bool v);     // Phase 3

// Distance fade — labels fade out beyond this distance. 0 = always opaque.
float distance_limit();  void set_distance_limit(float v);

// Click-to-pin: lock a single NPC into a sticky panel that doesn't move
// with the NPC. Click an NPC label to pin; click the pin badge to unpin.
// Phase 3 feature.
int  pinned_entity_id(); void set_pinned_entity_id(int id);

// Autonomous-validation export (mirrors trigger_overlay::set_export_frames).
void set_export_frames(int n);
int  export_frames_remaining();

// Called from INSIDE mtr::menu::on_end_scene, after ImGui::NewFrame() and
// before ImGui::Render().
void tick(IDirect3DDevice9* dev);

} // namespace mtr::npc_overlay
```

---

## Phase plan

### Phase 0 — Fact-finding (cap: 1 hr direction-eval)

Acceptance: every "Unverified" item is closed with a written outcome.

#### 0A — Walker offset confirmation (cap: 15 min)
- Existing code says `node+0x40 = entity`, `node+0x5C = inner`, `node+0x44 = flags`. Verify on a live frame: log all three for the player's transform-node and compare to what `freecam.cpp::find_player_transform_node` finds (which uses `node+0x5C` as the subject offset).
- **Output:** confirmed walker offsets + which one to use for "the logical NPC pointer" (lean: `+0x40`).

#### 0B — Name read safety (cap: 15 min)
- For each entity returned by the walker, read `entity+0x50` as `char**`. Validate via `is_safe_to_read` (already implemented in widget_probe). Log first 16 entities + their names on the first armed frame.
- **Output:** confirmed that `+0x50` is consistently a heap pointer to ASCII for EVERY entity in the transform list (or the subset of types where it isn't, so we can filter).

#### 0C — Anim controller index per entity (cap: 30 min) — Phase 2 prerequisite
- Read `entity+0x158` for the player. Cross-reference with `unk_726D5C` (the anim pool head). Determine: is `+0x158` an index into the pool, or a direct controller pointer?
- **Output:** the offset chain from entity → current anim controller → time/rate/loop_mode. If this is straightforward, Phase 2 ships next; if it's stolen-byte-thunked, we ship Phase 1 first and let Phase 2 take its time.

### Phase 1 — Name + Pos + Distance overlay (cap: 3 hr)

Acceptance: with NPCs in scene, each one renders a text label at its world position showing its name + pos + distance. Labels follow the NPCs as the camera and NPCs move. Off-frustum NPCs don't render. Performance: < 1 fps drop with 50+ NPCs visible.

- Implement `mtr::npc_overlay::tick()`:
  - Read view + proj (SEH-guarded) — reuse `trigger_overlay`'s pattern.
  - Walk transform list (reuse `interp_npc.cpp::walk_transform_list` template; copy into `npc_overlay.cpp` as a private helper or factor into a shared header).
  - For each entity:
    - SEH-guard around the entity-pointer-read (defensive — entity might be mid-destruction)
    - Read pos `+0x58`, name `+0x50` via `is_safe_to_read`-style guards
    - Project pos to clip space via `row_mul(world_pos_row_4d, view_proj)`
    - Skip if `clip.w <= epsilon` (behind camera) — single-point reject is OK for labels (unlike box edges, you can't "partially see" a label)
    - NDC → screen with D3D9 Y-flip
    - Compute distance = `length(pos - cam_pos)` where `cam_pos` is extracted from the view matrix (negate translation row through rotation transpose)
    - Build label string per active toggles
    - `ImGui::GetForegroundDrawList()->AddText(...)` at the projected screen pos
    - Optional: `AddCircleFilled` 3px dot at exact projected pos for "anchor" indicator
- UI toggle in tab_debug
- **Acceptance gate:** user confirms visually; ALSO `npc_overlay-phase1-verify` autonomous scenario asserts walker enumerates >= 1 entity on the main menu without crash (main menu has no real NPCs but the walker should not crash on the empty / minimal list).

### Phase 2 — Animation state (cap: 4 hr) — depends on Phase 0C outcome

Acceptance: each NPC's label shows the current anim controller's time + rate (e.g. `t=12.34 r=1.0 loop=clamp`). Optionally the anim "name" if we can map controller index back to a string identifier.

- Read `entity+0x158` (anim state pointer). Follow to the active controller. Read time/rate/loop_mode from the controller layout `+0x00/+0x0C/+0x10`.
- If the controller has a debug-name field (TBD via Phase 0C), display it.
- If we can find the anim CURVE name (track tree → curve → curve name) we display that instead of just the controller state.

### Phase 3 — kv_get registry dump on selected NPC (cap: 3 hr)

Acceptance: clicking an NPC label pins it into a sticky panel showing all the property keys + values from `entity_get_kv`. Pin survives camera moves and NPC movement. Click again to unpin.

- Click detection: each NPC's projected screen-space rect is hit-tested against ImGui mouse coords. (ImGui input under DI-exclusive needs the WH_MOUSE_LL hook that `sprite_picking` already uses.)
- Once pinned: enumerate likely property keys (a static list of common ones from `research/findings/entity-system.md` — `position`, `health`, `currentAnim`, `aiState`, etc.). For each: hash via Jenkins lookup2, call `entity_get_kv`, deref + decode.
- Render in a draggable, semi-transparent ImGui window.

### Phase 4 — UX polish (cap: 2 hr)

- Distance fade (alpha by distance to limit)
- Class filter (whitelist by entity-class string match against `+0x50` name regex or vtable VA filter)
- Off-screen NPCs: small edge indicators showing direction to the nearest off-screen NPC (optional, debatable utility)
- Color coding: hostile NPCs red, neutral yellow, friendly green (requires reading aiState / faction property — depends on Phase 3)

### Phase 5 — Stress + edge cases (cap: 1 hr)

- Test in 5+ levels with varying NPC counts (mini-games, intermissions, bossfights, etc.)
- Verify SEH guards fire correctly when an NPC is mid-destruction
- Confirm walker handles level transitions cleanly
- Confirm overlay handles freecam mode
- Performance check: 200+ NPCs scene, < 2 fps drop

---

## Risks

- **R1: `+0x50` is not consistently a name pointer.** Mitigation: Phase 0B verifies on first launch. If only some classes have it, filter by class (vtable VA whitelist) and skip the rest with no label.
- **R2: Walker-offset disagreement** (`+0x40` vs `+0x5C`). Phase 0A closes by direct comparison.
- **R3: Anim state behind a stolen-byte thunk.** Phase 0C verifies. If yes, defer Phase 2; ship Phase 1+3+4 first.
- **R4: Click detection unreliable.** Phase 3 needs `sprite_picking` integration; same DI-exclusive issue as the per-element UI controls. Already solved in [sprite_picking.cpp](../../src/mtr-asi/src/sprite_picking.cpp).
- **R5: Performance with 200+ NPCs.** Per-NPC cost: 1 matrix-vec + 1 string format + 1 ImGui AddText. Estimated 200 × ~5µs = 1ms. At 240Hz that's 24% of frame budget — acceptable, but if it bites we add per-frame caching of formatted strings (rebuild only when value changes).
- **R6: Label clutter.** With 50+ NPCs visible the screen turns to label soup. Mitigation: distance fade (R4 in trigger overlay), per-class filter, and "show only visible" toggle that skips labels currently obscured by geometry (would need a depth-buffer read — heavy; defer).
- **R7: NPC `+0x58` pos is not where the visible model is.** The renderer reads `*(entity+0x48)+0x10` for character entities. Labels would appear at logic-pos which can drift from render-pos by an animation cycle. Mitigation: prefer `*(entity+0x48)+0x10` if non-NULL, fall back to `+0x58`.
- **R8: Click-pin conflicts with sprite_picking.** The picking system already grabs WH_MOUSE_LL events. If both are active, contention. Mitigation: route clicks through a single dispatcher with priority order (sprite picking when in per-element UI mode; npc picking when overlay is on).

---

## Alternatives considered

- **A1: Engine's `DrawDebugLine/Sphere/Point` callbacks.** Same blocker as the trigger overlay: stolen-byte IAT thunks. Out of scope until/unless we decode them.
- **A2: Single combined "world overlay" module.** Trigger boxes + NPC labels in one module. Considered but rejected — different rendering primitives (wireframe vs text), different update cadences (triggers static / NPCs moving), different filter surfaces. Two sister modules sharing a projection-math header is cleaner.
- **A3: Render labels via in-engine 2D HUD instead of ImGui.** No — ImGui is the right tool here. The HUD is engine-native and we'd be fighting the sprite_xform pipeline.
- **A4: Pre-bake NPC info from `.sx`/`.sc` files.** Static data only; doesn't reflect live state (current anim, current pos, current AI state). Useless for the actual debugging use case.

---

## Autonomous validation

Pattern: same as `overlay-phase1-verify` shipped today. New scenario `npc-overlay-phase1-verify`:

1. Boot to main menu (existing dismissal logic)
2. Settle 60 frames
3. Enable npc_overlay + show_name + show_pos
4. Arm export 60 frames
5. Each frame: log `NPC_OVERLAY_FRAME_BEGIN`, `NPC_OVERLAY_VIEW`, `NPC_OVERLAY_PROJ`, then per visible NPC: `NPC_OVERLAY_NPC f=N idx=K name="..." pos=X,Y,Z screen=SX,SY` (or `clipped` if behind camera)
6. After export drains, take screenshot, pass

Validator script `tools/validate-npc-overlay-frames.ps1`:
- Re-projects each logged NPC pos via the logged matrices
- Asserts the logged screen-space coord matches the recomputed value within 0.5px
- Asserts entity name strings are non-empty and ASCII

**Caveat:** main menu has no NPCs in the gameplay sense. The autonomous test asserts the walker doesn't crash and either yields zero entities or yields entities whose name reads succeed. The visual validation (NPCs actually labeled correctly) requires loading a save game with NPCs — out of scope for autonomous overnight; user verifies visually.

---

## Open questions

- **OQ1:** Is the player's transform-list node enumerated by `walk_transform_list` (per `interp_npc.cpp` we explicitly skip the player via `entity != player`)? For overlay we WANT to include the player. Resolved: pass `player=nullptr` to the walker (already its default).
- **OQ2:** Are there entities that should NOT be labeled (UI widgets, particle emitters, audio listeners)? Probably yes. Phase 0B's first-launch dump tells us; Phase 4 adds a class filter to hide them.
- **OQ3:** Distance from camera — extract from view matrix's translation row * rotation^T? Or from the world matrix at `0x724C50` (translation row directly)? Likely the latter — world matrix's row 3 is `cam_pos` directly. Verify in Phase 1.
- **OQ4 — answered by trigger overlay session:** click-to-pin under DI-exclusive needs the WH_MOUSE_LL bridge. Already proven; Phase 3 reuses.

---

## What the audit needs to evaluate

1. **Architecture audit** — Module fit. Should `npc_overlay` and `trigger_overlay` share a header for the projection math, or should the math be inlined in each? Anything in the existing project that already does NPC enumeration that I should reuse instead of duplicating? Cross-module concerns (interp_npc.cpp may interp the same nodes we read — race?).
2. **Engineering audit** — Performance estimate (200 × matvec + string format + AddText at 240Hz). SEH-guard placement. Walker-offset confusion (`+0x40` vs `+0x5C`). Phase 0C anim RE feasibility from existing primitives. Any reason ImGui::AddText is the wrong API choice for this volume of text?

---

## Phase 0 outcome (empty until executed)

(Populated as Phase 0 progresses.)
