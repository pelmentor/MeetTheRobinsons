// MtrPlayerManager — Phase 1 skeleton implementation.
// See mtr/coop/remote_player_manager.h for design.

#include "mtr/coop/remote_player_manager.h"
#include "mtr/coop/remote_player.h"
#include "mtr/coop/engine_player.h"
#include "mtr/coop/net/net_session.h"
#include "mtr/coop/net/remote_pose_packet.h"
#include "mtr/coop_spawn_probe.h"
#include "mtr/interp.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop {

MtrPlayerManager& MtrPlayerManager::instance() {
    static MtrPlayerManager s;
    return s;
}

MtrPlayerManager::~MtrPlayerManager() {
    // Singleton dtor runs at process exit (DLL detach). Engine may already
    // be down. Drop wrappers WITHOUT driving engine teardown. See
    // remote_player.h `destroy_engine_entity_seh` contract.
    //
    // No log emitted here — at DLL detach the log subsystem may itself
    // already be torn down, and we don't want to acquire any lock during
    // shutdown that could collide with whatever the host is doing.
    std::scoped_lock lk(m_mu);
    m_by_engine.clear();
    m_players.clear();
}

bool MtrPlayerManager::install() {
    bool first_time = false;
    {
        std::scoped_lock lk(m_mu);
        if (!m_installed) {
            m_installed = true;
            first_time = true;
        }
    }
    if (first_time) {
        mtr::log::info("[mtr_player_manager] install: ready (Phase 1 skeleton,"
                       " no networking yet)");
    }
    return true;
}

MtrRemotePlayer* MtrPlayerManager::by_engine_entity_locked(void* engine_entity) {
    if (!engine_entity) return nullptr;
    auto it = m_by_engine.find(engine_entity);
    return (it == m_by_engine.end()) ? nullptr : it->second;
}

MtrRemotePlayer* MtrPlayerManager::register_local(void* engine_wilbur) {
    if (!engine_wilbur) {
        mtr::log::info("[mtr_player_manager] register_local: refused"
                       " (engine_wilbur=NULL)");
        return nullptr;
    }

    MtrRemotePlayer* raw = nullptr;
    uint8_t id = 0;
    std::size_t count_snap = 0, map_snap = 0;
    bool created = false;

    {
        std::scoped_lock lk(m_mu);
        if (auto* existing = by_engine_entity_locked(engine_wilbur)) {
            return existing;
        }
        // Phase 1.4b wire-id alignment: when a network session is active
        // the local player's wire id is determined by session role (host=0,
        // client=1), NOT by registration order. Both endpoints must agree
        // on which packet.player_id belongs to whom; sequence-counter
        // assignment would put client's local at id=0, but client's
        // *remote* (the host's pose) belongs at id=0. Override to keep
        // the wire id matching the session role.
        if (mtr::coop::net::NetSession::instance().active()) {
            id = mtr::coop::net::NetSession::instance().local_wire_id();
        } else {
            id = m_next_player_id++;
        }
        auto up = std::unique_ptr<MtrRemotePlayer>(
            new MtrRemotePlayer(id, MtrRemotePlayer::Role::Local,
                                engine_wilbur, /*owns_engine=*/false));
        raw = up.get();
        // Order matters for strong-exception-guarantee: push into the
        // vector FIRST. If push_back throws (e.g. bad_alloc), `up` still
        // owns the wrapper and destructs cleanly on unwind. Then insert
        // into the sidecar map — if THAT throws, we have an inconsistent
        // map vs vector but no dangling raw pointer in the map (which
        // would be a use-after-free). Vector-first inversion of the
        // original code per audit 2026-05-12.
        m_players.push_back(std::move(up));
        m_by_engine[engine_wilbur] = raw;
        count_snap = m_players.size();
        map_snap = m_by_engine.size();
        created = true;
    }

    if (created) {
        mtr::log::info("[mtr_player_manager] register_local: id=%u entity=%p"
                       " (count=%zu, map=%zu)",
                       static_cast<unsigned>(id), engine_wilbur, count_snap,
                       map_snap);
    }
    return raw;
}

