# UI widget system — Phase 0 RE findings (2026-05-09)

Date: 2026-05-09 evening (autonomous Phase 0 while user sleeps)
Status: **Static analysis complete; runtime probe deferred.** Sufficient to
inform MVP plan; the remaining `m_pcName` offset hunt requires runtime
deploy + log.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## Goal

Find the engine path from a `SpriteEntry` (sprite-batcher list node at
`0x7271E8`) back to a stable widget identity, so we can stop using
heap-pointer state_keys + bbox quadrants and start using engine-level
string IDs.

## Confirmed (static analysis, no runtime needed)

### 1. Widget identifiers exist as strings in `.sc` binaries

`Game/data/screens/WilburMainMenu.sc` contains the widget identifiers as
plain ASCII strings (verified by reading the file directly):

```
Sprite WilburMainMenu       <-- screen root
Sprite IDG_WILBUR           <-- group container
Sprite IDS_WILBUR_HAND       wilburBottom.dbl
Sprite IDS_WILBUR_BODY       wilburTop.dbl
Sprite IDG_MENU
Text IDT_MINIGAMES           ...fiesta FrontEnd_MiniGames
Text IDT_CHEATS              ...fiesta FrontEnd_Cheats
Text IDT_BEGIN_GAME          ...fiesta FrontEnd_NewGame
Sprite IDG_HIGHLIGHT
Sprite IDS_BOTTOM_LEFT       GlowBox_Corner.dbl
Sprite IDS_BOTTOM            GlowBox_Line.dbl       <-- bottom gradient
Sprite IDS_TOP               GlowBox_Line.dbl       <-- top gradient
Sprite IDS_HIGHLIGHT_BACKGROUND  whitesprite.dbl    <-- selection gradient
...
```

Each widget definition is `<class-token> <instance-name>` followed by an
asset reference (for sprites) and 18× `ProjectItem` markers (probably a
property bag or class-hierarchy tag).

The `.h` files (`WilburMainMenu.h`) define `#define IDS_TOP (270)` etc.
**These numeric IDs are preprocessor constants for the engine's build-
time C++ source — they are not stored at runtime.** The runtime stores
the strings, not the numbers. (Confirmed via the audit by independent
reading of the binary).

### 2. Screen class system (already mapped per `project_screen_system.md`)

- Each screen has a hardcoded ctor (e.g., `ScreenWilburMainMenu_ctor` @
  `0x45BFF0`). Allocates 548 bytes, calls base ctor `sub_5317E7`, sets
  vtable to `0x6B0D70`.
- `ScreenWilburMainMenu` vtable @ `0x6B0D70`:
  - `[1]` (offset 4) = `sub_45BFE0` returns `"ScreenWilburMainMenu"`
    string at `0x6B2C50` — the screen class identifier.
  - (Note: `screen_register_factory` says `vtable+20 = GetName`; the
    actual GetName for THIS class is at `vtable+4`. The factory might
    use a different vtable layer or the IDA decompile is misreading
    the offset. **Empirical** check: `sub_45BFE0` is the GetName.)
- 56 screens registered at startup via `screen_register_factory`
  (`0x6049A0`) called from `0x45DF40..0x45E1A0`. Each screen has its
  own ctor.

### 3. Sprite widget class

- **Sprite vtable @ `0x6B8E98`.** Identified by:
  - `sub_48FFE0` returns `"Sprite"` string at `0x6B8E7C` — the widget
    type identifier (the `<class-token>` in `Sprite IDS_TOP`).
  - This GetType thunk sits at `vtable[1]` (offset 4) of the Sprite
    vtable.
- **Sprite ctor @ `sub_490830`.** Allocates a 260-byte object (vtable
  thunk `sub_48FD50` returns `260`), calls base class ctor
  `sub_48FB70(this, a2, a3, a4)`, sets vtable, initializes a sub-object
  at `this+90` (= `this+0x168`).
- **`sub_48FB70` is a SecuROM-style stolen-byte IAT thunk** —
  `jmp [g_securom_thunk_table_base + 0x2F0FA]`. Per
  `feedback_securom_terminology_2026-05-08.md` this is NOT live SecuROM,
  just stolen-byte indirection; the destination is reachable at runtime
  but not from static IDA without table contents.

### 4. Sprite-push function `sub_4E9350` (the engine's `SubmitSprite`)

