# Lessons learned

Cross-session meta notes — *what didn't work, why, and the rule we extract from it*. Add to this when a multi-hour rabbit hole yields a generalisable insight; otherwise feature-specific findings go in their own file under this directory.

## Process / collaboration

### L0. Classify "crutch vs root-cause" BEFORE coding

Every fix has at least two implementations:
- **Crutch**: hides the symptom by intercepting at a layer downstream of the bug (e.g. mangling D3D output matrices, lying to APIs about return values, pattern-scanning when offsets are knowable).
- **Root-cause**: targets the actual broken code path (binary patch at a known offset, source-level fix, parameter substitution at a function whose contract is to take that parameter).

The user has zero patience for crutches piling up. Classify both options out loud, name the recommended one, and only start coding once "no crutch" is the verdict. See `feedback_no_crutches.md` in personal memory.

### L1. Rename in IDB and update docs the moment you find something

Knowledge that lives only in the chat history is gone after compaction. The durable record is:
1. The IDA database (`ida/*.i64`) — function/global names.
2. `research/findings/*.md` — call chains, addresses, why decisions were made.
3. `research/findings/symbol-table.md` — the cross-reference between original `sub_…` names and ours.

Citing a function as `sub_XXXXXX` in chat is a smell — pause, rename it first.

## Reverse engineering & static analysis

### L2. On this build, "SecuROM protection" is just aPLib compression + BCJ filter + IAT resolver. The dump IS complete.

Reverse-engineered the entire SecuROM 7 stub at `0x02EF16A0` on 2026-05-05 (full writeup in [securom7-stub-re.md](securom7-stub-re.md)). What looked like "SecuROM-encrypted regions" / "lazy-decrypt blocks" / "VM-dispatched calls" in earlier session-by-session investigation turned out to be **none of those things**. The stub does:

1. **aPLib decompress** (public-domain LZ codec) of `rr02` → `rr01`.
2. **BCJ-86 reverse byte filter** (LZMA SDK, public) on the decompressed code stream.
3. **Custom IAT resolver** walks a flat table at end of `rr02`, calls `LoadLibraryA` + `GetProcAddress`, populates IAT slots.
4. Two-byte PE header tweak (clears high bits at 0x4002DF / 0x400307).
5. `jmp 0x0062B48A` → real OEP (`_WinMainCRTStartup`).

**No real encryption, no per-page key derivation, no anti-debug post-OEP.** By the time game code starts running, all of `rr01` is plaintext. Our existing menu-time PE-sieve dump captures it verbatim — it's already 100% complete for this build.

So when earlier session work claimed "the bytes that would encode the call don't exist in the dumped pages" or "they lived inside a SecuROM-decrypt-on-demand block that hadn't been triggered" — that was **wrong**. The bytes are in the dump; they just don't exist in the *game code* either, because the game doesn't statically call `DirectInput8Create`. Verified specifically:

- `FF 15 20 60 6A 00` (`call dword ptr [DirectInput8Create]`) — zero matches because the game never calls this function. The IAT slot is populated by the stub but unused — a linker-included unused import.
- `FF 15 44 62 6A 00` (`call dword ptr [PeekMessageA]`) — only 1 match, in `game_bink_play_video`. The other big caller (`game_MessageLoop`) uses `mov edi, [PeekMessageA]; call edi` — different bytes, my earlier `FF 15 …` search missed it.

Two real reasons static tracing sometimes fails on this image, in order of frequency:

1. **IDA auto-analysis is incomplete.** Large stretches of `rr01`/`rr02` contain plaintext code that IDA never disassembled. `define_func` on the orphan entry recovers it. (How we recovered `game_render_overlay_quad_if_enabled` at `0x4B1150` and `_WinMainCRTStartup` at `0x62B48A`.) Easy fix.

2. **Compiler used `mov reg, [iat]; call reg` rather than `call [iat]`.** `FF 15 …` byte search misses these. The IAT slot is loaded into a register once per function, then `call reg` (= `FF Dx` for register x). To find such calls, search for the IAT slot address as a 4-byte little-endian operand instead — that catches the `mov reg, [iat]` instruction.

