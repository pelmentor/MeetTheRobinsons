# Phase D — Auto-naming via state_key target memory probe (2026-05-06)

Goal of Phase D: replace manual `name` labelling on per-element slots with
auto-populated names from texture-asset paths. Once we know "this slot
maps to `Art/HUD/health_bar.dds`", the user doesn't need to type names at
all, and persistence becomes by-name (cross-session-robust).

## Static-RE result: blocked by SecuROM

The plan was to trace from the sprite-batcher's state_key consumer
(sub_565CF0) backward through callees to the texture-create path, then
hook there to record `path → state_key` at load time.

What I found in IDA:

1. **render_sprite_batcher (sub_4E8D30)** treats state_key as opaque. It's
   read at `entry+0x10` (`v8[4]` in the decompile), used only for grouping
   adjacent same-state entries via `==` comparison, then handed to
   `sub_565CF0` as the third arg.
2. **sub_565CF0** is a SecuROM thunk: `jmp <g_securom_thunk_table_base + 194614>`.
   It dispatches to a runtime-decrypted function — invisible to static
   analysis.
3. **g_sprite_list_head writers** (= sprite pushers) are also runtime-
   decrypted. A byte-pattern search for `E8 71 72 00` (the address as
   little-endian) finds matches at 0x4D28xx outside any defined function:
   the bytes are SecuROM stub-style `push <arg>; push <ret>; push <real>; ret`
   patterns, where the "real target" gets decrypted at runtime. So the
   pushers exist in the binary, but their bodies are encrypted at rest.
4. **D3D imports**: only `Direct3DCreate8` is imported — Wilbur is a D3D8
   game (dxwrapper translates to D3D9 at runtime). No `D3DXCreateTextureFromFileExA`
   or other path-aware D3D imports. Texture creation goes through
   `IDirect3DDevice8::CreateTexture` (vtable indirect, no path arg) — the
   path-to-texture mapping is purely an engine-level concern, and that
   engine code is the SecuROM-encrypted part.

There ARE plenty of texture-related strings in `.rdata` (`DB_TEXTURE_NAMES`,
`Texture Names`, `IRRMgr Blank Texture`, etc.) and the Gamebryo-style
"NiObject" / `NiAVObject` heritage suggested by project memories means the
texture object likely has an `m_pcName` field at a known offset (4 or 8
bytes after vtable). But we can't statically reach the loader to hook it.

## Pivot: read state_key target memory at runtime

Since static RE is blocked but `state_key` is a heap pointer to a real
object **at runtime**, we can:

1. Walk the live sprite list (we already do this in `process_list`).
2. For each tracked slot, dereference its state_key.
3. Read 256 bytes — the start of the target object.
4. Look for embedded printable strings (likely `m_pcName`) at fixed
   offsets, or pointers to strings that get chased one indirection.
5. Once we know the offset, sprite_xform.cpp can read the name field per
   frame and auto-populate the slot's `name`.

This sidesteps SecuROM entirely. We don't need to know HOW the loader
works — just where the name field lives once the object is alive.

## Implementation: state_key_probe.cpp (shipped 2026-05-06)

`src/mtr-asi/src/state_key_probe.cpp` exposes `dump_all_to_csv()`. UI
button "Dump probe CSV" lives in the per-element TreeNode. When clicked,
writes `Game/mtr-asi-state-key-probe.csv` with one row per tracked slot:

```
state_key, name, group, frame_count, total_count, vtable_at_+0,
longest_string, chase_results, longest_string_offset, bytes_at_+00..+FF
```

Where:
- `state_key`: the heap pointer (= entry+0x10 / texture-object pointer)
- `name`/`group`: user-provided manual labels (if any), useful for
  cross-checking
- `vtable_at_+0`: first dword at `*state_key`. C++ objects almost always
  start with a vtable pointer, so this identifies the class (multiple
  slots sharing a vtable = same C++ class)
- `longest_string`: longest run of printable ASCII (≥4 chars) in the
  256-byte dump. Almost certainly the name string if it's stored inline
- `chase_results`: for offsets +4, +8, +0xC, +0x10, +0x14, +0x18, +0x1C,
  reads the dword at that offset, treats it as a pointer, dereferences,
  reads a C-string. Output as `+OFF->STRING ` for each that hit. Catches
  the common case where `m_pcName` is `char*` not `char[]`
- `bytes_at_+00..+FF`: raw hex dump (for offline inspection)

Reads are guarded by `VirtualQuery + SEH __try` so we don't crash on bad
pointers.

## How to use (user-facing)

1. Load Wilbur, get to a state with the desired sprites visible.
2. Open Insert menu → Display tab → expand "Per-element control".
3. (Optional) Use Pin to freeze the list at this frame.
4. Click "Dump probe CSV".
5. Copy `Game/mtr-asi-state-key-probe.csv` and send it back for offline
   analysis.
6. Capture the menu and the gameplay HUD separately so we get diverse
   class samples (different texture types may have different layouts).

## Expected analysis outcome

After the user provides a CSV, offline analysis should:

1. Cluster by `vtable_at_+0` — same vtable = same C++ class. Most
   sprite assets probably share one vtable (e.g. NiSourceTexture).
2. Look for a consistent printable-string offset across many rows of the
   same vtable. If `chase_results` shows `+8->Art/HUD/health_bar.dds`
   for many rows, the m_pcName-as-pointer offset is `+8`.
3. Could also be inline char[] (then `longest_string_offset` is the same
   number for many rows of the same vtable).
4. Once the offset is locked in, code it into sprite_xform.cpp:

```cpp
// Pseudocode for the read-at-fixed-offset path.
const char* read_state_key_name(uint32_t state_key) {
    // chase one pointer indirection at offset N
    uintptr_t pname = *(uintptr_t*)(state_key + N);
    if (!is_readable(pname)) return nullptr;
    return (const char*)pname;
}
```

5. Use it from `process_list`:

```cpp
const char* p = read_state_key_name(e->state_key);
if (p && p[0] && slot.name[0] == 0) {
    // auto-populate the user-empty name slot
    strncpy(slot.name, p, sizeof(slot.name) - 1);
}
```

## Alternative if the probe shows no consistent layout

If the texture object doesn't have a name field (e.g. it's a low-level
D3D wrapper with no asset metadata), we'd have to:

a) **Hook at higher level**. The .sx script VM uses asset names that
   resolve to state_keys eventually. We could intercept the script-side
   asset reference to get the name, but that's a more complex hook.
b) **Hook IDirect3DDevice8::CreateTexture** via vtable. We'd see every
   D3D-level texture creation and the dimensions, but no path. Less
   useful.
c) **Pattern-search the asset DB**. Wilbur has `DB_TEXTURE_NAMES` etc.
   strings; maybe there's a runtime registry that maps name → handle
   we can scan from outside.

We pick after seeing the probe CSV. (a) is the fallback most likely to
work; the others are less leveraged.

## Cross-references

- [`src/mtr-asi/src/state_key_probe.cpp`](../../src/mtr-asi/src/state_key_probe.cpp) — probe implementation
- [`src/mtr-asi/src/sprite_xform.cpp`](../../src/mtr-asi/src/sprite_xform.cpp) — where auto-naming will plug in
- [`research/findings/per-element-v2-plan.md`](per-element-v2-plan.md) — Phase D in context of full plan
- `memory/project_per_element_v2_shipped.md` — current v2 state
