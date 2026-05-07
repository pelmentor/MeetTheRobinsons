# High-FPS render decoupling — engineering project plan

This is the working project plan for shipping Bloodborne-tier 240 Hz
rendering for Wilbur. Sister doc: [`high-fps-decoupling.md`](high-fps-decoupling.md)
covers the *what* (architecture, layers, hook points). This doc covers
the *how* (milestones, scope, validation, risk, timeline).

The plan assumes ~3–6 weeks of evening work, shipped in 6 milestones
each independently useful.

## Goals & non-goals

### Goals (what we're committing to)

1. Render at any frame rate up to 240+ Hz with **correct simulation
   speed** (no fast-physics, no fast-anim, no twitchy camera).
2. **Visually fluid** at the user's target rate for the most prominent
   on-screen elements (camera + Wilbur + visible NPCs in combat).
3. **Toggleable runtime** — user can compare on/off mid-game without
   restart.
4. **Per-system kill-switches** — if NPC interp glitches, disable just
   that without losing camera interp.
5. **Mod menu UI** for target rate + per-system toggles, sitting in
   the existing Tools tab.

### Non-goals (explicitly NOT shipping in this project)

1. **240 Hz bone-skinning interpolation** (Phase 2 layer 4). Anim bones
   stay at 60 Hz. Acceptable because skinned characters look fine when
   their bones tick at 60 Hz as long as their *world transform* (position
   + rotation) is interpolated.
2. **Particle interpolation** (Phase 2 layer 5). 60 Hz particles look
   fine.
3. **Mini-game support beyond main campaign**. DigDug / ChargeBall /
   Hamster mini-games may need separate handling — punt to a follow-up
   project.
4. **Cutscenes**. Auto-detect cuts and skip interp during cutscenes —
   don't try to make scripted cinematics 240 Hz.
5. **VR or multi-display tricks**. Just standard windowed/fullscreen.

## Constraints

### Technical
- Must not require modifying the game executable (.asi mod only)
- Must not depend on SecuROM-decrypted code addresses being stable —
  hook only call sites we own, never patch encrypted regions