This is the central function that allocates a `SpriteEntry` and pushes
it to the sprite-batcher list at `0x7271E8`.

Signature (cdecl, 8 args):

```c
int sub_4E9350(
    unsigned int a1,    // state_key (texture pointer)
    int16 a2,           // → SpriteEntry+0x14 (uint16)
    int16 a3,           // → SpriteEntry+0x16 (sort_key)
    unsigned int a4,    // flags (& 0x10 / 0x200 / 0x8 / 0x1 / 0x2 / 0x4 / 0x20)
    void* a5,           // 0x30 bytes positions (4 vec3 — inline_positions)
    void* a6,           // 0x20 bytes uvs (4 vec2 — inline_uvs)
    int a7,             // colors block (16 bytes — inline_colors)
    int a8              // → SpriteEntry+0x20 (8 bytes)
);
```

Verified field offsets match `sprite_xform.cpp::SpriteEntry`:
- `a1` (state_key)  → entry+0x10
- `a2`              → entry+0x14
- `a3` (sort_key)   → entry+0x16
- `a4` (flags)      → entry+0x08
- `a5` (positions)  → entry+0x28 (memcpy 0x30 bytes)
- `a6` (uvs)        → entry+0x5C (memcpy 0x20 bytes)
- `a7` (colors)     → entry+0x80
- `a8` (misc)       → entry+0x20

This is the **SINGLE choke-point** for sprite submission. Hooking this
function captures every sprite drawn by the engine.

### 5. Caller landscape

`xrefs_to(sub_4E9350)` returns 14 call sites in 9+ functions:

| Caller | Function | Notes |
|--------|----------|-------|
| `0x4D253C` | `sub_4D2420` | render_sprite_batcher area |
| `0x5D2A38`, `0x5D2A59` | `sub_5D28A0` | unknown widget class (2 calls) |
| `0x5F2786` | `sub_5F25E0` | unknown widget class |
| `0x6025D4`, `0x6029C5` | unnamed | 2 distinct call sites |
| `0x60D133` | unnamed | |
| `0x60E17D` | `sub_60E110` | |
| `0x65EE2A` | `sub_65ED70` | |
| `0x6728BD` | `sub_6727E0` | |
| `0x6737CE` | `halo_set_world_pos` | already RE'd halo system |
| `0x673A63` | `sub_673940` | |

Each caller is a different widget class's draw method (sprite, text,
halo, etc.). Multiple calls within the same caller mean the widget
draws several sprites per call (e.g., a button draws background + label).

## Phase 0.5 RUNTIME PROBE — landed 2026-05-09 night

**Status: working, with partial coverage. Driver:** new `widget-probe`
scenario in `tools/run-test.ps1` (autonomous test loop). The probe MinHooks
`sub_4E9350`, scans the caller's stack frame for pointer-typed dwords, and
for each candidate scans both inline char[] and char* fields (within
0x200 bytes) for ASCII strings. Output: `Game/mtr-asi-widget-probe.log`.

### Confirmed runtime offsets

For 2 of the 14 callers — `ret=0x006025D9` and `ret=0x006029CA` (both no-
function shrapnel inside a big render-text caller, ~`0x6024B0`+) — the
widget `this` pointer is at `[esp+40]` in the caller's frame at the moment
of `call sub_4E9350` (= `ret_slot[10]` in our hook).

On the widget object reached that way, `m_pcName` is a **`char*` field at
offset `+0x130` (= 304 decimal)**. Adjacent fields:
- `+0x104` (260 dec) → `char*` to visible label, e.g. `"ACCEPT (ENTER)"`
- `+0x130` (304 dec) → `char*` to widget identifier, e.g. `"Btn_Legend_Accept"`
- `+0x134` (308 dec) → `char*` to localized label (often = the +260 string)

For the 3rd captured caller (`ret=0x0060D138`) the captured pointers were
to global static-data structs (process command line, GUI scroll-bar string
pool), not widget objects — the caller is likely an event-bus dispatcher,
not a widget render.

### Captured widget identifiers from a single 120-frame run at GameSelectScreen

```
Btn_Legend_Accept            (ENTER button hint)
Btn_Quit_Game                (visible only when "Quit" highlighted)
FrontEnd_GameSelect          (text label)
FrontEnd_Load
FrontEnd_New
FrontEnd_SelectAnOption
```

