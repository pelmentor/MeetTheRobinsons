# Coop Phase 1 — UDP transport + engine interface design (2026-05-11)

## Status: DESIGN — REVISED post-live-test. See R1/R2/R3 corrections inline + live-test findings doc.

> **CORRECTIONS 2026-05-11 (after live tests):** Three assumptions in this design were invalidated by live-test results in [coop-phase0-live-test-findings-2026-05-11.md](coop-phase0-live-test-findings-2026-05-11.md):
>
> **R1 — Coordinator class is even more minimal than designed.** Only `vtable[5] IsMPActive` was exercised during load-save play (5 distinct caller RAs, all in input-election code). `vtable[9] GetNetworkTime` dormant. Phase 1b drops from 0.5wk → 0.1wk.
>
> **R2 — `entity_install_network_manager` (0x5B0C70) is DORMANT in normal play.** The "POST-hook to inject NetworkManager" install vector below is wrong because the install fn never fires in load-save. Need to find an alternative install path (likely hook Protagonist ctor directly, or `entity_factory_construct` POST). Phase 1d grows from 0.5wk → 1.0-1.5wk to characterize.
>
> **R3 — ~~Phase 2 (input separation) drops substantially.~~ RETRACTED 2026-05-11 evening.** Deeper RE of the registrar (`sub_5A2400` / `sub_5A2690`) revealed the `unk_741920` table is a **cheat-code / Konami-sequence dispatcher**, not gameplay input. The `sub_5A2480` "primitive" disables cheat codes when MP is active — it is NOT a network-input-injection point. Phase 2 reverts to v2 plan's ~4wk estimate. Real gameplay-input separation point is still unidentified — needs follow-up RE. Full correction: [coop-phase0-finding-f2-corrected-2026-05-11.md](coop-phase0-finding-f2-corrected-2026-05-11.md).

Phase 0 is closed. This doc lays out the Phase 1 design — the work that goes from "we have probe-grade stubs and an RE-mapped interface" to "actual networking that runs publish/receive through a real UDP socket". It includes the assumptions that need live verification before locking the design, so the user can react before commitment.

## Scope absorbed into Phase 1

v2 plan split Phase 1 (UDP) and Phase 3 (replication). The Phase 0 finds collapsed Phase 3 into mostly "implement the manager interface", so Phase 1 now bundles:

- **Phase 1a — UDP transport.** Sockets, framing, sequencing, reliability for ordered messages.
- **Phase 1b — Coordinator class.** Real `MultiplayerCoordinator` (replaces the stub).
- **Phase 1c — NetworkManager class.** Real `NetworkManager` (replaces the stub).
- **Phase 1d — Wire to engine.** Install in `g_mp_coordinator_ptr` (0x745BE8) + `entity+216` install hook OR vtable patch.
- **Phase 1e — Bring-up + verification.** Two-process test (run-coop-test.ps1 single-process) until DistributedState round-trip works.

Estimate: **5 wk** total (was 4 wk + 4 wk = 8 wk in v2).

## What we know (locked from Phase 0)

### Coordinator interface (`g_mp_coordinator_ptr` @ 0x745BE8)

Engine consumers call:

| Slot | Offset | Signature | Use |
|---|---|---|---|
| 5 | 20 | `bool IsMPActive()` | Global activation gate; 12 consumers check this |
| 9 | 36 | `int GetNetworkTime()` | Time-sync; replaces `dword_6FFCB4` in time-of-day-driven code |
| 15 | 60 | unknown (input election) | Used in `sub_5A2980` input path |
| 19 | 76 | `bool ShouldElectInput(int, int)` | Same path |
| 23 | 92 | unknown | Same path |

vtable[15/19/23] semantics are partial — characterize live with the probe stack before locking.

### NetworkManager interface (`entity+216` per entity)

| Slot | Offset | Signature | Use |
|---|---|---|---|
| 0 | 0 | `void* Dtor(int flag)` | Engine may call vtable[0](this, 1) to free |
| 3 | 12 | `void Register(Entity*, int arg)` | Called by install fn after factory returns |
| 10 | 40 | `void Commit()` | Engine calls after publish |
| 11 | 44 | `void* GetWriteBufferByName(const char* name)` | Allocates/returns a buffer the engine fills |
| 12 | 48 | `void* GetReadBufferByName(const char* name)` | Returns a buffer the engine reads from (or null = "no data") |

The bulk-state buffer is 18 dwords (72 bytes) — matches `entity_publish_distributed_state`'s copy of `entity+88..168`. Other channel names may have different buffer sizes — characterize live.

### Activation chain

Engine call → `entity_install_network_manager(entity)` → check `g_mp_coordinator_ptr != 0 && coordinator->vtable[5]()` → call `entity->vtable[49]()` factory → store result at `entity+216` → call `manager->vtable[3](entity, arg)` to register.

### Publishing/receiving

