// Scene-visibility activity tracker.
//
// Static RE established that (scene+104) bit 0 is the engine's master
// "scene hidden" flag, and we identified all 8 writers (renamed in IDB).
// None of them are *statically* camera-driven — but two writers can be
// indirectly camera-driven if the scripts they consume change with camera
// state:
//
//   - scene_set_visible (sub_4AABC0): explicit (scene, on) API. Anyone
//     can call it, including script handlers.
//   - script_set_instance_hidden (sub_5E3DC0): reads `instance_hidden`
//     property from a script object; toggles bit 0 to match. If a per-
//     frame script handler updates the property based on camera angle,
//     this fires with effectively-camera-driven semantics.
//
// This module hooks both, counts invocations per frame, and snapshots the
// last completed frame's counts for the menu UI. If the user pans the
// camera at corners and these counters spike, the corner-cull is going
// through one of these paths. If counters stay near zero, the cull is
// happening through a non-(scene+104)-bit-0 mechanism (and the static-RE
// conclusion that (scene+104) bit 0 is not the corner-cull driver gets
// runtime confirmation).
//
// Also keeps a "sticky" log of the last 8 distinct scene pointers that
// were hidden in the most recent frame, so a single corner-pan can be
// translated into a small set of scene addresses to investigate.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::scene_vis_log {

namespace {

// Renamed in IDB — the canonical scene visibility setter.
constexpr uintptr_t kSceneSetVisibleVA       = 0x004AABC0;
// Renamed in IDB — script `instance_hidden` property reader / bit-0 toggle.
constexpr uintptr_t kScriptSetInstanceHiddenVA = 0x005E3DC0;

// scene_set_visible(scene, on): __stdcall(int, char). Returns int.
// __stdcall = no `this`, callee-cleanup. We map it as a regular cdecl
// with explicit args; MinHook handles the calling convention via the
// trampoline (the hook function uses the same convention as orig).
using PFN_SceneSetVisible        = int (__stdcall*)(int scene, char on);
// script_set_instance_hidden: __thiscall(int this). Returns int.
using PFN_ScriptSetInstanceHidden = int (__fastcall*)(int this_, int /*edx*/);

PFN_SceneSetVisible        g_orig_set_visible       = nullptr;
PFN_ScriptSetInstanceHidden g_orig_script_set_hidden = nullptr;

// Per-frame counters. Read-and-reset by frame_tick(); UI reads the
// `last_*` snapshots to display the previous frame's stats (avoids
// in-flight torn reads).
std::atomic<uint64_t> g_frame_hides{0};
std::atomic<uint64_t> g_frame_shows{0};
std::atomic<uint64_t> g_frame_script_calls{0};
std::atomic<uint64_t> g_frame_script_hides{0};
std::atomic<uint64_t> g_frame_script_shows{0};

// Cumulative since install (sanity check; never reset).
std::atomic<uint64_t> g_cum_hides{0};
std::atomic<uint64_t> g_cum_shows{0};
std::atomic<uint64_t> g_cum_script_calls{0};

// Snapshots that the UI reads (last completed frame's counts).
std::atomic<uint64_t> g_last_hides{0};
std::atomic<uint64_t> g_last_shows{0};
std::atomic<uint64_t> g_last_script_calls{0};
std::atomic<uint64_t> g_last_script_hides{0};
std::atomic<uint64_t> g_last_script_shows{0};

// Sticky log of distinct scene pointers hidden in the current frame
// (rolling window, deduplicated). Helps translate "saw N hides at corners"
// into a small list of addresses we can chase down.
constexpr int kMaxStickyScenes = 8;
std::atomic<uint32_t> g_sticky_scenes[kMaxStickyScenes]{};
std::atomic<int>      g_sticky_count{0};

void sticky_record_hide(uint32_t scene_va) {
    if (scene_va == 0) return;
    // Linear scan — small N, lock-free dedup is unnecessary if we tolerate
    // occasional dupes during races (this is diagnostic logging).
    const int n = g_sticky_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < kMaxStickyScenes; ++i) {
        if (g_sticky_scenes[i].load(std::memory_order_relaxed) == scene_va) return;
    }
    if (n < kMaxStickyScenes) {
        // CAS the slot to avoid two threads claiming the same index.
        int idx = g_sticky_count.fetch_add(1, std::memory_order_relaxed);
        if (idx < kMaxStickyScenes) {
            g_sticky_scenes[idx].store(scene_va, std::memory_order_relaxed);
        }
    }
}

int __stdcall hk_scene_set_visible(int scene, char on) {
    if (on) {
        g_frame_shows.fetch_add(1, std::memory_order_relaxed);
        g_cum_shows.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_frame_hides.fetch_add(1, std::memory_order_relaxed);
        g_cum_hides.fetch_add(1, std::memory_order_relaxed);
        sticky_record_hide(static_cast<uint32_t>(scene));
    }
    return g_orig_set_visible(scene, on);
}

