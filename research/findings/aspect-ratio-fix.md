# Aspect ratio fix (4:3 → 16:9 / arbitrary, live-toggleable)

**Status: SOLVED, no crutches (2026-05-04).** Implementation: two hooks in `src/mtr-asi/src/d3d9_hook.cpp` (`hk_BuildProjMatrix`, `hk_CameraCompute`) plus atomic target storage in `src/mtr-asi/src/aspect_patch.cpp`.

## TL;DR

Two function-level hooks let us live-override the projection aspect without touching D3D output matrices and without modifying the binary on disk:

| Hook                    | VA           | Convention | Role |
|-------------------------|--------------|------------|------|
| `hk_BuildProjMatrix`    | `0x00562B20` | `__cdecl`  | Substitute `aspect` parameter on every projection-matrix build. |
| `hk_CameraCompute`      | `0x00564600` | `__thiscall` | Per-frame: write target aspect into the camera's cached aspect field `*(this+12)` and re-arm dirty flag `*(this+112)=1` so the original re-runs the build path with the right aspect. Cache-invalidation half — without it, changes only land on scene transitions. |

Default at startup: primary monitor's aspect (auto-detected). Live-changeable from Insert menu (4:3 / 16:10 / 16:9 / 21:9 / Monitor / Custom slider).

## How the game builds projection (recovered call chain)

```
game_camera_init_with_defaults  (default camera init)
  ├── sets near/far/fov defaults from globals if camera fields == defaults
  └── game_camera_setup(this, 0, 0, 1.0, 1.0)        @ 0x561D6B
        ├── reads g_window_client_width  / g_window_client_height (= GetClientRect of focus window)
        ├── CameraAspect = game_GetCameraAspect()    @ 0x561C10
        └── *(float*)(this + 12) = CameraAspect * (a4/a5)   ← per-camera cached aspect

game_camera_apply_state (per-frame camera tick)
  └── game_camera_recompute_projection(this)                                @ 0x564650+0x7A
        if (*(BYTE*)(this+112) /* dirty */) {
            sub_5626D0(0); sub_562650(); sub_5625C0();
            game_BuildPerspectiveMatrix(*(float*)(this+8)  /* fov_deg */,
                       *(float*)(this+12) /* aspect  */,
                       *(float*)(this+0)  /* near    */,
                       *(float*)(this+4)  /* far     */);  ← BUILDER, hook target
            sub_562CB0(0); sub_4C5FC0(...); sub_562690();
            *(BYTE*)(this+112) = 0;
        }
        return this + 48;   // returns a pointer to the cached matrix region
```

Then the wrapper `wrap_SetTransform` (`wrap_SetTransform`) reads `g_state_global` (`0x006FBD58`) and dispatches to `IDirect3DDevice::SetTransform` via vtable.

### Key globals

| VA           | Symbol                                | Value at runtime |
|--------------|---------------------------------------|------------------|
| `0x006FBC38` | `g_window_client_width`               | Set by `sub_5635E0` from `GetClientRect` of focus window. |
| `0x006FBC3C` | `g_window_client_height`              | "                                                          |
| `0x006FBD14` | `g_window_show_state`                          | Static `1`. **Never written**; only read. (See dead-code note below.) |
| `0x006FBD58` | `g_d3d_transform_state` (D3DTS_*)     | Set by callers right before `wrap_SetTransform`. |

## Dead-code red herring at `0x006C750C`

`game_GetCameraAspect`:

```c
double game_GetCameraAspect() {
    result = 1.3333334;                                 // ← AB AA AA 3F at 0x006C750C
    if (g_window_show_state != 3)
        return (double)g_window_client_width / (double)g_window_client_height;
    return result;                                      // never reached in retail
}
```

`g_window_show_state` (`0x006FBD14`) has only **read** xrefs across the entire binary — it's statically `1`, never written. The `!= 3` branch is always taken; the constant `0x006C750C` and the WSGF guide's `AB AA AA 3F` byte-pattern at file offset `0x8EFEE` are **dead code in this build**.

Earlier attempts:
- Byte-patch `0x006C750C` from `1.333…` to `1.778…` — invisible, the constant is never returned.
- Patch the `jz` at `0x561C1F` so the function always returns the constant — still invisible, because the aspect getter is only called from `game_camera_setup` which runs once at scene init. Even if it were always called, the cache at `*(camera+12)` swallows changes.
- WSGF's "patch `AB AA AA 3F` → `39 8E E3 3F` at file offset `0x8EFEE`" — that offset lives inside the SecuROM-encrypted `rr02` section in the SHIPPED EXE; the runtime bytes at the corresponding RVA are different. (`tools/patch_aspect.py` exists as a fallback path but is not the primary fix on this build.)

