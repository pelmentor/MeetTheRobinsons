// Process-wide crash handler — installs SetUnhandledExceptionFilter at
// DLL_PROCESS_ATTACH so any unhandled SEH exception (engine OR our hooks)
// gets logged to mtr-asi.log AND dumped to mtr-asi.dmp (next to the ASI).
//
// Why we need this (2026-05-09): user reported a crash during in-game
// save with no SEH marker in our log. Without a stack trace we can't
// distinguish engine-side from hook-side crashes. This handler captures:
//   - exception code (AV, illegal-instruction, stack-overflow, ...)
//   - faulting address (EIP / RIP)
//   - module that owns EIP (so we know if it's Wilbur.exe or mtr-asi.asi)
//   - a minidump that IDA / WinDbg / VS can load for full stack trace
//
// The handler runs FROM the crashing thread, BEFORE Windows kills the
// process — so it's the last chance to record what happened.
//
// Re-entrance safety: the handler uses a std::atomic flag to guarantee
// only the first crash is logged + dumped (a fault in the dump-write
// itself shouldn't loop). If another thread faults concurrently, it
// returns EXCEPTION_CONTINUE_SEARCH and Windows takes the process down
// without re-entering us.

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <atomic>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace mtr        { HMODULE self_module(); }
namespace mtr::log   { void info(const char* fmt, ...); }
namespace mtr::test_harness {
    int  coop_port();
    bool result_path(char* out, size_t out_size);
}

namespace mtr::crash_handler {

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
std::atomic<bool>            g_handled{false};
std::atomic<bool>            g_sym_initialized{false};

bool resolve_dump_path(char* out, size_t out_size) {
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = std::snprintf(out, out_size,
                          "%smtr-asi-crash-%04u%02u%02u-%02u%02u%02u.dmp",
                          modpath,
                          st.wYear, st.wMonth, st.wDay,
                          st.wHour, st.wMinute, st.wSecond);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
        default:                                 return "UNKNOWN";
    }
}

