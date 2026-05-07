# ImGui menu architecture (D3D9 injection)

**Status: SHIPPED (2026-05-04).** Implementation in `src/mtr-asi/src/menu.cpp` + `src/mtr-asi/src/d3d9_hook.cpp`.

## Why D3D9 (not D3D8)

The game uses D3D8, but it loads through dxwrapper's `d3d8to9` which translates every D3D8 call into a D3D9 call against the real `d3d9.dll`. The actual GPU device is `IDirect3DDevice9`, wrapped by dxwrapper's `m_IDirect3DDevice9Ex`. Game sees the wrapper, doesn't know d3d9 is underneath.

Implication: hooking at the D3D9 layer in `d3d9.dll` catches game's render calls regardless of whether they originated as D3D8 or were translated.

ImGui has a maintained `imgui_impl_dx9` backend; D3D8 has no first-party ImGui backend (community DX8 backends exist but are unmaintained). One more reason to hook at D3D9.

## Vtable hooks (dummy device pattern)

Standard pattern; works for any process that loads d3d9.dll:

```cpp
HMODULE d3d9_mod = GetModuleHandleA("d3d9.dll");          // wait until loaded
auto pCreate9 = (Direct3DCreate9_t)GetProcAddress(d3d9_mod, "Direct3DCreate9");
IDirect3D9* d3d = pCreate9(D3D_SDK_VERSION);
void** d3d_vt = *(void***)d3d;                            // IDirect3D9 vtable
void* p_CreateDevice = d3d_vt[16];

HWND hidden = CreateWindowExA(...);                       // never shown
D3DPRESENT_PARAMETERS pp = {.Windowed=TRUE, ...};
IDirect3DDevice9* dummy;
d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hidden,
                  D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                  &pp, &dummy);
void** dev_vt = *(void***)dummy;                          // IDirect3DDevice9 vtable
void* p_EndScene     = dev_vt[42];
void* p_Reset        = dev_vt[16];
void* p_SetTransform = dev_vt[44];

// Release dummy + DestroyWindow — vtables are shared module-wide, hooks survive

MH_CreateHook(p_CreateDevice, ...);
MH_CreateHook(p_EndScene,     ...);
MH_CreateHook(p_Reset,        ...);
MH_CreateHook(p_SetTransform, ...);
MH_EnableHook(MH_ALL_HOOKS);
```

Vtable indices (IDirect3DDevice9, from `d3d9.h`):
- 16: Reset
- 17: Present
- 41: BeginScene
- 42: EndScene
- 43: Clear
- 44: SetTransform

Vtable indices (IDirect3D9):
- 16: CreateDevice

## ImGui lifecycle

Lazy-init on first `EndScene`:

```cpp
void on_end_scene(IDirect3DDevice9* dev) {
    if (!g_imgui_ready) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        dev->GetCreationParameters(&cp);          // get focus HWND
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(cp.hFocusWindow);
        ImGui_ImplDX9_Init(dev);
        // Subclass WndProc here too (g_orig_wndproc + ours)
        g_imgui_ready = true;
    }
    // poll input -> NewFrame -> draw menu -> Render -> RenderDrawData
}
```

On `Reset` (alt-tab, resize): `ImGui_ImplDX9_InvalidateDeviceObjects` before, `ImGui_ImplDX9_CreateDeviceObjects` after.

## Input handling: DirectInput exclusive defeats WndProc

The game uses `DirectInput8Create` and (presumed) `DISCL_EXCLUSIVE | DISCL_FOREGROUND` cooperation level on its keyboard/mouse devices. Effect: Windows delivers raw input directly to DirectInput, **WM_KEYDOWN / WM_LBUTTONDOWN / etc. never reach our WndProc subclass**.

Our `subclass_wndproc` calls `ImGui_ImplWin32_WndProcHandler` correctly, but no input events arrive, so `IsKeyPressed`, mouse clicks, etc. all fail silently.

### Fix: poll directly, feed ImGui via `io.AddXxxEvent`

In `on_end_scene` *before* `NewFrame`:

```cpp
ImGuiIO& io = ImGui::GetIO();

POINT pt;
if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt))
    io.AddMousePosEvent((float)pt.x, (float)pt.y);

static bool prev[3] = {};
bool now[3] = {
    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
    (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0,
};
for (int i = 0; i < 3; ++i)
    if (now[i] != prev[i]) { io.AddMouseButtonEvent(i, now[i]); prev[i] = now[i]; }

static bool prev_ins = false;
bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
if (ins && !prev_ins) g_visible = !g_visible;
prev_ins = ins;
```

`GetAsyncKeyState` reads system-wide state and **bypasses DirectInput's exclusive grab** for these keys. Mouse position via `GetCursorPos` is always available.

### Cursor visibility

Game calls `ShowCursor(0)` on `WM_ACTIVATEAPP` (we saw it in `game_MainWndProc`). System cursor is hidden. Solution: `io.MouseDrawCursor = g_visible;` — when menu is open, ImGui draws its own cursor; when closed, system stays hidden as game intended.

### WndProc subclass — keep but don't depend on it

`subclass_wndproc` is still installed (chained via `CallWindowProcA(g_orig_wndproc, ...)`). Reasons:
- Other input messages (focus changes, system events) still flow normally.
- `WM_MOUSEWHEEL` is sometimes delivered even with DirectInput exclusive (varies by game). Free win if it works.
- Can selectively swallow keys/mouse messages when menu is visible to keep them from reaching the game's WndProc (defensive).

## Files

- `src/mtr-asi/src/menu.cpp` — ImGui state, WndProc subclass, input polling, draw loop
- `src/mtr-asi/src/d3d9_hook.cpp` — vtable capture, MinHook setup, EndScene/Reset/SetTransform/CreateDevice hooks
- `src/mtr-asi/third_party/imgui` — vendored Dear ImGui v1.91.5 (master branch checkout)
