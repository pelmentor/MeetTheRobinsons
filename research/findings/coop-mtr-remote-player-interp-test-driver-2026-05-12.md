# `MtrRemotePlayer` interp test driver (Phase 1.2) — SHIPPED 2026-05-12

**Date:** 2026-05-12 (afternoon, second half).
**Status:** SHIPPED. Build green. Soak `load-save-1-show-ingame` PASS at 4550
frames; 3600-frame orphan-alive window clean; `interp_writes` /
`interp_pushes` growing monotonically with `faults=0`. End-to-end pipeline
validated. Companion to
[coop-mtr-remote-player-interp-scaffold-2026-05-12.md](coop-mtr-remote-player-interp-scaffold-2026-05-12.md).

> Phase 1.1 added the *plumbing* (snapshot pair, alpha calc, SEH-wrapped
> apply). Phase 1.2 adds the *first user* of that plumbing — a cmdline-
> gated driver that feeds `engine_wilbur`'s live pose into every Remote
> player each sim tick. Validates the entire scaffold without any
> networking code.

---

## Why this lands before networking

Per the prior session's 3-agent next-step consensus (architect / reviewer /
explorer all converged on interp scaffold for Phase 1.1), the explicit
post-scaffold pickup item was:

> **Phase 1.2 — interp test driver**: cmdline-gated self-test
> (`-mtrasi-coop-remote-interp-test`) that, in `MtrPlayerManager::do_pulse`,
> reads `engine_wilbur`'s pose via `mtr::interp::capture_entity_pose` and
> pushes it as a snapshot to the orphan via `push_interp_snapshot`.
> ~30 LOC in remote_player_manager.cpp. Proves the scaffold end-to-end
> without any networking code.

This proves: capture → push → alpha-blend → write all work on real engine
entity memory without crashing or corrupting state. When networking lands
later it can drop the simulated pose source for the real wire one.

---

## What shipped

### New cmdline flag

`-mtrasi-coop-remote-interp-test`. Off by default. When set, the manager
drives every Remote player's interp from `engine_wilbur`'s live pose each
sim tick. The flag string is 31 chars; no other coop flag is a prefix or
substring of it, so the `strstr` substring match is collision-free.

### `MtrPlayerManager::do_pulse` — test-driver block

In `src/mtr-asi/src/coop/remote_player_manager.cpp`:

- **One-shot announce**: on the first pulse with the flag detected, emit
  `[mtr_player_manager] interp test driver: ENABLED (...)`. Fixes the
  observability gap where the heartbeat suppresses the interp tail line
  while all counters are still zero (driver enabled, no Remote registered
  yet). Audit fix.
- **Capture OUTSIDE the lock**: `mtr::interp::capture_entity_pose(w, out)`
  is itself SEH-wrapped; keeping it outside `m_mu` ensures an engine
  fault never holds the manager lock through SEH unwind. The captured
  pose is a stack-local value-copy by the time `m_mu` is taken — engine
  teardown between capture and push cannot corrupt the in-hand data.
- **Push + enable INSIDE the lock**: honours the
  `push_interp_snapshot` threading contract (documented in
  `remote_player.h`: "MUST hold MtrPlayerManager::m_mu"). For each Remote
  player: `set_interp_enabled(true)` then `push_interp_snapshot(pos, rot)`.
- **Order**: push runs BEFORE `up->do_pulse()` so the same tick that
  captured the pose also blends with it (one-tick lag rather than two).

### Architectural fix: extract `engine_player` module (principle 7)

The Phase 1.2 audit (domain-fidelity, 85% confidence) flagged a
principle-7 violation: `kPlayerCtrlMgrVA = 0x728A40` plus its SEH-wrapped
deref had **duplicated copies** in both
[coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp)
(engine-wrapper layer) and
[remote_player_manager.cpp](../../src/mtr-asi/src/coop/remote_player_manager.cpp)
(gameplay/network layer). Phase 1.2 added a second call site in the
manager, *deepening* the violation rather than fixing it.

Per RULE №1 ("no crutches, do it properly"), extracted the engine-side
detail into:

- **New header**: [include/mtr/coop/engine_player.h](../../src/mtr-asi/include/mtr/coop/engine_player.h)
  — minimal surface: `void* mtr::coop::engine_player::engine_wilbur_ptr() noexcept`.
- **New impl**: [src/coop/engine_player.cpp](../../src/mtr-asi/src/coop/engine_player.cpp)
  — `kPlayerCtrlMgrVA` constant lives here only; `engine_wilbur_ptr()`
  is the sole exported function.
