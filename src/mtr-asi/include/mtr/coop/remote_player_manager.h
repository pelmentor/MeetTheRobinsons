// MtrPlayerManager — owner / registry of MtrRemotePlayer instances.
//
// Phase 1 skeleton (2026-05-12). Models MTA's CClientPlayerManager:
//   reference/mtasa-blue/Client/mods/deathmatch/logic/CClientPlayerManager.{h,cpp}
//
// Surface:
//   install()           — idempotent; called once from dllmain after MinHook.
//   register_local()    — wrap the engine-constructed wilbur as player 0.
//                         Lazy: first `local()` call reads the well-known
//                         engine_wilbur VA and registers if non-NULL.
//   register_remote()   — wrap an orphan we asked the engine to build via
//                         coop_spawn_probe. Caller passes the entity ptr
//                         AFTER coop_registry_mirror has cloned the +0xCCC
//                         registry onto it. Owns_engine=true.
//   by_engine_entity()  — sidecar-map lookup. O(N), N<=2 in MVP.
//   by_player_id()      — lookup by 0/1/...
//   local()             — returns the local-player wrapper, registering
//                         lazily on first call.
//   count()             — number of registered players.
//   unregister()        — explicit teardown of one player. Drives engine
//                         teardown iff owns_engine.
//
// What the manager DOES NOT do (Phase 1 scope):
//   - per-tick DoPulse  — no networking yet, nothing to push.
//   - serialise / wire  — networking lands later, will compose on top.
//   - lifecycle events  — no level-transition hook; manager's lifetime is
//                         the process lifetime (singleton storage).
//
// Architectural principle #3 (parallel class hierarchy). #5 (minimum
// viable subset). #7 (wrapper layer ≠ network layer — manager lives in
// coop/ which will eventually contain network code, but the manager
// itself stays neutral: it owns wrappers, not packets).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mtr::coop::net { struct PoseSnapshotBody; }

namespace mtr::coop {

class MtrRemotePlayer;

class MtrPlayerManager {
public:
    static MtrPlayerManager& instance();

    bool install();

    MtrRemotePlayer* register_local(void* engine_wilbur);
    MtrRemotePlayer* register_remote(void* engine_entity);

    MtrRemotePlayer* by_engine_entity(void* engine_entity);
    MtrRemotePlayer* by_player_id(uint8_t id);
    MtrRemotePlayer* local();

    std::size_t count() const;

    // True iff a Remote-player wrapper is currently registered (i.e. the
    // engine has built the P2 orphan AND we've taken ownership of it).
    // Used by the coop-lan-soak test scenario to gate the soak phase on
    // remote-orphan presence. Thread-safe (acquires m_mu briefly).
    bool has_remote() const;

    void unregister(MtrRemotePlayer* p);

    // Per-sim-tick heartbeat. Called from the sim_decouple aggregator
    // POST hook (once per real sim step, matches MTA's DoPulse cadence).
    // Iterates registered players, calls each player's `do_pulse()`,
    // bumps the manager's aggregate counter, and periodically emits a
    // diagnostic log line. Safe under SEH — pulse work happens inside
    // each player; manager iteration is plain in-process bookkeeping.
    void do_pulse();

    uint64_t pulse_count() const;

    // Phase 1.4b — network recv bridge. Called from the NetSession recv
    // thread after parse_pose_snapshot succeeds. Acquires m_mu, applies
    // the out-of-order time_ctx gate, and (on a fresh packet) calls
    // MtrRemotePlayer::push_interp_snapshot_at with the receive-side
    // QPC. The QPC is captured BEFORE lock acquisition to keep alpha-
    // blend timing reflective of wall-clock arrival, not the moment
    // the lock was finally acquired under sim-thread contention.
    //
    // Threading: recv thread is the only caller. Internally serialises
    // against the sim thread's do_pulse via m_mu. Logging happens
    // outside the lock (snapshot-then-log pattern, same as do_pulse).
    void on_remote_packet(const mtr::coop::net::PoseSnapshotBody& body);

private:
    MtrPlayerManager()  = default;
    ~MtrPlayerManager();

    MtrPlayerManager(const MtrPlayerManager&)            = delete;
    MtrPlayerManager& operator=(const MtrPlayerManager&) = delete;

    // Internal — caller must hold m_mu.
    MtrRemotePlayer* by_engine_entity_locked(void* engine_entity);
    MtrRemotePlayer* by_player_id_locked(uint8_t id);
    bool             has_remote_locked() const;

    std::vector<std::unique_ptr<MtrRemotePlayer>> m_players;
    // Sidecar back-pointer map. MTA precedent: `m_pStoredPointer` slot on
    // each CEntitySA (set via SetStoredPointer at CClientPed.cpp:3641).
    // We picked sidecar over modifying engine struct layout (Principle 1).
    // Populated/depopulated by register_*/unregister.
    std::unordered_map<void*, MtrRemotePlayer*>   m_by_engine;
    mutable std::mutex                            m_mu;
    bool                                          m_installed         = false;
    bool                                          m_local_lazy_tried  = false;
    uint8_t                                       m_next_player_id    = 0;
    uint64_t                                      m_pulse_count       = 0;
    // Outbound wire sequence counter (modular 0..65535 per
    // kProtocolVersion=2). Bumped in do_pulse for each pose snapshot
    // we emit. Sim-thread-only; no synchronisation needed.
    uint16_t                                      m_send_time_ctx     = 0;
    // First-recv announcement and one-shot unknown-player-id warning,
    // both bumped from on_remote_packet under m_mu (snapshot-then-log
    // pattern: log happens outside the lock). Reset in unregister so a
    // peer reconnect after a disconnect logs again — staying latched
    // for the whole process lifetime would silently swallow legitimate
    // diagnostics on a second session (audit fix 2026-05-12 Phase 1.4d).
    bool                                          m_first_recv_logged   = false;
    bool                                          m_unknown_id_logged   = false;
    // Phase 1.4d — auto-orphan-spawn signal. Set true (latched) by
    // on_remote_packet on the recv thread the first time a packet
    // arrives for an unknown wire id. Read by do_pulse on the sim
    // thread; when set AND no Remote wrapper exists yet AND the engine
    // is in gameplay, do_pulse triggers coop_spawn_probe::try_spawn_p2
    // to materialise the orphan. Recv thread does NOT call try_spawn_p2
    // itself — that would violate Principle 7 by touching engine state
    // from the transport thread. The flag is a one-way latch that maps
    // "peer is talking to us" into "spawn an orphan when next safe to
    // do so", and clears on unregister so a reconnect re-arms it.
    //
    // MTA precedent: CPacketHandler::Packet_PlayerList creates the
    // CClientPlayer wrapper synchronously on the game thread, gated on
    // STATUS_JOINED (the equivalent of our is_in_gameplay() check).
    // reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:929
    std::atomic<bool>                             m_peer_seen{false};
};

} // namespace mtr::coop
