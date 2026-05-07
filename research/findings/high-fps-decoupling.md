# High-FPS render decoupling (240 Hz target)

> **Status (2026-05-07): Phase 1 + Phase 2 layers 1+2+3 shipped.** All
> milestones M0 → M6 from [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md)
> are live in `Game/mtr-asi.asi`. The single deferred item is M3.2
> (aim-mode auto-snap), which requires script-VM RE; in practice the
> cut-detection threshold (M2.2) catches fast aim adjustments and the
> manual M3 toggle covers the rest. See per-milestone shipped docs in
> `research/findings/decouple-*.md`.

How to render Wilbur at 240 Hz without breaking physics, animation,
camera, or AI. This is the same architecture that the Bloodborne 60 fps
fan-patch uses. **Not a quick hack** — it's a multi-week project at the
high-quality end. We can ship a 30%-effort/80%-effect intermediate
target.

> **For the working project plan** with milestones, scope, validation
> protocol, risk register, and timeline estimates, see
> [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md).
> This doc covers the *what* (architecture); that doc covers the *how*
> (engineering plan).

## Why naive FPS unlock breaks the game

Reverse engineering of [systems-survey-2026-05-06.md](systems-survey-2026-05-06.md)
established that **`0.003` is hardcoded in four integration sites**, each
called once per render frame:

| Site | Function | Effect of 240 fps without intervention |
|---|---|---|
| Physics state machine | [`physics_state_machine_tick` @ 0x4DC150](systems-survey-2026-05-06.md) | `pos += vel * 0.003` per render frame → **4× speed** |
| Animation controllers | [`anim_controller_advance` @ 0x4E2B00](animation-system.md) | `time += 0.003 * rate` per frame → **4× anim playback** |
| Camera follow | [`pathcam_smooth_pretick` @ 0x58C000](player-system.md) | spring damping at 0.003 → **camera snaps instantly to target** |
| Particles | [`particle_update_tick` @ 0x4D9230](ai-script-vm.md) | lifetime/pos/rot at 0.003 → **particles live 4× shorter** |

There is **no accumulator** in the engine pump (`sub_572040`). Each render
frame triggers one full simulation pass. The constant `flt_6FFCBC = 0.003f`
is a **scale factor calibrated to render-rate ≈ sim-rate ≈ 60 Hz** — not
a real delta-time. Velocities are stored as `units-per-3ms-step`, scaled
to give correct visual motion when called ~60 times per second.

Increase the call rate, and everything moves faster. Decrease, slower.
This is why our existing FPS limiter has the *Test 8* caveat: the
limiter itself is clean, but the engine wasn't authored for arbitrary
render rates.

## Architecture: decouple sim from render + interpolate

