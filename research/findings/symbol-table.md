# Symbol table — Wilbur.exe + Launcher.exe

Authoritative mapping of original IDA names (`sub_XXXXXX` / `unk_XXXXXX` / `dword_XXXXXX`) to our renames, with addresses, brief role, and the IDA database where the rename was applied. Updated whenever we rename anything in IDA.

**Image bases (no ASLR — VA == runtime address):**
- Wilbur.exe: `0x00400000` — IDB at `ida/400000.Wilbur.exe.i64`
- Launcher.exe: `0x00400000` — IDB at `ida/Launcher.exe.i64`

Refresh procedure when renaming: use `mcp__ida-pro-mcp__rename` (batch mode), then `mcp__ida-pro-mcp__idb_save`, then update the relevant table row here.

---

## Wilbur.exe — display & projection pipeline

| VA           | Original    | Our name                                     | Role |
|--------------|-------------|----------------------------------------------|------|
| `0x00561C10` | `sub_561C10` | `game_GetCameraAspect`                      | Returns `g_window_client_width / g_window_client_height` when `g_window_show_state != 3`; otherwise returns the dead-code constant `1.3333334`. |
| `0x00561C50` | `sub_561C50` | `game_camera_init_with_defaults`            | Camera-init: fills near/far/FOV/etc from globals if camera fields equal the hard-coded defaults; ends with `game_camera_setup(this, 0,0, 1.0, 1.0)`. |
| `0x00562B20` | `sub_562B20` | `game_BuildPerspectiveMatrix`               | `__cdecl(fov_deg, aspect, near, far)` — thin shell: deg→rad, calls `game_PerspectiveFovRH`, then `IDirect3DDevice::SetTransform` via vtable[156]. **Hooked by mod (`hk_BuildProjMatrix`).** Four call sites: `game_camera_recompute_projection` (main, aspect from camera+12), `sub_4BCE00` + `sub_4BD210` (both pass `aspect=1.0` — shadow/reflection probes), and `game_render_overlay_quad_if_enabled` (`0x004B1150`, also `aspect=1.0`). Three non-main-camera callers all hardcode `aspect=1.0`; our hook over-substitutes them. See aspect-ratio-fix.md. |
| `0x004B1150` | `sub_4B1150` | `game_render_overlay_quad_if_enabled`       | Renders an overlay/blit quad when `dword_729E7C` is non-null. Sets up fixed 45°/1.0/0.1/5.0 perspective, pushes identity-with-(-0.5,-0.5)-translation transform, draws a screen-space quad via `sub_5623F0`. Defined as a function manually (was an orphan in IDA — `0xCC` pad ends at `0x4B114F`, function starts at `0x4B1150`). |
| `0x0063CAF7` | `sub_63CAF7` | `game_PerspectiveFovRH`                     | Right-handed perspective matrix builder. `m[0]=cot(fovY/2)/aspect`, `m[5]=cot(fovY/2)`, `m[10]=far/(near-far)`, `m[11]=-1`, `m[14]=far*near/(near-far)`. Sole caller: `game_BuildPerspectiveMatrix`. |
| `0x005625C0` | `sub_5625C0` | `wrap_SetTransform_state`                   | Thin wrapper, calls device's vtable[148] (a SetX-style state call). |
| `0x005625E0` | `sub_5625E0` | `wrap_SetTransform`                         | `__cdecl(matrix*)` — reads `g_d3d_transform_state`, calls `IDirect3DDevice::SetTransform(device, state, matrix)`. **Diagnostic hook — caller via `_ReturnAddress()`.** |
| `0x005626D0` | `sub_5626D0` | `game_d3d_select_transform_stack`           | `void(int idx)` — sets the index into `dword_6FBD40[]` table of transform-state stacks (idx 0 = world/proj stack used by main camera). Stores idx in `unk_729910`, fetches stack pointer into `g_d3d_transform_state`. |
| `0x00562650` | `sub_562650` | `game_d3d_push_transform_state`             | Pushes current transform stack frame: increments `dword_729914[idx]`, calls device vtable[152] with a 64-byte slot pointer derived from `(3*idx + count) * 64 + 0x72A030`. Pair with `_pop_transform_state`. |
| `0x00562690` | `sub_562690` | `game_d3d_pop_transform_state`              | Pops current transform stack frame: decrements `dword_729914[idx]`, calls device vtable[148] with paired slot pointer. |
| `0x00562CB0` | `sub_562CB0` | `game_d3d_get_current_transform_matrix`     | `int(int idx)` — returns pointer to the active transform matrix (constant 7511600 = `0x72A170`). Used by `game_camera_recompute_projection` to copy the freshly-built projection into the camera's cached slot. |
| `0x004C5FC0` | `sub_4C5FC0` | `matrix4_copy`                              | `_DWORD* __thiscall(this, src)` — straight 16-dword copy (a 4×4 matrix). 26 callers across the engine. |
| `0x004C08A0` | `sub_4C08A0` | `game_world_to_screen_with_nearclip`        | Project a world-space point to screen coords; rejects (`return 0`) if behind the near plane (`v4[2]*x + v4[10]*z + v4[6]*y + v4[14] > -near`). Calls `game_camera_recompute_projection` to ensure projection is up-to-date, then perspective-divide + viewport scale. |
| `0x00564550` | `sub_564550` | `game_camera_setup`                         | Caches `*(this+12) = game_GetCameraAspect() * (a4/a5)` and stores window-derived viewport ints in this+136/+140. |
| `0x00564600` | `sub_564600` | `game_camera_recompute_projection`          | "Rebuild projection if dirty" gateway. If `*(this+112)` set, calls `game_BuildPerspectiveMatrix(fov, aspect, near, far)` and copies result via `game_d3d_get_current_transform_matrix → matrix4_copy → camera+0x30..0x6F`, clears flag. **Hooked by mod (`hk_CameraCompute`) for cache invalidation.** |
| `0x00564650` | `sub_564650` | `game_camera_apply_state`                   | Per-frame camera tick. Calls `game_camera_recompute_projection` then submits via `wrap_SetTransform`. |
| `0x005635E0` | _undefined_ | `game_window_show_and_update_client_size`   | `ShowWindow(g_focus_hwnd, g_window_show_state); GetClientRect(...) → g_window_client_width/height`; later calls `UpdateWindow`. Not auto-discovered by IDA originally; defined as a function manually. |
| `0x004DF2C0` | `sub_4DF2C0` | `game_camera_build_view_frustum`            | `__cdecl(out_buf*, near, far, has_extra_clip, extra_axis, extra_d, fov_deg, aspect)`. Builds 6 frustum planes + optional clip plane + 4 near-plane corners into out_buf. Called from `game_camera_apply_state` ONLY when `*(camera+144)` dirty. **Aspect input controls left/right plane angles** — this is the cull frustum's aspect knob. See aspect-ratio-fix.md. |
| `0x004DF1F0` | `sub_4DF1F0` | `game_camera_apply_optional_clip_plane`     | Conditionally writes a 7th clip plane at `out_buf+96..127` and the corner-rectangle metadata at `+448`/`+449`. Called from `game_camera_build_view_frustum`. |
| `0x00562B70` | `sub_562B70` | `build_ortho_matrix`                        | `__cdecl(l, r, t, b, n, f)` — calls `matrix4_make_ortho` (sub_63CB8B), then routes the matrix through `dword_72E67C[156]` (slot 39 = `MultiplyTransform`-style). **Hooked by mod (`hk_BuildOrtho`)** — but only fires for debug overlay (sub_41C788) at runtime; HUD path bypasses it. |
| `0x00562AA0` | `sub_562AA0` | `transform_apply_scale_via_stack`           | `__cdecl(sx, sy, sz)` — calls `matrix4_make_scale` (sub_63C4E3) then routes via `dword_72E67C[156]`. Sole caller: `render_sprite_batcher` (0x4E8DDA, args 2.0, -2.0, 1.0). **Hooked by mod (`hk_MatrixSetXformA`)** for sprite-batcher matrix override path. |
| `0x00562AE0` | `sub_562AE0` | `transform_apply_translate_via_stack`       | `__cdecl(tx, ty, tz)` — calls `matrix4_make_translate` (sub_63C573) then routes via `dword_72E67C[156]`. Sole caller: `render_sprite_batcher` (0x4E8DEB, args -2.0, -2.0, 0.0). **Hooked by mod (`hk_MatrixSetXformB`)** for sprite-batcher matrix override path. |
| `0x0063C4E3` | `sub_63C4E3` | `matrix4_make_scale`                        | `__stdcall(out_buf, sx, sy, sz)` — pure diagonal scale matrix `diag(sx, sy, sz, 1)`, all other slots = 0. Reachable via `dword_715B64` thunk. |
| `0x0063C573` | `sub_63C573` | `matrix4_make_translate`                    | `__stdcall(out_buf, tx, ty, tz)` — pure row-3 translation matrix; `m[3][0..2] = (tx, ty, tz)`, identity diagonal, other slots = 0. Reachable via `dword_715B48` thunk. |
| `0x0063CB8B` | `sub_63CB8B` | `matrix4_make_ortho`                        | Builds an orthographic projection matrix from `(out, l, r, t, b, n, f)` bounds. Sole caller: `build_ortho_matrix` (sub_562B70). |
| `0x004E8D30` | `sub_4E8D30` | `render_sprite_batcher`                     | Per-frame sprite/2D batcher. Walks the linked list at `unk_7271E8` and renders each via `render_draw_primitive_dispatch` (sub_5692A0) → DrawPrimitive vtable[280]. Sets per-frame projection+view via `transform_apply_scale_via_stack(2,-2,1)` + `transform_apply_translate_via_stack(-2,-2,0)`. Vertex format = pure XYZ (12 B/vert) NOT XYZRHW. Single call site: `render_frame_top_level` at 0x4D23BF. **Strongest HUD path candidate** — confirmation pending Test 7b. |
| `0x005692A0` | `sub_5692A0` | `render_draw_primitive_dispatch`            | DrawPrimitive call dispatcher. Selects primitive type by `a1`: case 5 = quads (4 verts/quad, prim type 6 = D3DPT_TRIANGLELIST, 2 prims/quad). Calls `dword_72E67C[280]` (slot 70 = `DrawPrimitive`), `[332]` (slot 83 = `SetStreamSource`), `[304]` (slot 76 = `SetVertexShader`/SetFVF). |