- **CMakeLists**: add `src/coop/engine_player.cpp` to the mtr-asi target.
- **Call-site rewrite**: replaced 1 call in `remote_player_manager.cpp`'s
  anon namespace + 1 in `MtrPlayerManager::local()` + 3 in
  `coop_spawn_probe.cpp` (attach_engine_cm_to_orphan + B7.12 dump path +
  B7.13 mirror path). No call site re-introduces the raw VA. Per RULE №2,
  the old `read_engine_wilbur_seh` helper and `kPlayerCtrlMgrVA` constant
  were deleted from both files entirely — no parallel paths.

After: only `coop/engine_player.cpp` knows the 0x728A40 VA, and any
future engine-side state on the wilbur singleton extends that module
rather than re-leaking VAs into gameplay-layer files.

---

## 2-agent audit findings (all addressed in same session)

### Audit Fix A — Observability gap (correctness/observability, 82% conf)

**Finding**: heartbeat suppresses the interp tail line when all three
counters are zero. When the test driver is active but no Remote is
registered yet (e.g. before coop_spawn_probe fires its B7.2 keep-alive
path), counters stay zero → log shows no signal the driver is on. A log
observer cannot distinguish "driver active, no Remote yet" from "driver
disabled".

**Fix**: one-shot `[mtr_player_manager] interp test driver: ENABLED ...`
log on the first pulse where the flag is detected. Verified in trace
`tools/test-runs/20260512-100344-load-save-1-show-ingame/mtr-asi.log:183`,
appearing ~15s ahead of the first interp counter heartbeat.

### Audit Fix B — Principle 7 violation (architecture, 85% conf)

Described above. Extracted to `mtr::coop::engine_player` module.

### Audit Fix C — Comment doc nit (correctness audit, low conf)

Flag string is 31 chars, not 35 as the comment claimed. Comment updated
to match reality AND to state the substantive reason (no other flag is a
prefix/substring, so accidental match is unreachable).

### Audit clean on the rest

Reviewer 2 (correctness) explicitly verified 7 specific concerns clean:
- Race/ordering on capture-outside / push-inside split — safe (same
  sim thread; value-copy semantics).
- TOCTOU — none introduced.
- Static-init thread-safety on `remote_interp_test_enabled` — C++11
  magic statics, fine.
- Iterator invalidation — neither inner loop can mutate `m_players` (no
  call reaches back into the manager).
- `interp_pushes` vs `interp_writes` double-counting — separate
  counters, no double-counting possible.
- First-tick-after-registration behaviour — `prev.valid=false` path
  correctly snaps to `curr` via the
  `if (!prev.valid) out = b_snap` branch in `apply_entity_pose_interp`.

---

## Validation

Two soak runs, both `load-save-1-show-ingame` with
`-mtrasi-coop-keep-orphan -mtrasi-coop-mirror-registry
-mtrasi-coop-remote-interp-test`:

- **Pre-audit-fixes** (Phase 1.2 driver wired, `read_engine_wilbur_seh`
  duplicated, no ENABLED log): `frames=4550 result=pass`, 3600-frame soak
  clean. Trace `tools/test-runs/20260512-095618-load-save-1-show-ingame/`.
  Heartbeat shows `interp_writes=interp_pushes=p0_pulses faults=0` once
  the orphan registers (~pulse 870).
- **Post-audit-fixes** (engine_player module extracted, ENABLED log
  added, comment fixed): `frames=4550 result=pass`, 3600-frame soak
  clean. Trace `tools/test-runs/20260512-100344-load-save-1-show-ingame/`.
  Same `interp_writes=interp_pushes=p0_pulses faults=0` shape.
  ENABLED log fires at `16:03:47.669`, ~15s ahead of the first interp
  heartbeat.

Both runs identical to pre-Phase-1.2 baseline in terms of overall stability
(no new faults, no regressions). The added work per tick (one pose capture
+ one push per Remote) added zero observable cost at the soak grain.

Build: `mtr-asi.asi` 701952 → 702464 (Phase 1.2 wiring +512 bytes) →
703488 (audit-fix refactor +1024 bytes, the new engine_player.cpp TU
contributing most).

---

## Counter readback (proof of end-to-end)

From the post-audit-fixes trace, the first interp heartbeat at pulse 900:

```
[16:04:02.820] [mtr_player_manager] pulse: total=900 players=2
                p0_pulses=26 p1_pulses=26
                interp_writes=26 pushes=26 faults=0
```

- `players=2`: local (engine_wilbur wrapper) + remote (orphan wrapper).
- `p0_pulses == p1_pulses == 26`: both wrappers ticking from the same
  manager pulse loop.