**Stolen-byte wrappers** (entries 10–26 of the import descriptor table at `0xF5F83A..`, `0x2AF7EF0..`) are plain `jmp dword ptr [IAT]` thunks — no encryption, just an extra hop. Most game code uses the main IAT at `0x6A6000` directly. Cosmetic clutter; ignore unless a specific call site looks wrong.

**Rule:** when something looks like it's "missing" from the dump:
1. Search for the IAT-call byte pattern AND the IAT-load-into-register pattern (`mov reg, [iat]` = `8B XX <iat-LE>`, where XX is one of `05/0D/15/1D/2D/35/3D` for eax/ecx/edx/ebx/ebp/esi/edi).
2. Search for the literal IAT slot address as a 4-byte LE operand — catches `mov reg, [iat]` and `cmp reg, [iat]` and any other indirect use.
3. If the import truly has zero references in the dump, the game probably doesn't use it — confirm by hooking it at runtime and seeing if the hook ever fires.
4. `define_func` on regions IDA labelled "<no function>" near interesting xrefs — most "orphans" are just IDA gaps.

**API-boundary hooking** (on `d3d9.dll`, `binkw32.dll`, `user32.dll`, etc.) is still the right move when you want to be sure to catch every call regardless of how the game expresses it.

This lesson supersedes earlier overstatements about "SecuROM-encrypted" / "SecuROM-VM dispatched" code. Those concerns were valid for some other SecuROM 7 builds; on this one, the protection is light. The reverse-engineered stub procedure is permanently documented in [securom7-stub-re.md](securom7-stub-re.md) as the source of truth.

### L3. Orphan functions can look perfect — verify with xrefs first

