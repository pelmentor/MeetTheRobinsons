# Coop Phase 1.4d — auto-orphan-spawn-on-session SHIPPED (2026-05-12)

Continuation of [coop-phase-1-4b-net-session-2026-05-12.md](coop-phase-1-4b-net-session-2026-05-12.md).
Predecessors: 1.4a wire format → 1.4b UDP transport → **1.4d auto-spawn**.

## TL;DR

The peer's `MtrRemotePlayer` wrapper is no longer gated on a UI button
or test scenario. When a co-op session is active AND a packet has arrived
for an unknown wire id AND no Remote wrapper exists AND the engine is in
gameplay state, the sim-thread `MtrPlayerManager::do_pulse` fires
`coop_spawn_probe::try_spawn_p2()`. The recv thread sets ONE atomic
flag — never touches engine state (Principle 7 preserved).

Two RULE №2 dedups landed in the same change:

1. **Phase 1.2 test driver retired** (`-mtrasi-coop-remote-interp-test`).
   The driver was a parallel pose-delivery path next to the live
   network path. Driver block in `do_pulse` deleted; `remote_interp_
   test_enabled()` helper deleted; one cmdline flag retired.
2. **`live_orphan_entity()` retired**. The probe-internal cache of
   "the last orphan I built" was a parallel surface to the manager's
   own wrapper registry. `coop_vibrate_route` now consults
   `MtrPlayerManager::by_engine_entity()` instead — single source of
   truth (Principle 7).

## Design — 3-agent consensus

Architect / code-reviewer / MTA-explorer in parallel.

### Triggering path (architect's pick: hybrid (a)+(b))

Pure (b) — spawn from recv-thread callback — violates Principle 7.
Pure (a) — spawn at session start — fails because the engine may be in
the main menu when the session opens. Solution: recv thread sets an
atomic `m_peer_seen`, sim-thread polls it.

```cpp
// On recv thread (inside on_remote_packet, under m_mu):
m_peer_seen.store(true, std::memory_order_relaxed);

// On sim thread (top of do_pulse, OUTSIDE m_mu mostly):
if (net.active() && m_peer_seen.load(relaxed)) {
    bool need_spawn;
    { std::scoped_lock lk(m_mu); need_spawn = !has_remote_locked(); }
    if (need_spawn && coop_spawn_probe::is_in_gameplay()) {
        coop_spawn_probe::try_spawn_p2();  // engine work, sim thread
    }
}
```

`m_peer_seen` is a LATCH, not a one-shot. While `is_in_gameplay()`
returns false (menu/loading/cutscene), the check re-runs every sim
tick. Once the engine reaches gameplay, the spawn fires. The
`has_remote_locked()` guard prevents re-firing once an orphan exists.

### MTA precedent

- `reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:929` — `new CClientPlayer(...)` synchronous on game thread.
- `CPacketHandler.cpp:761` — STATUS_JOINED gate (analogue of our `is_in_gameplay()`).
- `CPacketHandler.cpp:1334-1337` — unknown-peer packets DROPPED silently. No queue, no buffer.
- No cmdline flag controls auto-spawn in MTA. Always on once joined.

We collapse MTA's `Packet_PlayerList` + per-player-sync into one
packet type. Per Principle 5 (minimum viable subset for 2-player MVP),
this is intentional. The first pose-snapshot doubles as the join
signal. Documented design debt: if/when >2 players ever scope in, we
need a roster packet.

### Keep-alive coupling

`try_spawn_p2`'s `keep_alive` clause at coop_spawn_probe.cpp:1730
now ORs in `NetSession::active()`:

```cpp
bool keep_alive = keep_orphan_enabled()
                  || mtr::coop::net::NetSession::instance().active();
```

The `-mtrasi-coop-keep-orphan` cmdline flag still works for the
standalone diagnostic probe (UI button without session) — distinct
use case, kept per Principle 5. Session-driven auto-spawn doesn't
need a second flag (RULE №2).

### Spawn position

Was hardcoded `{0,0,0}` (origin — often outside level bounds, can
trip AABB/navmesh asserts). Now reads `engine_wilbur`'s current
position via `mtr::interp::capture_entity_pose` (SEH-wrapped), falls
back to origin only if engine_wilbur is unavailable. Reviewer audit
P2 — root-cause fix per Principle 4.