### Display globals

| VA           | Original     | Our name                  | Notes |
|--------------|--------------|---------------------------|-------|
| `0x006C750C` | _4-byte float_ | (no name)                | `1.3333334f` (dead-code aspect fallback referenced by `game_GetCameraAspect`). Don't patch — `g_window_show_state` is statically `1`, never `3`, so the path is unreachable. |
| `0x006FBC38` | (already named) | `g_window_client_width` | Set by `game_window_show_and_update_client_size` from `GetClientRect` of focus window. |
| `0x006FBC3C` | (already named) | `g_window_client_height`| ↑ |
| `0x006FBC7C` | `unk_6FBC7C` | `g_focus_hwnd`            | HWND of game's focus window (created in `CreateWindowExA`). |
| `0x006FBD14` | `unk_6FBD14` | `g_window_show_state`     | `ShowWindow` cmd parameter. Statically initialised to `1` (`SW_NORMAL`). No code writes to it — purely const. |
| `0x006FBD58` | `unk_6FBD58` | `g_d3d_transform_state`   | D3DTRANSFORMSTATETYPE value used by `wrap_SetTransform` (`3` = `D3DTS_PROJECTION`). |

### `game_camera_setup` arguments + camera struct field map

| Offset | Field                       |
|--------|-----------------------------|
| +0     | `near` plane (float)        |
| +4     | `far` plane (float)         |
| +8     | FOV degrees (float)         |
| +12    | `aspect` (float, cached)    |
| +16    | (a1[4]) misc, default 50.0  |
| +20    | (a1[5]) misc, default 70.0  |
| +24    | byte flag — uses `byte_6FBC5C` |
| +25..  | further config dwords sourced from `*_6FBC5*` globals |
| +48..+111 | 4×4 projection matrix cache (16 floats) — populated by `game_camera_recompute_projection` from `game_d3d_get_current_transform_matrix(0)` after a fresh `game_BuildPerspectiveMatrix` build. `game_camera_recompute_projection` returns `&camera+48` so callers can read the matrix here directly. |
| +112   | "projection dirty" byte flag — recompute if non-zero. Cleared at end of `game_camera_recompute_projection`. |
| +116   | `a2` argument to `game_camera_setup` (camera-id?) |
| +120   | `a3` argument                |
| +124   | `a4` (viewport-x scale, float, default 1.0) |
| +128   | `a5` (viewport-y scale, float, default 1.0) |
| +132   | "secondary dirty" byte flag — `game_camera_apply_state` calls `sub_562BC0` when set. |
| +136   | `g_window_client_width  * a4` (int, viewport pixels) |
| +140   | `g_window_client_height * a5` (int, viewport pixels) |
| +144   | "transform dirty" byte flag — when set, `game_camera_apply_state` calls `sub_4DF2C0` with this+0..28 to rebuild a transform stored at this+148..595. |
| +148..+595 | Cull frustum buffer rebuilt by `game_camera_build_view_frustum` (`sub_4DF2C0`). Layout: +148..163 near plane, +164..179 far, +180..195 top, +196..211 bottom, +212..227 left, +228..243 right, +244..275 optional 7th plane + duplicate near, +404..420 4 near-plane view-space corners (relative offsets +0..+272 inside `out_buf`). |
| +596, +597 | Bytes — used for cached comparison in `game_camera_apply_state`. |
| +616   | "apply_state in progress" byte — set 1 at start of `game_camera_apply_state`, 0 at end. |
| +617..+619 | Cached copies of this+24, this+596, this+597 used to detect changes between frames. |

