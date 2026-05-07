# Camera subsystem — RE'd from runtime + static (2026-05-05)

This documents the camera architecture in retail Wilbur, end to end. Built up over multiple probe + IDA sessions; the final mtr-asi FreeCam plugs natively into this pipeline.

## High-level flow per frame

```
render_frame_top_level (sub_4D22D0)
   ├── sub_4BD0C0                     shadow caster setup
   ├── camera_apply_all_active (sub_4C1E40)
   │     for each cam in g_active_camera_array[g_active_camera_count]:
   │        if cam.flag(+0x308) == 1: RT-camera path (sub_4CD520/40/50 + sub_4C1BA0)
   │        else                    : main-camera apply (sub_4C1BA0)
   ├── game_render_main_scene (sub_563C70)   reads g_d3d_view_matrix_global (0x724C10),
   │                                          submits to D3D, renders scene
   └── ...post passes
```

The camera-tick that BUILDS each camera's view matrix runs *before* `camera_apply_all_active`. It is dispatched per frame on each camera-controller object (PathCam, deathcam, …) and writes the view matrix into the render-side camera struct.

## Two structs

The runtime uses two distinct objects per camera:

### Render-side camera struct (`outer_cam`)

Allocated on the heap; pointers stored in `g_active_camera_array` (`0x6F5E70`). Size ≥ 1024 bytes; the projection cache *embedded inside it* is exactly `0x270` bytes (`game_camera_init_with_defaults` at `0x561C50` does `memset(this, 0, 0x270)` on its dedicated buffer; that buffer is at `outer+0x40`).

| Offset    | Field |
|-----------|-------|
| `+0x00`   | `1.0f` constant (used by the engine; not a vtable) |
| `+0x04..+0x33` | pad / fill pattern |
| `+0x34`   | **pointer to view matrix** (typically `outer+0x370`) |
| `+0x40`   | **embedded projection-cache** (the 0x270-byte struct from `game_camera_init_with_defaults`) |
| `+0x40..+0x4F` | near, far, fov, aspect (RH; default 0.25 / 1500 / 65 / 1.333) |
| `+0x40+0x30..+0x6F` | cached 4x4 projection matrix |
| `+0x40+0x70` | projection-dirty flag |
| `+0x40+0x94..+0x190+` | view-frustum buffer (planes + corners), built by `game_camera_build_view_frustum` |
| `+0x2B0..+0x2EF` | **embedded WORLD matrix** (camera-to-world, view⁻¹). Row 3 = eye position in world. |
| `+0x308`  | flag — `0` = main scene, `1` = render-target/probe |
| `+0x30C`  | RT context pointer (when flag == 1) |
| `+0x360`  | view-state sub-object (the camera-controller stores a pointer to *this*) |
| `+0x370`  | view matrix object (4x4 floats; pointed to by `+0x34`) |

### Camera-controller class

A C++ class instance allocated separately from the render-side struct. Per-frame "tick" lives here; the controller writes the view matrix into the render-side struct it owns.

Verified runtime layout for a PathCam instance:

| Offset   | Field |
|----------|-------|
| `+0x00`  | vtable pointer (= `g_PathCam_vtable @ 0x6C9AE0`, or `g_deathcam_vtable @ 0x6C1D40`, …) |
| `+0x10`  | pointer to the view-state object inside `outer_cam` (= `outer+0x360`) |
| `+0x14`  | name string ASCII ("PathCam\\0" / "deathcam\\0" …) |
| `+0x18..` | per-class state |
| `+0x230` | embedded sub-object (base-class init via `sub_58C640`) |
| `+0x28C` | embedded sub-object with vtable `0x6C9AB4` |
| `+0x50`  | matrix slot (cached source for the view-matrix build) |
| `+0xD0`  | secondary matrix combined via `sub_41A380` |

Two sub-objects (at `+0x230` and `+0x28C`) are always present and behave like base classes; they are initialised by the ctor (`PathCam_ctor` @ `0x58E6C0`).

## Camera-tick: PathCam_tick (`0x58C910`)

The single function whose `matrix4_copy` calls land in the view-matrix slot every frame. Confirmed via runtime probe — every per-frame write to `outer+0x370` originated from the call at `0x58C99F`.

