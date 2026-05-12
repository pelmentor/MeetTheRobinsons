// Per-screen UI aspect rules.
//
// The engine renders all 2D UI (HUD, menus, dialogs, loading screens) under a
// single ortho-projection set up once per frame in sub_4A9CE0 -> sub_562B70.
// To make aspect choice GRANULAR per UI context, we let the user define a
// rules table: each rule is `{ pattern, aspect }`. At ortho-build time, we
// look up the current top screen name and apply the first rule whose pattern
// matches (case-insensitive substring). If no rule matches, the default
// fallback aspect applies — which is "no override" unless the user sets it.
//
// Pattern matching is intentionally simple (substring, case-insensitive) so
// rules are obvious to read. Examples:
//
//   pattern         aspect
//   --------        ------
//   PauseMenu       1.333    -> any screen named "*PauseMenu*" pillarboxed at 4:3
//   MainMenu        1.333    -> main menu at 4:3
//   Loading         1.778    -> loading screens at 16:9 (engine native widescreen)
//   Hud             0.0      -> 0.0 == "no override" (HUD stretches with screen)
//
// Storage is fixed-size (kMaxRules) — keeps it simple, no allocation games.

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mtr     { HMODULE self_module(); }
namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::sprite_matrix {
    bool enabled();        void set_enabled(bool v);
    bool auto_from_rules(); void set_auto_from_rules(bool v);
    float pos_offset_x();  void set_pos_offset_x(float v);
    float pos_offset_y();  void set_pos_offset_y(float v);
}
namespace mtr::sprite_xform {
    int  save_count();
    bool save_at(int i, uint32_t* out_state_key,
                 float* out_ox, float* out_oy,
                 float* out_sx, float* out_sy,
                 bool* out_hidden,
                 char* out_name,  size_t out_name_sz,
                 char* out_group, size_t out_group_sz);
    bool save_at_full(int i, uint32_t* out_state_key,
                      uint16_t* out_uv_bucket,
                      uint8_t*  out_screen_context,
                      uint8_t*  out_bbox_quadrant,
                      uint16_t* out_sort_key,
                      float* out_ox, float* out_oy,
                      float* out_sx, float* out_sy,
                      bool* out_hidden,
                      char* out_name,  size_t out_name_sz,
                      char* out_group, size_t out_group_sz,
                      char* out_path,  size_t out_path_sz,
                      char* out_widget_name, size_t out_widget_name_sz);
    void add_pending_by_widget_name(const char* widget_name,
                                    float ox, float oy,
                                    float sx, float sy, bool hidden,
                                    const char* name, const char* group);
    void load_apply(uint32_t state_key, float ox, float oy,
                    float sx, float sy, bool hidden,
                    const char* name, const char* group);
    void load_apply_full(uint32_t state_key,
                         uint16_t uv_bucket,
                         uint8_t  screen_context,
                         uint8_t  bbox_quadrant,
                         uint16_t sort_key,
                         float ox, float oy,
                         float sx, float sy, bool hidden,
                         const char* name, const char* group);
    void add_pending_by_path(const char* path,
                             uint16_t uv_bucket,
                             uint8_t  screen_context,
                             uint8_t  bbox_quadrant,
                             uint16_t sort_key,
                             float ox, float oy,
                             float sx, float sy, bool hidden,
                             const char* name, const char* group);
}