## 2-agent audit findings + dispositions

### Applied same session

- **(95%) `live_orphan_entity()` returns null in session keep-alive**
  — `coop_vibrate_route` would skip its C1 fix in session mode.
  **Fixed**: `coop_vibrate_route` now consults
  `MtrPlayerManager::by_engine_entity()` directly; `live_orphan_entity()`
  deleted entirely (RULE №2 dedup against the manager's authoritative
  registry).
- **(88%) RULE №2 — Phase 1.2 driver parallel path**.
  **Fixed**: `remote_interp_test_enabled()` helper deleted; driver-
  pose-capture + push loop in `do_pulse` deleted; cmdline flag
  `-mtrasi-coop-remote-interp-test` retired.
- **(82%) Principle-7 latent — `live_orphan_entity` second
  reference to engine entity**. Same fix as 95%: removed.

### Documented as design intent

- **(85%) `m_peer_seen` relaxed atomic ordering** — relaxed is fine
  here because the spawn block re-checks all preconditions under
  m_mu. The auditor's analysis confirmed correctness; no code change.
- **(82%) re-arm race on rapid disconnect/reconnect** — transient,
  self-correcting via next recv packet (which re-sets `m_peer_seen`).
  Documented in code comment.
- **(80%) double `is_in_gameplay()` call** — second call inside
  `try_spawn_p2` is a deliberate TOCTOU re-check on engine state
  that can flip during level transitions. Not a bug.