Publish fires from `entity_reset_and_publish` (vtable slot 13 of the 10 participating classes — entity reset/respawn paths). Per-frame transform updates fire from `entity_publish_netactor_transforms` over `entity+492..504` (4 sub-actor slots).

## Open questions (blocked on live-test)

These are the answers Phase 1's design will lock based on, captured here so the user knows what the probe-stack test is buying us.

| Question | How to answer | Impact on design |
|---|---|---|
| Q1: Which entity classes ACTUALLY hit `entity_install_network_manager` in normal gameplay? | Live run with current probe stack; read install-fn observation in Debug tab | Tells us which classes need a NetworkManager; rest don't need replication |
| Q2: When MP-active=true, which coordinator slots get called? | Live run; check `coord_unknown` log warnings | Locks coordinator vtable design |
| Q3: When NetworkManager-install is armed, which NetworkManager slots get called? | Same | Locks manager vtable design |
| Q4: What channel names besides "DistributedState" does the engine ask for via vtable[11/12]? | Same — `netmgr_get_write_buffer` logs the name | Reveals other replication channels (input? animation state?) |
| Q5: What's the engine's response to vtable[12] returning null vs a buffer? | Same — observe if engine misbehaves or just no-ops | Determines whether we can lazy-init buffers |
| Q6: Does the engine call `manager->vtable[3]` immediately or save it for a later event? | Same | Determines lifecycle ordering for our impl |

All 6 are answered by **one live test session** with the current probe stack (~30s of armed time).

## Phase 1a — UDP transport design (independent of engine)

### Wire protocol

Authoritative-host UDP. One socket per process. Single connection (no NAT traversal — LAN only).

Packet header (8 bytes):
```
struct PacketHeader {
    uint16_t protocol_id;      // = 0x4D52 ("MR") for sanity
    uint8_t  protocol_version; // = 1
    uint8_t  packet_type;      // 0=handshake, 1=heartbeat, 2=channel_data, 3=ack, 4=disconnect
    uint16_t sequence;         // monotonic per-sender; wraps
    uint16_t ack;              // last seq we received (gaps trigger resend on reliable channels)
};
```

Channel data packets (type=2) carry one or more `<channel_id, length, payload>` triples. Channel id maps to the engine's string channel name (e.g. "DistributedState" → channel 0; others assigned as they appear in vtable[11] calls).

### Reliability model

Three classes of channels:
- **Unreliable** (transform updates from `entity_publish_netactor_transforms`): drop if dropped. Lowest latency.
- **Reliable-ordered** (most bulk-state publish): retransmit on gap. Higher latency but no loss.
- **Reliable-once** (handshake, disconnect, lobby messages): retransmit until acked.

The classification per channel name is something we need to learn empirically — start with all channels as reliable-ordered, downgrade specific ones as we observe semantics.

### Connection model

- Host: opens UDP socket on configurable port (default 31415), waits for client handshake.
- Client: sends handshake to host IP:port, waits for handshake ack.
- After ack: both sides set up `MultiplayerCoordinator`, write to 0x745BE8, set IsMPActive=true.
- Heartbeats every 100ms; disconnect on 3s silence.

### Threading

One dedicated network thread per process (single-threaded I/O, lock-free queues to/from game thread).

- Game thread → Network thread: outbound channel data queue. Filled by NetworkManager vtable[11]/[10] calls.
- Network thread → Game thread: inbound channel data queue. Drained by NetworkManager vtable[12] calls.

The game thread MUST NOT block on network I/O. The NetworkManager vtable methods always return immediately (write to ring buffer, signal network thread).

## Phase 1b — MultiplayerCoordinator real implementation

Replaces the stub. Implements:

- `vtable[5] IsMPActive()` — returns true while we have a live peer connection.
- `vtable[9] GetNetworkTime()` — returns synchronized network time. Algorithm: host uses local time; client uses local + offset learned during handshake.
- `vtable[15/19/23]` — locked after live characterization (Q2).

Class is a singleton owned by mtr-asi. Pointer written to 0x745BE8 on connection up; cleared on disconnect.

## Phase 1c — NetworkManager real implementation

Replaces the stub. Per-entity instances (not singleton this time).

