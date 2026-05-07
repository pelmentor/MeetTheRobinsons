# UI granularity — engineering plan

How to make the working UI override (`mtr::sprite_matrix`) granular so it
applies to fullscreen menus but leaves HUD untouched. Ships in two
phases: Phase 2 (refactor existing rules-based path, ~2 days) and
Phase 3 (per-sprite classification, ~2-3 weeks of RE).

## Status checkpoint (2026-05-06)

### Phase 1 — DONE

Dead-code cleanup completed and deployed:
- `mtr::aspect_ui` namespace removed (was dead storage; no real consumer)
- Narrow-FOV branch in `hk_BuildProjMatrix` removed (UI hypothesis ruled out by logs)
- HUD-pillarbox branch in `hk_BuildOrtho` reduced to diagnostic logging only
- Menu UI section "UI aspect — default" removed
- ~150 LOC of dead code eliminated; build verified clean
- `mtr-asi.asi` deployed to `Game/`

### What works (confirmed via runtime)

| Module | What it does | Status |
|---|---|---|
| `mtr::aspect` | World camera aspect via `hk_BuildProjMatrix` substitution | Works for 3D world |
| `mtr::fov` | World camera FOV override | Works |
| `mtr::sprite_matrix` | Sprite-batcher matrix (sx/sy/tx/ty) override | **Works for ALL UI (menus + HUD shared)** |
| `mtr::ui_aspect_rules` | Per-screen rule table (substring match) | Consumed by `sprite_matrix::auto_from_rules` |
| `mtr::screen_push` | Screen-stack mirror (push/pop tracking) | Works |

### The granularity problem

`render_sprite_batcher` (sub_4E8D30) renders **every UI sprite** in one
call per frame using ONE projection+view matrix. Both fullscreen menu
sprites AND HUD sprites share that matrix. We cannot differentiate them
at the matrix-application level.

User observation (2026-05-06): when `sprite_matrix` override is active
(via manual sliders or `auto_pillarbox to 4:3` button), HUD elements
("PICKUP TEXT", "Mission Text", button hints) get centered along with
fullscreen menus. They want fullscreen menus pillarboxed but HUD left
alone.

## Phase 2 — Time-gated override via screen-stack

**Approach**: use the existing `auto_from_rules` path. When a fullscreen
menu screen is on top of the stack, apply pillarbox; otherwise no
override. HUD-only state typically has no menu screen on top, so HUD
stays untouched.

**Cost**: ~80 LOC delta. Mostly UI/UX improvement; the underlying
mechanism already exists.

**Limitation (acceptable for v1)**: this assumes mutual exclusion
between fullscreen-menu state and HUD-visible state. Verified true for
Wilbur's main flow:
- Main menu → no HUD
- Pause menu → covers HUD entirely
- In-game → only HUD, no menu screen on stack

Edge case where it fails: an overlay screen that shows alongside HUD
(e.g., notification popup). Phase 3 addresses this properly.

### Phase 2 deliverables

#### P2.1 — Pre-populate default rules at install

In `mtr::ui_aspect_rules::install()` (new function), populate the rules
table with sensible defaults when empty:

```cpp
void install_defaults() {
    if (rule_count() > 0) return;  // user-configured; don't clobber
    add_rule("MainMenu",   1.333f);   // 4:3 pillarbox
    add_rule("Pause",      1.333f);
    add_rule("Loading",    1.333f);
    add_rule("Options",    1.333f);
    add_rule("Help",       1.333f);
    add_rule("Inventory",  1.333f);
    add_rule("Map",        1.333f);
    add_rule("Cheats",     1.333f);
    add_rule("Title",      1.333f);
    add_rule("Background", 1.333f);
    add_rule("MiniHamster",1.333f);   // mini-game menus
    add_rule("DigDug",     1.333f);
    add_rule("ChargeBall", 1.333f);
    add_rule("AFViewer",   1.333f);
    // Patterns not matched -> 0.0 (no override) -> HUD stays untouched.
}
```

The `1.333f` value is the TARGET ASPECT (4:3). The actual factor is
computed each frame in `sprite_matrix::auto_from_rules` mode as
`target_aspect / screen_aspect` — so rules work on any monitor.

Rules are user-editable (existing UI). Defaults are set once on first
install and only when the rules table is empty (no user state to
clobber).

#### P2.2 — Quick-setup button

