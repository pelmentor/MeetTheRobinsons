#include <windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mtr::log {

static FILE*      s_file = nullptr;
static std::mutex s_mu;

// Scan cmdline for `-mtrasi-coop-port=N`. Used to compose per-process log
// filenames when two Wilburs share Game/ in a dual-local test. Returns 0
// on absence (default mtr-asi.log path), positive on success. Same parsing
// rules as the test_harness module: anchored at word boundary, value
// terminated by whitespace or NUL.
static int scan_coop_port() {
    const char* line = GetCommandLineA();
    if (!line) return 0;
    const char* needle = "-mtrasi-coop-port=";
    const size_t needle_len = 18;
    const char* p = line;
    while ((p = std::strstr(p, needle)) != nullptr) {
        const bool prev_ok = (p == line) || p[-1] == ' ' || p[-1] == '\t';
        if (prev_ok) {
            const char* val = p + needle_len;
            const int n = std::atoi(val);
            if (n > 0 && n < 65536) return n;
        }
        ++p;
    }
    return 0;
}

void init() {
    if (s_file) return;
    const int port = scan_coop_port();
    char path[64];
    if (port > 0) {
        std::snprintf(path, sizeof(path), "mtr-asi-%d.log", port);
    } else {
        std::strncpy(path, "mtr-asi.log", sizeof(path));
        path[sizeof(path) - 1] = 0;
    }
    // _fsopen with _SH_DENYNO so other processes can tail/cat the log live.
    s_file = _fsopen(path, "w", _SH_DENYNO);
    if (s_file) {
        std::fprintf(s_file, "=== mtr-asi log (path=%s pid=%lu) ===\n",
                     path, GetCurrentProcessId());
        std::fflush(s_file);
    }
}

void info(const char* fmt, ...) {
    std::scoped_lock lock(s_mu);
    if (!s_file) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(s_file, "[%02u:%02u:%02u.%03u] ",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(s_file, fmt, ap);
    va_end(ap);

    std::fputc('\n', s_file);
    std::fflush(s_file);
}

void shutdown() {
    std::scoped_lock lock(s_mu);
    if (s_file) {
        std::fclose(s_file);
        s_file = nullptr;
    }
}

} // namespace mtr::log
