// Phase 0.5 RE: locate widget m_pcName offset on Sprite/Text/Group widget
// objects.
//
// HOOK: MinHook detour on sub_4E9350 (engine SubmitSprite, central choke
// point for sprite-batcher submissions; per Phase 0 RE in
// research/findings/ui-widget-system-phase0-2026-05-09.md).
//
// CAPTURE: at hook entry, walk the caller's stack frame (32 dwords above
// the saved return-address slot). For each pointer-looking dword, treat
// it as a candidate widget object and scan the first 0x100 bytes for ASCII
// strings matching widget identifier patterns ("IDS_*", "IDT_*", "IDG_*",
// "IDC_*", etc). Both inline char[] fields and char* fields (single-deref)
// are tested.
//
// LOG: one-shot. Disarms after `budget` findings. Output:
//   Game/mtr-asi-widget-probe.log
//
// SAFETY: VirtualQuery-based bounds check on every read. No SEH (avoids
// __try / C++ unwind interaction).

#include "mtr/widget_probe.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace mtr        { HMODULE self_module(); }
namespace mtr::log   { void info(const char* fmt, ...); }
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
}

namespace mtr::widget_probe {

namespace {

constexpr uintptr_t kSubmitSpriteVA = 0x004E9350;
constexpr uintptr_t kSpriteCtorVA   = 0x00490830;
constexpr int       kDefaultBudget  = 500;
// m_pcName lives at this offset on widget objects (Sprite, Text, Button,
// FrontEnd_*) — confirmed at runtime via Phase 0.5 widget_probe scan.
constexpr int       kWidgetNameOffset = 0x130;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_armed{false};
std::atomic<int>  g_remaining{0};
std::atomic<int>  g_findings{0};
std::mutex        g_io_mu;
FILE*             g_log = nullptr;

// === Phase 0B caller-PC audit ============================================
// When armed, every sub_4E9350 PRE→POST pair logs the caller's return
// address paired with the SpriteEntry's state_key + sort_key. The Render
// method that submits a given widget's SpriteEntry is the caller; the
// caller_PC therefore identifies which Render method (and thus which
// widget class) renders the gradient. Dedup keeps the log short — same
// (caller_PC, state_key) pair is only logged once per arm.
std::atomic<bool> g_caller_audit_armed{false};
std::atomic<int>  g_caller_audit_remaining{0};
constexpr size_t  kCallerAuditDedupCap = 512;
struct CallerAuditEntry { uint32_t caller_pc; uint32_t state_key; };
CallerAuditEntry  g_caller_audit_dedup[kCallerAuditDedupCap]{};
std::atomic<int>  g_caller_audit_dedup_count{0};
// Pending caller_PC stashed by PRE for POST to consume. Single-slot —
// rendering is single-threaded; same lifetime contract as
// g_pending_widget_name above.
uint32_t          g_pending_caller_pc = 0;
// Also stash ESI / EDI / EBX / EBP at PRE so POST can sample candidate
// widget-name offsets on whichever register is actually the widget
// `this`. Per the 0x60D000 Render decompile, ESI is the strongest
// candidate; we record all four in case a different caller uses a
// different register.
uint32_t          g_pending_esi = 0;
uint32_t          g_pending_edi = 0;
uint32_t          g_pending_ebx = 0;
uint32_t          g_pending_ebp = 0;

// Dedup: 32-entry hash set of (ret_addr ^ inner_off ^ str_hash). Keeps the
// log short — same widget hovered for many frames hits the same triple
// every time. Cleared on arm().
struct DedupEntry { uint32_t key; uint32_t pad; };
constexpr size_t  kDedupCap = 1024;
DedupEntry        g_dedup[kDedupCap]{};
std::atomic<int>  g_dedup_count{0};

uint32_t hash_str(const char* s) {
    uint32_t h = 0x811C9DC5u;
    for (; *s; ++s) {
        h ^= static_cast<uint32_t>(static_cast<uint8_t>(*s));
        h *= 0x01000193u;
    }
    return h ? h : 1u;
}

bool dedup_check_or_insert(uint32_t key) {
    // Linear probe in g_dedup. Returns true if NEW (inserted), false if seen.
    int n = g_dedup_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_dedup[i].key == key) return false;
    }
    if (n >= static_cast<int>(kDedupCap)) return false;  // overflow: drop
    g_dedup[n].key = key;
    g_dedup_count.store(n + 1, std::memory_order_release);
    return true;
}

