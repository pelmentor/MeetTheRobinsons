# UI widget Phase 4 — failed attempts + open paths (2026-05-09 evening)

Status: **NOT SHIPPED.** Two attempts at the Sprite ctor hook (which
would close the IDS_*/IDG_* coverage gap) caused a 6-second freeze on
main-menu load with repeating laggy audio (= audio buffer underrun, CPU
starvation). Both attempts reverted. The user's original gradient
granularity complaint **remains unfixed**.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).
Reaffirmed 2026-05-09: "ALWAYS FOLLOW RULE №1 - THAT'S OUR NEW RULE №2".

## What we tried + why it failed

### Background: the goal

After Phase 1-3 shipped (engine-level widget identity capture for the 3
known callers of sub_4E9350: Btn_*, FrontEnd_*), we discovered the
remaining 11 callers don't fire during static GameSelectScreen state.
The user's actual gradient complaint involves 3 Sprite widgets that
share `GlowBox_Line.dbl`:
- `IDS_TOP` — top gradient bar
- `IDS_BOTTOM` — bottom gradient bar
- `IDS_HIGHLIGHT_TOP` (or `IDS_HIGHLIGHT_BACKGROUND` in WilburMainMenu) — selection overlay

Phase 4 goal: hook the Sprite class ctor at `sub_490830` so widget_name
is captured at construction-time (independent of which path renders
the widget later). Code-architect agent ranked this Approach 2 in its
audit, recommended after Approach 1 (widening the existing stack scan).

### Attempt 1 — Naked stub for sub_490830 (ctor)

Hand-written assembly: `pushad`/dispatch_pre/`popad` + `pop` engine
caller_ret to static slot + `call` trampoline + `push` saved
caller_ret + POST capture + `ret`.

**Symptom**: harness pass returned in 17 sec (vs 5.8 sec baseline),
hits-counter showed 0 ctor calls captured. User reported: "main menu
loads it freezes for 6 seconds with a repeating laggy sound".

**Root cause**: sub_490830 has SEH prologue at function entry:
```asm
0x490830: push -1                          ; SEH "marker"
0x490832: push offset SEH_490830           ; SEH handler
0x490837: mov  eax, fs:0
0x49083d: push eax                         ; saved old fs:[0]
0x49083e: mov  fs:0, esp                   ; INSTALL SEH frame
```

The naked stub's `pop dword ptr [g_caller_ret_save]` rewrites ESP
between the engine's `call sub_490830` instruction and the original
prologue. By the time `mov fs:0, esp` runs inside the trampoline, the
SEH-frame's `prev` pointer is offset wrong. Any subsequent exception
walk through the linked list finds dangling `prev` pointers and either
unwinds way past the intended frame or fails silently — both cause
slow-path exception handling that the audio thread can't tolerate.

### Attempt 2 — __fastcall detour for sub_490830

Replaced the naked stub with a clean MSVC `__fastcall` detour:
```cpp
void* __fastcall hk_sprite_ctor(void* this_ptr, void* /*edx*/,
                                int a1, int a2, int a3) {
    void* result = g_orig_sprite_ctor(this_ptr, nullptr, a1, a2, a3);
    sprite_ctor_dispatch_post(this_ptr);
    return result;
}
```

`__fastcall(this[ECX], dummy[EDX], a1[esp+0], a2[esp+4], a3[esp+8])` is
binary-compatible with `__thiscall(this, a1, a2, a3)` — same ECX-this,
same args layout, same callee-cleanup-args via `ret 0Ch`. MSVC handles
prologue/epilogue cleanly; SEH frame integrity preserved (in theory).

**Symptom**: Same 6s freeze. Half-second main-menu visibility before
the harness eager-pass killed Wilbur. User: "Your attempts freeze on
main screen and then menu appears for half a second and you kill the
game."

