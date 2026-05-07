// Direct screen-push for screens locked behind progression in retail Wilbur.
//
// The engine has a screen-stack manager whose method `sub_604310(this, name)`
// pushes a screen by name. Several names are interesting because the screen
// itself is fully implemented and the underlying functionality works -- the
// only reason a player can't see them is that the menu button to open them
// is gated by progression. Names verified present in retail strings:
//
//   ScreenCheats              -- cheat-code entry (5 slots)
//   ScreenWilburExtras        -- extras menu (gallery / unlocks hub)
//   ScreenWilburAFViewerMain  -- Action Figure viewer (collectibles)
//   ScreenWilburArtSelect     -- concept-art viewer
//   ScreenWilburMainMenu      -- main menu (sanity check; always reachable)
//
// `sub_604310` is __thiscall: ECX = screen-manager singleton, stack arg = name.
// The manager pointer isn't easy to reach from a static global in retail; but
// every screen transition during gameplay calls sub_604310 with the live
// pointer. We hook it, capture `this` once on first invocation, then issue
// our own calls with the captured pointer. No allocation, no fake state --
// the engine's own machinery is what does the work.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::screen_push {

namespace {

constexpr uintptr_t kScreenPushVA = 0x00604310;
constexpr uintptr_t kScreenRegisterVA = 0x0060E9F0; // sub_60E9F0(registry, name, ctor)
constexpr uintptr_t kScreenRegistryVA = 0x00744A80; // 0x744A80 — the static screen registry

// Screen pop wrapper. Static RE: sub_604C90(this) walks
//   this[6]+52 (the screen stack) → calls sub_6045F0(stack), which loops
// over the stack list and calls vtable[0](item, 1) (destructor) on each
// matching item. Strongest candidate for "pop" on the screen manager.
// Has 5 callers in unnamed regions (likely button-handler / state-machine
// code) — all wrapped via this single funnel.
//
// We hook it in best-effort mode: on every fire, decrement our mirror's
// depth. If the hook target is wrong we just see the existing
// staleness behavior (no regression). If it's right, current_top_name
// returns the actual current top after a back-out.
constexpr uintptr_t kScreenPopVA = 0x00604C90;

// __thiscall via __fastcall trick (consistent with d3d9_hook / console).
using PFN_ScreenPush     = char (__fastcall*)(void* this_, void* /*edx*/, const char* name);
using PFN_ScreenRegister = int  (__cdecl   *)(void* registry, const char* name, void* ctor);
using PFN_ScreenPop      = int  (__fastcall*)(void* this_, void* /*edx*/);

PFN_ScreenPush     g_orig_push     = nullptr;
PFN_ScreenRegister g_orig_register = nullptr;
PFN_ScreenPop      g_orig_pop      = nullptr;

std::atomic<void*> g_captured_manager{nullptr};
std::atomic<int>   g_capture_log_count{0};

// Screen stack mirror — push appends, pop decrements depth.
// Sized for the longest screen names observed at startup (e.g.
// "ScreenWilburAFViewerMain" = 24 chars + NUL). 16-deep is plenty for
// menu nesting (typical depth observed: 3-4).
constexpr size_t kNameBufSize = 64;
constexpr int    kMaxStackDepth = 16;
char  g_screen_stack[kMaxStackDepth][kNameBufSize] = {{0}};
int   g_screen_stack_depth = 0;
std::mutex g_stack_mu;

char __fastcall hk_screen_push(void* this_, void* /*edx*/, const char* name) {
    if (this_ && !g_captured_manager.load()) {
        g_captured_manager.store(this_);
        if (g_capture_log_count.fetch_add(1) < 1) {
            mtr::log::info("screen_push: captured screen-manager this=%p (first call name=\"%s\")",
                           this_, name ? name : "(null)");
        }
    }
    char rc = g_orig_push(this_, nullptr, name);
    // Only mirror successful pushes (rc=1). Failed pushes (rc=0, e.g. screen
    // not registered for current state) shouldn't displace our "current top"
    // approximation.
    if (rc != 0 && name) {
        std::scoped_lock lk(g_stack_mu);
        if (g_screen_stack_depth < kMaxStackDepth) {
            std::strncpy(g_screen_stack[g_screen_stack_depth], name, kNameBufSize - 1);
            g_screen_stack[g_screen_stack_depth][kNameBufSize - 1] = 0;
            ++g_screen_stack_depth;
        }
    }
    return rc;
}

// Pop hook — decrement mirror depth on each fire. Best-effort: we don't
// know the popped screen's name from sub_604C90's signature (it operates
// on the stack head implicitly), so we just trust that our last-pushed
// is what gets popped. If the engine pops something other than the top
// of our mirror, depth tracking goes wrong, but the worst case is the
// mirror returns an outdated name — same as the pre-pop-hook behavior.
int __fastcall hk_screen_pop(void* this_, void* /*edx*/) {
    int rc = g_orig_pop(this_, nullptr);
    {
        std::scoped_lock lk(g_stack_mu);
        if (g_screen_stack_depth > 0) {
            --g_screen_stack_depth;
            // Zero out the popped slot so a leak doesn't leave a stale
            // name visible in current_top_name if the depth ever wraps.
            g_screen_stack[g_screen_stack_depth][0] = 0;
        }
    }
    return rc;
}

int __cdecl hk_screen_register(void* registry, const char* name, void* ctor) {
    // Log registry adds. Restrict to the static screen registry at 0x744A80
    // to avoid spamming for other registries that share this allocator.
    if (registry == reinterpret_cast<void*>(kScreenRegistryVA)) {
        mtr::log::info("screen_register: name=\"%s\" ctor=%p",
                       name ? name : "(null)", ctor);
    }
    return g_orig_register(registry, name, ctor);
}

} // namespace