bool resolve_log_path(char* out, size_t out_size) {
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-widget-probe.log", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

bool open_log() {
    std::scoped_lock lk(g_io_mu);
    if (g_log) return true;
    char path[MAX_PATH];
    if (!resolve_log_path(path, sizeof(path))) return false;
    g_log = std::fopen(path, "w");
    if (!g_log) {
        mtr::log::info("widget_probe: fopen(%s) failed", path);
        return false;
    }
    std::fprintf(g_log,
        "# widget_probe: Phase 0.5 RE log (find widget m_pcName offset)\n"
        "# format: <screen> ret=<call_site_addr> stack_off=<bytes_above_ret_slot>"
        " obj=<obj_ptr> <inline_off|char*_off>=+<bytes>"
        " <direct|single_deref> \"<id_string>\"\n");
    std::fflush(g_log);
    return true;
}

void close_log() {
    std::scoped_lock lk(g_io_mu);
    if (g_log) { std::fclose(g_log); g_log = nullptr; }
}

bool is_safe_to_read(const void* p, size_t n) {
    if (!p || n == 0) return false;
    auto v = reinterpret_cast<uintptr_t>(p);
    if (v < 0x00010000) return false;
    if (v + n < v) return false;
    if (v + n > 0x7FFE0000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return v + n <= region_end;
}

// Permissive filter: ASCII strings >= 4 chars, must start with a letter
// or underscore. Matches widget identifiers (IDS_TOP, "Sprite", screen
// names) AND asset paths (GlowBox_Line.dbl, etc.). Filtering by exact
// IDx_ prefix in code missed cases — easier to filter offline.
bool looks_like_meaningful_string(const char* s, size_t len) {
    if (len < 4) return false;
    char c0 = s[0];
    bool starts_alphanum_or_under =
        (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_';
    if (!starts_alphanum_or_under) return false;
    // Reject pure-digit-and-junk: require at least 2 letters in first 4 chars.
    int letters = 0;
    for (size_t i = 0; i < 4 && i < len; ++i) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ++letters;
    }
    return letters >= 2;
}

bool extract_string(const char* p, char* out, size_t out_size, size_t* out_len) {
    constexpr size_t kMaxScan = 56;
    if (out_size < 5) return false;
    size_t lim = (out_size - 1 < kMaxScan) ? out_size - 1 : kMaxScan;
    if (!is_safe_to_read(p, lim)) return false;
    size_t i = 0;
    while (i < lim) {
        char c = p[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
        out[i] = c;
        ++i;
    }
    out[i] = 0;
    if (out_len) *out_len = i;
    return looks_like_meaningful_string(out, i);
}

void emit_finding(const char* top, uint32_t ret_addr, int stack_off,
                  uint32_t obj_addr, int inner_off, bool indirect,
                  const char* str) {
    // Dedup key = (ret_addr, inner_off, str_hash). Same widget hovered for
    // 100 frames produces 100 identical triples — log once, ignore rest.
    uint32_t key = ret_addr ^ (static_cast<uint32_t>(inner_off) << 16) ^ hash_str(str);
    if (!dedup_check_or_insert(key)) return;
    std::scoped_lock lk(g_io_mu);
    if (g_log) {
        std::fprintf(g_log,
            "%s ret=0x%08X stack_off=%+d obj=0x%08X %s=+%d %s \"%s\"\n",
            top, ret_addr, stack_off, obj_addr,
            indirect ? "char*_off" : "inline_off", inner_off,
            indirect ? "single_deref" : "direct",
            str);
        std::fflush(g_log);
    }
    g_findings.fetch_add(1);
    g_remaining.fetch_sub(1);
}

void scan_object_for_id(uint32_t obj_addr, uint32_t ret_addr, int stack_off,
                        const char* top_screen) {
    constexpr int kScanLimit = 0x200;
    if (!is_safe_to_read(reinterpret_cast<void*>(obj_addr), kScanLimit)) return;
    char str[64];
    // Inline string scan with skip-past-found-string to avoid logging
    // shifted views ("...exe", "..exe", ".exe") of the same string.
    int off = 0;
    while (off < kScanLimit) {
        size_t len = 0;
        if (!extract_string(reinterpret_cast<const char*>(obj_addr + off),
                            str, sizeof(str), &len)) {
            off += 4;
            continue;
        }
        emit_finding(top_screen, ret_addr, stack_off, obj_addr, off,
                     /*indirect=*/false, str);
        if (g_remaining.load() <= 0) return;
        // Skip past this string + NUL, aligned up to dword.
        int skip = static_cast<int>((len + 1 + 3) & ~3);
        off += (skip > 4 ? skip : 4);
    }
    // Indirect (char*) scan. Dedupe on the dereffed string by tracking
    // the last logged ptr_val — most callers pack adjacent char* slots,
    // and adjacent slots holding the same string is rare; skip-on-equal
    // is enough.
    uint32_t last_ptr = 0;
    for (int o = 0; o < kScanLimit; o += 4) {
        if (!is_safe_to_read(reinterpret_cast<void*>(obj_addr + o), 4)) continue;
        uint32_t ptr_val = *reinterpret_cast<uint32_t*>(obj_addr + o);
        if (ptr_val < 0x00010000 || ptr_val > 0x7FFE0000) continue;
        if (ptr_val == last_ptr) continue;
        size_t len = 0;
        if (!extract_string(reinterpret_cast<const char*>(ptr_val),
                            str, sizeof(str), &len)) continue;
        last_ptr = ptr_val;
        emit_finding(top_screen, ret_addr, stack_off, obj_addr, o,
                     /*indirect=*/true, str);
        if (g_remaining.load() <= 0) return;
    }
}

// PUSHAD layout (at the moment of `push esp; call dispatcher`):
//   [+0]  EDI   (last pushed by pushad)
//   [+1]  ESI
//   [+2]  EBP
//   [+3]  ESP_before_pushad   (useless to us)
//   [+4]  EBX
//   [+5]  EDX
//   [+6]  ECX
//   [+7]  EAX   (first pushed by pushad)
//   [+8]  saved ret-addr (from the engine's `call sub_4E9350`)
//   [+9..+16]  the 8 cdecl args of sub_4E9350
//   [+17..]    caller's stack frame
struct PushadBlock {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t ret_addr;
    // ret_addr is followed by the 8 cdecl args, then caller's stack frame.
    // Reached by indexing past &ret_addr (see capture_widget_paths).
};

void capture_widget_paths(const PushadBlock* p) {
    if (!g_armed.load(std::memory_order_acquire) ||
        g_remaining.load(std::memory_order_acquire) <= 0) return;

    char top[64] = "?";
    mtr::screen_push::current_top_name(top, sizeof(top));

    // Heap range filter: typical Wilbur runtime allocations live above
    // 0x00800000 (EXE end + early bss) and well below 0x40000000. Excludes
    // module-data spurious matches (Wilbur.exe at 0x400000-~0x7C0000).
    constexpr uint32_t kHeapMin = 0x00800000u;
    constexpr uint32_t kHeapMax = 0x10000000u;

    // Path 1: scan caller's stack frame above the args.
    const uint32_t* ret_slot = &p->ret_addr;
    for (int i = 1; i <= 60 && g_remaining.load() > 0; ++i) {
        if (!is_safe_to_read(ret_slot + i, 4)) break;
        uint32_t cand = ret_slot[i];
        if (cand < kHeapMin || cand > kHeapMax) continue;
        scan_object_for_id(cand, p->ret_addr, i * 4, top);
    }

    // Path 2: scan callee-saved registers (ESI/EDI/EBX/EBP) as additional
    // `this` candidates. Skip caller-saved (EAX/ECX/EDX) — those almost
    // always hold transient junk at the moment of `call sub_4E9350` (a
    // __thiscall caller would have clobbered ECX while computing args).
    // Negative stack_off encodes register identity: -4=EBX, -5=EBP, -6=ESI,
    // -7=EDI.
    struct RegProbe { uint32_t val; int stack_off; };
    const RegProbe regs[] = {
        { p->ebx, -4 },
        { p->ebp, -5 },
        { p->esi, -6 },
        { p->edi, -7 },
    };
    for (const auto& r : regs) {
        if (g_remaining.load() <= 0) break;
        if (r.val < kHeapMin || r.val > kHeapMax) continue;
        scan_object_for_id(r.val, p->ret_addr, r.stack_off, top);
    }

    if (g_remaining.load() <= 0) {
        g_armed.store(false, std::memory_order_release);
        close_log();
        mtr::log::info("widget_probe: budget exhausted; %d findings",
                       g_findings.load());
    }
}

} // namespace

// === Production capture: SpriteEntry* -> widget_name side-table ===========
//
// Always-on, low-overhead. At each sub_4E9350 PRE-call we identify a
// candidate widget via the EBX/EBP/ESI/EDI registers + caller's stack
// frame, read m_pcName at the confirmed offset (+0x130) as a char*, and
// stash it in a thread-local-style "pending" slot. After the trampoline
// returns (giving us the new SpriteEntry*), the POST dispatcher pairs
// the SpriteEntry pointer with the pending name in the side-table.
//
// Side-table is a fixed-size open-address hash. Cleared at frame top by
// process_list (sprite_probe wrapper) via clear_frame_table().

constexpr size_t kSideTableCap = 1024;   // ~ max sprites/frame
struct SideEntry {
    void*       entry_ptr;
    const char* widget_name;   // points into engine memory; lifetime = the widget object
};
SideEntry g_side_table[kSideTableCap]{};
std::atomic<uint32_t> g_side_count{0};

const char* g_pending_widget_name = nullptr;
constexpr int  kProdNameOffset    = 0x130;   // confirmed for Btn/Text widgets

inline bool prod_safe_read_ptr(uint32_t addr) {
    return is_safe_to_read(reinterpret_cast<void*>(addr), 4);
}

// === Widget construction-time name map (Phase 4 / Approach 2) =============
//
// Hook the Sprite class ctor (sub_490830, __thiscall) and capture each
// widget's m_pcName at +0x130 the moment it's set by the base ctor chain.
// Stash the name in a global ptr->name map keyed by the widget object's
// `this` pointer. At sub_4E9350 PRE-time, we look up each candidate
// pointer (registers + stack-frame dwords) directly against the map for
// O(log N) hits — no offset guessing per widget class, no failure mode
// when `this` is in a register we didn't think to check, and no
// dependency on the SubmitSprite caller's prologue layout.
//
// Why this is RULE №1-compliant: we read the widget's name AT THE
// AUTHORITATIVE CREATION SITE (the engine's own ctor), not at submit
// time when the calling-convention residue may have erased the
// pointer. The same approach scales to Text and Group ctors when their
// addresses are known.

// Name-keyed dedup with last-writer-wins on widget_ptr (pre-Phase-1 fix,
// 2026-05-09). The previous pointer-keyed dedup appended a stale entry
// every time the engine reallocated a widget on screen reload, so
// widget_map_lookup returned the FIRST stale match. With name as the key,
// re-construction of the same logical widget refreshes its pointer in
// place and lookups by the live pointer succeed; lookups by the dead
// pointer correctly miss. Names are owned (copied into a fixed buffer)
// so the table is robust to engine-side string lifetime, and the pointer
// returned by widget_map_lookup remains valid for the lifetime of the
// process (entries are never deleted or moved).
struct WidgetMapEntry {
    std::atomic<uint32_t> widget_ptr;   // updated in-place on name collision
    char                  name[64];     // owned copy; NUL-terminated
};

constexpr size_t kWidgetMapCap   = 4096; // grows over session; never cleaned
constexpr size_t kWidgetNameMax  = sizeof(WidgetMapEntry::name);
WidgetMapEntry        g_widget_map[kWidgetMapCap]{};
std::atomic<uint32_t> g_widget_map_count{0};
std::mutex            g_widget_map_mu;

void widget_map_insert(uint32_t widget_ptr, const char* name) {
    if (!widget_ptr || !name || !name[0]) return;
    std::scoped_lock lk(g_widget_map_mu);
    uint32_t n = g_widget_map_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        if (std::strncmp(g_widget_map[i].name, name, kWidgetNameMax) == 0) {
            // Last-writer-wins: refresh the pointer for the same logical
            // widget. Release-store so a concurrent lookup that observes
            // this pointer also observes whatever the producer published
            // before this call.
            g_widget_map[i].widget_ptr.store(widget_ptr,
                                             std::memory_order_release);
            return;
        }
    }
    if (n >= kWidgetMapCap) return;
    std::strncpy(g_widget_map[n].name, name, kWidgetNameMax - 1);
    g_widget_map[n].name[kWidgetNameMax - 1] = '\0';
    g_widget_map[n].widget_ptr.store(widget_ptr, std::memory_order_relaxed);
    g_widget_map_count.store(n + 1, std::memory_order_release);
}

const char* widget_map_lookup(uint32_t widget_ptr) {
    if (!widget_ptr) return nullptr;
    // Bracket with the same mutex used by insert so an in-place
    // widget_ptr update is never half-observed alongside a concurrent
    // append. ~50 ns per call; called ~hundreds/frame at most.
    std::scoped_lock lk(g_widget_map_mu);
    uint32_t n = g_widget_map_count.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        if (g_widget_map[i].widget_ptr.load(std::memory_order_relaxed)
            == widget_ptr) {
            return g_widget_map[i].name;
        }
    }
    return nullptr;
}

} // namespace mtr::widget_probe (close to expose extern-C linkage cleanly)