Three failed examples in this project:
- `sub_56D1D0` (renamed `ORPHAN_unused_d3d_init_DEAD_CODE`) — D3D init logic, GetMonitorInfo, fullscreen branch, CreateDevice, SetWindowPos. **Zero xrefs anywhere.** Unreachable. Patching had no effect.
- The 4/3 aspect float at `0x006C750C` (referenced by `game_GetCameraAspect`'s fallback branch) — looked like THE constant. But the conditional `g_window_show_state != 3` is **always true** at runtime (the global is statically `1` and never written by any code in the binary), so the fallback is dead. Patching it had no effect.
- Three of the five `AB AA AA 3F` matches in the unpacked dump (`0x00564783`, `0x0058CDE0`, `0x007140A8`) had no xrefs — orphan bytes in the literal pool, not real constants.

**Rule:** before relying on a found function/constant, run all three checks:
1. `xrefs_to <addr>` — code and data references.
2. `find_bytes` of the address as little-endian — references via constant pool.
3. `find` for the address as immediate operand — references via `mov reg, imm`.

If all three are empty, the symbol is dead. Also check whether the **conditions** that would route control to the symbol are reachable (a referenced function is still effectively dead if it's only called from a branch that's never taken).

### L4. WSGF / community guides are version-specific

The WSGF guide for *Meet the Robinsons* says:
> "open Wilbur.exe in a hex editor, look for `AB AA AA 3F` at file offset `0x8EFEE`, change to `39 8E E3 3F`"

On our SecuROM-7 retail build, file offset `0x8EFEE` lives **inside the encrypted `rr02` section**. The runtime bytes at the corresponding RVA are different from what's on disk; modifying the encrypted bytes doesn't predictably propagate. The WSGF author probably tested on an unpacked / cracked / re-released version with a different layout.

The guide WAS useful as a starting clue: it told us a 4/3 float exists somewhere as a single constant, which made the problem feel tractable and gave us the byte pattern to search for. But the actual fix on this build is a function-level hook, not the documented disk patch.

**Rule:** treat third-party widescreen / FOV / cheat guides as **starting hints**, not solutions. Verify the documented byte pattern lives where the guide claims, and that it's actually reachable at runtime, before trusting the recipe.

## Hooking, lifecycle, OS plumbing

### L5. Constant patching ≠ visible effect when game caches

Even if you find and patch the right 4-byte constant, the game might **read it once** at startup and cache the derived state (e.g. a projection matrix in a camera struct field). Subsequent runtime mutation of the constant has no effect until the cache is invalidated.

In our case, `game_camera_setup` reads `game_GetCameraAspect()` once and stores `aspect * (a4/a5)` at `*(camera + 12)`. The matrix is rebuilt only when `*(camera + 112)` (dirty flag) is set — which happens on scene load / camera reset, not on user input.

**Rule:** when overriding cached values, also invalidate the cache. We hooked `game_camera_recompute_projection` to write our target into `*(this+12)` AND set `*(this+112) = 1` per call, forcing the rebuild. Without that, menu changes only landed on scene transitions.

### L5a. One dirty flag often isn't enough — the same source feeds multiple caches

A camera struct that holds an `aspect` field at `*(camera+12)` may use that value in *several* places, each with its own dirty flag. We initially dirtied only `*(camera+112)` (projection-matrix cache) and the projection updated correctly. But the **frustum-culling planes** at `*(camera+148..)` are rebuilt by `game_camera_build_view_frustum` only when `*(camera+144)` is dirty — and we never set that. Result: projection at 16:9, cull frustum stuck at 4:3, edge geometry pops/culls inside the visible area.

When you find one cache invalidator, **search for sibling dirty flags in the same struct**. The decompiler exposes them as adjacent byte fields with the same `if (*(this + N)) { ...recompute...; *(this + N) = 0; }` shape. Likely there's also a "world-transform dirty" or "shader-uniform dirty" living a few bytes away that needs the same treatment for any aspect/FOV/near/far override.

**Rule:** when overriding any input that's logically a property of an object (camera, light, render target), enumerate **all** dirty flags on that object before declaring victory. The first one you find covers one consumer; the bug for any other consumer is silent until something visible (here: cull artifacts) reveals it.

### L6. DirectInput exclusive silently drops `WM_KEY` / `WM_*BUTTON`

DirectInput8 with `DISCL_EXCLUSIVE | DISCL_FOREGROUND` bypasses Windows' WM message delivery for keyboard and mouse. Your WndProc subclass will never see `WM_KEYDOWN`; mouse clicks won't reach `ImGui_ImplWin32_WndProcHandler`.

**Symptom:** ImGui draws fine, but checkboxes don't toggle, hotkeys don't work, mouse cursor "doesn't click".

**Rule:** poll `GetAsyncKeyState` / `GetCursorPos` in the render thread, feed events via `io.AddXxxEvent`. These read kernel-side input state that DirectInput exclusive doesn't intercept. Our [`menu.cpp::poll_input_to_imgui`](../../src/mtr-asi/src/menu.cpp) does this. Set `io.MouseDrawCursor = menu_visible` so ImGui draws its own cursor only when the menu is open (the game's `ShowCursor(0)` on `WM_ACTIVATEAPP` hides the system cursor).

### L7. DLL_PROCESS_DETACH on process exit should be a no-op

When `DllMain` is called with `dwReason == DLL_PROCESS_DETACH` and `lpvReserved != NULL`, the **process is terminating**. The OS will reclaim everything — kernel handles, mapped images, threads. Doing further work under loader lock (`MH_Uninitialize`, `ImGui_ImplDX9_Shutdown`, unsubclassing WndProc) risks:
- Deadlocks against threads stuck in our hooks.
- Leaking handles that keep the EPROCESS alive past natural exit, leaving zombie processes that hold named mutexes and file mappings (here: the Disney mutex and our own `mtr-asi.asi` image).

**Rule:** check `lpvReserved`. Do cleanup only when it's `NULL` (explicit `FreeLibrary` — never happens in this project). On process termination, just close the log file and return.

This single change in [`dllmain.cpp`](../../src/mtr-asi/src/dllmain.cpp) eliminated most of the "Wilbur is HasExited=True but unreaped" zombies that previously broke our dev iterations.

### L8. Deferred init thread races with game init

A worker thread spawned from `DllMain` does not run synchronously with game startup. SecuROM unpacks first, the CRT runs, then `main()` runs. Our worker thread is scheduled at some point; **when** depends on the OS scheduler.

