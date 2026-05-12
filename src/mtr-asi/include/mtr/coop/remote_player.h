// MtrRemotePlayer — mtr-asi's parallel class to the engine wilbur entity.
//
// Phase 1 skeleton (2026-05-12). Models the MTA shape:
//
//     MTA            : CClientPed       ─── m_pPlayerPed* ──►  CPlayerPedSA
//     mtr-asi (ours) : MtrRemotePlayer  ─── m_engine_entity* ► engine wilbur
//
// Two classes pointing at each other. The wrapper (this class) holds
// future network / interp / input state. The engine entity (orphan wilbur,
// or — for the local player — the engine-constructed wilbur) holds
// rendering / animation / per-entity registry. Lifetimes are coordinated:
// the wrapper actively drives engine teardown on destruction iff it was
// the one that asked the engine to construct it (`m_owns_engine`).
//
// Reference for the pattern (cloned to reference/mtasa-blue/):
//   Client/mods/deathmatch/logic/CClientPed.{h,cpp}     (the wrapper)
//   Client/game_sa/CPlayerPedSA.{h,cpp}                 (engine side)
//
// Architectural principles applied:
//   #2 engine-extension paradigm  — wrapper owns its own state, talks to
//                                   engine via clean ptr boundary.
//   #3 parallel class hierarchy   — the canonical MTA pattern, ported.
//   #5 minimum viable subset      — Phase 1 holds id / role / engine ptr;
//                                   network / interp / input fields land
//                                   when the corresponding subsystem does.
//   #7 wrapper layer ≠ network    — this header lives under coop/ which
//                                   eventually owns gameplay+network code;
//                                   engine-wrapping primitives stay in
//                                   coop_spawn_probe / coop_registry_mirror.
//
// Design doc:
//   research/findings/coop-mtr-remote-player-design-2026-05-12.md

#pragma once

#include <cstdint>

#include "mtr/interp.h"

namespace mtr::coop {

class MtrPlayerManager;  // owner; private ctor/dtor friends with manager.

class MtrRemotePlayer {
    friend class MtrPlayerManager;

public:
    // The wrapper class can wear two hats. MTA's equivalent flag is
    // `CClientPlayer::m_bIsLocalPlayer`. We use an enum because it makes
    // call sites self-documenting.
    enum class Role : uint8_t {
        Local,   // the engine constructed this wilbur (= host's player or
                 // engine-spawned P2 in some future scheme); WE DO NOT
                 // drive engine teardown.
        Remote,  // we asked the engine to construct this (via
                 // entity_factory_construct); WE DO drive engine teardown
                 // on disconnect / unload.
    };

    void*    engine_entity() const noexcept { return m_engine_entity; }
    Role     role()          const noexcept { return m_role; }
    uint8_t  player_id()     const noexcept { return m_player_id; }
    bool     is_local()      const noexcept { return m_role == Role::Local; }
    bool     owns_engine()   const noexcept { return m_owns_engine; }
    bool     is_alive()      const noexcept { return m_engine_entity != nullptr; }
    uint64_t pulse_count()   const noexcept { return m_pulse_count; }

    // Per-sim-tick heartbeat. MTA precedent:
    //   CClientPed::DoPulse  → tasks + anim + position update + voice
    // Bumps m_pulse_count unconditionally so the manager's aggregate log
    // proves the tick reaches each wrapper. When interp is enabled AND
    // role==Remote AND both snapshots are valid, also blends prev→curr
    // and writes the result into the engine entity's pos+rot fields.
    void do_pulse();

    // Interp scaffold (Phase 1.1, 2026-05-12). MTA precedent:
    //   CClientPed::m_interp + SetTargetPosition + Interpolate
    //   (Client/mods/deathmatch/logic/CClientPed.{h:751-769,cpp:3401,5370})
    //
    // push_interp_snapshot — caller (eventually network recv, today a test
    // driver) hands us a fresh authoritative pos+rot. Shifts prev←curr,
    // fills curr with the new pose stamped at qpc_now().
    //
    // THREADING CONTRACT: caller MUST hold MtrPlayerManager::m_mu while
    // calling this. Two reasons: (a) the prev/curr shift is non-atomic,
    // so a concurrent do_pulse() reader on another thread could observe a
    // half-shifted state; (b) the manager iterates m_players under m_mu,
    // so caller-holds-lock keeps push and pulse on the same wrapper
    // serialised. The current call sites (coop_spawn_probe wiring after
    // register_remote) satisfy this — see remote_player_manager.cpp's
    // single-lock discipline.
    //
    // alpha for do_pulse is computed Quake-style: snapshot arrived at
    // curr.qpc; we render the world delayed by (curr.qpc - prev.qpc) so
    // alpha grows 0→1 across the post-curr window. Past that window we
    // clamp to 1.0 (= snap to curr) until the next push.
    //
    // Local players ignore interp writes — they own their own pose via
    // the engine's input pipeline. The flag is still tracked so the same
    // wrapper class can wear both hats without behaviour leakage.
    void push_interp_snapshot(const float pos[3], const float rot[9]);