// Bridge function — defined here at file scope, BUT it calls
// widget_map_insert which lives inside mtr::widget_probe. Since both
// are in the same TU and widget_map_insert has external linkage (it's
// in mtr::widget_probe but NOT in an anonymous namespace), this links.
namespace mtr::widget_probe {
    void widget_map_insert(uint32_t widget_ptr, const char* name);
}
extern "C" void __cdecl sprite_ctor_dispatch_post_impl(uint32_t widget_ptr,
                                                       const char* name) {
    mtr::widget_probe::widget_map_insert(widget_ptr, name);
}

// File-scope POST handler. Pure cdecl, callable from the naked stub's
// inline asm. Self-contained: doesn't depend on namespace-private
// helpers — does its own VirtualQuery range check + string extraction +
// map insert. The map storage is shared with the namespace via a
// trivial extern lookup function (added below).

extern "C" void __cdecl sprite_ctor_dispatch_post_impl(uint32_t widget_ptr,
                                                       const char* name);

extern "C" std::atomic<uint32_t> g_sprite_ctor_hits{0};

extern "C" void __cdecl sprite_ctor_dispatch_post(void* this_ptr) {
    g_sprite_ctor_hits.fetch_add(1, std::memory_order_relaxed);
    if (!this_ptr) return;
    auto p = reinterpret_cast<uintptr_t>(this_ptr);
    constexpr uintptr_t kOff = 0x130;
    auto name_field = p + kOff;
    if (name_field < 0x10000u || name_field > 0x7FFE0000u) return;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(name_field),
                     &mbi, sizeof(mbi)) != sizeof(mbi)) return;
    if (mbi.State != MEM_COMMIT) return;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return;
    auto name_ptr = *reinterpret_cast<uint32_t*>(name_field);
    if (name_ptr < 0x00010000u || name_ptr > 0x7FFE0000u) return;
    if (VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(name_ptr)),
                     &mbi, sizeof(mbi)) != sizeof(mbi)) return;
    if (mbi.State != MEM_COMMIT) return;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return;
    // Validate string: 1+ char, ASCII printable, ≤56 bytes, NUL-terminated.
    auto* s = reinterpret_cast<const char*>(static_cast<uintptr_t>(name_ptr));
    int n = 0;
    for (; n < 56; ++n) {
        char c = s[n];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return;
    }
    if (n < 4 || n >= 56) return;
    sprite_ctor_dispatch_post_impl(static_cast<uint32_t>(p), s);
}

