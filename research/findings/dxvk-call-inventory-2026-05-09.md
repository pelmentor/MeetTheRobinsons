# DXVK call inventory + Phase C swap-in checklist

Date: 2026-05-09
Status: Phase E in progress; Phase C ready to execute when DXVK is built.

This doc inventories every D3D8/D3D9 API surface the game and mtr-asi
touch, so the Phase D verification matrix has a clear list of what to
re-check after we swap dxwrapper out for DXVK.

---

## 1. Wilbur.exe's D3D imports (verified via IDA imports table)

| Module | Symbol | Purpose | DXVK status |
|--------|--------|---------|-------------|
| `d3d8` | `Direct3DCreate8` | one-shot factory entry | DXVK exposes the same export |
| `dinput8` | `DirectInput8Create` | input enumeration | unrelated to DXVK |
| `dsound` | `DirectSoundCreate` | audio | unrelated to DXVK |
| `binkw32` | `_BinkOpenDirectSound@4`, `_BinkSetSoundSystem@8`, etc. | Bink intro video | unrelated to DXVK |

**Total D3D imports: 1.** Everything else flows through the
`IDirect3D8` and `IDirect3DDevice8` vtables built by `Direct3DCreate8`.

This is exactly the surface area DXVK's `d3d8.dll` is designed to
expose — Wilbur is the "minimal D3D8 entry-point" pattern DXVK was
built for.

---

## 2. mtr-asi's D3D9 hooks (after dxwrapper's d3d8to9 translation)

mtr-asi never touches D3D8 directly. dxwrapper currently translates
every D3D8 call to D3D9 and we hook the D3D9 device vtable. After the
swap, DXVK's `d3d8.dll` does the same translation internally and we
hook DXVK's D3D9 vtable instead. The vtable layout is fixed by the
public D3D9 ABI, so the slot indices are stable across implementations.

| Hook | Vtable slot | Source | Purpose |
|------|-------------|--------|---------|
| `IDirect3D9::CreateDevice` | 16 | [d3d9_hook.cpp](../../src/mtr-asi/src/d3d9_hook.cpp) | windowed override + grab device for ImGui |
| `IDirect3DDevice9::Reset` | 16 | [d3d9_hook.cpp](../../src/mtr-asi/src/d3d9_hook.cpp) | re-apply window mode + ImGui invalidate |
| `IDirect3DDevice9::EndScene` | 42 | [d3d9_hook.cpp](../../src/mtr-asi/src/d3d9_hook.cpp) | ImGui render + input poll |
| `IDirect3DDevice9::SetTransform` | 44 | [d3d9_hook/sprite_hooks.cpp](../../src/mtr-asi/src/d3d9_hook/sprite_hooks.cpp) | aspect ratio, sprite override |
| `IDirect3DDevice9::SetClipPlane` | 41 | [d3d9_hook/diag_hooks.cpp](../../src/mtr-asi/src/d3d9_hook/diag_hooks.cpp) | diagnostic |
| `IDirect3DDevice9::SetRenderState` | 57 | [d3d9_hook/diag_hooks.cpp](../../src/mtr-asi/src/d3d9_hook/diag_hooks.cpp) | clip-plane filter, fog disable |

The vtable slot indices are part of the D3D9 ABI ([Microsoft D3D9 SDK
header `d3d9.h`](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/)
— DXVK's `d3d9.dll` exports the same vtable layout). No re-hooking
required after swap; same slot numbers, same call signatures.

---

## 3. Engine-side D3D9 calls mtr-asi cares about (write-paths)

These are calls the engine makes that we either filter, augment, or
read state from. Verified via IDA RE.

| Call site | Engine VA | What it does | mtr-asi response |
|-----------|-----------|--------------|------------------|
| `camera_apply_all_active` | `0x4C1E40` | writes view + world matrix globals (`0x724C10` / `0x724C50`) | M3 view-interp POST hook |
| `per_camera_apply` | `0x4C1BA0` | per-camera apply funnel | FreeCam PRE hook |
| `sprite_batcher (sub_4E8D30)` | `0x4E8D30` | builds 2D sprite matrices via `dword_715B64` / `dword_715B48` thunks | sprite_xform per-element overrides |
| `MatrixSetXformA` (sprite scale) | `0x63C4E3` | premultiplies scale into sprite matrix stack | sprite_xform PRE-multiply hook |
| `halo_component_update` | `0x6678D0` | per-frame halo screen projection | M3.3 halo follow-fix PRE hook (just shipped) |
| `engine_pump_alt` halo block | `0x68149A` | builds proj matrix to `0x745AA0`, walks scene-component list | observed only; not hooked |

All the above are hooks on **engine-internal functions** — they don't
touch D3D9 directly. The D3D9 calls happen downstream when the engine
flushes its draw queue. So DXVK swap doesn't affect these hooks at all;
they fire identically before and after the swap.

---

## 4. ImGui's DX9 backend (third-party, used as-is)

`third_party/imgui/backends/imgui_impl_dx9.cpp` makes the following
D3D9 calls each frame in `EndScene`:

- `CreateVertexBuffer` / `CreateIndexBuffer` (once on init)
- `Lock` / `Unlock` (every frame, dynamic geometry)
- `SetStreamSource`, `SetIndices`, `SetVertexShader`, `SetPixelShader = nullptr`
- `SetFVF`, `SetTextureStageState`, `SetSamplerState`
- `DrawIndexedPrimitive`
- State-save / state-restore around all of the above (`StateBlock`)

All of these are bog-standard D3D9 fixed-function-pipeline calls. DXVK
handles them all natively. No expected issues.

The state-save / state-restore via `IDirect3DStateBlock9` is the most
likely thing to behave differently across implementations — if the
mtr-asi menu draws and then the next engine frame is broken, suspect
this first. Test path: open Insert menu, close it, verify next frame's
geometry is intact.

---

## 5. Phase C swap-in checklist

When DXVK has been built (via `tools/build-dxvk.sh` from MSYS2 or WSL):

1. **Backup the dxwrapper trio**
   ```sh
   cd Game/
   mv d3d8.dll        d3d8.dll.dxwrapper.bak
   mv dxwrapper.dll   dxwrapper.dll.bak
   mv dxwrapper.ini   dxwrapper.ini.bak
   ```

2. **Drop in DXVK's DLLs**
   The build script (`tools/build-dxvk.sh`) deploys to `Game/` automatically
   if the existing files have the "DXVK" marker; otherwise it warns and
   skips. After the backup above is in place, re-run with `--no-deploy`
   removed so it copies cleanly.

3. **Verify swap state**
   ```sh
   ls -la Game/
   # Expected:
   #   d3d8.dll    (DXVK ~3-4 MB, has 'DXVK' marker string)
   #   d3d9.dll    (DXVK ~5-6 MB, has 'DXVK' marker string)
   #   dinput8.dll (Ultimate ASI Loader — unchanged)
   #   mtr-asi.asi (mtr-asi mod — unchanged)
   #   dxvk.conf   (Wilbur-tuned, just landed in Phase E)
   #   No dxwrapper.dll, no dxwrapper.ini.
   ```

4. **Smoke test (Phase D, item 1)**
   Launch via `Game/run.bat`. Watch for:
   - Wilbur's launcher renders normally (it uses GDI, unaffected by D3D
     swap, but if it fails we know LoadLibrary chain is broken).
   - Game intro Bink video plays (DirectSound path, unaffected).
   - Main menu appears at monitor resolution (windowmode.cpp's
     borderless handler should kick in regardless of which D3D backend).
   - Hit Insert: ImGui menu opens. **This is the canary test for
     `IDirect3DStateBlock9` cross-impl behavior.**