Visible-label strings simultaneously captured (text content of those
widgets):
```
ACCEPT (ENTER)
Load a Saved Game
MAIN MENU
NEW GAME
Options
QUIT GAME (ESC)
Quit Game
Select an Option
```

### Coverage gap — Sprite (IDS_*) widgets

The probe did **NOT** capture any `IDS_*` / `IDG_*` widget identifiers on
GameSelectScreen, despite `WilburGameSelect.sc` listing 5 of them
(`IDS_HIGHLIGHT_TOP`, `IDS_HIGHLIGHT_BOTTOM_LEFT`, etc) plus 5 `IDG_*`
groups. Reason: the callers that submit Sprite widgets (presumably some
of the 11 not-yet-fired entries in our 14-caller list) keep their `this`
in callee-saved REGISTERS (ESI/EDI/EBX) at the moment of `call
sub_4E9350`, NOT on the stack. Our probe only scans the stack.

This matters: the user's primary complaint (top/bottom/selection
gradients in WilburMainMenu) targets `IDS_TOP` / `IDS_BOTTOM` /
`IDS_HIGHLIGHT_BACKGROUND` — all Sprite widgets, all in the unreachable
class.

### Path A LANDED 2026-05-09 night

Naked-stub register capture shipped per RULE №1 (user said "abide by
rule №1" when shown the path-A/B/C choice).

- `hk_SubmitSprite_naked` in `widget_probe.cpp` — inline asm:
  ```asm
  pushad
  push esp
  call widget_probe_dispatch
  add  esp, 4
  popad
  jmp  dword ptr [g_orig_SubmitSprite_trampoline]
  ```
- `widget_probe_dispatch` reads the pushad block: walks caller's stack
  frame above the args (path 1, was already there) AND scans 4 callee-
  saved registers (EBX/EBP/ESI/EDI) as additional `this` candidates
  (path 2, new). Caller-saved EAX/ECX/EDX deliberately skipped — those
  hold transient junk at the moment of `call sub_4E9350`.
- Heap range tightened to `0x00800000..0x10000000` to filter junk
  pointers fast (otherwise per-call VirtualQuery overhead hung the game).

**Result on first run:** still 3 of 14 callers firing during the 120-
frame window at GameSelectScreen. The IDS_*/IDG_* widgets in
WilburGameSelect.sc (highlight gradients, group containers) DON'T submit
through any of the captured paths. Two reasonable hypotheses:

1. They submit via callers we haven't observed (the other 11 of 14 don't
   fire during this idle main-menu state — possibly only when user
   navigates / a state changes).
2. They use a different submit path entirely (a render bypass for
   stateless gradient sprites).

Coverage will grow as the test harness drives the game into more screens
(scenarios `gameplay-baseline`, `pause-menu`, `digdug-minigame` etc).
Phase 1 ships now with the captured pattern; missing widgets fall back
to state_key.

### Probe code references

- Public API: `mtr::widget_probe::install() / arm(budget) / disarm()`
- Implementation: [src/mtr-asi/src/widget_probe.cpp](../../src/mtr-asi/src/widget_probe.cpp)
- Header: [src/mtr-asi/include/mtr/widget_probe.h](../../src/mtr-asi/include/mtr/widget_probe.h)
- Driver scenario: `tick_widget_probe` in
  [src/mtr-asi/src/test_harness.cpp](../../src/mtr-asi/src/test_harness.cpp)
- How to run: `pwsh tools/run-test.ps1 -Scenario widget-probe -Redeploy`

### Original RE artefact (pre-probe)

The remaining gap is `widget_object → widget_instance_name` mapping. We
KNOW the strings exist somewhere on the runtime widget object (because
the .sc loader puts them there during ctor), but we don't yet know the
field offset. Static analysis dead-ends at the `sub_48FB70` IAT thunk
that jumps to the base-class ctor (where the name field is initialized).

**Resolution path (Phase 0.5, requires runtime deploy)**:

1. Add a one-shot probe to mtr-asi: hook `sub_4E9350` PRE.
2. On first call after main menu becomes visible:
   - Walk the stack at function entry to recover the caller's `this`
     pointer. In `__thiscall` the `this` is in ECX before the call;
     pushed to stack via `push ecx` typically. Read `[ESP+xx]` for the
     caller's saved `this`.
   - Dump 0x200 bytes of `*this` to log.
   - Look for ASCII strings matching widget identifiers (`IDS_TOP`,
     `IDS_BOTTOM`, etc.) — find the offset.