**Don't waste time patching `0x006C750C`.** The widescreen issue is upstream: by the time `game_camera_setup` runs, `g_window_client_width/height` haven't yet been resized to the user's chosen resolution, so the cached `*(camera+12)` aspect is computed from a 4:3 default window.

## Why hook `game_BuildPerspectiveMatrix` (matrix builder)

Signature: `int __cdecl game_BuildPerspectiveMatrix(float fov_deg, float aspect, float near, float far)`.

```c
v5 = a1 * 0.017453292;          // FOV deg → rad
game_PerspectiveFovRH(buf, v5, a2 /*aspect*/, a3 /*near*/, a4 /*far*/);
return device->SetTransform(g_d3d_transform_state, buf);
```

`game_PerspectiveFovRH` (`0x0063CAF7`) is the right-handed perspective formula: `m[0]=cot(fovY/2)/aspect`, `m[5]=cot(fovY/2)`, `m[10]=far/(near-far)`, `m[11]=-1`, `m[14]=far*near/(near-far)`.

The function's contract takes `aspect` as input. We substitute the `aspect` argument with `mtr::aspect::current()` regardless of caller. **Param substitution at the documented input — not output mangling, not a crutch.**

### Over-substitution risk on shadow/reflection probe call sites

`game_BuildPerspectiveMatrix` has **four xrefs** (verified in IDA 2026-05-04):

| Caller VA      | Caller                                  | Aspect arg        | Notes |
|----------------|-----------------------------------------|-------------------|-------|
| `0x0056462A`   | `game_camera_recompute_projection`      | `*(camera+12)`    | Main camera. Writes the result via `matrix4_copy` into camera+48 cache. |
| `0x004BD086`   | `sub_4BCE00`                            | **`1.0` literal** | Setup-with-allocation path for what looks like a render-to-texture probe (allocates a 512-sized RT via `sub_4C9D10`, copies the resulting 4×4 into `target+16` via `qmemcpy`). FOV is computed from a per-object atan2; aspect is hardcoded square. |
| `0x004BD3D5`   | `sub_4BD210`                            | **`1.0` literal** | Same shape as `sub_4BCE00` minus the allocation — looks like the per-frame update for the same probe class. |
| `0x004B1186`   | `game_render_overlay_quad_if_enabled` (`sub_4B1150`) | **`1.0` literal** | Resolved 2026-05-05 (was undefined orphan in an unanalyzed code region; `define_func` recovered it). Fixed `(fov=45°, aspect=1.0, near=0.1, far=5.0)` perspective for an overlay/blit quad rendered when `dword_729E7C` (texture/RT pointer) is non-null. Aspect doesn't actually affect the quad — it's drawn in screen space with a baked-in vertex array — but our hook substitutes anyway. |

**All three non-main-camera callers pass `aspect=1.0` literal.** Our `hk_BuildProjMatrix` is currently substituting on every one of them.