**Root cause hypothesis** (we couldn't fully verify because reverting
was urgent — user had observed this twice in a row): MinHook's normal
trampoline is *also* incompatible with sub_490830's SEH prologue. The
trampoline copies the first 5+ bytes (`push -1; push @handler;` —
that's 7 bytes covered by 2 push instructions). When called, it pushes
those 2 dwords then jumps to byte 7 (mid-`mov eax, fs:0`). But
MinHook's trampoline is at a SEPARATE memory location — when
`mov fs:0, esp` later runs, ESP points into the trampoline's stack
context, not the engine's. The SEH frame's `next` pointer (= old
`fs:[0]`) becomes a pointer to data in the trampoline's stack that
gets overwritten on the next call → linked-list corruption.

This is **fundamental to ANY hook on sub_490830** that uses MinHook's
prologue-copy mechanism. We need a different upstream point.

### Why "hits = 0" was misleading

The hits counter showed 0 in BOTH attempts. We thought this meant the
ctor wasn't firing. Actually: the counter increments inside
`sprite_ctor_dispatch_post`, which runs AFTER the trampoline returns.
If the trampoline corrupts the stack mid-execution (because of SEH
issues above), it never returns cleanly to our dispatcher — control
gets handed off elsewhere via SEH unwind, the dispatcher never runs,
counter stays 0. The freeze + 0-hits combo is consistent with
"trampoline runs forever inside SEH unwind retries, never reaches our
POST code".

## What survived in code (kept after revert)

- **`mtr::widget_probe::widget_map_insert`** + the map storage (1024
  entries, `widget_ptr → name`). Currently empty — no inserter wired in.
- **`mtr::widget_probe::widget_map_lookup`** — used by
  `prod_capture_pre` as a fast path before stack/register scan.
  Returns nullptr (map empty) → falls through to existing scan.
- **`sprite_ctor_dispatch_post` + `sprite_ctor_dispatch_post_impl`**
  bridge functions at file scope. Validate string at +0x130 then
  insert into map. Currently dead (no caller).
- The naked stub `hk_sprite_ctor_naked` was deleted; the __fastcall
  variant `hk_sprite_ctor` was deleted. Only the dispatch helpers
  remain. Re-wiring is straightforward when a new hook target is found.
- New `hold-at-menu` scenario in `test_harness.cpp` — boots to menu,
  never auto-kills. Lets user drive testing manually.

## Phase 4 attempt 3 — open paths (RULE №1-compliant)

### Path A — Hook the widget factory `sub_4916E0`

Per IDA xrefs, `sub_490830` (the Sprite ctor) has ONE caller:
`sub_4916E0` at `0x491747`. The factory dispatches to type-specific
ctors via `switch (a3)`:
- cases 0-4 → `sub_490830` (Sprite, 620 bytes)
- case 5 → `sub_490FB0`
- case 6 → `sub_491A30`
- case 7 → `sub_4927A0`
- case 8 → `sub_490AA0`
- case 9 → `sub_491B30`
- case 10 → `sub_491D00`

If the factory itself doesn't have an SEH prologue, hooking IT works
for ALL widget types in one go. After it returns, the new widget
pointer is stored in `*(this+12)` and (per the decompile)
`sub_609190(this+12, v7)` registers it in a list.

**Verification needed before coding**: disassemble sub_4916E0, confirm
no `mov fs:0, esp` in prologue. If clean, the __fastcall detour pattern
should work. Estimated effort: 2-3 hours.

### Path B — Hook the .sc loader

The `.sc` files (`Game/data/screens/*.sc`) are parsed at screen-load
time. The parser instantiates widgets one by one. Hooking the parser
captures (widget_name from .sc text, widget_pointer from ctor return)
in pairs naturally — no need to read m_pcName at all.

**Unknowns**: which function is the .sc parser? Need to RE the
screen-loading path. The screen ctor (e.g., `ScreenWilburMainMenu_ctor`
at `0x45BFF0` per Phase 0) calls `sub_5317E7` (base screen ctor) — at
some point that loads the .sc file. Probably traceable via xrefs to
the .sc-file-open syscall (CreateFile / fopen).

Estimated effort: 4-8 hours (deeper RE).

### Path C — Hook a vtable slot, not a function

Sprite vtable is at `0x6B8E98`. If a vtable slot has a "post-construct
init" or "set name" virtual method, we can hook that AT THE VTABLE
LEVEL — patching the slot to point to our function. Then no SEH issue
(we're not modifying any function's bytes; we're just changing a
function pointer in data).

**Unknowns**: which vtable slot, if any, is called post-name-set
during construction. Phase 0 RE found `vtable[1]` is `GetType` (returns
"Sprite"). Other slots not yet mapped. Could probe at runtime.

Estimated effort: 3-6 hours.

### Recommended order for next session

1. **Path A first** (factory hook) — most likely to succeed quickly.
   The factory function is one level up from the SEH-using ctor;
   probably has a normal prologue. Same `__fastcall(this[ECX], dummy,
   args)` pattern that already works for non-SEH targets.
2. **Path B as fallback** — if the factory hook also runs into
   SEH/freeze issues OR doesn't fire for the IDS_*/IDG_* widgets
   we care about.
3. **Path C as last resort** — only if A+B both fail and we genuinely
   need vtable-level patching.

## Files relevant to next session

- `src/mtr-asi/src/widget_probe.cpp` — widget_map + lookup + dispatch
  helpers stay; ctor hook is removed but the bridge functions are
  ready for re-wire to a new hook target.
- `src/mtr-asi/include/mtr/widget_probe.h` — public API (no changes
  needed for Phase 4 attempt 3).
- `src/mtr-asi/src/sprite_xform.cpp` — CompositeKey + Slot.widget_name
  + pending_by_widget_name plumbing all in place. Once a hook target
  populates the map, this layer just works.
- `src/mtr-asi/src/test_harness.cpp` — new `hold-at-menu` scenario
  (no auto-kill) for manual testing.
- `Game/mtr-asi-ui.ini` — confirmed working ini schema with new
  `x_<i>_widget_name` field. Empty for sprites without captured name.

## Files NOT to recreate

- `widget_probe`'s naked stub for sub_490830 — proven harmful, in git
  history. Don't re-attempt without an SEH-safe hooking scheme.
- The __fastcall detour for sub_490830 — same problem. Likely the
  function itself can't be hooked at all by MinHook's prologue-copy
  approach.

## Build artifact

`Game/mtr-asi.asi` 601088 bytes (2026-05-09 12:26). Phase 1-3 active,
Phase 4 disabled (clean state per user's last manual test).
