# Phase 1.3 vs Phase 1.4 ordering — decision: skip 1.3, do 1.4

**Date**: 2026-05-12 (continuation session)
**Status**: SHIPPED (decision; first sub-step of 1.4 lands this session)
**Predecessors**:
- `research/findings/coop-mtr-remote-player-interp-test-driver-2026-05-12.md`
- `memory/project_state_2026-05-12_interp_test_driver.md`

## Question

The prior session's documented next-pickup left two phases open:

- **Phase 1.3** — per-Remote input buffer, modelled on MTA's
  `m_SyncBuffer` (timestamped frames of replicated input).
- **Phase 1.4** — UDP network transport (sockets + packet framing) for
  the existing pose-sync pipeline.

The memory checkpoint suggested running a fresh 3-agent consensus on
ordering — the original plan had 1.3 first, but per RULE №2 (no
migration baggage) the buffer entry type should match the wire format
exactly, which argued for 1.4 first.

## 3-agent consensus

Three agents in parallel (per the no-questions rule in
`feedback_no_questions_when_rule1_dictates.md`):

### Architect (feature-dev:code-architect)

Recommended **co-design 1.3 + 1.4 as a single phase**. Rationale: the
wire packet and the buffer-entry type cross the same API boundary, so
locking them in together avoids the parallel-old-vs-new-paths
violation. Proposed `RemotePosePacket` (wire) + `PoseFrame` (buffer
entry) types defined together.

### Reviewer (feature-dev:code-reviewer)

Recommended **Option A (1.3 first, then 1.4)** as PROPER. Argued the
architect's premise was wrong: MTA's evidence shows the buffer entry
type is **game-side** (derived from the engine's controller-state
layout), the wire format is **decoded into** that type, the two are
independent. Co-design therefore creates the very baggage RULE №2
prohibits (a type alias spanning layers "for compatibility").

### Explorer (feature-dev:code-explorer)

Reported MTA's actual buffer/packet relationship from the source:

- `m_SyncBuffer` is `std::list<SDelayedSyncData*>` on `CClientPed`
  (`reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.h:96,690`).
- `SDelayedSyncData` is a **post-decode applied-state** struct — holds
  arrival time + discriminant + decompressed `CControllerState` +
  decoded weapon slot/ammo/aim vectors. No packet framing, no sequence
  numbers.
- The **input** path (`PACKET_ID_PLAYER_KEYSYNC`) goes through the
  buffer: `ReadKeysync` → `AddKeysync` → buffer.
- The **pose** path (`PACKET_ID_PLAYER_PURESYNC`) **bypasses the buffer
  entirely**: `ReadPlayerPuresync` → `SetTargetPosition`/`SetTargetRotation`
  → MTA's interpolation subsystem directly (`CNetAPI.cpp:991-992`).

## Decision

The explorer's finding is the kicker. **Our Phase 1.1+1.2 scaffold
(`push_interp_snapshot` → `m_prev_snap`/`m_curr_snap` → Quake-style
alpha blend in `do_pulse`) maps to MTA's puresync path, which doesn't
use the buffer.**

Therefore:

1. **Phase 1.3 (input buffer) is not a prerequisite for Phase 1.4.**
   It's a separate, deferrable concern that becomes relevant only if
   we move from pose-sync to input-replication (e.g., wanting Wilbur's
   animation to be driven by the engine's input pipeline rather than
   pose-blended on top).

2. **Phase 1.4 (network transport) lands directly on top of the
   existing pose-sync scaffold.** Receiver side calls
   `MtrRemotePlayer::push_interp_snapshot(pos, rot)` on each valid
   incoming packet. No buffer layer needed.

3. **Phase 1.3 returns later — or never — based on observed need.**
   Per RULE №2, we don't pre-build the buffer "just in case" we
   switch architectures later. If/when we need input replication,
   that's a separate phase with its own design pass.

This is per CLAUDE.md RULE №1 (proper root-cause architecture, not a
preemptive build-out) and RULE №2 (no parallel paths, no migration
baggage).

## Phase 1.4 plan

### 1.4a — wire format (this session)

- `src/mtr-asi/include/mtr/coop/net/remote_pose_packet.h` — defines
  `PacketHeader` (8 B: magic + version + type + pad) and
  `PoseSnapshotBody` (52 B: player_id + time_ctx + pad + pos[3] +
  rot[9]). Total wire size: 60 bytes per snapshot.
- `src/mtr-asi/src/coop/net/remote_pose_packet.cpp` — `validate_header`
  and `as_pose_snapshot` helpers. No I/O; pure type/byte handling.

Inert until 1.4b lands. Not migration baggage — this is a new feature
being built incrementally, not a replacement of existing code.

### 1.4b — socket lifecycle (next session)

- `NetSession` class: owns the UDP socket; bound listen mode or
  connected send mode. Send/recv loop on its own thread.
- Cmdline-gated activation:
  - `-mtrasi-coop-host PORT` (host: listens on PORT, broadcasts pose).
  - `-mtrasi-coop-connect IP:PORT` (client: receives pose, applies
    via `push_interp_snapshot`).

### 1.4c — integration (next session)