bool install() {
    void* p = reinterpret_cast<void*>(kScreenPushVA);
    if (MH_CreateHook(p, &hk_screen_push,
                      reinterpret_cast<void**>(&g_orig_push)) != MH_OK) {
        mtr::log::info("screen_push: MH_CreateHook(%p) failed", p);
        return false;
    }
    if (MH_EnableHook(p) != MH_OK) {
        mtr::log::info("screen_push: MH_EnableHook failed");
        return false;
    }
    mtr::log::info("screen_push: hook armed at %p", p);

    void* preg = reinterpret_cast<void*>(kScreenRegisterVA);
    if (MH_CreateHook(preg, &hk_screen_register,
                      reinterpret_cast<void**>(&g_orig_register)) != MH_OK) {
        mtr::log::info("screen_register: MH_CreateHook(%p) failed", preg);
    } else if (MH_EnableHook(preg) != MH_OK) {
        mtr::log::info("screen_register: MH_EnableHook failed");
    } else {
        mtr::log::info("screen_register: hook armed at %p (filter: registry=0x%p)",
                       preg, (void*)kScreenRegistryVA);
    }

    void* ppop = reinterpret_cast<void*>(kScreenPopVA);
    if (MH_CreateHook(ppop, &hk_screen_pop,
                      reinterpret_cast<void**>(&g_orig_pop)) != MH_OK) {
        mtr::log::info("screen_pop: MH_CreateHook(%p) failed", ppop);
    } else if (MH_EnableHook(ppop) != MH_OK) {
        mtr::log::info("screen_pop: MH_EnableHook failed");
    } else {
        mtr::log::info("screen_pop: hook armed at %p (sub_604C90)", ppop);
    }

    return true;
}

bool ready() { return g_captured_manager.load() != nullptr; }

bool push(const char* name) {
    void* mgr = g_captured_manager.load();
    if (!mgr || !name) return false;
    char rc = g_orig_push(mgr, nullptr, name);
    mtr::log::info("screen_push: push(\"%s\") -> rc=%d (this=%p)", name, (int)rc, mgr);
    return rc != 0;
}

// Copy the current top screen name into `out` (NUL-terminated, truncated to
// out_size-1). Returns true if the mirror has at least one screen.
//
// Tracks pushes via screen_manager_push_by_name and pops via sub_604C90
// (best-effort — see hk_screen_pop). When depth==0 (the early startup
// splash, before any screen has been pushed), returns the synthetic
// name "<startup>" so:
//   - per-screen aspect rules can match the splash via pattern "startup"
//   - sprite_xform's screen_context bucket gets a stable hash for that
//     state instead of hashing an empty string
//   - the menu's status panel shows "<startup>" instead of "(empty)"
// The synthetic name is reported as a successful read (returns true) so
// downstream code treats it like any other screen.
bool current_top_name(char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    std::scoped_lock lk(g_stack_mu);
    if (g_screen_stack_depth <= 0) {
        std::strncpy(out, "<startup>", out_size - 1);
        out[out_size - 1] = 0;
        return true;
    }
    const char* top = g_screen_stack[g_screen_stack_depth - 1];
    if (top[0] == 0) {
        std::strncpy(out, "<startup>", out_size - 1);
        out[out_size - 1] = 0;
        return true;
    }
    std::strncpy(out, top, out_size - 1);
    out[out_size - 1] = 0;
    return true;
}

// Diagnostic accessor — depth + flat list of names for menu inspection.
int stack_depth() {
    std::scoped_lock lk(g_stack_mu);
    return g_screen_stack_depth;
}

bool stack_at(int idx, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    std::scoped_lock lk(g_stack_mu);
    if (idx < 0 || idx >= g_screen_stack_depth) {
        out[0] = 0;
        return false;
    }
    std::strncpy(out, g_screen_stack[idx], out_size - 1);
    out[out_size - 1] = 0;
    return true;
}

} // namespace mtr::screen_push
