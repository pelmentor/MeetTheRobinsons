# Coop Phase 1.4b — NetSession UDP transport SHIPPED (2026-05-12 night)

Continuation of the Phase 1.4b.1 dual-launch bypass earlier this evening.
This session adds the actual UDP transport (Phase 1.4b) plus the
host-side pose emission and client-side dispatch into the existing
MtrRemotePlayer interp scaffold (Phase 1.4c integration).

## Headline

`mtr-asi` now ships a complete Phase 1.4b/c send + recv pipeline gated
on the existing co-op cmdline flags. Two-machine OR two-process-same-
machine sessions can exchange pose snapshots at sim-rate over raw
Winsock UDP. NetSession is pure transport (no engine, no packet types);
MtrPlayerManager owns the gameplay-layer semantics. Protocol bumped to
v2 same session per audit (16-bit time_ctx).

## 3-agent consensus

Three agents in parallel (architect, code-reviewer, MTA-precedent
explorer) on the design question. Convergent recommendations:

- **NetSession = pure UDP I/O.** No knowledge of `PoseSnapshotBody` or
  `PacketHeader`. The recv callback (a lambda in `dllmain.cpp`) does
  the parse + dispatch. Matches MTA's `CNet` / `CNetAPI` split exactly
  (`reference/mtasa-blue/Client/sdk/net/CNet.h` lines 86-88).
- **Shared `cmdline_utils` module.** Three independent copies of
  `cmdline_has_flag` existed (`coop_dual_launch.cpp`,
  `coop_registry_mirror.cpp`, raw-strstr in `remote_player_manager.cpp`).
  Per RULE №2, dedupe immediately rather than add a fourth.
- **Recv thread daemon-style** with `SO_RCVTIMEO=250ms` + atomic stop
  flag + `closesocket` in dtor. Handles Windows quirk that
  `closesocket()` from another thread doesn't reliably wake a blocked
  `recvfrom` with `WSAEINTR`.
- **send OUTSIDE m_mu.** Capture pose / encode / send happen after the
  manager's existing lock-scope releases. Reviewer P3 hazard
  (theoretical sendto blocking with lock held) eliminated at root.
- **explicit (uint16_t)(in - last) cast** for modular subtraction
  (reviewer P18). C++ promotes uint8/uint16 - uint8/uint16 to signed
  int; explicit cast recovers wrap-around semantics.
- **inet_pton not inet_addr.** inet_addr is ambiguous on broadcast.
- **client `connect()`s the UDP socket.** Kernel filters recv to the
  connected peer at zero runtime cost. Free P19 defense.
- **`by_player_id_locked` internal variant** to keep lookup +
  push_interp_snapshot_at on a single mutex acquisition.

### MTA precedent extracted

- `CNet` is purely abstract; concrete impl lives in a separately-loaded
  RakNet `net.dll`. We use a concrete `NetSession` since we're not
  RakNet-backed.
- Client-side MTA runs all packet handling on the main game thread via
  RakNet's `DoPulse` callback (no cross-thread). Server side has a
  dedicated sync thread + 2-queue model. Our raw-UDP architecture maps
  more closely to MTA's *server* threading: a dedicated recv thread
  marshals into the game thread via a lock-protected method
  (`on_remote_packet`).
- MTA gates puresync emit at 100ms (`CTickRateSettings.iPureSync`). We
  emit every sim pulse (60Hz). For 2-player localhost the 6x bandwidth
  is 3.6 KB/s — negligible. Audit flagged it (80% conf) but it's a
  Principle-5-deferred decision: 60Hz emit means tighter interp window
  (16ms vs 100ms) and no extra knob to maintain. Add a gate if WAN
  scale becomes a concern.
- MTA uses RakNet's `UNRELIABLE_SEQUENCED` (16-bit seq). We needed our
  own. Started with 8-bit `time_ctx` (Phase 1.4a wire format); audit
  caught the 4.27s wrap / 2.1s ambiguity hazard. Bumped to 16-bit
  same session — wraps every ~18 min, half-window ~9 min, past any
  realistic delay.

## What shipped

### New files
- `src/mtr-asi/include/mtr/cmdline_utils.h` — `has_flag`,
  `get_flag_value` (whole-word-matching value extractor).