- Host: on each `MtrPlayerManager::do_pulse`, capture local
  `engine_wilbur` pose (same path as 1.2 test driver), serialize as
  `PoseSnapshotBody`, send via `NetSession`.
- Client: `NetSession` on recv → `validate_header` → `as_pose_snapshot`
  → `MtrPlayerManager::on_remote_packet(pkt)` (new method) →
  `MtrRemotePlayer::push_interp_snapshot`.
- Once 1.4c works end-to-end, retire the 1.2 test driver per RULE №2
  (it was always a transitional validator for the 1.1 scaffold; the
  network path is its proper replacement).

### Live-test constraint

The Disney mutex blocks same-machine dual-launch (documented in
existing memory). Live testing 1.4b/c needs either:
- two physical Windows machines on the same LAN, or
- a Disney mutex workaround (rename or pre-acquire-and-rename),
  which itself needs design + RE.

This is a separate concern that surfaces in 1.4b; not blocking 1.4a.

## Packet field selection — Principle 5 anchor

Per `docs/COOP_SCOPE.md`: per-player state covers position, velocity,
animation state, health, current weapon, current ability mode,
interaction target. Phase 1.4a ships **pose only** (pos + rot) because
that matches the existing `EntityPose` scaffold. Velocity / anim_key /
health / weapon become separate sub-phases (1.4d, 1.4e, ...) added
when each is demonstrated necessary — not pre-emptively.

Same is true of the `qpc` field: it's deliberately **not on the wire**.
Sender QPC values are meaningless to the receiver (different PCs,
different counters). Receiver stamps incoming packets with
`mtr::interp::qpc_now()` so interp uses local-clock semantics. Matches
MTA's `CClientTime::GetTime() + ulDelay` pattern in
`CClientPed.cpp:961`.

## What changed in the codebase this session

New files only:
- `src/mtr-asi/include/mtr/coop/net/remote_pose_packet.h`
- `src/mtr-asi/src/coop/net/remote_pose_packet.cpp`

Modified:
- `src/mtr-asi/CMakeLists.txt` — adds new source file.

No existing files touched. The wire format types compile in but have
no callers yet (1.4b/c land next session). Build size remained at
703488 bytes — the new TU's free functions and POD constants get
dead-stripped by `/OPT:REF` because nothing references them yet. That
is expected and proper: an under-construction subsystem with no live
callers is not migration baggage, and `/OPT:REF` keeps the binary
clean of unreferenced symbols.

## 2-agent audit results (post-ship)

Per `feedback_audit_pattern.md`, ran two confidence-filtered (>70%)
audits in parallel:

### Audit 1 — domain fidelity (CLAUDE.md compliance)

**Clean.** No issues at >70% confidence. Checked:
- Principle 7 (engine-VA isolation): clean. No engine headers
  included; no engine constants in net/.
- Principle 5 (minimum viable subset): clean. Fields = pos+rot only;
  velocity / anim / health / weapon explicitly deferred and named in
  doc rather than silently omitted.
- RULE №1 (proper fix): clean. API shape is the proper memcpy-based
  pattern, no quick-fix workarounds.
- RULE №2 (no migration baggage): clean. Version mismatch drops the
  packet (no negotiated downgrade); `PacketType` enum has one value
  and no "legacy" markers.
- Doc/code consistency: clean. All size constants (8 + 52 = 60)
  guarded by static_asserts. Layout-compatibility claim with
  `EntityPose` correctly worded (field-by-field copy of pos/rot
  elements, not whole-struct copy).
- `kPacketMagic = 0x4F43524D` LE byte order: confirmed produces
  'M','R','C','O' on the wire.
- `time_ctx` modular comparison: confirmed RFC 1982 idiom is correct
  for 8-bit serial numbers.

### Audit 2 — correctness (races, UB, ordering, edges)

**One issue at 85% confidence, fixed in same session.**

`encode_pose_snapshot` originally memcpy'd the caller's
`PoseSnapshotBody` verbatim, including the two reserved `_pad[2]`
bytes. The header constructor zeros `hdr._pad = 0` but there was no
corresponding hygiene on the body padding. Risk: (a) wire bytes
become non-deterministic depending on caller stack hygiene, (b) a
small information-disclosure channel where host stack bytes travel
to the client.

Fix: zero `_pad[0]` and `_pad[1]` in a local copy before the body
memcpy:

```cpp
PoseSnapshotBody wire_body = body;
wire_body._pad[0] = 0;
wire_body._pad[1] = 0;
std::memcpy(bytes + sizeof(PacketHeader), &wire_body,
            sizeof(PoseSnapshotBody));
```

The receiver side intentionally does NOT validate `_pad == 0` on
either header or body — that would block a hypothetical v2 sender
that promotes `_pad` to a meaningful field. Silent acceptance is the
correct forward-compatibility posture for a v1 receiver.

All other correctness concerns audited clean: buffer overrun/underrun
guards correct, no integer overflow possible (size_t against compile-
time constants), memcpy is the strict-aliasing-safe pattern, no
nullptr deref paths, no static-init ordering issues (constexpr), no
sign issues (no uint8_t arithmetic in this file), round-trip symmetry
between encode and parse is correct.
