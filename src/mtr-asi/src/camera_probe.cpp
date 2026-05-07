// Camera-subsystem runtime probe.
//
// Goal: identify the camera struct's POSE fields and find the per-frame
// gameplay-camera-tick. Static analysis is insufficient because:
//   (a) game_camera_apply_state has 0 xrefs (vtable-dispatched),
//   (b) game_BuildPerspectiveMatrix's only "main scene" caller is
//       game_camera_recompute_projection, which writes to a CACHED matrix —
//       the renderer reads from the cache, not the builder, so we can't
//       statically find "what writes the camera matrix per frame".
//
// Approach:
//   1. Log every distinct camera struct passed to game_camera_recompute_projection
//      (this_) on first observation. We know its size is 0x270 = 624 bytes.
//   2. On every Nth frame, dump the struct contents and diff against last
//      dump per slot. Fields that change every frame = pose-related.
//   3. Log every D3DTS_VIEW matrix sent through wrap_SetTransform with caller
//      address — gives us the actual VIEW matrix sources.
//
// Outputs go to Game/mtr-asi.log with [camprobe] prefix.

#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::camera_probe {

namespace {

constexpr size_t kCamStructSize = 0x270;   // from game_camera_init_with_defaults memset
constexpr int    kMaxTrackedCams = 8;

struct TrackedCam {
    void*    addr;
    uint64_t hits;
    uint8_t  last_snapshot[kCamStructSize];
    bool     have_snapshot;
};

TrackedCam        g_tracked[kMaxTrackedCams]{};
std::atomic<int>  g_tracked_count{0};
std::mutex        g_tracked_mu;

constexpr int    kMaxViewCallers = 16;
struct ViewCallerEntry {
    void* caller;
    uint64_t hits;
};
ViewCallerEntry  g_view_callers[kMaxViewCallers]{};
std::atomic<int> g_view_caller_count{0};
std::mutex       g_view_caller_mu;

std::atomic<uint64_t> g_frame{0};

void log_diff(void* addr, uint64_t hit, const uint8_t* old_buf, const uint8_t* new_buf) {
    // Walk in 4-byte words, print groups of contiguous changed words.
    const int max_changes_logged = 12;
    int changes = 0;
    int i = 0;
    while (i < (int)kCamStructSize && changes < max_changes_logged) {
        // align to 4
        const int wi = i & ~3;
        if (wi + 4 > (int)kCamStructSize) break;
        const uint32_t a = *reinterpret_cast<const uint32_t*>(old_buf + wi);
        const uint32_t b = *reinterpret_cast<const uint32_t*>(new_buf + wi);
        if (a != b) {
            // try as float
            float fa, fb;
            std::memcpy(&fa, &a, 4);
            std::memcpy(&fb, &b, 4);
            const bool fa_finite = (fa == fa) && (fa > -1e20f) && (fa < 1e20f);
            const bool fb_finite = (fb == fb) && (fb > -1e20f) && (fb < 1e20f);
            if (fa_finite && fb_finite && (fa != 0.0f || fb != 0.0f)) {
                mtr::log::info("[camprobe] cam=%p hit=%llu  +%-3d (0x%03X)  %.6f -> %.6f  (0x%08X -> 0x%08X)",
                               addr, hit, wi, wi, fa, fb, a, b);
            } else {
                mtr::log::info("[camprobe] cam=%p hit=%llu  +%-3d (0x%03X)  0x%08X -> 0x%08X",
                               addr, hit, wi, wi, a, b);
            }
            ++changes;
        }
        i = wi + 4;
    }
    if (changes >= max_changes_logged) {
        mtr::log::info("[camprobe] cam=%p hit=%llu  ... (truncated, more changes follow)", addr, hit);
    }
}

} // namespace

