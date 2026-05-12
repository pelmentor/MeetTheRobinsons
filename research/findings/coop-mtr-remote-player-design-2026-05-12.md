# `MtrRemotePlayer` design — parallel-class hierarchy (Principle 3)

**Date:** 2026-05-12 (sketch).
**Status:** PARTIALLY IMPLEMENTED. Skeleton (id / role / engine ptr /
pulse hook / sidecar map) shipped earlier 2026-05-12. Interp scaffold
(`m_prev_snap`, `m_curr_snap`, `push_interp_snapshot`, blend-and-write
in do_pulse) shipped same day — see
[coop-mtr-remote-player-interp-scaffold-2026-05-12.md](coop-mtr-remote-player-interp-scaffold-2026-05-12.md)
for what shipped, the 3-agent ordering consensus, the 2-agent audit
fixes, and the revised post-scaffold roadmap.

The original "design only — lands when networking begins" framing was
correct for the input/network fields; interp's data path is
independent of network packet shape, so it lands first.

> Architectural principle 3: *"Parallel class hierarchy mirroring engine
> structures. For coop: `MtrRemotePlayer` (our class, owns network /
> interp / input) + orphan entity (engine class, owns rendering /
> animation) connected by pointer. Same shape as MTA's `CClientPlayer ↔
> CPlayerPedSA`."*

---

## MTA precedent

Reference: `reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPed.{h,cpp}`
and `Client/game_sa/CPlayerPedSA.{h,cpp}`.

MTA's split:

```
   ┌─────────────────────────────┐         ┌───────────────────────┐
   │ MTA-side                    │         │ Engine-side (GTA SA)  │
   │   CClientPed                │         │   CPlayerPedSA        │
   │     m_pPlayerPed*  ─────────┼────────►│     real engine ped   │
   │     m_RemoteDataStorage     │         │     ▲                 │
   │     m_TaskManager           │         │     │  per-tick:      │
   │     m_pNetGame              │         │     │  ProcessControl │
   │     interp state            │         │     │  driven from    │
   │     network ID              │         │     │  context-swap'd │
   │     ...                     │         │     │  singletons     │
   └─────────────────────────────┘         └───────────────────────┘
```

CClientPed (MTA's class) holds network/interp/input state. CPlayerPedSA
(engine class, constructed via real engine ctor) holds rendering and
animation. They're TWO classes pointing at each other, not one class
trying to be both.

Network packet → write to `CClientPed`'s state. Per-frame tick of MTA →
update target position on CPlayerPedSA. Engine per-frame tick → drives
animation from the engine ped's data.

---

## MTR equivalent — `MtrRemotePlayer`

For Meet the Robinsons, the analogous shape:

```
   ┌─────────────────────────────┐         ┌───────────────────────┐
   │ mtr-asi side                │         │ Engine side (Wilbur)  │
   │   MtrRemotePlayer           │         │   Orphan wilbur       │
   │     m_engine_entity*  ──────┼────────►│     (real entity      │
   │     m_net_input             │         │      built via        │
   │     m_target_pos            │         │      entity_factory_  │
   │     m_target_rot            │         │      construct)       │
   │     m_target_velocity       │         │                       │
   │     m_health                │         │     +0xCCC registry   │
   │     m_anim_state            │         │       (21 keys,       │
   │     m_interp_history        │         │        mirrored from  │
   │     m_player_id (0 or 1)    │         │        engine_wilbur) │
   │     m_input_buffer          │         │     +0xD04 subscribers│
   │     m_last_packet_seq       │         │       (40 wrappers,   │
   │     ...                     │         │        all tick on    │
   └─────────────────────────────┘         │        the orphan)    │
                                            │     vtable[0] anim    │
                                            │     vtable[N] render  │
                                            └───────────────────────┘