---

## Wilbur.exe — main loop & engine clock

| VA           | Original    | Our name                              | Role |
|--------------|-------------|---------------------------------------|------|
| `0x0056F6E0` | `sub_56F6E0` | `game_MessageLoop`                   | WinMain body. `SetThreadAffinityMask(_, 1)` + standard PeekMessage/GetMessage pump. PeekMessage-empty branch invokes `App->vtable[4]` = per-frame idle tick. Called from CRT startup at `0x0062B5C0`. See [frame-pacing.md](frame-pacing.md). |
| `0x0056F757` | _call site_ | (per-frame tick)                     | `g_App_singleton[0]->vtable[4]()` — game update + render. FPS limiter hooks `hk_EndScene` (already owned) rather than wrapping this. |
| `0x004A3CCE` | `sub_4A3CCE` | `game_get_time_ms`                   | Engine ms clock. Cached or QPC-derived. 30+ callers (animation/AI/physics) — game logic is largely time-based. |
| `0x00584670` | `sub_584670` | `game_init_perf_timer`               | One-time QPC base + frequency setup at `0x00741310..0x00741378`. |
| `0x00584710` | `sub_584710` | _thunk to `game_get_time_ms`_        | What the rest of the engine actually calls. |
| `0x0072F710` | `unk_72F710` | `g_App_singleton`                    | `[0]` = App instance, `[2]` = hInstance, populated by `game_MessageLoop`. |

