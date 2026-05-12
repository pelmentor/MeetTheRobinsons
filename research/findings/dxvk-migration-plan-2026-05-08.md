# DXVK migration plan — replace dxwrapper, multi-week scope

Date: 2026-05-08
Status: planning + Phase A starting.
Governing rule: [feedback_no_crutches.md (RULE №1)](memory/feedback_no_crutches.md) — proper, root-cause solutions only.

## Architecture decision

**Single-layer**: Wilbur.exe → d3d8.dll (DXVK) → Vulkan → GPU.

DXVK upstream supports D3D8 natively (verified from
[DXVK README](https://raw.githubusercontent.com/doitsujin/dxvk/master/README.md):
"A Vulkan-based translation layer for Direct3D 8/9/10/11"). For D3D8
support we need both `d3d8.dll` and `d3d9.dll` from DXVK.

**No dxwrapper**: dxwrapper provides three things in our config —
D3D8→D3D9 translation, D3D9 wrapping, and FullscreenWindowMode. The
first two are obviated by DXVK; the third we replicate natively in
mtr-asi.

**Why not keep dxwrapper for FullscreenWindowMode**: that would be a
crutch — dragging an 8MB wrapper just for one feature when we already
have full control of the D3D8 chain. We hook CreateWindowEx /
IDirect3D8::CreateDevice ourselves, override the present params or
window style. Cleaner foundation.

**No prebuilt DXVK DLLs**: DXVK as a submodule, built from source via
meson + mingw-w64. We control the version and can patch if needed.

## Current dxwrapper feature inventory (from `Game/dxwrapper.ini`)

```
D3d8to9                = 1    -> obviated by DXVK D3D8 support
EnableD3d9Wrapper      = 1    -> obviated by DXVK D3D9
EnableWindowMode       = 1    -> we replicate (window mode override)
FullscreenWindowMode   = 1    -> we replicate (borderless monitor-size)
```

All other flags in dxwrapper.ini are at their defaults (= disabled).

## DXVK build environment (Windows native)

DXVK's upstream build path:
- meson 0.58+
- mingw-w64 GCC 10.0+ (with posix threads)
- glslang
- ninja

Windows-native means **MSYS2** with the mingw-w64 toolchain. Pure
`Build Tools for Visual Studio` won't work — DXVK requires
mingw-w64-specific extensions.

Cross-file: `build-win32.txt` (for 32-bit, which Wilbur.exe needs).

Build command:
```
meson setup --cross-file build-win32.txt --buildtype release --prefix /tmp/dxvk-install build.w32
cd build.w32 && ninja install
```

Output: `d3d8.dll` and `d3d9.dll` in `bin/`.

## Phase plan

### Phase A — DXVK submodule + build infrastructure (~2-3 days work)

1. Add DXVK as `third_party/dxvk` submodule, pinned to a known-good
   release tag (latest stable as of 2026-05-08).
2. Write `tools/build-dxvk.sh` (MSYS2/Linux/WSL) and possibly
   `tools/build-dxvk.bat` (MSYS2 launcher from Windows cmd).
3. Document MSYS2 toolchain install in `docs/dxvk-build.md`.
4. CI consideration: add a manual GitHub Actions workflow to build
   DXVK DLLs from our pinned submodule (not auto-triggered, but
   available so we can re-build without local toolchain).

Output: `Game/d3d8.dll` and `Game/d3d9.dll` from our DXVK build.

### Phase B — native FullscreenWindowMode (~2-3 days work)

Implement a `windowmode.cpp` module in mtr-asi that replicates
dxwrapper's `FullscreenWindowMode = 1` + `EnableWindowMode = 1`:

1. Hook `IDirect3D8::CreateDevice` (via the d3d8.dll we hook for
   ImGui anyway).
2. Force `D3DPRESENT_PARAMETERS.Windowed = TRUE`.
3. Hook `CreateWindowExA/W` or `SetWindowLongA/W` to remove
   WS_OVERLAPPEDWINDOW + apply WS_POPUP style.
4. Resize the window client area to monitor size + position at (0,0).
5. Optional: handle alt-tab properly (release device on minimize,
   re-acquire on restore).

Reuse pattern from existing `d3d9_hook.cpp` (we already hook D3D
device creation for ImGui).

### Phase C — dxwrapper retirement (LANDED 2026-05-09)

Toolchain set up + DXVK built + DLLs deployed. Pending: runtime smoke test.

1. **Toolchain**: MSYS2 v20260322 installed via `winget`. mingw-w64
   i686 GCC 16.1.0 (POSIX threads), meson 1.11.1, ninja, glslang
   (x86_64 host). Yandex mirror prepended to MSYS2 mirrorlists for
   reasonable download speeds.
2. **Cross-file fix**: `third_party/dxvk/build-win32.txt` patched to
   pass `--use-temp-file` to windres. Without it, windres's `popen()`
   to spawn gcc-as-preprocessor was failing on Windows (mixed-slash
   path quoting issue inside cmd.exe). Saved 30 minutes of head-
   scratching for the next person.
3. **Build**: `tools/build-dxvk.sh --no-deploy` produced:
   - `third_party/dxvk-build/dxvk-v2.7.1/x32/d3d8.dll` (3.56 MB)
   - `third_party/dxvk-build/dxvk-v2.7.1/x32/d3d9.dll` (7.28 MB)
4. **Deploy**: dxwrapper trio backed up to `Game/*.dxwrapper.bak`,
   DXVK DLLs copied in. "DXVK" marker string verified in both DLLs.
5. **Final Game/ state**:
   - `d3d8.dll` (DXVK 3.56 MB, was dxwrapper stub 484 KB)
   - `d3d9.dll` (DXVK 7.28 MB, did not exist before)
   - `dxvk.conf` (our tuned config from Phase E)
   - `dxwrapper.dll.bak`, `dxwrapper.ini.bak`,
     `d3d8.dll.dxwrapper.bak` — preserved for rollback if anything
     breaks during Phase D.
   - `dinput8.dll`, `mtr-asi.asi` — unchanged.

`third_party/dxwrapper` submodule + Phase A's repo plan to remove
the `.gitmodules` entry — deferred until Phase D confirms DXVK works
end-to-end. No reason to drop the rollback path before then.

### Phase D — full verification (~1 week work)

Test that everything we built on top of dxwrapper still works:

1. **ImGui menu** (Insert key): should render correctly via DXVK.
2. **FreeCam** (F3): D3D state pushes still hold.
3. **Aspect ratio override**: aspect_patch + sprite_xform + sprite_split.
4. **camera_apply hook + interp**: M3.1 view interp writes to globals,
   verify they reach the GPU through DXVK.
5. **Sprite batcher overrides**: matrix4_make_scale / translate_via_stack
   patches — these write to D3D state via dword_715B64 / 715B48 thunks.
6. **Cursor virt-cursor** (DI deltas + ImGui mouse pos).
7. **All dt-correctness paths** (Phase 1-3): verify they still work
   over DXVK's D3D9 backend.
8. **All sprite_xform per-element overrides** (per-element pivot).
9. **All vis_test / scene_vis_log diagnostics**.

If anything breaks: investigate root cause (likely DXVK-vs-dxwrapper
behavioral difference). NEVER add a crutch to "make it work";
understand the difference and fix properly.

### Phase E — DXVK config tuning + call inventory (LANDED 2026-05-09)

DXVK has a `dxvk.conf` for tuning. Tuned config + call-inventory doc
landed 2026-05-09. See:

- `Game/dxvk.conf` — Wilbur-specific tuning, ~10 explicit settings, each
  with a commented rationale (frame pacing handed off to our limiter,
  low-latency present queue, sprite-batcher precision flags, sampler
  anisotropy bump, no managed-resource eviction).
- `research/findings/dxvk-call-inventory-2026-05-09.md` — full inventory
  of Wilbur's D3D imports (just `Direct3DCreate8`), mtr-asi's D3D9
  vtable hooks (slot indices verified against ABI), engine-internal
  hooks unaffected by the swap, and the Phase C swap-in checklist +
  Phase D verification matrix (20 items, "least-surprising failure first").

Native Vulkan opportunities (deferred to Phase F+):
- HDR via VK_EXT_hdr_metadata.
- VRR: DXVK already exposes this via maxFrameLatency.
- Compute-shader-based effects layered on top via our own Vulkan
  side-channel (very advanced; defer indefinitely).

### Phase F — open

Once everything works end-to-end, this phase opens up:
- Modern display features (HDR, true VRR).
- Higher-quality assets streamed via DXVK's better memory mgmt.
- Native Vulkan extensions.

## Risks (anticipated, with crutchless mitigations)

1. **DXVK D3D8 has bugs Wilbur triggers**: investigate the bug,
   patch DXVK locally, upstream the fix. Don't add a workaround in
   our code.

2. **MSYS2 build is fragile / fails for some users**: write thorough
   docs; possibly cache built DLLs in a release-style download (but
   the SOURCE is authoritative and submodule-pinned).

3. **Performance regression**: DXVK is generally faster than D3D9 for
   modern GPUs, but corner cases exist. Profile, identify, fix root
   cause (driver issue, DXVK bug, or our hook overhead).

4. **ImGui dx9 backend doesn't work on DXVK**: I don't expect this
   (DXVK exposes the full D3D9 API), but if it happens, use ImGui's
   native Vulkan backend over DXVK's underlying VkDevice (advanced).

5. **FreeCam / camera_apply hook breaks**: investigate why; should
   work since DXVK uses standard D3D9 device + transform matrix
   semantics.

## Pinning + version

DXVK pinned to **v2.7.1** as `third_party/dxvk` submodule (verified via
`git describe --tags` in third_party/dxvk).

## Documentation work

- `docs/dxvk-build.md` — toolchain install, build commands, expected
  outputs.
- `docs/dxvk-migration.md` — high-level overview for users who want
  to understand why we switched.
- `research/findings/dxvk-d3d-call-inventory-2026-05-XX.md` (later) —
  inventory of every D3D8/D3D9 API the game actually uses, for
  verification matrix.

## What this plan is NOT

- **Not** a quick fix or a "let's just drop in the prebuilt DLL" hack.
- **Not** a coexistence with dxwrapper (wrapper-on-wrapper).
- **Not** a hand-tuned binary patch (DXVK is the proper foundation).
- **Not** rushed. Per RULE №1, weeks-or-months is fine — we do it
  right.