```

**Ownership:**

- `MtrRemotePlayer` is OURS. We construct it, manage its lifetime,
  destruct it.
- The engine entity (orphan) is created via `entity_factory_construct`
  and torn down via the engine's vtable[0] (scalar deleting destructor).
- **`MtrRemotePlayer`'s destructor actively drives the engine teardown**
  by calling the engine entity's destructor — same shape as MTA's
  `~CClientPed → _DestroyModel → pool->RemovePed → ~CPlayerPedSA →
  m_pInterface->Destructor(true)` (CClientPed.cpp:3858, CPlayerPedSA.cpp:134-155).
  Not "we observe the engine tearing it down"; the wrapper drives the
  teardown via the engine's own primitives.

**Pointer direction:**

- `MtrRemotePlayer::m_engine_entity` is a non-owning pointer in the C++
  sense, but the wrapper actively coordinates the engine entity's
  lifecycle (construction + destruction). Think of it as `unique_ptr`
  semantics implemented through engine primitives rather than `delete`.
- **Back-reference (engine entity → `MtrRemotePlayer`):** MTA uses a
  free pointer slot on each `CEntitySA` (`m_pStoredPointer`, set via
  `SetStoredPointer(this)` at CClientPed.cpp:3641). For MTR, we should
  check if Wilbur's entity has an equivalent free slot. If yes: use it
  (zero extra data structures, follows MTA precedent). If no: fall back
  to a sidecar map (`std::unordered_map<engine_entity*, MtrRemotePlayer*>`)
  in the manager.

---

## Module layout (future)

```
src/mtr-asi/
├── include/mtr/coop/
│   ├── remote_player.h          # MtrRemotePlayer class definition
│   ├── remote_player_manager.h  # owner / lifecycle of MtrRemotePlayer instances
│   ├── net_packet.h             # PlayerStatePacket etc.
│   └── input_buffer.h           # per-player input ring buffer
└── src/coop/
    ├── remote_player.cpp        # state mgmt, interp, lifecycle
    ├── remote_player_manager.cpp  # construction, destruction, lookup
    ├── net_packet.cpp           # packet serialise / parse
    └── input_buffer.cpp         # input replay onto orphan
```

Existing modules absorb into this hierarchy:

- `coop_spawn_probe`: the orphan construction primitive. Becomes a
  private utility called from `remote_player_manager::create_local_orphan()`.
- `coop_registry_mirror`: the +0xCCC mirror primitive. Becomes a step in
  `remote_player_manager::create_local_orphan()`.
- `coop_orphan_filter`: deleted per the retirement plan.

---

## Lifecycle (anticipated)

```
Server start (host) / Connect to host (P2)
   │
   ▼
remote_player_manager::install()
   │
   ▼
On orphan construction event (= b7.13 trigger):
   ├── entity_factory_construct  (engine spawns orphan)
   ├── mirror_engine_registry    (per-entity state mirrored from engine_wilbur)
   ├── new MtrRemotePlayer(engine_entity)  (our class wraps it)
   └── manager.register(remote_player)
   │
   ▼