- Must handle level loads, saves, cutscenes, mini-games gracefully
  (auto-disable in cases we don't support)

### Process
- Each milestone must be **independently reverible** — toggle at runtime
- Each milestone must be **independently testable** — has an acceptance
  test that doesn't depend on later milestones
- Each milestone must be **shippable** — produces a working `.asi` even
  if next milestones never happen

## Prerequisites — M0: tooling & test harness

Before any code touches the actual decoupling logic, set up the
infrastructure to validate it. ~3–5 days.

### M0.1 — Telemetry overlay

Extend the existing FPS overlay (in [`menu.cpp`](../../../src/mtr-asi/src/menu.cpp))
with detailed timing:
```
RENDER:    241.3 fps   4.14 ms
SIM:        59.8 Hz   16.71 ms   (target 60.0 Hz)
ALPHA:     0.234     (interp progress in sim window)
DECOUPLED: ON         CAM_INTERP: ON   PLAYER_INTERP: ON  NPC_INTERP: OFF
```

So at any moment we can see: render rate, sim rate, current interpolation
alpha, and which interpolation layers are active.

Acceptance: overlay shows real-time numbers, all toggles wired.

### M0.2 — Validation save points

Identify and document 4 reference save states:
1. **Open level start** (no enemies, free movement) — basic walk/jump
   timing test
2. **Combat scene** (3+ NPCs visible, particles flying) — stress test
3. **Cutscene transition** — cut-detection test
4. **Mini-game (Hamster mode)** — alternate-class test

Save state files committed to `research/test-saves/` with documentation.

Acceptance: each save loads to a known repeatable state.

### M0.3 — Validation harness

Document a manual test protocol in `docs/decouple-test-protocol.md`:

| Test | Procedure | Pass criteria |
|---|---|---|
| **Jump height** | Stand on flat ground, jump straight up, observe peak height visually against a known reference | Same height across render rates |
| **Walk speed** | Walk a fixed straight line to a known landmark, time it | Same time across render rates (±5%) |
| **Anim timing** | Play a known animation (jumping, sliding) and time start-to-end | Same duration across render rates |
| **Camera follow** | Run in a circle, observe camera-lag distance | Same lag distance + same smoothness curve |
| **Aim responsiveness** | Aim a weapon, snap to a target | No noticeable lag |
| **Cutscene** | Load M0.2 save 3, watch a cutscene | No visible interp artifacts at cuts |

Each test recorded as a video clip at 30/60/120/240 Hz for side-by-side
comparison.

Acceptance: protocol document checked in; protocol can be run by user
with no code knowledge.

### M0.4 — Logging infrastructure

Add detailed log output (toggleable, NOT default-on for performance):
- Per-sim-tick: timestamp + chosen alpha + delta from last
- Per-snapshot: object count + total bytes
- Per-render-frame: which interpolation layers ran + timing

File: `mtr-asi-decouple.log`. Rotates at 10 MB.

Acceptance: log files produced when enabled, ~zero overhead when disabled.

**M0 deliverables**: telemetry overlay + 4 save points + test protocol +
logging infra. Not a "build" yet — just the inspection toolkit.

**M0 ship**: bundled with current `mtr-asi.asi` as new module
`src/mtr-asi/src/decouple_telemetry.cpp` + UI section in menu. ~250 LOC.

## M1 — Sim throttle (Phase 1)

~5–7 days. Goal: render decoupled from sim, sim runs at fixed target
Hz, gameplay speeds correct.

### M1.1 — `sim_decouple.cpp` skeleton

New module `src/mtr-asi/src/sim_decouple.cpp`:
```cpp
namespace mtr::sim_decouple {
    enum Mode { OFF, THROTTLE_60, THROTTLE_30, THROTTLE_TARGET, THROTTLE_AUTO };
    void set_mode(Mode m);
    void set_target_hz(int hz);
    bool is_throttling();
    int  current_target_hz();
    void install();    // called from dllmain
}
```

UI: new section in Tools tab — "Decouple sim from render" with mode
combo + target Hz slider + status indicator (current sim Hz).

Acceptance: module builds, UI controls visible, no actual hooks
installed yet.

### M1.2 — Hook `simulation_tick_aggregator`

Wrap the engine's per-frame sim aggregator at `0x67F430`:
```cpp
HOOK_FN(simulation_tick_aggregator) {
    if (!is_throttling()) return ORIGINAL(args);

    const uint64_t now    = qpc_now();
    const uint64_t target = qpc_freq() / current_target_hz();
    if (now - g_last_sim_qpc < target) {
        return 0;  // skip this tick
    }
    g_last_sim_qpc = now;
    return ORIGINAL(args);
}
```

Acceptance:
- With throttle ON at 60 Hz: jump height + walk speed unchanged across
  render rates 30 / 60 / 120 / 240 (run M0.3 tests T1, T2, T3).
- With throttle OFF: behavior identical to baseline (regression check).

**Risk**: PathCam smoothing still in render path → camera will be ~4×
fast at 240 Hz with sim throttled. Will be visible. M1.3 fixes this.

### M1.3 — Throttle `pathcam_smooth_pretick`

Same pattern, separate hook at `0x58C000`:
```cpp
HOOK_FN(pathcam_smooth_pretick) {
    if (is_throttling() && now - g_last_pathcam_qpc < target_step) {
        return 0;
    }
    g_last_pathcam_qpc = now;
    return ORIGINAL(args);
}
```

Acceptance: M0.3 test T4 (camera follow) — same lag distance + smoothness
across render rates.

### M1.4 — Hidden `0.003` site sweep

We confirmed 4 sites statically. There may be more. Run this protocol:
1. Enable throttle at 60 Hz, render at 240 Hz.
2. Play through M0.2 save 1 (open level) for 10 minutes, recording.
3. Compare to same scene at 60 Hz render.
4. Look for *anything* that drifts: timer-driven effects, fades,
   periodic events, screen shakes.
5. For each drift, find the call site (likely involves a hardcoded
   `0.003` or `dword_6FFCA4`-derived float) and either throttle or
   fix per-site.

Acceptance: nothing visibly time-dependent looks wrong at 240/60 vs
baseline 60/60.

**Risk**: this is the hardest milestone. If we find 10 hidden sites,
each is its own mini-investigation. Time-box to 3 days; if more than
5 unfixed sites remain, document and ship M1 with known-issues list.

### M1.5 — Mini-game / mode handling

Test M1 with:
- Hamster mini-game (M0.2 save 4)
- DigDug if accessible
- ChargeBall if accessible

Mini-games may have separate sim ticks (e.g., the DigDug top-down loop
might run a separate sim path). If they break:
- Auto-disable throttle when mini-game class is active (detect via
  `entity_lookup_by_name_retry("player")` returning a `wilburDigDug` /
  `miniHamsterPlayer` class instead of `actor`).

Acceptance: mini-games either work correctly throttled OR auto-disable
gracefully.

**M1 ship**: `mtr-asi.asi` with sim throttling for main adventure mode.
~400 LOC. Visually still 60 Hz, but render submission is at user's
target rate. Useful for VRR users + capture pipelines + as foundation
for M2+.

## M2 — Snapshot infrastructure

~3–5 days. Pure plumbing, no visible effect yet.

### M2.1 — Snapshot framework

```cpp
namespace mtr::interp {
    struct TransformSnap {
        D3DMATRIX view_matrix;          // for camera
        float     world_pos[3];         // for entities
        float     world_rot[4];         // quaternion
        uint64_t  capture_qpc;
    };

    void   begin_sim_tick();    // called PRE simulation_tick_aggregator
    void   end_sim_tick();      // called POST simulation_tick_aggregator
    float  current_alpha();     // (now - last_sim) / sim_step, clamped [0, 1]
    bool   is_cut_detected();   // used by clients to skip interp this frame
}
```

`begin_sim_tick`/`end_sim_tick` capture the global "previous = current,
current = read live state" double-buffer.

Acceptance: framework builds + telemetry shows alpha values stepping
through [0, 1] as expected.

### M2.2 — Cut detection

Compare current sim's view matrix to previous sim's. If translation
delta > 5.0 world units OR rotation > 30°, mark `is_cut_detected = true`
for the next render frame.

Tunable thresholds via UI sliders (initially: cut if |Δpos| > 5.0).

Acceptance: M0.2 save 3 — cuts detected for cutscene cuts; not detected
for normal walk + run + jump.

### M2.3 — Hook fence pattern

Establish the **save-write-restore** pattern that M3+ will use:
```cpp
// PRE-render hook (in hk_BeforeFrameRender or equivalent):
for each interp client: client.write_interpolated();

// POST-render hook (after RENDER returns):
for each interp client: client.restore_original();
```

Without restore, sim's next read sees our modified state. With restore,
sim sees correct state. The fence ensures interp affects only render
output.

Acceptance: framework builds; pre/post hooks fire in correct order;
clients can register and the framework calls them in dependency order.

**M2 ship**: no visible change — but the snapshot infra is now in place.
~300 LOC.

## M3 — View matrix interpolation

~3–5 days. **First milestone with visible improvement**.

### M3.1 — View interp client

Register a client of M2 that:
- Snapshots view matrix at end of sim tick (read from `outer_cam+0x10`)
- On render-frame: lerps prev↔curr by `current_alpha()`
- Slerps rotation, lerps translation
- Writes interpolated matrix into `outer_cam+0x10`
- Restore on POST-render

Disable when:
- `mtr::sim_decouple` not throttling
- FreeCam active (FreeCam already provides smooth motion)
- `is_cut_detected()` returns true

Acceptance:
- M0.3 test T4 at 240 Hz: camera looks fluid, not stutter
- T5 (aim): no perceptible aim lag introduced
- Cuts in cutscenes don't show "glide" artifacts

### M3.2 — Aim-mode snap (per D4)

Default math is `pos = lerp(prev_pos, curr_pos, alpha)` and
`rot = slerp(prev_rot, curr_rot, alpha)`. No exceptions, no predict.

**Aim mode handling**: when the camera is in targeting mode (detected
via the `SetPathCamTargetingBehavior` script command setting a flag),
the view interp client clamps `alpha = 1.0` for that frame — i.e.
renders the freshest sim view directly, no interp blend. This gives
aim mode the same input-to-photon latency as baseline 60 Hz.

Detection: hook the registration of `SetPathCamTargetingBehavior` to
sniff when it's invoked + which entity is in aim mode. Cache a
`g_aim_mode_active` flag, cleared on a timer or on
`WilburAbortTargeting`.

Acceptance:
- M0.3 test T5 (aim responsiveness) at 240 Hz feels native (matches
  baseline 60 Hz to user perception).
- Non-aim camera at 240 Hz looks fluid (M0.3 test T4).
- Switching into/out of aim mode produces no visible artifact (no
  pop, no double-image).

**M3 ship**: visible 240 Hz fluidity for camera with aim-mode
correctness. Most users will perceive this as "240 Hz Wilbur" because
camera dominates. ~280 LOC (250 base + 30 aim-mode).

## M4 — Wilbur transform interpolation

~3–5 days.

### M4.1 — Stable Wilbur handle

Cache `entity_lookup_by_name_retry("player")` result, refresh every N
sim ticks (in case the level reloads and the entity ptr changes).
Detect stale (NULL on lookup) → invalidate.

Acceptance: handle stays valid through normal play; correctly invalidates
on level load.

### M4.2 — Wilbur snapshot client

At sim tick: read `wilbur+0x58..+0x60` (pos.x/y/z) + rotation quat (TBD
offset — likely `+0x68..+0x74`). Snapshot.

At render: lerp between snapshots, write interpolated values.

POST-render: restore original.

Acceptance: at 240 Hz, Wilbur's running animation looks fluid (not
60-Hz-stuttery). At 60 Hz with no decouple, behavior identical.

