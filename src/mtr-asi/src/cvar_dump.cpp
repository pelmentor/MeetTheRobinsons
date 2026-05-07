// Cvar registration dumper.
//
// Hooks the engine's eight typed-variant registration functions
//   console_register_var_typed_a..h  (0x5894D0 .. 0x589A50)
// and records every (type, group, name, address, caller) tuple as the engine
// registers its cvars during init. All eight share the same prefix arg shape
// __thiscall(this, group_ptr, name_ptr, addr, ...) — confirmed by reading
// each decompile, e.g. sub_655FA0 (engine "scene" namespace) calls
//   sub_589CE0("scene", "fogEnabled", 0x745279, ...)
// which routes through one of the typed_X functions. So a uniform hook on
// the 8 leaf functions catches every registration regardless of which
// wrapper (sub_589CE0 / 589D10 / 589810 / 589E20 / direct call) made it.
//
// IMPORTANT: The eight functions have DIFFERENT trailing-arg counts (6 to 10).
// In __thiscall, the callee pops args via `ret N`. If we declare a hook with
// a different arg count than the orig, MSVC compiles it to pop the WRONG
// amount on return → caller-frame stack corruption → crash.
// So each hk_typed_X has its own signature with the exact arg count, even
// though we only inspect the first three (group, name, address).
//
// Output: dump_to_file() writes a text table for offline grep. Once we know
// the (group, name, address), live-edit is a simple write to *(float*)addr —
// pure data layer, no patches.
//
// Install in DllMain (post-MH_Initialize) so the hooks are armed before the
// game's WinMain runs the registration code.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::cvar_dump {

