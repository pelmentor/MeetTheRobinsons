#include <windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mtr::log {

static FILE*      s_file = nullptr;
static std::mutex s_mu;

void init() {
    if (s_file) return;
    // _fsopen with _SH_DENYNO so other processes can tail/cat the log live.
    s_file = _fsopen("mtr-asi.log", "w", _SH_DENYNO);
    if (s_file) {
        std::fputs("=== mtr-asi log ===\n", s_file);
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