// Called from hk_CameraCompute (every entry to game_camera_recompute_projection).
void on_camera_compute(void* this_) {
    if (!this_) return;

    const uint64_t frame = g_frame.fetch_add(1) + 1;

    std::scoped_lock lock(g_tracked_mu);
    int slot = -1;
    const int n = g_tracked_count.load();
    for (int i = 0; i < n; ++i) {
        if (g_tracked[i].addr == this_) { slot = i; break; }
    }
    if (slot < 0) {
        if (n >= kMaxTrackedCams) return;
        slot = n;
        g_tracked[slot].addr = this_;
        g_tracked[slot].hits = 0;
        g_tracked[slot].have_snapshot = false;
        g_tracked_count.store(n + 1);
        mtr::log::info("[camprobe] new camera struct #%d at %p (frame=%llu)", slot, this_, frame);
    }
    g_tracked[slot].hits++;

    // Snapshot every 60 hits per camera. First snapshot establishes baseline,
    // subsequent ones diff against the previous.
    const uint64_t hit = g_tracked[slot].hits;
    if (hit == 1 || (hit % 60) == 0) {
        uint8_t buf[kCamStructSize];
        // The struct is at least 0x270 bytes. Use SEH wrapping at the call site
        // is overkill — stick to basic memcpy and hope it's mapped.
        std::memcpy(buf, this_, kCamStructSize);

        if (g_tracked[slot].have_snapshot) {
            // Diff dump.
            log_diff(this_, hit, g_tracked[slot].last_snapshot, buf);
        } else {
            // Initial dump — log first 64 dwords as floats so we can identify
            // pose fields.
            mtr::log::info("[camprobe] BASELINE cam=%p hit=%llu  size=0x%X", this_, hit, (unsigned)kCamStructSize);
            for (int i = 0; i < 64; ++i) {
                uint32_t w = *reinterpret_cast<const uint32_t*>(buf + i*4);
                float f; std::memcpy(&f, &w, 4);
                const bool finite = (f == f) && (f > -1e20f) && (f < 1e20f) && (f != 0.0f);
                if (finite) {
                    mtr::log::info("[camprobe]    +%-3d (0x%03X)  %.6f  (0x%08X)", i*4, i*4, f, w);
                } else if (w != 0) {
                    mtr::log::info("[camprobe]    +%-3d (0x%03X)  0x%08X", i*4, i*4, w);
                }
            }
            g_tracked[slot].have_snapshot = true;
        }
        std::memcpy(g_tracked[slot].last_snapshot, buf, kCamStructSize);
    }
}

// Called from the sub_4C1BA0 hook. Captures the outer camera struct that
// owns the view matrix (pointer at this+0x34 — sub_4C1BA0 does
// `mov edx,[esi+34h]; push edx; matrix4_copy(...)` so it's a pointer not
// embedded) and the world matrix at this+0x2B0 (embedded; sub_4C1BA0 does
// `lea eax,[esi+2B0h]; push eax; matrix4_copy(...)`).
namespace {
constexpr int kMaxOuterCams = 8;
struct OuterCam {
    void*    addr;
    uint64_t hits;
};
OuterCam        g_outer[kMaxOuterCams]{};
std::atomic<int> g_outer_count{0};
std::mutex      g_outer_mu;
} // namespace