---

## Wilbur.exe — Bink integration

| VA           | Original     | Our name                                    | Role |
|--------------|--------------|---------------------------------------------|------|
| `0x0055C190` | `sub_55C190` | `game_bink_play_video`                      | Bink playback driver. Sole caller of `BinkOpen` in Wilbur. Statically has zero xrefs (callers live inside SecuROM-VM regions and dispatch via indirect trampolines). On `BinkOpen → NULL`, returns `0` cleanly — game treats as "video done", proceeds to next stage. Hook target for non-rename intro skip; full design in [bink-integration.md](bink-integration.md). |
| `0x00583E50` | `sub_583E50` | `game_string_normalize_to_static_buffer`    | Normalizes a path/string buffer through a SecuROM-VM helper (`sub_586780`) and returns pointer to a process-global static buffer at `0x740F90`. Used to convert `path_buf[260]` → the form `BinkOpen` accepts. |

### DirectInput IAT slot

| IAT slot     | Symbol                     | Module    | Notes |
|--------------|----------------------------|-----------|-------|
| `0x006A6020` | `DirectInput8Create`       | `dinput8` | Zero static xrefs (SecuROM-VM dispatched). Hook target for the input-cleanup design in [dinput-cleanup.md](dinput-cleanup.md). |

### Bink IAT slots (in `.idata` at `0x006A6300..0x006A633C`)

