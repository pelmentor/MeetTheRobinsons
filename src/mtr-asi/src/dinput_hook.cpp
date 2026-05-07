// DirectInput8 input suppression. The game uses DI8 in exclusive-foreground
// mode to read keyboard + mouse, bypassing the WndProc message stream. While
// freecam is active we want the player to stop walking / aiming, so we hook
// IDirectInputDevice8::GetDeviceState and zero the returned data when the
// freecam is on. Our own freecam controls use GetAsyncKeyState/GetCursorPos
// (independent of DI8), so they still work.
//
// Hook strategy mirrors the d3d9_hook approach: load dinput8.dll, create our
// own dummy IDirectInput8 + IDirectInputDeviceW instance, read vtable[9]
// (GetDeviceState), and MinHook that function pointer. MinHook patches the
// function prologue, so the override fires for *all* device instances
// (keyboard, mouse) created by the same dinput8.dll module.
//
// Cursor-freedom: while the menu is visible, the OS cursor must move
// freely so ImGui can track / click. The game pins the cursor through
// TWO mechanisms simultaneously; both have to be neutralised:
//
//   1. DI-EXCLUSIVE on the mouse device freezes GetCursorPos /
//      WM_MOUSEMOVE / MSLLHOOKSTRUCT.pt at OS level. We hook
//      IDirectInput8::CreateDevice + IDirectInputDevice8::SetCooperativeLevel
//      + Acquire so we can: capture the game's mouse device, remember the
//      original (exclusive) cooperative level, and on menu-visible swap to
//      DISCL_NONEXCLUSIVE (Unacquire → SetCoopLevel → Acquire). Restored on
//      hide. While the override is held, the Acquire hook refuses any
//      game-side re-acquire.
//
//   2. SetCursorPos recenter — many engines implement relative mouse-look
//      by polling GetCursorPos, computing delta from window center, then
//      SetCursorPos(center) every frame. With ImGui drawing a cursor on
//      top, the recenter manifests as "cursor shakes and jumps back to
//      one spot every frame". We hook SetCursorPos + ClipCursor in user32
//      and no-op them when UI is visible.
//
// Belt + braces: even if the game uses only one of these mechanisms (or
// some unknown variant), at least one of the two hook layers catches it.
//
// Device-pointer capture is also backstopped by latching in the
// GetDeviceState hook itself — if the CreateDevice hook misses the very
// first call (timing race during DLL init), the device is still latched
// the first time the game polls mouse state.

#include <windows.h>
#include <dinput.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace mtr::log     { void info(const char* fmt, ...); }
namespace mtr::freecam { bool active(); }
namespace mtr::menu    { bool is_visible(); }
namespace mtr::console { bool is_visible(); }

namespace mtr::dinput_hook {

namespace {

// IMPORTANT: the game uses IID_IDirectInput8A (verified via the
// {BF798030-...} GUID byte pattern in the binary). We must create our
// dummy instance with the A IID and use the A vtable, otherwise the
// CreateDevice / Acquire / SetCooperativeLevel hooks land on a vtable
// the game never dispatches through and silently no-op.
using PFN_GetDeviceState     = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using PFN_CreateDevice       = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using PFN_SetCoopLevel       = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*, HWND, DWORD);
using PFN_Acquire            = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*);
using PFN_Unacquire          = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8A*);
using PFN_SetCursorPos       = BOOL (WINAPI*)(int, int);
using PFN_ClipCursor         = BOOL (WINAPI*)(const RECT*);

PFN_GetDeviceState g_orig_GetDeviceState = nullptr;
PFN_CreateDevice   g_orig_CreateDevice   = nullptr;
PFN_SetCoopLevel   g_orig_SetCoopLevel   = nullptr;
PFN_Acquire        g_orig_Acquire        = nullptr;
PFN_Unacquire      g_orig_Unacquire      = nullptr;
PFN_SetCursorPos   g_orig_SetCursorPos   = nullptr;
PFN_ClipCursor     g_orig_ClipCursor     = nullptr;

// Captured on the game's CreateDevice(SysMouse). Used by the menu-visibility
// swap to call SetCooperativeLevel/Acquire/Unacquire on the actual game
// device (not our dummy instance).
std::atomic<IDirectInputDevice8A*> g_mouse_dev{nullptr};
// Last cooperative level the game asked for on the mouse device. Restored
// when the menu hides.
std::atomic<DWORD> g_mouse_orig_coop{0};
std::atomic<HWND>  g_mouse_coop_hwnd{nullptr};
// Which level we're currently holding on the device. The menu-visibility
// swap writes to this; gameplay state restores from g_mouse_orig_coop.
std::atomic<DWORD> g_mouse_current_coop{0};
// True when we've forced NONEXCLUSIVE because UI is visible. Set during
// the swap; the Acquire hook reads this to suppress the game's re-acquire
// attempts while the override is in place.
std::atomic<bool>  g_coop_overridden{false};

