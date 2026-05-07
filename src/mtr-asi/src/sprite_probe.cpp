// Sprite-batcher list probe (Phase 3 M3.2 instrumentation).
//
// Static RE of the per-entry layout is in research/findings/sprite-list-layout.md
// — entry has a `state_key` field at +0x10 (32-bit) used as the run-grouping
// key, very likely a texture handle or pointer. To classify menu vs HUD
// sprites we need EMPIRICAL data: capture per-entry state_key + sort_key +
// position across multiple game states (main menu / in-game / pause / etc.)
// and offline correlate which keys cluster in which contexts.
//
// HOOK STRATEGY (revised 2026-05-06): we DON'T patch render_sprite_batcher's
// prologue. The first attempt did that via MinHook, and the trampoline copy
// of `sub esp,20h; call sub_564880` apparently caused a black-screen freeze
// after device Reset (likely an issue with copying a `call rel32` into the
// trampoline on the rr01 segment). Instead we patch the SINGLE call site at
// `render_frame_top_level` (0x4D23BF) — a clean 5-byte `call rel32` rewrite
// that bypasses any prologue/trampoline complexity. Original bytes are
// `E8 6C 69 01 00` (call render_sprite_batcher); we replace with
// `E8 <our wrapper rel32>`. The wrapper does its work and then calls the
// real render_sprite_batcher directly via its known VA.
//
// The wrapper is gated by `g_armed`. When not armed, it does nothing
// extra — just calls the original function. Zero overhead when off.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mtr        { HMODULE self_module(); }
namespace mtr::log   { void info(const char* fmt, ...); }
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
    int  stack_depth();
}
namespace mtr::sprite_split {
    bool         enabled();
    unsigned int execute_split();
}
namespace mtr::sprite_xform {
    void process_list();
}

