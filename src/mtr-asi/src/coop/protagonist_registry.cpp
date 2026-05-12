// Protagonist instance registry — see mtr/protagonist_registry.h.

#include "mtr/protagonist_registry.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::protagonist_registry {

namespace {

// ============================================================================
// Engine VAs
// ============================================================================
//
// We hook the TRUE wilbur class factory at 0x48E030 (POST-return). This is the
// function entity_factory_construct dispatches to via class_entry[+4] when the
// bag's class field is "wilbur" (the wilbur class entry lives at 0x6B8614 in
// .rdata with name "wilbur" inline at +8, vtable[0]=0x48D5E0, vtable[1]=this).
//
// We previously tried 0x48D8A0 — also an mtr_alloc(3404) wrapper, but a
// different code path (possibly minigame variant / alternate init). It never
// fires during normal gameplay. The protagonist class allocator at 0x5B71C0
// (3,276-byte instances) is for a different class not used in normal gameplay
// either.

constexpr uintptr_t kWilburFactoryVA = 0x0048E030;

// ============================================================================
// State
// ============================================================================

constexpr int kMaxInstances = 2;   // 2P coop cap

std::mutex            g_mutex;
bool                  g_installed                  = false;
uint32_t              g_instance[kMaxInstances]    = {0};
std::atomic<uint64_t> g_total_registrations{0};
std::atomic<uint64_t> g_total_unregistrations{0};
bool                  g_log_enabled                = false;

// ============================================================================
// Cmdline scan
// ============================================================================

bool has_cmdline_flag(const char* flag_name) {
    LPSTR line = GetCommandLineA();
    if (!line || !flag_name) return false;
    char needle[64];
    int n = std::snprintf(needle, sizeof(needle), "-%s", flag_name);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
    const char* p = line;
    while ((p = std::strstr(p, needle)) != nullptr) {
        const char after = p[n];
        const bool prev_ok = (p == line) || p[-1] == ' ' || p[-1] == '\t';
        const bool after_ok = (after == 0 || after == ' ' || after == '\t' ||
                               after == '=' || after == ':');
        if (prev_ok && after_ok) return true;
        ++p;
    }
    return false;
}

// ============================================================================
// Hook trampolines + thunks
// ============================================================================
//
// wilbur_class_factory_alloc_ctor @ 0x48E030: dispatched by
// entity_factory_construct as `class_entry->vtable[+4](class_entry, bag)` —
// __thiscall (class_entry in ECX, bag on stack). IDA labels it __stdcall(int)
// because it doesn't read ECX as `this` in the body, but the caller invokes it
// per __thiscall ABI.
//
// MSVC __fastcall mapping for __thiscall: arg0 in ECX, edx_dummy, then stack
// args. Declare the trampoline + hook as __fastcall(this_, edx_dummy, bag) to
// match the original ABI exactly so MinHook's stack discipline is preserved.

using PFN_WilburAlloc = uint32_t (__fastcall*)(uint32_t this_,
                                                uint32_t edx_dummy,
                                                uint32_t bag);

PFN_WilburAlloc g_tramp_wilbur = nullptr;

uint32_t __fastcall hk_wilbur_alloc(uint32_t this_, uint32_t edx_dummy,
                                    uint32_t bag) {
    uint32_t result = g_tramp_wilbur(this_, edx_dummy, bag);
    if (result != 0) {
        register_instance(result);
    }
    return result;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

void register_instance(uint32_t this_ptr) {
    if (this_ptr == 0) return;
    int assigned_idx = -1;
    bool already_present = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (int i = 0; i < kMaxInstances; ++i) {
            if (g_instance[i] == this_ptr) {
                already_present = true;
                assigned_idx = i;
                break;
            }
        }
        if (!already_present) {
            for (int i = 0; i < kMaxInstances; ++i) {
                if (g_instance[i] == 0) {
                    g_instance[i] = this_ptr;
                    assigned_idx = i;
                    g_total_registrations.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    if (!already_present && assigned_idx >= 0 && g_log_enabled) {
        mtr::log::info("[protag_registry] REGISTER this=0x%08X -> player_idx=%d",
                       this_ptr, assigned_idx);
    } else if (!already_present && assigned_idx < 0 && g_log_enabled) {
        mtr::log::info("[protag_registry] REGISTER OVERFLOW this=0x%08X "
                       "(already %d slots full)",
                       this_ptr, kMaxInstances);
    }
}

void unregister_instance(uint32_t this_ptr) {
    if (this_ptr == 0) return;
    int cleared_idx = -1;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (int i = 0; i < kMaxInstances; ++i) {
            if (g_instance[i] == this_ptr) {
                g_instance[i] = 0;
                cleared_idx = i;
                g_total_unregistrations.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    if (cleared_idx >= 0 && g_log_enabled) {
        mtr::log::info("[protag_registry] UNREGISTER this=0x%08X (was player_idx=%d)",
                       this_ptr, cleared_idx);
    }
}

int player_idx_for(uint32_t this_ptr) {
    if (this_ptr == 0) return -1;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < kMaxInstances; ++i) {
        if (g_instance[i] == this_ptr) return i;
    }
    return -1;
}

Observation observation() {
    Observation out;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < kMaxInstances; ++i) {
        out.instance[i] = g_instance[i];
        if (g_instance[i] != 0) ++out.count;
    }
    out.total_registrations   = g_total_registrations.load();
    out.total_unregistrations = g_total_unregistrations.load();
    return out;
}

bool install() {
    if (g_installed) return true;

    g_log_enabled = has_cmdline_flag("mtrasi-protag-registry-log");

    if (MH_CreateHook(reinterpret_cast<LPVOID>(kWilburFactoryVA),
                      reinterpret_cast<LPVOID>(&hk_wilbur_alloc),
                      reinterpret_cast<LPVOID*>(&g_tramp_wilbur)) != MH_OK) {
        mtr::log::info("[protag_registry] MH_CreateHook(wilbur_factory) FAILED");
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<LPVOID>(kWilburFactoryVA)) != MH_OK) {
        mtr::log::info("[protag_registry] MH_EnableHook(wilbur_factory) FAILED");
        return false;
    }

    g_installed = true;
    mtr::log::info("[protag_registry] installed (wilbur_factory=0x%08X log=%d)",
                   static_cast<uint32_t>(kWilburFactoryVA),
                   g_log_enabled ? 1 : 0);
    return true;
}

} // namespace mtr::protagonist_registry