```c
int __thiscall PathCam_tick(int this) {
    // Pre: tick the input/source object at this+0x38
    (*(this[14].vtable[1]))(this[14], this);
    sub_58C000(this);

    // Tick the controller at this+60 (or fallback via this+16)
    int v2 = *(this+60);
    if (!v2) v2 = *(*(this+16) + 116);
    (*(v2.vtable[1]))(v2, this);
    sub_58BA30(this);

    // Build the view matrix into a local, then write to the view slot.
    sub_41A380(this+80, this+208);                          // combine cached + secondary
    matrix4_copy(*(this+16) + 16,    /* = outer+0x370 */
                 local_view_matrix);                       // <-- this is the write
    // post:
    int v3 = *(this+64);
    if (!v3) v3 = *(*(this+16) + 120);
    (*(v3.vtable[1]))(v3, this);

    // Also copy this+80 to (this+16).matrix slot for downstream consumers
    int v4 = *(this+16);
    if (v4) matrix4_copy(v4+16, this+80);
}
```

Key insight: the controller's `+0x10` field points to the render-side camera's view-state sub-object (`outer+0x360`), and the view matrix is at `view-state + 0x10` (= `outer+0x370`). That's the slot the engine's downstream pipeline reads from.

## Apply: per-camera (`sub_4C1BA0`, called by `camera_apply_all_active`)

`__thiscall(outer_cam, char skip_inverse_compute)`. After the camera-tick has populated the view matrix slot, this runs for each active camera:

```
// Compute world = inverse(view) when skip_inverse == 0
if (!skip_inverse) sub_50B340(view_ptr=*(this+0x34), &world=this+0x2B0);

// Save 'this' pointer to global so other engine code can find the active cam
g_active_camera_ptr = this;     // 0x724F3C

// Send view + world to D3D globals
matrix4_copy(g_d3d_view_matrix_global  /*0x724C10*/, *(this+0x34));
matrix4_copy(g_d3d_world_matrix_global /*0x724C50*/, this+0x2B0);
```

The `0x724C10` / `0x724C50` globals are then consumed by `game_render_main_scene` (`sub_563C70`) and various per-object passes that look up `0x724C10` for view-space transforms.

The actual function body sits behind a SecuROM thunk: prologue at `0x4C1BA0` is `sub esp,0Ch / push esi / mov esi,ecx / jmp [F8788C]`. The matrix-copy logic at `0x4C1BAC..0x4C1C40` is reached through the thunk; static decompile is partial, but the byte-level decode is verified.

## Found classes

| Vtable VA | Name | Notes |
|-----------|------|-------|
| `0x6C9AE0` | `PathCam` | Default gameplay camera. Path-following, with scriptable target and timing (PathCamTarget, PathCamSpeed, PathCamAccel, …). Tick = `PathCam_tick @ 0x58C910` (vtable[1] direct). |
| `0x6C1D40` | `deathcam` | Death/post-death camera. Tick goes through wrapper `sub_533C10` which calls `PathCam_tick`. Ctor lives in SecuROM region near `0x2111020`. |

`camera_system_ctor` (`0x5D2350`) constructs three cameras up front via name-lookups on the registry returned by `sub_58D950(dword_741648)`:

- **ScriptCam** — `sub_58D330("ScriptCam")` lookup; if missing, allocates with `sub_5832C0(76)` then `sub_58CA80(this, "ScriptCam", 1)`. 76-byte controller; used during cinematic/scripted shots.
- **StationaryCam** — name-lookup `sub_58D200("StationaryCam", 0)`; allocates with `sub_5832C0(672)`.
- **PathCam** — `sub_5832C0(768)` then `PathCam_ctor(v9, "PathCam", 0)` (768-byte controller).

Together with `deathcam`, these four classes share the per-frame apply path (`camera_apply_all_active` → `sub_4C1BA0` → globals `0x724C10`/`0x724C50`). Wall-collision anims, scripted catscenes, and death transitions can hand control between them; freecam therefore hooks downstream of all of them, at `camera_apply_all_active` POST.

Likely additional classes (BossCam etc.) exist — see strings `CameraSetBossCamSourceDistance`, `CamSourceDistance`, `CamPositionAndTarget` at `0x6C2C84+`. All script-binding names are SLNG-hashed and have no direct xrefs (Bob Jenkins lookup2). Reaching them statically requires hashing the names and searching for matching immediates.

### Named-target handle (PathCam target)

PathCam holds a *target handle* at `controller+0x34`. Runtime dump shows the handle's layout:

| Offset | Field |
|--------|-------|
| `+0x00` | vtable `0x6C990C` (no static xrefs found — must be assigned via byte-pattern write or SecuROM-thunked code) |
| `+0x04..+0x13` | inline 16-byte ASCII name buffer. Observed values: `"ScriptCam"` (cinematic-driven), `"Avatar"` (Wilbur during free-roam) |
| `+0x14..` | unknown (mostly zero in the dump) |

So `*(controller+0x34)` is **not** the player heap object — it's a name handle. To get the actual entity (and hence its position vec3 for MMB-teleport), the name must be resolved through the same registry the camera-system ctor uses (`sub_58D950(dword_741648)` + `sub_58D330(name)`). REing that resolver is the next step for MMB-tp.

## Matrix functions

| VA | Name | Role |
|----|------|------|
| `0x4C5FC0` | `matrix4_copy` | 16-dword copy. |
| `0x50B340` | `matrix4_invert_affine` | Affine inverse (rotation transpose + translated-back). Used by `sub_4C1BA0` to compute `world = view⁻¹`. |
| `0x50B990` | `matrix4_lookat_rh` | `(eye, target, up, out)` — RH LookAt; writes a row-major view matrix. Three callers in retail (shadow caster, cinematic `sub_4E6A20`, and a self-contained matrix updater) — **not** the main-gameplay path. PathCam builds its matrix via `sub_41A380` + matrix-copy, not via this function. |
| `0x562B20` | `game_BuildPerspectiveMatrix` | Calls `game_PerspectiveFovRH` (`0x63CAF7`) and submits the result to D3D. mtr-asi hooks this for FOV/aspect override. |

## Globals

| VA | Name | Role |
|----|------|------|
| `0x724C10` | `g_d3d_view_matrix_global` | The 4x4 view matrix sent to D3DTS_VIEW. |
| `0x724C50` | `g_d3d_world_matrix_global` | World matrix for the currently-applied camera. |
| `0x724F3C` | `g_active_camera_ptr` | Pointer to the camera most recently applied. Saved by `sub_4C1BA0`. |
| `0x6F5E70` | `g_active_camera_array` | Array of `outer_cam*` pointers, one per active camera. |
| `0x6F5E74` | `g_active_camera_count` | Length of the above array. |
| `0x6FBD58` | `g_d3d_transform_state` | Currently-selected D3DTS_* index for `wrap_SetTransform`. |
| `0x6FBD40` | `dword_6FBD40` | Stack-index → D3DTS_* table; `[0]=3 (PROJECTION)`, `[1]=2 (VIEW)`, `[2..5]=TEXTURE0..3`, `[6]=2 (VIEW alias)`. |

## mtr-asi FreeCam — native plug-in

[`src/mtr-asi/src/freecam.cpp`](../../src/mtr-asi/src/freecam.cpp) holds the freecam pose (position + yaw + pitch + speeds). [`src/mtr-asi/src/d3d9_hook.cpp`](../../src/mtr-asi/src/d3d9_hook.cpp) hooks `sub_4C1BA0` (per-camera apply) as a PRE-hook:

```cpp
int __fastcall hk_PerCameraApply(void* this_, int /*edx*/, int skip_inverse) {
    if (this_ /* outer_cam */) {
        const uint32_t state = *reinterpret_cast<const uint32_t*>(static_cast<char*>(this_) + 0x308);
        const bool main_cam = (state == 0);   // 0 = main, 1 = RT/shadow/probe

        if (main_cam && mtr::freecam::active()) {
            // overwrite the view matrix at *(outer+0x34) before the orig
            // copies it to globals + sends to D3D
            ...
        }
        if (main_cam && mtr::draw_dist::has_override()) {
            // write outer+0x44 (per-camera FAR field) +
            // outer+0xD4+16..31 (frustum's far plane equation) +
            // outer+0xD4+352..395 (frustum's far corners) every frame
            ...
        }
        if (main_cam && mtr::scene::side_cull_disabled()) {
            // overwrite top/bottom/left/right planes at outer+0xD4+32/+48/+64/+80
            // with always-pass equations (0,0,0,1)
            ...
        }
        mtr::scene::enforce_fog_disabled();    // pumps fogEnabled BYTE at 0x745279 to 0
    }
    return g_orig_PerCameraApply(this_, 0, skip_inverse);
}
```