| IAT slot     | Symbol                         |
|--------------|--------------------------------|
| `0x006A6300` | `_BinkPause@8`                 |
| `0x006A6304` | `_BinkSetSoundSystem@8`        |
| `0x006A6308` | `_BinkOpenDirectSound@4`       |
| `0x006A630C` | `_BinkWait@4`                  |
| `0x006A6310` | `_BinkSetVolume@12`            |
| `0x006A6314` | `_BinkOpen@8` ← hook target if going IAT |
| `0x006A6318` | `_BinkSetSoundTrack@8`         |
| `0x006A631C` | `_BinkNextFrame@4`             |
| `0x006A6320` | `_BinkCopyToBuffer@28`         |
| `0x006A6324` | `_BinkDX8SurfaceType@4`        |
| `0x006A6328` | `_BinkDoFrame@4`               |
| `0x006A632C` | `_BinkGetError@0`              |
| `0x006A6330` | `_BinkSetSoundOnOff@8`         |
| `0x006A6334` | `_BinkClose@4`                 |
| `0x006A6338` | `_BinkCopyToBufferRect@44`     |

A second IAT mirror at `0x00F5F83A..0x00F5F876` lives inside the SecuROM-decrypted `rr02` segment; some callers reach Bink through this mirror. Inline hook on `binkw32!BinkOpen` catches both; IAT hook on `0x6A6314` only catches the regular `.idata` path.

---

## Wilbur.exe — registry / window persistence

| VA           | Original    | Our name                          | Role |
|--------------|-------------|-----------------------------------|------|
| `0x00563060` | `sub_563060` | `game_LoadWindowSizeFromRegistry`| Reads "window pos x/y", "window width/height" from HKCU registry. |
| `0x0056F7B0` | `sub_56F7B0` | `game_SaveWindowSizeToRegistry`  | Inverse — saves window geometry on exit. Called from `game_MainWndProc`. |

### Window-persistence globals

| VA           | Original     | Our name                     | Notes |
|--------------|--------------|------------------------------|-------|
| `0x006FE188` | `unk_6FE188` | `g_window_width`             | Persisted window width (NOT rendering resolution). |
| `0x006FE18C` | `unk_6FE18C` | `g_window_height`            | ↑ |
| `0x006FE190` | `unk_6FE190` | `g_window_pos_x`             | ↑ |
| `0x006FE194` | `unk_6FE194` | `g_window_pos_y`             | ↑ |
| `0x0072F704` | `unk_72F704` | `g_save_window_size_flag`    | When set, `WM_DESTROY` triggers save-to-registry. |

---

## Launcher.exe (renamed in earlier session)

170+ functions named — full list lives in [launcher-internals.md](launcher-internals.md). Headline entries:

| VA           | Our name                              | Role |
|--------------|---------------------------------------|------|
| `0x004047C0` | `Launcher_LaunchGame`                 | Constructs `Wilbur.exe -dxfullscreen -dxadapter=0 -dxresolution=WxH [-dxdiskdriveletter=L] -launchit` cmdline and `CreateProcess`'s. (Note: emits malformed cmdline with no space between `WxH` and `-dxdiskdriveletter`.) |
| `0x00403F40` | `Launcher_Settings_Save`              | Writes user settings to HKCU registry. |
| `0x004042F0` | `Launcher_Settings_Load`              | Inverse. |
| `0x00409050` | `CMyApp_InitInstance`                 | MFC entry. |
| `0x00408300` | `Launcher_DInput_Init`                | Sets up DirectInput8 for keymap probing. |
| `0x00401E50` | `Launcher_D3D9_EnumerateAllModes`     | Builds the resolution list shown in the launcher dropdown. |
| `0x004092D0` | `CMainSettingsDialog_OnInitDialog`    | Tab/page setup. |
| `0x004095E0` | `CMainSettingsDialog_AllocPages`      | Allocates the per-tab MFC page objects. |

---

## Console / REPL infrastructure

The engine's REPL processor is fully present in retail Wilbur — just no input UI. **Phase 1 — full RE of API surface — completed 2026-05-05.** See [debug-features.md §In-game console](debug-features.md) for restoration plan.

