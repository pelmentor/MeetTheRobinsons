#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdlib.h>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::cmdline {

namespace {

using PFN_GetCmdA = LPSTR  (WINAPI*)();
using PFN_GetCmdW = LPWSTR (WINAPI*)();

PFN_GetCmdA g_orig_A = nullptr;
PFN_GetCmdW g_orig_W = nullptr;

char  g_modified_a[2048] = {};
WCHAR g_modified_w[2048] = {};
bool  g_built_a = false;
bool  g_built_w = false;

void native_dims(int& w, int& h) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoA(mon, &mi)) {
        w = mi.rcMonitor.right  - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
}

// Locate "-dxresolution=WxH" in `line`. On success, sets val_start to the first
// digit of W, val_len to the byte length of "WxH", and out_W/out_H to parsed dims.
bool find_dxres_a(const char* line, const char*& val_start, int& out_W, int& out_H, int& val_len) {
    const char* needle = "-dxresolution=";
    const char* p = strstr(line, needle);
    if (!p) return false;
    const char* val = p + strlen(needle);
    int W = 0, H = 0;
    const char* q = val;
    while (*q >= '0' && *q <= '9') { W = W * 10 + (*q - '0'); ++q; }
    if (*q != 'x' && *q != 'X') return false;
    ++q;
    while (*q >= '0' && *q <= '9') { H = H * 10 + (*q - '0'); ++q; }
    if (W <= 0 || H <= 0) return false;
    val_start = val;
    val_len   = static_cast<int>(q - val);
    out_W = W; out_H = H;
    return true;
}

bool find_dxres_w(const wchar_t* line, const wchar_t*& val_start, int& out_W, int& out_H, int& val_len) {
    const wchar_t* needle = L"-dxresolution=";
    const wchar_t* p = wcsstr(line, needle);
    if (!p) return false;
    const wchar_t* val = p + wcslen(needle);
    int W = 0, H = 0;
    const wchar_t* q = val;
    while (*q >= L'0' && *q <= L'9') { W = W * 10 + (*q - L'0'); ++q; }
    if (*q != L'x' && *q != L'X') return false;
    ++q;
    while (*q >= L'0' && *q <= L'9') { H = H * 10 + (*q - L'0'); ++q; }
    if (W <= 0 || H <= 0) return false;
    val_start = val;
    val_len   = static_cast<int>(q - val);
    out_W = W; out_H = H;
    return true;
}

LPSTR WINAPI hk_GetCommandLineA() {
    if (g_built_a) return g_modified_a;

    LPSTR orig = g_orig_A();
    if (!orig) return orig;

    int nativeW = 0, nativeH = 0;
    native_dims(nativeW, nativeH);

    const char* val_start = nullptr;
    int origW = 0, origH = 0, val_len = 0;
    if (!find_dxres_a(orig, val_start, origW, origH, val_len)) {
        // No -dxresolution token: append one with native dims.
        const size_t orig_len = strlen(orig);
        char appended[64];
        int  ap_len = sprintf_s(appended, sizeof(appended), " -dxresolution=%dx%d", nativeW, nativeH);
        if (ap_len <= 0 || orig_len + static_cast<size_t>(ap_len) + 1 > sizeof(g_modified_a)) {
            mtr::log::info("cmdline(A): inject would overflow buffer, passthrough");
            size_t n = orig_len;
            if (n >= sizeof(g_modified_a)) n = sizeof(g_modified_a) - 1;
            memcpy(g_modified_a, orig, n);
            g_modified_a[n] = 0;
        } else {
            memcpy(g_modified_a, orig, orig_len);
            memcpy(g_modified_a + orig_len, appended, static_cast<size_t>(ap_len));
            g_modified_a[orig_len + ap_len] = 0;
            mtr::log::info("cmdline(A): no -dxresolution= token, injected %dx%d", nativeW, nativeH);
            mtr::log::info("cmdline(A) orig: %s", orig);
            mtr::log::info("cmdline(A) new : %s", g_modified_a);
        }
        g_built_a = true;
        return g_modified_a;
    }

    const size_t prefix_len = static_cast<size_t>(val_start - orig);
    char numbuf[64];
    int  num_len = sprintf_s(numbuf, sizeof(numbuf), "%dx%d", nativeW, nativeH);
    if (num_len <= 0) {
        mtr::log::info("cmdline(A): sprintf failed, passthrough");
        size_t n = strlen(orig);
        if (n >= sizeof(g_modified_a)) n = sizeof(g_modified_a) - 1;
        memcpy(g_modified_a, orig, n);
        g_modified_a[n] = 0;
        g_built_a = true;
        return g_modified_a;
    }
    const size_t suffix_off = prefix_len + static_cast<size_t>(val_len);
    const size_t suffix_len = strlen(orig + suffix_off);

    if (prefix_len + static_cast<size_t>(num_len) + suffix_len + 1 > sizeof(g_modified_a)) {
        mtr::log::info("cmdline(A): rewrite would overflow buffer, passthrough");
        size_t n = strlen(orig);
        if (n >= sizeof(g_modified_a)) n = sizeof(g_modified_a) - 1;
        memcpy(g_modified_a, orig, n);
        g_modified_a[n] = 0;
        g_built_a = true;
        return g_modified_a;
    }

    memcpy(g_modified_a, orig, prefix_len);
    memcpy(g_modified_a + prefix_len, numbuf, static_cast<size_t>(num_len));
    memcpy(g_modified_a + prefix_len + num_len, orig + suffix_off, suffix_len);
    g_modified_a[prefix_len + num_len + suffix_len] = 0;

    mtr::log::info("cmdline(A): %dx%d -> %dx%d", origW, origH, nativeW, nativeH);
    mtr::log::info("cmdline(A) orig: %s", orig);
    mtr::log::info("cmdline(A) new : %s", g_modified_a);
    g_built_a = true;
    return g_modified_a;
}