Key change from stub: **per-channel-name buffer map**. Each `(entity*, channel_name)` pair gets its own buffer. The buffer size for each name is locked after Q4 characterization (likely "DistributedState" = 72 bytes is the only one for now, but we'll extend as more channels appear).

- `vtable[11] GetWriteBufferByName(name)`: lookup/create buffer for (this, name); return pointer; mark dirty.
- `vtable[10] Commit()`: walk dirty buffers; serialize each via the transport's outbound channel; clear dirty flags.
- `vtable[12] GetReadBufferByName(name)`: lookup buffer in inbound queue for (this, name); return pointer or null.

Memory model: pre-allocate small buffer pool per-manager. Names are interned (string-table). The engine's "name" arg is the interned string pointer (per replication-primitive RE), so identity comparison is fast.

## Phase 1d — Engine wiring

Two install vectors:

1. **Coordinator install**: same as the probe — write our coordinator pointer to 0x745BE8 directly. Reversible.

2. **NetworkManager per-entity install**: same as the probe's POST-write approach. Hook `entity_install_network_manager` POST, overwrite `entity+216` with a per-entity `new NetworkManager(entity)`. Avoids the vtable[49] patch (which would conflict with non-Protagonist entity classes whose slot 49 is a list-walker).

Cleanup: hook the destructor path to delete our NetworkManager when the entity is destroyed. Without this, we leak. The natural place is `vtable[0]` (entity dtor) — needs RE pass to confirm.

## Phase 1e — Bring-up + verification

Tested via `tools/run-coop-test.ps1 -Mode single-process` (already shipped Phase 0D):

- T1: Both processes start; handshake completes; both report `peer_connected=true`.
- T2: Coordinator installed on both; IsMPActive=true; engine consumers fire vtable[5] calls.
- T3: NetworkManager installed on Protagonist entities (and any others identified by Q1).
- T4: Host's `entity_publish_distributed_state` writes to a buffer; Commit serializes; UDP packet appears on wire.
- T5: Client's `entity_receive_distributed_state` returns the buffer; entity+88..168 gets updated.
- T6: Repeat T4-T5 across N entities; verify no buffer collisions, no use-after-free.

Pass criteria: full round-trip in <50ms per channel per frame; no crashes for 5 minutes of armed time.

## Implementation milestones

| Sub-phase | Effort | Output | Risk |
|---|---|---|---|
| 1a transport | 1.5 wk | UDP socket + packet framing + handshake + reliability layer | Low |
| 1b coordinator | 0.5 wk | Replace stub with real class | Low |
| 1c manager | 1.0 wk | Per-entity managers + channel buffer map | Medium (per-entity lifecycle) |
| 1d wiring | 0.5 wk | Install hooks + destructor cleanup | Medium (lifecycle) |
| 1e verify | 1.5 wk | T1-T6 round-trips + stability | High (unknowns) |
| **Total** | **5 wk** |  |  |

## What this design intentionally does NOT include

- **Input separation.** That's Phase 2 — the engine's input path is already player-index aware (per Phase 0C catalog), so it's a smaller pass than v2 estimated.
- **NPC + prop replication.** Phase 4. The transform-replication primitive (`entity_publish_netactor_transforms`) at the sub-actor layer handles much of this — characterize once Phase 1 is up.
- **Script VM replication.** Phase 5. Existing `transremote` / `recv` script primitives suggest the engine has built-in routing; needs runtime hook of script_register_command to characterize.
- **Save/pause/UI.** Phase 6.
- **Stability/polish.** Phase 7.

## Risk summary

| Risk | Likelihood | Mitigation |
|---|---|---|
| Engine consumer calls a coordinator slot we haven't characterized | High | Probe stack has logging fallback; live test surfaces immediately |
| NetworkManager lifecycle has hidden engine-side state | Medium | vtable[0] dtor RE pass before Phase 1d |
| String-keyed channel names diverge across engine paths | Medium | Live test of vtable[11] surfaces all names |
| Engine assumes reliable delivery on a channel we send unreliably | Medium | Start all channels reliable-ordered, downgrade after empirical observation |
| Two-process single-machine testing hits the Disney mutex | Known blocker | Phase 0D test harness already addresses (single-process mode = same engine state, just two coordinator/manager pairs) |

## What is required BEFORE starting Phase 1 implementation

1. **Live test of the current Phase 0 probe stack** — answers Q1-Q6.
2. Decision: per-entity NetworkManager (Phase 1c is +0.5wk for the buffer map) vs singleton-with-routing (~simpler, less faithful to the engine model). Recommend per-entity.
3. Decision: do we wait on Phase 0B's full save-format RE to inform Phase 1, or move ahead and address save-resume separately later? Recommend move ahead; save-resume is a Phase 6 concern.

## Engine VAs for Phase 1 wiring

| VA | Symbol | Phase 1 use |
|---|---|---|
| 0x5AFDB0 | `entity_publish_distributed_state` | Calls our vtable[11] + vtable[10] |
| 0x5AFE90 | `entity_receive_distributed_state` | Calls our vtable[12] |
| 0x5B06A0 | `entity_publish_netactor_transforms` | Calls publish hooks for transform side-channel |
| 0x5B0C70 | `entity_install_network_manager` | POST-hook to inject NetworkManager |
| 0x5B2080 | `entity_reset_and_publish` | Fires on respawn — trigger to send full state |
| 0x745BE8 | `g_mp_coordinator_ptr` | Direct write to install coordinator |
| 0x5B96F0 | `entity_factory_construct` | Phase 2 — player2 spawn |

## Recommendation

Phase 1 is concrete and ready. Implementation can begin AFTER one live test session with the current probe stack. That session is ~30 minutes of user time, answers all 6 open questions, and lets us lock the vtable interfaces before writing real code.

The probe stack is built. The design is laid out. Phase 1's gate is one test session.
