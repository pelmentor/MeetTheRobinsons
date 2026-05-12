# UI element identity refactor — plan (2026-05-09)

Date: 2026-05-09 evening (overnight planning while user sleeps)
Status: **REVISED post-audit 2026-05-09 23:55** — see "Audit findings" section
near the bottom. Two agents (Plan + code-reviewer) found critical issues with
the original plan; the revised approach favours a 3-5 day MVP path over the
8-15 day full refactor. Phase 0 RE proceeds autonomously while the user
sleeps; no code deploy until plan choice approved.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## User's complaint (verbatim)

> We need to refactor how we control the UI ELEMENTS, for example there's
> gradient thing in main menu on top and bottom and on the selection option
> (dynamic). Currently if I scale the gradient then I get what I want —
> stretch the bottom and upper gradients to fill the wide screen, but it
> also stretches the selection option gradient which looks ugly. And it's
> not persistent (main menu is proved to be persistent) but when I quit from
> gameplay to main menu — my changes are gone, new objects in memory fill
> that ui element address. I need a new smart ways of handling ui elements
> — very robust, very flexible and persistent.

Two distinct problems:

1. **Disambiguation failure**. Top gradient + bottom gradient + selection
   gradient all share atlas (= same `state_key`) and may share UV region
   (= same `uv_bucket`). The dynamic selection moves with input, so
   `bbox_quadrant` is unstable. Result: scaling one scales them all.

2. **Re-entry persistence failure**. Going gameplay → main menu re-creates
   the menu's UI element heap objects. Their `state_key` values change
   (heap-pointer-based identity). The user's edits don't follow.

## Discovery: engine has stable per-widget IDs

`Game/data/screens/*.h` files contain `#define`'d widget IDs for every UI
element in every screen. Example from `WilburMainMenu.h`:

```c
#define WILBURMAINMENU            (256)   // the screen itself
#define IDG_WILBUR                (257)   // group: Wilbur character art
#define IDS_WILBUR_HAND           (258)   // sprite: hand piece
#define IDS_WILBUR_BODY           (259)   // sprite: body piece
#define IDG_MENU                  (260)   // group: menu item list
#define IDT_BEGIN_GAME            (263)   // text: "Begin Game"
#define IDG_HIGHLIGHT             (264)   // group: highlight overlay
#define IDS_BOTTOM                (267)   // sprite: BOTTOM GRADIENT
#define IDS_TOP                   (270)   // sprite: TOP GRADIENT
#define IDS_HIGHLIGHT_BACKGROUND  (271)   // sprite: SELECTION GRADIENT (dynamic)
#define IDT_BEGIN_GAME            (263)   // text: "Begin Game"
// ... etc — 24 widgets in this screen alone
```

Naming convention:
- `IDS_*` = sprite (image)
- `IDT_*` = text label
- `IDG_*` = group (container)

The user's three gradients have **distinct IDs** (267, 270, 271). Engine-level
identity that's already stable across sessions, semantically meaningful, and
tied to source of truth.

Per `project_screen_system.md`: each screen has a hardcoded ctor that loads
its `.sc` data file. The .h IDs are passed to widget-creation calls during
ctor execution. Each widget object gets its ID stored on it.

## What we need to RE