LPWSTR WINAPI hk_GetCommandLineW() {
    if (g_built_w) return g_modified_w;

    LPWSTR orig = g_orig_W();
    if (!orig) return orig;

    int nativeW = 0, nativeH = 0;
    native_dims(nativeW, nativeH);

    const wchar_t* val_start = nullptr;
    int origW = 0, origH = 0, val_len = 0;
    if (!find_dxres_w(orig, val_start, origW, origH, val_len)) {
        const size_t orig_len = wcslen(orig);
        wchar_t appended[64];
        int     ap_len = swprintf_s(appended, _countof(appended), L" -dxresolution=%dx%d", nativeW, nativeH);
        if (ap_len <= 0 || orig_len + static_cast<size_t>(ap_len) + 1 > _countof(g_modified_w)) {
            mtr::log::info("cmdline(W): inject would overflow buffer, passthrough");
            size_t n = orig_len;
            if (n >= _countof(g_modified_w)) n = _countof(g_modified_w) - 1;
            memcpy(g_modified_w, orig, n * sizeof(WCHAR));
            g_modified_w[n] = 0;
        } else {
            memcpy(g_modified_w, orig, orig_len * sizeof(WCHAR));
            memcpy(g_modified_w + orig_len, appended, static_cast<size_t>(ap_len) * sizeof(WCHAR));
            g_modified_w[orig_len + ap_len] = 0;
            mtr::log::info("cmdline(W): no -dxresolution= token, injected %dx%d", nativeW, nativeH);
        }
        g_built_w = true;
        return g_modified_w;
    }

    const size_t prefix_len = static_cast<size_t>(val_start - orig);
    wchar_t numbuf[64];
    int     num_len = swprintf_s(numbuf, _countof(numbuf), L"%dx%d", nativeW, nativeH);
    if (num_len <= 0) {
        mtr::log::info("cmdline(W): swprintf failed, passthrough");
        size_t n = wcslen(orig);
        if (n >= _countof(g_modified_w)) n = _countof(g_modified_w) - 1;
        memcpy(g_modified_w, orig, n * sizeof(WCHAR));
        g_modified_w[n] = 0;
        g_built_w = true;
        return g_modified_w;
    }
    const size_t suffix_off = prefix_len + static_cast<size_t>(val_len);
    const size_t suffix_len = wcslen(orig + suffix_off);

    if (prefix_len + static_cast<size_t>(num_len) + suffix_len + 1 > _countof(g_modified_w)) {
        mtr::log::info("cmdline(W): rewrite would overflow buffer, passthrough");
        size_t n = wcslen(orig);
        if (n >= _countof(g_modified_w)) n = _countof(g_modified_w) - 1;
        memcpy(g_modified_w, orig, n * sizeof(WCHAR));
        g_modified_w[n] = 0;
        g_built_w = true;
        return g_modified_w;
    }

    memcpy(g_modified_w, orig, prefix_len * sizeof(WCHAR));
    memcpy(g_modified_w + prefix_len, numbuf, static_cast<size_t>(num_len) * sizeof(WCHAR));
    memcpy(g_modified_w + prefix_len + num_len, orig + suffix_off, suffix_len * sizeof(WCHAR));
    g_modified_w[prefix_len + num_len + suffix_len] = 0;

    mtr::log::info("cmdline(W): %dx%d -> %dx%d", origW, origH, nativeW, nativeH);
    g_built_w = true;
    return g_modified_w;
}

} // namespace

void install() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) {
        mtr::log::info("cmdline: GetModuleHandle(kernel32) returned null");
        return;
    }
    void* pA = reinterpret_cast<void*>(GetProcAddress(k32, "GetCommandLineA"));
    void* pW = reinterpret_cast<void*>(GetProcAddress(k32, "GetCommandLineW"));
    if (!pA || !pW) {
        mtr::log::info("cmdline: GetProcAddress failed (A=%p W=%p)", pA, pW);
        return;
    }

    bool ok = true;
    if (MH_CreateHook(pA, &hk_GetCommandLineA, reinterpret_cast<void**>(&g_orig_A)) != MH_OK) {
        mtr::log::info("cmdline: MH_CreateHook(GetCommandLineA) failed"); ok = false;
    }
    if (MH_CreateHook(pW, &hk_GetCommandLineW, reinterpret_cast<void**>(&g_orig_W)) != MH_OK) {
        mtr::log::info("cmdline: MH_CreateHook(GetCommandLineW) failed"); ok = false;
    }
    if (!ok) return;

    if (MH_EnableHook(pA) != MH_OK || MH_EnableHook(pW) != MH_OK) {
        mtr::log::info("cmdline: MH_EnableHook failed");
        return;
    }
    mtr::log::info("cmdline: GetCommandLineA/W hooks armed (A=%p W=%p)", pA, pW);
}

} // namespace mtr::cmdline
