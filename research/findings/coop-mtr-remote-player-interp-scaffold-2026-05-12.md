# `MtrRemotePlayer` interp scaffold — SHIPPED 2026-05-12

**Date:** 2026-05-12 (afternoon).
**Status:** SHIPPED. Build green, 3-agent design consensus, 2-agent audit
applied. Soak `load-save-1-show-ingame` PASS at 4550 frames (3600-frame
orphan-alive window clean). Companion to (and partial supersession of)
[coop-mtr-remote-player-design-2026-05-12.md](coop-mtr-remote-player-design-2026-05-12.md).

> Architectural principle 3: *"Parallel class hierarchy mirroring engine
> structures."* This session adds the first piece of MTA-shape interp
> state to `MtrRemotePlayer`, matching CClientPed's `m_interp`.

---

## Why interp first (not input buffer)

Three agents in parallel (architect/reviewer/explorer) converged on this
ordering against the four documented next-session items from the prior
checkpoint. Quoting the architect agent:

> The data dependency runs one way: `SetTargetPosition → m_interp →
> Interpolate()` in every pulse. This chain has no dependency on
> `m_SyncBuffer` whatsoever. You can populate `m_interp.pos` manually
> (e.g., by hardcoding a target position in `do_pulse`) to prove the
> lerp is working visually, with zero networking code written.

MTA precedent (verified against tree):
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.h` lines
  751-769 — the `m_interp` struct on `CClientPed`. Plain struct, no net
  dependency.
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.cpp`
  - line 3401 `Interpolate()` — read side; called every pulse from
    `DoPulse` at line 2873.
  - line 5370 `SetTargetPosition(pos, ulDelay, contactEntity)` — write
    side; fills `m_interp.pos.*` from position + deadline.
- `reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp` line
  991 confirms the receive-side wiring is independent: puresync calls
  `pPlayer->SetTargetPosition(...)`. Keysync (line 719) calls
  `SetControllerState(...)` — wholly different field
  (`m_SyncBuffer`/`AddKeysync` at CClientPed.h:96 + 690). The two fields
  are independent on CClientPed, so MTA built them sequentially:
  interp first, input buffer when network packet shape was known.

Items 1 (C2/C3 longer soak), 2 (per-player input buffer), 4 (minor
probes) were the alternatives. RULE №1 review explicitly rejected (1)
as the soak-treadmill anti-pattern and (4) as procrastination. (2) was
deferred because, per MTA's own ordering and our missing network packet
format, building an input buffer now would land against an undefined
contract and risk RULE №2 baggage when the format lands.

---

## Design (MTR-specific)

MTA's `m_interp` model is **target+deadline**: store `vecTarget`,
`vecError`, `ulStartTime`, `ulFinishTime`, and each pulse re-read the
engine ped's current pos to lerp `current_pos + vecError * alpha`. This
relies on a live engine-driven ped position that the dead-reckoner
nudges toward target.

For MTR's orphan, the engine does NOT drive the position between net
ticks (we ARE the simulation for the orphan). So we use Quake-style
**snapshot interpolation**: store `prev` and `curr` poses + their qpc
timestamps, render delayed by one window:

```
   alpha = (now - curr.qpc) / (curr.qpc - prev.qpc),  clamped [0, 1]
```

At `now == curr.qpc` alpha=0 → render `prev`. At `now == curr.qpc +
window` alpha=1 → render `curr`. Between, smooth blend. Past one
window without a fresh push, alpha clamps at 1 (hold `curr`).

The two designs converge for steady-state ~13Hz network with no jitter,
but Quake-style is more robust when the engine writes nothing between
snapshots (our case) and MTA-style is more robust when the engine is
already moving the entity (their case). The code-architect agent's
recommendation and the audit agent's MTA-fidelity question both
confirmed snapshot interp is correct for MTR.

Single-window LAN lag: at 13Hz that's ~77ms, identical to what classic
Quake/HL use.

---

## Files shipped

### Public API additions (`mtr::interp`)

`src/mtr-asi/include/mtr/interp.h`:

```cpp
struct EntityPose {
    float    pos[3];
    float    rot[9];
    uint64_t qpc;
    bool     valid;
};

uint64_t qpc_now();
bool     capture_entity_pose(const void* engine_entity, EntityPose& out);
void     apply_entity_pose_interp(const EntityPose& prev,
                                  const EntityPose& curr,
                                  float alpha,
                                  void* engine_entity);
```

`src/mtr-asi/src/interp.cpp`: thin forwarders to the existing private
`detail::PlayerSnap` / `detail::capture_pos_rot` / `detail::compose_
player_interp` / `detail::write_pos_rot`. Layout equivalence enforced
by static_asserts on size + offsetof for each field.

Principle 7 split honoured: engine entity layout knowledge (`+0x58
pos`, `+0x70 rot`) stays in interp's TU. `coop/` holds the public
`EntityPose` struct only — no `#include "interp/interp_internal.h"`.

### `MtrRemotePlayer` additions

`src/mtr-asi/include/mtr/coop/remote_player.h`:
- `#include "mtr/interp.h"`.
- New fields: `m_prev_snap, m_curr_snap` (EntityPose),
  `m_interp_enabled` (bool, default false), `m_interp_writes`,
  `m_interp_pushes`, `m_interp_faults` (uint64_t counters).
- New methods: `push_interp_snapshot(pos[3], rot[9])`,
  `set_interp_enabled(bool)`, `interp_enabled()`, `interp_writes()`,
  `interp_pushes()`, `interp_faults()`.
- `m_role` made `const Role` (audit fix: was non-const, only protection
  against Local-player interp writes was the do_pulse runtime guard).