Why this hook point: retail Wilbur swaps cameras at runtime — `PathCam`/`ScriptCam`/`StationaryCam`/`deathcam`. `sub_4C1BA0` is the single funnel every camera class goes through during `camera_apply_all_active`. Hooking PathCam_tick alone missed the others (the wall-lock/ScriptCam-takeover symptom). Hooking `camera_apply_all_active` POST and writing to globals 0x724C10/50 also didn't propagate because D3D was already given the per-camera matrix from `*(outer+0x34)` upstream of the global-write step. `sub_4C1BA0` PRE catches everything before the orig fans out to globals + D3D.

Same hook does data-level overrides on the engine's CACHED frustum buffer at `outer+0xD4`, because `game_camera_build_view_frustum` (0x4DF2C0) only fires ~5 times at scene init and the buffer is reused for the rest of the run. Diagnostic logging confirmed this — overriding the build-frustum function had zero runtime effect after init.

Frustum buffer layout (offsets within outer+0xD4):

| Offset | Field | Used by |
|--------|-------|---------|
| `+0..15`  | near plane `(0, 0, 1, near)` | view-frustum cull |
| `+16..31` | far plane `(0, 0, -1, -far)` | **`mtr::draw_dist`** writes per frame |
| `+32..47` | top plane | **`side_cull_disabled`** sets to `(0,0,0,1)` |
| `+48..63` | bottom plane | same |
| `+64..79` | left plane | same |
| `+80..95` | right plane | same |
| `+96..127` | optional extra clip plane | water/reflection passes |
| `+352..399` | 4 far-corner positions (16 floats) | engine-internal LOD/visibility tests; orig hardcodes z=-10000, draw_dist recomputes from current fov/aspect |

This is still not a crutch — the engine itself reads from those exact offsets; we just rewrite values in its own buffer with our chosen far/side planes. No code patches, no thunk hooks, no fake state.

### Earlier alternatives (tried, didn't work)

| Approach | Outcome |
|---------|---------|
| Substitute projection's far at `game_BuildPerspectiveMatrix` | Only affects depth precision -> z-fighting at distance, no draw distance change |
| Direct write to scene cvars (clipFar @ 0x745260, fogFar @ 0x745280, etc.) | Engine reads them but they don't gate draw distance (clipFar is consumed by DOF math at sub_647FBF onwards) |
| Hook `game_camera_build_view_frustum` to substitute `far_` arg | Function only fires ~5 times at scene init; engine never rebuilds per frame |
| Force `+144` dirty flag in `hk_CameraCompute` | Was set AFTER apply_state had already passed the dirty check; ineffective |
| Hook `game_camera_apply_state` PRE to dirty `+144` | apply_state itself isn't called per frame, so still no rebuild |
| Hook `sub_4E0B90` (per-object visibility test, SecuROM thunk) and force return TRUE | 1-instruction JMP thunk + MinHook trampoline = save-load crash. Rolled back. |
| Patch the call site `call sub_4E0B90` at 0x4C385D with `mov al,1; nop*3` | Builds, doesn't crash, but no observable cull-distance change. sub_4E0B90 is a per-object visibility test (low byte = "draw"), NOT the camera-relative cull plane. Code lives as `mtr::force_vis` but unused in UI. |
| Hook `IDirect3DDevice9::SetClipPlane` / `D3DRS_CLIPPLANEENABLE` | **Confirmed never called** by Wilbur — D3D9 user clip planes are not the gate. |

Activation: `F3` toggle; on first activate, pose is seeded from the engine's current view matrix (decoded via the RH-LookAt inverse: eye = `-(t·R)`, yaw = `atan2(-fwd.x, fwd.z)`, pitch = `asin(fwd.y)` — note the negated `fwd.x` matches our convention where `forward.x = -sin(yaw)*cos(pitch)`, chosen so positive yaw == camera-right turn, matching mouse convention).

Controls:
- Mouse (primary look): cursor-recenter approach via GetCursorPos+SetCursorPos. Suppressed when ImGui menu/console is open so the user can interact with UI.
- Arrows = fallback look (rad/s).
- `WASD` translate, `Space/C` up/down, `Shift` 4× speed.
- **Mouse wheel** = exponential adjust of move_speed (~6× per click). Plumbed via `WH_MOUSE_LL` low-level hook on a dedicated message-pump thread ([`src/mtr-asi/src/input_hook.cpp`](../../src/mtr-asi/src/input_hook.cpp)) because DI8-exclusive eats wheel from WndProc. While freecam is active the LL hook also swallows the wheel from the game.
- **MMB** = teleport-player-to-camera request. Currently a discovery dump while we finish reverse-engineering the named-target → entity resolution path (see Pending below).

