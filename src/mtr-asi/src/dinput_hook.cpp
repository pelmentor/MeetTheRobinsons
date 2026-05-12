// DirectInput8 hook + virtual cursor.
//
// Two responsibilities:
//
//  1. Input suppression. The game uses DI8 in exclusive-foreground for
//     keyboard + mouse, bypassing the WndProc message stream. While
//     freecam / menu / console is up we don't want gameplay input
//     bleeding through, so the GetDeviceState hook zeroes the returned
//     buffer. Freecam's own controls use GetAsyncKeyState / GetCursorPos
//     (independent of DI8), so they still work.
//
//  2. Virtual cursor. Under DI-EXCLUSIVE on a mouse, Windows pins the OS
//     cursor — GetCursorPos returns a frozen value, WM_MOUSEMOVE isn't
//     routed, MSLLHOOKSTRUCT.pt reflects the pin. We confirmed this on
//     this build's runtime: SetCooperativeLevel(NONEXCLUSIVE) returns
//     DI_OK but the OS-level pin doesn't release. So instead of fighting
//     Windows + dinput8.dll for cursor control, we drive an in-process
//     virtual cursor from DI's relative deltas (DIMOUSESTATE.lX/lY/lZ +
//     rgbButtons) — which are the canonical input source DI EXISTS to
//     deliver, regardless of pin state. menu.cpp feeds ImGui from this
//     virt cursor when the UI is interactive. Same architecture used by
//     ReShade, NVIDIA Overlay, MSI Afterburner — standard for in-game
//     mod menus that coexist with exclusive-mouse engines.
//
// Hook strategy: load dinput8.dll, create our own dummy IDirectInput8A
// (game uses A, not W — verified via GUID byte pattern in IDB), read
// vtable[9] (GetDeviceState), MinHook the function pointer. MinHook
// patches the function prologue so the override fires for ALL device
// instances created by the same dinput8.dll module. Same approach for
// the IDirectInput8::CreateDevice slot used to capture the game's mouse
// device pointer.

#include <windows.h>
#include <dinput.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace mtr::log     { void info(const char* fmt, ...); }
namespace mtr::freecam { bool active(); }
namespace mtr::menu    { bool is_visible(); }
namespace mtr::console { bool is_visible(); }
namespace mtr::screen_push { bool current_top_name(char* out, size_t out_size); }

