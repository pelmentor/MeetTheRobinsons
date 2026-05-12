// Same-machine dual-launch bypass for Phase 1.4 co-op live-test.
//
// Wilbur.exe (and Launcher.exe before it) creates a Global named mutex
//   "Global\Disney_s_Meet_The_Robinsons"
// via CreateMutexA in WinMain, and exits with "Another instance already
// exists" if GetLastError() == ERROR_ALREADY_EXISTS afterward.
//
// For co-op live-testing we need two Wilbur.exe instances on the same
// machine. Solution: hook kernel32!CreateMutex{A,W}, intercept the call
// with this exact name, and rewrite it to
//   "Global\Disney_s_Meet_The_Robinsons_pid<PID>"
// so each instance owns a distinct kernel object. The singleton check
// then runs against a name no one else claimed, sees no conflict, and the
// game starts.
//
// Why a name rewrite rather than (a) suppressing ERROR_ALREADY_EXISTS,
// (b) NOPping the engine's exit branch, or (c) editing Wilbur.exe on
// disk: only the rewrite removes the actual root cause (kernel-namespace
// contention). The others fight the singleton guard or violate Principle
// 1. RULE No 1 demands the root-cause fix. See:
//   research/findings/coop-dual-launch-bypass-2026-05-12.md.
//
// Why an implicit gate on co-op flags rather than a dedicated
// `-mtrasi-coop-allow-dual-launch` flag: a dedicated flag is RULE No 2
// baggage. The only reason dual-launch is needed is when co-op is on, so
// `-mtrasi-coop-host` or `-mtrasi-coop-connect` *is* the activation
// signal. One flag, one semantic.
//
// Why hook both A and W: Launcher.exe is confirmed CreateMutexA
// (launcher-internals.md). Wilbur.exe's encoding is not yet RE'd. Hooking
// both is defensive and costs ~one extra detour; each hook still
// rename-on-match-only, so Principle 4 (targeted) is preserved.
//
// Principle 7: this is OS-API hooking, not engine-wrapping. It happens to
// live under coop/ because the feature is co-op-only — no engine VAs are
// dereferenced and no gameplay state is touched.

#include <windows.h>
#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "mtr/cmdline_utils.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop::dual_launch {

namespace {

// The exact singleton-guard mutex name. Match must be string-exact;
// any other named mutex passes through untouched. Both the A and W
// forms below are the on-the-wire representation (single backslash).
constexpr const char*    kDisneyMutexNameA = "Global\\Disney_s_Meet_The_Robinsons";
constexpr const wchar_t* kDisneyMutexNameW = L"Global\\Disney_s_Meet_The_Robinsons";

using CreateMutexA_fn = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
using CreateMutexW_fn = HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
CreateMutexA_fn g_orig_CreateMutexA = nullptr;
CreateMutexW_fn g_orig_CreateMutexW = nullptr;

// Magic-static cache: cmdline is process-lifetime constant.
bool bypass_enabled() {
    static const bool s_enabled = []{
        LPSTR cl = GetCommandLineA();
        if (!cl) return false;
        return mtr::cmdline_utils::has_flag(cl, "-mtrasi-coop-host") ||
               mtr::cmdline_utils::has_flag(cl, "-mtrasi-coop-connect");
    }();
    return s_enabled;
}

HANDLE WINAPI hk_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL bInitialOwner, LPCSTR lpName) {
    if (lpName != nullptr && std::strcmp(lpName, kDisneyMutexNameA) == 0) {
        char rewritten[160];
        const int n = std::snprintf(
            rewritten, sizeof(rewritten),
            "%s_pid%lu", kDisneyMutexNameA, GetCurrentProcessId());
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(rewritten)) {
            mtr::log::info("dual_launch: rewrote CreateMutexA(\"%s\") -> \"%s\"",
                           lpName, rewritten);
            return g_orig_CreateMutexA(sa, bInitialOwner, rewritten);
        }
        // Defensive fall-through: if snprintf truncated (unreachable —
        // the prefix is 34 chars and PID is at most 10 digits), behave as
        // if we weren't installed rather than silently corrupting the name.
        mtr::log::info("dual_launch: snprintf truncation (impossible), falling through");
    }
    return g_orig_CreateMutexA(sa, bInitialOwner, lpName);
}

HANDLE WINAPI hk_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL bInitialOwner, LPCWSTR lpName) {
    if (lpName != nullptr && std::wcscmp(lpName, kDisneyMutexNameW) == 0) {
        wchar_t rewritten[160];
        const int n = std::swprintf(
            rewritten, sizeof(rewritten) / sizeof(wchar_t),
            L"%ls_pid%lu", kDisneyMutexNameW, GetCurrentProcessId());
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(rewritten) / sizeof(wchar_t)) {
            mtr::log::info("dual_launch: rewrote CreateMutexW(\"Global\\Disney_...\") -> \"..._pid%lu\"",
                           GetCurrentProcessId());
            return g_orig_CreateMutexW(sa, bInitialOwner, rewritten);
        }
        mtr::log::info("dual_launch: swprintf truncation (impossible), falling through");
    }
    return g_orig_CreateMutexW(sa, bInitialOwner, lpName);
}

} // anon

void install() {
    if (!bypass_enabled()) {
        // Normal launch: singleton enforcement stays in place. No hook
        // installed, zero overhead on any CreateMutex call.
        return;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) {
        mtr::log::info("dual_launch: GetModuleHandle(kernel32) returned null");
        return;
    }
    void* pA = reinterpret_cast<void*>(GetProcAddress(k32, "CreateMutexA"));
    void* pW = reinterpret_cast<void*>(GetProcAddress(k32, "CreateMutexW"));
    if (!pA || !pW) {
        mtr::log::info("dual_launch: GetProcAddress failed (A=%p W=%p)", pA, pW);
        return;
    }

    bool ok = true;
    if (MH_CreateHook(pA, &hk_CreateMutexA,
                      reinterpret_cast<void**>(&g_orig_CreateMutexA)) != MH_OK) {
        mtr::log::info("dual_launch: MH_CreateHook(CreateMutexA) failed");
        ok = false;
    }
    if (MH_CreateHook(pW, &hk_CreateMutexW,
                      reinterpret_cast<void**>(&g_orig_CreateMutexW)) != MH_OK) {
        mtr::log::info("dual_launch: MH_CreateHook(CreateMutexW) failed");
        ok = false;
    }
    if (!ok) return;

    // Enable both hooks individually so a partial failure is observable and
    // doesn't leave one detour armed with no matching pair. If either fails
    // after the other succeeded, disable the one that came up — RULE No 1:
    // root-cause cleanup, not "best-effort leave-it-armed".
    const MH_STATUS sa = MH_EnableHook(pA);
    const MH_STATUS sw = MH_EnableHook(pW);
    if (sa != MH_OK || sw != MH_OK) {
        if (sa == MH_OK) MH_DisableHook(pA);
        if (sw == MH_OK) MH_DisableHook(pW);
        mtr::log::info("dual_launch: MH_EnableHook failed (A=%d W=%d) -- bypass NOT armed",
                       static_cast<int>(sa), static_cast<int>(sw));
        return;
    }
    mtr::log::info("dual_launch: CreateMutex{A,W} hooks armed; will rename "
                   "\"%s\" -> \"%s_pid%lu\" for THIS process",
                   kDisneyMutexNameA, kDisneyMutexNameA, GetCurrentProcessId());
}

} // namespace mtr::coop::dual_launch