### M4.3 — Teleport detection

Wilbur teleports happen on level load, respawn after death, certain
scripted events. Same approach as M2.2 but per-Wilbur:
- Translation delta > 10 units → snap, skip interp this frame.

Acceptance: respawn after death looks correct (no ghost-glide).

**M4 ship**: visible 240 Hz fluidity for camera + Wilbur. This is the
"~80% of perceived benefit" point. ~300 LOC.

## M5 — NPC interpolation

~5–7 days. **Required for v1 ship** — without it, on-screen NPCs
stutter at 60 Hz next to a 240 Hz Wilbur, which is a clear visual
regression. Skip-by-cancellation only if M5 hits unrecoverable
architecture issue.

### M5.1 — Visible NPC enumeration

Walk `dword_7427C0` entity registry hash. For each entry, read its
visibility flag (from `+0x50`). Only snapshot visible entities.

Worry: registry may have hundreds of entries. Filter by:
- Visibility bit
- Distance from player (skip > 50 units)
- "Active" flag in entity (TBD which offset)

Acceptance: snapshot list has ≤ 30 entities in typical combat scene.

### M5.2 — Per-NPC snapshot via class layout registry

Same pattern as M4 (snapshot pos+rot pair). Driven by the
`EntityLayout` registry from R2 mitigation.

