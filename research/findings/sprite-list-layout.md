# Sprite-batcher list layout (M3.1)

Static RE of the per-frame sprite list at `g_sprite_list_head` (0x7271E8),
walked by `render_sprite_batcher` (sub_4E8D30). This is Phase 3 milestone
M3.1 from `ui-granularity-plan.md`: layout the entry struct so M3.2 can
identify the menu-vs-HUD classification signal.

**Status:** layout decoded from static RE of render_sprite_batcher's two
walk passes. Pushers (writers) are NOT statically xref-able — they all use
struct-relative addressing through SecuROM thunks. Confirming offsets/types
of the unknown fields needs runtime instrumentation (M3.2 work).

## Globals (renamed in IDB 2026-05-06)

| VA          | New name                        | Role                                                |
| ----------- | ------------------------------- | --------------------------------------------------- |
| 0x7271E8    | `g_sprite_list_head`            | Singly-linked `SpriteEntry*` list head              |
| 0x727204    | `g_sprite_pos_stream`           | DWORD: pointer to position vertex stream (12 B/v)   |
| 0x727208    | `g_sprite_uv_stream`            | DWORD: pointer to uv vertex stream (8 B/v)          |
| 0x72720C    | `g_sprite_color_stream`         | DWORD: pointer to color vertex stream (4 B/v ARGB)  |
| 0x727210    | `g_sprite_double_buffer_flip`   | u8: XOR'd 1 each frame after batch — buffer index   |
| 0x727230    | `g_sprite_per_vertex_color_mode`| u8: 1 = per-vertex colors path active in shader     |

The three vertex streams hold up to N quads worth of inline data, written
by the render pass. Quads are issued to D3D as `D3DPT_TRIANGLELIST` with
2 prims per quad (case 5 in `render_draw_primitive_dispatch`).

## SpriteEntry layout

```c
struct SpriteEntry {
    /* +0x00 */ struct SpriteEntry* next;            // singly linked list
    /* +0x04 */ uint32_t            unk_04;          // ?
    /* +0x08 */ uint32_t            flags;           // see Flags table
    /* +0x0C */ uint32_t            unk_0C;          // ?
    /* +0x10 */ uint32_t            state_key;       // texture / state-grouping key
    /* +0x14 */ uint16_t            unk_14;          // ?
    /* +0x16 */ uint16_t            sort_key;        // run grouping key (Z order?)
    /* +0x18 */ uint32_t            unk_18;          // ?
    /* +0x1C */ uint8_t             alpha_mod;       // global alpha modulator
    /* +0x1D */ uint8_t             blend_mode;      // blend state index (state group)
    /* +0x1E */ uint16_t            unk_1E;          // ?
    /* +0x20 */ uint8_t             pad_20[8];       // ?
    /* +0x28 */ float               inline_positions[12];  // 4 verts × XYZ
    /* +0x58 */ float*              ext_positions;   // active when flags & 0x01
    /* +0x5C */ float               inline_uvs[8];   // 4 verts × UV
    /* +0x7C */ float*              ext_uvs;         // active when flags & 0x02
    /* +0x80 */ uint8_t             inline_colors[16];  // 4 verts × RGBA, see flags
    /* sizeof = 0x90 minimum; tail (>= 0x90) not yet RE'd */
};
```

### Flags (entry +0x08)

Decoded from the two passes in render_sprite_batcher:

| Bit  | Mask    | Meaning                                                 |
| ---- | ------- | ------------------------------------------------------- |
| 0    | 0x0001  | positions stored externally; deref `[entry+0x58]`       |
| 1    | 0x0002  | uvs stored externally; deref `[entry+0x7C]`             |
| 2    | 0x0004  | colors stored externally; deref `[entry+0x80]` as ptr   |
| 5    | 0x0020  | per-vertex color (vs replicated single color)           |
| 6    | 0x0040  | "needs sort" — pass 1 sets bit 7 in response, retains   |
| 7    | 0x0080  | "consumed/sorted" — set during pass 1 / batch consume   |
| 8    | 0x0100  | per-vertex alpha modulation (full RGB+A path)           |
| 10   | 0x0400  | per-vertex alpha-only (alpha-channel-only path)         |

Bits 3, 4, 9, 11+ untouched by render_sprite_batcher — likely owned by
pushers / sort logic.

### Pass 1 — sort flags + sort-order check (0x4E8D45..0x4E8D7D)

```
for (e = head; e; e = e->next) {
    if (e->flags & 0x40) {
        e->flags |= 0x80;          // mark consumed
    } else {
        e->flags &= ~0x80;
        u16 k = e->sort_key;
        ++live_count;
        if (k < prev_k) needs_sort = 1;
        prev_k = k;
    }
}
if (live_count && needs_sort) sub_4E8AE0();   // sort (SecuROM thunk)
```

### Pass 2 — emit runs (0x4E8E9F..0x4E92D3)

Runs are grouped on three keys: `sort_key`, `state_key`, `blend_mode`.
For each run:

```
walk forward while (sort_key, state_key, blend_mode) match:
    if !(flags & 0x80) {
        flags |= 0x80;
        positions = (flags & 1) ? *(void**)(e+0x58) : e+0x28;   // 48 B
        uvs       = (flags & 2) ? *(void**)(e+0x7C) : e+0x5C;   // 32 B
        colors    = (flags & 4) ? *(void**)(e+0x80) : e+0x80;   // 16 B
        memcpy(g_sprite_pos_stream + 12*v_idx,   positions, 0x30);
        if (state_key) memcpy(g_sprite_uv_stream + 8*v_idx, uvs, 0x20);
        // colors per-vertex when (flags & 0x20), replicated otherwise
        // alpha modulated through entry+0x1C (alpha_mod):
        //   per-component: out = (alpha_mod * comp) >> 8, clamped to 0xFF
        //   bit 0x100 path = full RGBA modulation (4 verts × 4 channels)
        //   bit 0x400 path = alpha-channel only (4 verts × A only)
        v_idx += 4;
    }
emit run as draw call: render_draw_primitive_dispatch(5, base, count, ...)
```

After each run, the active state is pushed to D3D via the sub_56xxxx
helpers, then `render_draw_primitive_dispatch(5, ...)` emits the
triangle list.

## What this tells us for M3.2 (menu-vs-HUD classification)

**Best candidates ordered by signal quality:**

1. **`state_key` (entry+0x10)** — 32-bit handle/pointer used for state
   grouping. Same key = same draw batch. Almost certainly a texture
   pointer or texture handle — distinct between menu textures (button
   art, background overlays) and HUD textures (mission text, button
   hints). HIGHEST PROMISE: if we can resolve a texture name/path from
   this key, we get unambiguous classification.

2. **Y component of `inline_positions`** — vert positions are 4×XYZ at
   +0x28..+0x57. The transform `(2x-2, -2y-2, z)` maps input XY in
   `[0..1]` to NDC. HUD elements typically near edges (Y close to 0 or
   1 in input space, mapping to ±2 in clip after transform); menu
   elements typically centered (Y around 0.5, mapping near 0 in clip).
   Plausible heuristic if texture-key lookup proves expensive.

3. **`sort_key` (entry+0x16)** — 16-bit, run-grouping key. May encode
   layer/depth: low values = background (e.g. fade-to-black overlay),
   middle = HUD, high = menu (drawn on top). Needs runtime sampling
   to confirm distribution.

4. **`blend_mode` (entry+0x1D)** — 8-bit blend state index. Less likely
   to discriminate (HUD and menu both use alpha blending), but worth
   capturing as a tertiary signal.

**Unlikely classifiers:**
- `alpha_mod` — used both for HUD fade and menu fade, no signal.
- `flags` — pure rendering hints, not authorial intent.

## Pusher functions (NOT statically traceable)

`g_sprite_list_head` has only TWO static xrefs, both reads inside
render_sprite_batcher (0x4E8D3D, 0x4E8E8F). All writers use
struct-relative addressing — the list head is updated through some
manager struct's pointer field. The manager struct is reached via
SecuROM-thunked accessors (e.g. `sub_4E8AE0` is a thunk).

### Implication for M3.2

Static RE alone cannot show us where entries are allocated and what
populates `state_key`. We must use runtime instrumentation:

1. **Hook render_sprite_batcher (sub_4E8D30) prologue.** At the entry
   point, walk `g_sprite_list_head` and snapshot per-entry data
   (state_key, sort_key, flags, first inline position) along with the
   current top-screen name from `mtr::screen_push`. Log a CSV row per
   entry. Capture data across menu / HUD / paused states, then offline
   correlate which `state_key` values appear in which states.

2. **Resolve state_key.** If state_key is a D3D9 texture pointer, hook
   `IDirect3DDevice9::SetTexture` or read the texture's metadata via
   the device's resource list. If it's an internal handle, find the
   handle→texture lookup in code that consumes state_key (the run
   emit calls some sub_56xxxx with it).

This instrumentation is M3.2's first deliverable — a non-invasive
data-collection pass that gives us the empirical mapping needed to
implement the M3.3 split-pass hook.

## What NOT to confuse this with

- The `transform_apply_scale_via_stack` / `transform_apply_translate_via_stack`
  hooks (already shipped as Phase 2 sprite_matrix override) operate on
  the transform stack, NOT on individual entries. They get called
  ONCE per render_sprite_batcher invocation, before the entries are
  walked. To go per-entry, we'd hook between pass 1 and pass 2 —
  walking the list ourselves and either rewriting positions/UVs in
  place or replacing the matrix per-run.

- The list head value itself is overwritten each frame by the pushers
  (`g_sprite_list_head = head; e->next = head; head = e`). After
  render_sprite_batcher returns, the consumed list is reset by the
  pushers' frame init (likely sets `g_sprite_list_head = NULL` and
  reallocates). We can't safely cache entry pointers across frames.

## Related research

- `research/findings/ui-render-investigation.md` — original sprite-batcher
  identification.
- `research/findings/render-pipeline.md` — sprite batcher position in
  per-frame flow (sub_4D23BF caller).
- `research/findings/ui-granularity-plan.md` — Phase 3 plan including
  M3.1..M3.5 milestones.
- `memory/project_sprite_batcher_path.md` — sprite-batcher hook details
  for Phase 1+2 transform override (the matrix substitution path).