void on_camera_apply(void* this_) {
    if (!this_) return;
    std::scoped_lock lock(g_outer_mu);
    int slot = -1;
    const int n = g_outer_count.load();
    for (int i = 0; i < n; ++i) {
        if (g_outer[i].addr == this_) { slot = i; break; }
    }
    if (slot < 0) {
        if (n >= kMaxOuterCams) return;
        slot = n;
        g_outer[slot].addr = this_;
        g_outer[slot].hits = 0;
        g_outer_count.store(n + 1);
        mtr::log::info("[outercam] new camera struct #%d at %p", slot, this_);

        // First-hit header dump: outer+0 (vtable), +4..+0x33 (header before
        // projection-cache at +0x40). This identifies the camera class —
        // outer+0 is the vtable pointer; we can then look up the vtable in
        // IDA and find the camera-tick method.
        const uint8_t* base = static_cast<const uint8_t*>(this_);
        const uint32_t* hdr = reinterpret_cast<const uint32_t*>(base);
        mtr::log::info("[outercam]   header dwords (outer+0x00..+0x33):");
        for (int i = 0; i < 13; ++i) {
            mtr::log::info("[outercam]     +%-3d (0x%02X)  0x%08X", i*4, i*4, hdr[i]);
        }
    }
    g_outer[slot].hits++;

    const uint64_t hit = g_outer[slot].hits;

    // Every 120 hits (~2 sec), dump the WORLD matrix at this+0x2B0 (camera-to-
    // world; row3 = eye position) and the VIEW matrix (followed via this+0x34
    // which is a POINTER, not embedded — sub_4C1BA0 does mov edx,[esi+34h] then
    // matrix4_copy with edx as src).
    if (hit == 1 || (hit % 120) == 0) {
        const uint8_t* base = static_cast<const uint8_t*>(this_);
        // world matrix embedded at +0x2B0
        const float* w = reinterpret_cast<const float*>(base + 0x2B0);
        mtr::log::info("[outercam] cam=%p hit=%llu  WORLD@+0x2B0:", this_, hit);
        mtr::log::info("[outercam]   row0: %9.4f %9.4f %9.4f %9.4f", w[0],  w[1],  w[2],  w[3]);
        mtr::log::info("[outercam]   row1: %9.4f %9.4f %9.4f %9.4f", w[4],  w[5],  w[6],  w[7]);
        mtr::log::info("[outercam]   row2: %9.4f %9.4f %9.4f %9.4f", w[8],  w[9],  w[10], w[11]);
        mtr::log::info("[outercam]   row3 (eye): %9.4f %9.4f %9.4f %9.4f", w[12], w[13], w[14], w[15]);

        // view matrix is at *(this+0x34). Pointer may be NULL early in init.
        const void* view_ptr = *reinterpret_cast<void* const*>(base + 0x34);
        mtr::log::info("[outercam] cam=%p hit=%llu  view-ptr@+0x34 = %p", this_, hit, view_ptr);
        if (view_ptr) {
            const float* v = static_cast<const float*>(view_ptr);
            mtr::log::info("[outercam]   view row0: %9.4f %9.4f %9.4f %9.4f", v[0],  v[1],  v[2],  v[3]);
            mtr::log::info("[outercam]   view row1: %9.4f %9.4f %9.4f %9.4f", v[4],  v[5],  v[6],  v[7]);
            mtr::log::info("[outercam]   view row2: %9.4f %9.4f %9.4f %9.4f", v[8],  v[9],  v[10], v[11]);
            mtr::log::info("[outercam]   view row3 (-eye): %9.4f %9.4f %9.4f %9.4f", v[12], v[13], v[14], v[15]);
        }
    }
}

// Called from sub_58C910 hook (the gameplay-camera-tick we identified
// from matrix4_copy callers). Dumps the controller's `this` and its
// first 64 bytes — vtable + sub-object pointers.
void on_camera_tick(void* this_) {
    if (!this_) return;
    static std::atomic<int> g_logged{0};
    if (g_logged.load() >= 4) return;

    static std::mutex g_mu;
    std::scoped_lock lock(g_mu);
    if (g_logged.load() >= 4) return;
    g_logged.fetch_add(1);

    const uint32_t* h = static_cast<const uint32_t*>(this_);
    mtr::log::info("[camtick] this=%p  vtable=0x%08X", this_, h[0]);
    for (int i = 0; i < 16; ++i) {
        mtr::log::info("[camtick]    +%-3d (0x%02X) = 0x%08X", i*4, i*4, h[i]);
    }
    // Dereference +16 (the sub-object containing the view matrix at +16):
    const uint32_t sub_ptr = h[4];
    if (sub_ptr) {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(sub_ptr);
        mtr::log::info("[camtick]   sub@%p (controller_this+16):", reinterpret_cast<void*>(sub_ptr));
        for (int i = 0; i < 8; ++i) {
            mtr::log::info("[camtick]     sub +%-3d (0x%02X) = 0x%08X", i*4, i*4, s[i]);
        }
    }
}