MtrRemotePlayer* MtrPlayerManager::register_remote(void* engine_entity) {
    if (!engine_entity) {
        mtr::log::info("[mtr_player_manager] register_remote: refused"
                       " (engine_entity=NULL)");
        return nullptr;
    }

    MtrRemotePlayer* raw = nullptr;
    uint8_t id = 0;
    std::size_t count_snap = 0, map_snap = 0;
    bool created = false;

    {
        std::scoped_lock lk(m_mu);
        if (auto* existing = by_engine_entity_locked(engine_entity)) {
            return existing;
        }
        // Phase 1.4b wire-id alignment (see register_local). When the
        // session is active, the remote wrapper takes the peer's wire
        // id (host=1, client=0) so on_remote_packet's by_player_id
        // lookup matches the incoming body.player_id.
        if (mtr::coop::net::NetSession::instance().active()) {
            id = mtr::coop::net::NetSession::instance().remote_wire_id();
        } else {
            id = m_next_player_id++;
        }
        auto up = std::unique_ptr<MtrRemotePlayer>(
            new MtrRemotePlayer(id, MtrRemotePlayer::Role::Remote,
                                engine_entity, /*owns_engine=*/true));
        raw = up.get();
        // Vector-first insertion order per audit 2026-05-12.
        m_players.push_back(std::move(up));
        m_by_engine[engine_entity] = raw;
        count_snap = m_players.size();
        map_snap = m_by_engine.size();
        created = true;
    }

    if (created) {
        mtr::log::info("[mtr_player_manager] register_remote: id=%u entity=%p"
                       " (count=%zu, map=%zu)",
                       static_cast<unsigned>(id), engine_entity, count_snap,
                       map_snap);
    }
    return raw;
}

MtrRemotePlayer* MtrPlayerManager::by_engine_entity(void* engine_entity) {
    std::scoped_lock lk(m_mu);
    return by_engine_entity_locked(engine_entity);
}

MtrRemotePlayer* MtrPlayerManager::by_player_id(uint8_t id) {
    std::scoped_lock lk(m_mu);
    return by_player_id_locked(id);
}

MtrRemotePlayer* MtrPlayerManager::by_player_id_locked(uint8_t id) {
    for (auto& up : m_players) {
        if (up && up->player_id() == id) return up.get();
    }
    return nullptr;
}

bool MtrPlayerManager::has_remote_locked() const {
    for (const auto& up : m_players) {
        if (up && up->role() == MtrRemotePlayer::Role::Remote) return true;
    }
    return false;
}

MtrRemotePlayer* MtrPlayerManager::local() {
    {
        std::scoped_lock lk(m_mu);
        for (auto& up : m_players) {
            if (up && up->is_local()) return up.get();
        }
    }
    // Lazy register on first call: read engine_wilbur from the well-known
    // VA. If it's still NULL (engine hasn't booted player yet), give up
    // for this call — caller can retry later.
    void* engine_wilbur = engine_player::engine_wilbur_ptr();
    if (!engine_wilbur) {
        // Avoid spamming the log on repeated misses.
        bool first_miss = false;
        {
            std::scoped_lock lk(m_mu);
            first_miss = !m_local_lazy_tried;
            m_local_lazy_tried = true;
        }
        if (first_miss) {
            mtr::log::info("[mtr_player_manager] local: engine_wilbur not yet"
                           " populated; will retry on next call");
        }
        return nullptr;
    }
    return register_local(engine_wilbur);
}

std::size_t MtrPlayerManager::count() const {
    std::scoped_lock lk(m_mu);
    return m_players.size();
}

bool MtrPlayerManager::has_remote() const {
    std::scoped_lock lk(m_mu);
    return has_remote_locked();
}

uint64_t MtrPlayerManager::pulse_count() const {
    std::scoped_lock lk(m_mu);
    return m_pulse_count;
}

