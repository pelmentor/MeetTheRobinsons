# Known issues / pitfalls

Living list. Status updated as we resolve. Last update: 2026-05-05.

> See also: [debug-features.md](debug-features.md) — catalog of hidden debug features and launch flags discovered in the unpacked Wilbur.exe (cheats, FreeCam, FirstPerson, TeleporterMenu, `-letitsnow`, etc.). These aren't issues — they're latent capabilities that could become future menu options once activation paths are understood.

## 1. Mouse buttons stuck after Wilbur.exe crashes / hangs

**Status:** mitigated, not fully fixed.

**Symptom**: cursor moves but left/right/middle clicks do nothing system-wide. Affects all applications until manually fixed.

**Cause**: Wilbur.exe puts its DirectInput8 mouse device in `DISCL_EXCLUSIVE | DISCL_FOREGROUND` cooperative mode, taking exclusive control of the mouse from Windows' standard input pipeline. When the game terminates abnormally (Task Manager force-kill while game is hung, our mod's hooks not graceful-shutdown, etc.) the device handle isn't released cleanly. The kernel thinks "exclusive owner" still has the mouse. Result: WM_*BUTTON / DirectInput / raw input all dropped for clicks.

Movement still works because cursor position is system-wide state, not exclusive-owned.

**What we did so far:**
- `DllMain DLL_PROCESS_DETACH` is now a **no-op when `lpvReserved != NULL`** (process termination). Previously we ran `MH_Uninitialize`, `ImGui_ImplDX9_Shutdown`, and unsubclassed WndProc under loader lock — that was a deadlock vector that contributed to zombies that wouldn't release input. See [`src/mtr-asi/src/dllmain.cpp`](../../src/mtr-asi/src/dllmain.cpp).
- `tools/run_game.py` cleans up stale Wilbur zombies on every launch (kills processes that hold the Disney mutex via `tools/find_mutex_holder.py`).

**Workarounds (in order of escalation)**:

1. Run `tools/mouse-wake.ps1` (sends `ClipCursor(NULL)`, `BlockInput(false)`, `SendInput(LEFTUP/RIGHTUP/MIDDLEUP)`, `ShowCursor(true)`). Often works.
2. `Ctrl + Alt + Del` → Cancel. Secure desktop transition releases user-process input grabs.
3. `Win + L` → log back in. Session-level cleanup releases DirectInput devices.
4. Reboot. Always works.

**Long-term fix (designed, not implemented):** API-boundary hook chain on `dinput8!DirectInput8Create` → `IDirectInput8::CreateDevice` → `IDirectInputDevice8::SetCooperativeLevel`. Captures every DirectInput device at runtime so we can call `Unacquire` / `SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND)` from a menu button. Full design in [dinput-cleanup.md](dinput-cleanup.md). Note: this only helps when the menu is reachable (game still running) — for genuine crashes, the post-kill wake-up scripts remain the recovery path.

## 2. Bink intro logos at game launch

**Status:** ✅ resolved (workaround).

**Symptom**: every launch played a string of logo `.BIK` videos before reaching the menu — irritating during dev iterations. (Earlier note about 800×600 corner rendering: that turned out to be a side-effect of the now-resolved native-res / aspect issue and is not currently observed.)

**Solution:** `tools/skip_intros.py` renames the five logo files (`BinkLegal.BIK`, `legal.BIK`, `dsny.BIK`, `avlogo.BIK`, `bvg.BIK`) to `*.intro_skip`. Bink open fails for those names; game proceeds to the next stage (main menu) without crashing.

```
python tools/skip_intros.py            # rename → skip
python tools/skip_intros.py --restore  # rename back
python tools/skip_intros.py --check    # report state
```

In-game cutscenes (egy, sti, dfi, end, credits, etc.) are NOT affected.

## 3. Save slot write fails: "couldn't be written"

**Status:** ✅ apparently resolved by going through the launcher with native resolution; not reproducing as of 2026-05-04. Keep an eye on it.

User confirmed during the aspect-fix testing session that saves are now created without issues when launched via `Launcher.exe → Wilbur.exe -dxresolution=2560x1440 -launchit`. Earlier failures may have been correlated with broken cmdline (the launcher emits a malformed `-dxresolution=640x480-dxdiskdriveletter=c -launchit` with no space between tokens; our cmdline rewrite fixes the resolution but the trailing token is still glued).

If it returns: use `Process Monitor` to capture file/registry calls during save attempt. Look for `WriteFile` / `CreateFile` failures and exact paths.

## 4. Mutex zombie blocks subsequent launches

**Status:** ✅ automated.

**Symptom**: After force-killing a hung Wilbur.exe, next launch errors with **"Another instance of the Meet The Robinsons already exists. Exiting."**

**Cause**: Game creates `Global\Disney_s_Meet_The_Robinsons` mutex. The launcher AND `Wilbur.exe` both check it (the launcher via its own startup, the game via its `WinMain`). On hard kill, the kernel mutex object can outlive the process if any other process retains a `hProcess` handle to the dead Wilbur (the EPROCESS isn't reaped → the mutex handle inside it isn't closed → the named mutex stays alive).