`src/mtr-asi/src/coop/remote_player.cpp`:
- `do_pulse()` extended: when enabled + role=Remote + curr.valid,
  compute Quake-style alpha and apply via `mtr::interp::apply_entity_
  pose_interp`, all wrapped in SEH for stale-entity recovery.
- `push_interp_snapshot()`: shifts prev←curr, fills curr + qpc, bumps
  push counter.

### Manager heartbeat extension

`src/mtr-asi/src/coop/remote_player_manager.cpp`:
- `do_pulse()` heartbeat now also snapshots aggregate interp counters
  under the lock and emits them on a tail line **only when any are
  non-zero** (keeps the inert-scaffold case zero-overhead in logs).

---

## Audit findings + fixes (applied in same session)

Two-agent parallel audit (domain-fidelity + correctness) caught two
medium-severity issues before shipping.

### Fix 1 — SEH path called `log::info` under `m_mu`

`do_pulse`'s `__except` originally called `mtr::log::info` to report
the stale-entity AV. But the manager calls `MtrRemotePlayer::do_pulse`
under `MtrPlayerManager::m_mu` (see remote_player_manager.cpp:215+),
and `log::info` acquires log's own mutex `s_mu`. This is exactly the
lock-order hazard the 2026-05-12-a audit fix established for the
ctor/dtor (which are intentionally silent for this reason).

**Fix**: removed the log call from the SEH path; added
`m_interp_faults` counter incremented inside `__except`; the manager
snapshots it under the lock and reports outside the lock alongside
writes/pushes in the periodic heartbeat.

### Fix 2 — Permanent disable on first AV was a RULE №1 crutch

Original `__except` cleared `m_interp_enabled = false`, permanently
freezing the entity at whatever pose held when the AV fired. This is
the catch-and-ignore shape RULE №1 prohibits — masking the stale-ptr
problem without root-cause fix or self-heal path.

**Fix**: in `__except`, null `m_engine_entity` (it's dead) and clear
`m_prev_snap.valid = m_curr_snap.valid = false`. A future
`push_interp_snapshot` + fresh `register_remote` cleanly re-primes
without permanently disabling interp on the wrapper. MTA precedent:
`CClientPed` handles stream-out by nulling `m_pPlayerPed` and skipping
`Interpolate()` until the streamer reattaches.

### Fix 3 — `m_role` non-const

Audit flagged: `m_role` was mutable, only do_pulse's runtime guard
prevented Local-player interp writes if the role were ever flipped.
**Fix**: `const Role m_role`. Compile-time enforcement of the
documented immutability invariant. Deleted copy/move assignment
operators meant no impact.

### Documentation strengthened

`push_interp_snapshot`'s comment was upgraded from "single-writer
assumed" to "MUST be called while holding MtrPlayerManager::m_mu" so
future network-recv callers don't accidentally break the contract.

---

## Validation

Three runs `load-save-1-show-ingame` with `-mtrasi-coop-keep-orphan
-mtrasi-coop-mirror-registry`:

- **Pre-audit scaffold** (interp default OFF): `frames=4550 result=pass`,
  3600-frame soak clean. Trace `tools/test-runs/20260512-093258-load-
  save-1-show-ingame/`.
- **Post-audit scaffold** (audit fixes applied, interp default OFF):
  `frames=4550 result=pass`, 3600-frame soak clean. Trace
  `tools/test-runs/20260512-093852-load-save-1-show-ingame/`.

Both runs identical to pre-scaffold post-retirement baseline. The
scaffold is **inert by default**: do_pulse short-circuits on
`!m_interp_enabled` → existing soak behaviour unchanged. The
heartbeat's interp tail line did not fire (all counters zero).

Build: `mtr-asi.asi` 700928 → 701952 (+1024 bytes for scaffold +
audit-fix counters + heartbeat extension).

---

## What's NOT validated yet (deferred to next session)

The scaffold ships **inert by default**. We have NOT yet:

1. Exercised `push_interp_snapshot` end-to-end (no caller wired).
2. Exercised the `do_pulse` blend write on a live entity (no caller
   sets `set_interp_enabled(true)`).
3. Exercised the SEH self-heal path (no stale-entity scenario).

These need a **test driver** — a small Phase 1.2 addition that, gated
by a cmdline flag like `-mtrasi-coop-remote-interp-test`, reads
`engine_wilbur`'s pose each manager pulse and pushes it as a snapshot
to the orphan via `push_interp_snapshot`. The orphan should then
slide-track engine_wilbur with one-snapshot-window of visual delay.
This proves the wiring without any networking code.

This was intentionally scoped OUT of the scaffold milestone to keep
"plumbing" and "first user" as separate decisions per the agent
consensus. The scaffold lands clean and inert; the test driver lands
next.

---

## Post-scaffold roadmap (revised from prior checkpoint)

1. **Phase 1.2 — interp test driver**: cmdline-gated self-test that
   drives the orphan's pose from engine_wilbur each pulse. ~30 LOC in
   remote_player_manager.cpp. Validates the scaffold end-to-end.
2. **Phase 1.3 — input buffer scaffold** (MTA `m_SyncBuffer` /
   `AddKeysync` / `UpdateKeysync`): per-player input ring on
   MtrRemotePlayer. Lands AFTER the network packet shape is decided so
   the buffer entry type matches what the wire will deliver. Per
   RULE №2 we don't want to build it against an undefined contract.
3. **Phase 1.4 — network transport**: UDP socket + packet framing.
   Lands AFTER scaffold + buffer so the receive path has somewhere
   well-defined to write.
4. **C2/C3 watch**: only run a longer soak if a specific crash address
   surfaces in normal gameplay or the test driver. Not before — that's
   the soak-treadmill anti-pattern per RULE №1.

The two minor open probes (`[0xF8E718]`, `this+4` layout) remain
nice-to-have; resolve opportunistically when adjacent work touches
those reads.
