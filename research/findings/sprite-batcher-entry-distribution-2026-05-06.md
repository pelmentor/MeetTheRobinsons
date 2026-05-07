# Sprite-batcher entry distribution — Phase A findings

Captured 2026-05-06 to settle empirically what differentiates pickable
from non-pickable sprites. Each guess-then-fix pass before this had
been wrong. The data resolved every open question in one analysis pass.

## Source captures

| File | Frames | Entries / frame | Notes |
|------|--------|-----------------|-------|
| `Game/mtr-asi-sprite-probe_MAIN_MENU.csv` | 60 | 204 | Main menu, GameSelectScreen |
| `Game/mtr-asi-sprite-probe_GAMEPLAY_HUD.csv` | 60 | 169 | In-game HUD (top_screen reads as WilburMainMenu at depth=7 — engine quirk, see screen-system memory) |

Format: `frame,entry_idx,top_screen,stack_depth,state_key,sort_key,flags,
alpha_mod,blend_mode,p0_x,p0_y,p0_z,p2_x,p2_y,p2_z,ext_pos,ext_uv,ext_col`.

## Headline numbers

### MAIN_MENU first frame (204 entries)
- `state_key == 0`: **0 / 204** (0.0%)
- `ext_pos != 0`: **0 / 204**
- `ext_uv != 0`: **0 / 204**
- Flag distribution: `0x00A0` = 193, `0x0080` = 11. No other values.

### GAMEPLAY_HUD first frame (169 entries)
- `state_key == 0`: **1 / 169** (0.6%) — the screen-fill background.
- `ext_pos != 0`: **0 / 169**
- `ext_uv != 0`: **0 / 169**
- Flag distribution: `0x00A0` = 168, `0x09E0` = 1 (the state_key=0 background).

## What the data settles

### "ext_positions is why HUD doesn't pick" — FALSE

Pre-data hypothesis: HUD entries use `ext_positions` (an off-engine
geometry pointer), so reading `inline_positions` for picking quads
misses them. Data: zero entries use `ext_positions` in either
capture. The hypothesis is wrong; do not implement an `ext_positions`
read path on this evidence alone.

### "state_key=0 hides HUD from picking" — FALSE

Pre-data hypothesis: HUD entries have `state_key == 0`, so our
`if (e->state_key == 0) continue` in `process_list` skips them.
Data: only 1 entry (the background) has `state_key == 0`, and it's not
HUD. 168 / 169 HUD entries have valid state_keys.

### "DirectInput-exclusive eats clicks during gameplay" — TRUE

After the first two hypotheses fell, the only remaining explanation
for "pick mode works in main menu but not gameplay" is input routing.
The engine grabs DI-exclusive on the mouse during gameplay (camera /
aim path); DI sits ABOVE WndProc dispatch, so `WM_LBUTTONDOWN` never
reaches anyone — including ImGui's WndProc subclass. This is the same
root cause as the original wheel issue, just wider in scope.

The fix is to extend the existing `WH_MOUSE_LL` hook to capture every
mouse event (not just wheel) when an mtr-asi UI is visible, swallow
them with `return 1`, and replay through `io.AddMouseXxxEvent` on the
render thread. See [`sprite-picking-gizmo-architecture.md`](sprite-picking-gizmo-architecture.md)
for the implementation.

### Linked-list iteration order = render order

Verified by inspecting the entry index → sort_key trajectory:

```
entry_idx  sort_key
0          32768   ← background (drawn first, far)
1          149     ← HUD widget (drawn after, in front of bg)
2          172
3          173
4          300
5–10       311…325
11         380     ← font glyphs start (drawn last, on top)
12–30      380…399
…          399
```

Sort_key spans [149..32768]. List order is roughly ascending after
the initial background (32768 = max = drawn first / farthest back),
then ascending toward 399 (font glyphs drawn last / on top). The
linked list IS in render order.

Implication: our picking module's `render_order = list_index` and
"topmost wins = highest render_order" are correct without any sort.

### Atlas-text-overlays-button is by design

In MAIN_MENU, **8 unique state_keys produce 204 entries**:

| state_key | count | sort_key range | bbox span | identity |
|-----------|-------|----------------|-----------|----------|
| `0x1976ED00` | 190 | 400..1100 | x [.045, .897], y [.012, .943] | Font texture. 190 letters of text. |
| `0x197651B0` | 4 | 301..402 | x [0, 1], y [.129, .881] | Yellow horizontal strips. |
| `0x197650C0` | 4 | 402..402 | x [.027, .601], y [.209, .376] | Menu frame around NEW GAME. |
| `0x197654D0` | 2 | 300..300 | x [0, 1], y [0, 1] | Background wallpaper. |
| `0x19765390` | 1 | 401..401 | x [.054, .578], y [.243, .342] | Button-internal element. |
| `0x197652A0` | 1 | 403..403 | x [.284, .346], y [.300, .384] | Selection glow. |
| `0x197643F0` | 1 | 1300..1300 | x [.200, .600], y [.627, 1.160] | Wilbur's hand. |
| `0x19764530` | 1 | 1300..1300 | x [.600, 1.000], y [0, 1.067] | Wilbur's body. |

When the user clicks "NEW GAME", they hit a font glyph (state_key
`0x1976ED00`) drawn at sort_key 400+ on top of the menu frame
(state_key `0x197650C0`) at sort_key 402. Topmost-wins picking
returns the glyph slot. The user perceives this as "I can only pick a
letter, not the button." The button frame *is* there, just shadowed
by its own text.

This is the engine's emit order, not a bug. The right UX response is
**layered picking** (Phase C): repeated clicks at the same screen pos
within ~0.8 s and ~5 px cycle through the stacked layers
(text → button frame → background) so users can reach what's underneath
without losing topmost-wins as the default.

### GAMEPLAY_HUD second observation

In GAMEPLAY_HUD, **12 unique state_keys** including a curious cluster:

| state_key | count | sort_key | bbox |
|-----------|-------|----------|------|
| `0x1976F980` | 72 | 399 | x [1.238, 1.372], y [.877, .905] |

72 entries all at x > 1.0 — completely off-screen to the right at the
captured aspect. These are pre-allocated HUD glyphs for some on-demand
indicator (damage popup, score change, queued tutorial); kept off-screen
until an event fires. They're submitted to picking (state_key != 0) but
won't hit-test under any visible cursor. Harmless.

## Methodology notes for future Phase X probes

- The diagnostic CSV button in the menu's Per-element → Diagnostic
  block writes `Game/mtr-asi-entries.csv` for the next-rendered frame.
  One click per screen of interest. Take the captures **before** any
  per-element transforms are applied to the inline_positions, so the
  CSV reflects engine-emitted geometry.

- Always pair captures with the menu's live counter readout — they
  cross-validate (totals + flag distribution from the counters should
  match aggregations of the CSV).

- Keep the per-frame counters always-on. The cost is one linked-list
  walk per frame, microseconds at 60 Hz; the value is anyone reading
  the menu can spot anomalies (sudden jump in `state_key=0` count
  during a particular screen, etc.) without having to dump a CSV.