3. Rerun for several widgets to verify the offset is consistent across
   widget instances.
4. Document the offset and proceed to Phase 1.

This mirrors the approach we used successfully for the player-entity
layout dump (2026-05-09 morning).

## Phase 1 LANDED 2026-05-09 night — production-mode hook + side-table

After Path A's naked-stub register capture confirmed the technique
works, the same hook was extended to a PRE+POST pattern that captures
the SpriteEntry* return value (so the side-table can pair it with the
widget_name). The naked stub now reads:

```asm
hk_SubmitSprite_naked:
    ; PRE: capture widget_name into g_pending_widget_name
    pushad
    push esp
    call widget_probe_dispatch_pre
    add  esp, 4
    popad

    ; Forward to trampoline. CRITICAL: pop our caller_ret to a STATIC
    ; slot (not a register) — the trampoline is __cdecl and clobbers
    ; caller-saved registers freely. Initial attempt with `pop ecx;
    ; ...; push ecx` crashed with 0xC0000005 because trampoline
    ; clobbered ECX and the post-call push pushed garbage as our
    ; caller_ret -> bad jump on `ret`.
    pop  dword ptr [g_caller_ret_save]
    call dword ptr [g_orig_SubmitSprite_trampoline]
    push dword ptr [g_caller_ret_save]

    ; POST: pair eax (= SpriteEntry*) with g_pending_widget_name
    push eax              ; save return value
    push eax              ; pass as cdecl arg
    call widget_probe_dispatch_post
    add  esp, 4
    pop  eax              ; restore return value

    ret
```

### Production capture path (always-on, low-overhead)

`prod_capture_pre()`:
1. Range-filter candidates to heap (`0x00800000..0x10000000`) — excludes
   module data + spurious matches.
2. Try callee-saved registers first: EBX, EBP, ESI, EDI. (Caller-saved
   EAX/ECX/EDX deliberately skipped — those hold transient junk at the
   moment of `call sub_4E9350`.)
3. Fallback: scan caller's stack frame at offsets `[+40..+64]` (where
   Btn/Text widgets keep `this`).
4. For each candidate, fast-path read `*(this+0x130)` as char* (the
   confirmed offset). Validate it's a printable ASCII string.
5. Stash in `g_pending_widget_name`.

`prod_capture_post(SpriteEntry*)`:
1. If pending name + valid SpriteEntry, insert into fixed-size side-
   table (1024 entries, open-address linear scan).
2. Side-table cleared at the end of each `wrapper_render_sprite_batcher`
   call (= per render frame).

### Public API (in `mtr/widget_probe.h`)

```cpp
const char*   widget_name_for_entry(void* entry_ptr);
void          clear_frame_table();
unsigned int  frame_table_size();
void          debug_dump_frame_table();
```

### What's verified working

- `boot-to-main-menu` scenario passes in 5.8s end-to-end with the new
  PRE/POST hook (vs 3.7s with the simpler jmp-trampoline pre-Phase-1).
  The ~2s overhead is the per-call dispatch cost; acceptable.
- `widget-probe` scenario passes with 271 verbose findings logged AND
  the side-table populating in parallel (proves both paths run).
- `verify-main-menu-visible` scenario added to hold at main menu for
  ~8s + screenshot every 1s. Confirmed the engine pushes
  GameSelectScreen onto the screen stack DURING the splash transition
  (before splash visually fades), so widget capture during the
  splash period IS capturing real main-menu widgets.

### Two infrastructure bugs surfaced (not blocking UI work)

1. **DI keyboard injection flaky on cold launch.** The 0x1C-RETURN
   inject every 60 frames sometimes fails to dismiss the title splash —
   user has to manually press a key. Worked in earlier sessions
   (autonomous loop overnight 2026-05-09). Possible cause: title-screen
   input poll timing race on first launch.
2. **`screen_push::current_top_name()` reports `GameSelectScreen`
   before splash visually clears.** The engine pushes the screen onto
   its stack early; the splash is just an overlay during fade. Means
   `boot-to-main-menu` "passes" before any human-visible main-menu
   transition. Fix: require N consecutive stable frames + post-fade
   marker (e.g. cursor visible) before accepting pass. Not blocking;
   the captured widget data is still real.

## Implications for the MVP plan