- **(80%) spawn-on-first-pose-vs-roster — design debt per
  Principle 5**. This document is the written record (Principle 4
  transitional-design-debt: "we know the gap; here's why we accept
  it for MVP; here's what triggers a redesign").

## What shipped

### Modified files
- `src/mtr-asi/include/mtr/coop/remote_player_manager.h` — `<atomic>`
  include, `has_remote_locked()` private decl, `std::atomic<bool>
  m_peer_seen{false}`.
- `src/mtr-asi/src/coop/remote_player_manager.cpp` — `coop_spawn_probe.h`
  include; deleted `remote_interp_test_enabled()` helper +
  `cmdline_utils.h` include; deleted driver block in `do_pulse`;
  added Phase 1.4d auto-spawn block at top of `do_pulse`; sets
  `m_peer_seen` on unknown-id packet; resets one-shot latches in
  `unregister` when last Remote drops.
- `src/mtr-asi/include/mtr/coop_spawn_probe.h` — public
  `bool is_in_gameplay()` decl.
- `src/mtr-asi/src/coop/coop_spawn_probe.cpp` — `net_session.h` +
  `interp.h` includes; moved `is_in_gameplay()` out of anonymous
  namespace; spawn position from `engine_wilbur`'s pose;
  `keep_alive` ORs in `NetSession::active()`; deleted
  `live_orphan_entity()`.
- `src/mtr-asi/src/coop/coop_vibrate_route.cpp` — removed
  `live_orphan_entity` forward decl; added manager includes; route
  check now via `MtrPlayerManager::by_engine_entity()` →
  `MtrRemotePlayer::Role::Remote`; updated install log message.

### Cmdline contract (post-1.4d)

- `-mtrasi-coop-host PORT` — host session.
- `-mtrasi-coop-connect IP:PORT` — client session.
- `-mtrasi-coop-keep-orphan` — manual diagnostic probe keep-alive
  (no session needed). UI-button path; auto-spawn does NOT require
  this flag.
- `-mtrasi-coop-mirror-registry` — Tier-3 registry mirror toggle
  (unchanged).
- `-mtrasi-coop-remote-interp-test` — **RETIRED**. Was the Phase 1.2
  test scaffold; superseded by 1.4b/1.4d live network path.

## Build state

712192 → 712192 B (PE alignment absorbed the net change: +auto-spawn
block, +has_remote_locked, +pose-position lookup, +manager-route in
vibrate; −Phase 1.2 driver block, −`live_orphan_entity()`, −helper).
Both builds clean; zero warnings.

## Live-test plan (unchanged from 1.4b; now end-to-end with NO probe button)

```
Game\Wilbur.exe -mtrasi-coop-host 7777 -dxresolution=1280x720 -launchit
Game\Wilbur.exe -mtrasi-coop-connect 127.0.0.1:7777 -dxresolution=1280x720 -launchit
```

Expected log progression on the side that receives the peer's first
packet:

1. `[net_session] HOST: bound 0.0.0.0:7777, waiting for peer's first datagram`
2. `[net_session] recv thread started`
3. Once both sides are in gameplay (after CONTINUE GAME → save loads):
   - `[mtr_player_manager] on_remote_packet: no remote wrapper for
     wire player_id=X (dropping; future occurrences silent until a
     remote is registered)` — **fires ONCE per session.**
4. Within ≤ 1 sim tick of (3): `coop_spawn_probe: ATTEMPTING factory
   call. screen="..." pre_list_count=N` + the full breadcrumb ladder.
5. `coop_spawn_probe: B7.13 registry mirror: seen=N inserted=N copied=N faults=0`
6. `coop_spawn_probe: phase1 manager registered: mtr_remote_player id=X (count=Y)`
7. `[mtr_player_manager] on_remote_packet: first packet accepted
   (wire player_id=X)` — the FIRST pose snapshot that lands AFTER the
   wrapper exists.
8. Manager heartbeat (every 60 sim pulses ≈ 1s): `interp_writes`
   climbs on the side with the registered wrapper.

The orphan's transform-list entity now blends per-tick toward the
peer's wire-delivered pose. Visual: the peer wilbur's avatar
appears at the local wilbur's position (initial spawn coord),
then snaps to the peer's actual world position on the first interp
sample, then tracks smoothly at ~17ms lag.

## Next session pickup — Phase 1.5 + live test

### Phase 1.5 — live test (user-driven)

End-to-end two-Wilbur dual-launch using the live-test plan above. Verify:
- Both sides log the expected sequence.
- Pose snapshots flow both directions.
- Visual: peer's avatar visible and tracking on each side.
- No crash within first 60s of session.

### Phase 1.6 — input routing (Phase 2 step c)

Currently both wilburs receive ALL local input. Per-player input
routing was the Phase 2 work the original `coop_phase_0c` plan led
to. With auto-spawn shipped and pose flowing, input routing is the
next correctness gate: P1's keyboard must drive ONLY P1's wilbur on
P1's machine, etc.

### Phase 1.6+ — optional: roster packet

If/when scope expands beyond 2 players, MTA's split applies (separate
roster packet from per-player sync). Today's "first pose IS the
join signal" is the Principle-5 MVP choice; it scales to 2 only.

## Patterns reinforced

- **3-agent design + 2-agent audit continues to deliver.** Architect's
  hybrid (a)+(b) was the right call; reviewer caught the
  `live_orphan_entity` 95%-conf shipping bug AND the keep-orphan flag
  death; MTA-explorer extracted exactly the precedents we needed
  (CPacketHandler:929, the no-flag pattern, the unknown-peer drop).
- **Audit finds what design phase can't.** The
  `live_orphan_entity` 95% finding only emerged from inspecting the
  shipped interaction between Phase 1.4d's keep_alive widening and
  the existing `live_orphan_entity` cmdline-flag gate. Same lesson
  as 1.4b's "log after shutdown" 88% — post-ship audits catch
  cross-module coupling that design phase can't see.
- **RULE №2 dedup compounds.** This session retired:
  (a) the Phase 1.2 driver (replaced by 1.4b/1.4d live path),
  (b) `live_orphan_entity()` (replaced by manager registry).
  Both retirements were FORCED by the same Principle-7 question:
  "who is the single source of truth for X?" The answer for pose
  delivery and for wrapper-identity is the manager. The probe is a
  one-shot diagnostic; it doesn't get to maintain parallel state.
- **Principle-5 design debt with written rationale ≠ Principle-4
  crutch.** The "first pose IS the join" choice is documented in
  this file with the redesign trigger (>2 players). That's
  Principle 5 done right: minimum viable, with the scope boundary
  written down so a future principle-4 audit can recognise it as a
  scoped intent rather than as suppression.
