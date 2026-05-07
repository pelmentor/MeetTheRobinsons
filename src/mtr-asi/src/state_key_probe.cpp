// state_key target-memory probe.
//
// Phase D static analysis dead-ended on SecuROM: the sprite list pushers,
// state_key consumers (sub_565CF0), and the texture loader path are all
// runtime-decrypted, so we can't trace state_key → texture path statically.
//
// What we CAN do at runtime: state_key is a heap pointer (the texture/asset
// object). If we read the bytes at that pointer we can identify the
// object's layout — its vtable, embedded name strings, etc. Wilbur is
// Gamebryo-derived (per project memories), and Gamebryo's NiObject base
// has a well-known layout: vtable + ref_count + (NiAVObject-derived: char*
// m_pcName at a fixed offset). One memory dump is enough to find the
// right offset and unlock auto-naming.
//
// Usage: open the Insert menu, ensure some sprites are rendering, click
// "Dump state_key probe CSV" in the per-element TreeNode. Output goes to
// Game/mtr-asi-state-key-probe.csv. Send it back for offline analysis;
// once we have the m_pcName offset we wire it into sprite_xform.cpp's
// auto-naming path.

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace mtr     { HMODULE self_module(); }
namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::sprite_xform {
    struct SlotInfo {
        int      slot_idx;
        uint32_t state_key;
        uint16_t uv_bucket;
        uint8_t  screen_context;
        uint8_t  bbox_quadrant;
        uint16_t last_uv_bucket;
        uint8_t  last_screen_context;
        uint8_t  last_bbox_quadrant;
        bool     last_concrete_valid;
        uint32_t frame_count;
        uint64_t total_count;
        char     name[48];
        char     group[32];
        float    offset_x, offset_y, scale_x, scale_y;
        bool     hidden;
    };
    int snapshot_slots(SlotInfo* out, int max_out);
}

namespace mtr::state_key_probe {

namespace {

// VirtualQuery-guarded read. Returns bytes actually read (0 on failure).
size_t read_safe(uintptr_t addr, void* dst, size_t want) {
    if (addr == 0 || addr < 0x10000) return 0;  // null / NT_HEADERS area
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    const DWORD readable_mask =
        PAGE_READONLY | PAGE_READWRITE |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable_mask)) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    // Clamp `want` to the end of the page region — keeps cross-region reads
    // from tripping a fault even when the next page is unreadable.
    const uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t region_end   = region_start + mbi.RegionSize;
    if (addr + want > region_end) want = region_end - addr;
    if (want == 0) return 0;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(addr), want);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return want;
}

bool resolve_csv_path(char* out, size_t out_size) {
    HMODULE m = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(m, modpath, sizeof(modpath))) return false;
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-state-key-probe.csv", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

// Try to read a C-string at addr (up to N bytes, stopping at NUL). Returns
// the count of bytes read (NUL excluded), or 0 on failure / no NUL.
size_t read_cstring_safe(uintptr_t addr, char* out, size_t out_size) {
    if (out_size == 0) return 0;
    out[0] = 0;
    char buf[256];
    const size_t got = read_safe(addr, buf, sizeof(buf));
    if (got == 0) return 0;
    size_t n = 0;
    while (n < got && n + 1 < out_size && buf[n] != 0) {
        const unsigned char c = static_cast<unsigned char>(buf[n]);
        // Reject obviously non-string bytes early (control chars except
        // tab, non-ASCII high) to keep the output readable.
        if (c < 0x20 && c != '\t') break;
        if (c >= 0x7F) break;
        out[n] = buf[n];
        ++n;
    }
    out[n] = 0;
    return n;
}

std::atomic<uint64_t> g_dump_count{0};

} // namespace

uint64_t last_dump_count() { return g_dump_count.load(); }

// === Auto-name helper (Phase D.4) ===========================================
//
// User-captured probe data on 2026-05-06 confirmed every state_key target
// has a 32-byte NUL-terminated path buffer at offset 0x2C. Sample paths
// across 8 distinct sprites:
//
//   0x2C: "s\\maps_WIN32\\fiesta_english.tga"
//   0x2C: "options\\maps\\glowbox_corner.tga"
//   0x2C: "\\frontend\\shell\\whitesprite.tga"
//   0x2C: "\\mainmenu\\maps\\wilburbottom.tga"  ... etc.
//
// The buffer holds the trailing portion of the original asset path
// (truncated from the front when the path is longer than 31 chars).
// vtable_at_+0 was 0 in every sample so the target object is a plain
// struct (not a C++-vtabled object) — the field offset is hard-coded by
// engine layout and stable.
//
// read_state_key_path() reads + validates that buffer with the same
// VirtualQuery + SEH guards as dump_all_to_csv(). Returns the byte
// length on success (NUL-terminator excluded), 0 on read failure or
// when the buffer doesn't contain printable ASCII.

size_t read_state_key_path(uint32_t state_key, char* out, size_t out_size) {
    constexpr ptrdiff_t kStateKeyPathOffset = 0x2C;
    constexpr size_t    kStateKeyPathSize   = 32;
    if (out_size == 0) return 0;
    out[0] = 0;

    char buf[kStateKeyPathSize] = {0};
    const size_t got = read_safe(static_cast<uintptr_t>(state_key) + kStateKeyPathOffset,
                                 buf, kStateKeyPathSize);
    if (got < 4) return 0;

    // Validate: printable ASCII run, terminated by NUL within the buffer
    // OR exactly fills the buffer (31 chars + no terminator is valid).
    size_t n = 0;
    while (n < got && buf[n]) {
        const unsigned char c = static_cast<unsigned char>(buf[n]);
        if (c < 0x20 || c >= 0x7F) return 0;
        ++n;
    }
    if (n < 4) return 0;
    if (n + 1 > out_size) n = out_size - 1;
    std::memcpy(out, buf, n);
    out[n] = 0;
    return n;
}