    // Phase 1.4b/c — explicit-QPC variant. Used by the network recv
    // path: the recv thread captures qpc_now() BEFORE acquiring the
    // manager m_mu, then hands the captured timestamp here so the
    // snapshot's qpc reflects the receive-side wall clock (not the
    // moment after lock acquisition, which could be milliseconds later
    // under sim-thread contention). Sender's QPC is meaningless across
    // machines and is intentionally not on the wire.
    void push_interp_snapshot_at(const float pos[3], const float rot[9],
                                 uint64_t qpc);

    // Wire-protocol sequence (modular 0..65535). Widened from uint8_t
    // per Phase 1.4b audit (85% conf): 8-bit wrap at 4.27s gave a
    // 2.1s stale/fresh ambiguity window; 16-bit pushes that to ~9 min
    // which is past any realistic packet delay. Out-of-order UDP drops
    // happen in MtrPlayerManager::on_remote_packet before pushing the
    // snapshot. Defaults to 0xFFFF so the first received packet (any
    // value 0..32767) always passes via (uint16_t)(v - 0xFFFF) wrap.
    uint16_t last_time_ctx() const noexcept { return m_last_time_ctx; }
    void     update_time_ctx(uint16_t tc) noexcept { m_last_time_ctx = tc; }

    bool     interp_enabled() const noexcept { return m_interp_enabled; }
    void     set_interp_enabled(bool on) noexcept { m_interp_enabled = on; }
    uint64_t interp_writes()  const noexcept { return m_interp_writes; }
    uint64_t interp_pushes()  const noexcept { return m_interp_pushes; }
    uint64_t interp_faults()  const noexcept { return m_interp_faults; }

    // Public so std::unique_ptr can call it on manager teardown. Direct
    // `delete` from outside the manager is non-idiomatic — use
    // MtrPlayerManager::unregister() instead, which drives engine teardown
    // first (when owns_engine).
    ~MtrRemotePlayer();

private:
    MtrRemotePlayer(uint8_t player_id, Role role, void* engine_entity,
                    bool owns_engine);

    MtrRemotePlayer(const MtrRemotePlayer&)            = delete;
    MtrRemotePlayer& operator=(const MtrRemotePlayer&) = delete;
    MtrRemotePlayer(MtrRemotePlayer&&)                 = delete;
    MtrRemotePlayer& operator=(MtrRemotePlayer&&)      = delete;

    // SEH-wrapped vtable[0](this, 1) — engine's scalar deleting destructor.
    // Called only by MtrPlayerManager::unregister(), and only when
    // `m_owns_engine` is true. Marked as "Phase 1 not exercised in tests"
    // because the current keep-alive scenarios run to process exit; manager
    // shutdown is intentionally a no-op on engine state (see manager dtor).
    void destroy_engine_entity_seh();

    uint8_t     m_player_id;
    // `m_role` is established at construction and never changes. `const`
    // turns any accidental future mutation into a compile error; without
    // it, the `do_pulse` guard (`m_role != Role::Remote → return`) is the
    // only protection against Local-player interp writes. Per audit
    // 2026-05-12-b.
    const Role  m_role;
    void*       m_engine_entity;
    bool        m_owns_engine;
    uint64_t    m_pulse_count = 0;

    // Interp scaffold state. Both snapshots default to {valid=false},
    // m_interp_enabled defaults to false → do_pulse is a no-op until an
    // outside caller (test driver, future network recv) flips the flag
    // and pushes at least one snapshot.
    mtr::interp::EntityPose m_prev_snap{};
    mtr::interp::EntityPose m_curr_snap{};
    bool                    m_interp_enabled = false;
    uint64_t                m_interp_writes  = 0;
    uint64_t                m_interp_pushes  = 0;
    // Bumped from inside the SEH `__except` in do_pulse when
    // apply_entity_pose_interp AVs. Lock-order safe: no logging from the
    // handler — the manager's heartbeat surfaces non-zero faults outside
    // its own lock.
    uint64_t                m_interp_faults  = 0;
    // Wire-protocol sequence counter (16-bit per kProtocolVersion=2).
    // Initialised to 0xFFFF so the first received packet (typically
    // value 0) is treated as fresh via the modular comparison in
    // on_remote_packet: (uint16_t)(0 - 0xFFFF) = 1 < 32768 → accepted.
    uint16_t                m_last_time_ctx  = 0xFFFF;
};

} // namespace mtr::coop