void MtrPlayerManager::do_pulse() {
    // Phase 1.4d — auto-orphan-spawn-on-session.
    //
    // When all four conditions hold, fire try_spawn_p2() to materialise an
    // orphan for the peer. Conditions:
    //   (i)  a co-op session is active (host or client),
    //   (ii) at least one packet from the peer has arrived (m_peer_seen
    //        latched by on_remote_packet on the recv thread),
    //   (iii) no Remote wrapper exists yet (idempotency),
    //   (iv) the engine is in gameplay state (is_in_gameplay() — same gate
    //        the manual probe uses; otherwise try_spawn_p2 would early-
    //        return with "gated: not in gameplay" and the trigger would
    //        be consumed without effect on a buffered flag).
    //
    // Retry policy: m_peer_seen is a LATCH, not a one-shot. While
    // is_in_gameplay() returns false (main menu / loading / cutscene),
    // do_pulse re-checks every sim tick until the engine reaches gameplay,
    // at which point try_spawn_p2 fires. The has_remote_locked() check
    // then prevents the auto-path from re-firing on subsequent pulses.
    //
    // Thread safety: this runs on the sim thread (sim_decouple aggregator
    // POST hook). try_spawn_p2 itself installs/uninstalls breadcrumb
    // hooks and calls entity_factory_construct — engine-thread-only
    // operations. The recv thread never reaches this code; it only
    // writes the m_peer_seen atomic in on_remote_packet (Principle 7
    // — transport stays at the wrapper boundary).
    //
    // Spawn position: try_spawn_p2 internally uses engine_wilbur's
    // current world position (read at call time) so the orphan spawns
    // at the local player's location rather than {0,0,0} which may be
    // outside level bounds. See coop_spawn_probe.cpp kInitPos logic.
    //
    // MTA precedent: CPacketHandler::Packet_PlayerList calls
    // `new CClientPlayer(...)` synchronously on the game thread, gated
    // on STATUS_JOINED — the analogue of our is_in_gameplay() check.
    // No queue, no recv-thread construction.
    // reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:929
    {
        auto& net = mtr::coop::net::NetSession::instance();
        if (net.active()
            && m_peer_seen.load(std::memory_order_relaxed)) {
            // Brief lock just for has_remote_locked. We RELEASE it before
            // calling try_spawn_p2 because try_spawn_p2 → register_remote
            // re-acquires m_mu; holding it across that call would
            // self-deadlock.
            bool need_spawn = false;
            {
                std::scoped_lock lk(m_mu);
                need_spawn = !has_remote_locked();
            }
            if (need_spawn && mtr::coop_spawn_probe::is_in_gameplay()) {
                // try_spawn_p2 logs its own diagnostic on the result; we
                // don't double-log here to keep the heartbeat clean.
                // try_spawn_p2 (with session active) keeps the orphan
                // alive AND calls register_remote internally — by the
                // next pulse, has_remote_locked() returns true and the
                // auto-spawn loop terminates.
                mtr::coop_spawn_probe::try_spawn_p2();
            }
        }
    }

    bool should_log = false;
    uint64_t pulse_snap = 0;
    std::size_t n_snap = 0;
    uint64_t p0_snap = 0, p1_snap = 0;
    // Aggregate interp diagnostics (Phase 1.1). Snapshot under the lock,
    // print outside — same pattern as the pulse_count snapshot above, so
    // logging never re-enters log's mutex while we hold m_mu.
    uint64_t interp_writes_sum = 0;
    uint64_t interp_pushes_sum = 0;
    uint64_t interp_faults_sum = 0;

    {
        std::scoped_lock lk(m_mu);

        // Even when no players are registered, advance the counter so the
        // "is the pulse wired" diagnostic works (early ticks before any
        // spawn show pulse_count > 0, players=0).
        for (auto& up : m_players) {
            if (up) up->do_pulse();
        }
        ++m_pulse_count;
        // Periodic heartbeat: every 60 sim pulses. At default sim_hz=60
        // this is once per second. Snapshot the values under the lock so
        // they're consistent in the log line, but RELEASE the lock before
        // calling log::info — log holds its own mutex and we don't want
        // any m_mu → s_mu order to be reachable, so future log extensions
        // can never deadlock against the manager. Audit fix 2026-05-12.
        if ((m_pulse_count % 60) == 0) {
            should_log = true;
            pulse_snap = m_pulse_count;
            n_snap = m_players.size();
            if (n_snap > 0 && m_players[0]) p0_snap = m_players[0]->pulse_count();
            if (n_snap > 1 && m_players[1]) p1_snap = m_players[1]->pulse_count();
            for (auto& up : m_players) {
                if (!up) continue;
                interp_writes_sum += up->interp_writes();
                interp_pushes_sum += up->interp_pushes();
                interp_faults_sum += up->interp_faults();
            }
        }
    }

    if (should_log) {
        // Default heartbeat stays as-is for backward-compatible log
        // greps. Interp diagnostics tail on only when interesting (any
        // non-zero), so the inert-scaffold case adds nothing to the log.
        if (interp_writes_sum || interp_pushes_sum || interp_faults_sum) {
            mtr::log::info("[mtr_player_manager] pulse: total=%llu players=%zu"
                           " p0_pulses=%llu p1_pulses=%llu"
                           " interp_writes=%llu pushes=%llu faults=%llu",
                           static_cast<unsigned long long>(pulse_snap), n_snap,
                           static_cast<unsigned long long>(p0_snap),
                           static_cast<unsigned long long>(p1_snap),
                           static_cast<unsigned long long>(interp_writes_sum),
                           static_cast<unsigned long long>(interp_pushes_sum),
                           static_cast<unsigned long long>(interp_faults_sum));
        } else {
            mtr::log::info("[mtr_player_manager] pulse: total=%llu players=%zu"
                           " p0_pulses=%llu p1_pulses=%llu",
                           static_cast<unsigned long long>(pulse_snap), n_snap,
                           static_cast<unsigned long long>(p0_snap),
                           static_cast<unsigned long long>(p1_snap));
        }
    }

    // Phase 1.4c — emit local pose on the wire (when a co-op session is
    // active). Capture is OUTSIDE m_mu so capture_entity_pose's SEH path
    // never escapes while we hold the manager mutex; encode + send are
    // also outside the lock to keep sendto from ever blocking sim with
    // m_mu held (reviewer P3 mitigation at root: lock-free send path).
    // The wire id is session-determined via NetSession; the time_ctx is
    // a sim-thread-only modular counter on the manager (no lock needed).
    auto& net = mtr::coop::net::NetSession::instance();
    if (net.active()) {
        if (void* w = engine_player::engine_wilbur_ptr()) {
            mtr::interp::EntityPose send_pose{};
            if (mtr::interp::capture_entity_pose(w, send_pose)) {
                mtr::coop::net::PoseSnapshotBody body{};
                body.player_id = net.local_wire_id();
                body.time_ctx  = m_send_time_ctx++;
                std::memcpy(body.pos, send_pose.pos, sizeof(body.pos));
                std::memcpy(body.rot, send_pose.rot, sizeof(body.rot));
                uint8_t buf[mtr::coop::net::kPoseSnapshotPacketSize];
                const std::size_t written =
                    mtr::coop::net::encode_pose_snapshot(buf, sizeof(buf), body);
                if (written == mtr::coop::net::kPoseSnapshotPacketSize) {
                    net.send(buf, written);
                }
            }
        }
    }
}