namespace mtr::sprite_probe {

namespace {

// Single call site at 0x4D23BF: `E8 6C 69 01 00` (call rel32 -> 0x4E8D30).
constexpr uintptr_t kCallSiteVA            = 0x004D23BF;
constexpr uintptr_t kRenderSpriteBatcherVA = 0x004E8D30;

// SpriteEntry layout (see research/findings/sprite-list-layout.md). Only
// fields we need; tail beyond +0x90 is unknown.
#pragma pack(push, 4)
struct SpriteEntry {
    /* +0x00 */ SpriteEntry* next;
    /* +0x04 */ uint32_t     unk_04;
    /* +0x08 */ uint32_t     flags;
    /* +0x0C */ uint32_t     unk_0C;
    /* +0x10 */ uint32_t     state_key;
    /* +0x14 */ uint16_t     unk_14;
    /* +0x16 */ uint16_t     sort_key;
    /* +0x18 */ uint32_t     unk_18;
    /* +0x1C */ uint8_t      alpha_mod;
    /* +0x1D */ uint8_t      blend_mode;
    /* +0x1E */ uint16_t     unk_1E;
    /* +0x20 */ uint8_t      pad_20[8];
    /* +0x28 */ float        inline_positions[12];
    /* +0x58 */ float*       ext_positions;
    /* +0x5C */ float        inline_uvs[8];
    /* +0x7C */ float*       ext_uvs;
    /* +0x80 */ uint32_t     inline_colors[4];
};
#pragma pack(pop)
static_assert(sizeof(SpriteEntry) == 0x90, "SpriteEntry layout drift");

constexpr uintptr_t kSpriteListHeadVA = 0x007271E8;

using PFN_RenderSpriteBatcher = unsigned int (__cdecl*)();

std::atomic<bool>     g_installed{false};
std::atomic<bool>     g_armed{false};
std::atomic<int>      g_frames_remaining{0};
std::atomic<uint64_t> g_frame_counter{0};
std::atomic<uint64_t> g_total_entries_captured{0};
std::atomic<uint64_t> g_last_frame_entry_count{0};
std::mutex            g_io_mu;
FILE*                 g_csv = nullptr;
uint8_t               g_orig_call_bytes[5] = {0};   // saved before patching

bool resolve_csv_path(char* out, size_t out_size) {
    if (!out || out_size < MAX_PATH) return false;
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-sprite-probe.csv", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

bool open_csv_for_capture(bool truncate) {
    std::scoped_lock lk(g_io_mu);
    if (g_csv) return true;
    char path[MAX_PATH] = {0};
    if (!resolve_csv_path(path, sizeof(path))) return false;
    g_csv = std::fopen(path, truncate ? "w" : "a");
    if (!g_csv) {
        mtr::log::info("sprite_probe: fopen(%s) failed", path);
        return false;
    }
    if (truncate) {
        std::fprintf(g_csv,
            "frame,entry_idx,top_screen,stack_depth,"
            "state_key,sort_key,flags,alpha_mod,blend_mode,"
            "p0_x,p0_y,p0_z,p2_x,p2_y,p2_z,"
            "ext_pos,ext_uv,ext_col\n");
        std::fflush(g_csv);
    }
    mtr::log::info("sprite_probe: csv opened %s (truncate=%d)", path, truncate ? 1 : 0);
    return true;
}

void close_csv() {
    std::scoped_lock lk(g_io_mu);
    if (g_csv) {
        std::fclose(g_csv);
        g_csv = nullptr;
        mtr::log::info("sprite_probe: csv closed");
    }
}

void capture_frame(uint64_t frame_no) {
    SpriteEntry* head = *reinterpret_cast<SpriteEntry**>(kSpriteListHeadVA);
    if (!head) {
        g_last_frame_entry_count.store(0);
        return;
    }
    char top[64] = {0};
    mtr::screen_push::current_top_name(top, sizeof(top));
    const int stack_d = mtr::screen_push::stack_depth();
    for (char* p = top; *p; ++p) if (*p == ',') *p = '_';

    int entry_idx = 0;
    uint64_t frame_count = 0;
    {
        std::scoped_lock lk(g_io_mu);
        if (!g_csv) return;
        for (SpriteEntry* e = head; e; e = e->next, ++entry_idx) {
            const float* pos = (e->flags & 0x1) ? e->ext_positions : e->inline_positions;
            if (!pos) continue;
            const float p0x = pos[0], p0y = pos[1], p0z = pos[2];
            const float p2x = pos[6], p2y = pos[7], p2z = pos[8];
            std::fprintf(g_csv,
                "%llu,%d,%s,%d,"
                "0x%08X,%u,0x%04X,%u,%u,"
                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                "%d,%d,%d\n",
                static_cast<unsigned long long>(frame_no), entry_idx, top, stack_d,
                e->state_key, static_cast<unsigned>(e->sort_key),
                static_cast<unsigned>(e->flags),
                static_cast<unsigned>(e->alpha_mod), static_cast<unsigned>(e->blend_mode),
                p0x, p0y, p0z, p2x, p2y, p2z,
                (e->flags & 0x1) ? 1 : 0,
                (e->flags & 0x2) ? 1 : 0,
                (e->flags & 0x4) ? 1 : 0);
            ++frame_count;
        }
        std::fflush(g_csv);
    }
    g_last_frame_entry_count.store(frame_count);
    g_total_entries_captured.fetch_add(frame_count);
}

// The wrapper that replaces render_sprite_batcher at the call site.
// Capture path runs first (only when armed), then forwards to the real
// function via its known VA. No trampoline — the original function bytes
// are untouched.
unsigned int __cdecl wrapper_render_sprite_batcher() {
    // Probe runs first — captures the original list state before any
    // split / per-key mutations. If probe is armed alongside other
    // modes, the CSV reflects the pre-mutation entries.
    if (g_armed.load()) {
        const uint64_t fno = g_frame_counter.fetch_add(1);
        capture_frame(fno);
        int remaining = g_frames_remaining.fetch_sub(1) - 1;
        if (remaining <= 0) {
            g_armed.store(false);
            close_csv();
            mtr::log::info("sprite_probe: capture budget exhausted; disarmed");
        }
    }
    // Per-state_key live tracker + transform applier. Always runs (so the
    // menu's live key list updates even when no transforms are set);
    // pure tally pass when no transforms are active.
    mtr::sprite_xform::process_list();

    // Phase 3 split-pass (experimental): if enabled, classify entries
    // and render in two passes. Otherwise just forward.
    if (mtr::sprite_split::enabled()) {
        return mtr::sprite_split::execute_split();
    }
    return reinterpret_cast<PFN_RenderSpriteBatcher>(kRenderSpriteBatcherVA)();
}

bool install_call_site_patch() {
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kCallSiteVA), 5,
                        PAGE_EXECUTE_READWRITE, &old_protect)) {
        mtr::log::info("sprite_probe: VirtualProtect(rwx) failed (gle=%lu)", GetLastError());
        return false;
    }
    // Save original 5 bytes for sanity check + future restore.
    std::memcpy(g_orig_call_bytes, reinterpret_cast<void*>(kCallSiteVA), 5);
    // Verify it's the call we expect (E8 6C 69 01 00 -> render_sprite_batcher).
    // If the bytes drifted (different build, post-unpack mismatch), refuse.
    if (g_orig_call_bytes[0] != 0xE8) {
        mtr::log::info("sprite_probe: unexpected byte at call site: 0x%02X (wanted 0xE8)",
                       g_orig_call_bytes[0]);
        VirtualProtect(reinterpret_cast<void*>(kCallSiteVA), 5, old_protect, &old_protect);
        return false;
    }
    // Compute new relative offset: target - (call_addr + 5).
    const uintptr_t target = reinterpret_cast<uintptr_t>(&wrapper_render_sprite_batcher);
    const int32_t  rel    = static_cast<int32_t>(
        target - (kCallSiteVA + 5));
    uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
    std::memcpy(patch + 1, &rel, sizeof(rel));
    std::memcpy(reinterpret_cast<void*>(kCallSiteVA), patch, 5);