1. **Widget→sprite link**. When a sprite is pushed to the render list (via
   the engine's sprite-push function — referenced in
   `project_per_element_v2_shipped.md` as "sprite pushers behind stolen-byte
   IAT thunks"), we need to capture its widget ID. Options:
   - Hook the push function. Read the calling widget's ID field.
   - Or: walk back from `state_key` or `state_key+0x2C` (which we already
     read for path) to a widget object that holds the ID.
   - Or: the SpriteEntry's `unk_04` / `unk_0C` / `unk_14` / `unk_18` fields
     might already be the widget ID.

2. **Widget object layout**. Where is the ID stored on the runtime widget
   object? Probably at a fixed offset.

3. **Per-screen ID space**. Same ID number across different screens means
   different widgets. So full identity = `(screen_name, widget_id)`.

4. **Repeated widgets**. Some screens may instantiate the same widget
   template multiple times (e.g., a list of save slots all sharing
   `IDS_SAVE_SLOT`). Need disambiguator — probably `instance_index`.

## Proposed identity scheme (post-refactor)

Replace `CompositeKey { state_key, uv_bucket, screen_context, bbox_quadrant }`
with:

```cpp
struct WidgetIdentity {
    char     screen_name[24];   // e.g. "WilburMainMenu"
    uint16_t widget_id;         // e.g. 270 (= IDS_TOP)
    uint8_t  instance_index;    // 0 unless screen has multiple instances
    // Optional fallback for sprites the engine draws without a widget ID
    // (cursor, debug overlays, particles attached to widgets):
    uint32_t fallback_state_key;
    uint16_t fallback_uv_bucket;
};
```

Lookup priority:
1. By `(screen_name, widget_id, instance_index)` — engine-level identity,
   stable forever.
2. Fall back to `(state_key, uv_bucket)` for sprites without a widget ID.

Persistence ini key example:
```ini
[ui_widgets]
WilburMainMenu/270/0/scale_x = 1.5    # IDS_TOP — stretch wide
WilburMainMenu/270/0/scale_y = 1.0
WilburMainMenu/267/0/scale_x = 1.5    # IDS_BOTTOM — stretch wide
WilburMainMenu/267/0/scale_y = 1.0
WilburMainMenu/271/0/scale_x = 1.0    # IDS_HIGHLIGHT_BACKGROUND — UNCHANGED
WilburMainMenu/271/0/scale_y = 1.0
```

Three different gradients, three different settings. User can stretch top/bottom
without touching selection.

## Phase plan (~8-10 days)

### Phase 0 — Widget→sprite link RE (1-2 days)

Find the function that pushes a SpriteEntry to the list (head at
`0x7271E8`). Hook it, capture caller's return address + any pointer args.
For each unique caller, RE the calling widget's class to find the widget ID
field.

Tools:
- IDA xrefs to the sprite list head.
- Runtime probe in mtr-asi: log `(state_key, _ReturnAddress(), arg0, arg1)`
  for the first ~50 distinct entries. User runs main menu, sends log.
- Cross-reference with `Game/data/screens/WilburMainMenu.h` — once we know
  e.g. the top gradient's caller, we can deduce that's `IDS_TOP`.

Output: knowledge of where the widget ID lives at runtime.

### Phase 1 — Widget ID extraction (1 day)

Write a pure-C++ helper in mtr-asi that, given a `SpriteEntry*`, returns
`(screen_name, widget_id, instance_index)` or "no widget" if the sprite was
pushed without a widget context (e.g. cursor).

Tested by walking the sprite list during main menu and verifying every
expected widget ID from `WilburMainMenu.h` appears (24 widgets → 24 IDs).

### Phase 2 — .h file parser (½ day)

At mod load time, scan `Game/data/screens/*.h`. Parse `#define IDx_NAME (NUM)`
into a map `(screen_name, widget_id) → widget_name`. Used for auto-naming
in the UI list.

Cheap one-time pass; ~56 files × ~20 lines each.

### Phase 3 — Identity refactor in sprite_xform (2 days)

Replace `CompositeKey` with `WidgetIdentity`. Slot lookup becomes:
- Fast path: hash by `(screen_name, widget_id, instance_index)`.
- Fallback: by `(state_key, uv_bucket)` if no widget ID.

Migration:
- v2 ini files load as legacy slots (in a separate `g_legacy_slots` array).
- On first widget-ID match for a sprite that previously had a v2 slot, the
  user can hit a "Migrate to widget ID" button (or it auto-migrates).
- v2 slots stay around for sprites that genuinely have no widget ID.

### Phase 4 — Cross-session + re-entry persistence (1 day)

- Persistence keyed by widget identity, not heap pointers.
- Re-entry test: edit gradient in main menu → start gameplay → exit to main
  menu → verify edit persists.
- Cross-session test: edit → quit Wilbur → relaunch → enter main menu →
  verify edit persists.

### Phase 5 — UI improvements (1 day)

- Tree grouped by screen → widget. E.g.:
  ```
  WilburMainMenu (24 widgets)
    Sprites
      IDS_TOP (270)
      IDS_BOTTOM (267)
      IDS_HIGHLIGHT_BACKGROUND (271)
      ...
    Text
      IDT_BEGIN_GAME (263)
      ...
    Groups
      IDG_MENU (260)
      ...
  ```
- Picker: when user clicks an element, show "Selected: WilburMainMenu /
  IDS_HIGHLIGHT_BACKGROUND (271)".
- Filtering: hide widgets not currently visible (so user doesn't see the
  full 56-screen list when only 1 screen is up).

### Phase 6 — Bulk operations by widget category (½ day)

Already partially shipped via `derive_group_from_path`. Rework to use
`IDS_/IDT_/IDG_` prefix as the natural grouping. Bulk: "scale all sprites
in WilburMainMenu by 1.2", "hide all text in HUD".

### Phase 7 — Cleanup (½ day)

Once new system proven:
- Remove `bbox_quadrant` (unstable, useless once we have widget IDs).
- Demote `state_key` from primary key to a fallback-only field.
- Clean up `state_key_probe.cpp`'s path-extraction (keep as fallback for
  widget-less sprites).