Implementation flow:
1. Each render frame's snapshot pass: walk visible-entity list (M5.1).
2. For each, look up its class in the layout registry.
3. If registered: snapshot `entity + layout.pos_offset` and
   `entity + layout.rot_offset` per the `rot_kind` (quat / euler / matrix).
4. If unregistered: skip + log class name to `mtr-asi-decouple.log`.
5. Restore pattern (POST-render) mirrors snapshot.

Acceptance:
- Empirical class set covering 30 min of varied main-adventure
  gameplay registered + interpolated correctly.
- Unknown classes auto-skip without crash + log entry produced for each.
- Combat scene from M0.2 save 2: ALL visible NPCs in the scene
  registered + fluidly interpolated.

### M5.3 — Cost cap

Snapshot+lerp+restore for 30 NPCs is ~30 × 50 ns × 3 = ~5 μs. Fine.
But if scene has 200 entities, this could matter. Add per-frame budget.

Acceptance: in worst-case scene, decouple overhead < 0.5 ms per render
frame.

**M5 ship**: visible 240 Hz for camera + Wilbur + NPCs. Bone skinning
still 60 Hz but unnoticeable. ~600 LOC.

## M6 — Robustness + polish

~5–7 days.

### M6.1 — Pause / load handling

When the game pauses (menu, loading screen), sim ticks should stop.
Render frames during pause should freeze to last sim state, not
extrapolate forward (alpha clamps to 1.0).

Detection: pause via screen system (we already RE'd that — Pause screen
is one of the named screens).

Acceptance: pausing doesn't show "drift" past the freeze point.

### M6.2 — Cutscene handling

Cutscenes script the camera tightly. Two options:
1. Detect cutscene start (some flag in screen state) → disable interp
   for the cutscene
2. Per-frame cut detection (M2.2) catches each cut individually

Option 1 is safer; needs to find the cutscene flag.

Acceptance: cutscenes look visually identical to baseline 60 Hz.

### M6.3 — Mini-game handling

Detect mini-game mode via player class. For modes we don't support,
auto-disable interp + sim throttle.

Acceptance: mini-games unchanged from baseline.