    DWORD restore_protect = 0;
    VirtualProtect(reinterpret_cast<void*>(kCallSiteVA), 5, old_protect, &restore_protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kCallSiteVA), 5);
    mtr::log::info("sprite_probe: call-site patched at %p (orig=%02X %02X %02X %02X %02X, "
                   "wrapper=%p, rel=0x%08X)",
                   (void*)kCallSiteVA,
                   g_orig_call_bytes[0], g_orig_call_bytes[1], g_orig_call_bytes[2],
                   g_orig_call_bytes[3], g_orig_call_bytes[4],
                   (void*)target, static_cast<unsigned>(rel));
    return true;
}

} // namespace

bool install() {
    if (g_installed.exchange(true)) return true;
    if (!install_call_site_patch()) {
        g_installed.store(false);
        return false;
    }
    return true;
}

bool installed()           { return g_installed.load(); }
bool armed()               { return g_armed.load(); }
int  frames_remaining()    { return g_frames_remaining.load(); }
uint64_t total_captured()  { return g_total_entries_captured.load(); }
uint64_t last_frame_count(){ return g_last_frame_entry_count.load(); }

void arm(int frame_budget) {
    if (frame_budget <= 0) return;
    if (!g_installed.load()) {
        mtr::log::info("sprite_probe: arm() called before install()");
        return;
    }
    if (!open_csv_for_capture(/*truncate=*/true)) return;
    g_frames_remaining.store(frame_budget);
    g_frame_counter.store(0);
    g_total_entries_captured.store(0);
    g_armed.store(true);
    mtr::log::info("sprite_probe: armed for %d frames", frame_budget);
}

void disarm() {
    g_armed.store(false);
    g_frames_remaining.store(0);
    close_csv();
    mtr::log::info("sprite_probe: disarmed");
}

bool csv_path(char* out, size_t out_size) {
    return resolve_csv_path(out, out_size);
}

} // namespace mtr::sprite_probe