5. **If anything breaks**
   - First check `Game/dxvk.log` (DXVK writes init + warnings here).
   - Then `Game/mtr-asi.log` (windowmode + d3d9_hook trace).
   - Per RULE №1, find the root cause, don't add a crutch. DXVK is
     well-documented enough that any divergence has a known explanation.

---

## 6. Phase D verification matrix

After Phase C lands, walk through each of these and confirm they still
work. Order is "least surprising failure first" so the obvious problems
surface fast.

| # | Feature | How to test | Source |
|---|---------|-------------|--------|
| 1 | Game launches + main menu | `run.bat` | engine |
| 2 | Borderless fullscreen at monitor res | observe window | mtr-asi `windowmode.cpp` |
| 3 | Insert menu (ImGui) | press Insert in-game | mtr-asi `menu.cpp` |
| 4 | F3 freecam (mouse-look + WASD) | toggle F3, fly around | mtr-asi `freecam.cpp` |
| 5 | F3 + MMB player teleport | freecam + MMB click | mtr-asi `freecam.cpp` (just landed) |
| 6 | Aspect ratio override | switch to 16:9 in menu | mtr-asi `aspect_patch.cpp` |
| 7 | Sprite-batcher matrix override | enable auto-pillarbox | mtr-asi `sprite_xform.cpp` |
| 8 | M3 view interp + halo follow-fix | "Recommended" preset, rotate camera | mtr-asi `interp_view.cpp` + `interp_halo.cpp` |
| 9 | M4 player interp | "All smoothing" preset, walk Wilbur | mtr-asi `interp_player.cpp` |
| 10 | Throttle (60 Hz lock) | enable, watch `Sim Hz` reading | mtr-asi `sim_decouple.cpp` |
| 11 | dt-correctness (HUD particles) | walk through scenes | mtr-asi `dt_correctness.cpp` |
| 12 | Per-element sprite_xform overrides | configured rules | mtr-asi `sprite_xform.cpp` |
| 13 | Console (F2) | press F2 in-game | mtr-asi `console.cpp` |
| 14 | Screenshot (F12) | press F12 | mtr-asi `screenshot.cpp` |
| 15 | Cursor virt-cursor (DI deltas) | open menu, move cursor | mtr-asi `dinput_hook.cpp` |
| 16 | Bink intro video | wait through intro | engine + binkw32 |
| 17 | DirectSound audio | play any sound | engine + dsound |
| 18 | DirectInput keyboard / mouse | gameplay | engine + dinput8 |
| 19 | Save / load (slot creation) | start new game, save | engine I/O (known issue: separate IO bug, may still fail) |
| 20 | Mini-game launches (DigDug, etc.) | trigger one | engine + mtr-asi auto-disable-in-minigame |

If any item regresses vs the dxwrapper baseline, root-cause it before
shipping. Mostly we expect 1-15 to all just work because DXVK's D3D9
implementation is mature; 16-20 are non-D3D paths that should be
trivially unaffected.

---

## 7. Open questions / pre-flight

- **DXVK build environment on this Windows machine**: not present
  (no MSYS2, no meson, no glslangValidator, WSL is docker-only).
  User needs to set up the toolchain per [docs/dxvk-build.md](../../docs/dxvk-build.md)
  before Phase C can land. Alternative: GitHub Actions cross-build
  workflow (Phase A item not yet implemented).
- **DXVK logging**: leave `dxvk.logLevel = info` so the first runtime
  test produces a log that we can post-mortem before declaring Phase C
  successful.
