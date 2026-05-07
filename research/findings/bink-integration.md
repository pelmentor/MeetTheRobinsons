# Bink integration — Wilbur.exe ↔ binkw32.dll

What we know about the game's Bink usage and where to hook it cleanly.

## Static linkage

Wilbur.exe imports 15 Bink functions from `binkw32.dll` via the regular PE IAT in `.idata` at `0x006A6300..0x006A633C`:

| IAT slot     | Symbol                         |
|--------------|--------------------------------|
| `0x006A6300` | `_BinkPause@8`                 |
| `0x006A6304` | `_BinkSetSoundSystem@8`        |
| `0x006A6308` | `_BinkOpenDirectSound@4`       |
| `0x006A630C` | `_BinkWait@4`                  |
| `0x006A6310` | `_BinkSetVolume@12`            |
| **`0x006A6314`** | **`_BinkOpen@8`**          |
| `0x006A6318` | `_BinkSetSoundTrack@8`         |
| `0x006A631C` | `_BinkNextFrame@4`             |
| `0x006A6320` | `_BinkCopyToBuffer@28`         |
| `0x006A6324` | `_BinkDX8SurfaceType@4`        |
| `0x006A6328` | `_BinkDoFrame@4`               |
| `0x006A632C` | `_BinkGetError@0`              |
| `0x006A6330` | `_BinkSetSoundOnOff@8`         |
| `0x006A6334` | `_BinkClose@4`                 |
| `0x006A6338` | `_BinkCopyToBufferRect@44`     |

There is a second IAT mirror at `0x00F5F83A..0x00F5F876` inside the SecuROM-rebuilt import section in `rr02`. The import-name strings (`"_BinkPause@8"`, `"BinkSetSoundSystem@8"`, etc.) are plaintext at `0x02EF07C1..`. The mirror is just a stolen-byte wrapper — plain code in our dump, but it adds an indirection between caller and IAT slot. Most game-code callers go through the main IAT directly; the wrapper applies only on SecuROM-tampered code paths. (See [unpack-state.md](unpack-state.md) §"Wrapper noise we live with".)

## The single Bink playback driver

`game_bink_play_video` (`0x0055C190`, formerly `sub_55C190`). Statically it has **zero xrefs**. Callers either (a) live in code regions IDA's auto-analysis didn't disassemble (`define_func` would recover them), or (b) live in SecuROM lazy-decrypt blocks that hadn't run when our menu-time PE-sieve dump was taken. The function itself is fully decompilable because the intros DO play before the main menu, so this code path was decrypted by dump time.

Behaviour:

```c
char __thiscall game_bink_play_video(int this, int video_id, int snd_track, _BYTE *out_skipped, ...) {
    BYTE path_buf[260];
    sub_55BA50(video_id, path_buf);                                 // SecuROM thunk → builds path
    BinkSetSoundTrack(1, &this[24]);
    const char* normalized = game_string_normalize_to_static_buffer(path_buf);  // → static 0x740F90
    HBINK h = BinkOpen(normalized, 0x4000);                         // <-- THE call site, 0x55C209
    unk_72976C[0] = h;
    if (h && /* ... volume/state setup ... */) {
        // PeekMessage / DispatchMessage loop — drives BinkWait, BinkDoFrame, frame copies
        // breaks on: skip-callback fires, hit end-of-video, or BinkGetError() returns non-empty
        // ...
        BinkSetSoundOnOff(h, 0);
        return 1;
    } else {
        sub_55BDF0(this);   // cleanup
        return 0;           // <-- "video failed to open" path; what skip_intros.py exploits
    }
}
```

The interesting properties for a hook:
- `BinkOpen` is **the gate**. Returning `NULL` from it makes the function return `0` cleanly. Caller (inside SecuROM region) treats `0` the same way it treats successful end-of-video — moves on to the next stage.
- `tools/skip_intros.py` exploits this exact behaviour by renaming the .BIK files so `BinkOpen`'s file lookup fails. A hook that selectively returns `NULL` is the same effect with no on-disk changes.

Path-construction internals (`sub_55BA50` → `sub_586780`) live inside SecuROM-VM regions; hard to RE statically. We don't need to — `BinkOpen` receives the final path string in plaintext (Bink doesn't accept encrypted paths).