- `src/mtr-asi/src/cmdline_utils.cpp` — implementation, ~70 lines.
- `src/mtr-asi/include/mtr/coop/net/net_session.h` — public surface
  for the transport.
- `src/mtr-asi/src/coop/net/net_session.cpp` — Winsock UDP impl,
  ~330 lines.

### Modified files
- `src/mtr-asi/include/mtr/coop/remote_player.h` — added
  `push_interp_snapshot_at(pos, rot, qpc)` overload, `m_last_time_ctx`
  (uint16_t, init 0xFFFF), `last_time_ctx()`, `update_time_ctx()`.
- `src/mtr-asi/src/coop/remote_player.cpp` — `push_interp_snapshot`
  now delegates to `push_interp_snapshot_at(pos, rot, qpc_now())`;
  new method implements the actual snapshot write.
- `src/mtr-asi/include/mtr/coop/remote_player_manager.h` — added
  `on_remote_packet`, `by_player_id_locked`, `m_send_time_ctx`
  (uint16_t), `m_first_recv_logged`, `m_unknown_id_logged`, fwd-decl
  of `net::PoseSnapshotBody`.
- `src/mtr-asi/src/coop/remote_player_manager.cpp` — implemented
  `on_remote_packet` with snapshot-then-log discipline, send block at
  end of `do_pulse` (OUTSIDE m_mu), session-aware wire-id in
  `register_local`/`register_remote`, migrated
  `remote_interp_test_enabled` to shared helper.
- `src/mtr-asi/src/coop/coop_dual_launch.cpp` — replaced local
  `cmdline_has_flag` with `mtr::cmdline_utils::has_flag`.
- `src/mtr-asi/src/coop/coop_registry_mirror.cpp` — same migration.
- `src/mtr-asi/include/mtr/coop/net/remote_pose_packet.h` — bumped
  `kProtocolVersion` to 2, widened `time_ctx` to uint16_t,
  re-pad layout: `{uint8_t player_id; uint8_t _pad; uint16_t
  time_ctx; float pos[3]; float rot[9];}` — wire size unchanged at
  52 bytes.
- `src/mtr-asi/src/coop/net/remote_pose_packet.cpp` — `_pad` is now
  scalar (was `_pad[2]`).
- `src/mtr-asi/src/dllmain.cpp` — added forward-include of net_session.h
  + remote_pose_packet.h; recv callback lambda + `install()` call
  immediately after `MtrPlayerManager::install()`.
- `src/mtr-asi/CMakeLists.txt` — added `src/cmdline_utils.cpp`,
  `src/coop/net/net_session.cpp`; added `ws2_32` to link libs.

### Cmdline contract
- `-mtrasi-coop-host PORT` — bind 0.0.0.0:PORT, learn peer addr from
  first inbound datagram, then send to it.
- `-mtrasi-coop-connect IP:PORT` — connect() the UDP socket to peer
  (filters recv at kernel level; uses `send()` instead of `sendto`).
- Wire IDs: host = 0, client = 1. `register_local` / `register_remote`
  consult `NetSession::local_wire_id()` / `remote_wire_id()` when the
  session is active; fall back to `m_next_player_id++` when not.

### Cmdline parsing edge cases handled
- `parse_port` rejects 0, >65535, non-numeric, trailing garbage.
- `parse_ip_port` rejects missing colon, empty IP, bad IPv4 (via
  `inet_pton`), bad port.
- `get_flag_value` rejects value starting with `-` (= next flag, not
  a value), flag at end-of-cmdline, value too long for buffer.

## 2-agent audit (after initial ship)

### Correctness audit findings (≥70% conf)

- **(88%) Log use-after-shutdown in NetSession dtor on FreeLibrary
  path** — `mtr::log::shutdown()` runs in DllMain; NetSession's static
  dtor runs after; recv thread's `"recv thread exiting"` log fires
  through a shut-down log subsystem. **FIXED same session**: removed
  the exit-log entirely. The "started" log on entry is safe (runs
  during `install()` which always precedes any log shutdown).
- **(80%) `get_flag_value` early-return at end-of-cmdline** — the
  audit's reasoning was actually flawed (strstr returns the EARLIEST
  match; a NUL-terminator after the flag means no later match is
  possible). **No code change** but updated the comment to document
  the analysis so a future reader doesn't repeat the audit's
  mistake. 0% functional change.
