# Sprite per-element control architecture (sprite_xform)

State of the granular UI control system as shipped 2026-05-06. Supersedes
the bbox-classifier split-pass approach (M3.3) which proved too brittle.

## High-level shape

```
render_frame_top_level (engine)
        │
        │ call at 0x4D23BF (5-byte rewrite -> our wrapper)
        ▼
wrapper_render_sprite_batcher  [src/mtr-asi/src/sprite_probe.cpp]
        │
        │  1. probe::capture (if CSV armed)
        │  2. sprite_xform::process_list   <── walks list, modifies inline_positions
        │  3. sprite_split::execute_split  (if "[experimental]" enabled, off by default)
        │     OR direct call to render_sprite_batcher (default)
        ▼
render_sprite_batcher (sub_4E8D30, engine)
        │
        │ pass 1 (sort gate)
        │ pass 2 (run-grouped emit)
        │   → calls transform_apply_scale_via_stack (0x562AA0)
        │     calls transform_apply_translate_via_stack (0x562AE0)
        │     OUR HOOKS apply sprite_matrix factors / pass_override / pos_offset
        ▼
draw calls
```

Two layers of override compose:

1. **Global matrix transforms** (sprite_matrix) — apply uniformly to the
   whole batch via the sprite-batcher's matrix-builder hooks. These are
   the original Phase 2 controls: `auto_from_rules` factor, manual
   `mul_a_*` / `mul_b_*` sliders, `pos_offset_x/y`, and the new
   `pass_override_factor` channel used by split-pass.

2. **Per-entry vertex modification** (sprite_xform) — modifies each
   entry's `inline_positions` array (4 verts × XYZ at +0x28) BEFORE the
   engine reads them. Affects only entries whose `state_key` matches a
   user-tracked slot.

The two compose: vertex modified by sprite_xform, then engine multiplies
by the (possibly sprite_matrix-modified) transform stack. Both layers
work; the user can use either or both.

## sprite_xform module ([src/mtr-asi/src/sprite_xform.cpp](src/mtr-asi/src/sprite_xform.cpp))

### Slot table

64 slots, each tracks one `state_key`:

```c
struct Slot {
    uint32_t state_key;
    uint64_t total_count;        // cumulative entries seen
    uint32_t frame_count;         // entries this frame
    uint32_t last_seen_frame;
    char     name[48];            // user label
    char     group[32];           // user group tag
    float    offset_x, offset_y;  // applied around centroid
    float    scale_x,  scale_y;   // applied around centroid
    bool     hidden;              // sets alpha_mod=0
    bool     highlight;           // sets alpha_mod=255 (force visible)
};
```

LRU eviction: on full table, the slot with oldest `last_seen_frame` gets
evicted to make room for a new key. Practical capacity: easily handles
the 16-32 keys/frame seen in typical Wilbur play.

### process_list()

Called every frame from the wrapper:

1. Reset all slots' `frame_count = 0` (so the UI list reflects only
   currently-rendering keys).
2. Walk the sprite list (head at `g_sprite_list_head` = 0x7271E8).
3. For each entry: find_or_insert(state_key) → bump counts.
4. If `enabled` is true and the slot has a non-identity transform:
   - hidden → set entry->alpha_mod = 0 and skip the transform
   - highlight → set entry->alpha_mod = 255
   - apply_transform(inline_positions, ox, oy, sx, sy)

### apply_transform — centroid-based

```c
void apply_transform(float* pos, float ox, float oy, float sx, float sy) {
    if (sx == 1 && sy == 1 && ox == 0 && oy == 0) return;  // identity early out
    const float cx = (pos[0] + pos[3] + pos[6] + pos[9])  * 0.25f;
    const float cy = (pos[1] + pos[4] + pos[7] + pos[10]) * 0.25f;
    for (int v = 0; v < 4; ++v) {
        pos[v*3 + 0] = (pos[v*3+0] - cx) * sx + cx + ox;
        pos[v*3 + 1] = (pos[v*3+1] - cy) * sy + cy + oy;
    }
}
```

Scale is applied around the per-entry centroid (so "scale 0.5×" shrinks
in place rather than drifting toward origin). Offset is post-scale.
Z is untouched.

Only `inline_positions` (entry+0x28..+0x57) is modified; entries with
flag bit 0 set use `ext_positions` which we don't write — their backing
buffer might be const memory.

## Persistence

Section `[sprite_xform]` in `Game/mtr-asi-ui.ini`:

```ini
[sprite_xform]
count=2
x_0_state_key=0x19770D00
x_0_name=global font
x_0_group=HUD
x_0_offset_x=0.000000
x_0_offset_y=0.000000
x_0_scale_x=1.000000
x_0_scale_y=1.000000
x_0_hidden=0
x_1_state_key=...
```

`save_count()` returns count of slots with non-identity state — slots
with both no transform AND no name AND no group are skipped (saves
ini space, avoids stale entries).

A slot persists if it has ANY user state — labelling an unmodified key
is a valid action and survives save/load.

### Cross-session caveat

