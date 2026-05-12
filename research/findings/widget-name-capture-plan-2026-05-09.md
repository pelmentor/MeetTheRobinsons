# Widget-name capture plan v3 — proper cross-session/cross-screen widget identity

**Date:** 2026-05-09
**Revision:** v3 (addresses three rounds of audit; ready for execution)
**Governing rule:** RULE №1 — no crutches, no quick fixes. Pick the proper root-cause solution. **Weeks/months are OK; a 72-hour focused RE session is totally acceptable.** The time caps in Phase 0 below are *guidelines for re-evaluating direction*, not wall-clock budgets to ship at any cost. If a phase hits its cap and the proper-fix path is still tractable, extending is correct; cutting corners to fit the cap is RULE-№1-violating.

---

## TL;DR

The user wants granular UI control: scale individual gradient widgets (`IDS_TOP`, `IDS_BOTTOM`, `IDS_HIGHLIGHT_BACKGROUND`) without affecting siblings that share their texture. Today's matcher uses geometric heuristics (`state_key + uv_bucket + screen_context + bbox_quadrant + sort_key`) which are fragile across screen reloads. The proper fix is to capture the **engine's own widget name string** at construction and bind it to widget pointers. This document plans that capture work end-to-end.

**Critical insight from audit:** today's already-shipped `sort_key` discriminator may already solve the user's symptoms. **Phase −1 verifies this with measurable criteria before any new RE work begins.**

**Bundled prerequisite:** an existing bug in `widget_map_insert` (pointer-keyed dedup, wrong on screen reload) must be fixed first as a standalone commit. This is needed regardless of whether the Phase 0+ work proceeds.

---

## Why this is needed

What fails today:

- `state_key` is the texture handle. Stable while texture is loaded; reset when the engine reallocates the texture object on screen transitions. Multiple gradient widgets share one state_key.
- `bbox_quadrant` is 3×3 — too coarse. Cursor-following overlays cross bins between frames; static gradients near a bin boundary collide with sliding ones.
- `uv_bucket + screen_context + sort_key` may discriminate static gradients (CSV diagnostic earlier today: TOP/BOTTOM `sort=301`, HIGHLIGHT `sort=402`); cross-screen-reload behavior is **unverified** — Phase −1 closes that gap.
- A naive runtime path-based rebind we tried this morning **mismatched specialized slots** when the user navigated through screens that share generic textures (`whitesprite.tga`). Reverted same-day. Don't re-attempt that direction.

The engine's own widget-name string (`m_pcName`, confirmed at `+0x130` for `Btn_*`/`FrontEnd_*` widgets, **unverified for Sprite**) is the authoritative identity if it can be captured. It is set in `Game/data/screens/*.sc` files at parse time, lives in long-lived engine memory, and is unique per widget per screen.

---

## Why the trivial approach is blocked

`widget_probe` already hooks `sub_4E9350` (SubmitSprite, clean prologue) and reads `m_pcName` opportunistically by scanning the caller's stack frame and callee-saved registers. Works for 3 of 14 callers (`Btn_*`, `FrontEnd_*`). Does NOT work for the gradient render path — verbose probe found zero `IDS_*` strings on `GameSelectScreen` despite 224 raw findings.

The natural alternative — hooking the widget constructor — is blocked:

- `sub_490830` (Sprite ctor): SEH prologue (`mov fs:0, esp`). MinHook prologue-copy trampoline corrupts SEH frame chaining → 6-second freeze + audio buffer underrun. Verified twice. **Documented in [`research/findings/ui-widget-phase4-failed-attempts-2026-05-09.md`](ui-widget-phase4-failed-attempts-2026-05-09.md).** That doc is the canonical Phase 4 post-mortem.
- `sub_4916E0` (widget factory, parent of Sprite ctor): SAME SEH prologue. Verified by IDA disasm today.
- `sub_522E00` (.sc-related screen helper): SAME SEH prologue.

**Hooking SEH-prologued functions via MinHook prologue-copy is fundamentally unsafe.** A POST-return wrapper on those functions MAY be safe (the SEH frame is fully unwound before our handler runs); each candidate must be verified individually.

---

## Path-name conventions (fixed once for all docs)

To resolve the audit's "naming collision" finding (audit A4):

