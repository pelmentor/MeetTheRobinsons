# DirectInput cleanup — releasing the mouse without a reboot

How the game owns mouse/keyboard input, why hard kills leave the mouse stuck system-wide, and how a runtime hook can release the cooperative-level grab on demand.

## What we know about the game's DirectInput usage

`Wilbur.exe` statically imports a single function from `dinput8.dll`:

| IAT slot     | Symbol                | Module    |
|--------------|-----------------------|-----------|
| `0x006A6020` | `DirectInput8Create`  | `dinput8` |

A second mirror at `0x02EE2030` is a SecuROM-rebuilt thunk in `rr02` (plain `jmp dword ptr [0x6A6020]` — no encryption, just an extra import-stub).

**Why we can't find the call site statically:** Verified 2026-05-05 — there is no `FF 15 20 60 6A 00` (`call dword ptr [0x6A6020]`) byte sequence anywhere in the dumped image. Same for `FF 15 B0 5A 65 00` (call to the `0x655AB0` thunk). The literal `0x6A6020` only appears inside the three thunks; no game code references it. This isn't an IDA auto-analysis gap — the bytes that would encode the call genuinely don't exist in the dump pages.

This is a SecuROM 7 lazy-decryption artefact: our PE-sieve dump (`process_22276/`) was taken at the main menu. DirectInput initialization likely runs only after entering an in-game 3D scene, and that code block was still in its un-triggered SecuROM state when the dump was taken. The DInput init code path is genuinely missing from our image. (See [unpack-state.md](unpack-state.md) "Known incompleteness".) GUIDs (`GUID_SysMouse`, `GUID_SysKeyboard`) are also not present as plaintext bytes — same root cause.

Recovery option: take a fresh PE-sieve dump from inside an active 3D scene to capture the decrypted DInput init path.

But we don't actually need to. The API surface is stable, and capturing every `IDirectInputDevice8*` from `DirectInput8Create` onward gives us full control regardless of where in the game the call lives.

## Why the mouse stays stuck after hard kills

`Wilbur.exe` puts its DirectInput8 mouse device in `DISCL_EXCLUSIVE | DISCL_FOREGROUND` cooperative mode. While the game runs:

- Windows' standard input pipeline (`WM_*BUTTON`, raw input, GetAsyncKeyState for buttons) is suppressed for the mouse buttons. DirectInput owns them.
- Cursor position is system-wide state, so it still moves; only the buttons are dropped.

On a normal exit (Alt+F4, in-game quit), the game calls `IDirectInputDevice8::Unacquire()` and the kernel releases the exclusive grab. Everything goes back to normal.

On a **hard kill** (Task Manager → End Process, our `tools/run_game.py` `TerminateProcess`, or a crash that bypasses cleanup), no user-mode shutdown code runs. The kernel *should* free the device handle when EPROCESS gets reaped — but in practice if any external `hProcess` handle keeps the EPROCESS alive (zombie state, see [known-issues.md §1](known-issues.md)), the device grab stays. Result: mouse-stuck system-wide until reboot or one of the wake-up tricks works.

## The fix: API-boundary hook chain

Three hooks, all on stable API entry points:

```
DirectInput8Create          ← IAT 0x006A6020 (or inline on dinput8.dll)
    ↓ captures IDirectInput8*
IDirectInput8::CreateDevice ← vtable[3], capture each IDirectInputDevice8*
    ↓ records device pointer in our list
IDirectInputDevice8::SetCooperativeLevel ← vtable[13]
    ↓ optional: log the flags or override DISCL_EXCLUSIVE → DISCL_NONEXCLUSIVE
```

With every captured `IDirectInputDevice8*` in a list, we can:

1. **Menu button "Release input lock"**: iterate list, call `Unacquire()` then `SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)` on each. Mouse buttons immediately go back to Windows. Game keeps running but loses input until next focus change (or until user re-enters menu and clicks "Restore").

2. **Watchdog cleanup (future)**: a separate process (or our existing `tools/run_game.py`) detects the game window has stopped responding, attaches via debugger or calls our exposed control API, releases input, then `TerminateProcess`. No mouse-stuck even on crash.

3. **Auto-release on focus loss**: hook `WM_ACTIVATEAPP` — when the game loses focus, drop exclusive cooperative level. When it regains, re-acquire. Lets Alt+Tab work cleanly without the game locking the mouse on the desktop.

### Why API-boundary instead of game-internal

We can't see the game's `IDirectInputDevice8*` storage from static analysis. Hooking `DirectInput8Create` and walking the vtable is the only path. Same approach as `BinkOpen` — the game-side code is opaque, the DLL API is not.

This also generalises across game versions / DRM variants — the API is stable between any DX9 game using DirectInput, so the hook code is reusable.

## Hook design

### IAT vs inline

Two slots exist for `DirectInput8Create`:
- `.idata` IAT at `0x006A6020`
- SecuROM mirror at `0x02EE2030`

Per [lessons-learned L15](lessons-learned.md), prefer **inline hook on `dinput8!DirectInput8Create`** to catch both:

```cpp
HMODULE dinput8 = LoadLibraryA("dinput8.dll");
auto p = GetProcAddress(dinput8, "DirectInput8Create");
MH_CreateHook(p, &hk_DirectInput8Create, (LPVOID*)&g_orig_DirectInput8Create);
```