`state_key` is a heap pointer (texture handle / asset pointer). Its
value DIFFERS across game sessions due to allocation order and ASLR.
Persistence is best-effort: same-session restart preserves work, but
cross-session you may see transforms apply to "the wrong sprite" (the
heap object that landed at that address) or sit dormant. Worst case
visible glitch, easily reverted via "Reset all transforms".

The "stable identifier" problem is unsolved — we'd need to RE the
texture loader to map `path → state_key` at create time. That's the
deferred Phase 3 work.

## Why bbox-classifier split-pass was demoted

The original M3.3 plan was a vertex-bbox classifier in
`sprite_split.cpp`: entries with all 4 verts in `[0,1]²` → menu (apply
pillarbox), else HUD (leave alone). Two-pass rendering using the
engine's flag bit 0x40 to gate which entries each pass renders.

Tested live and found brittle:

| State                     | Outcome                                                            |
| ------------------------- | ------------------------------------------------------------------ |
| Main menu (pure menu)     | All verts in `[0,1]²` → all pillarboxed ✓                          |
| In-game HUD only          | HUD verts extend outside `[0,1]²` → not pillarboxed ✓              |
| Tip popup                 | Tip in `[0,1]²` (pillarboxed), HUD bits leak (not pillarboxed) ✓   |
| Pause overlay (ESC menu)  | **MENU FRAME** verts go to x=1.061 → frame stays full-width while inner content pillarboxes → visual SPLIT ✗ |
| Cutscene letterbox        | Bars at y < 0 → not pillarboxed ✓                                  |

The pause-overlay edge case made it unusable. Menu frames are authored
to extend slightly past the unit square (the gold border in the
ScreenWilburPause UI for example), and the bbox classifier is a hard
boundary — there's no per-asset knowledge of "this frame goes with the
inner menu content even though it extends past [0,1]²".

A texture-pointer-based classifier (group all entries with the same
state_key under one classification) would be cleaner. But state_keys
are session-pointers and we don't have name-based persistence yet — so
the user can't preconfigure "these state_keys are menu, those are HUD"
across sessions. That's the gap auto-naming (Phase 3) closes.

The pivot to sprite_xform sidesteps this entirely: instead of classifying
into menu/HUD buckets, expose the raw state_keys and let the user dial
in per-key transforms manually. Tedious but correct.

## Menu UI structure

`Per-element control (by state_key, live)` TreeNode in Display tab,
inside the "UI aspect..." section. Layout:

```
▼ Per-element control (by state_key, live)  (?)
  ☐ Enabled  (must be on for transforms / hide to apply)
  Total entries this frame: 169

  [Reset all transforms] [Forget all keys]

  Top 29 keys this frame, grouped by user-assigned group:

  ▼ HUD  (4 keys)
      0x19770D00  [name input]  [group input]   86/f cum 21989853
      [Hilite] [☐ Hide] [Reset]   ox: 0.0000  oy: 0.0000  sx: 1.0  sy: 1.0
      ──────────────────────────────────────────────────────────
      ...

  ▼ Menu  (5 keys)
      ...

  ▼ Ungrouped  (20 keys)
      ...
```

DragFloat semantics on every numeric:
- drag to change
- Ctrl+click to type a number
- Shift+drag for 10× step
- Alt+drag for 0.1× finer step
- 4-decimal precision
- range -5..+5 for offsets, 0.05..5 for scales (clamped on input)

## Workflow for finding a specific sprite (e.g. "wilbur smile")

1. Enable per-element (`Enabled` checkbox).
2. Scroll the live key list.
3. For a candidate row, hold the `Hilite` button — its sprites become
   forced-visible (alpha=255). Use this to spot which key is which.
4. Or tick `Hide` to make them invisible — visually confirm by what
   disappears.
5. Once identified, type a name (e.g. "wilbur smile") and group
   (e.g. "main_menu") — saved on focus loss.
6. Dial in offset / scale via DragFloats (Ctrl+click for exact values).

## Pending work

- **Phase 2 of per-element** (~½ day): per-group bulk transforms — tick
  a group expander to apply offset/scale/hide to all members at once.
- **Phase 3 of per-element** (multi-day RE): auto-naming. Hook the
  engine's texture-create path, record `texture_path → state_key`, so
  names auto-populate from asset paths and persistence becomes by-name
  (robust across sessions). Trace from sub_565CF0 (sprite-batcher
  state-setup consumer of state_key) back through SecuROM thunks at
  runtime to reach the loader.

## Cross-references

- [`research/findings/sprite-list-layout.md`](sprite-list-layout.md) — M3.1 SpriteEntry struct
- [`research/findings/sprite-classification-findings.md`](sprite-classification-findings.md) — M3.2 capture analysis (12 CSVs)
- [`research/findings/ui-granularity-plan.md`](ui-granularity-plan.md) — original Phase 1-3 plan (Phase 3 part now revised by this doc)
- `memory/project_ui_granularity.md` — canonical project state for UI control
- `memory/project_state_2026-05-06_per_element.md` — end-of-day checkpoint