namespace mtr::dinput_hook {

namespace {

using PFN_GetDeviceState     = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using PFN_SetCooperativeLevel = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, HWND, DWORD);

PFN_GetDeviceState      g_orig_GetDeviceState      = nullptr;
PFN_SetCooperativeLevel g_orig_SetCooperativeLevel = nullptr;

// Companion to windowmode's WM_ACTIVATEAPP swallow: engine acquires DI
// with DISCL_EXCLUSIVE | DISCL_FOREGROUND. We need the device to deliver
// input regardless of window foreground state, so we swap to
// NONEXCLUSIVE | BACKGROUND.
//
// **Why nonexclusive too?** DirectInput specifies that the keyboard device
// rejects `DISCL_EXCLUSIVE | DISCL_BACKGROUND` outright (returns
// DIERR_UNSUPPORTED). Stripping only DISCL_FOREGROUND while keeping
// DISCL_EXCLUSIVE produces that invalid combination and the engine's
// SetCooperativeLevel call fails — the keyboard never acquires and the
// engine main loop hangs in early init. Substituting DISCL_NONEXCLUSIVE
// at the same time keeps the call legal. Engine doesn't actually need
// exclusivity for its input model (it queries via GetDeviceState which
// works fine on a shared device), and shared devices let multiple
// processes coexist — exactly what we want for multi-window coop.
HRESULT STDMETHODCALLTYPE hk_SetCooperativeLevel(IDirectInputDevice8A* self,
                                                  HWND wnd, DWORD flags) {
    constexpr DWORD kDISCL_EXCLUSIVE    = 0x00000004;
    constexpr DWORD kDISCL_NONEXCLUSIVE = 0x00000008;
    constexpr DWORD kDISCL_FOREGROUND   = 0x00000001;
    constexpr DWORD kDISCL_BACKGROUND   = 0x00000002;

    DWORD new_flags =
        (flags & ~(kDISCL_FOREGROUND | kDISCL_EXCLUSIVE))
        | kDISCL_BACKGROUND
        | kDISCL_NONEXCLUSIVE;

    if (new_flags != flags) {
        mtr::log::info(
            "dinput_hook: SetCooperativeLevel hwnd=%p flags=0x%08lX -> 0x%08lX "
            "(forced NONEXCLUSIVE | BACKGROUND so device delivers input "
            "regardless of focus and the call stays legal on keyboard)",
            wnd, flags, new_flags);
    }
    return g_orig_SetCooperativeLevel(self, wnd, new_flags);
}

// Virtual cursor — accumulator updated from DIMOUSESTATE.lX/lY each time
// the game polls the mouse via GetDeviceState. menu.cpp reads (x, y),
// buttons, and wheel when UI is interactive and feeds them to ImGui.
std::atomic<int>     g_virt_x{0};
std::atomic<int>     g_virt_y{0};
std::atomic<int>     g_virt_clamp_w{1920};
std::atomic<int>     g_virt_clamp_h{1080};
std::atomic<uint8_t> g_virt_buttons{0};        // bit b set if rgbButtons[b] is down
std::atomic<int>     g_virt_wheel_accum{0};    // sum of DIMOUSESTATE.lZ since last drain
// Sub-pixel residue accumulator so the sensitivity scale below 1.0 doesn't
// drop fractional movement on every poll. Carried in fixed-point hundredths.
int g_virt_residue_x_q = 0;
int g_virt_residue_y_q = 0;
// DI delivers raw mickey counts (one per HID report tick — at 1600 DPI a
// typical desk-shake fires hundreds of mickeys), bypassing the OS pointer
// sensitivity slider. Default 75 (= 0.75x) — slightly slower than raw
// passthrough; user-tuned for this game on 1080p. Menu exposes a slider
// for tuning.
std::atomic<int> g_virt_scale_q{75};  // percent — 75 = 0.75x

// Test-harness keypress injection. test_harness sets g_inject_kb_scan to a
// DIK_* scancode and g_inject_kb_remaining > 0 to inject a key-down state
// for that scancode for N consecutive GetDeviceState polls (5 ≈ enough
// for any game to notice the press; auto-decrements on each poll). Used
// to dismiss the title screen "PRESS ANY KEY" gate, where SendInput /
// keybd_event don't reach DI-exclusive mode reliably but a direct OR
// into the returned keyboard buffer always does.
std::atomic<uint8_t> g_inject_kb_scan{0};
std::atomic<int>     g_inject_kb_remaining{0};

HRESULT STDMETHODCALLTYPE hk_GetDeviceState(IDirectInputDevice8A* This,
                                            DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_orig_GetDeviceState(This, cbData, lpvData);
    const bool ui_visible = mtr::menu::is_visible() || mtr::console::is_visible();
    const bool suppress   = mtr::freecam::active() || ui_visible;

    // Mouse foreground-gate: when this process's window isn't the system
    // foreground, return zeroed mouse state so motion + clicks don't bleed
    // into the unfocused Wilbur. Keyboard (256-byte buffer) is NOT
    // foreground-gated because NONEXCL+BG mode is intentional for keyboard
    // (autonomous tests + type-anywhere chat-style coop want it). User
    // request 2026-05-12: "Еще надо сделать чтобы с мышки ввод не шёл на
    // окно когда оно не в фокусе" — mouse per-window, keyboard shared.
    //
    // **Gate only after engine init completes.** Empirical 2026-05-12: the
    // engine's init phase (Disney splash / "press any key" / pre-screen
    // loader) hangs when mouse always returns zero on the unfocused Wilbur
    // — some pre-screen init path depends on mouse activity to proceed.
    // We use `screen_push::current_top_name` returning true as the "past
    // splash, screen system is alive" signal: once any screen has been
    // pushed, the engine is past the init-gate phase and mouse-gating is
    // safe. Latched in a static so we don't re-query after first true.
    static std::atomic<bool> s_past_splash{false};
    if (SUCCEEDED(hr) && lpvData
        && (cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2))) {
        bool past = s_past_splash.load(std::memory_order_relaxed);
        if (!past) {
            char top[64] = {0};
            if (mtr::screen_push::current_top_name(top, sizeof(top))) {
                s_past_splash.store(true, std::memory_order_relaxed);
                past = true;
            }
        }
        if (past) {
            HWND fg = GetForegroundWindow();
            DWORD fg_pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);
            if (fg_pid != GetCurrentProcessId()) {
                std::memset(lpvData, 0, cbData);
                // Skip virt-cursor + injection blocks — buffer is intentionally
                // zero this poll, and no useful state to read.
                return hr;
            }
        }
    }

    if (SUCCEEDED(hr) && lpvData
        && (cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2))) {
        // Read DI deltas + buttons + wheel BEFORE we zero the buffer for the
        // game. Only when UI is visible — freecam alone shouldn't drive the
        // virt cursor, since the menu isn't open.
        if (ui_visible) {
            const auto* m = static_cast<const DIMOUSESTATE*>(lpvData);
            const int w = g_virt_clamp_w.load(std::memory_order_relaxed);
            const int h = g_virt_clamp_h.load(std::memory_order_relaxed);
            const int s = g_virt_scale_q.load(std::memory_order_relaxed);
            // Scale raw mickeys by `s` percent. Carry fractional remainder
            // so slow movements aren't quantised away.
            const int dx_q = m->lX * s + g_virt_residue_x_q;
            const int dy_q = m->lY * s + g_virt_residue_y_q;
            const int dx = dx_q / 100;
            const int dy = dy_q / 100;
            g_virt_residue_x_q = dx_q - dx * 100;
            g_virt_residue_y_q = dy_q - dy * 100;
            int x = g_virt_x.load(std::memory_order_relaxed) + dx;
            int y = g_virt_y.load(std::memory_order_relaxed) + dy;
            if (x < 0)     x = 0;
            if (y < 0)     y = 0;
            if (x > w - 1) x = w - 1;
            if (y > h - 1) y = h - 1;
            g_virt_x.store(x, std::memory_order_relaxed);
            g_virt_y.store(y, std::memory_order_relaxed);
            uint8_t btn = 0;
            for (int i = 0; i < 4; ++i) {
                if (m->rgbButtons[i] & 0x80) btn |= static_cast<uint8_t>(1u << i);
            }
            g_virt_buttons.store(btn, std::memory_order_relaxed);
            if (m->lZ != 0) {
                g_virt_wheel_accum.fetch_add(m->lZ, std::memory_order_relaxed);
            }
        }
    }

    // Suppress mouse + keyboard data when freecam / menu / console is up.
    if (SUCCEEDED(hr) && lpvData && suppress) {
        if (cbData == 256u || cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2)) {
            std::memset(lpvData, 0, cbData);
        }
    }

    // Test-harness keypress injection — applied AFTER suppression so the
    // injected key reaches the game even when our menu is open (relevant
    // if a future scenario opens the menu mid-test). Only for keyboard
    // buffers (cbData == 256) and only while the harness has armed it.
    if (SUCCEEDED(hr) && lpvData && cbData == 256u) {
        const int rem = g_inject_kb_remaining.load(std::memory_order_relaxed);
        if (rem > 0) {
            const uint8_t scan = g_inject_kb_scan.load(std::memory_order_relaxed);
            if (scan != 0) {
                static_cast<uint8_t*>(lpvData)[scan] = 0x80;
            }
            g_inject_kb_remaining.store(rem - 1, std::memory_order_release);
        }
    }
    return hr;
}