namespace mtr::ui_aspect_rules {

void save_to_ini();
bool load_from_ini();
void request_save();
void flush_pending_save();

namespace {

// kMaxRules sized to comfortably fit every screen the engine registers at
// boot (~57 in retail, observed via screen_register hook 2026-05-09) plus
// headroom for user-added patterns. Bumping this is cheap (zero-init
// storage, fixed-stride array).
constexpr size_t kMaxRules     = 96;
constexpr size_t kPatternBytes = 48;

struct Rule {
    char  pattern[kPatternBytes];   // empty == disabled slot
    float aspect;                   // 0.0 == "no override" (use default)
};

Rule        g_rules[kMaxRules] = {};
size_t      g_rule_count       = 0;
float       g_default_aspect   = 0.0f;     // 0 = no default override
std::mutex  g_mu;

// Debounced save state. UI sites call request_save() (cheap — just a flag);
// flush_pending_save() actually writes the ini. The flush runs once per
// menu draw if dirty AND ≥kSaveDebounceMs since the last flush. Net effect:
// dragging a slider for 5 s ends with one ini write at release, not 60+.
//
// Why we need this: save_to_ini() does ~7 WritePrivateProfileStringA calls
// per sprite_xform slot. With ~20 user-labelled slots that's ~140 file
// open/parse/write/close operations per save. Called from
// IsItemDeactivatedAfterEdit at the end of every drag was the source of
// the user-reported "micro lag when moving UI elements" on 2026-05-06.
std::atomic<bool>  g_save_dirty{false};
std::atomic<DWORD> g_last_save_tick{0};
// 1500 ms = comfortably longer than a typical interactive flurry (drag,
// type-a-name, click another field). Combined with the single-pass save
// (saves now ~5 ms instead of ~500 ms), the user never sees a save-in-
// progress hitch — even multi-second editing sequences flush exactly once
// after the user stops touching anything.
constexpr DWORD    kSaveDebounceMs = 1500;

bool icase_substr(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t hl = std::strlen(hay);
    const size_t nl = std::strlen(needle);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; ++i) {
        size_t j = 0;
        for (; j < nl; ++j) {
            const int a = std::tolower(static_cast<unsigned char>(hay[i + j]));
            const int b = std::tolower(static_cast<unsigned char>(needle[j]));
            if (a != b) break;
        }
        if (j == nl) return true;
    }
    return false;
}

} // namespace

size_t max_rules()   { return kMaxRules; }
size_t rule_count()  { std::scoped_lock lk(g_mu); return g_rule_count; }

bool get_rule(size_t idx, char* out_pattern, size_t pattern_size, float* out_aspect) {
    std::scoped_lock lk(g_mu);
    if (idx >= g_rule_count) return false;
    if (out_pattern && pattern_size > 0) {
        std::strncpy(out_pattern, g_rules[idx].pattern, pattern_size - 1);
        out_pattern[pattern_size - 1] = 0;
    }
    if (out_aspect) *out_aspect = g_rules[idx].aspect;
    return true;
}

void set_rule(size_t idx, const char* pattern, float aspect) {
    std::scoped_lock lk(g_mu);
    if (idx >= kMaxRules) return;
    if (idx >= g_rule_count) g_rule_count = idx + 1;
    if (pattern) {
        std::strncpy(g_rules[idx].pattern, pattern, kPatternBytes - 1);
        g_rules[idx].pattern[kPatternBytes - 1] = 0;
    } else {
        g_rules[idx].pattern[0] = 0;
    }
    g_rules[idx].aspect = aspect;
}

void add_rule(const char* pattern, float aspect) {
    std::scoped_lock lk(g_mu);
    if (g_rule_count >= kMaxRules) return;
    Rule& r = g_rules[g_rule_count++];
    if (pattern) {
        std::strncpy(r.pattern, pattern, kPatternBytes - 1);
        r.pattern[kPatternBytes - 1] = 0;
    } else {
        r.pattern[0] = 0;
    }
    r.aspect = aspect;
}

void remove_rule(size_t idx) {
    std::scoped_lock lk(g_mu);
    if (idx >= g_rule_count) return;
    for (size_t i = idx; i + 1 < g_rule_count; ++i) {
        g_rules[i] = g_rules[i + 1];
    }
    g_rule_count--;
    g_rules[g_rule_count].pattern[0] = 0;
    g_rules[g_rule_count].aspect = 0.0f;
}

float default_aspect()             { return g_default_aspect; }
void  set_default_aspect(float a)  { g_default_aspect = a; }

// Auto-sync flag: when on, sync_from_registry() pulls every name from
// screen_push's registered list into the rules table on each menu draw.
// Default OFF — manual "+ Add ALL N known screens" button is the
// authoritative way to bulk-populate. Auto-sync default ON was wrong:
// it added rules behind the user's back on every menu open, defeating
// "Clear all" and creating a save-spike each time.
std::atomic<bool> g_auto_sync_screens{false};