DI8 input suppression: [`src/mtr-asi/src/dinput_hook.cpp`](../../src/mtr-asi/src/dinput_hook.cpp) hooks `IDirectInputDevice8::GetDeviceState`. While freecam **or** Insert menu **or** console is visible, returned buffers for keyboard (256 bytes) / mouse (DIMOUSESTATE 16 / DIMOUSESTATE2 20) are zeroed — game sees no keys held and no mouse delta, so the player stops moving. Our freecam controls use GetAsyncKeyState/GetCursorPos and aren't affected.

## Scene cvars (corrected addresses — NOT the +0x100 offset I had earlier)

[`src/mtr-asi/src/aspect_patch.cpp`](../../src/mtr-asi/src/aspect_patch.cpp) — `mtr::scene` namespace. Block base is `0x745240` (clearColorR), confirmed by sub_5B1E10 reading `*((float*)&g_input_state_base + 68)` = `0x745158 + 272 = 0x745268` (defaultShutdownDistance, registered with immediate 7623272 = 0x745268).

| Cvar | VA | Type |
|------|----|----|
| clearColorR/G/B/A | 0x745240..0x74524C | DWORD |
| forceClear | 0x745250 | BOOL |
| backBufferDepth / frontBufferDepth | 0x745254 / 0x745258 | DWORD |
| **clipNear** | 0x74525C | float |
| **clipFar** | 0x745260 | float |
| fadeSpan | 0x745264 | float |
| **defaultShutdownDistance** | 0x745268 | float |
| deathPlaneY | 0x74526C | float |
| fallToDeathDistance | 0x745274 | float |
| **fogEnabled** | 0x745279 | BYTE |
| fogNear / fogFar / fogDensity | 0x74527C / 0x745280 / 0x745284 | float |
| fogColorR/G/B/A | 0x745288 / 0x74528C / 0x745290 / 0x745294 | DWORD |
| baseFOV | 0x745298 | float |