void MtrPlayerManager::on_remote_packet(
    const mtr::coop::net::PoseSnapshotBody& body) {
    // Capture local-clock QPC BEFORE acquiring m_mu so the snapshot's
    // qpc reflects wall-clock arrival, not the moment after lock
    // acquisition (which could be milliseconds later under sim-thread
    // contention). Sender's QPC is meaningless across machines and is
    // intentionally not on the wire.
    const uint64_t recv_qpc = mtr::interp::qpc_now();

    // Stack-copy the body fields we'll touch outside the lock so the
    // log lines have stable values regardless of recv-thread reuse of
    // the parse buffer in NetSession.
    const uint8_t  body_player_id = body.player_id;
    const uint16_t body_time_ctx  = body.time_ctx;

    bool first_recv          = false;
    bool unknown_id_first    = false;
    bool out_of_order        = false;

    {
        std::scoped_lock lk(m_mu);
        MtrRemotePlayer* p = by_player_id_locked(body_player_id);
        if (!p) {
            // No wrapper for this id yet (e.g. orphan not yet spawned).
            // Log only the first occurrence — later packets may be
            // legitimately racing the spawn path.
            //
            // Phase 1.4d — signal the sim thread to materialise the
            // orphan on its next pulse. relaxed because the only thing
            // that matters is "eventually visible to do_pulse"; a missed
            // observation just means spawn happens one tick later, no
            // correctness consequence.
            m_peer_seen.store(true, std::memory_order_relaxed);
            if (!m_unknown_id_logged) {
                m_unknown_id_logged = true;
                unknown_id_first = true;
            }
        } else {
            // Modular out-of-order test (kProtocolVersion=2 widens
            // this to 16-bit). uint16_t - uint16_t promotes to (signed)
            // int in C++; explicit cast back to uint16_t recovers
            // wrap-around semantics. The 0xFFFF init value of
            // m_last_time_ctx + this comparison together let the first
            // packet of any sequence land cleanly.
            const uint16_t delta = static_cast<uint16_t>(
                body_time_ctx - p->last_time_ctx());
            if (delta >= 32768) {
                out_of_order = true;
            } else {
                p->update_time_ctx(body_time_ctx);
                p->set_interp_enabled(true);
                p->push_interp_snapshot_at(body.pos, body.rot, recv_qpc);
                if (!m_first_recv_logged) {
                    m_first_recv_logged = true;
                    first_recv = true;
                }
            }
        }
    }
    // Log outside the lock — discipline matches register_*, unregister,
    // and the do_pulse heartbeat. Out-of-order drops are intentionally
    // silent here; the diagnostic surface is NetSession::packets_recvd
    // versus the manager's interp_pushes_sum in the periodic heartbeat.
    if (first_recv) {
        mtr::log::info("[mtr_player_manager] on_remote_packet: first packet "
                       "accepted (wire player_id=%u)",
                       static_cast<unsigned>(body_player_id));
    }
    if (unknown_id_first) {
        mtr::log::info("[mtr_player_manager] on_remote_packet: no remote "
                       "wrapper for wire player_id=%u (dropping; future "
                       "occurrences silent until a remote is registered)",
                       static_cast<unsigned>(body_player_id));
    }
    (void)out_of_order;
}

