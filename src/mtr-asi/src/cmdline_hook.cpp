#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdlib.h>

namespace mtr     { HMODULE self_module(); }
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

// "-letitsnow" is an Easter-egg cmdline flag (Disney/Avalanche). The string
// lives at 0xF003E4 in the runtime-decompressed `rr01` region; the cmdline
// flag-parsing happens in the same region as the `-dxwindowed` family
// (sub_62B050 strstr + per-flag byte global). Direct static xrefs are
// empty due to register-indirect addressing in rr01, so we can't easily
// flip the post-parse global at runtime — but injecting "-letitsnow" into
// argv at boot uses the engine's own activation path. Failure mode is
// benign (no effect if the flag is dead in retail).
//
// Persisted in mtr-asi-ui.ini under [Boot]letitsnow=0|1.
bool resolve_ini_path(char* out, size_t out_size) {
    if (!out || out_size < MAX_PATH) return false;
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-ui.ini", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

bool read_letitsnow_setting() {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return false;
    return GetPrivateProfileIntA("Boot", "letitsnow", 0, ini) != 0;
}
void write_letitsnow_setting(bool v) {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return;
    WritePrivateProfileStringA("Boot", "letitsnow", v ? "1" : "0", ini);
}

// Returns true if the cmdline already contains "-letitsnow" as a token.
bool has_letitsnow_a(const char* line) {
    return line && std::strstr(line, "-letitsnow") != nullptr;
}
bool has_letitsnow_w(const wchar_t* line) {
    return line && std::wcsstr(line, L"-letitsnow") != nullptr;
}

// Returns true if the cmdline contains `-mtrasi-keep-dxresolution`. Used to
// opt out of the auto-rewrite of -dxresolution to native monitor dims. The
// rewrite is normally a feature (the Disney launcher passes a tiny default
// like 320x240 that we bump to native), but the dual-local LAN test harness
// needs two windowed Wilburs at a small explicit resolution so they can
// coexist on one screen without fighting for fullscreen-borderless focus.
bool has_keep_dxres_a(const char* line) {
    return line && std::strstr(line, "-mtrasi-keep-dxresolution") != nullptr;
}
bool has_keep_dxres_w(const wchar_t* line) {
    return line && std::wcsstr(line, L"-mtrasi-keep-dxresolution") != nullptr;
}

// Final post-processing: append " -letitsnow" if the user wants it and the
// rewritten cmdline doesn't already have it. Runs on every successful path
// of hk_GetCommandLineA / hk_GetCommandLineW (including passthrough cases).
void maybe_inject_letitsnow_a() {
    if (!read_letitsnow_setting()) return;
    if (has_letitsnow_a(g_modified_a)) return;
    const size_t cur_len = std::strlen(g_modified_a);
    const char* tok = " -letitsnow";
    const size_t tok_len = std::strlen(tok);
    if (cur_len + tok_len + 1 > sizeof(g_modified_a)) return;
    std::memcpy(g_modified_a + cur_len, tok, tok_len);
    g_modified_a[cur_len + tok_len] = 0;
    mtr::log::info("cmdline(A): injected -letitsnow");
}
void maybe_inject_letitsnow_w() {
    if (!read_letitsnow_setting()) return;
    if (has_letitsnow_w(g_modified_w)) return;
    const size_t cur_len = std::wcslen(g_modified_w);
    const wchar_t* tok = L" -letitsnow";
    const size_t tok_len = std::wcslen(tok);
    if (cur_len + tok_len + 1 > _countof(g_modified_w)) return;
    std::memcpy(g_modified_w + cur_len, tok, tok_len * sizeof(WCHAR));
    g_modified_w[cur_len + tok_len] = 0;
    mtr::log::info("cmdline(W): injected -letitsnow");
}

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

    // -mtrasi-keep-dxresolution: opt-out of the native-dims rewrite. Pass
    // the original cmdline through unmodified (still through the modified
    // buffer so letitsnow injection can run).
    if (has_keep_dxres_a(orig)) {
        size_t n = std::strlen(orig);
        if (n >= sizeof(g_modified_a)) n = sizeof(g_modified_a) - 1;
        std::memcpy(g_modified_a, orig, n);
        g_modified_a[n] = 0;
        mtr::log::info("cmdline(A): -mtrasi-keep-dxresolution present, "
                       "passthrough");
        maybe_inject_letitsnow_a();
        g_built_a = true;
        return g_modified_a;
    }

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
        maybe_inject_letitsnow_a();
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
        maybe_inject_letitsnow_a();
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
        maybe_inject_letitsnow_a();
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
    maybe_inject_letitsnow_a();
    g_built_a = true;
    return g_modified_a;
}

LPWSTR WINAPI hk_GetCommandLineW() {
    if (g_built_w) return g_modified_w;

    LPWSTR orig = g_orig_W();
    if (!orig) return orig;

    if (has_keep_dxres_w(orig)) {
        size_t n = std::wcslen(orig);
        if (n >= _countof(g_modified_w)) n = _countof(g_modified_w) - 1;
        std::memcpy(g_modified_w, orig, n * sizeof(WCHAR));
        g_modified_w[n] = 0;
        mtr::log::info("cmdline(W): -mtrasi-keep-dxresolution present, "
                       "passthrough");
        maybe_inject_letitsnow_w();
        g_built_w = true;
        return g_modified_w;
    }

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
        maybe_inject_letitsnow_w();
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
        maybe_inject_letitsnow_w();
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
        maybe_inject_letitsnow_w();
        g_built_w = true;
        return g_modified_w;
    }

    memcpy(g_modified_w, orig, prefix_len * sizeof(WCHAR));
    memcpy(g_modified_w + prefix_len, numbuf, static_cast<size_t>(num_len) * sizeof(WCHAR));
    memcpy(g_modified_w + prefix_len + num_len, orig + suffix_off, suffix_len * sizeof(WCHAR));
    g_modified_w[prefix_len + num_len + suffix_len] = 0;

    mtr::log::info("cmdline(W): %dx%d -> %dx%d", origW, origH, nativeW, nativeH);
    maybe_inject_letitsnow_w();
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

// Public API for the snow Easter-egg toggle. The setting is persisted in
// mtr-asi-ui.ini; the cmdline hook re-reads it lazily on the first
// GetCommandLine call (which happens during CRT init, before menu is up).
// Toggling at runtime via the menu writes the ini but the cmdline is
// already cached for THIS process — change takes effect on the NEXT launch.
bool snow_at_boot()        { return read_letitsnow_setting(); }
void set_snow_at_boot(bool v) {
    write_letitsnow_setting(v);
    mtr::log::info("cmdline: snow_at_boot persisted = %d (effective on next launch)", v ? 1 : 0);
}

} // namespace mtr::cmdline