int __fastcall hk_script_set_instance_hidden(int this_, int /*edx*/) {
    g_frame_script_calls.fetch_add(1, std::memory_order_relaxed);
    g_cum_script_calls.fetch_add(1, std::memory_order_relaxed);

    int rc = g_orig_script_set_hidden(this_, 0);

    // Inspect the resulting bit 0 to bucket as hide vs show. The decompile
    // shows the function reads (this+4) → (+492) → (+104) chain. Each
    // dereference must be guarded — these are pointers freshly written
    // by the orig, so they MAY be null/invalid in edge cases (script
    // object not fully initialized, etc.). If the chain breaks, we just
    // skip the hide/show classification for this call.
    if (this_) {
        __try {
            int* p_inner = reinterpret_cast<int*>(this_ + 4);
            int inner = *p_inner;
            if (inner) {
                int* p_scene = reinterpret_cast<int*>(inner + 492);
                int scene = *p_scene;
                if (scene) {
                    BYTE flag = *reinterpret_cast<BYTE*>(scene + 104) & 1;
                    if (flag) {
                        g_frame_script_hides.fetch_add(1, std::memory_order_relaxed);
                        sticky_record_hide(static_cast<uint32_t>(scene));
                    } else {
                        g_frame_script_shows.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Pointer chain dereference faulted — skip classification.
        }
    }
    return rc;
}

} // namespace

bool install() {
    void* p_set = reinterpret_cast<void*>(kSceneSetVisibleVA);
    if (MH_CreateHook(p_set, &hk_scene_set_visible,
                      reinterpret_cast<void**>(&g_orig_set_visible)) != MH_OK) {
        mtr::log::info("scene_vis_log: MH_CreateHook(scene_set_visible @%p) failed", p_set);
        return false;
    }
    if (MH_EnableHook(p_set) != MH_OK) {
        mtr::log::info("scene_vis_log: MH_EnableHook(scene_set_visible) failed");
        return false;
    }
    mtr::log::info("scene_vis_log: hooked scene_set_visible at %p", p_set);

    void* p_script = reinterpret_cast<void*>(kScriptSetInstanceHiddenVA);
    if (MH_CreateHook(p_script, &hk_script_set_instance_hidden,
                      reinterpret_cast<void**>(&g_orig_script_set_hidden)) != MH_OK) {
        mtr::log::info("scene_vis_log: MH_CreateHook(script_set_instance_hidden @%p) failed",
                       p_script);
    } else if (MH_EnableHook(p_script) != MH_OK) {
        mtr::log::info("scene_vis_log: MH_EnableHook(script_set_instance_hidden) failed");
    } else {
        mtr::log::info("scene_vis_log: hooked script_set_instance_hidden at %p", p_script);
    }
    return true;
}

// Snapshot the current frame's counters into the `last_*` atomics and
// reset the per-frame counters. Called from EndScene (alongside
// vis_test_probe::frame_tick) so the menu sees stable last-frame counts.
void frame_tick() {
    g_last_hides.store(g_frame_hides.exchange(0));
    g_last_shows.store(g_frame_shows.exchange(0));
    g_last_script_calls.store(g_frame_script_calls.exchange(0));
    g_last_script_hides.store(g_frame_script_hides.exchange(0));
    g_last_script_shows.store(g_frame_script_shows.exchange(0));

    // Reset sticky list at frame boundary so each "pan to corner" gives
    // a fresh capture.
    g_sticky_count.store(0, std::memory_order_relaxed);
    for (int i = 0; i < kMaxStickyScenes; ++i) {
        g_sticky_scenes[i].store(0, std::memory_order_relaxed);
    }
}

uint64_t last_hides()        { return g_last_hides.load(); }
uint64_t last_shows()        { return g_last_shows.load(); }
uint64_t last_script_calls() { return g_last_script_calls.load(); }
uint64_t last_script_hides() { return g_last_script_hides.load(); }
uint64_t last_script_shows() { return g_last_script_shows.load(); }
uint64_t cum_hides()         { return g_cum_hides.load(); }
uint64_t cum_shows()         { return g_cum_shows.load(); }
uint64_t cum_script_calls()  { return g_cum_script_calls.load(); }

int sticky_scene_count() {
    int n = g_sticky_count.load();
    return (n > kMaxStickyScenes) ? kMaxStickyScenes : n;
}

uint32_t sticky_scene_at(int idx) {
    if (idx < 0 || idx >= kMaxStickyScenes) return 0;
    return g_sticky_scenes[idx].load();
}

} // namespace mtr::scene_vis_log