bool capture_and_hook() {
    HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
    if (!dinput) dinput = LoadLibraryW(L"dinput8.dll");
    if (!dinput) {
        mtr::log::info("dinput_hook: dinput8.dll not loaded and LoadLibrary failed");
        return false;
    }

    using PFN_DirectInput8Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto pCreate = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(dinput, "DirectInput8Create"));
    if (!pCreate) {
        mtr::log::info("dinput_hook: GetProcAddress(DirectInput8Create) failed");
        return false;
    }

    // Use IID_IDirectInput8A — the game uses the A variant (verified in IDB
    // by the {BF798030-...} GUID byte pattern at 0x6dc7d4). dinput8.dll's
    // shared-vtable behaviour means the W path would also work in practice,
    // but matching the game's variant exactly removes one possible source
    // of divergence.
    IDirectInput8A* di = nullptr;
    HRESULT hr = pCreate(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8A,
                         reinterpret_cast<LPVOID*>(&di), nullptr);
    if (FAILED(hr) || !di) {
        mtr::log::info("dinput_hook: DirectInput8Create failed hr=0x%08lX", hr);
        return false;
    }

    IDirectInputDevice8A* dev = nullptr;
    hr = di->CreateDevice(GUID_SysKeyboard, &dev, nullptr);
    if (FAILED(hr) || !dev) {
        mtr::log::info("dinput_hook: CreateDevice(SysKeyboard) failed hr=0x%08lX", hr);
        di->Release();
        return false;
    }

    void** vt = *reinterpret_cast<void***>(dev);
    void* p_GetDeviceState      = vt[9];   // IDirectInputDevice8 slot 9
    void* p_SetCooperativeLevel = vt[13];  // IDirectInputDevice8 slot 13
    mtr::log::info("dinput_hook: device=%p vtable=%p GetDeviceState=%p SetCooperativeLevel=%p",
                   dev, vt, p_GetDeviceState, p_SetCooperativeLevel);

    dev->Release();
    di->Release();

    if (MH_CreateHook(p_GetDeviceState, &hk_GetDeviceState,
                      reinterpret_cast<void**>(&g_orig_GetDeviceState)) != MH_OK ||
        MH_EnableHook(p_GetDeviceState) != MH_OK)
    {
        mtr::log::info("dinput_hook: MinHook on GetDeviceState failed");
        return false;
    }
    mtr::log::info("dinput_hook: hooked IDirectInputDevice8::GetDeviceState at %p",
                   p_GetDeviceState);

    // Companion hook — swap DISCL_FOREGROUND for DISCL_BACKGROUND on every
    // device the engine acquires through this dinput8.dll. Lets DI return
    // real input regardless of window foreground state. Same vtable as the
    // GetDeviceState slot — patching once covers all device instances.
    if (MH_CreateHook(p_SetCooperativeLevel, &hk_SetCooperativeLevel,
                      reinterpret_cast<void**>(&g_orig_SetCooperativeLevel)) != MH_OK ||
        MH_EnableHook(p_SetCooperativeLevel) != MH_OK)
    {
        mtr::log::info("dinput_hook: MinHook on SetCooperativeLevel failed "
                       "(non-fatal — GetDeviceState hook still active)");
    } else {
        mtr::log::info("dinput_hook: hooked IDirectInputDevice8::SetCooperativeLevel at %p",
                       p_SetCooperativeLevel);
    }
    return true;
}