**Solution:** `tools/find_mutex_holder.py` enumerates kernel handles via `NtQuerySystemInformation`, filters to the Mutant type, identifies the holder process, and reports it. `tools/run_game.py` runs this on every launch and `TerminateProcess`'s any holder it finds, then proceeds to deploy + launch. No more "Another instance" errors during dev iteration.

## 5. `mtr-asi.asi` file locked after game crashes

**Status:** ✅ automated.

**Cause**: kernel-level cleanup delay — same root as #4 (zombie process holds the mapped image).

**Solution:** `tools/run_game.py` does a rename-trick if `Game/mtr-asi.asi` is locked when deploying:

```python
new_path = path.with_suffix(path.suffix + f".zombie-{ts}")
os.rename(path, new_path)        # the kernel handle stays with the renamed file
shutil.copy2(build_artifact, path)  # fresh DLL lands at the canonical path
```

The `*.zombie-*` files clean up themselves at the next reboot (or you can `del /q Game\mtr-asi.asi.zombie-*`).

## 6. Game hangs on shutdown sometimes

**Status:** likely fixed by the DLL_PROCESS_DETACH no-op (see #1).

If it still happens: same symptom as before — Wilbur.exe goes "Not Responding". WMI Terminate via `tools/run_game.py --clean` finishes the kill.

## 7. dxwrapper's `FullscreenWindowMode = 1` doesn't fill the screen by default

**Status:** documented surprise, not a bug.

Borderless centered window opens at backbuffer size. Native fill requires the **backbuffer itself** to be monitor-sized — done in our cmdline hook (`src/mtr-asi/src/cmdline_hook.cpp`) by rewriting `-dxresolution=…` to monitor dims (or injecting it if the launcher passes none).

dxwrapper's `UseShadowBackbuffer` would upscale a 800×600 buffer to monitor size — that's a stretch effect, not a true high-res render. We don't use it.

## 8. Frustum culling at 16:9 (root cause located 2026-05-05; one-line fix prepared, NOT applied)

**Status:** RE complete, fix ready to apply on user authorization. Full writeup: [aspect-ratio-fix.md "Cull frustum: root cause located"](aspect-ratio-fix.md).

**Root cause:** `game_camera_build_view_frustum` (`0x004DF2C0`) takes `aspect` as the input that determines left/right plane angles (via `tan(fov/2) * aspect`). It's called from `game_camera_apply_state` only when the dirty flag at `*(camera+144)` is set. Our `hk_CameraCompute` dirties `*(camera+112)` (projection cache) but NOT `*(camera+144)` (frustum cache) — so the frustum is built once at level load using the as-it-was 4:3 aspect, then never rebuilt while we keep overriding the projection to 16:9. Projection and frustum diverge → cull crosses inside the visible field.

**Fix (one line, prepared, not applied per user AFK directive):** extend `hk_CameraCompute` in [src/mtr-asi/src/d3d9_hook.cpp](../../src/mtr-asi/src/d3d9_hook.cpp) to also set `*(this+144) = 1` whenever it dirties `+112`. Diff and rationale in aspect-ratio-fix.md. One-frame lag on aspect change after the patch; negligible.

**Validation plan when applying:**
1. Build with the patch. Launch via `Game\run.bat`.
2. Set aspect to 16:9 in menu, walk into a wide-camera area (e.g. exterior shots with sky).
3. Pan slowly. Edge geometry should now appear at the actual screen edge, not stop ~5–10% in.
4. If the edges *over*-extend (geometry that should be behind a portal becomes visible), the frustum is now too wide — try the alternative-zero-lag hook instead (also documented).

No gameplay impact either way; cosmetic.

## 9. `hk_BuildProjMatrix` over-substitutes shadow/reflection probe builds (open, behaviour pending visual confirmation)

**Status:** identified by static RE (2026-05-04). NOT visually confirmed yet — left for next session to either reproduce-and-fix or dismiss-with-confidence.

**Finding:** `game_BuildPerspectiveMatrix` is called from four sites — main camera (`game_camera_recompute_projection`), `sub_4BCE00`, `sub_4BD210`, and (resolved 2026-05-05) `game_render_overlay_quad_if_enabled` (`sub_4B1150`, formerly an orphan). The three non-main-camera callers ALL pass `aspect=1.0` literal: the two `sub_4BCE0*` paths are render-to-texture probes (allocate 512-sized RTs and qmemcpy the resulting 4×4 into a probe-context offset); the overlay-quad path uses a fixed 45°/1.0/0.1/5.0 perspective for a screen-space blit. Our `hk_BuildProjMatrix` overrides their aspect to monitor-aspect on all three.

**Likely visible symptom (not yet confirmed):** subtly wrong reflections / distorted shadow volumes at non-1.333 aspects. Easy place to check is any reflective surface or strong shadow caster.

**Proposed fix:** drop `hk_BuildProjMatrix` entirely. `hk_CameraCompute` already provides a correct main-camera override path (writes target aspect into camera+12, sets dirty flag, original recompute body picks it up). Probe paths then run untouched with their intended `aspect=1.0`. See [aspect-ratio-fix.md](aspect-ratio-fix.md) for the rationale and the validation script.

If, after dropping the hook, the live aspect change in the menu stops working — keep both hooks and add a return-address check to the builder hook to substitute only when called from `0x0056462A`.