Single-click "Pillarbox menus to monitor aspect" button in the UI:
1. Enable `sprite_matrix`
2. Enable `auto_from_rules`
3. Reset rules to defaults (or keep user's if any)
4. Reset manual sliders to 1.0 (so they don't interfere)

Result: typical user gets the working config in one click.

#### P2.3 — Promote `auto_from_rules` as primary mode

UI restructure of the "sprite-batcher matrix override" section:

```
[x] UI aspect override (recommended)
   |
   |  Mode: (•) Auto from screen rules    ( ) Manual sliders (advanced)
   |
   |  [Quick-setup: pillarbox menus]
   |
   |  Status:
   |    Current top screen: ScreenWilburMainMenu
   |    Matched rule:       "MainMenu" -> 1.333 (4:3)
   |    Active factor:      0.750 (4:3 in 16:9)
   |    Override:           ACTIVE
   |
   |  > Rules editor (collapsing)
   |  > Manual sliders (advanced, collapsing)
```

Manual sliders move into a collapsing-header sub-section labeled
"Advanced — manual matrix override". Default: closed.

Auto-from-rules is the canonical primary mode. Manual is
diagnostic / RE-time-only.

#### P2.4 — Live diagnostic display

Always-visible in the UI override section (when override is enabled):
- Current top screen name (large, colored)
- Stack depth + full stack listing (collapsible)
- Matched rule (which pattern won) + the resolved aspect
- Computed factor (with formula breakdown)

User can see exactly what's happening at any moment. No magic.

#### P2.5 — "Add rule for current screen" button

When a screen is on top, a button below the rules table:
```
[+ Add rule for "ScreenWilburMonoRailHUD" → 0.0 (no override)]
```

One-click to add an opt-out rule for whatever's on top right now. Used
when user discovers a screen that shouldn't be pillarboxed.

#### P2.6 — Rules persistence (optional, P2 stretch)

Save rules table to `Game/mtr-asi-ui.ini` so user config survives
restarts. Load on `mtr::ui_aspect_rules::install()` before populating
defaults.

INI format:
```
[ui_rules]
rule_0 = MainMenu      = 1.333
rule_1 = Pause         = 1.333
rule_2 = MonoRailHUD   = 0.0
default_aspect         = 0.0
```

Hand-editable. Standard `WritePrivateProfileString` / `GetPrivateProfileString`.

### Phase 2 ship plan

1. P2.1 — defaults (~20 LOC)
2. P2.2 — quick-setup button (~30 LOC)
3. P2.3 — UI restructure (~50 LOC, mostly moving things around)
4. P2.4 — live diagnostic (~40 LOC)
5. P2.5 — add-rule button (~20 LOC)
6. P2.6 — INI persistence (~80 LOC, optional)

Total: ~160 LOC (with P2.6) or ~80 LOC (without). Realistic 2-3 evenings.

### Phase 2 acceptance tests

Manual test protocol:
1. Fresh `mtr-asi.asi` (no saved rules) → defaults populated → quick-setup → main menu pillarboxed correctly
2. Start game → top screen has no menu rule → HUD untouched
3. Pause → "Pause" matches → menu pillarboxed
4. Resume → HUD untouched again
5. Mini-hamster game → "MiniHamster" matches → mini-game menu pillarboxed
6. User adds rule for `ScreenWilburMonoRailHUD` → 0.0 → that screen no longer overrides
7. Restart → rules persist (if P2.6 shipped)

## Phase 3 — Per-sprite classification

**Approach**: hook the sprite batcher itself, walk its sprite list,
classify each sprite (menu vs HUD), apply different transforms per
sprite class. Solves the limitation in Phase 2.

**Cost**: ~2-3 weeks of focused work. Most of it is RE; some is
implementation.

**Why this is "weeks of work"**: the sprite batcher's data structures
are mostly opaque from static analysis. We need:
- RE the linked-list entry layout at `unk_7271E8`
- Identify how each sprite is tagged (parent screen ptr? type byte? texture-based heuristic?)
- Determine if sprites can be selectively re-batched with different transforms
- Implement a 2-pass batcher hook
- Validate across all UI states (main menu, in-game HUD, pause overlay, mini-game)

This is comparable in scope to a single high-fps-decoupling milestone.
We don't ship Phase 3 in v1 of the granularity work — Phase 2 is the
"good enough" v1.

### Phase 3 milestones

#### M3.1 — RE the sprite list (~3-5 days)

Goal: full layout of `unk_7271E8` linked list entries. Fields needed:
- Position / size of the sprite
- Texture / material reference
- Parent / owner pointer (Screen? entity?)
- Type / category byte if exists
- Vertex stream layout

Approach:
- Decompile `render_sprite_batcher` (sub_4E8D30) in detail
- Trace each field access in the inner loop
- Find writers of the list (where sprites get added)
- Cross-reference with Screen render paths

Deliverable: documented struct in `research/findings/sprite-list-layout.md` + IDA renames.

#### M3.2 — Identify menu-vs-HUD signal (~3-5 days)

Goal: a per-sprite predicate `is_fullscreen_menu(sprite)` that's
reliable across all UI states.

Hypotheses (in order of feasibility):
1. **Owner pointer**: each sprite has a back-pointer to its Screen
   instance. Check if the owner is a known fullscreen-menu screen.
2. **Texture path**: menu sprites use textures from `Frontend/`,
   `screens/` paths; HUD uses `weapons/`, `objects/`, etc. Heuristic
   classification by texture name.
3. **Z-order or layer flag**: some flag in the sprite indicating
   "on-screen-overlay" vs "fullscreen UI".
4. **Position-based**: HUD elements typically anchor to screen edges;
   menus are full-screen. Less reliable but a fallback.

Whichever signal works gets implemented as the classifier. Multiple
signals can be combined for robustness.

#### M3.3 — Two-matrix batcher hook (~3-5 days)

Goal: hook `render_sprite_batcher`, walk the sprite list, apply
transforms to two groups separately.

Implementation strategy:
- Hook batcher entry. Save current transforms.
- Walk sprite list once: tally group counts.
- If only one group present: pass-through (no override).
- If both groups: split the rendering into two passes:
  1. First pass: render menu sprites with override matrix
  2. Second pass: render HUD sprites with identity matrix (no override)
- Restore original transforms on exit.

Risk: the batcher may not be split-safe (vertex buffers, state machines
inside). Need to verify.

Fallback if split rendering fails: do per-sprite vertex rewriting
(transform vertex positions before batcher sees them). Slower but
robust.

#### M3.4 — Cross-state validation (~3-5 days)

Test every UI state systematically:
- Main menu (only menu sprites)
- In-game (only HUD sprites)
- Pause overlay (HUD + menu both visible — the proof state)
- Mini-games (separate sprite paths possible)
- Cutscenes (special UI states)
- Loading screens
- Error / popup dialogs

Each state has a known correct visual; verify each looks right with
classification active.

#### M3.5 — Polish + ship (~2-3 days)

- Remove debug logging
- Performance profiling (extra sprite walk + classification cost)
- UI: per-class toggle (override menus / override HUD / override both)
- Documentation: how the classifier works, edge cases, troubleshooting

### Phase 3 ship criteria

- All 7 UI states render correctly with classifier active
- Pause-overlay state shows menu pillarboxed AND HUD untouched simultaneously
- No measurable FPS regression
- Failure modes: classifier returns unclear → defaults to "treat as HUD" (safer — leaves it untouched)

### Phase 3 risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| Sprite list layout opaque even after RE | LOW | HIGH | Use position/texture heuristics as classifier (M3.2 fallback); accept worse classification |
| Batcher not split-safe (vertex buffer state) | MED | HIGH | Pre-batcher vertex rewriting fallback (M3.3) |
| Classifier mis-categorizes one screen | MED | LOW | UI override per-screen; user can manually flag |
| Performance regression | LOW | MED | Profile before/after; cap sprite-list walk overhead |
| Mini-games use different batcher | LOW | MED | M3.4 explicit testing; fall back to Phase 2 in mini-games |

## Why both phases ship together

Phase 2 alone gives users 90% of what they want: typical Wilbur play
(menu / in-game / pause) does not have menu+HUD coexistence.

Phase 3 covers the remaining 10% (overlay screens like notifications,
hint popups during gameplay) and is the technically-correct solution.

Phase 2 is shippable in 2-3 evenings; Phase 3 is a 2-3 week project. So
we ship Phase 2 first as a solid v1, then iterate on Phase 3.

## Architecture invariants

After both phases ship, the UI override architecture is:

1. **`mtr::sprite_matrix`** — owns the matrix override values
   (sx/sy/tx/ty per call) and the enable toggle
2. **`mtr::ui_aspect_rules`** — owns the per-screen rules table
3. **`mtr::screen_push`** — owns the screen-stack mirror
4. **`hk_matrix_set_via_xform_a/b`** — applies the override at the right
   matrix-builder call
5. **(Phase 3 only) `mtr::sprite_classify`** — owns the per-sprite
   menu-vs-HUD classifier

The hook surface is minimal (2 hooks for matrix + 1 hook for batcher in
Phase 3). All UI state is observable through the menu's diagnostic
display. No hidden state, no auto-detection that surprises the user.

## See also

- [`ui-render-investigation.md`](ui-render-investigation.md) — original
  investigation that ruled out the `BuildOrtho` / narrow-FOV paths
- [`render-pipeline.md`](render-pipeline.md) — sprite batcher in the
  per-frame call graph
- [`investigation-state-2026-05-06.md`](investigation-state-2026-05-06.md)
  — checkpoint where sprite-batcher path was confirmed working
