// Per-object visibility-test diagnostic probe.
//
// `sub_4E0B90` (the per-object visibility test) is a SecuROM-protected
// 1-instruction thunk: `jmp dword ptr [byte_F5F876+0x340BE]` = `jmp [F92F34]`.
// SecuROM resolves the IAT slot at runtime to point at the real impl in a
// decrypted segment.
//
// MinHook'ing the thunk itself was previously unstable (1-byte instruction →
// fragile trampoline → save-load crashes). Multi-call-site patching (force_vis)
// is robust but coarse: it eliminates the test entirely, no diagnostics.
//
// This module installs an *IAT-slot patch* at 0xF92F34: read the real impl
// VA, replace the slot with our wrapper, and route all 4 thunk callers through
// us. No MinHook trampoline (we don't touch the thunk or the impl), just a
// single 4-byte write to the indirect jump table — the same mechanism
// classical "IAT hooks" use for DLL imports.
//
// The wrapper:
//   - increments per-call-site counters (4 known sites + 1 catch-all)
//   - optionally short-circuits to return 1 (force-pass) for ALL callers
//   - forwards to the real impl with __cdecl convention (matching the
//     caller-cleanup `add esp, 18h` observed at every call site)
//
// Use case: if `force_vis` (the call-site rewrite) seems dead, this probe
// answers "is vis_test even being called?", "how many objects fail?", and
// "from which site?". Force-pass is functionally identical to force_vis but
// applied centrally — useful for A/B comparison.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::vis_test_probe {

namespace {

// IAT slot the SecuROM thunk reads. Address verified via:
//   sub_4E0B90: `jmp dword ptr byte_F5F876+340BEh`
//   F5F876 + 340BE = F92F34
constexpr uintptr_t kIatSlotVA = 0xF92F34;

// Known call sites of sub_4E0B90 (from IDA xrefs). Used to bucket counters
// by site via _ReturnAddress() (= byte AFTER the 5-byte CALL instruction).
struct Site {
    uintptr_t call_va;       // VA of the CALL instruction
    uintptr_t return_va;     // call_va + 5 (instruction after CALL)
    const char* tag;
};
constexpr Site kSites[] = {
    {0x004BC406, 0x004BC406 + 5, "sub_4BC340 (scene-tree list update)"},
    {0x004C385D, 0x004C385D + 5, "sub_4C3790 (main render loop)"},
    {0x004CBAC7, 0x004CBAC7 + 5, "sub_???? @ 4CBAC7 (orphan, real call)"},
    {0x004E6A5A, 0x004E6A5A + 5, "sub_4E6A20 (reflection probe)"},
};
constexpr size_t kNumSites = sizeof(kSites) / sizeof(kSites[0]);
constexpr size_t kCatchAllBucket = kNumSites; // index 4 = unknown caller

// vis_test signature: __cdecl with 6-7 args. Every call site cleans up via
// `add esp, 18h` (= 6 args = 24 bytes), so caller-cleanup is confirmed →
// __cdecl. We forward 7 args to be safe (caller-cleanup means extra args
// at the tail are harmless garbage — the impl reads only the first N).
//
// IDA decompiles report the impl returns a struct pointer; callers use only
// AL ("is visible" flag). We pass the full int through.
using PFN_VisTest = int (__cdecl*)(void*, void*, void*, void*, void*, void*, void*);

PFN_VisTest g_real_impl = nullptr;
std::atomic<bool> g_installed{false};

std::atomic<bool> g_force_pass{false};

// Per-frame counters. Read-and-reset by `frame_tick()`.
std::atomic<uint64_t> g_frame_total{0};
std::atomic<uint64_t> g_frame_pass{0};
std::atomic<uint64_t> g_frame_per_site[kNumSites + 1]{};

// Cumulative since install. For sanity check.
std::atomic<uint64_t> g_cum_total{0};
std::atomic<uint64_t> g_cum_pass{0};

// Snapshots that the UI reads (last completed frame's counts).
std::atomic<uint64_t> g_last_frame_total{0};
std::atomic<uint64_t> g_last_frame_pass{0};
std::atomic<uint64_t> g_last_frame_per_site[kNumSites + 1]{};

int __cdecl wrapper(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    // Identify caller by return address (= site_va + 5).
    void* ret_addr = _ReturnAddress();
    size_t bucket = kCatchAllBucket;
    for (size_t i = 0; i < kNumSites; ++i) {
        if (ret_addr == reinterpret_cast<void*>(kSites[i].return_va)) {
            bucket = i;
            break;
        }
    }
    g_frame_per_site[bucket].fetch_add(1, std::memory_order_relaxed);
    g_frame_total.fetch_add(1, std::memory_order_relaxed);
    g_cum_total.fetch_add(1, std::memory_order_relaxed);

    int result;
    if (g_force_pass.load(std::memory_order_relaxed)) {
        result = 1;
    } else {
        result = g_real_impl ? g_real_impl(a, b, c, d, e, f, g) : 1;
    }
    if (result) {
        g_frame_pass.fetch_add(1, std::memory_order_relaxed);
        g_cum_pass.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

} // namespace

// Single-shot patch attempt. Returns true if the slot now points to our
// wrapper, false otherwise.
static bool try_patch_once() {
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(kIatSlotVA);

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_READWRITE, &old_prot)) {
        return false;
    }

