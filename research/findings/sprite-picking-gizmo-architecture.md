# Sprite picking + on-screen gizmo architecture

State as shipped 2026-05-06. Replaces the row-controls-only edit flow
with direct screen manipulation: click a sprite in the game window,
drag handles on the picked quad to translate / scale, and the changes
flow through the existing `sprite_xform` slot system. Adds a new
`sprite_picking` module and unifies all mouse-input capture through
`WH_MOUSE_LL` so the system works in both menu and gameplay contexts.

## High-level shape

```
                 ┌──── menu draw_frame (render thread) ────────────────┐
                 │                                                     │
sprite-batcher   │  ImGui_ImplWin32_NewFrame                           │
hook (existing)  │       │                                             │
       │         │       │  drain LL hook queue:                       │
       ▼         │       │    AddMousePosEvent                         │
process_list ────┼──►    │    AddMouseButtonEvent     ◄── WH_MOUSE_LL  │
   walks list,   │       │    AddMouseWheelEvent          (separate    │
   per entry:    │       │                                 thread)     │
   1. compute    │       ▼                                             │
      composite  │  ImGui::NewFrame                                    │
      key       ─┼──►   │                                              │
   2. submit ────┼──►   per-element TreeNode (slot list, controls)    │
      quad to    │       │                                             │
      picking    │       │  Pick mode + Drag-group + Auto-group        │
      (always)   │       │  toggles, slot rows with row controls       │
   3. apply      │       │                                             │
      transform  │       ▼                                             │
      to        ─┼──►  overlay block:                                  │
      inline      │       │    factors = sprite_matrix::current_factors│
      _positions  │       │    remap, pixel_to_engine                  │
                  │       │                                            │
                  │       │    Hilite overlay (existing)               │
                  │       │    selection outline (cyan poly)           │
                  │       │    translate handle  ◄── click + drag      │
                  │       │    4 corner handles  ◄── click + drag      │
                  │       │    pick-mode click handler:                │
                  │       │       pick_engine_at(layer_index)          │
                  │       │       set_selected(slot_idx)               │
                  │       ▼                                            │
                  │  ImGui::Render → DX9 backend → present             │
                  └─────────────────────────────────────────────────────┘
```

## Modules and their roles

### `sprite_picking.cpp` (new)

Quad capture + hit-test. Owns:

- **Per-frame quad list**: filled by `sprite_xform::process_list` as it
  walks the sprite-batcher entries. Each submission is `(slot_idx,
  state_key, 4-corner xy in engine [0,1]² space, render_order)` where
  `render_order` is the linked-list iteration index (= engine render
  order; see findings doc on the entry distribution).

- **Hit-test**: `pick_engine(ex, ey)` → topmost slot under the cursor.
  `pick_engine_at(ex, ey, layer_index)` → Nth-from-top slot, deduping
  consecutive same-slot hits so each layer is visibly distinct.

- **Selection state**: persistent `selected()` slot index across frames,
  cleared via Esc / Deselect button / `clear_selection()`.

- **Pick-mode toggle**: `pick_mode()` / `set_pick_mode(bool)` —
  consulted by the menu's overlay click handler.

Threading: both submission (render thread, `process_list`) and read
(render thread, menu draw) happen on the same thread within one frame
in fixed sequence — no lock needed for the quad buffer. Selection /
pick-mode atomics are fine for the cross-call state.

### `sprite_xform.cpp` (extended)

Already owned per-slot transforms. New responsibilities:

- Submits picking quads via `sprite_picking::submit(slot_idx, state_key,
  inline_positions)` for every entry it sees, regardless of `flag & 0x1`
  (which used to gate transform writes — picking should always work even
  on entries we can't transform).

- Auto-grouping: `auto_group_from_path` toggle (per-frame, applies on
  first auto-name) + `auto_group_all_from_paths()` one-shot retroactive
  pass. Heuristic: parent dir if path has separator, else basename
  without extension.

- Group-drag broadcast: `apply_group_translate_delta(group, exclude,
  dx, dy)` + `apply_group_scale_factor(group, exclude, fsx, fsy)` apply
  the per-frame delta from the picked slot to every group peer.

- Sticky `edit_seq > 0` in `has_user_state`: any slot the user has ever
  touched (via any API) is protected from `alloc_slot` eviction, even
  if the values are explicitly back at defaults. Without this,
  "reset offset to 0" looked like an unedited slot and could be evicted.

- Phase A diagnostics: per-frame distribution counters
  (`state_key == 0`, `ext_pos / ext_uv` usage, flag-bit distribution,
  degenerate quads) read by the menu via `frame_diag()`. One-shot CSV
  dump of the entire entry list via `request_entry_csv_dump()`.

### `input_hook.cpp` (extended)

Originally captured only `WM_MOUSEWHEEL` for the freecam wheel-speed
control. Phase B extended it to capture every mouse event when an
mtr-asi UI is visible:

- `WM_MOUSEMOVE` → queued as `Pos`.
- `WM_LBUTTONDOWN/UP` / `WM_RBUTTONDOWN/UP` / `WM_MBUTTONDOWN/UP` →
  queued as `Button` with `button = 0/1/2`.
- `WM_MOUSEWHEEL` → queued as `Wheel` with signed delta.

Each event is recorded with screen-space `(pt.x, pt.y)` from the
`MSLLHOOKSTRUCT`. The hook returns 1 to **swallow** the event — so
DirectInput-exclusive, the game's WndProc, and ImGui's WndProc subclass
all see *nothing* for that event in the same frame. The queue is the
single source of truth.

Drained on the render thread in `menu.cpp`'s `draw_frame` before
`ImGui::NewFrame`. Each event is converted to client coords via
`ScreenToClient(g_hwnd)` and replayed through `io.AddMousePosEvent` /
`io.AddMouseButtonEvent` / `io.AddMouseWheelEvent`.

### `menu.cpp` (extended)

Owns the spatial mouse state machine in the post-menu overlay block:

1. Compute `Factors = sprite_matrix::current_factors()` once.
2. Define `remap` (engine → screen pixel) and `pixel_to_engine`
   (inverse) lambdas — shared across the Hilite overlay, selection
   outline, gizmo draw, and click hit-test, so they cannot drift apart.
3. Find the "selected slot's latest quad" (animated UI re-emits — pick
   the highest `render_order`).
4. Draw selection cyan polyline + filled tint.
5. Translate handle (filled circle at quad centroid) + 4 corner handles
   (filled squares at the quad corners).
6. Drag state machine: persistent `GizmoDrag` static struct holds
   `active`, `slot_idx`, `handle` (-1 = translate, 0..3 = corners),
   anchor mouse pos, anchor offset/scale, anchor center/handle pixel.
7. On left-click hovering a handle: auto-Specialize if slot is wildcard
   (creates a concrete variant pinned to the picked entry's UV /
   screen / quadrant), seed drag state, mark `click_consumed = true`.
8. While left held + dragging: compute new offset (translate) or new
   scale (per-axis ratio with Shift = uniform / Ctrl = snap 0.05),
   write via `set_transform_at`. If `drag_group` is on AND slot has a
   group, broadcast incremental delta to peers.
9. On left release: clear drag, `request_save()`.
10. Right-click on a handle: reset that mode (offset → 0,0 or scale →
    1,1) with auto-Specialize and group broadcast same as left-click.
11. Pick-mode click handler runs only if `!click_consumed`. Tracks
    last-click anchor + timestamp for layered cycling. Delegates to
    `sprite_picking::pick_engine_at(ex, ey, s_pick_layer)`.
12. Esc when selection is active → clears selection (only when
    `selected() >= 0`, so Esc still navigates ImGui menus otherwise).

Plus the small UI controls: Pick-mode checkbox, Drag-group checkbox,
Auto-group checkbox + Apply-now button, Diagnostic TreeNode with the
counter readout + dump CSV button.

### `aspect_patch.cpp` (extended)

Added `sprite_matrix::current_factors()` — the canonical resolver for
the engine's effective sprite-batcher transform this frame. Mirrors
`hk_MatrixSetXformA`'s activation order:

1. `pass_override_factor != 0` → `Fx = override`.
2. `auto_from_rules` + matching `ui_aspect_rules` rule → `Fx = target /
   screen`.
3. `enabled` (manual master) → `Fx = mul_a_a1`, `Fy = |mul_a_a2|`.
4. else → `Fx = Fy = 1`.

`pos_offset_x/y` always added in clip-space units. Used by both the
Hilite overlay and the picking pixel-to-engine conversion — single
source of truth, no drift between visual indicators and what the
engine actually renders.

## Architectural invariants

These exist to keep the interactions between modules sane.

**A1. Engine→screen remap = `((x - 0.5) * Fx + 0.5 + dx*0.5) * W`**.
Everything that converts between engine [0,1]² and screen pixels uses
`sprite_matrix::current_factors()` and the same formula. The Hilite
overlay, selection outline, gizmo handle positions, gizmo drag pixel→
engine inverse, and pick-mode click conversion all agree on this.

**A2. Picking quad submission is unconditional within `state_key != 0`**.
We submit regardless of `flag & 0x1`. Picking-then-failing-to-edit is
a better UX than not seeing the sprite at all. Slot state persists
through eviction via `edit_seq > 0` sticky-protection.

**A3. Pick-mode + gizmo drag share the same click-consumption rule**.
Inside one overlay block: `click_consumed` flag is set by gizmo handle
hit-tests (left or right click on a known handle). Pick-mode handler
gates on `!click_consumed`. Both run after the menu draw, on the
render thread, in fixed order. No double-firing, no race.

**A4. Auto-Specialize on first edit, never on first pick**. Picking a
wildcard slot just selects it. Editing it (left-drag a handle, right-
click a handle) is the moment we fork off a concrete variant pinned to
the picked entry's last-matched composite key. The wildcard parent is
preserved for un-touched variants. This is the no-crutch path —
silently mutating a wildcard would invisibly affect every other
variant sharing the asset.

**A5. WH_MOUSE_LL is the single source of mouse events while UI
visible**. The hook returns 1 to swallow, so the WndProc subclass, DI-
exclusive, and the game's own input pipeline all see nothing. ImGui's
state is hydrated only via the queue drain on the render thread. When
UI is hidden, the hook passes through (except for the freecam wheel
case), and the WndProc subclass resumes its normal role.

**A6. Linked-list iteration order = engine render order**. Verified
empirically via the Phase A entry CSVs: `sort_key` 32768 (background)
appears at list index 0 and the rest ascend up to 399 (font glyphs).
Topmost-wins picking via `render_order = list_index` is correct.
Layered picking dedupes consecutive same-slot hits because atlas-based
UI emits dozens of font glyphs in a row sharing one state_key.

## File map

| File | Role |
|------|------|
| `src/mtr-asi/src/sprite_picking.cpp` | Quad capture, hit-test, selection state, pick-mode toggle |
| `src/mtr-asi/src/sprite_xform.cpp` | Per-slot transforms, picking submission, auto-group, group-drag broadcast, eviction protection, Phase A diagnostics |
| `src/mtr-asi/src/input_hook.cpp` | WH_MOUSE_LL with full mouse capture + drain API |
| `src/mtr-asi/src/menu.cpp` | UI toggles + per-element TreeNode + overlay draw + spatial mouse state machine + LL queue drain |
| `src/mtr-asi/src/aspect_patch.cpp` | `sprite_matrix::current_factors()` resolver |

## Known limitations / next steps

1. **Atlas overlay perception**: the user perceives "I can only pick
   letters" because text glyphs are drawn on top of button frames in
   atlas-based UI. Phase C cycling lets users click through layers.
   No further fix needed — this is the engine's emit order.

2. **Keyboard side**: ImGui keyboard input (`Esc`, modifiers in gizmo,
   InputText typing) currently relies on the WndProc subclass. If DI
   is also exclusive on the keyboard during gameplay (untested), some
   shortcuts won't work in-game. `WH_KEYBOARD_LL` is the natural
   extension; deferred until evidence shows it's needed.

3. **`ext_positions` writes**: `apply_transform` only mutates
   `inline_positions`. If any entries actually USE `ext_positions` for
   render geometry (Phase A captures show zero in our tested screens,
   but wider testing might surface them), transforms won't apply.
   Addressing it would be a `ext_positions` redirect: allocate a
   per-entry buffer, copy the original verts in, write our transform
   to it, and override the entry's `ext_positions` pointer for the
   rest of the frame. Not needed yet.

4. **Group semantics from path**: `derive_group_from_path` is a
   filename heuristic. A more semantic approach would be to derive
   groups from the engine's screen hierarchy at the time of slot
   creation — e.g., "every slot that auto-named while
   `screen_context.top == ScreenWilburMainMenu`" → group "main_menu".
   Defer until users find the basename heuristic insufficient.