// Dump every currently-tracked slot's state_key target memory to CSV.
// Format per row:
//   state_key,name,group,vtable,bytes_at_+0..+0FF
//
// Where:
//   state_key = hex of slot's state_key (= the heap pointer we want to id)
//   name      = user label (manual, may be empty)
//   group     = user group
//   vtable    = first dword at *state_key (likely a vtable ptr if it's a C++ object)
//   bytes...  = next 256 bytes from the state_key pointer, hex-encoded.
//               Use the dump to find embedded name strings (look for printable
//               runs) at fixed offsets — that's m_pcName for Gamebryo-style
//               NiAVObject-derived classes.
//
// Also writes a "string scan" column: the longest printable C-string found
// in the dump at any offset. Helps spot the name field at a glance.
//
// Returns the number of slots dumped, or -1 on file open failure.
int dump_all_to_csv() {
    char path[MAX_PATH] = {0};
    if (!resolve_csv_path(path, sizeof(path))) return -1;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "w") != 0 || !fp) return -1;

    std::fprintf(fp, "state_key,name,group,frame_count,total_count,vtable_at_+0,"
                     "longest_string,longest_string_offset,bytes_at_+00..+FF\n");

    constexpr int kMax = 64;
    mtr::sprite_xform::SlotInfo slots[kMax]{};
    const int n = mtr::sprite_xform::snapshot_slots(slots, kMax);

    int dumped = 0;
    for (int i = 0; i < n; ++i) {
        const auto& s = slots[i];
        if (s.state_key == 0) continue;

        uint8_t bytes[256] = {0};
        const size_t got = read_safe(s.state_key, bytes, sizeof(bytes));

        uint32_t vtable = 0;
        if (got >= 4) std::memcpy(&vtable, bytes, 4);

        // Hex-encode the 256-byte dump (with '|' separators every 16 bytes
        // for visual reading).
        char hex[256 * 3 + 32] = {0};
        size_t hp = 0;
        for (size_t b = 0; b < got && hp + 4 < sizeof(hex); ++b) {
            std::snprintf(hex + hp, sizeof(hex) - hp, "%02X", bytes[b]);
            hp += 2;
            if ((b & 0xF) == 0xF && b + 1 < got) {
                hex[hp++] = '|';
            }
        }

        // Find the longest run of printable ASCII (>= 4 chars) in the
        // dump. That's almost certainly an embedded name/path string. Treat
        // bytes pointed-to as candidates too (common: pointer at +0x8 →
        // string elsewhere — chase one indirection).
        char longest[64] = {0};
        size_t longest_n = 0;
        size_t longest_off = 0;
        size_t cur_start = 0;
        size_t cur_n = 0;
        for (size_t b = 0; b < got; ++b) {
            const uint8_t c = bytes[b];
            const bool printable =
                (c >= 0x20 && c < 0x7F);
            if (printable) {
                if (cur_n == 0) cur_start = b;
                ++cur_n;
                if (cur_n > longest_n) {
                    longest_n   = cur_n;
                    longest_off = cur_start;
                }
            } else {
                cur_n = 0;
            }
        }
        if (longest_n >= 4) {
            const size_t copy_n = (longest_n < sizeof(longest) - 1) ? longest_n : sizeof(longest) - 1;
            std::memcpy(longest, &bytes[longest_off], copy_n);
            longest[copy_n] = 0;
        }

        // Also chase a few common "pointer fields" — read the dword at +4,
        // +8, +0xC, +0x10 and try to dereference them as strings. Spits
        // them out as `field_<offset>=<string>` if a string is found there,
        // appended after the longest_string column for diagnostic context.
        char chase[256] = {0};
        size_t cp = 0;
        for (int off : {4, 8, 0xC, 0x10, 0x14, 0x18, 0x1C}) {
            if (got < static_cast<size_t>(off + 4)) break;
            uint32_t field = 0;
            std::memcpy(&field, bytes + off, 4);
            if (field < 0x10000) continue;
            char str[80] = {0};
            if (read_cstring_safe(field, str, sizeof(str)) >= 4) {
                int written = std::snprintf(chase + cp, sizeof(chase) - cp,
                                            "+%X->%s ", off, str);
                if (written > 0) cp += static_cast<size_t>(written);
            }
        }

        // CSV-escape: replace commas in name/group/strings with semicolons,
        // quotes with apostrophes (cheap, no full RFC 4180).
        auto sanitize = [](char* s) {
            for (; *s; ++s) {
                if (*s == ',') *s = ';';
                if (*s == '"') *s = '\'';
                if (*s == '\n') *s = ' ';
                if (*s == '\r') *s = ' ';
            }
        };
        char name_s[sizeof(s.name)];
        char group_s[sizeof(s.group)];
        std::memcpy(name_s, s.name, sizeof(name_s));
        std::memcpy(group_s, s.group, sizeof(group_s));
        sanitize(name_s);
        sanitize(group_s);
        sanitize(longest);
        sanitize(chase);

        std::fprintf(fp, "0x%08X,%s,%s,%u,%llu,0x%08X,%s+%s,%zu,%s\n",
                     s.state_key, name_s, group_s,
                     s.frame_count,
                     static_cast<unsigned long long>(s.total_count),
                     vtable,
                     longest, chase, longest_off, hex);
        ++dumped;
    }

    std::fclose(fp);
    g_dump_count.fetch_add(1);
    mtr::log::info("state_key_probe: dumped %d slot(s) to %s", dumped, path);
    return dumped;
}

bool csv_path(char* out, size_t out_size) {
    return resolve_csv_path(out, out_size);
}

} // namespace mtr::state_key_probe
