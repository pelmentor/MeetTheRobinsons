# Launcher.exe — internals

Reverse engineering of `Game/Launcher.exe` (Disney's Meet the Robinsons 2007 PC, retail).
This is the configuration tool the user runs before the game; it prepares display
settings in registry and a `keymap.ini` next to the game executable, then `ShellExecuteExW`s
`Wilbur.exe` (or `WilburDebug.exe` in dev mode) with command-line arguments.

The IDA database is at [ida/Launcher.exe.i64](../../ida/Launcher.exe.i64). All identified
functions and MFC helper helpers are renamed in that database; this doc captures the
narrative.

---

## File metadata

| | |
|---|---|
| Path | `Game/Launcher.exe` |
| MD5 | `4bd41e2c992b25a088fe4462c468448d` |
| SHA-256 | `b59b14128ca41bb540c4deb93e16f1790d9c4a2ee849c4a39bf6225511dd0653` |
| Architecture | x86 (32-bit) |
| Image base | 0x00400000 |
| Image size | 0x67000 |
| Build | Statically linked MFC 7.x + MSVCRT |
| Total functions | 1151 (of which ~733 FLIRT-recognised library, ~388 user code) |

**No SecuROM**. Unlike `Wilbur.exe`, the launcher is not protected. PE-sieve unpack was
not needed; IDA opens it directly and Hex-Rays produces clean pseudocode.

---

## Process flow at a glance

```
start (CRT entry, 0x40c611)
 └─► wWinMain (0x418662) ──► AfxWinMain
      └─► CMyApp::InitInstance (CMyApp_InitInstance, 0x409050)
           ├─ Launcher_PrepareAppDataDir (0x408cc0)
           │   └─ SHGetFolderPathA(CSIDL_LOCAL_APPDATA) → cd %APPDATA%\Disney Interactive Studios\Meet The Robinsons
           ├─ Direct3DCreate9(D3D_SDK_VERSION=32) → app+40
           ├─ Launcher_D3D9_EnumerateAllModes (0x401e50)
           │   ├─ vt[16] GetAdapterCount, vt[20] GetAdapterIdentifier
           │   ├─ for each adapter: vt[24] GetAdapterModeCount, vt[28] EnumAdapterModes
           │   │     filter: format ∈ {22, 23, 35} (X8R8G8B8 / A8R8G8B8 / R5G6B5)
           │   │            min width / min height / min bits-per-channel
           │   ├─ Launcher_D3D9_EnumerateDeviceTypes (0x401c50)
           │   │     for each type ∈ {HAL=1, REF=3, SW=2}:
           │   │       Launcher_D3D9_FilterModesByFormat (0x401820)
           │   │         CheckDeviceFormat / multisample / depth-stencil / vertex-processing
           │   └─ qsort modes via CompareFunction (0x401350)
           ├─ CMainSettingsDialog::ctor (0x409190)  resID 102 (0x66)
           │     vtable off_425200, owns 3 page objects:
           │       CDialogPage1   resID 129 (0x81)  — Display
           │       CDialogPage2   resID 130 (0x82)  — Keyboard
           │       CDialogPage3   resID 131 (0x83)  — Gamepad
           │     CMainSettingsDialog_AllocPages (0x4095e0) news them up
           ├─ CDialog::DoModal (modal main dialog)
           │     OnInitDialog (CMainSettingsDialog_OnInitDialog, 0x4092d0)
           │       ├─ Launcher_DInput_Init (0x408300)
           │       │   ├─ DirectInput8Create
           │       │   ├─ fill DIK→display-name lookup at unk_431C08 (LoadStringW IDs 0x91..0xC9)
           │       │   └─ Launcher_DInput_EnumerateDevices (0x407b00)
           │       │       enum game controllers (sub_407860 callback, ≤ 9),
           │       │       SysKeyboard (GUID at 0x425b24), SysMouse (GUID at 0x425b34)
           │       └─ CTabCtrl::InsertItem ×3 + SetTimer 33 ms (~30 Hz)
           │     SetTimer drives:
           │       CDialogPage2 (Keyboard) → CDialogPage2_OnTimer_PollKeyboard (0x405920)
           │       CDialogPage3 (Gamepad)  → CDialogPage3_OnTimer_PollGamepadInput (0x406dc0)
           │       CGamePadConfigDialog (modal child) → _OnTimer_PollAxes (0x402fb0)
           ├─ Direct3D9::Release
           └─ CMainSettingsDialog::dtor (0x409230) → Launcher_DInput_ReleaseAll (0x407690)
```

When the user clicks **Play / OK**, `Launcher_LaunchGame (0x4047c0)` is invoked.

---

## What the launcher persists, and where

### 1. Registry — per-user display + mouse

`HKCU\SOFTWARE\Disney Interactive Studios\Meet The Robinsons\Settings`

| Value (REG_DWORD) | Field offset on dialog | Meaning |
|---|---|---|
| `resolution` | dlg[34] (byte 136) | 0=640×480, 1=800×600, 2=1024×768, 3=1280×1024 |
| `save size and position` | dlg[30] | nonzero ⇒ keep `window pos x/y` and `window width/height` values. When set to 0, all four `window pos/size` REG values are deleted. |
| `mouse look` | dlg[31] | mouselook on/off (default 1) |
| `invert mouse` | dlg[32] | invert Y (default 0) |
| `mouse sensitivity` | dlg[33] | 5..100, slider value (default 76) |
| `window pos x` / `window pos y` | — | game stores its window placement here at runtime |
| `window width` / `window height` | — | game stores its window size here at runtime |

Read: [`Launcher_Settings_Load (0x4042f0)`](../../ida/Launcher.exe.i64), then
[`Launcher_DetectAvailableResolutions (0x404130)`](../../ida/Launcher.exe.i64) walks the
mode list (built earlier by `Launcher_D3D9_EnumerateAllModes`) and toggles a per-resolution
"is supported" byte at dlg[116..119] for the four hard-coded sizes — disabling radio
buttons that the current adapter cannot reach.

Write: [`Launcher_Settings_Save (0x403f40)`](../../ida/Launcher.exe.i64).

### 2. `keymap.ini` — per-input-device button bindings

Located alongside `Wilbur.exe`. Filename per device built by
`Launcher_KeymapIni_BuildFilename (0x4066c0)` from the DirectInput device name
(`SysKeyboard_0` or `<Pad name>_<idx>`, deduplicated by index suffix).

Schema (each device file has a single section `[Button Map]`):

```ini
[Button Map]
STICK1_UP      = +RY        ; or "DigitalUp" / "Button##" / "not set"
STICK1_DOWN    = -RY
STICK1_LEFT    = -RX
STICK1_RIGHT   = +RX
STICK2_UP      = ...
...
BUTTON_START   = Button00
BUTTON_STICK1  = Button06
DPAD_UP        = ...
DPAD_DOWN      = ...
...
AXIS_DEAD_ZONE                  = 20    ; 0..100 percent
FORCE APPLICATION CONTROLLER ID = -1    ; -1 = auto, else override
```

Tokens used by the game: `+AxisN/-AxisN`, `DigitalUp/Down/Left/Right` (for hat-pad
emulation of an analog stick), `ButtonNN`, `not set`, and for keyboard the human-readable
key names from the localised string table (loaded by `Launcher_DInput_Init` from
LoadStringW IDs 0x91..0xC9 into `unk_42E... / unk_42F...` blocks).

Read: [`Launcher_KeymapIni_LoadAll (0x406a70)`](../../ida/Launcher.exe.i64) walks
all DInput devices, opens per-device file, fills dialog. Per-device readers:
[`Launcher_KeymapIni_ReadKeyboard (0x405ad0)`](../../ida/Launcher.exe.i64)
for keyboard, the gamepad path is read in the timer poll.

Write: [`Launcher_KeymapIni_SaveAll (0x4070b0)`](../../ida/Launcher.exe.i64) is the
master save (also performs an "at least one mapping bound" sanity check and warns if
the file ends up read-only). Per-device writers include
[`Launcher_KeymapIni_WriteKeyboard (0x4054d0)`](../../ida/Launcher.exe.i64).

### 3. Process launch

`Launcher_LaunchGame (0x4047c0)` builds `SHELLEXECUTEINFOW`:

```c
sei.lpFile      = L"Wilbur.exe";              // L"WilburDebug.exe" iff dlg[176] != 0
sei.lpDirectory = byte_4326C0;                // GetCurrentDirectoryA snapshot, widened
sei.nShow       = SW_SHOW;                    // = 5
sei.lpParameters =
    "-dxfullscreen -dxadapter=0 -dxresolution={640x480|800x600|1024x768|1280x1024} "
    "[-dxdiskdriveletter=<L>] "
    " -launchit"
```

The disk-drive-letter switch is filled in by
[`Launcher_FindWRRunDisk (0x403e30)`](../../ida/Launcher.exe.i64), which scans drive
letters c..z calling `GetVolumeInformationA` looking for a label `"WR_RUN"` (the retail
DVD volume label). When set, the game uses that drive for streaming asset access
instead of the install directory.

Before launch, `CCdRomDialog_ctor (0x401110)` may pop a CD insert prompt (resID 0x86)
if `Launcher_FindWRRunDisk` fails to find the disc.

Final step: `CWnd::ShowWindow(hide); CDialog::EndDialog(IDOK); ShellExecuteExW(...)`.

---

## Single-instance enforcement

`CMyApp_ctor (0x408ba0)` calls
`CreateMutexA(NULL, TRUE, "Global\\Disney_s_Meet_The_Robinsons")`. If
`GetLastError() == ERROR_ALREADY_EXISTS (183)`, the field at app[51] is left null
and `CMyApp_InitInstance` early-returns with a localised "another instance" message
box (string ID 0x88).

This means **the same mutex name guards the launcher AND the game**, so closing the
game without the launcher's clean shutdown can leave the mutex orphaned (kernel zombie),
matching `research/findings/known-issues.md` issue #4.

---

## Class layout (recovered from RTTI + ctor analysis)

The launcher is a textbook MFC dialog app:

```
CObject
└── CCmdTarget
    └── CWinThread
        └── CWinApp
            └── CMyApp                    [global theApp]
                vtable @ 0x425058 (off_425058)
                resID  — n/a (CWinApp)
                ctor   CMyApp_ctor (0x408ba0)
                dtor   CMyApp_dtor (0x408c40)
                init   CMyApp_InitInstance (0x409050)
                fields:
                  +160  CMyApp data block (initialised by CMyApp_InitDataFields, 0x401310)
                  +160 +12  width  (default 640)
                  +160 +16  height (default 480)
                  +160 +20  refresh hint (5)
                  +160 +28  bpp/format hint (15)
                  +160 +36..+39  bool flags
                  +204  IDirect3D9 *
                  +204+0?  D3D9 mode list root
                  +51?  HANDLE single-instance mutex

CDialog (MFC base)
├── CMainSettingsDialog              resID 0x66 (102)   vtable off_425200
│       ctor  CMainSettingsDialog_ctor (0x409190)
│       inits 3 page objects (CMainSettingsDialog_AllocPages 0x4095e0):
│         page[0] CDialogPage1   (Display)
│         page[1] CDialogPage2   (Keyboard)
│         page[2] CDialogPage3   (Gamepad)
│       paints icon when iconic (CMainSettingsDialog_OnPaint 0x409430)
│
├── CDialogPage1                     resID 0x81 (129)   vtable off_423AF8
│       Display tab (resolution radios + "save size&pos", "mouse look", "invert", sensitivity)
│       OnInitDialog          0x4044f0
│       DoDataExchange        0x404bd0
│       PreTranslateMessage   0x404c70
│       ResetDefaults         0x404480
│       UpdateDebugMode       0x4045b0
│
├── CDialogPage2                     resID 0x82 (130)   vtable off_424258
│       Keyboard tab (per-action bindings + device combobox dword_431524)
│       OnInitDialog          0x4065b0
│       DoDataExchange        0x405c00
│       OnTimer_PollKeyboard  0x405920
│       OpenGamePadConfig     0x406740 (opens CGamePadConfigDialog modally)
│
├── CDialogPage3                     resID 0x83 (131)   vtable off_424810
│       Gamepad tab (button rows × 4 sticks × analog/digital toggles)
│       ctor                  0x4067e0
│       dtor                  0x4068b0
│       OnTimer_PollInput     0x406dc0
│       OnGamepadCountChanged 0x406a00
│
├── CGamePadConfigDialog             resID 0x84 (132)   vtable off_4237A8
│       Per-device "press a button" capture dialog
│       ctor                  0x403a80
│       dtor                  0x4028c0
│       DoDataExchange        0x402460  (DDX_Text/DDX_Control rows + slider)
│       OnTimer_PollAxes      0x402fb0  (reads axis with deadzone, formats display)
│       OnInitDialog_Reset    0x4054b0  (resets active=−1 then UpdateData)
│
├── CGamePadAxisDialog                resID ? (likely 0x85 or another in 0x80s)  vtable off_4238EC
│       Axis-to-action assignment (Move FW/BW, Move L/R, Camera Up/Dn, Camera L/R, ...)
│       OnInitDialog 0x4029c0 (populates 8 comboboxes)
│       UpdateLists  0x403720
│       DeselectDuplicates 0x402560
│
└── CCdRomDialog                     resID 0x86 (134)   vtable off_423508
        "Please insert disc" prompt
        ctor          0x401110
        dtor          0x401170
        OnInitDialog  0x4011b0 (hides the IDCANCEL button)

(Tab page wrapper)
CTabPageDialog                       common base/template for the 3 tab pages
        ctor   0x407610   vtable off_424B90

(Keyboard scancode → display name resource block)
unk_431C08          DIK→ID lookup table (2048 bytes), filled by Launcher_DInput_Init
SubStr (0x42fb10)   per-axis "RX" / "RY" / "Z" string names
hInstance.LoadStringW IDs 0x91..0xC9   localized friendly key names
```

---

## D3D9 mode/format enumeration — formats actually allowed

`Launcher_D3D9_EnumerateAllModes (0x401e50)` filters EnumAdapterModes by these
`D3DFORMAT` codes (decimals as in the binary):

| code | format |
|---:|---|
| 22 | `D3DFMT_X8R8G8B8` (32 bit, 8 bpc) |
| 23 | `D3DFMT_R5G6B5`   (16 bit, 6 bits green) |
| 35 | `D3DFMT_A8R8G8B8` |

Per format, `Launcher_D3D9_FilterModesByFormat (0x401820)` then queries
multisample types (0..16), depth-stencil (`D3DFMT_D16/D24X8/D24S8/D32`),
back-buffer formats, and vertex processing modes
(`D3DCREATE_SOFTWARE`, `HARDWARE`, `MIXED`, `PUREDEVICE`).
The `Launcher_D3D9_FormatBitsHelper (0x401200)` switch maps each format to a
total-bits weight used for "bit depth per channel ≥ user min" filtering.

The four resolution radios on Display tab are the **only** ones the game accepts:

```
0 → 640×480
1 → 800×600
2 → 1024×768
3 → 1280×1024
```

These are hard-coded in `Launcher_DetectAvailableResolutions (0x404130)`. Anything
else the adapter reports is silently ignored. (Implication: lifting the resolution
ceiling would require either editing this function's switch table or bypassing the
launcher entirely — which is exactly what `mtr-asi.asi` does at runtime via
`IDirect3D9::CreateDevice` hook, see
[native-resolution-fix.md](native-resolution-fix.md).)

---

## DirectInput layer

| Function | Address | Purpose |
|---|---|---|
| `Launcher_DInput_Init` | 0x408300 | DirectInput8Create + DIK→ID table + LoadStringW(0x91..0xC9) for key labels |
| `Launcher_DInput_EnumerateDevices` | 0x407b00 | EnumDevices(GAMECTRL) + acquire keyboard (GUID_SysKeyboard) and mouse (GUID_SysMouse) |
| `Launcher_DInput_EnumGameCtrlsCb` | 0x407860 | per-device callback: cap to 9, dedup name `name_idx` |
| `Launcher_DInput_EnumDeviceObjectsCb` | 0x407990 | per-axis callback: SetProperty range −1000..+1000, mark which axes exist (X/Y/Z/RX/RY/RZ + sliders) |
| `Launcher_DInput_AcquireDevice` | 0x408140 | GetCapabilities + Acquire + initial GetDeviceState |
| `Launcher_DInput_PollAnyButton` | 0x407720 | one-shot "is any keyboard key or mouse button pressed?" — used by the keymap config UI |
| `Launcher_DInput_PollDeviceState` | 0x407d80 | full DIJOYSTATE poll for one device |
| `Launcher_DInput_GetActiveAxisOrButton` | 0x407e00 | reads DIJOYSTATE, returns name like `"+RY"` / `"DigitalLeft"` / `"B05"` |
| `Launcher_DInput_ReleaseAll` | 0x407690 | Unacquire+Release all gamepad/keyboard/mouse devices and the IDirectInput8 factory |
| `Launcher_DInput_AxisFlag` | 0x407d60 | accessor to dword_431980[16*dev + 2*axis] axis-presence flag |
| `Launcher_DInput_ResetAxisDeadZones` | 0x4082c0 | resets all 10 dead-zones to 20% |

---

## Memory layout — globals

| Address | Type | Meaning |
|---|---|---|
| 0x4326C0 | `CHAR[256]` | byte_4326C0 — current working dir snapshot (`GetCurrentDirectoryA` at startup) |
| 0x4327C0 | `BYTE` | byte_4327C0 — internal "developer build" flag. When non-zero unlocks `WilburDebug.exe` path, AppData "Avalanche Software" instead of "Disney Interactive Studios", several disabled UI elements re-enable, BUTTON_STICK1 / BUTTON_START become editable. **Always 0 in retail.** |
| 0x431524 | `DWORD` | currently selected gamepad index (CDialogPage2 / CGamePadConfigDialog combobox) |
| 0x431754 | `IDirectInputDevice8 *` | keyboard device |
| 0x431C00 | `IDirectInputDevice8 *` | mouse device |
| 0x431C08 | `DWORD[256]` | DIK scancode → game button id lookup |
| 0x432008 | `IDirectInputDevice8 *[10]` | gamepad device array |
| 0x432530 | `DWORD` | live gamepad count |
| 0x432540 | `WCHAR[64]` | scratch axis-name buffer for `Launcher_DInput_GetActiveAxisOrButton` |
| 0x42E000.. | `WCHAR[][32]` | localised key labels (filled by `Launcher_DInput_Init` LoadStringW) |
| 0x42D098.. | `CHAR[][32]` | canonical English action names: `DPAD_UP`..`STICK1_UP`..`BUTTON_STICK1`/`BUTTON_START` |
| 0x42FB10 (`SubStr`) | `CHAR[][32]` | axis short names (`X`,`Y`,`Z`,`RX`,`RY`,`RZ`,…) used in `+/-Axis` strings |
| 0x42D090 (`Target`) | `void *` | function pointer to `Launcher_GetThreadACP_Generic` (Win9x/NT) or `Launcher_GetACP_NT`, set up by `Launcher_GetACP_Init` |

---

## What this gives us for the mod

1. **The launcher's resolution radios are hard-coded to 4 sizes**. Our ASI mod
   bypasses this entirely by overriding `pp.BackBufferWidth/Height` at the
   `IDirect3D9::CreateDevice` boundary (see `native-resolution-fix.md`). The
   launcher never gets a chance to constrain us — by the time `CreateDevice` is
   called, the launcher's process is already gone (`ShellExecuteExW` fired-and-
   forgot). Only the registry values from `HKCU\...\Settings` survive.

