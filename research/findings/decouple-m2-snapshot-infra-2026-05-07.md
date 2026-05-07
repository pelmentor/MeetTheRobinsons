# M2 — Snapshot infrastructure

Implements the prev/curr double-buffer + alpha computation + cut detection
required by M3+ interpolation clients in the high-fps decouple plan. Pure
plumbing — no visible visual change. Live in `mtr-asi.asi` as of
2026-05-07 build 522240 bytes.

## Engine background

Per the engine pump (`sub_572040` @ 0x572040) order:

```
loop {
    sub_57A2C0()                  // dt prep
    simulation_tick_aggregator()  // sim @ 0x67F430
    sub_609B90()
    render_frame_top_level()      // includes camera_apply_all_active @ 0x4C1E40
    sub_5908C0()
}
```

`camera_apply_all_active` walks the active camera array, and via the
stolen-byte-thunked `sub_4C1BA0` (per-camera apply), writes the current
scene camera's view + world matrices into globals:

| Global VA | Size | Content |
|-----------|------|---------|
| `0x724C10` | 64 bytes | view matrix (D3DMATRIX, row-major float[16]) |
| `0x724C50` | 64 bytes | world matrix (same layout) |

By the time `camera_apply_all_active` returns, both globals reflect the
last-applied camera, which is the canonical scene camera for the frame.
Downstream render-path consumers (`sub_4C4110` UV scroll,
`render_sprite_batcher`, etc.) read these globals.

## Snapshot architecture

### State machine

Two slots:
- **prev** — sim tick T-1 (or invalid if only one tick has run)
- **curr** — sim tick T (or invalid if no tick has run yet)

Each slot stores `view[16]`, `world[16]`, `qpc` (capture timestamp), and
a `valid` flag.

A single `dirty` atomic bit signals "the most recent sim tick produced a
new state, the next camera_apply will reflect it":

1. `hk_sim_aggregator` POST (when orig runs): set `dirty = true`.
2. `hk_camera_apply_all_active` POST: if `dirty` was set, shift
   `prev <- curr` (if curr was valid), capture globals into curr, clear
   `dirty`. If `dirty` is false, leave snapshots untouched (sim was
   throttle-skipped this iteration, globals unchanged).

Throttle skip: `on_sim_tick_post` not called → dirty stays false → camera_apply
hook is a no-op for the snapshots → curr stays at last actual sim's state.
ALPHA continues to grow toward 1.0 (clamped) through these skipped frames.

### Alpha computation

```
alpha = clamp((now - curr.qpc) / (qpc_freq / target_hz), 0.0, 1.0)
```

- `curr.qpc` is set to QPC at POST camera_apply_all_active (when the
  capture happens).
- `target_hz` comes from `mtr::sim_decouple::target_hz()` — the user's
  configured sim rate.

Alpha=0 immediately after a fresh sim tick; grows toward 1.0 over the
sim_step window; clamps to 1.0 if sim takes longer than expected (long
load, lag spike).

### Cut detection (M2.2)

Per render frame, examine the latest sim's prev<->curr delta:

| Threshold | Default | Meaning |
|-----------|---------|---------|
| Translation | 5.0 world units | `\|curr.view[12..14] - prev.view[12..14]\|` |
| Rotation | 30 deg | `acos(dot(curr.view[8..10], prev.view[8..10]))` (forward axes) |

Either trigger → `is_cut_detected() == true` for the duration of this
sim window (from the moment of capture until the next sim tick captures
new curr). M3 clients read this flag to skip interp on cut frames so
cinematic cuts don't "glide".

The view matrix's translation column lives at row 3 (indices 12..15) in
D3DMATRIX row-major layout. The forward axis lives at row 2 (indices
8..10).

Thresholds are live-tunable from the menu (Tools → Decouple →
"Cut-detect thresholds" sliders). Defaults match the project plan §M2.2
recommendation (5 units / 30 deg).

## What's deferred (M2.3 fence pattern)

The plan describes a "save-write-restore" fence: PRE-render-frame writes
interp matrices into the globals, POST-render-frame restores the actual
curr matrices so sim's next read sees real state. **Not yet implemented**
because there's no client in M2 that writes — interp clients (M3 view
interp + M4 Wilbur transform interp + M5 NPC interp) will need it. M2.3
will land coupled with M3.1 since they share the same hook surface.

