# Sprite-batcher menu-vs-HUD classification findings (Phase 3 M3.2)

12 captures across game states (main menu / loaded gameplay / pause overlay /
tip overlay coexisting with HUD / cutscene / multiple menu screens) analysed
to identify a per-entry classifier for Phase 3 split-pass rendering.

## Summary of findings

The cleanest signal is **vertex position bounding box**. Sprites authored
for menus stay strictly within the unit square `[0, 1]²`; HUD and cutscene
letterbox bars extend outside it. This signal is **stateless, session-
independent, and per-entry** — no state_key fingerprinting required.

Secondary signals (state_key sets, sort_key tiers) confirm the position
classifier's correctness on edge cases.

## Per-capture summary

| Capture                          | Rows   | Distinct state_keys | Top screen           |
| -------------------------------- | -----: | ------------------: | -------------------- |
| GAMEPLAY_HUD                     | 10140  |                  12 | WilburMainMenu (d=7) |
| GAMEPLAY_ESC_MENU                |  9720  |                  12 | WilburMainMenu (d=7) |
| GAMEPLAY_ESC_MENU_map            |  3960  |                  14 | WilburMainMenu (d=7) |
| GAMEPLAY_ESC_MENU_mission        | 11400  |                  16 | WilburMainMenu (d=7) |
| GAMEPLAY_TIP_WINDOW              | 22560  |                  15 | WilburMainMenu (d=7) |
| CUTSCENE_MOM                     |  7560  |                   3 | WilburMainMenu (d=7) |
| MAIN_MENU                        | 12240  |                   8 | GameSelectScreen     |
| MAIN_MENU_load_save              |  7620  |                   8 | WilburNewLoadSave    |
| MAIN_MENU_options                | 15960  |                  11 | GameSelectScreen     |
| MAIN_MENU_2_cheats               | 25980  |                   7 | CheatsScreen         |
| MAIN_MENU_2_extras               |  8100  |                   7 | WilburExtras         |
| MAIN_MENU_2_loaded_save          | 10260  |                   7 | WilburMainMenu       |

> Note on `top_screen`: in-game captures all show `WilburMainMenu` because
> Wilbur's screen stack always has MainMenu sitting at depth=7 even during
> gameplay. The screen_push mirror tracks pushes/pops correctly, but the
> "top of stack" stays MainMenu in many gameplay states. This is why
> `screen_push`-based gating alone (Phase 2 approach) can't distinguish
> "pure HUD" from "HUD + ESC menu" at the top-of-stack level — the names
> are identical.

## Position bbox classifier (the winner)

| Capture                          | x_min  | x_max  | y_min  | y_max  | Inside [0,1]²? |
| -------------------------------- | ------:| ------:| ------:| ------:| -------------- |
| MAIN_MENU                        |   0.000|   0.886|   0.000|   0.901| **YES**        |
| MAIN_MENU_2_cheats               |   0.000|   0.883|   0.001|   0.901| **YES**        |
| MAIN_MENU_2_extras               |   0.000|   0.565|   0.001|   0.901| **YES**        |
| MAIN_MENU_2_loaded_save          |   0.000|   0.849|   0.000|   0.901| **YES**        |
| MAIN_MENU_load_save              |   0.000|   0.860|   0.001|   0.901| **YES**        |
| MAIN_MENU_options                |   0.000|   0.935|   0.000|   0.901| **YES**        |
| GAMEPLAY_HUD                     |  -0.283|   1.364|  -0.015|   0.888| NO (HUD)       |
| GAMEPLAY_TIP_WINDOW              |  -0.283|   1.364|  -0.015|   0.888| NO (HUD bits)  |
| GAMEPLAY_ESC_MENU                |  -0.064|   1.061|  -0.031|   0.901| NO (HUD bits)  |
| CUTSCENE_MOM                     |   0.000|   1.364|   0.000|   0.900| NO (letterbox) |

**The pattern is unambiguous:** all menu screens stay strictly inside
`[0, 1]²`. Anything outside is HUD or letterbox content authored to extend
to screen edges.

### Per-entry classifier rule

```
For each SpriteEntry e with 4 verts (XYZ at +0x28, 12 floats):
    is_menu = all 4 verts have x ∈ [0, 1] AND y ∈ [0, 1]
```

If `is_menu` → apply pillarbox transform (4:3 safe-area protection).
Else → leave alone (HUD/letterbox stays full-screen).

This is a **single bounding-box check per entry**, ~24 float compares.
~22000 entries/frame max in our captures → ~528000 compares/frame, well
under microsecond-scale on modern CPUs. Zero state needed; no
fingerprinting; no per-session data; works for first-frame classification.

### Edge cases the classifier handles correctly

1. **Pause overlay (ESC_MENU) — HUD + menu coexist.**  HUD bits have x
   reaching 1.061 (correctly classified as HUD); menu bits stay in
   `[0, 0.901]` (correctly classified as menu).
2. **Tip window over HUD.** Tip is in `[0, 1]²` (menu); HUD bits leak
   outside (HUD).