`hk_BuildProjMatrix` substitutes monitor aspect into **all four** of these. For the two probe call sites that pass `aspect=1.0` deliberately (square shadow / reflection / portal projection), our hook breaks their projection. The visible symptom would be subtle — wrong proportions inside reflective surfaces, distorted shadow volumes, or skewed render-to-texture content — and it is **not** the cause of the visible edge culling at 16:9 (that's separate, see below).

**Proposed correctness change (NOT yet applied — needs visual testing):** drop `hk_BuildProjMatrix` entirely. The live-override loop already works through `hk_CameraCompute` alone:

1. `hk_CameraCompute` writes target aspect to `*(camera+12)` and sets dirty `*(camera+112)=1`.
2. Original `game_camera_recompute_projection` body sees the dirty flag, calls `game_BuildPerspectiveMatrix(fov, *(camera+12), near, far)` — i.e. with our target aspect.
3. The two probe paths still run with their hardcoded `aspect=1.0` and produce correct square projections.

This is strict-improvement: same main-camera result, no probe regression. Validate by toggling aspect in the menu while looking at any reflective or shadow-receiving surface (the Robinson household has plenty); if reflections/shadows snap when removing the hook, the change is correct.

## Why also hook `game_camera_recompute_projection` (cache-invalidation gateway)

`game_camera_recompute_projection` checks `*(BYTE*)(this+112)` (dirty flag): if cached, returns the cached pointer; if dirty, calls `game_BuildPerspectiveMatrix` and clears the flag. Without our second hook, a live aspect change in the menu would not invalidate the cache — game would keep returning the previously-computed 4:3 matrix until something else dirtied the camera (scene change, camera reset).

`hk_CameraCompute` writes target aspect into `*(this+12)` and forces `*(this+112) = 1` if the cached value differs, so each per-frame call recomputes with our value.

`__thiscall` x86: `this` arrives in `ECX`. MinHook hooking on a `__thiscall` is done via `__fastcall` (this in ECX, edx unused for this signature) on MSVC.

## Live override flow

1. `mtr::aspect::init()` (in `aspect_patch.cpp`) is called from `DllMain` after `MH_Initialize`. Reads primary monitor dims via `MonitorFromPoint(0,0)` + `GetMonitorInfoA`, stores aspect in `std::atomic<float> g_target`.
2. `mtr::menu` exposes buttons `4:3 / 16:10 / 16:9 / 21:9 / Monitor (auto)` and a slider with `Apply`. Each calls `mtr::aspect::set(value)` → updates `g_target`.
3. `hk_CameraCompute` reads `g_target` per frame, writes to `*(this+12)`, sets dirty.
4. Original `game_camera_recompute_projection` calls `game_BuildPerspectiveMatrix` with our aspect → `hk_BuildProjMatrix` passes through (already correct). New matrix built. Cache cleared.
5. `wrap_SetTransform` writes the new matrix to D3D.

End-to-end latency on aspect change: **one frame**.

## Why not just patch the binary?

WSGF's hex hack is documented at file offset `0x8EFEE` in the retail Wilbur.exe — but that file location is inside the SecuROM-encrypted `rr02` section in the SHIPPED binary. After SecuROM decrypts at runtime, the bytes at the corresponding RVA are different from what's on disk; modifying the encrypted bytes does not predictably propagate. A PE-sieve unpacked dump (`ida/dumps/process_*/400000.Wilbur.exe`) shows the float `AB AA AA 3F` at five different VAs (only `0x006C750C` is in dead game_GetCameraAspect; the other four are orphan bytes or UI-billboard scaling, not the projection path). There is no single static-patch site that fixes the rendering aspect on this build.

`tools/patch_aspect.py` is kept as a fallback that performs WSGF's documented edit on the on-disk EXE; not the primary fix for this build.

## Cull frustum: root cause located (2026-05-05)

**Builder:** `game_camera_build_view_frustum` at `0x004DF2C0`. Signature:

```c
__cdecl game_camera_build_view_frustum(
    out_buf*,    // = camera + 148
    float near,
    float far,
    char  has_extra_clip,
    int   extra_clip_axis,
    float extra_clip_d,
    float fov_deg,
    float aspect)
```

Layout it writes at `out_buf` (= `camera + 148..`):
| Offset (rel) | Content                                                                |
|--------------|------------------------------------------------------------------------|
| `+0..15`     | Near plane normal `(0, 0, 1)` and `d = near`.                          |
| `+16..31`    | Far plane normal `(0, 0, -1)` and `d = -far`.                          |
| `+32..47`    | Top plane: `(0, sin(π/2 − fov/2), cos(π/2 − fov/2), 0)`.               |
| `+48..63`    | Bottom plane: mirror of top.                                           |
| `+64..79`    | Left plane: `(sin(π/2 − atan2(tan(fov/2)·aspect, 1)), 0, cos(...), 0)`.|
| `+80..95`    | Right plane: mirror of left.                                           |
| `+96..127`   | Optional 7th clip plane (managed by `game_camera_apply_optional_clip_plane`). |
| `+256..272`  | Four corners of the near-plane rectangle in view space.                |

The horizontal frustum derivation explicitly multiplies `tan(fov/2) * aspect` — so aspect is the input that controls the left/right plane angles. **`aspect` is the right knob.**

**Caller:** `game_camera_apply_state` (`0x00564650`) is the *sole* per-frame caller. It reads `aspect = *(camera + 12)` and passes it as `a8`, but the call only happens when the dirty flag at `*(camera + 144)` is set:

```c
char __thiscall game_camera_apply_state(char *this) {
    char v2 = *(this + 144);              // <-- frustum-dirty flag
    *(this + 616) = 1;
    if (v2) {                             // <-- this is the gate
        // read v17 = *(camera+12) = aspect
        // …
        *(this + 144) = 0;
        sub_4DF2C0(this + 148, …, /*aspect=*/ v17);
    }
    sub_5626D0(0);
    v8 = game_camera_recompute_projection(this);  // <-- our hk_CameraCompute fires HERE
    wrap_SetTransform(v8);
    …
}
```

**Why edge culling is visible at 16:9:** our `hk_CameraCompute` writes target aspect to `*(camera+12)` and dirties `*(camera+112)` (projection cache) — but NOT `*(camera+144)` (frustum cache). So:

1. `apply_state` checks `*(camera+144)` — it's whatever the game set last (typically `0` after the first scene-load build). Frustum rebuild is skipped.
2. `apply_state` calls `recompute_projection`, our hook writes target aspect to `*(camera+12)` and dirties `+112`.
3. Original recompute body sees `+112` dirty, builds projection at target aspect.
4. Result: projection matrix is 16:9, but the cull frustum at `camera+148..` is still the 4:3 one built at level load. Edges of the wider 16:9 view are inside the 4:3 cull pyramid → pop/cull.

The cull frustum *would* eventually rebuild — `*(camera+144)` is dirtied by camera-state-changing code paths, e.g. when the game itself changes FOV or near/far. But absent those events it stays stale.

**One-line fix** — in `src/mtr-asi/src/d3d9_hook.cpp`, extend `hk_CameraCompute` to dirty `+144` whenever it dirties `+112`:

```diff
 int __fastcall hk_CameraCompute(int this_, int /*edx_unused*/) {
     if (this_) {
         const float target = mtr::aspect::current();
         if (target > 0.1f && target < 10.0f) {
             float* aspect_field = reinterpret_cast<float*>(this_ + 12);
             BYTE*  proj_dirty   = reinterpret_cast<BYTE*>(this_ + 112);
+            BYTE*  frustum_dirty= reinterpret_cast<BYTE*>(this_ + 144);
             if (*aspect_field != target) {
                 *aspect_field = target;
                 *proj_dirty   = 1;
+                *frustum_dirty= 1;
             }
         }
     }
     return g_orig_CameraCompute(this_, 0);
 }
```

Order-of-operations note: this still leaves a one-frame latency because `apply_state` reads `+12` and `+144` *before* `recompute_projection` is called inside the same frame. So the timeline becomes:

- Frame N: aspect change to 16:9 in menu → `mtr::aspect::current()` returns 16:9.
- Frame N+1: `apply_state` reads old `+12` (4:3) and `+144 = 0` → skips frustum rebuild. Then `recompute_projection` runs, our hook writes `+12 = 16:9` and dirties `+112` and `+144`. Original recompute rebuilds projection.
- Frame N+2: `apply_state` reads new `+12` (16:9) and `+144 = 1` → calls `game_camera_build_view_frustum` with 16:9 → frustum is now correct.

A 1-frame lag on aspect change isn't visible. The current observed cull artifact is due to the frustum being stuck at 4:3 *forever* until something else dirties `+144`. With the patch, the frustum rebuilds at most one frame after the aspect changes, and stays in sync after.

**Alternative (zero-frame-lag):** also hook `game_camera_apply_state` and write `+12 + +144` *before* the original body runs. Slightly more invasive (extra hook, extra ECX-thunk), and the 1-frame lag is imperceptible, so the hk_CameraCompute one-liner is the right fix.

Tracked in [known-issues.md §8](known-issues.md).

## Files

- `src/mtr-asi/src/aspect_patch.cpp` — atomic target, `init()`/`set()`/`current()`/`original()`.
- `src/mtr-asi/src/d3d9_hook.cpp` — `hk_BuildProjMatrix`, `hk_CameraCompute`, hook installation in `capture_vtables_and_hook`.
- `src/mtr-asi/src/menu.cpp` — UI controls.
- `src/mtr-asi/src/dllmain.cpp` — calls `mtr::aspect::init()` from DllMain.
- `tools/patch_aspect.py` — disk-patch fallback (not primary).
- `tools/find_aspect_rva.py` — locates `AB AA AA 3F` candidates in the unpacked dump.
- `reference/Disney's Meet the Robinsons _ WSGF_widescreen_HACK.pdf` — WSGF guide (file-offset patch; not effective on this build).