`mtr::scene::set_fog_disabled(true)` writes 0 to fogEnabled BYTE; `enforce_fog_disabled()` is pumped each frame from hk_PerCameraApply (so the cvar doesn't drift back to 1); D3DRS_FOGENABLE is also forced 0 in hk_SetRenderState as belt-and-braces.

Note: writing to clipFar / fogFar / shutdownDistance directly does NOT affect draw distance — those values reach DOF math (sub_647FBF onwards) and other unrelated paths. The actual draw-distance gate is the per-camera frustum cached at outer+0xD4 (see frustum buffer table above).

## D3D transform-state stacks (six indexed slots)

`game_d3d_select_transform_stack(N)` (sub_5626D0) sets `g_d3d_transform_state` to `dword_6FBD40[N]`. The static array at `0x6FBD40` is:

| index | value | D3D constant |
|------:|------:|--------------|
| 0     | 3     | D3DTS_PROJECTION |
| 1     | 2     | D3DTS_VIEW |
| 2     | 16    | D3DTS_TEXTURE0 |
| 3     | 17    | D3DTS_TEXTURE1 |
| 4     | 18    | D3DTS_TEXTURE2 |
| 5     | 19    | D3DTS_TEXTURE3 |

`D3DTS_WORLD` (256) is handled separately (not in this array). Subsequent `wrap_SetTransform(matrix)` calls write the matrix to whichever state is currently selected. So `select_transform_stack(0); wrap_SetTransform(M)` sets M as the PROJECTION matrix.

This explains why `sub_4BC890`'s loop at `0x4BC99B`/`0x4BC9A7` was observed setting per-item PROJECTION matrices with `m00=18, m11=32` (≈3.5° FOV, near-orthographic) — that's the per-item projection setup for some pass (shadow caster / light view), not HUD.

## Per-object visibility test — `sub_4E0B90` (the cull gate)

`sub_4E0B90` at `0x004E0B90` is a 1-instruction SecuROM thunk:
`jmp dword ptr [byte_F5F876+0x340BE]` — the indirect-jump target lives at
**`0xF92F34`** (the IAT slot). At runtime SecuROM resolves this slot to the
real impl in a decrypted segment.

Calling convention: `__cdecl` (caller-cleanup, every call site does
`add esp, 18h` after — confirmed by IDA disasm at all 4 sites). Returns a
struct pointer; callers use only `AL` as "is visible" flag.

### Four call sites

| addr      | function         | role |
|-----------|------------------|------|
| `0x4BC406` | `sub_4BC340`    | **scene-tree visibility list update** (upstream — the result is stored in a per-node visibility byte array at `*((BYTE*)v2 + idx + 152)`; downstream rendering reads from this array) |
| `0x4C385D` | `sub_4C3790`    | main per-object render loop (downstream, gated by v2+149 flag) |
| `0x4CBAC7` | unrecognized fn | real CALL in unanalyzed code; same `add esp, 18h` cleanup |
| `0x4E6A5A` | `sub_4E6A20`    | reflection probe path |

### Override mechanisms in mtr-asi

1. **`mtr::force_vis`** (call-site rewrite): patches all four sites with
   `mov al, 1; nop*3` (5 bytes — same length as `call rel32`). Bypass is
   atomic (single 5-byte write per site, restored on toggle off). All 4
   sites verified via `is_call_e8` check before patch.

2. **`mtr::vis_test_probe`** (IAT-slot patch): replaces `*(0xF92F34)` with
   a wrapper. All 4 thunk callers route through it. No MinHook trampoline
   (just an indirect-jump retarget). Wrapper:
   - counts pass/fail per call site (via `_ReturnAddress() == site_va + 5`)
   - optionally short-circuits to return 1 (force-pass)
   - forwards via the saved real impl pointer
   Polled-install (up to 30 s) since the slot is SecuROM-resolved at runtime.

### Findings (as of 2026-05-05)

- Periphery cull (`MeshLOD.PeripheryRejectAngle/Dist` cvars) DOES exist with
  proper RE'd encoding (`cos²(deg)` storage, see `cvar-system.md`) but the
  user-visible "corner culling" symptom did NOT respond to setting the
  angle to `cos²(90°) = 0` and dist to `1e12`. Periphery is real but not
  the gate the user is hitting.

- `force_vis` (4 call sites) ALSO did not address the corner-cull symptom.
  The cull is somewhere else entirely — possibly:
  - Per-scene-flag `+104 bit 0` (sub_4BC340 zeros visibility for whole
    nodes when this is set; sub_4BC890 skips work for them)
  - A different code path (sub_4BC890 walks the scene tree and may have
    its own per-item logic before vis_test runs)
  - Per-actor distance-fade or sector visibility that runs higher up

- Next step: use `vis_test_probe` to characterize what fraction of objects
  fail vis_test in normal gameplay vs when the user reports corners. If the
  pass-rate is near 100% (i.e., vis_test isn't culling anything), the gate
  is upstream of the test — confirms force_vis was a dead lever.

## Probes used during RE (now removed from build)

These were temporary; their findings are baked into this doc and the IDB names. The file [`src/mtr-asi/src/camera_probe.cpp`](../../src/mtr-asi/src/camera_probe.cpp) is kept as an artifact but excluded from the build.

- **Camera struct probe** (`hk_CameraCompute` callback): tracked unique `outer_cam` addresses and dumped the embedded projection-cache + diff per-frame to discover the projection-cache layout.
- **Outer-cam probe** (`hk_CameraApply` callback): hooked `sub_4C1BA0` to capture the apply argument, dumped 1024 bytes of the outer struct, decoded the world matrix at `+0x2B0`. Confirmed `outer+0x40 = projection-cache` (offset diff between the two pointer types is exactly 0x40).
- **View matrix probe** (matched against a tracked outer cam): confirmed eye-position decode by recovering `(102.38, 82.60, 35.30)` from both row3 of the world matrix and inverse-of-view computation, on the same frame.
- **`matrix4_copy` probe**: hooked the copy function and filtered for destinations matching any tracked view-matrix slot. **Single caller** in 30 seconds of walking gameplay: address `0x58C9A4` (= return address from `0x58C99F`, inside `PathCam_tick`).
- **Camera-controller probe** (`PathCam_tick` callback): captured `this` pointer + first 64 bytes of the controller. Vtable at `+0x00` resolved to `0x6C9AE0`; ASCII bytes at `+0x14` resolved to `"PathCam\0"`.