// __fastcall detour for sub_490830 (engine __thiscall). The bridge:
// __thiscall(this, a1, a2, a3) is binary-compatible with
// __fastcall(this[ECX], dummy[EDX], a1[esp+0], a2[esp+4], a3[esp+8])
// — both put `this` in ECX, both have callee-cleanup-args, both have
// the same stack layout for the trailing args. The dummy EDX is the
// caller's clobbered EDX value, ignored by us.
//
// MSVC handles all calling-convention bookkeeping automatically — no
// stack manipulation, no SEH-prologue-disturbing `pop`/`push` of the
// engine's caller_ret slot. The compiler-generated prologue saves
// callee-saved regs cleanly; the epilogue does `ret 0Ch` (matching
// __thiscall's callee-cleanup convention).
using PFN_SpriteCtor = void* (__fastcall*)(void* this_ptr, void* edx_dummy,
                                            int a1, int a2, int a3);
PFN_SpriteCtor g_orig_sprite_ctor = nullptr;

void* __fastcall hk_sprite_ctor(void* this_ptr, void* edx_dummy,
                                int a1, int a2, int a3) {
    void* result = g_orig_sprite_ctor(this_ptr, edx_dummy, a1, a2, a3);
    sprite_ctor_dispatch_post(this_ptr);
    return result;
}