| VA           | Our name                              | Role |
|--------------|---------------------------------------|------|
| `0x0057D630` | `jenkins_lookup2_hash`                | **Bob Jenkins lookup2** hash. Sig `(buf, len, seed=0)`. Mixing constants 13/8/13/12/16/5/3/10/15. Backbone of cvar lookup. |
| `0x005873A0` | `console_printf`                      | Print sink. Routes through SecuROM-thunked low-level output. Hook target for ring buffer. |
| `0x005873F0` | `console_printf_arg`                  | Single-arg printf variant. |
| `0x00587440` | `console_set_active_context`          | Internal: writes `state[5] = ctx_idx`. |
| `0x00587500` | `console_verbosity_cmd`               | Built-in `Verbosity` cvar handler. Maps `none`/`error`/`notify`/`debug` to int 0..3. Demonstrates command-handler signature `char(state, name, value_str)`. |
| `0x00587740` | `console_tokenize_token`              | Tokenizer: skip whitespace, copy until separator-set, return next-input. Used by dispatcher and run-text-script. |
| `0x00587A70` | `console_resolve_context`             | Resolve context by name → idx ≥ 0 or -1. Caches at `state[4]`. |
| `0x00587DE0` | `console_resolve_in_script_text`      | Scans `.ini`-style text body for `name = value`. Handles `\` continuation, `;`/`\n` separators, slash→backslash. Engine's `Robinsons.ini` parser hook. |
| `0x00587F90` | `console_save_context_cmd`            | `save [ctx]? [path]?` writeconfig. Filters write-only and hidden. |
| `0x00588210` | `console_set_context_cmd`             | `context [name]` built-in. |
| `0x005882C0` | `console_list_vars_cmd`               | `list [ctx]?` built-in. |
| `0x005884C0` | `console_set_or_invoke_var_cmd`       | Fallback when no built-in matches. Read if no value, set+commit if value. Handles `[ctx]name` and `name = value`. |
| `0x00588B20` | `console_register_context`            | Append context to `state[2]` list (after dup-check). |
| `0x00588DB0` | `console_dispatch_line`               | **THE DISPATCHER.** `(state, line) → ok`. `#`-comment skip → tokenize → linear-scan command table at `state[10..]` (size `state[11]`, 12-byte entries `{fn, name, ?}`) → fall through to set/invoke. |
| `0x00588EB0` | `console_get_or_create_context`       | Public get-or-create. NULL/empty selects defaults; miss allocates 236-byte ctx, calls init(name, flag=0, capacity=32), registers. |
| `0x00588FB0` | `console_run_text_script`             | Multi-line text-script runner. Per line: handle `\` continuation, `;`/`\n` separators, `/`→`\`, tab→space, then `console_dispatch_line`. |
| `0x00589350` | `console_run_script`                  | Run a registered script with optional args. |
| `0x005890B0` | `console_create_ctx_run_script`       | get_or_create_context + run_script. |
| `0x005894D0..0x00589A50` | `console_register_var_typed_a..h` | 8 typed cvar registration wrappers (one per type: int/float/bool/string/...). |
| `0x0058A060` | `console_iter_next_var`               | Var iterator. `(ctx, *cookie) → var_ptr or 0`. Linear walk `ctx[22..]`. |
| `0x0058A090` | `console_lookup_var_in_ctx`           | Hash-table lookup. `bucket = jenkins_lookup2_hash & 0x1F`; head at `ctx[27 + bucket]`; chain via entry's next_idx. 32 buckets. |
| `0x0058A1A0` | `console_context_init`                | Context constructor. `(this, name, flag, capacity)`. 8*capacity entry array; 32 bucket-heads at -1. Total ctx size = 236 bytes. |
| `0x0058A2D0` | `console_context_add_var`             | Register cvar in context. Dup-checks, hashes, prepends to bucket chain. Var must implement vtable[0..16]. |
| `0x0058A360` | `console_lookup_var_by_name`          | Public lookup wrapper: strncpy(31), null-term, lowercase, delegate. |
| `0x00570092` | (in `game_App_ctor`)                  | Initial write of `g_console_state` (`mov g_console_state, eax`). All cvar registrations through the binary depend on this happening first. mtr-asi hooks install in deferred init, after this. |
| `0x007415E0` | `g_console_state`                     | **Global console state singleton (DWORD pointer).** Dereference once, pass result to all `console_*` API calls (`__thiscall`). |
| `0x006A6458` | `s_event_ConsoleVarModified`          | Named event ("ConsoleVarModified"); 4 subscriber registrations. Fires on `commit_or_notify` (vtable[16]). |
| `0x006C9258..0x006C9408` | _data_ | console output text cluster        | All console UI strings: `Global context`, `[%s] %s = %s`, `Variables in %s:`, etc. Includes typo'd retail string `"Somethingis wrong, no context set!"`. |

### Cvar object layout

```
+0   void* vtable
   vtable[0]:  const char* (*get_name)(this)
   vtable[4]:  const char* (*get_value_string)(this)
   vtable[8]:  int  (*parse_from_string)(this, const char* buf)   // staged value
   vtable[12]: int  (*read_to_buf)(this, char* buf, int max_len)  // 0 = success
   vtable[16]: void (*commit_or_notify)(this)                      // fires ConsoleVarModified