void MtrPlayerManager::unregister(MtrRemotePlayer* p) {
    if (!p) return;
    std::unique_ptr<MtrRemotePlayer> condemned;
    bool not_found = false;
    bool was_remote = false;
    {
        std::scoped_lock lk(m_mu);
        auto it = std::find_if(m_players.begin(), m_players.end(),
                               [p](const std::unique_ptr<MtrRemotePlayer>& up) {
                                   return up.get() == p;
                               });
        if (it == m_players.end()) {
            not_found = true;
        } else {
            condemned = std::move(*it);
            m_players.erase(it);
            if (condemned && condemned->engine_entity()) {
                m_by_engine.erase(condemned->engine_entity());
            }
            if (condemned
                && condemned->role() == MtrRemotePlayer::Role::Remote) {
                was_remote = true;
            }
            // Phase 1.4d — clear the one-shot log latches and the peer-seen
            // flag iff we just dropped a Remote. A reconnect from the peer
            // (different session, same process) must re-arm the auto-spawn
            // path AND re-log the first-recv / unknown-id diagnostics; the
            // process-lifetime latches otherwise silently swallow the
            // second session's diagnostics. Local-player unregister does
            // not arm any of these — leave the flags alone.
            if (was_remote && !has_remote_locked()) {
                m_first_recv_logged = false;
                m_unknown_id_logged = false;
                m_peer_seen.store(false, std::memory_order_relaxed);
            }
        }
    }
    // Outside the lock: log + drive engine teardown iff owns_engine.
    if (not_found) {
        mtr::log::info("[mtr_player_manager] unregister: %p not found", p);
        return;
    }
    if (condemned) {
        const uint8_t id = condemned->player_id();
        condemned->destroy_engine_entity_seh();
        mtr::log::info("[mtr_player_manager] unregister: id=%u done", id);
    }
}

} // namespace mtr::coop