- **(72%) `m_peer_addr` comment misleading** — the implementation IS
  correct (acquire-release on `m_peer_known` provides the
  happens-before ordering for `m_peer_addr` writes/reads). The
  "benignly racy" comment was wrong. **FIXED same session**: comment
  rewritten to explain the acquire/release contract accurately so a
  future cleanup doesn't break it.

### Domain-fidelity audit findings (≥70% conf)

- **(85%) 8-bit time_ctx wraps in 4.27s, ambiguity at 2.1s** —
  **FIXED same session**: widened to uint16_t, bumped kProtocolVersion
  to 2 (RULE №2 — no compat shim). Wire size unchanged at 52 B.
- **(80%) No rate-gate on send (MTA uses 100ms)** — **DEFERRED with
  documentation** (Principle 5). 3.6 KB/s at 60Hz is negligible; rate
  gate intentionally reduces snapshot freshness for no current
  benefit. Revisit if WAN or scale becomes a concern.
- **(75%) `note_bad_packet` on public surface = minor Principle-7
  leak** — **FIXED same session**: `RecvCallback` now returns bool
  (true=accepted, false=malformed/unknown). `recv_loop` owns the
  counter; the gameplay-layer callback only votes pass/fail per
  packet, never writes transport state directly. `note_bad_packet`
  public method removed.
- **(72%) Blocking-send hazard acknowledged + deferred to "if it
  fires" comment** — **DEFERRED with documentation update**. The
  audit's recommendation (`FIONBIO`) conflicts with the SO_RCVTIMEO-
  based recv-thread design (FIONBIO is socket-wide; would force a
  separate handling for recv). Per Principle 5: 60-byte UDP sendto on
  localhost is empirically sub-µs; the send_errors counter and rate-
  limited logging surface any pathological case if it ever occurs.
  Not a "fix later" deferral — a measured MVP scope decision.

### Build state

- 706048 (pre-Phase-1.4b) → 712192 (initial ship) → 712192
  (post-audit-fix). Audit fixes were type widenings (uint8→uint16
  in 3 places, net +4 B) + log-removal (recv-loop exit line, ~50 B) +
  bool return type (~0 B) — net-zero.
- Deployed to `Game/mtr-asi.asi`.
- Build clean both passes, no warnings.

## Live-test in isolation

Phase 1.4b/c is testable now:

```
Game\Wilbur.exe -mtrasi-coop-host 7777 -dxresolution=1280x720 -launchit
Game\Wilbur.exe -mtrasi-coop-connect 127.0.0.1:7777 -dxresolution=1280x720 -launchit
```

Expected behaviour:

- Both processes start (dual-launch bypass from earlier this session).
- Each emits `[net_session] HOST: bound 0.0.0.0:7777, waiting for
  peer's first datagram` / `[net_session] CLIENT: connected to 127.0.0.1:7777`.
- Each emits `[net_session] recv thread started`.
- Once each process reaches a state where `engine_player::engine_wilbur_ptr()`
  returns non-NULL (typically after main menu → into a level), the
  `do_pulse` send block fires every sim tick.