namespace mtr::widget_probe {

const char* prod_try_read_widget_name(uint32_t this_candidate) {
    // Multi-offset m_pcName reader (2026-05-09). The engine's widget
    // hierarchy stores the name char* at one of several offsets depending
    // on class:
    //   +0x130 — Btn_*, FrontEnd_*, Scn_* (the common case)
    //   +0x134 — Wilbur*Background, popup-frame widgets
    //   +0x140 — some script-class widgets
    // Caller-PC audit on 2026-05-09 captured all three patterns. We try
    // each in order; first valid name wins. Cheap — one VirtualQuery per
    // miss, four max per candidate.
    constexpr int kOffsets[] = { 0x130, 0x134, 0x140 };
    for (int off : kOffsets) {
        uint32_t slot = this_candidate + (uint32_t)off;
        if (!prod_safe_read_ptr(slot)) continue;
        uint32_t name_ptr = *reinterpret_cast<uint32_t*>(slot);
        if (name_ptr < 0x00010000u || name_ptr > 0x7FFE0000u) continue;
        char buf[64];
        size_t len = 0;
        if (!extract_string(reinterpret_cast<const char*>(name_ptr),
                            buf, sizeof(buf), &len)) continue;
        if (!looks_like_meaningful_string(buf, len)) continue;
        return reinterpret_cast<const char*>(name_ptr); // engine-lifetime borrowed
    }
    return nullptr;
}

void prod_capture_pre(const PushadBlock* p) {
    g_pending_widget_name = nullptr;
    constexpr uint32_t kHeapMin = 0x00800000u;
    constexpr uint32_t kHeapMax = 0x10000000u;

    // === FAST PATH (Phase 4 / Approach 2): widget ctor map lookup =======
    // For widgets created via the hooked Sprite ctor (sub_490830), we
    // already know their `this` -> name mapping. Just look up each
    // candidate against the map. This is O(N) over the widget map size
    // (typically <200 entries), but the inner loop is a single dword
    // compare so cache-friendly and fast in practice.
    auto try_map = [](uint32_t cand) -> const char* {
        if (cand < 0x00800000u || cand > 0x10000000u) return nullptr;
        return widget_map_lookup(cand);
    };

    const uint32_t reg_cands[4] = { p->ebx, p->ebp, p->esi, p->edi };
    for (uint32_t cand : reg_cands) {
        const char* name = try_map(cand);
        if (name) { g_pending_widget_name = name; return; }
    }
    const uint32_t* ret_slot = &p->ret_addr;
    for (int i = 1; i <= 60; ++i) {
        if (!is_safe_to_read(ret_slot + i, 4)) break;
        uint32_t cand = ret_slot[i];
        const char* name = try_map(cand);
        if (name) { g_pending_widget_name = name; return; }
    }

    // === SLOW PATH (Phase 1): direct *(this+0x130) read =================
    // Fallback when the widget wasn't created through the hooked ctor (or
    // the ctor hook installed AFTER the widget was created). Try
    // callee-saved registers first, then the [esp+40] working offset.
    for (uint32_t cand : reg_cands) {
        if (cand < kHeapMin || cand > kHeapMax) continue;
        const char* name = prod_try_read_widget_name(cand);
        if (name) { g_pending_widget_name = name; return; }
    }
    for (int i = 9; i <= 16; ++i) {
        if (!is_safe_to_read(ret_slot + i, 4)) break;
        uint32_t cand = ret_slot[i];
        if (cand < kHeapMin || cand > kHeapMax) continue;
        const char* name = prod_try_read_widget_name(cand);
        if (name) { g_pending_widget_name = name; return; }
    }
}

void prod_capture_post(void* sprite_entry) {
    if (!g_pending_widget_name || !sprite_entry) return;
    uint32_t n = g_side_count.load(std::memory_order_relaxed);
    if (n >= kSideTableCap) return;
    g_side_table[n].entry_ptr   = sprite_entry;
    g_side_table[n].widget_name = g_pending_widget_name;
    g_side_count.store(n + 1, std::memory_order_release);
    g_pending_widget_name = nullptr;
}

// Per-call counter so we can verify the stub fires; logged via dispatcher
// observable from menu / log if needed. Atomic load+store, very cheap.
std::atomic<uint64_t> g_dispatch_calls{0};

extern "C" void __cdecl widget_probe_dispatch_pre(const void* pushad_block) {
    g_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    auto* p = static_cast<const PushadBlock*>(pushad_block);
    prod_capture_pre(p);
    if (g_armed.load(std::memory_order_acquire)) {
        capture_widget_paths(p);
    }
    // Phase 0B caller-PC audit: stash the caller's return address AND the
    // four pointer-y registers so POST can sample candidate name offsets
    // on whichever register is the widget `this`.
    if (g_caller_audit_armed.load(std::memory_order_acquire)) {
        g_pending_caller_pc = p->ret_addr;
        g_pending_esi = p->esi;
        g_pending_edi = p->edi;
        g_pending_ebx = p->ebx;
        g_pending_ebp = p->ebp;
    } else {
        g_pending_caller_pc = 0;
    }
}

extern "C" void __cdecl widget_probe_dispatch_post(void* /*eax_unused*/) {
    // 2026-05-09: sub_4E9350 returns *(SpriteEntry+0x18), NOT the
    // SpriteEntry pointer — `mov eax, [ebp+18h]` at 0x4E94AE in IDA. EAX
    // is therefore useless as a key. Read the engine's sprite-list head
    // instead: after sub_4E9350 returns, the freshly-allocated entry is
    // at the head of the list, which we can resolve directly.
    constexpr uintptr_t kSpriteListHeadVA = 0x007271E8;
    void* sprite_entry = *reinterpret_cast<void**>(kSpriteListHeadVA);
    prod_capture_post(sprite_entry);
    // Phase 0B caller-PC audit: if armed, log unique (caller_PC, state_key)
    // pairs. SpriteEntry layout: state_key at +0x10, sort_key at +0x16.
    if (g_caller_audit_armed.load(std::memory_order_acquire)
        && g_pending_caller_pc != 0
        && sprite_entry != nullptr) {
        uint32_t caller_pc = g_pending_caller_pc;
        g_pending_caller_pc = 0;
        uint8_t* e = reinterpret_cast<uint8_t*>(sprite_entry);
        if (is_safe_to_read(e + 0x10, 8)) {
            uint32_t state_key = *reinterpret_cast<uint32_t*>(e + 0x10);
            uint16_t sort_key  = *reinterpret_cast<uint16_t*>(e + 0x16);
            // Linear-probe dedup. Cap at kCallerAuditDedupCap; once full,
            // we silently drop further entries (auto-disarm via budget
            // below catches it cleanly).
            int n = g_caller_audit_dedup_count.load(std::memory_order_acquire);
            bool seen = false;
            for (int i = 0; i < n; ++i) {
                if (g_caller_audit_dedup[i].caller_pc == caller_pc
                    && g_caller_audit_dedup[i].state_key == state_key) {
                    seen = true;
                    break;
                }
            }
            if (!seen && n < (int)kCallerAuditDedupCap) {
                g_caller_audit_dedup[n].caller_pc = caller_pc;
                g_caller_audit_dedup[n].state_key = state_key;
                g_caller_audit_dedup_count.store(n + 1,
                                                 std::memory_order_release);
                mtr::log::info("[caller_audit] caller_pc=0x%08X "
                               "state_key=0x%08X sort_key=0x%04X "
                               "entry=%p esi=0x%08X edi=0x%08X "
                               "ebx=0x%08X ebp=0x%08X",
                               caller_pc, state_key, sort_key,
                               sprite_entry,
                               g_pending_esi, g_pending_edi,
                               g_pending_ebx, g_pending_ebp);
                // Sample +0x00..+0x200 at each pointer-y register. Wide
                // range needed because different widget classes store
                // m_pcName at different offsets (+0x130 for Btn/Scn_*,
                // +0x134 for Wilbur*Popup, inline near +0x100 for
                // ProjectItem-style; gradient widget's offset still
                // unknown). If no string found in this range, the
                // widget doesn't store its name on `this` and we'll
                // need a different identity source.
                const uint32_t cands[4] = {
                    g_pending_esi, g_pending_edi,
                    g_pending_ebx, g_pending_ebp
                };
                const char* reg_names[4] = { "esi", "edi", "ebx", "ebp" };
                for (int r = 0; r < 4; ++r) {
                    uint32_t base = cands[r];
                    if (base < 0x00800000u || base > 0x10000000u) continue;
                    for (int off = 0x000; off <= 0x200; off += 4) {
                        uint32_t addr = base + (uint32_t)off;
                        if (!is_safe_to_read(reinterpret_cast<void*>(addr), 4))
                            continue;
                        uint32_t val = *reinterpret_cast<uint32_t*>(addr);
                        // Try as char* (single-deref):
                        if (val >= 0x00010000u && val <= 0x7FFE0000u
                            && is_safe_to_read(reinterpret_cast<void*>(val), 8)) {
                            char buf[64] = {0};
                            size_t len = 0;
                            if (extract_string(
                                    reinterpret_cast<const char*>(val),
                                    buf, sizeof(buf), &len)
                                && looks_like_meaningful_string(buf, len)) {
                                mtr::log::info(
                                    "[caller_audit]   %s+0x%X char*=\"%s\"",
                                    reg_names[r], (unsigned)off, buf);
                                continue;
                            }
                        }
                        // Try as inline char[]:
                        if (is_safe_to_read(reinterpret_cast<void*>(addr), 8)) {
                            char buf[64] = {0};
                            size_t len = 0;
                            if (extract_string(
                                    reinterpret_cast<const char*>(addr),
                                    buf, sizeof(buf), &len)
                                && looks_like_meaningful_string(buf, len)) {
                                mtr::log::info(
                                    "[caller_audit]   %s+0x%X inline=\"%s\"",
                                    reg_names[r], (unsigned)off, buf);
                            }
                        }
                    }
                }
                int rem = g_caller_audit_remaining
                              .fetch_sub(1, std::memory_order_acq_rel);
                if (rem <= 1) {
                    g_caller_audit_armed.store(false,
                                               std::memory_order_release);
                    mtr::log::info("[caller_audit] disarmed: budget exhausted "
                                   "(unique entries=%d)", n + 1);
                }
            }
        }
    }
}

// MinHook gives us a trampoline pointer that calls the original function.
// The naked stub tail-jumps to it after running the capture dispatcher.
// File-scope (NOT inside an unnamed namespace, to keep extern-C linkage clean).
extern "C" void* g_orig_SubmitSprite_trampoline = nullptr;

// Scratch slot used by the naked stub to stash the engine's caller_ret
// across the call into the trampoline (we can't trust any register —
// trampoline = original sub_4E9350 = __cdecl, free to clobber EAX/ECX/EDX).
// NOT thread-safe; rendering happens on a single thread, so OK.
extern "C" void* g_caller_ret_save = nullptr;

// Naked stub: PRE-dispatch (capture widget_name) -> CALL trampoline ->
// POST-dispatch (pair return value SpriteEntry* with pending name).
// Preserves the original cdecl args layout + return value (EAX) so the
// engine's caller can't tell anything happened.
//
// The tricky part is the forward to the trampoline. Original sub_4E9350
// expects [its_ret(esp+0), arg1(esp+4), ...]. But after our entry we
// have [caller_ret(esp+0), arg1(esp+4), ...]. To rearrange, we POP
// caller_ret to a register before the call and PUSH it back after.
extern "C" __declspec(naked) void __cdecl hk_SubmitSprite_naked() {
    __asm {
        ; STACK ON ENTRY: [caller_ret(esp+0), arg1(esp+4), ..., arg8(esp+32)]

        ; --- PRE: capture widget_name (preserves all regs via pushad/popad) -
        pushad
        push    esp
        call    widget_probe_dispatch_pre
        add     esp, 4
        popad

        ; --- Forward to trampoline + post-capture (return value in eax) ---
        ; The MSVC inline-asm `pop reg / call [mem] / push reg` pattern was
        ; crashing — likely because `call` clobbered ECX (cdecl caller-save)
        ; before we pushed it back. Use a static save slot instead, no
        ; register dependency.
        pop     dword ptr [g_caller_ret_save]           ; save engine ret
        call    dword ptr [g_orig_SubmitSprite_trampoline]
        push    dword ptr [g_caller_ret_save]           ; restore engine ret

        ; STACK NOW: [caller_ret, arg1, ..., arg8].  EAX = SpriteEntry*

        ; --- POST: pair (eax, pending_widget_name) -------------------------
        push    eax                                     ; save return value
        push    eax                                     ; pass eax as cdecl arg
        call    widget_probe_dispatch_post
        add     esp, 4
        pop     eax

        ret
    }
}

bool install() {
    if (g_installed.exchange(true)) return true;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(kSubmitSpriteVA),
                                reinterpret_cast<void*>(&hk_SubmitSprite_naked),
                                &g_orig_SubmitSprite_trampoline);
    if (s != MH_OK) {
        mtr::log::info("widget_probe: MH_CreateHook failed (%d)",
                       static_cast<int>(s));
        g_installed.store(false);
        return false;
    }
    s = MH_EnableHook(reinterpret_cast<void*>(kSubmitSpriteVA));
    if (s != MH_OK) {
        mtr::log::info("widget_probe: MH_EnableHook failed (%d)",
                       static_cast<int>(s));
        g_installed.store(false);
        return false;
    }
    mtr::log::info("widget_probe: naked hook installed at 0x%08X (trampoline=%p stub=%p)",
                   kSubmitSpriteVA, g_orig_SubmitSprite_trampoline,
                   reinterpret_cast<void*>(&hk_SubmitSprite_naked));