DWORD WINAPI deferred_thread(LPVOID) {
    // Wait for the game to load dinput8.dll. The CRT loads it during startup
    // (via DllImports), so this is usually immediate after the loader.
    for (int i = 0; i < 300; ++i) {
        if (GetModuleHandleW(L"dinput8.dll")) break;
        Sleep(50);
    }
    capture_and_hook();
    return 0;
}

} // namespace

void install() {
    HANDLE t = CreateThread(nullptr, 0, deferred_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// Public virt-cursor API consumed by menu.cpp -----------------------------

void seed_virt_cursor(int x, int y) {
    g_virt_x.store(x, std::memory_order_relaxed);
    g_virt_y.store(y, std::memory_order_relaxed);
}

void set_virt_clamp(int w, int h) {
    g_virt_clamp_w.store(w > 0 ? w : 1, std::memory_order_relaxed);
    g_virt_clamp_h.store(h > 0 ? h : 1, std::memory_order_relaxed);
}

int virt_cursor_x() { return g_virt_x.load(std::memory_order_relaxed); }
int virt_cursor_y() { return g_virt_y.load(std::memory_order_relaxed); }

// Test-harness keypress injection. Set the DIK_* scancode + a "remaining
// polls" counter so the next N GetDeviceState calls return a buffer with
// that key bit set. ~5 polls is enough for any game's input poller to
// see the press. Cheap when not used (single relaxed atomic check).
void inject_kb_keypress(uint8_t dik_scancode, int poll_count) {
    g_inject_kb_scan.store(dik_scancode, std::memory_order_relaxed);
    g_inject_kb_remaining.store(poll_count, std::memory_order_release);
}

bool virt_button(int b) {
    if (b < 0 || b > 7) return false;
    return ((g_virt_buttons.load(std::memory_order_relaxed) >> b) & 1u) != 0u;
}

// Returns sum of DIMOUSESTATE.lZ since last call (already in WHEEL_DELTA
// units = multiples of 120 — caller divides by 120 for ImGui's "wheel ticks").
int virt_drain_wheel() {
    return g_virt_wheel_accum.exchange(0, std::memory_order_relaxed);
}

void set_virt_scale_pct(int percent) {
    if (percent < 1)   percent = 1;
    if (percent > 400) percent = 400;
    g_virt_scale_q.store(percent, std::memory_order_relaxed);
}
int virt_scale_pct() { return g_virt_scale_q.load(std::memory_order_relaxed); }

} // namespace mtr::dinput_hook