## Dependencies / risks

- **Phase 0 RE could find that some sprites genuinely have no widget ID**
  (e.g. cursor, debug overlay, particles). Falls back to v2-style identity.
  Acceptable.

- **Repeated-instance disambiguation may need additional RE.** If the
  engine reuses the same widget ID for multiple instances (e.g., 4 menu
  buttons all using `IDT_MENU_ITEM`), we need an instance index. The engine
  probably already has one — but we don't know yet.

- **Custom ASI-loaded screens** (none currently) wouldn't have .h files.
  Out of scope.

- **The .h files are part of the game's build** — they were meant to be
  preprocessor inputs to the C++ source, not runtime data. The IDs are
  baked into Wilbur.exe's compiled code. We're using them as documentation
  to map runtime IDs to readable names.

## What lives ALREADY that we keep

- [`sprite_xform.cpp`](../../src/mtr-asi/src/sprite_xform.cpp): scaffolding
  (slot array, eviction, edit_seq, ini load/save, picking integration).
  Composite-key code gets refactored, but the surrounding infrastructure
  stays.

- [`sprite_picking.cpp`](../../src/mtr-asi/src/sprite_picking.cpp): pick
  mode + handles. Just needs to surface widget identity in selection state
  instead of state_key.

- [`state_key_probe.cpp`](../../src/mtr-asi/src/state_key_probe.cpp):
  existing path-extraction kept as fallback for widget-less sprites.

- [`ui_aspect_rules.cpp`](../../src/mtr-asi/src/ui_aspect_rules.cpp): per-
  screen rules for sprite_matrix factors. Untouched — different concern.

## Why this is RULE №1-compliant

1. **Engine-level identity**: we use IDs the engine itself defines and
   uses internally. Not a fingerprint approximation, not a heuristic.
2. **Stable across all sessions and re-entries**: IDs are baked into
   compile-time constants in the engine.
3. **Self-documenting**: the .h files give us human-readable names "for
   free", no manual labelling needed.
4. **Smaller code**: simpler key, simpler lookup, simpler ini, fewer edge
   cases.
5. **Backwards compatible**: v2 ini files keep working via fallback path.

## Decision needed before starting

1. **Approve the multi-day plan**, OR scope down to a quick fix (e.g., just
   add `call_site` to existing CompositeKey as a stopgap — would solve the
   gradient case but not the persistence case).

2. **Phase 0 first or skip?** If we skip the RE phase and the widget ID
   is in an unknown place, we'd need iterative probing. Phase 0 first is
   safer (RULE №1).

3. **Start now (autonomous overnight) or wait for daytime?** I have IDA
   access; Phase 0 RE can proceed without user input. Result is just static
   analysis output — nothing deployed until plan approved.

## Quick-fix alternative (if user prefers)

If the user wants the gradient case fixed FAST without the full refactor,
the cheapest stopgap is:

1. Add `call_site` (capture from `_ReturnAddress()` of the engine's sprite-
   push function) to the existing `CompositeKey`. ~½ day.
2. Re-Specialize the gradients with the new disambiguator.

Doesn't solve the re-entry persistence problem (state_key still primary).
But would make top/bottom gradients distinguishable from selection gradient.

The full refactor is the proper RULE №1 fix; the stopgap is acknowledged
as a partial solution. User decides.

---

## Audit findings (2026-05-09 23:55)

