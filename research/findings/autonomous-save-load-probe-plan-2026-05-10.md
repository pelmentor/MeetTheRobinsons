# Autonomous Save-Load Probe Loop — FINAL Plan v0.2

**Status:** post-dual-audit. Synthesizes architect proposal + independent proposal + 2 audit passes.
**Date:** 2026-05-10.
**Goal:** Let the assistant launch Wilbur.exe with one cmdline flag, auto-load the user's first save, reach gameplay, fire `coop_spawn_probe::try_spawn_p2`, log result, exit cleanly.
**Standing rule:** RULE №1 — no shortcuts. Months OK if it gets us there cleanly.

## Non-goals

- Creating saves (user provides; not in scope).
- Direct-call save-loader (would skip preconditions; would require RE'ing `unk_72F824` co-routine).
- Mouse injection (not yet implemented in mod; menus are keyboard-drivable per RE).

---

## What changed from v0.1 after audit

The v0.1 draft was wrong in five places. Both audits flagged them independently:

| v0.1 mistake | Why wrong | Fix |
|---|---|---|
| Trust `is_safe_screen` for "in gameplay" | It's a blocklist, fires on pause/options/inventory/unknown-loading-screens | Add positive-match `is_gameplay_top`; don't reuse the probe gate |
| Phase B's three-branch detection | Hides RE ignorance about what `Continue`/`Load` buttons do | 30-min IDA on `sub_46DF10` + button callbacks → single deterministic action |
| `kStabilizeConsecutive = 30` frames | Frame-count is fragile across 60Hz↔240Hz | Gate on transform-list count crossing menu→gameplay threshold + stable K frames |
| Save-path RE before code | Wrong order; just out-of-scope | Skip pre-flight; let Phase B/C fail fast with `detail="continue greyed; populate slot 1"` |
| `phases[]` JSON | Speculative — `detail` string carries the same info | Drop. Single `detail` field names the failed phase |

Two audits also surfaced a **safety-critical risk** v0.1 missed: **WM_CLOSE during gameplay may trigger autosave that overwrites slot 1**. Verify before any run.

---

## Critical pre-work (before any harness LOC)

### Pre-1 — Smoke test: does DIK injection actually drive menu nav?

The existing `inject_kb_keypress` is proven for title-splash dismissal (single press absorbed by a "press any key" handler). It is **NOT proven** for menu navigation through Wilbur's widget/ControlMapper system.

**Action:** ship a `menu-nav-smoke` scenario (~50 LOC). Boot to main menu → settle 60 frames → inject `DIK_DOWN × 5 polls` → hold 60 frames → Pass if any of (`current_top_name` changed, `screen_push` fired, `screen_pop` fired) during the hold.

**Outcome gates everything else:**
- **Pass:** the full plan below proceeds as designed.
- **Fail:** DIK doesn't route through the menu's input layer (likely ControlMapper polling). Plan pivots to widget-callback dispatcher hook (originally a v1.0 deferral, becomes v0.1).

### Pre-2 — IDA query: what does `sub_46DF10`'s Continue button actually do?

30-minute IDA pass:
1. Decompile `sub_46DF10` (GameSelect screen ctor — independent agent located the `+1120` Continue gate flag here).
2. Identify the Continue button callback. Trace what screen it pushes.
3. Identify the Load button callback. Same.
4. Determine whether Continue jumps directly to gameplay (skip slot screen) or to slot screen.

**Outcome:** Phase B becomes a single deterministic action, not a three-branch heuristic.

### Pre-3 — Autosave-on-shutdown safety check

WM_CLOSE during gameplay may trigger autosave. **If autosave fires on shutdown, every harness run overwrites the user's slot 1.**

**Action:** one manual run. Save in slot 1, note slot-1 timestamp + content marker. Launch normally, reach gameplay, close via Alt+F4 (same as harness's WM_CLOSE path). Re-launch, check slot 1 — unchanged?

**If unchanged:** harness is safe to use repeatedly. Document the test result.
**If changed:** add explicit `request_shutdown_no_autosave()` API. Options:
- Patch the engine's "save-on-exit" code-site to NOP (cleanest but requires RE).
- TerminateProcess instead of WM_CLOSE (kill -9; engine never gets the autosave hook). Less clean but zero RE.

---

## Architecture (post-audit)

```
   tools/run-test.ps1
        │
        ▼  -mtrasi-test=coop-spawn-probe-from-save-1
   Wilbur.exe + mtr-asi.asi
        │
        ▼
   test_harness.cpp::tick()      ← called from on_end_scene each render frame
        │
        ▼
   Inline static-state phase machine (matches existing scenarios — pattern proven 4x)
        │
   ┌────┴───────────────────────────────────────────────────────────────┐
   │  A: BootDismissTitle        (existing pattern; advance: top == GameSelect)│
   │  B: ClickContinue           (single deterministic action from Pre-2)     │
   │  C: ConfirmSlot1            (only if Pre-2 says Load goes to slot picker)│
   │  D: WaitForGameplayTop      (positive-match is_gameplay_top())           │
   │  E: SettleByTransformCount  (transform-list growth + stable K frames)    │
   │  F: RunProbe                (try_spawn_p2 + last_result + COOP_PROBE_RESULT)│
   │  G: ReportAndShutdown       (write JSON + safe shutdown)                 │
   └────────────────────────────────────────────────────────────────────┘
```

**Why inline static-state, not `Phase[]` array:** the existing harness has 4 scenarios using inline static-state (`tick_overlay_phase1_verify`, `tick_npc_overlay_phase1_verify`, `tick_prop_overlay_phase1_verify`, `tick_boot_to_main_menu`). Independent audit pushed for `Phase[]` generalization now. Architect audit said inline is correct for v0.1. **Resolution: inline.** Reasons: pattern is established and proven; phase walker abstraction would need to handle per-phase state (frame counters, slot indices, retry counts) which inline does naturally; generalization deferred to when there are 6+ scenarios sharing 80% of phases — currently we have 4 sharing only Phase A.

---

## Phase contracts (final)

### Phase A — BootDismissTitle
Reuses proven `tick_boot_to_main_menu` body. Inject `DIK_RETURN` every 60 frames until `screen_push::ready()` AND top is `GameSelectScreen`.

**Advance:** `current_top_name == "GameSelectScreen"` AND `screen_push::ready()`.
**Timeout:** 30s.
**Cleanup before exit:** the harness must stop pushing `DIK_RETURN` the frame Phase A advances. Audit caught this — fixed-cadence injection in Phase A could leak a stray RETURN into Phase B if Phase A's loop body runs once more after the advance condition. Fix: add a `phase_a_done` static; gate the injection on `!phase_a_done`.

### Phase B — ClickContinue (deterministic per Pre-2 outcome)
**Single action.** Determined by Pre-2 IDA pass:
- If Continue jumps direct to gameplay: `inject_kb_keypress(DIK_RETURN, 10)` once → skip Phase C.
- If Continue requires slot pick OR if save absent (Continue greyed): `inject_kb_keypress(DIK_DOWN, 10)` then 30-frame wait then `inject_kb_keypress(DIK_RETURN, 10)` (selects "Load Game") → fall through to Phase C.

**Advance:** `current_top_name != "GameSelectScreen"` (edge-detected; not fixed-frame poll — audit caught this).
**Timeout:** 10s. On timeout, `detail="DIK_RETURN at GameSelectScreen had no observable effect"` — this is the exact failure the smoke test (Pre-1) is designed to catch first.

### Phase C — ConfirmSlot1 (skipped if Phase B routed to gameplay)
**`DIK_UP × 2`** to home cursor (audit flagged: verify slot list doesn't wrap on UP — one-minute in-game check before ship), wait 30 frames, **`DIK_RETURN`** to confirm slot 1.

**Advance:** `current_top_name` no longer matches the slot-picker screen name (captured during Pre-2 IDA pass).
**Timeout:** 60s (load can be slow).

### Phase D — WaitForGameplayTop
Positive-match gate. **New predicate `is_gameplay_top(name)`** — separate from probe's `is_safe_screen` blocklist:

```
is_gameplay_top(name):
  - matches one of known gameplay screen names (allowlist),
    captured during first run via screen_push log archaeology
  - OR: stack depth at gameplay-typical level
    (will need RE — placeholder until first run)
```

**v0.1 implementation:** start with empty allowlist + log every screen name encountered after Phase C. **First run will be a discovery run** that populates the allowlist from observed screen names. Document this as the v0.1 bootstrap step.

**Advance:** `is_gameplay_top(top) == true`.
**Timeout:** 90s (cold-cache asset load worst case).
**Heartbeat logging:** Phase D fires a `[testharness] Phase D heartbeat: top=<name> elapsed=<ms>` line every 5 wall-clock seconds (not frame-based — audit flagged that frame-rate stalls during heavy load can starve the log-stall watchdog).

### Phase E — SettleByTransformCount
**Transform-list-count gate** (audit's correction — frame-count was fragile):

1. At Phase D advance: snapshot transform-list count `N0` from `dword_724DE4` walker.
2. Wait until count > `N0 + threshold` (threshold ~10; menu has few transforms, gameplay has many).
3. Then wait until count is stable (changes ≤ ±2) for K=30 consecutive frames.
4. ALSO require `entity_lookup_by_name_retry("player", 1)` returns non-null using the `interp_player.cpp` __thiscall pattern (`g_entity_manager_ptr` from `0x7425AC` into ECX, SEH-wrap).

**Advance:** all three conditions hold.
**Timeout:** 30s after Phase D advanced (transform-list never grew → suspicious; player entity never resolved → fail with detail).

### Phase F — RunProbe
Call `mtr::coop_spawn_probe::try_spawn_p2()`, read `last_result()`. Emit one grep-stable line:

```
COOP_PROBE_RESULT screen=<name> attempted=<0|1> succeeded=<0|1> entity=<addr|null> list_delta=<n> post_init_reached=<0|1> post_init_v13=<hex> exception=<0|1> message="<...>"
```

**Result mapping:**
- `Pass`: `attempted && !exception` (probe ran cleanly; spawn outcome IS the diagnostic data).
- `Fail`: exception or gate rejection.

**Cleanup safety (audit fix):** add `coop_spawn_probe::cancel()` API that clears `g_observing` flag. Phase F's timeout handler calls `cancel()` before advancing to G. Closes the leak where SEH catches a fault inside `try_spawn_p2` and `g_observing` stays true forever.

**Timeout:** 5s.

### Phase G — ReportAndShutdown
Write `mtr-asi-test-result.json` (existing flat shape, NOT `phases[]` — audit dropped that). `detail` field carries failure point. Call `request_shutdown()` (or the no-autosave variant from Pre-3 if needed).

---

## Screenshot capture (mandatory diagnostic, not optional)

Existing API: `mtr::screenshot::request()` → writes `Game/screenshots/mtr_*.bmp` → `run-test.ps1` archives them per-run. Existing scenarios (`tick_overlay_phase1_verify` etc.) already use it.

Screenshots are **the only way** the assistant can verify what the game actually rendered. Logs say "DIK_RETURN injected at GameSelectScreen, top changed to ScreenWilburNewLoadSave" — but only a screenshot proves the slot list rendered correctly, the highlighted slot is slot 1, no error dialog popped. Especially critical for the discovery run (Phase D allowlist seeding) and any failure run.

**Capture points (mandatory):**

- Phase A advance: one screenshot when title splash dismissed (first `GameSelectScreen` view).
- Phase B advance: one screenshot the frame after each DIK injection lands (proof DIK had visible effect).
- Phase C advance: one screenshot of the slot picker before RETURN, one after (proof slot 1 was highlighted).
- Phase D advance: one screenshot the moment `is_gameplay_top` first returns true.
- Phase E advance: one screenshot at full-settle (gameplay rendered, transform list saturated).
- Phase F: one screenshot before `try_spawn_p2`, one after (proof of any visual difference from spawn).
- Any phase timeout: one screenshot at timeout instant — names what was on screen when it gave up.
- Heartbeat (Phase D long load): one screenshot every 5s wall-clock during long phases (load progress visible? hung? error dialog?).

**Rate limit:** existing scenarios cap at ~1 screenshot per 240 frames (~1s) to avoid disk thrash + log spam. Same discipline here. Phase advances are edge events — fire once, not every frame the condition holds.

**Run-test.ps1 archives the directory** to `Game/test-runs/<timestamp>/screenshots/`. The assistant reads them via the Read tool (multimodal — Claude can see BMPs directly).

## Output contract

Single grep-stable diagnostic line on probe completion:
```
COOP_PROBE_RESULT screen=<name> attempted=<0|1> succeeded=<0|1> entity=<addr|null> list_delta=<n> post_init_reached=<0|1> post_init_v13=<hex> exception=<0|1> message="<...>"
```

Result JSON (existing flat shape):
```json
{
  "scenario": "coop-spawn-probe-from-save-1",
  "result": "pass" | "fail",
  "elapsed_ms": <int>,
  "frames": <int>,
  "detail": "<phase letter / name>: <what happened>"
}
```

`detail` examples:
- `"F/RunProbe: probe ran, factory returned 0x<addr>, list_delta=1"` (pass)
- `"B/ClickContinue: DIK_RETURN at GameSelectScreen had no observable effect"` (fail)
- `"E/Settle: player entity never resolved within 30s"` (fail)

---

## Files to touch (v0.1)

- `src/mtr-asi/src/test_harness.cpp` — `tick_menu_nav_smoke` (Pre-1, ~50 LOC), `tick_coop_spawn_probe_from_save_1` (~200 LOC), `is_gameplay_top` predicate, `resolve_player_entity_for_harness` helper, transform-list count helper. Two new entries in `g_scenarios[]`.
- `src/mtr-asi/src/coop/coop_spawn_probe.cpp` — add `cancel()` to clear `g_observing`.
- `src/mtr-asi/include/mtr/coop_spawn_probe.h` — declare `cancel()`.
- `tools/run-test.ps1` — add `coop-spawn-probe-from-save-1` post-processing block to grep `COOP_PROBE_RESULT`.

No changes to existing scenarios. No new hooks. No new files.

---

## Acceptance criteria

### Pre-flight (gates v0.1 implementation)
- Pre-1: `pwsh tools/run-test.ps1 -Scenario menu-nav-smoke -Redeploy` exits 0 — DIK_DOWN observably affects `GameSelectScreen`.
- Pre-2: `sub_46DF10` decompiled. Continue and Load button callbacks identified. Phase B action is deterministic.
- Pre-3: manual save-corruption test passes. WM_CLOSE during gameplay does not modify slot 1.

### v0.1 ship criteria
`pwsh tools/run-test.ps1 -Scenario coop-spawn-probe-from-save-1 -Redeploy`:

- Exits within 4 minutes (sum of phase timeouts + 50% margin).
- Exit codes: 0 (probe ran), 1 (probe gate-rejected or scenario-failed), 2 (process died without JSON), 3 (harness watchdog), 4 (cmdline parse error).
- `mtr-asi.log` contains exactly one `COOP_PROBE_RESULT` line on success.
- `mtr-asi-test-result.json` contains `detail` naming the failed phase if not pass.
- Archived screenshots cover every phase advance + each DIK injection landing + every timeout instant. Assistant can read them post-run to verify what the game actually rendered (logs lie about what was visible; pixels don't).
- **Idempotency:** running the scenario 5x in a row leaves slot 1 byte-identical.

---

## Risk register (post-audit)

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | DIK injection doesn't drive menu nav (only title splash) | **High** | Pre-1 smoke test gates the full plan |
| 2 | Autosave on shutdown overwrites user's slot 1 | **High** | Pre-3 manual test; add `TerminateProcess` shutdown if needed |
| 3 | `is_safe_screen` false-positives mid-load (no positive allowlist) | **High** | Phase D uses new `is_gameplay_top` predicate; Phase E transform-count guard |
| 4 | Phase A's injection cadence leaks RETURN into Phase B | Medium | `phase_a_done` static gate |
| 5 | Slot-list cursor wraps on UP (DIK_UP × 2 lands on Cancel) | Medium | One-minute in-game check before ship |
| 6 | Loading screen runs >90s on cold cache | Medium | Phase D timeout configurable; heartbeat keeps watchdog alive |
| 7 | Probe crash → `g_observing` leaks if SEH catches | Medium | New `coop_spawn_probe::cancel()` API |
| 8 | Save-existence pre-flight not implemented | Low | Phase B reports "Continue greyed" via detail; user reads, populates slot |
| 9 | Transform-list count threshold too high/low | Low | Tune from first run's observed numbers |
| 10 | `entity_lookup_by_name_retry` returns null in gameplay | Low | Memory confirms `("player", 1)` per interp_player.cpp precedent |
| 11 | DI poll dropped under DXVK | Low | `polls=10` for nav phases (existing default 5) |

---

## Sequencing

1. **Pre-1** smoke scenario (~50 LOC, ~30 min) → run → verify DIK menu nav works.
2. **Pre-2** IDA query (~30 min) → Phase B determinism.
3. **Pre-3** manual safety run (~10 min) → autosave guard.
4. **v0.1 implementation** (~1-2 days):
   - `is_gameplay_top` predicate (start with allowlist seeded from Pre-3 observation).
   - `resolve_player_entity_for_harness` helper.
   - Transform-list count helper.
   - `coop_spawn_probe::cancel()` API.
   - `tick_coop_spawn_probe_from_save_1` body.
   - `run-test.ps1` post-processing.
5. **First run** = discovery run. Captures screen names. May reveal Phase D allowlist gaps. Tune.
6. **Acceptance run** (idempotency 5x).

If Pre-1 fails: pivot to widget-callback dispatcher hook, scrap the DIK approach, restart at Pre-1 with the new mechanism.
If Pre-3 fails (autosave overwrites slot 1): implement `TerminateProcess` shutdown variant or RE the engine's save-on-exit hook to NOP it.

---

## What this plan deliberately leaves out

- **`Phase[]` array generalization.** Deferred until 6+ scenarios share 80%+ of phases. Currently 4 share only Phase A.
- **Multi-save support.** Hardcoded slot 1. When other slots are needed, scenario name carries the slot index (`coop-spawn-probe-from-save-2`).
- **Hot-reload / mid-run retry.** Each run is full launch-to-exit. Faster iteration via `-Redeploy` only.
- **Save-path RE.** Out of scope; not needed unless harness creates saves.
- **Mouse injection.** Menus are keyboard-drivable. Mouse path unimplemented.
- **`phases[]` JSON.** Single `detail` string is sufficient.
- **Widget-callback dispatcher hook.** Deferred unless Pre-1 fails. Fallback path documented.

---

## File pointers for the implementation session

- This plan: `research/findings/autonomous-save-load-probe-plan-2026-05-10.md`
- Existing harness: `src/mtr-asi/src/test_harness.cpp` (study `tick_boot_to_main_menu` for Phase A pattern, `tick_overlay_phase1_verify` for settle pattern)
- DI injection: `src/mtr-asi/src/dinput_hook.cpp` (`mtr::dinput_hook::inject_kb_keypress`)
- Screen stack: `src/mtr-asi/src/screen_push.cpp` (`current_top_name`, `ready()`)
- Probe consumer: `src/mtr-asi/src/coop/coop_spawn_probe.cpp` (gets `cancel()` method)
- Outer launcher: `tools/run-test.ps1` (add post-processing block)
- Engine VAs: `g_entity_manager_ptr` @ `0x7425AC` (interp_player.cpp pattern); transform list head @ `dword_724DE4` (npc_overlay/prop_overlay walker pattern); GameSelect screen ctor `sub_46DF10` (Pre-2 target).