The current M2 snapshot path doesn't write anywhere, only observes. So no
fence is needed at this milestone.

## Why hook camera_apply_all_active POST (not pre)

- **Pre-hook** would capture stale globals (last frame's camera state)
  because `sub_4C1BA0` per-camera applies run inside the body. Useless
  for our purpose.
- **Post-hook** sees the freshest output (the decompile comment "Last
  write to globals 0x724C10 (view) / 0x724C50 (world) wins" confirms
  globals are stable on return).
- The function is `char camera_apply_all_active(void)` — `__cdecl`, no
  args, char return. MinHook detour with matching signature works
  trivially.

## Why hook globals (not entity state)

We could read view from `outer_cam+0x34` (the active camera's view
pointer) per the `sub_4C1BA0` decompile. Two reasons not to:

1. The active camera changes during gameplay (PathCam / ScriptCam /
   StationaryCam / FreeCam). We'd have to track which camera is active
   and follow vtable pointers.
2. The globals already aggregate the answer ("last write wins"). One
   read site, regardless of camera type.

For M3 view interp we'll write back to globals only — same scope. For
M4 Wilbur transform we'll snapshot per-entity from an Entity*, separate
infrastructure.

## API surface

```cpp
namespace mtr::interp {
    void install();
    void on_sim_tick_post();           // called from sim_decouple

    const Snapshot& prev();
    const Snapshot& curr();
    float current_alpha();             // [0, 1]
    bool  has_two_snapshots();
    bool  is_cut_detected();

    float cut_translation_threshold();
    void  set_cut_translation_threshold(float v);
    float cut_rotation_threshold_deg();
    void  set_cut_rotation_threshold_deg(float deg);

    uint64_t snapshots_taken();
    uint64_t cuts_detected();
}
```

## UI surface

Tools → Decouple → "Live measurements:":
- New `ALPHA: 0.234  (CUT this frame)` line — populated whether or not
  throttle is engaged (snapshot infra runs unconditionally).
- New `Snapshots: N  cuts: M  (interp ready: yes/no)` diagnostic line.
- New "Cut-detect thresholds" sliders (translation, rotation) for tuning.

FPS overlay (when decouple is active): `ALPHA` line now reads the real
interp alpha from this module instead of the placeholder 1.0.

## Validation

- With throttle OFF: `Snapshots` increments by 1 each render frame
  (every camera_apply runs without dirty, but the FIRST frame seeds via
  `!g_curr.valid`, then subsequent frames don't re-capture). On exit,
  Snapshots may equal 1.
- With throttle@60 + render at 240: `Snapshots` increments at ~60/sec
  (only fresh sim ticks trigger captures via dirty). ALPHA cycles 0 → 1
  four times per second.
- **Cut test**: load a save with a triggered cutscene; cuts should
  spike `cuts_detected` counter. Tune sliders if too many false-positives.

## Performance

- Snapshot capture: 2 × 64-byte memcpy + QPC + bool exchange = ~50 ns
  total. Once per sim tick.
- Cut detect: 3 floats + 1 acosf + 2 comparisons. Once per sim tick.
- ALPHA read (per frame, plus per-overlay-render): 1 QPC + 1 atomic
  load + clamp = ~30 ns.

Net cost: well under 1 microsecond per render frame even at 240 Hz. No
measurable impact on frame budget.

## Files

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/interp.h` | NEW — public API |
| `src/mtr-asi/src/interp.cpp` | NEW — snapshot + cut detection |
| `src/mtr-asi/src/sim_decouple.cpp` | calls `interp::on_sim_tick_post()` from hk_sim_aggregator |
| `src/mtr-asi/src/dllmain.cpp` | calls `interp::install()` from mtr_init |
| `src/mtr-asi/src/menu.cpp` | overlay + Decouple section read alpha + cut + show threshold sliders |
| `src/mtr-asi/CMakeLists.txt` | adds `src/interp.cpp` |

## See also

- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) §M2 — original spec
- [`high-fps-decoupling.md`](high-fps-decoupling.md) — architecture
- `memory/project_decouple_m0_m1_shipped.md` — preceding milestones