Two agents reviewed the plan and converged on critical findings. The plan
above is preserved verbatim above this line for reference, but **should
NOT be executed as written** — see revised approach below.

### Critical finding #1: widget identifiers are STRINGS, not numeric IDs

The Plan agent read `Game/data/screens/WilburMainMenu.sc` directly and found
that the binary screen-data contains widget identifiers as **plain ASCII
strings**, not numeric IDs:

```
"Sprite WilburMainMenu" ... "ScreenWilburMainMenu"
"Sprite IDG_WILBUR" ... "Sprite IDS_WILBUR_HAND" ... "wilburBottom.dbl"
"Sprite IDS_BOTTOM" ... "GlowBox_Line.dbl"
"Sprite IDS_TOP" ... "GlowBox_Line.dbl"
"Sprite IDS_HIGHLIGHT_BACKGROUND" ... "whitesprite.dbl"
```

**The `.h` files are just C-preprocessor symbolic constants** for the build
pipeline that generated the .sc binary. The numeric ID `270` may not exist
anywhere at runtime — only the string `"IDS_TOP"`. The engine almost
certainly stores `m_pcName`-style string fields on widget objects (the
existing `state_key_probe.cpp` already reads a `+0x2C` path-string field
on the texture target — strong precedent).

**Implications**:
- Identity should be **`widget_name` (string), not `widget_id` (uint16)**.
- `WilburMainMenu/IDS_TOP/scale_x = 1.5` in the ini — self-documenting.
- **Phase 2 (.h parser) is unnecessary** — strings are right there in .sc.
- Cross-screen semantic match: same name in different screens preserves
  meaning even though the numeric ID would differ.

### Critical finding #2: MVP path > full refactor

Both agents independently recommend the same alternative: a 3-5 day MVP
that adds `call_site` to the existing CompositeKey, instead of the
proposed 8-15 day full refactor.

**MVP architecture**:

1. Hook the engine's sprite-push function. Capture `_ReturnAddress()` per
   call. Add `call_site` (uint32 code address) as a 5th component of the
   composite key. **~½ day**.
2. Persist by call_site instead of state_key. Call sites are addresses
   inside Wilbur.exe → **fixed for the life of the binary, immune to
   heap-pointer churn**. Solves re-entry persistence directly. **1 day**.
3. Map call sites → widget-string-name via Phase 0 RE result (one-shot
   correlation). Auto-name slots from the mapping. **1-2 days**.

This delivers all three of the user's stated wins (disambiguation +
persistence + readable names) without the WidgetIdentity refactor. Existing
v2 work continues to function. The full refactor becomes optional polish.

**Why call_site is robust for persistence**:
- Different gradients are pushed by different engine functions (top gradient
  has its own draw call site; selection gradient has another).
- Code addresses are baked into Wilbur.exe → **identical across sessions**,
  identical across menu re-entries.
- More stable than state_key, more stable than widget IDs would be (IDs
  also work but require the .sc loader to keep them).

### Critical finding #3: Phase 0 RE strategy was wrong

Plan suggested hooking the engine's sprite-push function and post-correlating
callers with widget IDs. Both agents recommend hooking the **widget's own
`Render()` method** instead:

> Most engines have a virtual `Widget::Render(this, batcher)` that internally
> calls `batcher->push_sprite(...)`. Hooking the push function and reading
> `_ReturnAddress()` lands you inside `Render` — and `[esp+xx]` near that
> return is the widget `this` pointer. From `this->m_pcName` you get the
> string ID directly.

Cleaner than post-correlation; fewer false positives.

Also noted: dump SpriteEntry's `unk_04` / `unk_0C` / `unk_14` / `unk_18`
across many entries and look for the same string IDs we see in the .sc
file — could short-circuit the entire RE in 30 minutes.

### Critical finding #4: migration "auto on first match" is broken

`IDS_TOP`, `IDS_BOTTOM`, `IDS_HIGHLIGHT_BACKGROUND` all use the same atlas
texture (`GlowBox_Line.dbl` for top/bottom — `whitesprite.dbl` for selection).
A v2 ini entry keyed by `path = "GlowBox_Line.dbl"` cannot be deterministically
mapped to either `IDS_TOP` OR `IDS_BOTTOM`. Auto-migration on first match
will silently assign to whichever widget renders first — **the exact failure
mode the user is complaining about**.