// Resolve `addr` to "function+0xNN  (file:line)" via dbghelp, IF SymInitialize
// was called successfully at install time AND the PDB is reachable.
//
// Output formats:
//   "<sym>+0xNN  (<file>:<line>)"   — full hit, line table present
//   "<sym>+0xNN"                    — sym hit, no line info
//   ""                              — nothing resolved (caller falls back to
//                                     module+offset)
//
// Buffer is left as an empty C-string on miss. SEH-guarded — dbghelp can
// throw on a corrupted PDB load attempt, and we are very willing to lose
// symbol detail rather than crash the crash handler.
bool resolve_symbol_locked(uintptr_t addr, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (!g_sym_initialized.load(std::memory_order_acquire)) return false;

    __try {
        // SymFromAddr: name + displacement.
        union {
            SYMBOL_INFO si;
            char        buf[sizeof(SYMBOL_INFO) + 512];
        } u{};
        u.si.SizeOfStruct = sizeof(SYMBOL_INFO);
        u.si.MaxNameLen   = 511;

        DWORD64 disp = 0;
        BOOL sym_ok = SymFromAddr(GetCurrentProcess(),
                                  static_cast<DWORD64>(addr),
                                  &disp, &u.si);
        if (!sym_ok) return false;

        // SymGetLineFromAddr64: file + line.
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;
        BOOL line_ok = SymGetLineFromAddr64(GetCurrentProcess(),
                                            static_cast<DWORD64>(addr),
                                            &line_disp, &line);

        if (line_ok && line.FileName) {
            // Trim file path to just the basename to keep log lines compact.
            const char* slash = std::strrchr(line.FileName, '\\');
            if (!slash) slash = std::strrchr(line.FileName, '/');
            const char* tail = slash ? slash + 1 : line.FileName;
            std::snprintf(out, out_size, "%s+0x%X  (%s:%lu)",
                          u.si.Name,
                          static_cast<unsigned>(disp),
                          tail,
                          static_cast<unsigned long>(line.LineNumber));
        } else {
            std::snprintf(out, out_size, "%s+0x%X",
                          u.si.Name,
                          static_cast<unsigned>(disp));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool resolve_module_for_addr(uintptr_t addr, char* out, size_t out_size) {
    HMODULE mods[256] = {0};
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return false;
    int count = (int)(needed / sizeof(HMODULE));
    if (count > 256) count = 256;
    for (int i = 0; i < count; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
        uintptr_t base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        uintptr_t end  = base + mi.SizeOfImage;
        if (addr >= base && addr < end) {
            char name[MAX_PATH] = {0};
            if (GetModuleFileNameExA(proc, mods[i], name, sizeof(name))) {
                const char* tail = std::strrchr(name, '\\');
                if (!tail) tail = std::strrchr(name, '/');
                tail = tail ? tail + 1 : name;
                std::snprintf(out, out_size, "%s+0x%X",
                              tail, (unsigned)(addr - base));
                return true;
            }
        }
    }
    std::snprintf(out, out_size, "?+0x%X", (unsigned)addr);
    return false;
}

LONG WINAPI top_level_filter(EXCEPTION_POINTERS* ep) {
    // First-crash-only. If we're already in here (or a prior crash
    // already ran), pass through to next filter / OS.
    bool expected = false;
    if (!g_handled.compare_exchange_strong(expected, true)) {
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    if (!ep || !ep->ExceptionRecord) {
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD code        = ep->ExceptionRecord->ExceptionCode;
    auto  fault_addr  = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    // Log a one-line summary first (cheapest action — guarantees we get
    // SOMETHING in the log even if dump-write fails).
    char modspec[MAX_PATH + 32] = {0};
    resolve_module_for_addr(fault_addr, modspec, sizeof(modspec));
    char symspec[640] = {0};
    resolve_symbol_locked(fault_addr, symspec, sizeof(symspec));
    if (symspec[0]) {
        mtr::log::info("[CRASH] code=0x%08X (%s) addr=0x%p (%s) sym=%s tid=%lu",
                       (unsigned)code, exception_name(code),
                       ep->ExceptionRecord->ExceptionAddress,
                       modspec, symspec,
                       GetCurrentThreadId());
    } else {
        mtr::log::info("[CRASH] code=0x%08X (%s) addr=0x%p (%s) tid=%lu",
                       (unsigned)code, exception_name(code),
                       ep->ExceptionRecord->ExceptionAddress,
                       modspec,
                       GetCurrentThreadId());
    }

    // For AV: log read/write flag + accessed address.
    if (code == EXCEPTION_ACCESS_VIOLATION
        && ep->ExceptionRecord->NumberParameters >= 2) {
        const char* op = ep->ExceptionRecord->ExceptionInformation[0] == 0
                             ? "read"
                             : ep->ExceptionRecord->ExceptionInformation[0] == 1
                                   ? "write"
                                   : "exec";
        mtr::log::info("[CRASH] AV %s of 0x%p",
                       op,
                       (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    // Write minidump.
    char dump_path[MAX_PATH] = {0};
    if (resolve_dump_path(dump_path, sizeof(dump_path))) {
        HANDLE f = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;
            // MiniDumpWithDataSegs gives us globals (~5MB on this binary —
            // includes the engine sprite list head etc); MiniDumpWithThreadInfo
            // gives stack of every thread (so we see what alt-pump was doing).
            BOOL ok = MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), f,
                static_cast<MINIDUMP_TYPE>(
                    MiniDumpNormal
                    | MiniDumpWithDataSegs
                    | MiniDumpWithThreadInfo
                    | MiniDumpWithUnloadedModules),
                &mei, nullptr, nullptr);
            CloseHandle(f);
            mtr::log::info("[CRASH] dump %s -> %s",
                           ok ? "written" : "FAILED", dump_path);
        } else {
            mtr::log::info("[CRASH] CreateFileA failed (err=%lu) for %s",
                           GetLastError(), dump_path);
        }
    }

    // Write a result-JSON sentinel so run-test.ps1 sees a structured
    // "crash" outcome instead of misinterpreting "no JSON + process gone"
    // as exit 4 (launch failure). Only writes if the file doesn't exist
    // yet — we don't want to clobber a real PASS result that may have
    // been written milliseconds before the crash. Best-effort: a faulting
    // path here just falls through to WerFault as before.
    //
    // Phase 0D (2026-05-11): path goes through test_harness::result_path so
    // we land on the port-suffixed file when -mtrasi-coop-port=<N> was on
    // the cmdline. That keeps the host/client crash sentinels in their own
    // lanes during two-process coop runs.
    {
        char res_path[MAX_PATH] = {0};
        if (mtr::test_harness::result_path(res_path, sizeof(res_path))) {
            DWORD attr = GetFileAttributesA(res_path);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                HANDLE rf = CreateFileA(res_path, GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
                if (rf != INVALID_HANDLE_VALUE) {
                    char body[512];
                    int n = std::snprintf(body, sizeof(body),
                        "{\"scenario\":\"\","
                        "\"result\":\"crash\","
                        "\"elapsed_ms\":0,"
                        "\"frames\":0,"
                        "\"coop_port\":%d,"
                        "\"detail\":\"unhandled SEH 0x%08X (%s) at 0x%p (%s) tid=%lu\"}\n",
                        mtr::test_harness::coop_port(),
                        (unsigned)code, exception_name(code),
                        ep->ExceptionRecord->ExceptionAddress,
                        modspec, GetCurrentThreadId());
                    DWORD wr = 0;
                    WriteFile(rf, body, (DWORD)(n > 0 ? n : 0), &wr, nullptr);
                    CloseHandle(rf);
                }
            }
        }
    }

    // Pass to whatever was previously installed (CRT / Windows default
    // handler), which will normally pop the WerFault dialog and kill
    // the process. We've done our part.
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() {
    g_prev_filter = SetUnhandledExceptionFilter(&top_level_filter);

    // Initialize the symbol engine once at install time. SymInitialize at
    // fault time would be unsafe — the process heap may already be corrupt.
    // INVADE_PROCESS=TRUE pre-loads PDBs for every already-mapped module
    // (Wilbur.exe + dxvk d3d9.dll + mtr-asi.asi). Path '\0' means default
    // search: exe dir, _NT_SYMBOL_PATH, %SystemRoot%, then PDBs sitting
    // next to each module's DLL — which is where our .pdb lives.
    BOOL sym_ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    if (sym_ok) {
        SymSetOptions(SymGetOptions()
                      | SYMOPT_LOAD_LINES
                      | SYMOPT_DEFERRED_LOADS
                      | SYMOPT_FAIL_CRITICAL_ERRORS);
        g_sym_initialized.store(true, std::memory_order_release);
    }

    mtr::log::info("crash_handler: installed (prev=%p) sym=%s",
                   g_prev_filter, sym_ok ? "OK" : "FAIL");
}

// Public helper — callable from other TUs (e.g. coop_spawn_probe's VEH)
// to localize a known address inside our own ASI without waiting for the
// process-wide top-level filter to fire.
bool resolve_symbol(uintptr_t addr, char* out, size_t out_size) {
    return resolve_symbol_locked(addr, out, out_size);
}

} // namespace mtr::crash_handler