    // Phase 4 attempt 2 also DISABLED 2026-05-09. Even with __fastcall
    // (which avoids the naked-stub's SEH-disturbance), the user reported
    // the same freeze + half-second menu visibility before harness kill.
    // This means sub_490830 is either being called during a tight inner
    // loop that doesn't tolerate even MinHook's normal trampoline
    // overhead, OR the MinHook trampoline itself is corrupted by the
    // SEH prologue's stack rewrites (push fs:[0]; mov fs:[0],esp).
    // Regardless of the hook mechanism, hooking sub_490830 isn't safe.
    // Need a different upstream point: either the .sc loader (which
    // creates widgets at file-parse time, before runtime is sensitive)
    // or the widget factory sub_4916E0 (one level up from sub_490830).
    return true;
}

unsigned long long dispatch_call_count() {
    return static_cast<unsigned long long>(g_dispatch_calls.load());
}

// === Production side-table API ============================================

const char* widget_name_for_entry(void* entry_ptr) {
    if (!entry_ptr) return nullptr;
    uint32_t n = g_side_count.load(std::memory_order_acquire);
    // Linear scan. Frame caps at ~250 sprites; 250-element scan is faster
    // than maintaining an open-address hash for this size on x86 with
    // string lookups happening once per entry per frame in process_list.
    for (uint32_t i = 0; i < n; ++i) {
        if (g_side_table[i].entry_ptr == entry_ptr) {
            return g_side_table[i].widget_name;
        }
    }
    return nullptr;
}