    uintptr_t real = *slot;
    if (real == 0 || real < 0x400000 || real > 0x10000000 ||
        real == reinterpret_cast<uintptr_t>(&wrapper))
    {
        DWORD tmp = 0;
        VirtualProtect(slot, sizeof(uintptr_t), old_prot, &tmp);
        return false;
    }

    g_real_impl = reinterpret_cast<PFN_VisTest>(real);
    *slot = reinterpret_cast<uintptr_t>(&wrapper);

    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(uintptr_t), old_prot, &tmp);

    mtr::log::info("vis_test_probe: armed (slot 0x%p was 0x%p -> wrapper %p)",
                   reinterpret_cast<void*>(kIatSlotVA),
                   reinterpret_cast<void*>(real),
                   &wrapper);
    return true;
}

static DWORD WINAPI install_poller(LPVOID) {
    // Poll the IAT slot until SecuROM has resolved it (post-unpack). Most
    // hooks already install correctly after d3d9.dll loads, but the slot at
    // 0xF92F34 lives in a SecuROM-managed segment and we can't be sure of
    // the exact moment it's ready. Poll generously: up to ~30s, every 100ms.
    for (int i = 0; i < 300; ++i) {
        if (try_patch_once()) return 0;
        Sleep(100);
    }
    mtr::log::info("vis_test_probe: gave up after 30s — slot at 0x%p never resolved",
                   reinterpret_cast<void*>(kIatSlotVA));
    g_installed.store(false);
    return 0;
}

bool install() {
    if (g_installed.exchange(true)) {
        mtr::log::info("vis_test_probe: already installed");
        return true;
    }
    HANDLE t = CreateThread(nullptr, 0, install_poller, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return true;
}

bool active()           { return g_installed.load() && g_real_impl != nullptr; }
bool force_pass()       { return g_force_pass.load(); }
void set_force_pass(bool v) {
    g_force_pass.store(v);
    mtr::log::info("vis_test_probe: force_pass = %d", v ? 1 : 0);
}

uint64_t cum_total() { return g_cum_total.load(); }
uint64_t cum_pass()  { return g_cum_pass.load(); }

uint64_t last_frame_total() { return g_last_frame_total.load(); }
uint64_t last_frame_pass()  { return g_last_frame_pass.load(); }
uint64_t last_frame_site(size_t i) {
    if (i > kNumSites) return 0;
    return g_last_frame_per_site[i].load();
}

const char* site_tag(size_t i) {
    if (i < kNumSites) return kSites[i].tag;
    if (i == kCatchAllBucket) return "(unknown caller — return-addr unmatched)";
    return "(out of range)";
}
size_t num_sites() { return kNumSites + 1; } // 4 known + catch-all

// Call this once per frame (e.g. from EndScene hook) to atomically swap
// the running counters into "last frame" snapshots.
void frame_tick() {
    g_last_frame_total.store(g_frame_total.exchange(0));
    g_last_frame_pass.store(g_frame_pass.exchange(0));
    for (size_t i = 0; i <= kNumSites; ++i) {
        g_last_frame_per_site[i].store(g_frame_per_site[i].exchange(0));
    }
}

} // namespace mtr::vis_test_probe
