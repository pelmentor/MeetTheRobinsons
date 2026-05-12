# DirectInput buffer injection — driving game input from inside the mod (2026-05-09)

Status: **shipped + verified working**. Used by `test_harness::tick_boot_to_main_menu`
to dismiss the title screen.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## TL;DR

To make Wilbur respond to a synthetic keypress (e.g., dismissing the title
screen, navigating menus during automated tests), use
`mtr::dinput_hook::inject_kb_keypress(dik_scancode, poll_count)`.

It writes the pressed-bit directly into the buffer the engine reads from
`IDirectInputDevice8::GetDeviceState`. **All synthetic-OS-input paths
(SendKeys, keybd_event, SendInput, PostMessage) FAIL** for this game
because DirectInput-exclusive bypasses the synthetic-input pipeline.

## The problem

Wilbur uses DirectInput in exclusive-foreground mode for keyboard and
mouse. In this mode DirectInput acquires input from the kernel's raw-input
subsystem, **bypassing the path that synthetic OS input feeds into**.

Concrete failure sequence we hit on 2026-05-09 trying to dismiss the
title-screen "PRESS ANY KEY" gate:

| Method | Where it lives | Result |
|--------|----------------|--------|
| `[SendKeys]::SendWait("{ENTER}")` from PowerShell | OS WndProc | Window not focused (focus-steal protection); no-op |
| `keybd_event(VK_RETURN, 0x1C, 0/UP, 0)` from PowerShell | OS synthetic input | Goes to OS pipeline; engine doesn't see it |
| `mouse_event(MOUSEEVENTF_LEFTDOWN/UP)` from PowerShell | OS synthetic input | Same path; same no-op |
| `PostMessageA(hwnd, WM_KEYDOWN, VK_RETURN, ...)` | WndProc message queue | Engine doesn't read input via WndProc at title screen |
| In-mod `keybd_event` from inside the game's address space | OS synthetic input | Same OS-level pipeline; same no-op |
| **In-mod `dinput_hook` buffer write** | Engine's own DI buffer | **WORKS** — engine reads what we return |

We have a hint of this from the earlier RE pass — `dinput_hook.cpp` notes
that the OS cursor pin doesn't release even after we call
`SetCooperativeLevel(NONEXCLUSIVE)`. The keyboard analogue is the same:
DI-exclusive holds the input acquisition at the kernel level; synthetic
injections at the OS API boundary don't reach it.

## The fix

`dinput_hook.cpp` already hooks `IDirectInputDevice8::GetDeviceState` at
vtable slot 9 (function pointer `g_orig_GetDeviceState`). The hook fires
on every poll the engine makes for keyboard or mouse input.

Added two atomics:
```cpp
std::atomic<uint8_t> g_inject_kb_scan{0};
std::atomic<int>     g_inject_kb_remaining{0};
```

Added a buffer-OR pass at the end of `hk_GetDeviceState`:
```cpp
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
```

The `cbData == 256u` filter limits to keyboard polls (DIKEYBOARD_STATE is
a 256-byte array indexed by DIK_* scancodes; mouse buffers are smaller).

Public API:
```cpp
void mtr::dinput_hook::inject_kb_keypress(uint8_t dik_scancode, int poll_count);
```

`poll_count = 5` is enough for any input poller to observe the press
(games typically poll 60-240 Hz; we inject for ~50 ms wall-time).

## Caller usage example (test_harness)

```cpp
// In a per-render-frame tick:
if (need_dismiss_title_screen) {
    mtr::dinput_hook::inject_kb_keypress(0x1C, 5);  // DIK_RETURN, 5 polls
}
```

Repeat every ~60 frames (≈ ¼s at 240 fps) until the desired state
transition is detected — gives the engine ample chance to observe the
press across its scheduling jitter.

## Why this is RULE №1-compliant

1. **Modifies the data the engine itself reads** — no synthetic-input
   trick, no focus games, no timing race.
2. **Cooperates with the existing dinput_hook architecture** — the hook
   is already in place for input suppression (when the menu is open);
   adding injection is purely additive.
3. **Cheap when not used** — single relaxed atomic read per poll. No
   measurable overhead in the hot path.
4. **Filtered to keyboard polls only** — mouse and other DI device
   buffers are untouched.

## What this DOESN'T solve (yet)

- **Mouse-click injection.** The same buffer-OR pattern would work but
  isn't implemented yet. Keyboard is enough for menu navigation; mouse
  injection becomes useful for scenarios that pick a screen-coord (e.g.,
  click "Load" in the menu, target a specific save slot). When needed:
  add `inject_mouse_click(button_idx, poll_count)` mirroring the keyboard
  path but using `cbData == sizeof(DIMOUSESTATE)`.
- **Mouse-position injection.** Currently the virt-cursor reads
  `DIMOUSESTATE.lX/lY` deltas. To set absolute screen pos we'd need to
  inject deltas calibrated to (target - current). Defer until a scenario
  needs it.
- **Hold-key injection.** `inject_kb_keypress(scan, 5)` is a press-and-
  release. To hold a key (e.g., movement scenarios), make `poll_count`
  large or expose a separate `set_kb_held(scan, bool)` setter. Defer.

## Testing

Verified by `boot-to-main-menu` scenario: with no injection, game sits at
title screen forever (timeout). With `inject_kb_keypress(0x1C, 5)` fired
every 60 frames, screen advances to `GameSelectScreen` in ~241 frames
(≈ 1.4s of scenario time, 3.7s end-to-end including build + launch).

Back-to-back run (no game restart between) also passes — proves the
injection has no per-session side effects.

## Reference

- Public API: `mtr::dinput_hook::inject_kb_keypress(uint8_t scan, int polls)`
- Implementation: [src/mtr-asi/src/dinput_hook.cpp](../../src/mtr-asi/src/dinput_hook.cpp)
  (search for `g_inject_kb_scan` + `inject_kb_keypress`)
- First user: [src/mtr-asi/src/test_harness.cpp](../../src/mtr-asi/src/test_harness.cpp)
  (`tick_boot_to_main_menu` calls it for DIK_RETURN every 60 frames)
- DIK scancode reference: `<dinput.h>` — DIK_RETURN=0x1C, DIK_ESCAPE=0x01,
  DIK_SPACE=0x39, DIK_F1=0x3B, DIK_W=0x11, etc.