3. **Cutscene letterbox.** Bars reach x=1.364 (correctly classified as
   HUD; we don't pillarbox letterbox bars — that would shrink them).
4. **Centered HUD elements.** A centered crosshair fully inside `[0,1]²`
   would be misclassified as menu and get pillarboxed. Wilbur doesn't
   appear to have any (HUD elements are all corner-anchored / edge-
   anchored), but worth noting as a known limitation.

## Secondary findings (corroborating evidence)

### state_key sets — confirms textures cluster per state

| Test | Result |
| ---- | ------ |
| HUD-exclusive keys (in HUD, NOT in MAIN_MENU)         | **11 of 12** |
| MENU-exclusive keys (in MAIN_MENU, NOT in HUD)        | **7 of 8**   |
| Shared (HUD ∩ MAIN_MENU)                              | **1** (the font texture `0x1976ED00`) |
| TIP \\ HUD (3 tip-specific keys)                      | NOT in MAIN_MENU |
| ESC \\ HUD (10 esc-menu-specific keys)                | NOT in MAIN_MENU |

`0x1976ED00` is shared because it's the global font/text texture used by
both HUD text (mission text, button hints) and menu text (button labels,
dialog). State_key alone cannot differentiate text rendering.

### sort_key tiers — distinguishes text origin

For the shared font texture `0x1976ED00`:
- HUD context: `sort_key ∈ {380, 399}` (5160 entries in HUD capture)
- Menu context: `sort_key ∈ {400, 401, 402, 403, ..., 1100, ..., 1300}` (11400 entries in MAIN_MENU)

So even text-only entries cleanly separate by sort_key. (The position
classifier handles this implicitly — text uses inline_positions like any
other quad, and HUD-positioned text has those positions outside `[0, 1]`.)

### state_key stability across same-screen captures

| Screen     | Captures           | Keys consistent? |
| ---------- | ------------------ | ---------------- |
| MAIN_MENU  | base / loaded-save | shared 5 of 8    |
| Options    | options capture    | superset of base |
| Cheats     | base + cheats      | shared 5 of 7    |
| Extras     | base + extras      | shared 4 of 7    |

State_keys are stable WITHIN a session (allocations don't move). They
DIFFER across sessions (heap allocation order, level loads). So
state_key whitelisting would require either learning at runtime or
falling back to position-bbox.

### Flags

`flags & 0x7F` is dominated by `0x0020` (per-vertex color path) across
all captures. `0x0000` (no-flags-set) appears only in MAIN_MENU captures
(660 / 12240 = 5.4%). Not a strong classifier but might tag certain
solid-fill menu rectangles.

## M3.3 design recommendation

Use the **position-bbox classifier** as the primary signal:
- Stateless: no learning, no fingerprinting needed.
- Session-independent: no allocator-pointer dependency.
- Per-entry: granular enough for HUD-and-menu coexistence.
- Trivially correct for letterbox: bars stay full-screen.

Implementation outline (to be detailed in M3.3 design doc):

```
wrapper_render_sprite_batcher (replaces the call at 0x4D23BF):

    // Save original entry flags + classify entries
    for each entry e in g_sprite_list_head:
        original_flags[e] = e->flags
        is_menu[e] = bbox_inside_unit_square(&e->inline_positions)
    
    // Pass 1: render HUD (entries outside [0,1]^2) WITHOUT pillarbox
    for each entry e:
        e->flags = original_flags[e]
        if is_menu[e]:
            e->flags |= 0x40    # mark to skip
    sprite_matrix_override(passthrough)
    call render_sprite_batcher  # renders HUD only
    
    # Pass 2: render menu (entries inside [0,1]^2) WITH pillarbox
    for each entry e:
        e->flags = original_flags[e]
        if not is_menu[e]:
            e->flags |= 0x40    # mark to skip
        e->flags &= ~0x80       # clear consumed bit from prior pass
    sprite_matrix_override(pillarbox factor from ui_aspect_rules)
    call render_sprite_batcher  # renders menu only, pillarboxed
    
    # Restore
    for each entry e:
        e->flags = original_flags[e]
```

Concerns / open questions for M3.3:
1. **Cost of running render_sprite_batcher twice.** Each invocation is a
   D3D state setup + draw-list emit. Doubles the per-frame cost of UI
   rendering. In practice UI is a tiny fraction of frame time; should be
   imperceptible.
2. **Sort interaction.** Pass 1 calls the SecuROM-thunked
   `sub_4E8AE0` (sort) when it detects out-of-order keys. Sort runs
   before we restore flags for pass 2 — should be safe since we re-mark
   between passes.
3. **State_key shared between HUD and menu (font).** Position classifier
   handles this directly: HUD text has out-of-bounds positions, menu
   text has in-bounds. Confirmed in data.
4. **Flag bit 0x40 / 0x80 semantics.** Reverse-engineered from
   render_sprite_batcher's two passes:
   - 0x40 set → pass 1 sets 0x80 → pass 2 skips
   - 0x40 clear → pass 1 clears 0x80, processes normally
   This is the perfect mechanism for selective skipping — don't touch
   the list structure, just flip a bit per-entry per-pass.

## Files

- `mtr-asi-sprite-probe_*.csv` — raw captures in `Game/`
- `research/findings/sprite-probe-analyze.py` — analysis script
- `research/findings/sprite-list-layout.md` — M3.1 entry layout decode
- `memory/project_phase3_sprite_probe.md` — Phase 3 status