The audit already recommended a 3-5 day MVP that adds `call_site` to
CompositeKey. Phase 0 findings either confirm or refine that:

### Refinement: call_site alone is not sufficient for atlas-sharing widgets

Looking at `sub_5D28A0` which has TWO calls to `sub_4E9350` at
`0x5D2A38` and `0x5D2A59` — that's ONE widget class drawing TWO sprites
per Render. If `IDS_TOP` and `IDS_BOTTOM` are both `Sprite` widgets
drawn by the same Sprite::Render code path, **they will have IDENTICAL
call_site values**. Call-site disambiguation fails for them.

This is the "same class, different instances" problem the audit warned
about. The selection gradient (`IDS_HIGHLIGHT_BACKGROUND`) is also a
`Sprite` — so all three of the user's complained-about gradients likely
share the same call_site.

**MVP must include widget-instance identity, not just call_site.**

### Recommended MVP refinement (revised)

1. **Phase 0.5** (½ day, requires deploy): runtime probe at sub_4E9350
   to find m_pcName offset on Sprite widget objects.
2. **Phase 1** (1 day): hook sub_4E9350. Walk stack to recover caller
   `this`. Read m_pcName from the widget. Capture (state_key, m_pcName,
   _ReturnAddress) per entry.
3. **Phase 2** (1-2 days): add `widget_name` (string, ~40 bytes) to
   CompositeKey/Slot. Replace `bbox_quadrant` (unstable for dynamic
   elements). Persistence by `(screen_name, widget_name)`.
4. **Phase 3** (1 day): screen_name from `screen_push::current_top_name()`
   (already tracked) — best-effort, with fallback to call_site.
5. **Phase 4** (½ day): UI tree grouped by screen → widget_name.
6. **Phase 5** (½ day): migration of v2 entries — read-only legacy with
   "Migrate" button, NOT auto-migration.

**Total: 4-5 days** — achievable for what the user actually needs.

## Files / hooks to write

- New hook: `entity_widget_probe.cpp` (or extend `state_key_probe.cpp`).
  - MinHook on `sub_4E9350` PRE.
  - One-shot stack-walk + memory dump probe.
  - Steady-state: capture `(SpriteEntry*, widget_name)` per call,
    populate side-table cleared at top of each frame.
- Modify `sprite_xform.cpp`:
  - Add `widget_name[40]` to `CompositeKey` (or replace bbox_quadrant).
  - Read from side-table during `process_list`.
  - Update `key_matches_pattern` / `key_specificity` accordingly.
- Modify `ui_aspect_rules.cpp`:
  - Save/load `widget_name` field in ini schema.
- Modify `menu.cpp`:
  - Per-element tree grouped by screen → widget.

## Risks / unknowns remaining

1. **m_pcName offset stability**: needs runtime confirmation it's at the
   same offset for ALL widget classes, not just Sprite. Text, Group,
   etc. may inherit from a common base with a different layout.
2. **Stack-walk fragility**: the caller's `this` is at `[ESP+offset]`
   where offset depends on the caller's prologue. Most callers will be
   `__thiscall` with `push ecx; mov esi, ecx` (= `[ESP+0]` after push).
   But inlined/optimised callers may differ. **Mitigation**: read
   ECX directly via inline asm at hook entry — most reliable.
3. **Performance**: hook in hot path adds per-sprite overhead. Side-
   table lookup must be O(1). Capacity: ~250 entries/frame × 4 bytes
   key × 40 bytes value = 10 KB hash table — trivial.
4. **Cross-class identity collisions**: the same widget name (e.g.,
   `IDT_BACK`) appears in many screens with different meanings. Always
   key by `(screen_name, widget_name)` not `widget_name` alone.

## Reference data captured during this RE

- WilburMainMenu screen vtable @ `0x6B0D70`
- WilburMainMenu screen ctor @ `0x45BFF0` (allocates 548 bytes)
- Sprite widget vtable @ `0x6B8E98`
- Sprite widget ctor @ `0x490830` (260-byte instance)
- Sprite GetType thunk @ `0x48FFE0` returns `"Sprite"` @ `0x6B8E7C`
- Sprite-push function: `sub_4E9350`
- Sprite-list head: `0x7271E8`
- Screen registry: `g_screen_registry` @ `0x744A80`
- All widget IDs for WilburMainMenu screen: `Game/data/screens/WilburMainMenu.h`