- `interp_writes == interp_pushes == p0_pulses`: every Remote pulse both
  pushed and wrote — pipeline running end-to-end.
- `faults == 0`: no SEH AVs across all 3600 orphan-alive frames.

The orphan visually shadows engine_wilbur with one-tick lag (~17ms at
60Hz sim). At sim-tick push cadence, alpha sits near 1.0 (we're always
rendering close to the latest pose), so the orphan tracks tightly. When
the future network packet path lands, the push cadence drops to net rate
(~10-15Hz) and alpha grows visibly 0→1 across the post-snapshot window —
the actual Quake-style smoothing that Phase 1.1 designed for.

---

## Files touched this session

**Modified**:
- `src/mtr-asi/CMakeLists.txt` — add `src/coop/engine_player.cpp`.
- `src/mtr-asi/src/coop/coop_spawn_probe.cpp` — drop the local
  `kPlayerCtrlMgrVA` constant + 3 inline VA derefs; call
  `engine_player::engine_wilbur_ptr()` instead.
- `src/mtr-asi/src/coop/remote_player_manager.cpp` — Phase 1.2 driver
  block + one-shot ENABLED log + drop local `kPlayerCtrlMgrVA` /
  `read_engine_wilbur_seh` + comment fix (31 chars).

**New**:
- `src/mtr-asi/include/mtr/coop/engine_player.h`
- `src/mtr-asi/src/coop/engine_player.cpp`
- `research/findings/coop-mtr-remote-player-interp-test-driver-2026-05-12.md`
  (this doc)

---

## Repro

```pwsh
pwsh ./tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy `
    -TimeoutSec 180 `
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-mirror-registry -mtrasi-coop-remote-interp-test'
```

Expected: `frames=4550 result=pass`, `detail: F/RunProbe+Soak: probe ok;
soaked 3600 frames clean (min_tcount=1)`. Log should contain:
- One `[mtr_player_manager] interp test driver: ENABLED ...` line near
  the top.
- After the orphan registers (~pulse 870), every heartbeat (every 60
  pulses) shows non-zero `interp_writes`/`pushes`/`faults=0` tail.

---

## Next session pickup

1. **Phase 1.3 — input buffer scaffold**: MTA `m_SyncBuffer` /
   `AddKeysync` / `UpdateKeysync` shape on `MtrRemotePlayer`. Lands AFTER
   the network packet format is decided so the buffer entry type matches
   what the wire will deliver (RULE №2: don't build against an undefined
   contract). Until the packet format lands, this is blocked — pick
   Phase 1.4 or revisit the packet-format decision first.
2. **Phase 1.4 — network transport**: UDP socket + packet framing. This
   is the prerequisite for 1.3 in practical terms (decides the buffer
   entry type).
3. **C2/C3 watch**: only run a longer soak if a specific crash address
   surfaces. Not before — that's the soak-treadmill anti-pattern.
4. **Visual confirmation of interp test driver**: optional. With the
   driver on at sim-tick rate, the orphan tracks engine_wilbur within
   one tick — barely visible. To make it visually obvious, a debug-only
   "slower push cadence" option could be added (e.g. push every N=10
   ticks → ~166ms window), which would make the Quake-style interp
   visible in-engine. Low priority; the counter readback already proves
   correctness.

Two minor open probes (`[0xF8E718]`, `this+4` layout) still remain
nice-to-have.

---

## Pattern reinforced

This session ran end-to-end on:
1. **No-questions-from-user rule** (`feedback_no_questions_when_rule1_dictates.md`):
   "Go" → continue from the documented next-step pickup item in memory.
   No agent re-consultation needed — the prior session had already done
   the 3-agent next-step consensus and recorded Phase 1.2 as the next
   item. Re-running 3 agents here would be redundant.
2. **Standing audit pattern** (`feedback_audit_pattern.md`): after
   shipping the test driver, ran 2 audit agents in parallel (domain-
   fidelity + correctness). One returned clean; the other caught two
   real issues (observability gap + principle-7 violation). Both fixed
   in the same session before declaring shipped — soak re-run clean.
3. **RULE №1 + RULE №2**: principle-7 violation pre-existed Phase 1.2;
   we fixed it properly (extract module, delete duplicated constant +
   helper completely, update all call sites) rather than leaving the
   pre-existing violation in place "because it pre-existed". No
   migration baggage — old `kPlayerCtrlMgrVA` constants and
   `read_engine_wilbur_seh` helpers were deleted entirely.