// Set by the menu's "Dump gradient diag" button. Consumed (and cleared)
// at the very next clear_frame_table() — i.e. AFTER a render frame has
// fully populated the side-table and the sprite-batcher has drawn. Atomic
// so the menu thread (UI poll) and the render thread can't race.
std::atomic<bool> g_dump_next_frame_pending{false};

void request_dump_next_frame() {
    g_dump_next_frame_pending.store(true, std::memory_order_release);
}

void clear_frame_table() {
    if (g_dump_next_frame_pending.exchange(false, std::memory_order_acq_rel)) {
        debug_dump_frame_table();
    }
    g_side_count.store(0, std::memory_order_release);
}

unsigned int frame_table_size() {
    return g_side_count.load(std::memory_order_acquire);
}

// Dump the widget_ptr -> name construction-time map to the main log.
// For verifying the Phase 4 ctor hook is firing.
extern "C" std::atomic<uint32_t> g_sprite_ctor_hits;
void debug_dump_widget_map() {
    uint32_t n = g_widget_map_count.load(std::memory_order_acquire);
    uint32_t hits = g_sprite_ctor_hits.load(std::memory_order_acquire);
    mtr::log::info("widget_probe: ctor hits=%u, map size=%u entries", hits, n);
    int dumped = 0;
    for (uint32_t i = 0; i < n && dumped < 64; ++i) {
        const char* name = g_widget_map[i].name;
        if (!name[0]) continue;
        // Filter to widget-style names so the dump isn't drowned in
        // engine-internal strings.
        bool is_widgetish =
            (name[0] == 'I' && name[1] == 'D') ||
            (name[0] == 'B' && name[1] == 't' && name[2] == 'n') ||
            (std::strstr(name, "Front") != nullptr) ||
            (std::strstr(name, "_") != nullptr && name[0] >= 'A' && name[0] <= 'Z');
        if (!is_widgetish) continue;
        mtr::log::info("  ctor: ptr=0x%08X name=\"%s\"",
                       g_widget_map[i].widget_ptr.load(
                           std::memory_order_relaxed),
                       name);
        ++dumped;
    }
    if (dumped == 0 && n > 0) {
        // Always show first 16 entries when we got nothing widget-shaped,
        // for debugging.
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            const char* name = g_widget_map[i].name;
            mtr::log::info("  ctor: ptr=0x%08X name=\"%s\"",
                           g_widget_map[i].widget_ptr.load(
                               std::memory_order_relaxed),
                           name[0] ? name : "(null)");
        }
    }
}