+4   uint32 flags  (bit1 = simple display, bit3 = write-only, bit4 = hide-from-save)
```

### State / context dword-index layouts

```
g_console_state[2]   contexts list ptr (struct {ctx_ptr* arr, count, cap, growth})
g_console_state[4]   last-resolved cache idx
g_console_state[5]   active context idx
g_console_state[6]   text-script work buffer ptr
g_console_state[7]   text-script work buffer size
g_console_state[10]  built-in commands table ptr (12-byte entries: {fn, name, ?})
g_console_state[11]  built-in commands count

ctx byte +0..+19    name[20] (lowercased)
ctx byte +20        persistence flag
ctx[22]   entries array ptr (8-byte entries: {var_ptr, next_idx})
ctx[23]   entries count
ctx[24]   entries capacity
ctx[27..58]         32 hash bucket heads
ctx byte +64,+68    text body ptr / live flag
```

---

## Debug features catalog

Strings catalogued from the unpacked Wilbur.exe live in [debug-features.md](debug-features.md). Notable code addresses:

| VA           | What's there                                     |
|--------------|--------------------------------------------------|
| `0x006B00A0` | `"FreeCam"` string — referenced by `sub_682480`, `sub_682770`, `sub_6827D0` (camera-registration sites with capability-bit gates) |
| `0x006B46D0` | `"Scn_Cheats_UnlockChargeballCourts"` etc. — cheat-unlock scene-name strings; loaded by name-hash, no direct xref |
| `0x006C2534` | `"ToggleDebugPanel"` — debug-panel toggle command name |
| `0x006C1F90` / `0x00719A20` | `"DevLobby"` (two copies, possibly ASCII + wide) |
| `0x006C7674..0x006C76C8` | `-dxaa`, `-dxshaderdebugging`, `-dxwindowed` flag strings |
| `0x00F003E4` | `"-letitsnow"` Easter-egg flag |

---

## Screen system & state machine

Full architecture writeup: [screen-system.md](screen-system.md). Quick reference here.

| VA           | Our name                                | Role |
|--------------|-----------------------------------------|------|
| `0x00524380` | `state_machine_set_next_state`          | `__thiscall(this, name_str)`. Writes name into `this[2]` AND `dword_741868[263]`. Pure setter; engine's tick reads slot and transitions. |
| `0x0045D880` | `state_machine_route_action`            | `__thiscall(this, action_code)`. Switch on action 0/1/2/3/4/6 — reads props (TargetGameLevel etc), queues next state, returns next-state-name. |
| `0x005A0400` | `props_get`                             | `__stdcall(key_str) → value_str`. Engine's named property store. |
| `0x005A0420` | `props_set`                             | `__stdcall(key_str, value_str)` |
| `0x005A0440` | `props_has`                             | `__stdcall(key_str) → bool` |
| `0x005A0280` | `props_set_internal`                    | inner setter that walks the array |
| `0x005A0310` | `props_get_or_init`                     | get-or-create by name |
| `0x005A0200` | `props_get_int`                         | typed int getter |
| `0x00604310` | `screen_manager_push_by_name`           | `__thiscall(this, name_str) → char`. Pushes screen via inner-manager vtable[1]. Returns 0 if name not in inner registry, 1 if pushed. mtr-asi hooks this to capture `this`. |
| `0x0060E9F0` | `screen_registry_add`                   | `__cdecl(registry, name, ctor)`. Adds factory to a registry. mtr-asi hooks for startup-registration log. |
| `0x006049A0` | `screen_register_factory`               | `__cdecl(ctor_fn, flag)`. Calls ctor → `vtable[5]` for name → `screen_registry_add`. |
| `0x0060E980` | `screen_registry_entry_init`            | Inner init for 40-byte registry entry. |
| `0x00728A30` | `g_state_machine_ptr`                   | `void**`: state-machine singleton pointer. |
| `0x00744A80` | `g_screen_registry`                     | Master screen factory registry (linked list). |

### Per-screen ctors (selected, all in 0x45Bxxx-0x45Dxxx range)

| VA           | Name                              |
|--------------|-----------------------------------|
| `0x0045BFF0` | `ScreenWilburMainMenu_ctor`       |
| `0x0045C620` | `ScreenCheats_ctor`               |
| `0x0045CB30` | `ScreenTitle_ctor`                |
| `0x0045BEA0` | `ScreenWilburArtSelect_ctor`      |
| `0x0045BF10` | `ScreenWilburArtViewer_ctor`      |
| `0x0045C0D0` | `ScreenWilburExtras_ctor`         |
| `0x0045C540` | `ScreenMovieSelect_ctor`          |
| `0x0045B870` | `ScreenWilburAFViewerMain_ctor`   |

(Full list of 56 in `Game/mtr-asi.log` after a single playthrough — log lines `screen_register: name="..." ctor=...`)

### DEVMENU dead-end

`ScreenDevMenu` is in `Game/data/screens/mainmenu.sc` (data) but **has no compiled ctor in Wilbur.exe**. mainmenu.sc is orphaned 25-to-Life leftover — not loaded at runtime. See [screen-system.md §DEVMENU dead-end](screen-system.md).

### FreeCam — full activation chain (working in mtr-asi)

| VA           | Our name                                | Role |
|--------------|-----------------------------------------|------|
| `0x00682480` | `input_source_register_freecam`         | `__thiscall(int* this)`. Per-player iteration; registers FreeCam in 3 separate registries: input source (`sub_6913B0`, alloc 360), capability (`sub_58CA80`, alloc 76), control binding (`sub_693AB0`, alloc 768). |
| `0x00682770` | `input_register_freecam_sources`        | `__thiscall(int* this)`. Registers FreeCam in input-source name database (`sub_58D330`) and binds keys via `&unk_72F4C8`. |
| `0x006827D0` | `input_set_source_freecam`              | `__thiscall(int* this, int player_idx, char enable) → char`. **Activation entry point.** Looks up current source at `+304`, desired source name at `sub_58DA10()+20`, dispatches via vtable[5]@`0x485530` (switch-to) or vtable[4]@`0x485510` (switch-from). |
| `0x006823E0` | `input_init_freecam_inactive`           | Boot init. Calls `input_source_register_freecam(g_input_mgr)` then `input_set_source_freecam(g_input_mgr, 0, 0)` to ensure default camera is active at boot. |
| `0x00745B70` | `g_input_mgr`                           | Input manager singleton. `*g_input_mgr` = player count; `g_input_mgr[1]` = per-player state array. |

mtr-asi calls `input_set_source_freecam(g_input_mgr, 0, 1)` from the Native dev tab to activate FreeCam for player 0 — engine's own setter, no bypass.

---

## Cross-reference notes

- **Aspect-ratio call chain** (active in mod): `game_camera_init_with_defaults` → `game_camera_setup` → `game_camera_apply_state` → `game_camera_recompute_projection` (HOOK) → `game_BuildPerspectiveMatrix` (HOOK) → `wrap_SetTransform` → `IDirect3DDevice::SetTransform`. Fully covered in [aspect-ratio-fix.md](aspect-ratio-fix.md).
- **Window-resolution path:** `Launcher_LaunchGame` builds cmdline → our `cmdline_hook.cpp` rewrites `-dxresolution=WxH` to monitor dims (or injects it if absent) → game's CRT parses → game asks D3D for native backbuffer → `game_window_show_and_update_client_size` runs after the window is up and writes correct `g_window_client_width/height`.
- **The dead-code aspect constant at `0x006C750C`:** referenced ONLY by the unreachable fallback in `game_GetCameraAspect`. WSGF documents patching it; on this build it has no effect. See [aspect-ratio-fix.md](aspect-ratio-fix.md) for the full explanation.