This is the standard solution for the problem, used in every modern
"60 fps patch for a 30 fps console game" community project (Bloodborne,
Demon's Souls, Dark Souls, Sekiro, plus most Nintendo console emulators).

```
Without modification (the broken state):
  240 Hz render → 240 Hz sim → physics/anim/camera 4× faster, game unplayable

Phase 1 (sim-throttle, latency-only fix):
  240 Hz render → 60 Hz throttled sim → correct simulation speed
  But each 4 render frames show the same world state → visually 60 Hz

Phase 2 (interpolation, real 240 Hz fluidity):
  240 Hz render with lerp(prev_sim_state, curr_sim_state, alpha)
  60 Hz throttled sim under the hood → correct simulation speed
  Visually 240 Hz fluid → looks like a 240 Hz native game
```

## Phase 1 — sim throttle (~150 LOC, low risk)

**Goal:** decouple render rate from simulation rate. Render at any rate;
keep sim ticks pinned to 60 Hz (or whatever the user picks). Physics,
animation, AI all run at correct authored speed.

**Mechanism:**
1. Hook `simulation_tick_aggregator` (`0x67F430`).
2. Track `last_sim_qpc`. On entry: if `now - last_sim_qpc < target_step`,
   return without calling the original. Otherwise call original and
   update `last_sim_qpc`.
3. Default `target_step = 1/60` sec (matches authored cadence).

**Catch — PathCam runs in render path, not sim path:**

`pathcam_smooth_pretick` is reached via `PathCam_tick` ←
`camera_apply_all_active` ← `render_frame_top_level`. So it fires
**every render frame, not every sim frame**. Throttling
`simulation_tick_aggregator` alone does *not* fix camera smoothing.

Two options for fixing camera at Phase 1:
- **(a) Throttle `pathcam_smooth_pretick`** — only run it every Nth
  render frame. Simple, ~30 LOC.
- **(b) Patch the `0.003` constant inside it to use real dt** — proper
  fix but requires modifying the function body or wrapping it. ~80 LOC.

Option (a) is enough for Phase 1. Option (b) is what Phase 2 needs anyway.

**Phase 1 deliverables:**
- New module `src/mtr-asi/src/sim_decouple.cpp`
- `mtr::sim_decouple::install()` — hooks `simulation_tick_aggregator` +
  `pathcam_smooth_pretick`
- `mtr::sim_decouple::set_target_hz(int)` — runtime configurable
- UI checkbox + slider in Tools tab: "Decouple sim from render", target
  Hz

**What Phase 1 gets the user:**
- Render submits at 240 Hz (or whatever cap is set)
- Physics + anim + AI + camera run at correct authored speed
- **But visually still 60 Hz** — every 4 render frames show the same
  world state

**When is Phase 1 alone useful?**
- VRR (G-Sync / FreeSync) monitors — better frame pacing, less judder
- Streaming / capture — smoother capture timing
- Reduced input-to-photon latency on the render submission side
- *Not useful for "smoother gameplay feel"* — that needs Phase 2

**Test plan for Phase 1:**
1. Set sim target = 60. Set render cap = 240.
2. In game: jump → measure jump height + airtime via stopwatch.
3. Set sim target = 60. Set render cap = 30.
4. Repeat jump test. Both should match.
5. Walk a fixed distance — same time at any render rate.
6. Animations — visually identical timing at any render rate.

If these match, Phase 1 is correct. If they don't, there's another
hardcoded-step site we missed.

## Phase 2 — view + transform interpolation (~600–3000 LOC)

**Goal:** every render frame shows world state interpolated between the
previous and the next sim tick. World looks like it updates at the
render rate, not the sim rate.

### Per-render-frame algorithm

```c
// In hk_BeforeFrameRender or hk_PreCameraApply:
const double now = qpc_seconds();
const double dt_since_sim = now - g_last_sim_time;
const double sim_step = 1.0 / g_sim_target_hz;
const float alpha = clamp(dt_since_sim / sim_step, 0.0f, 1.0f);

// For each interpolatable entity:
//   render_transform = lerp(snapshot_prev, snapshot_curr, alpha)

// On each sim tick (in hk_simulation_tick_aggregator AFTER original):
//   snapshot_prev = snapshot_curr;
//   snapshot_curr = read_current_transforms();
```

### Layers, by impact-vs-effort

Tiered list — implement top to bottom; stop when "good enough".

| # | Layer | LOC est. | Visual impact | Notes |
|---|---|---|---|---|
| 1 | **View matrix (PathCam → outer cam)** | ~150 | Huge | Easiest. We already hook `camera_apply_all_active` for FreeCam — same hook works. |
| 2 | **Wilbur transform** | ~200 | Big | `entity_lookup_by_name_retry("player")` gives a stable handle. Snapshot pos+rotation, lerp on render. |
| 3 | **Visible NPC transforms** | ~500 | Medium | Walk the entity registry hash table (`dword_7427C0`), snapshot active entities. Hash filter or visibility-bit gate prevents thrashing. |
| 4 | **Animation bones (skinning)** | ~1500+ | Small if 1–3 are done | Each entity's anim graph writes to its scene matrix. Hook `anim_evaluate_track`'s SecuROM-thunked TRS builder (+161538) to snapshot, then lerp into the per-bone palette. |
| 5 | **Particles** | ~500 | Negligible | Particles already look fine at 60 Hz; not worth it. |

### Recommended Phase 2 minimal set

**Layers 1 + 2 (camera + Wilbur):** ~350 LOC, ~3 days work, ~80% of the
perceived fluidity benefit. Most fluidity comes from camera; secondary
fluidity from the player avatar (which is on-screen 90% of the time).
NPCs at 60 Hz with a 240 Hz camera look mostly fine.

**Full Phase 2 (layers 1–4):** ~2500 LOC, ~2 weeks. Bloodborne-tier
polish.

### Hook points

All already documented in our reverse engineering:
- View matrix: hook `camera_apply_all_active` (already known + used by FreeCam)
- Wilbur transform: `entity_lookup_by_name_retry("player")` returns
  `*(p+0x58)` = pos.x; snapshot 12 bytes (pos) + rotation
- NPC transforms: walk `dword_7427C0` (entity registry hash), filter by
  visibility flag
- Anim bones: hook the SecuROM TRS thunk at `g_securom_thunk_table_base
  + 161538`

### Phase 2 storage cost

Per snapshotted transform: ~32 bytes (3-vec pos + quat rot). For ~200
visible entities × 2 snapshots = ~12 KB. Trivial.

## Risks and edge cases

### Sim ticks that take longer than 16.67 ms

If a sim tick takes 20 ms (unlikely on modern hardware but possible
during level loading or asset streaming), `alpha` would exceed 1.0 →
visible "ghosting" past the current state. **Clamp alpha to [0, 1]**.

### Variable-rate sim

The user might set sim target = 30 Hz to lower CPU. Code must handle
arbitrary `sim_step`. Already handled by computing `alpha` from real
QPC delta and `g_sim_target_hz`.

### Reset / level load

When a level loads or the game resumes from pause, the
"prev snapshot" is stale. Either:
- Force `alpha = 1.0` for one frame on reset (simple, no glitch)
- Detect "snapshot stale" via a frame counter and rebuild

Cleanest: on level load, copy curr → prev so first interp frame is
identity.

### Sim that runs sub-frame (rare)

If the user sets render cap below sim target (e.g., render = 30,
sim = 60), then sim ticks twice per render. The interpolation alpha is
> 1.0 between snapshots. **Clamp**, or detect and skip the second
intermediate sim's interp.

### Cutscenes

Some cutscenes script the camera deterministically. Interpolation across
sudden cuts looks bad. Detect cuts via "view matrix delta exceeds
threshold" → skip interp for that frame.

### Aim / targeting modes

Targeting mode binds the camera tightly. Whether smoothed PathCam or
the targeting state owns the view depends on game state. In Phase 1
this works automatically; in Phase 2 verify aim feels responsive at
240 Hz (interpolation can introduce input lag if aim is interpolated).

## Comparison with the Bloodborne 60 fps approach

The Bloodborne patch (Lance McDonald + others, 2018–2024) follows the
same architecture:
- Locked sim (Bloodborne uses 30 Hz) — they keep it as-is
- Render at higher target (60 / 90 Hz) with view + entity interpolation
- Per-entity transform snapshots and lerps
- Took years of community work to get smooth.

We have advantages over the Bloodborne case:
1. **Full IDB control** + we've decoded the relevant entry points statically
2. **`game_get_time_ms` is QPC-based** so timing is precise
3. **Single FreeCam already hooks the right place** — view interp drops in
4. **No SecuROM at the runtime layer** for our hooks (we hook the
   already-decrypted call sites in our own DLL)
5. **MTR is a 2007 PC port**, smaller scope than a multi-million-line
   PS4 game

What we *don't* have: parity with Bloodborne's polish budget. So expect
the result to be 80–90% there, with edge cases (cutscenes, aim, certain
NPCs) needing per-case tweaks.

## Recommendation

1. **Ship Phase 1 first.** ~150 LOC, low risk, validates that the
   throttling architecture works without breaking sim. The output will
   feel like "60 fps gameplay submitted at 240 Hz" — good for VRR
   monitors, useful for capture, but not the "fluid 240" the user wants.
2. **Then ship Phase 2-minimal (layers 1–2).** ~350 LOC. Adds the
   actual visual fluidity for camera + Wilbur. NPC at 60 Hz is fine
   because humans look at the avatar and the camera, not at every
   background NPC.
3. **Phase 2-full** (layers 3–4) is a follow-up project, not a sprint.

## Alternatives considered

### Option X: just patch `flt_6FFCBC` to scale with frame rate

Rejected. The constant is read in many places and several aren't even
through `flt_6FFCBC` — they're hardcoded `0.003` literals in opcode
positions. Would need a comprehensive find-and-patch pass on all
literals across ~10 functions, with risk of patching constants that
are *not* the time step (e.g., a cosine threshold).

### Option Y: hook each consumer of the time step independently

Rejected as primary approach. Same outcome as the constant patch but
more invasive (more hook surface). Could be a fallback if Phase 1's
single-throttle approach has unforeseen issues.

### Option Z: patch the engine to add an accumulator

Rejected. Modifying the engine pump (`sub_572040`) to run sim N times
per render is invasive, the inner functions assume single-step
semantics in some places, and we'd need to verify against the entire
sim aggregator's downstream effects. The decoupling approach is the
industry-standard answer.

## Open questions for the user

1. **Target frame rate**: 240 Hz is the example, but design should
   support any target (60 / 120 / 144 / 240 / unlock). Default to
   monitor refresh rate?
2. **Sim rate**: keep at 60 Hz (authored cadence) or expose? 60 is the
   safe default.
3. **Phase 2 scope**: minimal (camera + Wilbur) or full (NPCs +
   particles)? Minimal is recommended.
4. **Test workload**: what's the most stressful gameplay scene to
   validate against? Combat with multiple NPCs + particles is the
   classic stress test.

## See also

- [`frame-pacing.md`](frame-pacing.md) — current FPS limiter design (Phase 0)
- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — `0.003` hardcoded sites + simulation tick map
- [`animation-system.md`](animation-system.md) — anim controller `0.003` step
- [`player-system.md`](player-system.md) — PathCam smoothing path
- [`entity-system.md`](entity-system.md) — entity registry hash for layer-3 interpolation
