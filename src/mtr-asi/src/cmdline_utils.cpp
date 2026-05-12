// Shared cmdline helpers. See mtr/cmdline_utils.h for the contract.

#include "mtr/cmdline_utils.h"

#include <cstring>

namespace mtr::cmdline_utils {

bool has_flag(const char* cmdline, const char* flag) {
    if (!cmdline || !flag) return false;
    const std::size_t flag_len = std::strlen(flag);
    if (flag_len == 0) return false;

    const char* p = cmdline;
    while ((p = std::strstr(p, flag)) != nullptr) {
        const char next = p[flag_len];
        if (next == '\0' || next == ' ' || next == '\t') return true;
        p += flag_len;
    }
    return false;
}

bool get_flag_value(const char* cmdline, const char* flag,
                    char* out, std::size_t cap) {
    if (!cmdline || !flag || !out || cap == 0) return false;
    const std::size_t flag_len = std::strlen(flag);
    if (flag_len == 0) return false;

    const char* p = cmdline;
    while ((p = std::strstr(p, flag)) != nullptr) {
        // Whole-word check: same terminator definition as has_flag.
        const char next = p[flag_len];
        if (next != ' ' && next != '\t') {
            // Two non-whitespace cases. NUL: flag occupies end-of-
            // cmdline with no following value, and strstr already
            // returned the EARLIEST occurrence in `p..`, so there can
            // be no later match past the NUL — no value possible. The
            // dash case (substring of a longer flag like `-foo-bar`
            // when searching for `-foo`) needs to keep searching; the
            // value case (the next token starts with `-` because the
            // user passed a flag where a value was expected) is
            // collapsed into the same path because either way no value
            // is present at this position.
            if (next == '\0') return false;
            p += flag_len;
            continue;
        }

        // Skip whitespace after the flag to find the value start.
        const char* v = p + flag_len;
        while (*v == ' ' || *v == '\t') ++v;
        if (*v == '\0') return false;        // end-of-cmdline, no value
        if (*v == '-')  return false;        // next is another flag

        // Copy until next whitespace / NUL.
        std::size_t i = 0;
        while (i + 1 < cap && v[i] != '\0' && v[i] != ' ' && v[i] != '\t') {
            out[i] = v[i];
            ++i;
        }
        // Refuse on truncation: better to fail loud than silently lose
        // characters from an IP:PORT or PORT value.
        if (i + 1 >= cap && v[i] != '\0' && v[i] != ' ' && v[i] != '\t') {
            return false;
        }
        out[i] = '\0';
        return true;
    }
    return false;
}

} // namespace mtr::cmdline_utils