// Called from hk_matrix4_copy when its destination is in the range that
// contains the known view-matrix object of any tracked outer-cam.
// Captures the caller (= who's writing the view matrix per frame).
void on_matrix4_copy(void* caller, void* dst, void* src) {
    static std::atomic<int> g_logged{0};
    static std::mutex       g_mu;

    if (g_logged.load() >= 32) return;

    // Check if dst is within +0x370 of any tracked outer-cam (view matrix
    // object) OR exactly the +0x2B0 world-matrix slot.
    bool relevant = false;
    const char* kind = "";
    {
        std::scoped_lock lock(g_outer_mu);
        const int n = g_outer_count.load();
        for (int i = 0; i < n; ++i) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(g_outer[i].addr);
            const uintptr_t d    = reinterpret_cast<uintptr_t>(dst);
            if (d == base + 0x370) { relevant = true; kind = "VIEW@+0x370"; break; }
            if (d == base + 0x2B0) { relevant = true; kind = "WORLD@+0x2B0"; break; }
            // dst could also point to a separate heap object pointed to by
            // *(outer+0x34) or *(outer+0x... ): try *(outer+0x34)
            const void* vp = *reinterpret_cast<void* const*>(base + 0x34);
            if (dst == vp) { relevant = true; kind = "VIEW (via +0x34 ptr)"; break; }
        }
    }
    if (!relevant) return;

    std::scoped_lock lock(g_mu);
    if (g_logged.load() >= 32) return;
    g_logged.fetch_add(1);
    mtr::log::info("[matcopy] caller=%p dst=%p src=%p kind=%s", caller, dst, src, kind);
}

// Called from hk_WrapSetTransform when state == D3DTS_VIEW. Logs unique callers
// of the view matrix path with a sample matrix for each.
void on_view_matrix(void* caller, const D3DMATRIX* m) {
    if (!m) return;

    std::scoped_lock lock(g_view_caller_mu);
    const int n = g_view_caller_count.load();
    int slot = -1;
    for (int i = 0; i < n; ++i) {
        if (g_view_callers[i].caller == caller) { slot = i; break; }
    }
    if (slot < 0) {
        if (n >= kMaxViewCallers) return;
        slot = n;
        g_view_callers[slot].caller = caller;
        g_view_callers[slot].hits = 0;
        g_view_caller_count.store(n + 1);

        // Decode the matrix as if it were a row-major LookAtRH view:
        //   eye recovered from m[3][0..2] dotted with rotation columns.
        const float r[3] = { m->m[0][0], m->m[1][0], m->m[2][0] };
        const float u[3] = { m->m[0][1], m->m[1][1], m->m[2][1] };
        const float z[3] = { m->m[0][2], m->m[1][2], m->m[2][2] };
        const float t[3] = { m->m[3][0], m->m[3][1], m->m[3][2] };
        const float eye[3] = {
            -(t[0]*r[0] + t[1]*u[0] + t[2]*z[0]),
            -(t[0]*r[1] + t[1]*u[1] + t[2]*z[1]),
            -(t[0]*r[2] + t[1]*u[2] + t[2]*z[2]),
        };
        const float fwd[3] = { -z[0], -z[1], -z[2] };

        mtr::log::info("[camprobe] new VIEW caller=%p  eye=(%.2f,%.2f,%.2f)  fwd=(%.3f,%.3f,%.3f)  m33=%.3f",
                       caller, eye[0], eye[1], eye[2], fwd[0], fwd[1], fwd[2], m->m[3][3]);
    }
    g_view_callers[slot].hits++;
}

} // namespace mtr::camera_probe