namespace {

struct Entry {
    char        type;     // 'a'..'h' — which typed_X function registered it
    const char* group;    // raw ptr (may be a string OR an entity-context ptr)
    const char* name;     // raw ptr — always a literal string in known callers
    uintptr_t   address;  // VA of the underlying variable storage
    void*       caller;   // _ReturnAddress() — useful to disambiguate group origins
};

// Per-typed_X signatures. MSVC __thiscall maps to __fastcall(this in ECX,
// edx unused, then stack args). Arg counts come straight from each function's
// IDA decompile — DO NOT add or remove args without checking the current
// signature, or the callee-cleanup `ret N` mismatch will corrupt the caller.
//
// typed_a : 8 stack args  (a, b, c)         — also typed_b
// typed_c : 9 stack args
// typed_d : 10 stack args
// typed_e : 6 stack args  (also typed_h)
// typed_f : 7 stack args  (also typed_g)
//
// We only USE the first 3 stack args (group, name, addr); the rest are
// forwarded byte-for-byte.

using PFN_TypedAB = char (__fastcall*)(void* this_, int /*edx*/,
                                        const char* g, const char* n, uintptr_t a,
                                        int a5, int a6, int a7, int a8, int a9);
using PFN_TypedC  = char (__fastcall*)(void* this_, int /*edx*/,
                                        const char* g, const char* n, uintptr_t a,
                                        int a5, int a6, int a7, int a8, int a9, int a10);
using PFN_TypedD  = char (__fastcall*)(void* this_, int /*edx*/,
                                        const char* g, const char* n, uintptr_t a,
                                        int a5, int a6, int a7, int a8, int a9, int a10, int a11);
using PFN_TypedEH = char (__fastcall*)(void* this_, int /*edx*/,
                                        const char* g, const char* n, uintptr_t a,
                                        int a5, int a6, int a7);
using PFN_TypedFG = char (__fastcall*)(void* this_, int /*edx*/,
                                        const char* g, const char* n, uintptr_t a,
                                        int a5, int a6, int a7, int a8);

PFN_TypedAB g_orig_a = nullptr;
PFN_TypedAB g_orig_b = nullptr;
PFN_TypedC  g_orig_c = nullptr;
PFN_TypedD  g_orig_d = nullptr;
PFN_TypedEH g_orig_e = nullptr;
PFN_TypedFG g_orig_f = nullptr;
PFN_TypedFG g_orig_g = nullptr;
PFN_TypedEH g_orig_h = nullptr;

std::vector<Entry> g_entries;
std::mutex         g_mu;
std::atomic<bool>  g_inited{false};

void record(char type, const char* group, const char* name, uintptr_t addr, void* caller) {
    std::scoped_lock lk(g_mu);
    g_entries.push_back({type, group, name, addr, caller});
}

// typed_a / typed_b — 8 stack args
char __fastcall hk_typed_a(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8, int a9) {
    record('a', g, n, a, _ReturnAddress());
    return g_orig_a(this_, edx, g, n, a, a5, a6, a7, a8, a9);
}
char __fastcall hk_typed_b(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8, int a9) {
    record('b', g, n, a, _ReturnAddress());
    return g_orig_b(this_, edx, g, n, a, a5, a6, a7, a8, a9);
}

// typed_c — 9 stack args
char __fastcall hk_typed_c(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8, int a9, int a10) {
    record('c', g, n, a, _ReturnAddress());
    return g_orig_c(this_, edx, g, n, a, a5, a6, a7, a8, a9, a10);
}

// typed_d — 10 stack args
char __fastcall hk_typed_d(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8, int a9, int a10, int a11) {
    record('d', g, n, a, _ReturnAddress());
    return g_orig_d(this_, edx, g, n, a, a5, a6, a7, a8, a9, a10, a11);
}

// typed_e / typed_h — 6 stack args
char __fastcall hk_typed_e(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7) {
    record('e', g, n, a, _ReturnAddress());
    return g_orig_e(this_, edx, g, n, a, a5, a6, a7);
}
char __fastcall hk_typed_h(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7) {
    record('h', g, n, a, _ReturnAddress());
    return g_orig_h(this_, edx, g, n, a, a5, a6, a7);
}

// typed_f / typed_g — 7 stack args
char __fastcall hk_typed_f(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8) {
    record('f', g, n, a, _ReturnAddress());
    return g_orig_f(this_, edx, g, n, a, a5, a6, a7, a8);
}
char __fastcall hk_typed_g(void* this_, int edx,
                            const char* g, const char* n, uintptr_t a,
                            int a5, int a6, int a7, int a8) {
    record('g', g, n, a, _ReturnAddress());
    return g_orig_g(this_, edx, g, n, a, a5, a6, a7, a8);
}

struct HookEntry {
    uintptr_t   va;
    void*       hook;
    void**      orig;
    const char* tag;
};

// Best-effort string read. A registration's `group` arg is sometimes a literal
// string (e.g. "scene"), sometimes a pointer to an entity struct used as a
// context key. We accept it as a string only if every byte up to the first
// NUL is printable ASCII and the NUL appears within max_len.
bool try_read_string(const char* p, char* out, size_t max_len) {
    if (!p) return false;
    __try {
        for (size_t i = 0; i < max_len; ++i) {
            unsigned char c = static_cast<unsigned char>(p[i]);
            if (c == 0) {
                out[i] = 0;
                return i > 0; // non-empty string
            }
            if (c < 0x20 || c > 0x7E) return false;
            out[i] = static_cast<char>(c);
        }
        return false; // no NUL within max_len
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool install() {
    if (g_inited.exchange(true)) return true;

    HookEntry entries[] = {
        {0x005894D0, &hk_typed_a, reinterpret_cast<void**>(&g_orig_a), "typed_a"},
        {0x005895A0, &hk_typed_b, reinterpret_cast<void**>(&g_orig_b), "typed_b"},
        {0x00589670, &hk_typed_c, reinterpret_cast<void**>(&g_orig_c), "typed_c"},
        {0x00589740, &hk_typed_d, reinterpret_cast<void**>(&g_orig_d), "typed_d"},
        {0x00589816, &hk_typed_e, reinterpret_cast<void**>(&g_orig_e), "typed_e"},
        {0x005898D0, &hk_typed_f, reinterpret_cast<void**>(&g_orig_f), "typed_f"},
        {0x00589990, &hk_typed_g, reinterpret_cast<void**>(&g_orig_g), "typed_g"},
        {0x00589A50, &hk_typed_h, reinterpret_cast<void**>(&g_orig_h), "typed_h"},
    };

    bool ok_all = true;
    for (auto& e : entries) {
        void* p = reinterpret_cast<void*>(e.va);
        if (MH_CreateHook(p, e.hook, e.orig) != MH_OK) {
            mtr::log::info("cvar_dump: MH_CreateHook(%s @ 0x%p) failed", e.tag, p);
            ok_all = false;
            continue;
        }
        if (MH_EnableHook(p) != MH_OK) {
            mtr::log::info("cvar_dump: MH_EnableHook(%s @ 0x%p) failed", e.tag, p);
            ok_all = false;
            continue;
        }
    }
    mtr::log::info("cvar_dump: registration hooks armed (ok=%d)", ok_all ? 1 : 0);
    return ok_all;
}

size_t count() {
    std::scoped_lock lk(g_mu);
    return g_entries.size();
}

bool dump_to_file(const char* path) {
    std::vector<Entry> snapshot;
    {
        std::scoped_lock lk(g_mu);
        snapshot = g_entries;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        mtr::log::info("cvar_dump: fopen failed for %s", path);
        return false;
    }

    std::fprintf(f, "# mtr-asi cvar dump  (entries=%zu)\n", snapshot.size());
    std::fprintf(f, "# columns: type | group | name | address | caller\n");
    std::fprintf(f, "# type:    a..h = which console_register_var_typed_X registered the cvar\n");
    std::fprintf(f, "#          a/b: 8 stack args   c: 9   d: 10   e/h: 6   f/g: 7\n");
    std::fprintf(f, "# group:   if it looks like a string, printed; otherwise raw pointer.\n");
    std::fprintf(f, "#\n");
    std::fprintf(f, "# %-2s | %-32s | %-40s | %-10s | %-10s\n",
                 "ty", "group", "name", "address", "caller");
    std::fprintf(f, "# ---+----------------------------------+------------------------------------------+------------+----------\n");

    char gbuf[128];
    char nbuf[128];
    for (const auto& e : snapshot) {
        const char* g_str = try_read_string(e.group, gbuf, sizeof(gbuf)) ? gbuf : nullptr;
        const char* n_str = try_read_string(e.name,  nbuf, sizeof(nbuf)) ? nbuf : nullptr;

        if (g_str) {
            std::fprintf(f, "  %c  | %-32.32s | ", e.type, g_str);
        } else {
            char gp[20];
            std::snprintf(gp, sizeof(gp), "(ptr=0x%p)", e.group);
            std::fprintf(f, "  %c  | %-32s | ", e.type, gp);
        }
        if (n_str) {
            std::fprintf(f, "%-40.40s | ", n_str);
        } else {
            char np[20];
            std::snprintf(np, sizeof(np), "(ptr=0x%p)", e.name);
            std::fprintf(f, "%-40s | ", np);
        }
        std::fprintf(f, "0x%08X | %p\n",
                     static_cast<uint32_t>(e.address), e.caller);
    }

    std::fprintf(f, "\n# end\n");
    std::fclose(f);
    mtr::log::info("cvar_dump: wrote %zu entries to %s", snapshot.size(), path);
    return true;
}

bool dump_default() {
    char exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return dump_to_file("mtr_cvars.txt");
    }
    for (DWORD i = n; i > 0; --i) {
        if (exe_path[i - 1] == '\\' || exe_path[i - 1] == '/') {
            exe_path[i] = 0;
            break;
        }
    }
    char out[MAX_PATH];
    std::snprintf(out, sizeof(out), "%smtr_cvars.txt", exe_path);
    return dump_to_file(out);
}

} // namespace mtr::cvar_dump