| New plan label | Phase 4 post-mortem label | Description |
|---|---|---|
| **0A** = Phase 1A | "Path A" in post-mortem | Factory hook on `sub_4916E0` — POST-return variant only (the post-mortem's direct-prologue attempt is dead) |
| **0B** = Phase 1B | "Path C" in post-mortem | Sprite vtable slot patch (data, not function bytes — no SEH issue) |
| **0C** = Phase 1C | "Path B" in post-mortem | `.sc` parser CALL-site patch |

The post-mortem ordered them A→C→B (factory → vtable → parser). v3 reorders them by **expected-value** based on Phase 0A's open question (see N3 fix in **Phase 0A** below): if `sub_4916E0`'s 3 known callers are all non-Sprite types, Phase 0A may evaluate to "factory not the path" within minutes and Phase 0B becomes the first real candidate.

---

## Phase −1 — Pre-flight verification (HARD GATE for all later work)

**Audit basis (architecture A1, B2, B3):** v3 cannot start Phase 0 until Phase −1 outputs a written outcome. Phase −1 also tests `add_pending_by_widget_name` (audit B3 — previously assumed to work, never tested).

### Setup (5 min)

- Latest deployed build (`Game/mtr-asi.asi` ≥ 605696 bytes; if older, redeploy the build at `src/mtr-asi/build/Release/mtr-asi.asi`).
- User launches via `Launcher.exe`. F2 to open the menu.

### Test sequence (with measurable criteria — audit B2)

| # | Action | PASS criterion | FAIL criterion |
|---|---|---|---|
| **−1.1** | Walk to GameSelectScreen | Picture → "Per-element control" tree opens | Crash, freeze > 2s, or no row appears |
| **−1.2** | Find `0x0FA091B0` row → Specialize 3× | 3 separate rows appear, each tagged `[uv=... s=... q=... sort=...]` with different combinations | Tags missing or all 3 tagged identically |
| **−1.3** | Note `sort_key` per row from the tag | Distinct values — at least one is `301` and one is `402` | All same `sort_key` |
| **−1.4** | Scale TOP slot's `sx` to 1.30, observe 5 seconds | TOP gradient bar visibly stretched on screen; BOTTOM and HIGHLIGHT visually unchanged compared to a screenshot taken before the scale (pixel-eyeball) | BOTTOM or HIGHLIGHT visibly stretched/moved |
| **−1.5** | Scale BOTTOM slot's `sy` to 1.30 | BOTTOM bar visibly taller; TOP and HIGHLIGHT unchanged | TOP or HIGHLIGHT visibly affected |
| **−1.6** | ESC out of GameSelectScreen, return | All 3 specialized rows still in the list, still tagged | Rows missing OR rows present but `[*]` wildcard |
| **−1.7** | Verify the slots STILL drive only their widget | Scaling each row affects only its widget per the −1.4/−1.5 criterion | Wildcards-everything regression |
| **−1.8** | Quit game, re-launch, walk back to GameSelectScreen | Slots load from ini AND visibly bind to live sprites — no manual re-Specialize needed (this is `add_pending_by_widget_name` reattach test) | Slots present in ini but not driving anything (= reattach broken) |

### Decision tree

- **All 8 pass** → today's `sort_key` build solves the user's symptom AND the cross-session reattach works. **Stop.** Reframe the user's "garbage on screen change" as UX confusion (row reorder, scroll loss after Specialize). Address the UX. **Phase 0+ does not begin.** Document closed.
- **−1.4 / −1.5 fail (sort_key not separating cleanly)** → Phase 0 begins. Widget-name capture is needed.
- **−1.6 / −1.7 fail (cross-screen state lost despite distinct sort_key)** → **persistence path is broken**. Triage path: read `mtr-asi.log`, check whether slots got evicted, or `widget_map_insert` got confused, or some pattern field is being clobbered. Fix the persistence bug surgically (estimated 2-4 hrs). Phase 0+ does NOT begin unless persistence fix proves insufficient.
- **−1.8 fails alone** → `add_pending_by_widget_name` reattach is broken. Fix the loader path in `ui_aspect_rules.cpp` (estimated 1-2 hrs). Phase 0+ does NOT begin.

### Acceptance for Phase −1

A new section in this document titled **"Phase −1 outcome"** containing:
- Date + build number (`Game/mtr-asi.asi` size in bytes)
- Outcome of each of the 8 tests (P/F)
- One-paragraph summary of which decision branch we're on
- If branching to "fix persistence/reattach", a hyperlink to the fix's code change

Without that section, **Phase 0 does not begin**.

---

## Pre-Phase-1 standalone bug fix (lands FIRST regardless of Phase 0)

**Audit basis (engineering E2, architecture A6, R7, R8).**

`widget_probe.cpp:361-373` `widget_map_insert` is currently **pointer-keyed dedup**:

```cpp
for (uint32_t i = 0; i < n; ++i) {
    if (g_widget_map[i].widget_ptr == widget_ptr) return;
}
```

Bug: same name with a NEW pointer (after screen reload reallocates the widget) appends a SECOND entry. `widget_map_lookup` returns the FIRST stale entry. R7's "last-writer-wins" mitigation in v1 was aspirational — the code did not implement it.

### Required change

- Switch dedup key from `widget_ptr` → `name` (string compare).
- On collision (existing entry with same name): update its `widget_ptr` in-place under `g_widget_map_mu`. Memory ordering: release on the pointer write so any reader-thread that subsequently reads the count gets the new pointer too.
- Bound the map: with name-keyed updates, total entries = number of distinct widget names ever seen. Engine boots ~100 widgets per session; 4096 cap stays.
- Atomic memory ordering: the lookup is currently lock-free (`g_widget_map_count.load(acquire)` then linear scan). With name-keyed in-place updates, a concurrent reader might observe a name with stale pointer for a few cycles. Mitigation: the read should retry once if the pointer it returns later fails a sanity check (within the heap range). Alternative: bracket the lookup with the mutex too — adds ~50 ns per `prod_capture_pre`, acceptable.

### Bundled fix scope

- **DO** this fix as its own commit (audit B1 — rollback plan):
  - The fix corrects existing buggy behavior even on today's build (where the map is currently empty so the bug is dormant).
  - If Phase 0+ work later needs to be reverted, this fix is a safe, isolated baseline.

---

## Phase 0 — RE investigation (only after Phase −1 says "proceed")

**Hard-sequenced sub-phases with explicit time caps and go/no-go gates** (audit A2). Numbers reflect prior Phase 4 attempts averaging 4–6 hrs each before disproof.

### Phase 0A — POST-return factory hook on `sub_4916E0` (cap: 30 min)

**Mechanism:** the failed attempts in the post-mortem were direct-prologue hooks (MinHook copies the prologue → SEH chain corrupts). POST-return is a different mechanism:

1. Patch each of the 3 known call sites of `sub_4916E0` (`0x45341F`, `0x4534B9`, `0x454C7B`) to redirect their `call rel32` to a wrapper.
2. The wrapper calls the engine function via its absolute VA (`0x4916E0`) using a normal indirect call. The engine function runs on its own stack with its SEH prologue executing normally — the wrapper has no SEH involvement.
3. After the engine function returns, the wrapper reads `*(this+12)` (the constructed widget pointer per the post-mortem decompile) and `*(widget+0x130)` (the name).
4. The wrapper inserts into `widget_map` (using the bug-fixed `widget_map_insert`).

```cpp
// Pseudocode for Phase 0A wrapper.
typedef void* (__thiscall* PFN_4916E0)(void* this_, int a1, int a2, int a3, int a4);
constexpr PFN_4916E0 real_4916E0 = reinterpret_cast<PFN_4916E0>(0x4916E0);

void* __fastcall wrap_4916E0(void* this_, void* /*edx*/, int a1, int a2, int a3, int a4) {
    void* ret = real_4916E0(this_, a1, a2, a3, a4);     // SEH executes safely on the original entry
    void* widget = *((void**)((char*)this_ + 12));      // POST: the constructed widget
    if (widget) {
        const char* name = *(const char**)((char*)widget + 0x130);
        if (looks_valid(name)) widget_map_insert((uint32_t)widget, name);
    }
    return ret;
}
```

### `looks_valid(name)` definition (audit N2)

A name candidate is valid if and only if ALL hold:

1. `name` non-null.
2. `name` lies in `[0x00010000, 0x7FFE0000)` (user-mode, non-NULL).
3. `VirtualQuery(name, ...)` returns committed, non-guard, non-noaccess memory.
4. First 64 bytes contain only ASCII printable (`0x20..0x7E`) followed by a NUL within those 64.
5. NUL terminates within `[4, 56]` — widget names are always at least 4 chars (e.g. `IDS_`) and never longer than ~50.
6. First 4 chars contain ≥ 2 ASCII letters (filters out all-digit or all-punctuation noise; matches the existing `widget_probe.cpp::looks_like_meaningful_string`).

Reuse the existing `widget_probe.cpp::extract_string + looks_like_meaningful_string` to avoid divergence.

### Phase 0A go/no-go (audit N3 fix)

The post-mortem documented that `sub_4916E0`'s 3 known callers pass widget-types `7`, `0xA`, `6` — none in the Sprite range `0..4`. **If those are the only callers**, this hook never fires for IDS_* widgets. Phase 0A's investigation MUST first answer:

- **0A.1** (5 min): Run `xrefs_to(0x4916E0)` exhaustively (incl. data-ref scan, indirect-call detection via `mov eax, 0x4916E0` immediates and function-pointer table writes). Confirm the 3 known callers ARE the only callers, or find more.
- **0A.2** (10 min): If the only callers pass non-Sprite types, Phase 0A is dead. Skip to Phase 0B without spending the rest of the cap.
- **0A.3** (15 min): If Sprites DO go through `sub_4916E0` (via an indirect path the xrefs missed, or one of the 3 callers actually dispatches Sprites), implement a single-call-site patch (use the existing `sprite_probe.cpp::install_call_site_patch` precedent) and verify the wrapper compiles and the basic capture works.

**Decision:** ✅ proceed to Phase 1A iff 0A.3 succeeds. ❌ otherwise advance to Phase 0B.

### Phase 0B — Sprite vtable logging shim (cap: 2 hrs) — likely first real candidate

Sprite vtable at `0x6B8E98`. `vtable[1] = GetType` known; other slots unmapped.

**Logging-shim approach:**

1. Read all 30+ vtable entries.
2. For each function-pointer slot, install a per-slot wrapper:
   - On entry, read `*(this+0x130)` (and adjacent offsets `+0x12C`, `+0x134`, `+0x138` per audit R9 — Sprite class may use a different offset than Btn).
   - Use `looks_valid(name)` (defined above) to filter.
   - Log: `(this, slot_index, name_at_+0x12C, name_at_+0x130, name_at_+0x134, name_at_+0x138, was_first_seen)` — `was_first_seen` from a small hash-set of `this` pointers cleared on screen transition.
   - Tail-jump to the original function pointer.
3. Run the game, walk to GameSelectScreen.
4. Inspect log for: which slot is first called per Sprite construction, and what slot fires while m_pcName is non-null but BEFORE the widget appears in render output.

### Phase 0B m_pcName offset validation (audit E6, A8/R9)

For at least 3 sample Sprite widgets, log m_pcName candidates at `+0x12C`, `+0x130`, `+0x134`, `+0x138` and look for the offset that consistently produces widget-identifier strings (`IDS_*`, `Btn_*`, etc.). If **+0x130 isn't the offset for Sprites**, find the actual one before Phase 1.

### Phase 0B performance check

The wrapper fires for every vtable call on every Sprite. If `vtable[N]` is `Render` (per-frame), this is thousands of calls/sec. Mitigation:
- Time-cap the wrapper at ~100 ns budget; bail out fast if frame-time delta > 5 fps under the shim.
- Use thread-local "in-wrapper" flag to skip re-entrant calls (audit E5 — known coverage limitation, not full mitigation).

### Phase 0B go/no-go

✅ if at least one slot fires once per widget (or first time per widget) AFTER `m_pcName` is set AND m_pcName offset confirmed → Phase 1B.
❌ otherwise → Phase 0C.

### Phase 0C — `.sc` parser hook (cap: 4 hrs)

Last resort. Investigation route:
- `CreateFileA` IAT at `0x6A6114`; one caller `0x586C32` is a generic file-open helper. Trace from screen init to find the .sc opener.
- Search for token strings that appear in .sc text format (e.g. references to `Sprite` as a parser keyword, separate from the vtable[1] `GetType` reference). The .sc files use plain text (`Sprite IDS_TOP { ... }`).
- Once the parser is located, find the call site where it dispatches to widget ctors with both `(name, ctor_address)` available simultaneously.

Stolen-byte thunks at `0x21703A8+` (one per .sc filename) make this region harder to instrument cleanly; vtable-redirect remains preferable if 0B finds anything usable.

### Phase 0 acceptance (any path)

Hook target must satisfy ALL of:

1. **SEH-proof** (audit E1): target's prologue is verified clean by disasm AND any function it calls synchronously before our hook reads m_pcName either (a) is also SEH-free, or (b) we hook POST-return so all SEH events resolve before we run.
2. **Capture coverage:** hook fires for all 11 widget types (`Sprite`, `Text`, `Group`, etc.) OR per-class hooks installed.
3. **m_pcName offset validated** (audit E6, A8/R9) for the actual classes we capture.
4. **Performance:** at GameSelectScreen with 200+ entries, frame rate within 2 fps of pre-Phase-0 baseline.

If any criterion fails → abort; document the negative result; don't proceed.

---

## Phase 1 — Implement the chosen hook (cap: 4 hrs)

Output of Phase 0 selects one of:

- **Phase 1A:** POST-return factory hook on `sub_4916E0` (call-site patch on each caller).
- **Phase 1B:** Sprite vtable slot patch on the slot identified in Phase 0B. Repeat for Text/Group vtables if they're separate.
- **Phase 1C:** `.sc` parser CALL-site patch.

### `g_pending_widget_name` dangling-reference mitigation (audit E3 — re-added in v3)

`widget_probe.cpp:327` `g_pending_widget_name` is a non-atomic borrowed `const char*` global, set in `widget_probe_dispatch_pre` (PRE) and consumed in `widget_probe_dispatch_post` (POST). The pointer borrows from engine memory. If the widget object is destroyed between PRE and POST, the pointer dangles.

**Mitigation in Phase 1:**

- The Phase 1 hook fires at construction; the widget object IS alive at the moment we capture the name. Insert into `widget_map` immediately (synchronously, under `g_widget_map_mu`).
- Once `widget_map` is the authoritative source, `prod_capture_pre`'s slow-path stack-scan + `g_pending_widget_name` machinery becomes redundant. Phase 4 removes it (conditionally — see audit C1 fix below).
- For the brief transition (Phase 1 lands but Phase 4 cleanup is deferred): keep `g_pending_widget_name` but add a guard — verify the pointer still points into committed memory and reads as a valid name before consumption. Two-step `looks_valid` check: read first 4 bytes, validate, then proceed. Cheap (one VirtualQuery per pre-call already happens in `is_safe_to_read`).

### Re-entrancy (audit E5 — coverage limitation)

For Path B (vtable patch): thread-local "in-wrapper" flag prevents infinite recursion, but if a container widget's ctor creates child widgets via the same vtable slot, child widgets are silently SKIPPED. **This is a known coverage limitation, not a fully-mitigated risk.** Acceptable iff the user's gradient widgets aren't container children. Phase 3 verifies this (test 3.8).

### Roll-back plan (audit B1)

The Phase 1 hook lands as a SEPARATE commit from the prerequisite `widget_map_insert` fix. If Phase 1 produces a regression, it can be reverted independently of the bug fix. The two changes have separate test sequences and separate go/no-go gates.

Concretely: a runtime kill-switch toggle (`g_widget_name_capture_enabled`, default-on) is added in Phase 1. The hook checks it before doing anything; flipping it to off restores pre-Phase-1 behavior without code revert. The kill-switch is not exposed in the menu UI for normal users — it's a developer-debugging knob set via `set_widget_name_capture(false)` in `dllmain.cpp::install_thread`.

---

## Phase 2 — Wire into existing infrastructure (cap: 1-2 hrs)

`widget_probe.cpp` already has:
- `widget_map`: `uint32 widget_ptr → const char* name` (after the prerequisite fix above)
- `widget_map_lookup` and `widget_map_insert` API
- `prod_capture_pre` slow-path that already tries `widget_map_lookup` first

The Phase 1 hook calls `widget_map_insert(widget_ptr, name)` once per construction. Subsequent SubmitSprite calls automatically pair their entries with `widget_name` via the existing slow-path. **No changes to `sprite_xform.cpp`.**

`add_pending_by_widget_name` already wires INI-loaded slots to first-sight widgets by name — already verified in Phase −1.8 (audit B3).

---

## Phase 3 — Validate end-to-end (cap: 2-3 hrs)

Sequential tests, each a hard gate:

- **3.1** `widget_probe::debug_dump_widget_map()` shows `IDS_TOP`, `IDS_BOTTOM`, `IDS_HIGHLIGHT_BACKGROUND` plus ≥ 5 other named widgets, with non-stale pointers (verify by re-running the dump and comparing).
- **3.2** Frame side-table (`debug_dump_frame_table`) shows each gradient sprite paired with its widget name.
- **3.3** Specialize a slot on the gradient. Confirm the slot's `widget_name_hash` is non-zero AND the slot's `widget_name` field shows the live string in the menu UI.
- **3.4** ESC to title, return to GameSelectScreen. Confirm the user's specialized slot still controls only its widget. Use the same screenshot-comparison criterion as Phase −1.4.
- **3.5** Restart the game. Confirm the slot reattaches via `add_pending_by_widget_name`.
- **3.6** Performance: no 6 s freeze, no audio buffer underrun, frame rate within 2 fps of pre-Phase-1 baseline.
- **3.7** Cross-screen reload sequence: GameSelectScreen → Options → GameSelectScreen → Load Game → GameSelectScreen, 5+ times. Confirm no `widget_map` corruption (manual inspection via dump button), no slot drift.
- **3.8** Re-entrancy edge case: identify a container widget on GameSelectScreen (e.g. menu list). Confirm its child widgets either (a) ARE captured, or (b) have stable identity via geometric heuristics so the user can still specialize them.
- **3.9** Capture-coverage measurement: dump `widget_map` after walking through 10 different screens; confirm % of captured names matches the engine's announced widget count for those screens (where measurable). Gives us a baseline coverage number for Phase 4 conditionality.

---

## Phase 4 — Cleanup (cap: 1 hr) — CONDITIONAL (audit C1 fix)

**Cleanup is gated by Phase 3.9 coverage measurement.** Audit C1 raised a contradiction: original v2 said "remove the slow-path stack scan as provably dead" while R1 named the slow-path as the fallback if Phase 0 fails entirely. Resolution:

| Phase 3.9 coverage | Cleanup action |
|---|---|
| ≥ 95% capture across 10 screens | Remove the slow-path stack scan in `prod_capture_pre`. Provably dead in practice. |
| 80–95% | KEEP slow-path. Add a runtime metric counter (`prod_slow_path_hits`) so we can monitor in real use. |
| < 80% | KEEP slow-path. The new hook is incomplete; classify Phase 1 as partial success. |

**Do NOT downgrade `bbox_quadrant` / `sort_key` weights in `key_specificity`** (audit A5). Those weights provide real separation today; they remain the fallback for any uncovered widget class. Don't touch them until 6+ weeks of widget_name reliability evidence.

### Memory updates regardless of coverage

- New entry: `project_widget_name_capture_phase1_shipped_<DATE>.md`
- Mark `project_widget_probe_phase4_failed_2026-05-09.md` as superseded with a forward-link.
- Update MEMORY.md index.

---

## Risks (revised — audit-complete)

- **R1: No safe hook target exists.** All candidates have SEH AND no vtable slot fires post-name-set. Mitigation: Phase 0 explicitly accepts this outcome — abort. Today's build's geometric matching shipped; static gradients work; cursor-follower remains fragile.
- **R2: Vtable patches break with re-entrant calls.** Downgraded to *known coverage limitation* (audit E5). Thread-local flag prevents recursion but silently skips child capture. Acceptable iff user's targets aren't affected; verified in Phase 3.8.
- **R3: Multiple widget classes share a vtable.** OK — `widget_map_insert` handles strings without caring about type.
- **R4: Stolen-byte thunks cover the call site we want to patch.** Mitigation: prefer Path 1B (vtable, data-only) over 1A or 1C.
- **R5: Engine creates pre-screen widgets we don't see.** Mitigation: install Phase 1 hook from `dllmain.cpp::install_thread` before first screen push. Verify via startup log.
- **R6: m_pcName isn't always at +0x130.** Phase 0B's vtable shim validates the offset for Sprites specifically (audit E6).
- **R7: Save/restore semantics with widget-name primary key.** Engineering audit found the v1 mitigation broken — `widget_map_insert` was pointer-keyed dedup. **Fixed in pre-Phase-1 commit** (name-keyed with last-writer-wins).
- **R8: Widget pool pointer reuse** (audit A6/R-unlisted-1). Same fix as R7. Plus `clear_frame_table` per frame already drops stale side-table entries.
- **R9: m_pcName offset varies per widget class** (audit A8/R-unlisted-3). Phase 0B logs +0x12C/+0x130/+0x134/+0x138 for ≥ 3 sample Sprites; if +0x130 isn't right, find the actual offset.
- **R10: Thread-safety during screen transition** (audit A7/R-unlisted-2). `g_widget_map_mu` covers inserts. With name-keyed in-place updates, atomic-write the pointer field on update; readers may briefly see old pointer with new entry — benign as long as both pointers point into committed memory (the lookup retries one heap-range check).
- **R11 (NEW): `g_pending_widget_name` dangles between PRE and POST.** Audit E3. Mitigation: re-validate the pointer before consumption in POST (cheap VirtualQuery via `is_safe_to_read`); long-term, the slow-path is removed in Phase 4 conditional cleanup.
- **R12 (NEW): Phase 1 ships, regression appears days later, can't easily revert.** Audit B1. Mitigation: kill-switch `g_widget_name_capture_enabled` (default-on) lets us flip behavior without revert; pre-Phase-1 fix is a separate commit so it survives any Phase 1 revert.

---

## Alternatives considered

- **A1:** Direct factory hook on `sub_4916E0` with `__fastcall` detour. Same SEH prologue. Phase 4 post-mortem proved this fails. Dead. (POST-return variant survives as Phase 0A.)
- **A2:** Path-based runtime rebind in `process_list`. Implemented this morning, reverted same-day. Generic textures (whitesprite.tga) cause cross-screen mis-matches. The proper "what is this widget" identity is the engine's name string, not the texture path.
- **A3:** Promote `bbox_quadrant` to finer (16×16 grid). Doesn't help cursor-follower. Doesn't help cross-screen reload.
- **A4:** Mandate manual user labeling. UX unacceptable: 50+ widgets per screen, no semantic anchor.
- **A5:** Pre-bake widget identity from `.sc` files at build time. Pros: no runtime hook. Cons: still need a runtime `widget_pointer → name` map to USE the IDs from sprite_xform; complementary to Phase 1, not a replacement. Future option if Phase 1 bandwidth needs reducing.
- **A6:** Coordinate-rect approach (per-screen rules with X,Y rect tolerance). Works for static gradients (TOP/BOTTOM at fixed coords). Fails for cursor-follower. Not a complete solution.

---

## Open questions for Phase 0

- **OQ1:** Does the .sc text parser invoke widget ctors directly, or via a string-keyed factory map?
- **OQ2:** Is `m_pcName` set BEFORE or AFTER the widget is added to the screen's render list? (Audit E6.)
- **OQ3:** How many of the 56 native screens use the standard widget set vs. custom widget classes?
- **OQ4 (audit-added):** Does sort_key actually separate the 3 gradients across screen reloads? (Closed by Phase −1.)

---

## Estimated effort (audit-revised)

**Caps are direction-re-evaluation prompts, NOT wall-clock budgets.** Per RULE №1, if a phase hits its cap and the proper-fix path is still tractable (e.g. you've identified the right hook target but the implementation needs more careful instrumentation), extending the phase is the right call. The cap fires a "stop and re-evaluate" decision: are we close, or are we drifting? Drift → switch sub-phase. Close → continue. **A 72-hour focused investigation is totally fine** if the work is genuinely producing the proper fix.

Numbers below reflect the median expected case; actual elapsed time depends on which Phase 0 sub-phase succeeds.

| Phase | Cap | Cumulative | Outcome to log |
|---|---|---|---|
| Phase −1 | 30 min | 30 min | Pass/fail per −1.1..−1.8; decision branch |
| Pre-Phase-1 widget_map_insert fix | 30 min | 1 hr | Commit hash + 1-line test |
| Phase 0A (factory POST-return) | 30 min | 1.5 hr | Either ✅ found Sprite path or ❌ skip to 0B |
| Phase 0B (vtable shim) | 2 hr | 3.5 hr | Either ✅ usable slot found OR ❌ skip to 0C |
| Phase 0C (.sc parser) | 4 hr | 7.5 hr | Either ✅ patchable site OR ❌ abort |
| Phase 1 (implement chosen hook) | 4 hr | 11.5 hr | Hook compiled + basic capture works |
| Phase 2 (wire to widget_probe) | 1 hr | 12.5 hr | No changes to sprite_xform; just verification |
| Phase 3 (validate end-to-end) | 3 hr | 15.5 hr | All 9 tests pass |
| Phase 4 (conditional cleanup) | 1 hr | 16.5 hr | Coverage measurement + appropriate cleanup |

**Best case** (Phase 0A succeeds at 0A.3): 1 hr 15 min Phase −1+pre-Phase-1+0A → Phase 1+2+3+4 = 9 hr → ~10 hr total.

**Worst case** (Phase 0 burns through to 0C): 7.5 hr Phase 0+pre-Phase-1+−1 → Phase 1+2+3+4 = 9 hr → ~16.5 hr total.

**Abort case** (Phase 0 fails at 0C): 7.5 hr → document negative outcome, no Phase 1+. Total spent: ~7.5 hr. The user's geometric-discriminator build remains in place.

---

## Audit history

- **v1** (2026-05-09 morning): original draft.
- **v2** (2026-05-09 afternoon): incorporated engineering audit (a16e16def18b94439) and architecture audit (a2730eb1ec8dd1c58).
- **v3** (this revision, 2026-05-09 evening): incorporated third-round verification audit (a567663eb0f078b30):
  - **E3** re-added — `g_pending_widget_name` dangling-reference mitigation now in Phase 1.
  - **A4** fixed — explicit Path A/B/C ↔ post-mortem mapping table at the top.
  - **N1** fixed — estimate table now uses cumulative caps and best/worst case math is correct.
  - **N2** fixed — `looks_valid(name)` defined explicitly with 6 criteria.
  - **N3** fixed — Phase 0A starts with a 5-min cheap "is sub_4916E0 even relevant?" check before committing the rest of its 30-min cap.
  - **C1** fixed — Phase 4 cleanup made conditional on Phase 3.9 coverage measurement.
  - **B1** fixed — explicit kill-switch + separate-commit rollback plan in Phase 1.
  - **B2** fixed — Phase −1 has measurable per-test pass/fail criteria.
  - **B3** fixed — `add_pending_by_widget_name` reattach is tested in Phase −1.8, not assumed.

---

## Phase −1 outcome

**Date:** 2026-05-09 evening
**Build:** `Game/mtr-asi.asi` 608256 bytes (deployed 15:50 with diagnostic logging on top of the 607744 pre-Phase-1 fix)
**Method:** instead of the 8-test eyeball protocol, three log lines (`[diag] specialize`, `[diag] screen_push/pop`, `[diag] new state_key`) gave the answer in a single repro.

### Per-test verdict (mapped from log evidence at `Game/mtr-asi.log` lines 164-474)

| # | Verdict | Evidence |
|---|---|---|
| −1.1 | PASS | Per-element control opened; rows visible |
| −1.2 | PASS | 5 distinct Specialize events captured with different `(uv, sc, q, sort)` tuples |
| −1.3 | PASS | sort_key varies (0x012C, 0x0192, 0x012D) — not all-same |
| −1.4 | PASS | User confirmed sx=1.342 on one row affected only its widget (screenshot 2) |
| −1.5 | n/a | Subsumed by −1.4 evidence |
| **−1.6** | **FAIL** | Same texture (`state_key=0x0F90E4D0`, whitesprite.tga) gets specialized 5 times across 2 different screens because `screen_context` differs (0x1F on GameSelectScreen, 0x3A on WilburMainMenu). Identity does NOT carry across screens. |
| **−1.7** | **FAIL** | (Same evidence as −1.6 — slots only drive their widget on the screen where they were specialized) |
| −1.8 | not tested | Cross-session reattach not exercised; moot given −1.6/−1.7 fail |

### Decision branch

Plan's nominal `−1.6/−1.7 fail` branch was "persistence path is broken — surgical 2-4 hr fix." **That misclassification doesn't apply here.** The state_key is fully stable across screen transitions (`0x0F90E4D0` lives the entire session); `[diag] new state_key` never fires for whitesprite after its first allocation. So there is no persistence bug to fix surgically.

The actual cause is by-design: `specialize_slot` ([sprite_xform.cpp:1499](../../src/mtr-asi/src/sprite_xform.cpp#L1499)) copies `last_concrete` whole, which includes `screen_context`. The same widget rendered on two screens has two different `screen_context` values, so a slot pinned to one screen can't match on another. Specialize-pinning is too narrow.

This is **the widget-name-capture branch.** Engine `m_pcName` is screen-independent by construction; matching by widget identity eliminates `screen_context` pinning cleanly. **Phase 0 begins** per the v3 plan, RULE №1 path (no quick-win Specialize variant).

### Secondary finding (out of scope for Phase 0, log it for later)

`screen_pop` hook on `sub_604C90` ([screen_push.cpp:111](../../src/mtr-asi/src/screen_push.cpp#L111)) never fired during the session despite the user navigating through 4 screens. Either the hook target is wrong or the engine doesn't call it on ESC. Depth tracking is therefore monotonic-up (1→2→3→4) instead of round-trip. This is a real RE bug but doesn't block Phase 0 — proceed with Phase 0A.1.

### Pre-Phase-1 widget_map_insert fix status

Shipped as a separate commit before this verification ([widget_probe.cpp:351-401](../../src/mtr-asi/src/widget_probe.cpp#L351-L401)). Map is currently empty (no Phase 1 hook yet) so the fix is dormant; the bug it eliminates would have struck the moment Phase 1 ctor capture lands. Build = 607744 bytes for the fix alone, 608256 with diagnostic logging on top.