- Client sends first → host's recv thread emits `[net_session] HOST:
  learnt peer addr 127.0.0.1:<ephemeral>`. Host's send block starts
  hitting send().
- Heartbeat in `MtrPlayerManager::do_pulse` (every 60 pulses ≈ 1s)
  shows `interp_writes` rising on the side that has a registered
  MtrRemotePlayer for the peer's wire_id.

The send/recv plumbing is testable BEFORE remote-player wrappers exist
on either side. Sender always emits; receiver's `on_remote_packet`
logs the first "no remote wrapper for wire player_id=X" once and
silently drops subsequent. When the orphan spawn pipeline materialises
a remote wrapper (Phase 2 work, partial), the `on_remote_packet` path
ramps up `interp_writes` automatically.

## Pattern reinforced

- **3-agent + 2-agent pattern continues to pay off**. The architect
  produced a complete blueprint; the reviewer caught the
  P3/P14/P18/etc. concrete pitfalls before they shipped; the explorer
  surfaced exactly the MTA precedents we needed (CNet split, server-
  side sync-thread pattern, TICK_RATE, RakNet UNRELIABLE_SEQUENCED).
- **Post-ship audit catches what the design phase misses**. F1 (log
  use-after-shutdown) wasn't visible in the design phase — it only
  emerged from inspecting the actual call sequence in the shipped
  code. The standing pattern (2 audit agents after substantial work)
  pulls its weight every session.
- **RULE №2 dedup is mandatory not optional**. Three copies of
  `cmdline_has_flag` were one short away from a fourth. Catching it
  in the same session that would have added the fourth is what RULE №2
  is for.
- **Wire-format versioning is cheap when no compat shim**. v1 → v2
  bump for the 8→16-bit time_ctx widening cost zero — we ship one
  protocol at a time per RULE №2; older senders simply get their
  packets dropped on version mismatch.

## Next session pickup — Phase 1.4d (orphan wiring) and Phase 1.5

### Phase 1.4d — orphan auto-register on the client side

For the client side of a session to actually display the host's
movement, an orphan wilbur must exist locally and be wrapped via
`MtrPlayerManager::register_remote`. Currently
`coop_spawn_probe.cpp:1815` is the sole call site, gated on the
`try_spawn_p2` SX command path. In a session this needs to fire
automatically: when NetSession recv sees a packet with the remote
wire id but no wrapper exists, the manager could ask coop_spawn_probe
to materialise one. Alternatively: the dual-launch session implies a
co-op scenario the level scripting will trigger.

Pick this up by first verifying what currently triggers
`try_spawn_p2` — if it's a cmdline / scripting flag, set it
implicitly when `-mtrasi-coop-host`/`-connect` is present. If it's
in-level, the test scenario needs to enter a level that triggers it
naturally.

### Phase 1.5 — retire Phase 1.2 test driver

`-mtrasi-coop-remote-interp-test` was always a transitional validator
(per the prior memory checkpoint). Once Phase 1.4d shows live-test
end-to-end success (host wilbur visible on client's screen with
interp blending), this flag is RULE-№2 retirement bait. Plan:

1. Confirm 1.4d works end-to-end (host pose → client wilbur via UDP).
2. Delete the `remote_interp_test_enabled()` magic-static + driver
   branch in `MtrPlayerManager::do_pulse` (lines 244-258 currently).
3. Delete the related "interp test driver: ENABLED" announce log.

### Phase 1.5+ — optional rate gate

The 80%-confidence audit finding (MTA-style 100ms TICK_RATE) is
deferred. If WAN or scale becomes a concern, add a manager-side
`m_last_send_ms` field + `GetTickCount64()` comparison in `do_pulse`
before the send block. Sites: gameplay layer only — transport stays
neutral.

### Phase 1.5+ — optional `register_remote(entity, wire_id)`

Currently `register_remote` consults `NetSession::remote_wire_id()`
internally when active. This works for the 2-player MVP. If a future
phase brings more than one remote, the API should take an explicit
wire id rather than auto-derive — but that's >2-player scope which
is out of MVP.

## Files touched

New:
- `src/mtr-asi/include/mtr/cmdline_utils.h`
- `src/mtr-asi/src/cmdline_utils.cpp`
- `src/mtr-asi/include/mtr/coop/net/net_session.h`
- `src/mtr-asi/src/coop/net/net_session.cpp`
- `research/findings/coop-phase-1-4b-net-session-2026-05-12.md` (this file)

Modified:
- `src/mtr-asi/include/mtr/coop/remote_player.h`
- `src/mtr-asi/src/coop/remote_player.cpp`
- `src/mtr-asi/include/mtr/coop/remote_player_manager.h`
- `src/mtr-asi/src/coop/remote_player_manager.cpp`
- `src/mtr-asi/src/coop/coop_dual_launch.cpp`
- `src/mtr-asi/src/coop/coop_registry_mirror.cpp`
- `src/mtr-asi/include/mtr/coop/net/remote_pose_packet.h` (v2)
- `src/mtr-asi/src/coop/net/remote_pose_packet.cpp`
- `src/mtr-asi/src/dllmain.cpp`
- `src/mtr-asi/CMakeLists.txt`