2. **Single-instance mutex name** is `Global\\Disney_s_Meet_The_Robinsons`. If we
   ever need to release a stuck mutex (`known-issues.md` issue #4), the WMI
   `Win32_Process::Terminate` we already use also frees the kernel mutex — but if
   we wanted a more surgical tool we'd need to enumerate handles and close that
   specific named object.

3. **The `-launchit` argument** is undocumented but always passed to `Wilbur.exe`.
   Likely the game's `WinMain` checks for it and refuses to run without it, so a
   minimum command line for direct-launch is:
   ```
   Wilbur.exe -dxfullscreen -dxadapter=0 -dxresolution=800x600 -launchit
   ```

4. **The `-dxdiskdriveletter=L` argument** points the game at a disc drive labelled
   `WR_RUN`. Not relevant for our installs (we run off HDD), but noteworthy:
   the game has a path-aware logic for streamed assets that is keyed by drive letter.

5. **Field offsets on `CMainSettingsDialog`** (e.g. `dlg[34]` = resolution index,
   `dlg[33]` = mouse sensitivity 5..100, `dlg[31]` = mouse look) are the same
   structure the game reads from `HKCU\...\Settings`. If we ever want our ImGui
   menu to write those settings, this is the exact key list.

6. **Three input subsystems** are exercised by the launcher (keyboard via DIK
   scancode lookup, mouse via DIDEVTYPE_SYSMOUSE, gamepad via EnumDevices(GAMECTRL)).
   This confirms our hypothesis that the game also uses DirectInput8 with
   `DISCL_EXCLUSIVE | DISCL_FOREGROUND`, which is what causes our menu input
   problem (see `imgui-menu-architecture.md` — solution: poll `GetAsyncKeyState`).

---

## Function inventory (user code, named in IDB)

Renamed user functions in the IDB are prefixed by their owning class or by
`Launcher_*` for module-level helpers, `MFC_*` for MFC C-runtime utility code that
appeared in the user-code address range. Full inventory accessible via
`func_query filter:!sub_*` in the IDA MCP, or from the IDB's Functions window.

The remaining 215 `sub_*` are all in the FLIRT-misclassified MFC/CRT static-link
range (0x40b000..0x422f1a). They belong to MFC virtual stubs, RTTI thunks,
and CRT internals — not launcher logic — and are out of scope for this writeup.

---

## See also

- [iat-anchors.md](iat-anchors.md) — for `Wilbur.exe` import table; some launcher
  functions parallel game functions (e.g. both call `Direct3DCreate9`).
- [native-resolution-fix.md](native-resolution-fix.md) — why the launcher's
  4-resolution table doesn't matter for our ASI mod.
- [known-issues.md](known-issues.md) issue #4 — mutex name details.