void clear_all() {
    {
        std::scoped_lock lk(g_mu);
        for (size_t i = 0; i < kMaxRules; ++i) {
            g_rules[i].pattern[0] = 0;
            g_rules[i].aspect     = 0.0f;
        }
        g_rule_count = 0;
    }
    // RULE №1 fix: if auto-sync were left enabled, every menu draw frame
    // would re-add the registry contents and the user's "Clear all" would
    // be silently undone. Disable auto-sync atomically with the clear so
    // the cleared state actually persists. User can re-enable via the
    // Auto-track checkbox.
    g_auto_sync_screens.store(false);
}

bool auto_sync_screens()         { return g_auto_sync_screens.load(); }
void set_auto_sync_screens(bool v){ g_auto_sync_screens.store(v); }

} // namespace mtr::ui_aspect_rules

// Bridge into screen_push (captures both class-registry names and runtime
// push instance names; see screen_push.cpp). Forward-declared here so we
// don't pull a header for one tiny call site.
namespace mtr::screen_push {
    int  registered_count();
    bool registered_at(int idx, char* out, size_t out_size);
}

namespace mtr::ui_aspect_rules {

// Pull every name from screen_push::registered_* into the rules table,
// skipping duplicates. Returns the number of newly-added rules. Cheap to
// call every frame: typical N≈60, M≈60, so ~3600 strcmp ops/frame.
int sync_from_registry() {
    const int reg_n = mtr::screen_push::registered_count();
    if (reg_n <= 0) return 0;
    int added = 0;
    std::scoped_lock lk(g_mu);
    for (int i = 0; i < reg_n; ++i) {
        char rname[kPatternBytes] = {0};
        if (!mtr::screen_push::registered_at(i, rname, sizeof(rname))) continue;
        if (rname[0] == 0) continue;
        bool dup = false;
        for (size_t r = 0; r < g_rule_count; ++r) {
            if (std::strcmp(g_rules[r].pattern, rname) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (g_rule_count >= kMaxRules) break;
        Rule& nr = g_rules[g_rule_count++];
        std::strncpy(nr.pattern, rname, kPatternBytes - 1);
        nr.pattern[kPatternBytes - 1] = 0;
        nr.aspect = 4.0f / 3.0f;
        ++added;
    }
    return added;
}

// Seed the rule table with sensible defaults for retail Wilbur. Substring
// patterns are picked from screen-name strings observed in the engine
// (`ScreenWilburMainMenu`, `ScreenPause`, `ScreenLoading`, etc.). All seeded
// rules target 4:3 (1.333) — that's the engine's authored aspect for menus
// and overlays, and the right pillarbox target on widescreen displays.
//
// First tries to load existing user state from `mtr-asi-ui.ini` next to the
// ASI. If no ini exists, seeds defaults and persists them so the user gets
// a baseline file to edit. Idempotent on re-call: skips if rules already
// loaded.
void install_defaults() {
    {
        std::scoped_lock lk(g_mu);
        if (g_rule_count != 0) return;
    }
    if (load_from_ini()) {
        return;
    }
    constexpr float kTarget43 = 4.0f / 3.0f;
    static const char* const kPatterns[] = {
        "MainMenu",      // ScreenWilburMainMenu
        "Pause",         // ScreenPause / ScreenWilburPause
        "Loading",       // ScreenLoading
        "Options",       // ScreenOptions / ScreenWilburOptions
        "Help",          // ScreenHelp
        "Inventory",     // ScreenInventory
        "Map",           // ScreenMap
        "Cheats",        // ScreenCheats
        "Title",         // ScreenTitle
        "Background",    // ScreenBackground
        "MiniHamster",   // mini-game
        "DigDug",        // mini-game
        "ChargeBall",    // mini-game
        "AFViewer",      // ScreenWilburAFViewerMain
    };
    for (const char* p : kPatterns) {
        add_rule(p, kTarget43);
    }
    save_to_ini();
}

// Resolve aspect for the given top-screen name. Returns:
//   > 0.0  : aspect override to apply
//   <= 0.0 : no override; caller should leave the ortho untouched
//
// First matching rule wins. If no rule matches, falls back to default.
float resolve_aspect(const char* top_name) {
    std::scoped_lock lk(g_mu);
    if (top_name && top_name[0]) {
        for (size_t i = 0; i < g_rule_count; ++i) {
            if (g_rules[i].pattern[0] && icase_substr(top_name, g_rules[i].pattern)) {
                return g_rules[i].aspect;
            }
        }
    }
    return g_default_aspect;
}

// Like resolve_aspect, but also returns the matched pattern so the UI can
// show "rule X matched". On match: returns true, fills out_pattern + out_aspect.
// On no match: returns false, fills out_pattern with "" and out_aspect with the
// default fallback (so caller still sees what's being applied).
bool resolve_match(const char* top_name,
                   char* out_pattern, size_t pattern_size,
                   float* out_aspect) {
    std::scoped_lock lk(g_mu);
    if (out_pattern && pattern_size > 0) out_pattern[0] = 0;
    if (out_aspect) *out_aspect = g_default_aspect;
    if (!top_name || !top_name[0]) return false;
    for (size_t i = 0; i < g_rule_count; ++i) {
        if (g_rules[i].pattern[0] && icase_substr(top_name, g_rules[i].pattern)) {
            if (out_pattern && pattern_size > 0) {
                std::strncpy(out_pattern, g_rules[i].pattern, pattern_size - 1);
                out_pattern[pattern_size - 1] = 0;
            }
            if (out_aspect) *out_aspect = g_rules[i].aspect;
            return true;
        }
    }
    return false;
}

// === Persistence ============================================================
//
// Rules + sprite_matrix toggle state are persisted to `mtr-asi-ui.ini` next to
// the ASI module (Game/mtr-asi-ui.ini). Format is plain Win32 INI, two
// sections: [ui_aspect_rules] (table) and [sprite_matrix] (toggles).
//
// Save policy: structural changes (add / remove / clear / install_defaults +
// load_from_ini) auto-save. Slider edits (set_rule per-frame from the UI)
// don't auto-save — those persist on explicit "Save" button press, to avoid
// disk writes during slider drags.

namespace {

bool resolve_ini_path(char* out, size_t out_size) {
    if (!out || out_size < MAX_PATH) return false;
    HMODULE self = mtr::self_module();
    char modpath[MAX_PATH] = {0};
    DWORD got = GetModuleFileNameA(self, modpath, sizeof(modpath));
    if (got == 0 || got >= sizeof(modpath)) return false;
    // Strip filename, keep trailing slash; append "mtr-asi-ui.ini".
    char* slash = std::strrchr(modpath, '\\');
    if (!slash) slash = std::strrchr(modpath, '/');
    if (!slash) return false;
    *(slash + 1) = 0;
    int n = std::snprintf(out, out_size, "%smtr-asi-ui.ini", modpath);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

} // namespace

// Single-pass save: open the file once, fprintf every section + key in one
// shot, fclose. Replaces ~540 WritePrivateProfileStringA calls (each opens,
// parses, edits, and rewrites the entire INI in-place) with one open / one
// write / one close — typically 50-100x faster on a file this size. The
// FPS drop the user experienced ("30 FPS during apply / save") was the
// CRT WritePrivateProfile path doing N² work on the file size with each
// call. Re-readable by GetPrivateProfileStringA on load — same INI format.
void save_to_ini() {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return;

    // Snapshot the rule table under the lock so we don't read torn state.
    Rule  snap[kMaxRules];
    size_t snap_count = 0;
    float snap_default = 0.0f;
    {
        std::scoped_lock lk(g_mu);
        snap_count   = g_rule_count;
        snap_default = g_default_aspect;
        for (size_t i = 0; i < snap_count; ++i) snap[i] = g_rules[i];
    }
    const bool auto_sync = g_auto_sync_screens.load();

    FILE* fp = nullptr;
    if (fopen_s(&fp, ini, "wb") != 0 || !fp) {
        mtr::log::info("ui_aspect_rules: save_to_ini fopen(%s) failed", ini);
        return;
    }

    std::fprintf(fp, "[ui_aspect_rules]\r\n");
    std::fprintf(fp, "count=%zu\r\n", snap_count);
    std::fprintf(fp, "default_aspect=%.6f\r\n", snap_default);
    std::fprintf(fp, "auto_sync_screens=%d\r\n", auto_sync ? 1 : 0);
    for (size_t i = 0; i < snap_count; ++i) {
        std::fprintf(fp, "rule_%zu_pattern=%s\r\n", i, snap[i].pattern);
        std::fprintf(fp, "rule_%zu_aspect=%.6f\r\n",  i, snap[i].aspect);
    }

    std::fprintf(fp, "\r\n[sprite_matrix]\r\n");
    std::fprintf(fp, "enabled=%d\r\n",        mtr::sprite_matrix::enabled() ? 1 : 0);
    std::fprintf(fp, "auto_from_rules=%d\r\n",mtr::sprite_matrix::auto_from_rules() ? 1 : 0);
    std::fprintf(fp, "pos_offset_x=%.6f\r\n", mtr::sprite_matrix::pos_offset_x());
    std::fprintf(fp, "pos_offset_y=%.6f\r\n", mtr::sprite_matrix::pos_offset_y());

    // Persist sprite_xform per-slot transforms. v2 schema includes the
    // composite-key components (uv_bucket / screen_context / bbox_quadrant
    // / sort_key). Slots with all-wildcard patterns load as wildcard.
    const int xn = mtr::sprite_xform::save_count();
    std::fprintf(fp, "\r\n[sprite_xform]\r\n");
    std::fprintf(fp, "count=%d\r\n", xn);
    for (int i = 0; i < xn; ++i) {
        uint32_t sk = 0;
        uint16_t uvb = 0xFFFF;
        uint8_t  sctx = 0xFF;
        uint8_t  quad = 0xFF;
        uint16_t sortk = 0xFFFF;
        float ox = 0, oy = 0, sx = 1, sy = 1;
        bool hidden = false;
        char name[48]        = {0};
        char group[32]       = {0};
        char path[48]        = {0};
        char widget_name[48] = {0};
        if (!mtr::sprite_xform::save_at_full(i, &sk, &uvb, &sctx, &quad, &sortk,
                                             &ox, &oy, &sx, &sy, &hidden,
                                             name, sizeof(name),
                                             group, sizeof(group),
                                             path, sizeof(path),
                                             widget_name, sizeof(widget_name))) continue;
        std::fprintf(fp, "x_%d_state_key=0x%08X\r\n",      i, sk);
        std::fprintf(fp, "x_%d_path=%s\r\n",               i, path);
        std::fprintf(fp, "x_%d_uv_bucket=0x%04X\r\n",      i, uvb);
        std::fprintf(fp, "x_%d_screen_context=0x%02X\r\n", i, sctx);
        std::fprintf(fp, "x_%d_bbox_quadrant=0x%02X\r\n",  i, quad);
        std::fprintf(fp, "x_%d_sort_key=0x%04X\r\n",       i, sortk);
        std::fprintf(fp, "x_%d_name=%s\r\n",               i, name);
        std::fprintf(fp, "x_%d_group=%s\r\n",              i, group);
        std::fprintf(fp, "x_%d_offset_x=%.6f\r\n",         i, ox);
        std::fprintf(fp, "x_%d_offset_y=%.6f\r\n",         i, oy);
        std::fprintf(fp, "x_%d_scale_x=%.6f\r\n",          i, sx);
        std::fprintf(fp, "x_%d_scale_y=%.6f\r\n",          i, sy);
        std::fprintf(fp, "x_%d_hidden=%d\r\n",             i, hidden ? 1 : 0);
        std::fprintf(fp, "x_%d_widget_name=%s\r\n",        i, widget_name);
    }

    std::fclose(fp);

    mtr::log::info("ui_aspect_rules: saved %zu rule(s) + %d xform(s) to %s",
                   snap_count, xn, ini);
    g_save_dirty.store(false);
    g_last_save_tick.store(GetTickCount());
}

// Mark the ini state dirty. Cheap — just an atomic store. The actual write
// happens later via flush_pending_save (called once per menu draw frame).
// Use this at every UI site that previously called save_to_ini() directly.
void request_save() {
    g_save_dirty.store(true);
}

// If a save was requested AND ≥ kSaveDebounceMs has elapsed since the last
// write, do the actual ini write now. Called from the menu draw path each
// frame and from the DLL-detach fence (with a force=true variant — see
// below). Cheap-and-safe to call every frame.
void flush_pending_save() {
    if (!g_save_dirty.load()) return;
    const DWORD now  = GetTickCount();
    const DWORD prev = g_last_save_tick.load();
    if (prev != 0 && (now - prev) < kSaveDebounceMs) return;
    save_to_ini();  // clears dirty + updates last_save_tick
}

// Returns true if the ini file existed and was loaded (rules table populated
// from disk). Returns false if no ini, in which case caller should fall back
// to install_defaults's seeded set.
bool load_from_ini() {
    char ini[MAX_PATH] = {0};
    if (!resolve_ini_path(ini, sizeof(ini))) return false;
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return false;

    char buf[128];
    GetPrivateProfileStringA("ui_aspect_rules", "count", "-1",
                             buf, sizeof(buf), ini);
    int count = std::atoi(buf);
    if (count < 0) return false;
    if (count > static_cast<int>(kMaxRules)) count = static_cast<int>(kMaxRules);

    GetPrivateProfileStringA("ui_aspect_rules", "default_aspect", "0.0",
                             buf, sizeof(buf), ini);
    float def = static_cast<float>(std::atof(buf));

    // Default OFF when key absent — explicit opt-in only.
    GetPrivateProfileStringA("ui_aspect_rules", "auto_sync_screens", "0",
                             buf, sizeof(buf), ini);
    g_auto_sync_screens.store(std::atoi(buf) != 0);

    {
        std::scoped_lock lk(g_mu);
        for (size_t i = 0; i < kMaxRules; ++i) {
            g_rules[i].pattern[0] = 0;
            g_rules[i].aspect     = 0.0f;
        }
        g_rule_count     = 0;
        g_default_aspect = def;
        for (int i = 0; i < count; ++i) {
            char keyp[32], keya[32];
            std::snprintf(keyp, sizeof(keyp), "rule_%d_pattern", i);
            std::snprintf(keya, sizeof(keya), "rule_%d_aspect",  i);
            char patbuf[kPatternBytes] = {0};
            GetPrivateProfileStringA("ui_aspect_rules", keyp, "",
                                     patbuf, sizeof(patbuf), ini);
            GetPrivateProfileStringA("ui_aspect_rules", keya, "0.0",
                                     buf, sizeof(buf), ini);
            std::strncpy(g_rules[i].pattern, patbuf, kPatternBytes - 1);
            g_rules[i].pattern[kPatternBytes - 1] = 0;
            g_rules[i].aspect = static_cast<float>(std::atof(buf));
        }
        g_rule_count = static_cast<size_t>(count);
    }

    // Restore sprite_matrix toggles. Default to off if keys missing — matches
    // pre-persistence behavior.
    GetPrivateProfileStringA("sprite_matrix", "enabled", "0",
                             buf, sizeof(buf), ini);
    mtr::sprite_matrix::set_enabled(std::atoi(buf) != 0);
    GetPrivateProfileStringA("sprite_matrix", "auto_from_rules", "0",
                             buf, sizeof(buf), ini);
    mtr::sprite_matrix::set_auto_from_rules(std::atoi(buf) != 0);
    GetPrivateProfileStringA("sprite_matrix", "pos_offset_x", "0.0",
                             buf, sizeof(buf), ini);
    mtr::sprite_matrix::set_pos_offset_x(static_cast<float>(std::atof(buf)));
    GetPrivateProfileStringA("sprite_matrix", "pos_offset_y", "0.0",
                             buf, sizeof(buf), ini);
    mtr::sprite_matrix::set_pos_offset_y(static_cast<float>(std::atof(buf)));

    // Load per-slot transforms (best-effort across sessions). v2 schema
    // includes uv_bucket / screen_context / bbox_quadrant; v1 ini files
    // omit those keys, so the default values 0xFFFF / 0xFF / 0xFF mean
    // "wildcard" — matches v1 behavior exactly (one wildcard slot per
    // state_key).
    GetPrivateProfileStringA("sprite_xform", "count", "0",
                             buf, sizeof(buf), ini);
    int xn = std::atoi(buf);
    if (xn < 0) xn = 0;
    for (int i = 0; i < xn; ++i) {
        char keybuf[32];
        uint32_t sk = 0;
        uint16_t uvb = 0xFFFF;
        uint8_t  sctx = 0xFF;
        uint8_t  quad = 0xFF;
        uint16_t sortk = 0xFFFF;
        float ox = 0, oy = 0, sx = 1, sy = 1;
        bool hidden = false;
        char name[48]        = {0};
        char group[32]       = {0};
        char path[48]        = {0};
        char widget_name[48] = {0};
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_state_key", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0",
                                 buf, sizeof(buf), ini);
        sk = static_cast<uint32_t>(std::strtoul(buf, nullptr, 0));
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_path", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "",
                                 path, sizeof(path), ini);
        // Skip empty/zero entries (no path AND no state_key — useless).
        if (path[0] == 0 && sk == 0) continue;
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_uv_bucket", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0xFFFF",
                                 buf, sizeof(buf), ini);
        uvb = static_cast<uint16_t>(std::strtoul(buf, nullptr, 0) & 0xFFFF);
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_screen_context", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0xFF",
                                 buf, sizeof(buf), ini);
        sctx = static_cast<uint8_t>(std::strtoul(buf, nullptr, 0) & 0xFF);
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_bbox_quadrant", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0xFF",
                                 buf, sizeof(buf), ini);
        quad = static_cast<uint8_t>(std::strtoul(buf, nullptr, 0) & 0xFF);
        // Default 0xFFFF = wildcard. Pre-sort_key ini entries (no key) load
        // as wildcard, matching legacy behaviour exactly.
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_sort_key", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0xFFFF",
                                 buf, sizeof(buf), ini);
        sortk = static_cast<uint16_t>(std::strtoul(buf, nullptr, 0) & 0xFFFF);
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_name", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "",
                                 name, sizeof(name), ini);
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_group", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "",
                                 group, sizeof(group), ini);
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_offset_x", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0.0",
                                 buf, sizeof(buf), ini);
        ox = static_cast<float>(std::atof(buf));
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_offset_y", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0.0",
                                 buf, sizeof(buf), ini);
        oy = static_cast<float>(std::atof(buf));
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_scale_x", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "1.0",
                                 buf, sizeof(buf), ini);
        sx = static_cast<float>(std::atof(buf));
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_scale_y", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "1.0",
                                 buf, sizeof(buf), ini);
        sy = static_cast<float>(std::atof(buf));
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_hidden", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "0",
                                 buf, sizeof(buf), ini);
        hidden = std::atoi(buf) != 0;
        std::snprintf(keybuf, sizeof(keybuf), "x_%d_widget_name", i);
        GetPrivateProfileStringA("sprite_xform", keybuf, "",
                                 widget_name, sizeof(widget_name), ini);

        // Phase 3 (2026-05-09): prefer widget_name-based matching when
        // a widget_name was recorded — engine-level identity, robust
        // across sessions AND across atlas-sharing widgets that path
        // alone can't disambiguate. Falls back to path-based matching
        // (Phase D.5 robust-against-ASLR), then to raw state_key
        // matching (least robust, only for entries with neither name
        // nor path).
        if (widget_name[0]) {
            mtr::sprite_xform::add_pending_by_widget_name(widget_name,
                                                          ox, oy, sx, sy, hidden,
                                                          name, group);
        } else if (path[0]) {
            mtr::sprite_xform::add_pending_by_path(path, uvb, sctx, quad, sortk,
                                                    ox, oy, sx, sy, hidden,
                                                    name, group);
        } else {
            mtr::sprite_xform::load_apply_full(sk, uvb, sctx, quad, sortk,
                                               ox, oy, sx, sy, hidden,
                                               name, group);
        }
    }

    mtr::log::info("ui_aspect_rules: loaded %d rule(s) + %d xform(s) from %s",
                   count, xn, ini);
    return true;
}

} // namespace mtr::ui_aspect_rules