HRESULT STDMETHODCALLTYPE hk_GetDeviceState(IDirectInputDevice8A* This, DWORD cbData, LPVOID lpvData) {
    HRESULT hr = g_orig_GetDeviceState(This, cbData, lpvData);
    // Backstop device-pointer capture: if the CreateDevice hook missed the
    // initial call due to a timing race during DLL init, the first time
    // the game polls mouse state we still see `This` and can latch it.
    // Buffer size disambiguates mouse vs keyboard.
    if (cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2)) {
        IDirectInputDevice8A* prev = nullptr;
        if (g_mouse_dev.compare_exchange_strong(prev, This,
                                                std::memory_order_acq_rel)) {
            mtr::log::info("dinput_hook: latched mouse via GetDeviceState=%p", This);
        }
    }
    // Suppress mouse + keyboard data when we don't want gameplay input
    // bleeding through (freecam, menu, console). Buffer-size dispatch:
    //   256 bytes  -> c_dfDIKeyboard (one byte per key)
    //   16 bytes   -> DIMOUSESTATE
    //   20 bytes   -> DIMOUSESTATE2
    // Joystick formats are larger and structurally different; left alone.
    const bool suppress = mtr::freecam::active()
                       || mtr::menu::is_visible()
                       || mtr::console::is_visible();
    if (SUCCEEDED(hr) && lpvData && suppress) {
        if (cbData == 256u || cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2)) {
            std::memset(lpvData, 0, cbData);
        }
    }
    return hr;
}

// SetCursorPos hook — the game's relative mouse-look polls GetCursorPos
// and recenters with SetCursorPos every frame. While the menu is up we
// drop those recenters on the floor so ImGui's cursor is free to roam.
// Our own freecam already gates its SetCursorPos calls on UI visibility,
// so suppression here doesn't break it.
BOOL WINAPI hk_SetCursorPos(int X, int Y) {
    if (mtr::menu::is_visible() || mtr::console::is_visible()) {
        return TRUE;
    }
    return g_orig_SetCursorPos(X, Y);
}

// ClipCursor hook — some engines call ClipCursor to a tiny region around
// the window center as part of mouse-look anchoring. Force-unclip while
// the menu is up.
BOOL WINAPI hk_ClipCursor(const RECT* lpRect) {
    if (mtr::menu::is_visible() || mtr::console::is_visible()) {
        return g_orig_ClipCursor(nullptr);
    }
    return g_orig_ClipCursor(lpRect);
}