Note: `LoadLibraryA` rather than `GetModuleHandleA` — `dinput8.dll` is statically imported by Wilbur but our DLL initialises before Wilbur.exe finishes its imports. Loading explicitly is the simplest way to ensure the library is mapped when we install the hook. The kernel reference-counts the load, so this doesn't double-load.

### Hook bodies

```cpp
struct DInputCapture {
    IDirectInput8A*           input  = nullptr;
    std::vector<IDirectInputDevice8A*> devices;
    HWND                      focus_hwnd = nullptr;
    DWORD                     last_coop_flags = 0;
    std::mutex                m;
};
DInputCapture g_dinput;

HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid,
                                     LPVOID* out, LPUNKNOWN unk) {
    HRESULT r = g_orig_DirectInput8Create(inst, ver, iid, out, unk);
    if (SUCCEEDED(r) && out && *out) {
        std::lock_guard<std::mutex> lk(g_dinput.m);
        g_dinput.input = (IDirectInput8A*)*out;
        // Hook CreateDevice (vtable[3] in IDirectInput8)
        void** vtbl = *(void***)g_dinput.input;
        MH_CreateHook(vtbl[3], &hk_CreateDevice, (LPVOID*)&g_orig_CreateDevice);
        MH_EnableHook(vtbl[3]);
    }
    return r;
}

HRESULT WINAPI hk_CreateDevice(IDirectInput8A* self, REFGUID rguid,
                                LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN unk) {
    HRESULT r = g_orig_CreateDevice(self, rguid, out, unk);
    if (SUCCEEDED(r) && out && *out) {
        std::lock_guard<std::mutex> lk(g_dinput.m);
        g_dinput.devices.push_back(*out);
        // Hook SetCooperativeLevel on FIRST device only — vtable layout is
        // identical across all IDirectInputDevice8 instances, so one hook
        // covers all of them.
        if (g_dinput.devices.size() == 1) {
            void** vtbl = *(void***)*out;
            MH_CreateHook(vtbl[13], &hk_SetCooperativeLevel,
                          (LPVOID*)&g_orig_SetCoop);
            MH_EnableHook(vtbl[13]);
        }
    }
    return r;
}

HRESULT WINAPI hk_SetCooperativeLevel(IDirectInputDevice8A* self,
                                      HWND hwnd, DWORD flags) {
    {
        std::lock_guard<std::mutex> lk(g_dinput.m);
        g_dinput.focus_hwnd = hwnd;
        g_dinput.last_coop_flags = flags;
    }
    return g_orig_SetCoop(self, hwnd, flags);
}
```

### Public release call

Exposed to `menu.cpp` as a button:

```cpp
namespace mtr::dinput {
    bool release_input_lock() {
        std::lock_guard<std::mutex> lk(g_dinput.m);
        if (!g_dinput.focus_hwnd) return false;
        for (auto* dev : g_dinput.devices) {
            dev->Unacquire();
            dev->SetCooperativeLevel(g_dinput.focus_hwnd,
                                     DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        }
        return true;
    }

    bool restore_input_lock() {
        std::lock_guard<std::mutex> lk(g_dinput.m);
        if (!g_dinput.focus_hwnd || !g_dinput.last_coop_flags) return false;
        for (auto* dev : g_dinput.devices) {
            dev->SetCooperativeLevel(g_dinput.focus_hwnd, g_dinput.last_coop_flags);
            dev->Acquire();
        }
        return true;
    }
}
```

Menu UI: two buttons in the existing Insert overlay — "Release input (drop exclusive)" and "Restore input". Use them when the game hangs or the mouse stops responding outside the game.

### Validation

1. Launch game, open Insert menu, click "Release input lock".
2. `Win+Tab` or `Alt+Tab` to another window. Mouse should click normally everywhere.
3. Return to game, click "Restore input lock". Game should resume capturing input.
4. Hard-kill the game while exclusive lock held but BEFORE clicking release. Mouse should stay stuck — confirms the released state is what the menu button creates.
5. After release, hard-kill. Mouse should remain functional (no exclusive grab to leak).

## Limitations

- A user can't reach the menu *during* a hang (the game's render thread is the one drawing the menu — if the game is hung, the menu is hung). For genuine crashes, the wake-up scripts in [`tools/`](../../tools/) (e.g. `tools/mouse-wake.ps1`) and the post-kill cleanup in `tools/run_game.py` remain the recovery path.
- Setting `DISCL_NONEXCLUSIVE | DISCL_BACKGROUND` permanently breaks in-game mouse-look (the game polls DInput state which won't update outside foreground). That's the trade — clicking "Restore" puts it back.

## Files & symbols

| VA           | Symbol                  | Notes |
|--------------|-------------------------|-------|
| `0x006A6020` | `DirectInput8Create` IAT | Zero static xrefs from game code; SecuROM-VM dispatched. |
| `0x02EE2030` | (mirror)                | Inside SecuROM `rr02`. |

Hook implementation would live in a new `src/mtr-asi/src/dinput_hook.cpp`. Wire from `dllmain.cpp` immediately after `MH_Initialize`. Menu buttons in `menu.cpp`.