**Revised migration**:
- v2 ini entries with paths → load into a `g_pending_legacy` queue (read-
  only).
- UI shows "Legacy slots needing assignment" section. Per-slot "Migrate"
  button lets user pick the target widget from the new tree.
- Auto-migration only when EXACTLY ONE current widget matches a path.

### Critical finding #5: instance_index over-engineered

`NewLoadSave.h` shows the engine authors prefer **distinct widget IDs per
slot** (`IDG_NEWLOADSAVE_SLOT1/2/3`, `IDS_NEWLOADSAVE_TOP1/2/3`) rather
than reusing one ID for multiple instances. **Drop `instance_index` from
the design** until Phase 0 finds an actual case where the engine reuses
one ID for simultaneous instances.

The real "repeated instance" case is **scrollable lists** (`mainmenu.h`
has `ID_ONLINEPROFILE_PROFILE1..PROFILE6`) — but that's already disambiguated
by distinct IDs.

### Critical finding #6: screen_name source unclear

`screen_push::current_top_name()` returns the top-of-stack screen — but this
may be wrong mid-frame if multiple screens render in the same pass, or if
a screen was just pushed/popped. The new identity needs `screen_name` from
the engine's OWN screen-type identity (e.g., the screen object's class
name, read via vtable).

Until that's RE'd, fall back to call_site (which IS stable) and treat
screen_name as best-effort metadata.

### Critical finding #7: estimate was optimistic

8-10 days → realistic 12-15 days. With the MVP path → 3-5 days. The
phases that blew estimates:
- Phase 0 RE if it requires deep call-site correlation (3-5 days, not 1-2).
- Phase 3 sprite_xform refactor (3-4 days, not 2 — heart-transplant on a
  load-bearing 1576-line file).
- Migration UX (extra ½ day under-estimated).

## Revised plan (post-audit)

### Phase 0 — Widget→sprite RE (autonomous, in progress)

Going on now while user sleeps. Static analysis only, no code deploy.
Goals:

1. Confirm widget-name strings appear in .sc binary (DONE per Plan agent).
2. Find the .sc loader function in IDA. Identify what offset on the
   runtime widget object stores the name string.
3. Cross-check by dumping SpriteEntry unknown fields at runtime
   (deferred — needs deploy to test).
4. Find the widget Render() method and the widget-this stack location at
   the sprite-push call.

Output: a clear path from `SpriteEntry*` → widget object → name string,
documented in this file with addresses and offsets.

### Phase 1 — MVP: call_site + widget-name auto-attribution

Once Phase 0 confirms either path:

1. Hook the sprite-push function (or wrap the existing `sprite_probe`
   detour at 0x4D23BF). Capture `_ReturnAddress()` per entry. Store as
   side-table `entry_ptr → call_site` for the current frame, cleared at
   top of each frame.
2. Add `call_site` (uint32) to `CompositeKey`. Component is wildcard
   (0x00000000) by default; concrete when extracted.
3. Resolve widget name by walking from sprite-push hook into widget
   `this->m_pcName` (offset from Phase 0). Side-table
   `call_site → widget_name`. Persistent (call_site is stable).
4. Auto-name slots from the side-table.
5. Persistence: ini key gains `call_site = 0x004C5A12` — stable across
   sessions, immune to heap churn.

### Phase 2 — Migration

v2 ini entries: load into `g_pending_legacy`. UI shows them under "Legacy
slots needing assignment". User clicks "Migrate" per slot, picks target
widget.

### Phase 3 — UI tree by widget name

Tree grouped by screen (from screen_push) → widget name (from call_site).
Familiar imgui look matches existing per-element node.

### Phase 4 — Backwards compat fallback

If a sprite has no widget context (cursor, particles), CompositeKey falls
back to v2 (state_key + uv_bucket + screen_context + bbox_quadrant).
Best-effort persistence. Acceptable scoping.

### Phase 5 — (Optional) Full WidgetIdentity refactor

Only if MVP proves insufficient. Drop CompositeKey entirely, use
`WidgetIdentity { screen_name, widget_name, call_site }` as primary.
Pure cleanup; user-visible behaviour unchanged from end-of-MVP state.

## Total: 3-5 days for MVP, +5-8 days optional for full refactor.