### M6.4 — UI polish (per D1 — fixed user-chosen integer)

- **No auto-detect**. Render Hz and Sim Hz are user-chosen integers
  (slider 30–500 / preset combo per D1).
- Per-system kill-switches: `camera_interp` / `wilbur_interp` /
  `npc_interp` independently toggleable from UI. Default: all on.
- Real-time stats overlay (M0.1) shown beside the toggles.
- Preset profiles ship as starting points, not fixed modes:
  - **Quality**: all interp on, sim 60 Hz, render 240 Hz (or user's pick)
  - **Performance**: sim throttle only (M1), interp off — for VRR users
    who want low latency without interp cost
  - **Compatibility**: decouple fully off — for troubleshooting / mods
- Each preset is a one-click apply that sets the underlying toggles.
  User can then tweak individually. (Presets are convenience, not
  modes — we never have hidden "smart" behavior.)

Acceptance: every toggle's effect is documented in the troubleshooting
doc (M6.5); UI text matches doc terminology; no behavior depends on
combinations the user can't see.

### M6.5 — Documentation

Update existing docs:
- [`high-fps-decoupling.md`](high-fps-decoupling.md) — mark Phase 1 + 2-min
  as "shipped"
- New section in [`README.md`](../../../src/mtr-asi/README.md) describing
  the decouple system + when to use it
- Troubleshooting section: "if X looks wrong, try toggling Y"

Acceptance: docs reflect shipped state; user can troubleshoot common
issues without code knowledge.

**M6 ship**: production-quality build. ~250 LOC + docs.

## Total scope summary

| Milestone | Days | LOC | Ships when |
|---|---:|---:|---|
| M0 — Tooling | 3–5 | 250 | Telemetry + saves + protocol ready |
| M1 — Sim throttle | 5–7 | 400 | Test 1–4 pass at 240/60 (per D5: all `0.003` sites fixed) |
| M2 — Snapshot infra | 3–5 | 300 | Plumbing + cut detection + fence (per D2) |
| M3 — View interp | 3–5 | 280 | Camera fluid at 240 Hz, aim mode snaps (per D4) |
| M4 — Wilbur interp | 3–5 | 300 | Wilbur fluid at 240 Hz |
| M5 — NPC interp | 5–7 | 600 | Combat NPCs fluid via class registry (per R2) |
| M6 — Polish | 5–7 | 250 | Production-ready, mini-game auto-disable (per D3), no auto-detect (per D1) |
| **Total** | **27–41 days** | **~2380 LOC** | |

Realistic calendar: **3–6 weeks of evening work**. With weekends + focus
days, achievable in 4 weeks for an experienced dev. With debugging
realities (especially M1.4 hidden-site sweep, which is now uncapped per
D5), allow 6.

## Risk register

Top risks ordered by impact + likelihood:

### R1 — Hidden `0.003` sites cascade through M1 (HIGH probability, HIGH impact)

Static RE found 4 sites. Runtime testing might reveal more. Each hidden
site is a separate mini-investigation, can blow M1 timeline.

**Mitigation** (per D5 — fix all, no known-issues ship):
- Build a generic `register_throttled_function(addr, sim_phase)` API in
  M1.1 so each new site is a one-line addition.
- Initial 3-day budget is for the *expected* count (4 known + maybe 2
  hidden). If reality is 8+, M1 calendar expands proportionally
  (~1 hr/site).
- If a site cannot be hooked (encrypted code), trigger cancellation
  criteria — stop, rethink architecture, don't ship around it.

### R2 — Per-class entity layout variance breaks M5 (MEDIUM, HIGH)

Entity layout is heterogeneous (per entity-system.md). Position fields
might be at +0x58 for `compactor` but +0x40 for `digDugAnt`.

**Mitigation** — class-layout registry, no per-class crutches:
- New header `src/mtr-asi/include/mtr/entity_layouts.h` with a typed
  `EntityLayout { class_id, pos_offset, rot_offset, rot_kind }` table.
- Empirically populate during M5: enable interp + enable
  unknown-class logging → play the main-adventure save → log shows
  every class encountered → register each.
- Auto-skip unknown class (no interp on it) **and log to
  `mtr-asi-decouple.log`** so we know what we missed. Skipping is fail-
  safe; logging makes "missing classes" observable without any user
  report.
- Cap at "all classes encountered in 30 minutes of varied gameplay" =
  ship boundary. Empirically expected to be 5–10 entries.

### R3 — Cutscene/cut detection imperfect (LOW, MEDIUM)

Some cuts will slip through M2.2's threshold-based detection, causing
visible glide.

**Mitigation**:
- M6.2 adds explicit cutscene-flag detection
- Tunable threshold; user can dial down if cuts fall through

### R4 — Aim/targeting feels laggy at 240 Hz (LOW, HIGH)

Interp shows lerp(prev_sim, curr_sim, alpha) — on average a frame is
~8 ms behind "current truth". Aim mode magnifies this perceptually.

**Mitigation** (per D4 — lerp only, no predict):
- Detect aim mode via `SetPathCamTargetingBehavior` script command
  registration → in aim mode, view interp **snaps to curr** (alpha=1)
  every render frame. No interp blend, no predict, just "render the
  freshest state we have."
- Net: aim mode visually = baseline 60 Hz (each render frame shows
  latest sim view). Same as user expects; only non-aim camera is fluid.
- This adds ~30 LOC to M3 (aim-mode detection + mode-specific alpha).

### R5 — Performance overhead exceeds budget (LOW, MEDIUM)

Snapshot+lerp+restore for many entities could cost > 1 ms per frame.
At 240 Hz that's 24% of budget.

**Mitigation**:
- M5.3 cost cap
- Profile before/after; allocate < 0.5 ms hard limit
- Filter aggressively (visible+nearby only)

### R6 — SecuROM thunks return wrong addresses on Reset/level load (LOW, HIGH)

We hook user-space addresses. SecuROM thunks point into runtime-decrypted
regions. If those addresses change after a Reset or level transition, our
hooks break.

**Mitigation**:
- We never hook SecuROM-encrypted code, only call sites we own
- Re-verify hook addresses after Reset (already done by minhook)
- Watchdog: if a hook stops firing for > 2 seconds with throttle on,
  log + auto-disable

### R7 — Multi-mode (mini-game) classes have different sim paths (MEDIUM, MEDIUM)

DigDug / Hamster / Driver might bypass the main aggregator entirely.

**Mitigation**:
- M1.5 explicitly tests each mode
- Auto-detect mode via player class lookup
- Provide per-mode disable

## Design decisions (resolved 2026-05-06)

The five design questions are settled — captured here so anyone returning
to this plan understands what was committed and why.

### D1 — Fixed user-chosen target rate

**Decision**: render cap is a **fixed integer in Hz, user-chosen**. No
auto-detection of monitor refresh, no dynamic tracking.

**UI**: Tools tab → "Decouple sim from render"
- Sim Hz: 60 (default), 30, 120 (preset combo)
- Render Hz: integer slider 30…500, default 240
- Status indicator: actual measured render + sim rates

**Why**: Predictable, matches the user's mental model, no surprise from
monitor reconfigs / external apps. User-supplied integer; we obey.

### D2 — Strict cut detection (two-layer)

**Decision**: cut detection is **strict** by default. Layered:
1. **Threshold-based** (M2.2): translation Δ > 5 world units OR rotation
   Δ > 30° between consecutive sim ticks → mark next render frame as
   no-interp.
2. **Explicit cutscene state** (M6.2): when game enters scripted-cutscene
   state (detected via screen system flag — to be located in M6), interp
   is disabled for the duration of the cutscene.

The two layers are AND-ed: either trigger disables interp.

**Why**: a stutter during a cut is invisible (eye expects discontinuity);
a "smooth glide" through a scripted cut is glaringly wrong. The cost of
a false-positive cut (one render frame shows snap-to-current instead of
interpolated) is one stutter frame — imperceptible. The cost of a
false-negative is a 8 ms visible glide through a scene cut — very
noticeable. So we err strict.

User can override threshold via UI sliders if a specific scene needs
adjustment.

### D3 — Mini-games auto-disable (graceful degrade)

**Decision**: in mini-game modes (`wilburDigDug`, `miniHamsterPlayer`,
`WilburDriver`), the entire decouple system **auto-disables** and
returns the user to baseline 60 Hz behavior. UI shows "Decouple disabled
(mini-game mode)".

**Detection**: at the start of each render frame, query
`entity_lookup_by_name_retry("player")` and check the C++ class. If
not `actor` (the main mode), disable.

**Why**:
- Mini-games may have their own sim paths that bypass
  `simulation_tick_aggregator` (we'd need to RE each separately).
- Mini-games are short, secondary content; user loses ~60s of fluidity
  per mini-game segment, gains 100% reliability.
- Adding mini-game support is a clean follow-up project — the
  architecture supports it via per-mode configuration; we just don't
  ship it in v1.

**Best-practice angle**: failing safe is not a crutch. A crutch would
be "make it kind of work in mini-games and hope nothing weird happens".
Auto-disable-with-status is honest.

### D4 — Lerp only, no predictive interp

**Decision**: all interpolation uses **plain lerp** (slerp for rotation).
No predictive / extrapolation modes.

**Why**:
- Lerp is **mathematically correct**: render frame N at alpha=α shows
  the state the sim *would have been* at α of the way through the
  current sim step. That's the truth.
- Predictive / extrapolate is *guessing the future* — fundamentally
  wrong, jitters on direction changes, introduces artifacts that have
  no good general solution.
- The "lerp adds 1-frame lag" concern: at 60 Hz sim, render shows
  ~8 ms-old state on average. That's smaller than baseline 60 Hz
  rendering's 16 ms input-to-photon. **Net change in latency is zero
  or slightly improved** because render frames now happen in between
  sim ticks instead of being aligned to them.
- For aim feel: D2's strict cut detection AND-detection of "user
  actively aiming" can disable view interp during aim if it ever feels
  laggy. M6 polish step.

**Best-practice angle**: the simpler primitive (lerp) is correct under
all conditions; the more complex one (predict) introduces a class of
bugs we'd need to whack one-by-one. Pick the principled primitive.

### D5 — Fix all 0.003 sites, no shipping with known time bugs

**Decision**: in M1.4 hidden-site sweep, **every discovered site is
fixed before M1 ships**. No "known issues" list shipped.

**Time budget**: M1.4 is given as much time as needed (initial estimate
3 days; expand if reality demands). Each new site is a ~1-hour fix
(register a new throttle-able function + add to the install list).

If a site cannot be fixed (e.g., it's in encrypted SecuROM code we can't
reach), the cancellation criteria trigger — we stop and rethink, not
ship-with-glitches.

**Why**:
- A "rare physics drift bug" is a crutch: the architecture is supposed
  to deliver correct simulation speed; if it doesn't, the architecture
  failed.
- Each unfixed site is a vector for confusing user reports later
  ("sometimes my Wilbur jumps higher!"). Debugging recurring user
  reports is more expensive than fixing the site upfront.
- The pattern is identical (one-line throttle hook per site), so cost
  scales linearly with site count, not exponentially.

**Best-practice angle**: ship something that's correct under its claimed
contract or don't ship. Half-correct is worse than not-yet-shipped.

## Pre-flight checklist before M0

Before we start, confirm:
- [ ] Latest `mtr-asi.asi` builds and runs cleanly
- [ ] FreeCam works (used as comparison + sanity check)
- [ ] FPS overlay works (will be extended in M0.1)
- [ ] FPS limiter works (will be the basis of M1's throttle pattern)
- [ ] Save state mechanism in game allows reproducible test scenes
- [ ] We have at least one save we can hot-reload for tests
- [ ] User has a 240 Hz capable display + monitor recording capability
  (for M0.3 video comparison)

If all are checked, M0 starts. Otherwise we resolve gaps first.

## Cancellation criteria

If during M1.4 we discover that the 0.003 step is referenced in 20+
distinct sites including in encrypted code regions, the project is
infeasible without binary patching. Cancel + document findings.

If during M2 we discover that the `outer_cam+0x10` matrix is read
*during* `camera_apply_all_active` (not just after), the fence pattern
is unreliable. Reroute to a different hook (e.g. patch `D3D9::SetTransform`
itself).

If by end of M1, no measurable reduction in physics drift at 240 Hz
vs. uncapped, the throttle architecture is wrong. Stop, redesign.

These are non-judgmental ship-or-stop gates.

## See also

- [`high-fps-decoupling.md`](high-fps-decoupling.md) — architecture (what)
- [`frame-pacing.md`](frame-pacing.md) — current FPS limiter (Phase 0)
- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — `0.003` sites map
- [`player-system.md`](player-system.md) — Wilbur lookup + PathCam path (M3+M4 hooks)
- [`entity-system.md`](entity-system.md) — entity registry + layout (M5 hooks)