// Dump the current frame's side-table to the main log (one line per
// distinct widget_name). For verifying Phase 1 capture is working
// without hooking sprite_xform yet.
void debug_dump_frame_table() {
    uint32_t n = g_side_count.load(std::memory_order_acquire);
    mtr::log::info("widget_probe: frame side-table contents (n=%u):", n);
    // Track names we've already printed to dedup the log output.
    const char* seen[64] = {0};
    int n_seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const char* name = g_side_table[i].widget_name;
        bool dup = false;
        for (int j = 0; j < n_seen; ++j) {
            if (seen[j] == name) { dup = true; break; }
        }
        if (dup) continue;
        if (n_seen < 64) seen[n_seen++] = name;
        mtr::log::info("  entry=%p widget_name=\"%s\"",
                       g_side_table[i].entry_ptr, name ? name : "(null)");
    }
}

bool installed()       { return g_installed.load(); }
bool armed()           { return g_armed.load(); }
int  findings_count()  { return g_findings.load(); }

void arm(int budget) {
    if (!g_installed.load()) {
        mtr::log::info("widget_probe: arm() before install()");
        return;
    }
    if (!open_log()) return;
    g_findings.store(0);
    g_remaining.store(budget > 0 ? budget : kDefaultBudget);
    g_dedup_count.store(0);
    std::memset(g_dedup, 0, sizeof(g_dedup));
    g_armed.store(true);
    mtr::log::info("widget_probe: armed for %d findings", g_remaining.load());
}

void disarm() {
    g_armed.store(false);
    g_remaining.store(0);
    close_log();
    mtr::log::info("widget_probe: disarmed (%d findings)", g_findings.load());
}

void caller_audit_arm(int budget) {
    if (!g_installed.load()) {
        mtr::log::info("widget_probe: caller_audit_arm() before install()");
        return;
    }
    int b = budget > 0 ? budget : 256;
    if (b > (int)kCallerAuditDedupCap) b = (int)kCallerAuditDedupCap;
    g_caller_audit_dedup_count.store(0, std::memory_order_release);
    std::memset(g_caller_audit_dedup, 0, sizeof(g_caller_audit_dedup));
    g_pending_caller_pc = 0;
    g_caller_audit_remaining.store(b, std::memory_order_release);
    g_caller_audit_armed.store(true, std::memory_order_release);
    mtr::log::info("[caller_audit] armed: budget=%d unique pairs", b);
}

void caller_audit_disarm() {
    g_caller_audit_armed.store(false, std::memory_order_release);
    mtr::log::info("[caller_audit] disarmed (unique entries=%d)",
                   g_caller_audit_dedup_count.load(std::memory_order_acquire));
}

bool caller_audit_armed() {
    return g_caller_audit_armed.load(std::memory_order_acquire);
}

int caller_audit_count() {
    return g_caller_audit_dedup_count.load(std::memory_order_acquire);
}

} // namespace mtr::widget_probe
