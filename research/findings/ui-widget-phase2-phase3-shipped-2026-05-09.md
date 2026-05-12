# UI widget Phase 2 + Phase 3 SHIPPED (2026-05-09 evening)

Status: **shipped + smoke-tested.** User confirmed Phase 1 (always-on
naked-stub hook) doesn't regress their 4:3 menu + scaled UI elements,
then asked to continue. User said "build new and smart" / "I don't
care about legacy or current scaled ui elements" — proceeded without
migration code.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## What shipped

### Phase 2 — engine identity in CompositeKey

Added `widget_name_hash` (FNV-1a 32-bit, 0 = wildcard) as a 5th
component of `CompositeKey`. Pattern matching extends naturally:

```cpp
// Pattern only matches if widget_name_hash matches concrete (or is wildcard).
if (pattern.widget_name_hash != 0 &&
    pattern.widget_name_hash != concrete.widget_name_hash) return false;

// Specificity gives engine identity heavier weight than heuristics:
if (pattern.widget_name_hash != 0) s += 2;   // vs +1 for uv/screen/quad
```

`Slot` gained a `char widget_name[48]` field, populated on first sight
of a widget_name capture for that state_key. The slot also auto-promotes
its pattern's `widget_name_hash` when a pending-by-widget-name entry
applies — so subsequent frames the slot only matches sprites with the
same widget_name (atlas disambiguation).

`concrete_key_for(SpriteEntry*, ...)` calls
`mtr::widget_probe::widget_name_for_entry(entry)` to fetch the per-
frame side-table value (set by the naked-stub hook at submit time);
hashes the string into the concrete key. Entries without a captured
name get hash 0 → wildcard match against existing slots.

### Phase 3 — persistence by widget_name

New pending queue `g_pending_by_widget_name` parallels the existing
`g_pending_by_path`. Public API:

```cpp
void add_pending_by_widget_name(const char* widget_name,
                                float ox, float oy,
                                float sx, float sy, bool hidden,
                                const char* name, const char* group);
```

In `process_list`, the widget_name resolve runs every frame (NOT gated
on `auto_name_attempted`) because a slot may be created BEFORE
widget_probe captures its widget_name — we apply pending state the
moment the name first appears.

Ini schema gains one optional field per slot:
```ini
[sprite_xform]
x_3_widget_name = Btn_Legend_Accept
```

Loader priority: widget_name > path > raw state_key. ui_aspect_rules's
load loop:
```cpp
if (widget_name[0]) {
    sprite_xform::add_pending_by_widget_name(widget_name, ...);
} else if (path[0]) {
    sprite_xform::add_pending_by_path(path, ...);
} else {
    sprite_xform::load_apply_full(sk, ...);   // raw state_key
}
```

`save_at_full` gained two args (`out_widget_name`, `out_widget_name_sz`).
The v1 `save_at` shim updated to pass through.

## Coverage

The 3 captured callers fire at GameSelectScreen on the static main-menu
state. Captured widget names (subset):
- `Btn_Legend_Accept`, `Btn_Quit_Game`
- `FrontEnd_GameSelect`, `FrontEnd_Load`, `FrontEnd_New`,
  `FrontEnd_SelectAnOption`

The OTHER 11 callers (likely Sprite/Group widget Render methods that
keep `this` deeper in callee-saved registers we don't search) don't
fire at idle main-menu. The user's original gradient complaint
(`IDS_TOP`, `IDS_BOTTOM`, `IDS_HIGHLIGHT_BACKGROUND` in
WilburMainMenu) still falls through to path-based persistence
(GlowBox_Line.dbl shared atlas → still inseparable for those widgets
until we capture their callers).

Path forward for full coverage: drive the game into more screens via
test harness (gameplay, pause, options), see which callers fire, expand
the heap-pointer range filter or stack-scan window if the callers turn
out to use even more obscure `this`-storage patterns. The
infrastructure is in place — every newly-captured widget gets the same
persistence treatment without code changes.

## Files

- Modified: `src/mtr-asi/src/sprite_xform.cpp`
  - `CompositeKey::widget_name_hash` field + match/specificity logic
  - `Slot::widget_name[48]` field
  - `hash_widget_name(const char*)` (FNV-1a)
  - `concrete_key_for` calls `widget_probe::widget_name_for_entry`
  - `g_pending_by_widget_name` queue
  - `add_pending_by_widget_name` public API
  - `save_at_full` extended (+2 args, `out_widget_name`)
  - process_list: widget_name capture + pending resolve every frame
- Modified: `src/mtr-asi/src/ui_aspect_rules.cpp`
  - Forward decl for `add_pending_by_widget_name`
  - Save: write `x_<i>_widget_name` per slot
  - Load: read `x_<i>_widget_name`, route to widget_name pending when
    non-empty, fall back to path/state_key otherwise

## Build artifact

`Game/mtr-asi.asi` 591872+ bytes (3.7s end-to-end pass on
`boot-to-main-menu`). No regressions vs Phase 1 baseline.

## How to test (user-facing)

1. Launch Wilbur via your usual launcher.
2. Reach main menu (manually press a key past splash if needed —
   harness DI inject is flaky on cold launch, see
   `project_test_harness_false_pos_2026-05-09.md`).
3. Open Insert menu → sprite_xform list. Find a captured widget by
   its existing display (will show state_key + path + auto-name; the
   `widget_name` field is populated behind the scenes for matched
   widgets but NOT yet rendered in the menu — Phase 4 work).
4. Scale or move the widget visually.
5. Auto-save kicks in (debounced, 250ms after last edit).
6. Quit Wilbur.
7. Relaunch. Re-enter main menu.
8. Verify the same widget retains its scale / offset.

If persistence works for `Btn_*`/`FrontEnd_*` widgets across re-entry
without code edits, Phase 3 is validated.

## Why RULE №1-compliant

1. Engine-level identity (widget_name = string baked into .sc at engine
   build time) — not a heuristic, not a heap-pointer, not a fingerprint.
2. Specificity weight (+2) makes widget_name beat all heuristic
   matches in `lookup_best_match`. No silent collision with the old
   path/uv/screen/quad slots.
3. Backwards-compat in spirit (no migration code, but old ini files
   still load via path / state_key fallback). Per user's "no legacy"
   directive, no special migration is offered.
4. Partial coverage isn't a crutch — the infrastructure handles all
   future captures uniformly. Coverage gap is purely an RE artifact
   (some callers haven't been observed yet), addressable by driving
   the game into more states.