## Proposed hook (NOT yet implemented — design only)

Replace the `tools/skip_intros.py` rename sledgehammer with a runtime `BinkOpen` short-circuit.

### Hook site choice

Three options, in order of robustness:

1. **Inline hook on `binkw32!BinkOpen`** (recommended). MinHook on the function entry inside the loaded `binkw32.dll`. Catches all callers regardless of whether they go through the regular `.idata` IAT or the SecuROM IAT mirror. Resolution:
   ```cpp
   HMODULE binkw32 = GetModuleHandleA("binkw32.dll");
   if (!binkw32) binkw32 = LoadLibraryA("binkw32.dll");  // shouldn't be needed; statically imported
   FARPROC p = GetProcAddress(binkw32, "_BinkOpen@8");
   MH_CreateHook(p, &hk_BinkOpen, (LPVOID*)&g_orig_BinkOpen);
   ```

2. **IAT hook on slot `0x006A6314`** (`.idata`). Simpler — replace one DWORD with our trampoline pointer. Misses callers that use the SecuROM IAT mirror at `0x00F5F84E`. Probably enough in practice (the BinkOpen call we see at `0x55C209` resolves through the regular `.idata` slot — confirmed by the xref to `0x6A6314`).

3. **Hook `game_bink_play_video` itself** at `0x0055C190`. Inspect the `video_id` argument and short-circuit before path construction. Cleaner abstraction (filenames not in our hook), but requires reverse-engineering the video-id → filename mapping (the SecuROM-decrypted `sub_55BA50` chain).

Inline hook on `binkw32!BinkOpen` is the right default.

### Hook body

```cpp
typedef void* (WINAPI *BinkOpen_t)(const char* filename, uint32_t flags);
BinkOpen_t g_orig_BinkOpen = nullptr;

static const char* kSkipBaseNames[] = {
    "BinkLegal", "legal", "dsny", "avlogo", "bvg",
};

void* WINAPI hk_BinkOpen(const char* filename, uint32_t flags) {
    if (filename) {
        const char* base = filename;
        for (const char* p = filename; *p; ++p)
            if (*p == '/' || *p == '\\') base = p + 1;

        for (const char* skip : kSkipBaseNames) {
            size_t n = strlen(skip);
            if (_strnicmp(base, skip, n) == 0 &&
                (base[n] == '\0' || base[n] == '.' || base[n] == ',')) {
                MTR_LOG("BinkOpen: skipping intro logo \"%s\"", filename);
                return nullptr;
            }
        }
    }
    return g_orig_BinkOpen(filename, flags);
}
```

Notes on the basename match:
- The filename arrives in plaintext at `0x740F90` (the static buffer). Format includes a path; we walk to the last separator.
- Bink filenames in this build are sometimes stored with a trailing `,1` or similar suffix indicating sound-track number — hence the `base[n] == ','` allowance. (Verify when implementing — the tools/skip_intros.py script renames bare basenames so this hasn't been observed, but log all `BinkOpen` calls for one launch first to be sure.)
- Case-insensitive (`_strnicmp`) because Windows is case-insensitive about filenames and Bink may have any casing.

### Wiring into the mod

New file `src/mtr-asi/src/bink_hook.cpp` containing the hook. Install from `dllmain.cpp` immediately after `MH_Initialize` (same place `cmdline_hook::install()` is called). Toggle in the menu (`menu.cpp`) under a "Skip intro logos" checkbox; default ON.

When ON: hook returns NULL for the 5 logo filenames.
When OFF: hook passes through (no behavioural change vs. unmodded game).

### Validation

- Run `python tools/skip_intros.py --restore` first so the .BIK files are back to original names. The hook should produce identical behaviour to the rename trick (logos skipped, game proceeds to main menu).
- Verify in-game cutscenes (egy, sti, dfi, end, credits, etc.) still play normally — the basename allowlist should not match any of those.
- One launch with `MTR_LOG("BinkOpen: open \"%s\"", filename)` (no skip) recommended before shipping the skip logic — confirms the filename format and uncovers any unexpected separators.

## Once shipped

`tools/skip_intros.py` becomes legacy / fallback. Don't delete — useful for debugging cases where the mod isn't loading. Add a note to its top docstring referencing this approach.