Per frame on host:
   ├── For each MtrRemotePlayer:
   │     ├── apply network input from peer → m_input_buffer
   │     ├── interp m_target_pos → engine entity pos write
   │     └── engine tick (via engine's natural sub_5AD9B0 walker)
   │           (orphan now ticks safely because all crashes are
   │            fixed at targeted call sites per principle 4)
   │
   ▼
On disconnect / unload:
   ├── manager.unregister(remote_player)
   ├── orphan entity → vtable[0] scalar deleting destructor
   └── delete MtrRemotePlayer
```

---

## What this design rejects (anti-patterns)

- **Extending the engine's in-memory layout or patching the engine vtable
  for our state.** We don't grow `wilbur`'s struct, don't add fields to
  the engine entity, don't swap its vtable for our own. The orphan IS a
  plain engine wilbur from the engine's perspective; per-player state
  lives entirely on `MtrRemotePlayer`. (Note: MTA does subclass its
  *wrapper* hierarchy — `CPlayerPedSA : CPedSA : CPhysicalSA : CEntitySA`,
  `CClientPlayer : CClientPed`. Those are plain C++ wrapper-class subclasses
  and are fine; we may do the same. The anti-pattern is modifying the
  engine's in-process struct/vtable.)
- **A god class** that's both "the engine entity" and "the network
  state". The `CClientPed ↔ CPlayerPed*` split exists in MTA precisely
  because the two have different lifetimes, different ownership, and
  different access patterns; collapsing them would re-introduce all the
  problems that split solved.
- **Spreading per-player state across globals.** Per-player data lives
  on the `MtrRemotePlayer` instance, not in module-level statics keyed
  by player ID. (MTA's `CRemoteDataStorage` is keyed in a static map
  `std::map<CPlayerPed*, CRemoteDataStorageSA*>` — that's pragmatic but
  marginally weaker than instance-owned state. We can match MTA's
  pragmatism or be stricter; prefer instance-owned where there's a clean
  way to do it.)

---

## Open questions for Phase 1

1. **Where does input come from for the LOCAL player?** Probably from
   the existing DirectInput hook (`dinput_hook`), wrapped in a per-player
   input buffer. The local `MtrRemotePlayer` reads from there; the
   remote one reads from `net_packet`.

2. **How does state sync timing work?** MTA does ~13 Hz state sync +
   linear interp. For LAN we could go higher (~30 Hz) without bandwidth
   concerns; lower latency.

3. **Who owns the engine_wilbur (P1)?** P1's wilbur is engine-owned; we
   don't construct it (the engine does at boot). On host, P1 still gets
   a `MtrRemotePlayer` (it's just LOCAL flavoured) so the manager
   uniformly handles both players. Field `m_is_local`. Same as MTA's
   `m_bIsLocalPlayer`.

4. **What's the back-reference engine→MtrRemotePlayer for?** Probably
   needed when an engine-side callback (e.g. damage event, animation
   notify) wants to inform the network layer. Implement as a flat map
   keyed by engine entity pointer; or, if vtable patching of the engine
   wilbur class is acceptable, add a slot ourselves (more invasive).

---

## Next steps

- **Phase 2 finish line**: orphan ticks safely without filter (gated on
  `sub_4CD7B0 → sub_58D330(NULL)` targeted fix). Independent of the
  skeleton — the skeleton works today against filter+mirror+keep-orphan.
- **Phase 1 grow**: networking, input buffer, interp on top of the
  skeleton below.

---

## SHIPPED — Phase 1 skeleton (2026-05-12 PM)

Files in the tree (~270 LOC across .h + .cpp):

- `src/mtr-asi/include/mtr/coop/remote_player.h`
- `src/mtr-asi/include/mtr/coop/remote_player_manager.h`
- `src/mtr-asi/src/coop/remote_player.cpp`
- `src/mtr-asi/src/coop/remote_player_manager.cpp`

Wired:

- `dllmain.cpp` calls `mtr::coop::MtrPlayerManager::instance().install()`
  right after `coop_orphan_filter::install()`.
- `coop_spawn_probe::try_spawn_p2()` calls `mgr.register_remote(entity)`
  right after the b7.13 mirror succeeds (keep-alive path). Then opportunistically
  `mgr.local()` to lazy-register engine_wilbur as the local player.

Behaviour validated on the canonical PASS:

```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker \
                -mtrasi-coop-filter-smart -mtrasi-coop-mirror-registry'
→ result=pass elapsed_ms=9390 frames=1932 (was 1931 pre-skeleton; +1 within noise)
→ [mtr_player_manager] install: ready (Phase 1 skeleton, no networking yet)
→ [mtr_remote_player] ctor: id=0 role=Remote entity=10D45960 owns=true
→ [mtr_player_manager] register_remote: id=0 entity=10D45960 (count=1)
→ [mtr_remote_player] ctor: id=1 role=Local  entity=10DAEF20 owns=false
→ [mtr_player_manager] register_local:  id=1 entity=10DAEF20 (count=2)
```

Two players registered: the orphan we constructed (Remote, owns_engine=true) and
engine_wilbur (Local, owns_engine=false). MTA parallel-class shape is now real
code, not a doc.

### Decisions taken during implementation

- **Sub-namespace `mtr::coop`**: this is the first module under the future
  coop subtree (per Principle 7). Existing flat `mtr::coop_X` modules
  stay where they are until they get absorbed.
- **Dtor public** on `MtrRemotePlayer`: `std::unique_ptr<MtrRemotePlayer>`
  needs to destruct via `std::default_delete`. Ctor stays private; manager
  is the only constructor.
- **Sidecar map deferred** (not yet needed): O(N) `by_engine_entity()`
  is fine for N=2. When a back-pointer mechanism is needed (engine →
  wrapper), add a sidecar `std::unordered_map<void*, MtrRemotePlayer*>`
  inside the manager — engine-side callbacks then look up via the map.
  Picked sidecar over modifying engine struct (Principle 1).
- **`~MtrRemotePlayer` does NOT auto-drive engine teardown**: the
  manager's `unregister()` is the explicit teardown driver. Singleton
  dtor at process exit must not touch engine memory. MTA's
  `CClientPlayerManager::DeleteAll()` is the equivalent explicit driver.
- **No flag gating**: per RULE №2, no `-mtrasi-coop-manager-enable`
  opt-in. The manager is always installed; its registrations are gated
  on the existing `-mtrasi-coop-keep-orphan` flow (no orphan → no
  Remote registration; lazy local registration only fires when
  spawn_probe runs).

### What this skeleton intentionally lacks (next milestones)

- Per-player input buffer (`m_input_buffer`) — depends on
  controlmapper_vtable_probe being re-purposed for per-player input
  routing per MTA's `CController` / pad-context-swap pattern.
- Net packet types (`PlayerStatePacket`) — depends on transport choice.
- Target-pos / target-vel interp state — depends on packet shape.
- Back-pointer (engine → wrapper) — sidecar map will land when first
  engine-side callback needs to find its `MtrRemotePlayer`.

These plug into the existing class incrementally as each subsystem is
designed; the class shape doesn't need to change.

---

## SHIPPED — Phase 1 step 2: pulse + sidecar map (2026-05-12 evening)

Two follow-on additions on top of the bare skeleton:

### `MtrRemotePlayer::do_pulse()` + `MtrPlayerManager::do_pulse()`

Per-sim-tick heartbeat in MTA's `CClientPed::DoPulse` shape. Wired into
`hk_sim_aggregator` (sim_decouple_throttle.cpp:272) right after
`freecam::on_post_sim_aggregator()`. Fires once per real sim step — the
MTA cadence. Skeleton bodies are pure bookkeeping:

```cpp
void MtrRemotePlayer::do_pulse() { ++m_pulse_count; }
```

Manager iterates the registered players, calls each `do_pulse()`, bumps
its aggregate counter, and every 60 pulses (≈ 1s at default sim_hz=60)
emits a heartbeat log line with per-player pulse counts. Live test
confirms:

```
[mtr_player_manager] pulse: total=900 players=2 p0_pulses=36 p1_pulses=36
```

(p0/p1 each got 36 pulses in the 555 ms between registration and the
first heartbeat log — matches the expected 60 Hz cadence.)

### Sidecar back-pointer map (engine → wrapper)

`MtrPlayerManager` now owns an `std::unordered_map<void*, MtrRemotePlayer*>`
populated/depopulated by `register_local/remote` and `unregister`. MTA's
equivalent is the `m_pStoredPointer` slot on each `CEntitySA`
(`SetStoredPointer(this)` at CClientPed.cpp:3641); we picked the sidecar
form over modifying the engine struct (Principle 1). O(1) lookup is
the immediate user-facing benefit; the bigger one is that any future
engine-side callback (anim notify, damage event, render hook) can find
its `MtrRemotePlayer` in one map probe.

Log line at register time now reports both counts:

```
[mtr_player_manager] register_remote: id=0 entity=10D48000 (count=1, map=1)
[mtr_player_manager] register_local:  id=1 entity=10DB7F20 (count=2, map=2)
```

### What still does not exist (and is the next natural step)

- Position / rot / vel target fields on `MtrRemotePlayer` (interp
  scaffold). MTA: `m_vecTargetPosition`, `m_vecTargetMoveSpeed`,
  `m_vecTargetTurnSpeed`, `m_vecTargetIncrements`.
- Per-player `CControllerState` analog + input buffer. MTA:
  `m_Controller`, `m_Pad.DoPulse(this)` (CClientPed.cpp:2899).
- Network packet types + transport. MTA: `g_pNet`, the lightsync /
  puresync packet pipeline. Independent of the skeleton.

These are independent — each lands in its own iteration once the
subsystem decision is made.

---

## Audit sweep (2026-05-12 evening, post pulse+sidecar)

Per the standing "after substantial work, spawn 2 audit agents" pattern.
MTA-fidelity audit + code-review audit ran in parallel.

### Fixes applied this turn

- **[Critical, code-review]** Lock-order hazard: `do_pulse()` plus every
  other manager method that mutated state was holding `m_mu` while
  calling `mtr::log::info` (which acquires its own `s_mu`). Today benign
  (log doesn't re-enter manager), but the lock-order inversion was
  wired into every hot-path method. Refactored `install`,
  `register_local`, `register_remote`, `unregister`, `do_pulse` to
  capture values under the lock, then release before logging.
  Wrapper ctor/dtor stripped of their log lines — manager owns logging
  (it's the actor with lock-aware concerns; the data class stays
  log-free).

- **[Important, code-review]** Sidecar map / vector insertion order was
  `m_by_engine[k] = raw` BEFORE `m_players.push_back(std::move(up))`.
  On `bad_alloc` from the push, `m_by_engine` would hold a dangling raw
  pointer (the unique_ptr destructs on stack unwind). Swapped to
  vector-first: if push throws, `up` still owns the wrapper and cleans
  up; map insert happens AFTER the vector commit. Strong-exception
  guarantee restored.

- **[Low-Medium, MTA-fidelity]** `destroy_engine_entity_seh` comment
  enlarged to surface the first-execution risk: the orphan holds
  b7.13-mirrored +0xCCC registry + active +0xD04 subscriber node + live
  transform-list node. A bare `vt[0](this,1)` will attempt engine-side
  teardowns on state the engine may not expect from a scalar-deleted
  path. SEH wrapper localizes crashes — does NOT make it safe. First
  test exercising this path should be in isolated scenario.

### Intentional deferrals (RULE №2: don't pre-build)

The MTA-fidelity audit flagged three structural gaps as HIGH/MEDIUM:

- **`Unlink()` on `MtrRemotePlayer`** (MTA: `CClientPlayer::Unlink`
  calls `RemoveFromList`). Today, no path destroys a wrapper outside
  the manager's `unregister()` or `~MtrPlayerManager` — the private
  ctor blocks external construction, and `unique_ptr<MtrRemotePlayer>`
  ownership flows only through the manager. Adding `Unlink()` is
  preemptive design for a hypothetical "engine-side dead-entity event"
  that doesn't exist yet. Defer until a concrete second deletion path
  is needed (per RULE №2: do not build infrastructure for hypothetical
  scenarios).

- **`m_pManager` back-pointer on `MtrRemotePlayer`** (MTA: `CClientPed::m_pManager`).
  Useful for self-unlink. Defer with `Unlink()`.

- **`DeleteAll()` + `m_bCanRemoveFromList` re-entry guard on
  `MtrPlayerManager`** (MTA: prevents vector mutation during iteration
  when each wrapper's dtor calls `RemoveFromList`). Defer with
  `Unlink()` — only needed once a self-unlink path exists.

- **Level-transition / disconnect lifecycle path.** MTA flushes via
  `DeleteAll()` on disconnect; we have no equivalent because we have
  no transport yet. When the engine ever invalidates the entity pool
  (level reset, save-load), the manager will hold dangling `void*`.
  Phase 2 work, gated on the per-level lifecycle hook.

- **`Exists(MtrRemotePlayer*)` / `contains(void*)` stale-pointer
  guards.** MTA exposes both. Trivial to add when a second caller
  materialises. Defer.

- **Wall-clock-based heartbeat period.** Current `m_pulse_count % 60`
  assumes sim_hz=60. At sim_hz=120 the log fires every 0.5 s, at
  sim_hz=30 every 2 s. OK for skeleton; revisit if it ever matters.

### Items confirmed NOT bugs (false positives from audit, kept for posterity)

- Lazy `local()` race — second caller correctly finds the entry
  inserted by the first caller's `register_local` duplicate check.
- `m_local_lazy_tried` double-write — monotonic flag, no reset path;
  second caller correctly sees `first_miss=false`.
- Singleton init order — `do_pulse()` reads no data populated by
  `install()`; local-static construction on first call is safe.
- SEH coverage on `do_pulse` — Phase 1 body is pure bookkeeping
  (`++m_pulse_count`); SEH is a future concern when input replay or
  pos write lands.