For patches that must apply before some game event:
- If the patch needs to be in place before the game's CRT calls a function (e.g. `GetCommandLineA`), install the hook **synchronously inside `DllMain`** — not from a worker thread.
- For things that need a target DLL to be loaded (e.g. d3d9.dll), poll for the load with a timeout (`GetModuleHandleA("d3d9.dll")` in a tiny Sleep loop) instead of assuming it's already there.

The cmdline hook is installed synchronously from DllMain for exactly this reason — by the time a worker thread would run, the CRT has already cached `__argv`.

### L9. Static globals can have zero writers

If `xrefs_to <global>` shows only **read** references, the global is statically initialised and constant for the lifetime of the process. Conditions that depend on it have known, fixed truth values; branches that depend on it being a different value are dead code.

The `g_window_show_state` global at `0x006FBD14` is `1` for the entire run — there is no code that ever sets it to `3`. The `game_GetCameraAspect` fallback branch (which checks `!= 3`) is unreachable. We wasted hours patching `0x006C750C` (the dead branch's constant) before noticing this.

**Rule:** check WRITE xrefs separately. If a conditional depends on a global that's never written, you can constant-fold the conditional and ignore the unreachable branch.

## Tooling

### L10. PowerShell with WinAPI ctypes is fragile; Python is cleaner

The first attempt at the mutex-holder finder was a PowerShell script using `[System.Runtime.InteropServices.Marshal]` and `Add-Type` for `NtQuerySystemInformation`. It hung indefinitely on certain handle types (ALPC ports, named pipes) because `DuplicateHandle` blocks for those, and PowerShell has no clean way to apply per-call timeouts.

The Python rewrite (`tools/find_mutex_holder.py`) uses `ctypes` directly and runs each `NtQueryObject` in a worker thread with `threading.Thread.join(timeout=)`. Reliable, simpler code, fewer escape-string headaches.

**Rule:** for Win32 stuff that needs ctypes/NT API access, write Python first. PowerShell + Add-Type is convenient for one-liners but accumulates pain quickly when you need timeout / signal control.

Also: `Remove-Variable * -Exclude PID` killed the user's VS Code PowerShell terminals because automatic variables can't be safely nuked. Don't.

### L11. Killing zombies needs handle-table traversal, not `taskkill`

`taskkill /F /PID NNN` and `Stop-Process -Id NNN -Force` are both ineffective against an already-exited process whose `EPROCESS` is held alive by an external `hProcess` handle (zombie state). They report success but the kernel object stays.

The mutex held by the zombie also stays — preventing subsequent launches.

**Rule:** to clear a zombie that holds a named mutex, find the **process holding the mutex handle** (which is the zombie itself in most cases, but may also be a parent or accidentally a debug tool that opened the mutex). Use `NtQuerySystemInformation(SystemExtendedHandleInformation)` filtered to Mutant type. `TerminateProcess` on the holder closes the kernel handle table → mutex object is released.

### L12. dxwrapper coexists with our ASI mod

`dxwrapper` proxies `d3d8.dll` (translates D3D8 → D3D9). It also acts as the .asi loader in this project, so we don't ship Ultimate ASI Loader on top.

Order:
1. Wilbur.exe imports d3d8 → Windows loader loads `dxwrapper.dll` (acting as d3d8 stub).
2. dxwrapper's `DllMain` runs, finds `mtr-asi.asi` next to it, `LoadLibrary`s it.
3. Our DllMain runs — installs cmdline hook, MH_Initialize, etc.
4. Game's main runs, calls `Direct3DCreate8(220)` → dxwrapper translates to `Direct3DCreate9` internally.
5. Game's `IDirect3D8::CreateDevice` → dxwrapper translates to `IDirect3D9::CreateDevice` → our hook fires.

dxwrapper README explicitly mentions ASI compatibility. See [dxwrapper-integration.md](dxwrapper-integration.md).

### L13. d3d8 PE headers don't ship with modern Windows SDK

Building anything that uses `d3d8.h` against the Windows 10/11 SDK won't work — the header is gone. Either vendor d3d8.h from an old DX8 SDK (heavy), or write your own minimal declarations.

For this project we abandoned direct D3D8 hooks in favour of letting dxwrapper do D3D8 → D3D9 translation, and hooking D3D9 (which is still in the Windows SDK).

### L14. Hooking a function with multiple semantically-distinct callers over-substitutes

`game_BuildPerspectiveMatrix` (`0x562B20`) takes `aspect` as a documented parameter. Hooking it and substituting the user's chosen aspect *seemed* clean — "param substitution at the documented input, not output mangling". But that builder has four callers:

- 1 main camera (passes `*(camera+12)`)
- 2 shadow/reflection probes (pass literal `aspect=1.0` for square render targets)
- 1 overlay quad blit (passes literal `aspect=1.0` for a screen-space quad)

Our blanket aspect substitution silently breaks the latter three — square projections become non-square. No crash, just visually wrong reflections / shadow volumes.

**Rule:** before hooking a low-level function, list every caller and the *intent* of each. If the callers have different intents (some want your override, others legitimately want their original arguments), hook one level up — at the caller you actually want to affect. Here, hooking only `game_camera_recompute_projection` (the main-camera path) and writing target aspect into the camera struct's `+12` field gives us the override exclusively on the main camera; probe callers run untouched with `aspect=1.0`.

If hooking one level up isn't possible, gate on `_ReturnAddress()` to substitute only when called from the specific caller you mean.

### L15. SecuROM IAT mirror affects hook design

Wilbur.exe has TWO sets of binkw32 import slots: the regular PE IAT in `.idata` at `0x006A6300..`, AND a SecuROM-decrypted mirror at `0x00F5F83A..` inside the `rr02` segment. Some game functions reach Bink (and other DLL imports) through the regular IAT; others through the SecuROM mirror via VM-dispatched trampolines.

**Implications for hook choice:**
- IAT hook on the `.idata` slot catches only the regular-IAT callers. Misses the SecuROM-mirror callers entirely.
- Inline hook on the actual exported function inside `binkw32.dll` (or `kernel32.dll`, etc.) catches both — the function entry is shared regardless of which IAT was used to call it.

**Rule:** prefer inline hooks on the foreign DLL's exported function over IAT hooks when working with SecuROM-protected binaries. IAT hooks are simpler but easy to get wrong on these targets.

### L16. Defining orphan functions: look for the `0xCC` pad

When IDA shows a `<no function>` region containing a `call` to something interesting (e.g. our orphan call to `game_BuildPerspectiveMatrix` at `0x4B1186`), the function it lives in is likely undiscovered, not nonexistent. Walk back from the call site looking for the canonical x86 prologue (`push ebp; mov ebp, esp` or `push ebx`) preceded by `0xCC` padding bytes. The byte just after the last `0xCC` is the function entry.

In our case, `0xCC 0xCC 0xCC 0xCC 0xCC 0xCC 0xCC` ended at `0x4B114F`; function started at `0x4B1150` with `55 8b ec` (`push ebp; mov ebp, esp`). One `define_func` call recovered 284 bytes of decompilable code that turned out to be the third `aspect=1.0` caller of `game_BuildPerspectiveMatrix` — closing a real loose end.

**Rule:** when an interesting xref points into "undefined" territory, run `define_func` first, then look at the result. Most of the time IDA can recover the function; you just need to give it the entry point.

## Small things

- `Game/` directory and `*.exe` / `*.i64` / `*.idb` files should not be committed.
- `mtr-asi.log` is opened with `_fsopen(_, "w", _SH_DENYNO)` so other processes can read it while the game runs (`type Game\mtr-asi.log`, Notepad++, etc.).
- Build output is `mtr-asi.asi` (suffix), not `.dll`.
- Build is 32-bit only (`-A Win32`); Wilbur.exe is i386.
- The launcher emits a malformed cmdline (`-dxresolution=640x480-dxdiskdriveletter=c -launchit` with no space). Our parser handles it; don't try to "fix" it upstream.
- The engine's millisecond clock is `game_get_time_ms` (`0x004A3CCE`); 30+ engine functions use it for animation/AI/physics. Game logic is largely time-based, not frame-tick-based, so an FPS cap should not change game speed (validate empirically before shipping).
