// MtrRemotePlayer — Phase 1 skeleton implementation.
// See mtr/coop/remote_player.h for design rationale.

#include "mtr/coop/remote_player.h"

#include <windows.h>

#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop {

MtrRemotePlayer::MtrRemotePlayer(uint8_t player_id, Role role,
                                 void* engine_entity, bool owns_engine)
    : m_player_id(player_id),
      m_role(role),
      m_engine_entity(engine_entity),
      m_owns_engine(owns_engine) {
    // Intentionally silent — the manager logs after construction completes,
    // OUTSIDE its own mutex. Logging here would force the manager to hold
    // its lock across a log::info call (lock-order hazard: m_mu held while
    // log's s_mu is acquired). Audit fix 2026-05-12.
}

MtrRemotePlayer::~MtrRemotePlayer() {
    // Intentionally silent — see ctor comment. The manager logs unregister
    // events outside its lock.
    //
    // Intentionally does NOT drive engine teardown here. Phase 1 contract:
    // the manager's `unregister()` is the explicit teardown driver. Singleton
    // dtor at process exit must NOT touch engine memory (engine may already
    // be down). When level-transition / disconnect flow lands in a future
    // session, that path calls `manager.unregister(p)` which invokes
    // `destroy_engine_entity_seh()` BEFORE the unique_ptr destructs.
}

void MtrRemotePlayer::do_pulse() {
    ++m_pulse_count;

    // Interp scaffold (Phase 1.1). Only Remote players have their pose
    // overridden; Local owns its pose via the engine's input pipeline.
    if (!m_interp_enabled)        return;
    if (m_role != Role::Remote)   return;
    if (!m_engine_entity)         return;
    if (!m_curr_snap.valid)       return;

    // Quake-style snapshot interp: render the world delayed by the
    // snapshot interval so alpha grows 0→1 between consecutive snapshots.
    //   t0   = prev.qpc      (oldest pose)
    //   t1   = curr.qpc      (newest pose)
    //   t_render = now - (t1 - t0)   (render lags by one window)
    //   alpha = (t_render - t0) / (t1 - t0)
    //         = (now - t1) / (t1 - t0)   (clamped to [0,1])
    // When the window elapses without a fresh push, alpha clamps at 1.0
    // and we hold curr until the next snapshot arrives.
    float alpha = 1.0f;
    if (m_prev_snap.valid && m_curr_snap.qpc > m_prev_snap.qpc) {
        const uint64_t window = m_curr_snap.qpc - m_prev_snap.qpc;
        const uint64_t now    = mtr::interp::qpc_now();
        if (now > m_curr_snap.qpc) {
            const uint64_t elapsed = now - m_curr_snap.qpc;
            if (elapsed >= window) {
                alpha = 1.0f;
            } else {
                alpha = static_cast<float>(elapsed) /
                        static_cast<float>(window);
            }
        } else {
            alpha = 0.0f;
        }
    }

    __try {
        mtr::interp::apply_entity_pose_interp(m_prev_snap, m_curr_snap,
                                              alpha, m_engine_entity);
        ++m_interp_writes;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Stale engine_entity pointer (engine reclaimed the slot). Two
        // changes vs an earlier draft caught by 2026-05-12-b audit:
        //
        //   1. DO NOT call log::info here — caller (MtrPlayerManager::
        //      do_pulse) holds m_mu around this code path; logging would
        //      acquire log's s_mu under m_mu, the exact lock-order
        //      hazard the ctor/dtor audit fix established. Accumulate a
        //      fault counter; the manager surfaces it from outside the
        //      lock in its periodic heartbeat.
        //   2. DO NOT permanently disable interp on this wrapper
        //      (`m_interp_enabled = false` is a catch-and-ignore crutch
        //      per RULE №1). Null the entity ptr (it's dead) and
        //      invalidate snapshots — the wrapper self-heals: a future
        //      push_interp_snapshot + fresh engine_entity registration
        //      will re-prime cleanly. MTA precedent: CClientPed handles
        //      stream-out by nulling m_pPlayerPed and skipping
        //      Interpolate() until the streamer reattaches.
        m_engine_entity = nullptr;
        m_prev_snap.valid = false;
        m_curr_snap.valid = false;
        ++m_interp_faults;
    }
}

void MtrRemotePlayer::push_interp_snapshot(const float pos[3],
                                           const float rot[9]) {
    push_interp_snapshot_at(pos, rot, mtr::interp::qpc_now());
}

void MtrRemotePlayer::push_interp_snapshot_at(const float pos[3],
                                              const float rot[9],
                                              uint64_t qpc) {
    m_prev_snap = m_curr_snap;
    std::memcpy(m_curr_snap.pos, pos, sizeof(m_curr_snap.pos));
    std::memcpy(m_curr_snap.rot, rot, sizeof(m_curr_snap.rot));
    m_curr_snap.qpc   = qpc;
    m_curr_snap.valid = true;
    ++m_interp_pushes;
}

void MtrRemotePlayer::destroy_engine_entity_seh() {
    if (!m_engine_entity) return;
    if (!m_owns_engine) {
        mtr::log::info("[mtr_remote_player] destroy_engine_entity: id=%u —"
                       " refusing (owns_engine=false; engine constructed it)",
                       static_cast<unsigned>(m_player_id));
        return;
    }

    // FIRST-EXECUTION RISK NOTE (audit 2026-05-12):
    // This path has never been exercised in Phase 1 testing. The orphan we
    // call vtable[0] on holds non-trivial live state:
    //   - b7.13-mirrored +0xCCC registry (21 keys cloned from engine_wilbur)
    //   - active node in the per-wilbur subscriber list at +0xD04
    //   - the engine's transform-list node attached during spawn
    // MTA's `~CClientPed` does substantial pre-teardown work before calling
    // the engine destructor (model dereference, task cancel, contact entity,
    // anim cleanup — CClientPed.cpp:291-409). A bare vt[0](this,1) here will
    // *attempt* the same engine-side teardowns on an entity holding state
    // that the engine probably doesn't expect from a "scalar-deleted" path.
    // First test exercising this code should be in an isolated scenario.
    // SEH wrapper keeps any crash localized; do NOT rely on it as "this is
    // safe".
    using PFN_EntityDtor = void (__fastcall*)(void* this_, void* /*edx*/,
                                              int free_flag);
    __try {
        void** vt = *reinterpret_cast<void***>(m_engine_entity);
        auto dtor = reinterpret_cast<PFN_EntityDtor>(vt[0]);
        dtor(m_engine_entity, nullptr, 1);
        mtr::log::info("[mtr_remote_player] destroy_engine_entity: id=%u"
                       " vtable[0](%p, 1) returned cleanly",
                       static_cast<unsigned>(m_player_id), m_engine_entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("[mtr_remote_player] destroy_engine_entity: id=%u"
                       " EXCEPTION inside vtable[0](%p, 1)",
                       static_cast<unsigned>(m_player_id), m_engine_entity);
    }
    m_engine_entity = nullptr;
}

} // namespace mtr::coop