HRESULT STDMETHODCALLTYPE hk_SetCoopLevel(IDirectInputDevice8A* This, HWND hwnd, DWORD level) {
    // Capture the original (game-requested) cooperative level for the mouse
    // device. We only know "this is the mouse" by matching against the
    // pointer captured in CreateDevice. Other devices pass through untouched.
    IDirectInputDevice8A* mouse = g_mouse_dev.load(std::memory_order_acquire);
    if (mouse && This == mouse) {
        g_mouse_orig_coop.store(level, std::memory_order_release);
        g_mouse_coop_hwnd.store(hwnd, std::memory_order_release);
        // If the menu is currently visible AND we're holding the override,
        // honour the override instead of accepting the game's exclusive
        // request — the menu visibility swap re-applies the correct level.
        if (g_coop_overridden.load(std::memory_order_acquire)
            && (level & DISCL_EXCLUSIVE)) {
            const DWORD downgraded = (level & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
            const HRESULT hr = g_orig_SetCoopLevel(This, hwnd, downgraded);
            g_mouse_current_coop.store(downgraded, std::memory_order_release);
            return hr;
        }
        g_mouse_current_coop.store(level, std::memory_order_release);
    }
    return g_orig_SetCoopLevel(This, hwnd, level);
}

HRESULT STDMETHODCALLTYPE hk_Acquire(IDirectInputDevice8A* This) {
    // While the menu cooperative-level override is active, refuse the game's
    // Acquire attempts on the mouse — re-acquiring would re-pin the cursor
    // (the OS hides the cursor on any DISCL_EXCLUSIVE acquire). Other
    // devices pass through.
    IDirectInputDevice8A* mouse = g_mouse_dev.load(std::memory_order_acquire);
    if (mouse && This == mouse
        && g_coop_overridden.load(std::memory_order_acquire)) {
        // Pretend success without actually grabbing. The game's polling
        // continues; GetDeviceState returns zeroed data anyway under our
        // suppress logic.
        return DI_OK;
    }
    return g_orig_Acquire(This);
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirectInput8A* This, REFGUID rguid,
                                          LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice,
                                          LPUNKNOWN pUnkOuter) {
    HRESULT hr = g_orig_CreateDevice(This, rguid, lplpDirectInputDevice, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDirectInputDevice && *lplpDirectInputDevice
        && IsEqualGUID(rguid, GUID_SysMouse)) {
        // Latch on first mouse device the game creates. If the game replaces
        // it later, we'd lose track — in practice they create one and reuse.
        IDirectInputDevice8A* prev = nullptr;
        if (g_mouse_dev.compare_exchange_strong(prev, *lplpDirectInputDevice,
                                                std::memory_order_acq_rel)) {
            mtr::log::info("dinput_hook: captured game mouse device=%p",
                           *lplpDirectInputDevice);
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

    // IDirectInputDevice8W vtable slots:
    //   0 QueryInterface  1 AddRef  2 Release
    //   3 GetCapabilities 4 EnumObjects 5 GetProperty 6 SetProperty
    //   7 Acquire 8 Unacquire 9 GetDeviceState 10 GetDeviceData
    //   11 SetDataFormat 12 SetEventNotification 13 SetCooperativeLevel ...
    void** dev_vt = *reinterpret_cast<void***>(dev);
    void* p_GetDeviceState = dev_vt[9];
    void* p_Acquire        = dev_vt[7];
    void* p_Unacquire      = dev_vt[8];
    void* p_SetCoopLevel   = dev_vt[13];
    mtr::log::info("dinput_hook: device=%p vtable=%p GetDeviceState=%p Acquire=%p SetCoop=%p",
                   dev, dev_vt, p_GetDeviceState, p_Acquire, p_SetCoopLevel);

    // IDirectInput8W vtable slots: 0..2 IUnknown, 3 CreateDevice, ...
    void** di_vt = *reinterpret_cast<void***>(di);
    void* p_CreateDevice = di_vt[3];
    mtr::log::info("dinput_hook: di=%p vtable=%p CreateDevice=%p",
                   di, di_vt, p_CreateDevice);

    dev->Release();
    di->Release();

    auto install_one = [](const char* tag, void* va, void* hk, void** orig_pp) -> bool {
        if (MH_CreateHook(va, hk, orig_pp) != MH_OK) {
            mtr::log::info("dinput_hook: MH_CreateHook(%s @%p) failed", tag, va);
            return false;
        }
        if (MH_EnableHook(va) != MH_OK) {
            mtr::log::info("dinput_hook: MH_EnableHook(%s @%p) failed", tag, va);
            return false;
        }
        mtr::log::info("dinput_hook: hooked %s at %p", tag, va);
        return true;
    };

    if (!install_one("GetDeviceState",     p_GetDeviceState,
                     reinterpret_cast<void*>(&hk_GetDeviceState),
                     reinterpret_cast<void**>(&g_orig_GetDeviceState))) {
        return false;
    }
    install_one("CreateDevice",       p_CreateDevice,
                reinterpret_cast<void*>(&hk_CreateDevice),
                reinterpret_cast<void**>(&g_orig_CreateDevice));
    install_one("SetCooperativeLevel", p_SetCoopLevel,
                reinterpret_cast<void*>(&hk_SetCoopLevel),
                reinterpret_cast<void**>(&g_orig_SetCoopLevel));
    install_one("Acquire",             p_Acquire,
                reinterpret_cast<void*>(&hk_Acquire),
                reinterpret_cast<void**>(&g_orig_Acquire));
    // We don't hook Unacquire — we only need the orig pointer to call it
    // ourselves during the cooperative-level swap. Read it from the dummy
    // device's vtable now (already unloaded — but the function pointer is
    // a vtable entry pointing into dinput8.dll, valid for the process).
    g_orig_Unacquire = reinterpret_cast<PFN_Unacquire>(p_Unacquire);

    // user32 hooks for the SetCursorPos-recenter and ClipCursor pinning
    // mechanisms. Belt + braces against the DI coop-level swap.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        if (auto p = GetProcAddress(user32, "SetCursorPos")) {
            install_one("SetCursorPos", reinterpret_cast<void*>(p),
                        reinterpret_cast<void*>(&hk_SetCursorPos),
                        reinterpret_cast<void**>(&g_orig_SetCursorPos));
        }
        if (auto p = GetProcAddress(user32, "ClipCursor")) {
            install_one("ClipCursor", reinterpret_cast<void*>(p),
                        reinterpret_cast<void*>(&hk_ClipCursor),
                        reinterpret_cast<void**>(&g_orig_ClipCursor));
        }
    }
    mtr::log::info("dinput_hook: install complete; g_orig_CreateDevice=%p SetCoopLevel=%p Acquire=%p Unacquire=%p SetCursorPos=%p ClipCursor=%p",
                   g_orig_CreateDevice, g_orig_SetCoopLevel, g_orig_Acquire,
                   g_orig_Unacquire, g_orig_SetCursorPos, g_orig_ClipCursor);
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

// Called by menu.cpp on each visibility transition (hidden -> visible or
// visible -> hidden). Two layers run in lockstep:
//   1. DI cooperative-level swap (EXCLUSIVE <-> NONEXCLUSIVE).
//   2. Explicit ClipCursor(NULL) on show — releases any active clip the
//      game may have set independently of DI.
// SetCursorPos suppression (game's recenter-for-mouse-look pattern) is
// always-on via hk_SetCursorPos's UI-visibility check; nothing to toggle
// here.
void set_ui_visible(bool visible) {
    IDirectInputDevice8A* dev = g_mouse_dev.load(std::memory_order_acquire);
    mtr::log::info("dinput_hook: set_ui_visible(%d) — dev=%p", visible ? 1 : 0, dev);

    if (visible && g_orig_ClipCursor) {
        g_orig_ClipCursor(nullptr);
    }

    if (!dev) {
        mtr::log::info("dinput_hook: ... mouse device not yet captured; skipping coop swap");
        return;
    }

    // Defaults if the game's SetCooperativeLevel call happened before our
    // hook was installed (rare timing race during DLL init). DISCL_EXCLUSIVE
    // | DISCL_FOREGROUND is what the vast majority of DI8 games use for
    // mouse, including this one. The HWND falls back to the foreground
    // window — same window the game would be using.
    DWORD orig = g_mouse_orig_coop.load(std::memory_order_acquire);
    HWND  hwnd = g_mouse_coop_hwnd.load(std::memory_order_acquire);
    if (orig == 0) orig = DISCL_EXCLUSIVE | DISCL_FOREGROUND;
    if (hwnd == nullptr) hwnd = GetForegroundWindow();
    if (hwnd == nullptr) {
        mtr::log::info("dinput_hook: ... no hwnd available; skipping coop swap");
        return;
    }

    if (visible) {
        if (g_coop_overridden.load(std::memory_order_acquire)) {
            mtr::log::info("dinput_hook: ... already in NONEXCLUSIVE override");
            return;
        }
        const DWORD target = (orig & ~DISCL_EXCLUSIVE) | DISCL_NONEXCLUSIVE;
        const HRESULT u_hr = g_orig_Unacquire ? g_orig_Unacquire(dev) : DI_OK;
        const HRESULT s_hr = g_orig_SetCoopLevel(dev, hwnd, target);
        const HRESULT a_hr = g_orig_Acquire ? g_orig_Acquire(dev) : DI_OK;
        mtr::log::info("dinput_hook: visible swap -> Unacquire=0x%08lX SetCoop(0x%lX)=0x%08lX Acquire=0x%08lX (hwnd=%p)",
                       u_hr, target, s_hr, a_hr, hwnd);
        if (SUCCEEDED(s_hr)) {
            g_coop_overridden.store(true, std::memory_order_release);
            g_mouse_current_coop.store(target, std::memory_order_release);
        }
    } else {
        if (!g_coop_overridden.load(std::memory_order_acquire)) {
            mtr::log::info("dinput_hook: ... no override held; nothing to restore");
            return;
        }
        const HRESULT u_hr = g_orig_Unacquire ? g_orig_Unacquire(dev) : DI_OK;
        const HRESULT s_hr = g_orig_SetCoopLevel(dev, hwnd, orig);
        const HRESULT a_hr = g_orig_Acquire ? g_orig_Acquire(dev) : DI_OK;
        g_coop_overridden.store(false, std::memory_order_release);
        if (SUCCEEDED(s_hr)) {
            g_mouse_current_coop.store(orig, std::memory_order_release);
        }
        mtr::log::info("dinput_hook: hidden restore -> Unacquire=0x%08lX SetCoop(0x%lX)=0x%08lX Acquire=0x%08lX",
                       u_hr, orig, s_hr, a_hr);
    }
}

// Called every render frame from menu.cpp's on_end_scene while UI is
// interactive. Re-asserts the cursor-freedom state in case anything has
// fought it back (game re-Acquire, ClipCursor, etc.). Belt + braces over
// the one-shot set_ui_visible(true) — if the override gets dropped, this
// re-applies it within one frame.
void tick_force_unpin() {
    if (g_orig_ClipCursor) g_orig_ClipCursor(nullptr);
    IDirectInputDevice8A* dev = g_mouse_dev.load(std::memory_order_acquire);
    if (!dev) return;
    if (g_coop_overridden.load(std::memory_order_acquire)) return;  // already held
    // Override isn't held yet (or was reset by something) — re-apply.
    set_ui_visible(true);
}

} // namespace mtr::dinput_hook
