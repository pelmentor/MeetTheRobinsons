# Coop Phase 0A — Dual-agent audit synthesis (2026-05-10)

**Status:** Audit complete. Both audits independently picked **Option A (cheap-derisk factory call)** as the immediate next action. Decision locked: spawn the probe today; parallelize 0B/0C with Phase 1 transport after A succeeds.
**Inputs:** [coop-phase-0a-entity-factory-2026-05-10.md](coop-phase-0a-entity-factory-2026-05-10.md), [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md), pre-audit memory checkpoint.
**Auditors:** code-architect (sequencing/scope), code-reviewer (correctness/ABI). Run in parallel.

---

## Top-line decision

**Option A — call `entity_factory_construct` from the mod with `class=protagonist; name=player2;` — happens NOW.**

Both auditors converged on this. The architect's pushback is the right framing: **A vs B/C is a false dichotomy.** A costs ~1 hour regardless; B (save format) and C (.sx catalog) are not on Phase 1's critical path. The audit-brief's framing of "A before/after B?" was wrong — the right sequencing is:

1. **Now:** Option A (factory-call probe).
2. **After A returns success:** start Phase 1 transport, with 0B/0C running in background.
3. **0D (test harness)** moved to *before* Phase 1, not parallel — gate on Phase 1 being testable.
4. **Phase 2 split:** 2A (spawn survives one sim tick, no input bind) → 2B (input routing). 2A unblocks Phase 3 design.

**If A fails:** the engine crash address pinpoints the missing dependency. We've found it before sinking weeks into Phase 1 transport.

---

## Two NEW critical gotchas surfaced by the audit (not in the original brief)

### Gotcha 7 — Queued vs active factory branch

[coop-phase-0a-entity-factory-2026-05-10.md:95-100](coop-phase-0a-entity-factory-2026-05-10.md):

```c
if ((dword_7193EC & entity[1]) == dword_7193E8 &&
     entity_property_get_int_thunk(entity+3196, /* unknown */ 7096512))
    sub_5AD410(entity);  // active path
else
    sub_5AD3E0(entity);  // queued path
```

If the entity goes to the **queued path** (`sub_5AD3E0`), it will exist as a live pointer but **not be walked** by the sim's per-frame transform/anim ticks. Returning non-null is not enough — we need to verify it's actually in the active world.

**Test implication:** the probe MUST measure transform-list growth at `dword_724DE4`, not just check return-non-null.

- **Delta = 1:** entity entered active path. ✅
- **Delta = 0, return non-null:** entity went to queued path or registered in a different list. ⚠ Diagnose before committing.
- **Delta = 0, return null:** registry lookup or ctor failed. (Failure mode, not a crash.)

The condition semantics depend on `dword_7193EC`/`dword_7193E8` (likely level-load state flags). Calling the factory **only from the in-game screen** (not main menu) is a precondition.

### Gotcha 8 — `entity+3196` is a SECOND bag

The factory tail reads properties from `(char*)entity + 3196` ([coop-phase-0a-entity-factory-2026-05-10.md:97 and 103](coop-phase-0a-entity-factory-2026-05-10.md)):

```c
int v13 = entity_property_get_thunk((char*)entity + 3196, /* unknown */ 6981768, default_misc);
sub_55AF00(entity, v13);
```

This is a **SECOND bag** — the entity's own permanent bag, distinct from the caller-provided construction-template bag. The protagonist ctor populates this bag from internal defaults. The factory's tail then reads from it and passes the result to `sub_55AF00`.

**The risk:** if the protagonist ctor leaves any keys unset in entity+3196 (because the game normally only spawns one protagonist via `.sc` loader, never tested manual construction), the missing-key path returns the `default_misc` (esi=0) default → propagated to `sub_55AF00` as null.

This is the **top candidate** for "factory returns non-null, engine crashes 3 frames later." Mitigation: hook `sub_55AF00` with a PRE-logger before running the experiment, so we know if it was reached and what it received.

---

## Architectural gap the v2 plan didn't account for

### Bag wire-format decision (must be locked at Phase 1, not Phase 3)

Bag keys are hashed by `string_intern_hash` (`sub_5D3E30`) — a **deterministic** function. Two host/client running the same Wilbur.exe will compute identical hash IDs for `"class"`, `"model_name"`, etc. — no string-table negotiation needed, **provided both run identical executables**.

But the v2 plan never specified the wire format for bag-property replication. Engineer's recommendation:

- **Ship template strings on the wire** (`"class=protagonist; name=player2; …"`).
- **Client reconstructs the bag locally** via `bag_init_from_template_THUNK`.
- **Then call the factory locally.**

This mirrors the engine's own pattern (level-load scripts call the factory with template strings). Don't ship raw `{key_id, val_id}` pairs — that path is fragile (requires bag-allocator RE for direct construction) and pointless (the template-string path is already proven).

**Add to Phase 1 design:** an executable-hash handshake at connect time. If client/host run different builds, hash collisions silently corrupt bags with no engine-level detection.

### Phase 2 unblock is bigger than the v2 plan said

v2 plan ([coop-multiplayer-plan-2026-05-10.md, Hard problem 2](coop-multiplayer-plan-2026-05-10.md)) claimed: *"Phase 2 starts with the offline RE work"* (assumed 2-3 weeks of factory RE). That gap **doesn't exist**. The 310-byte factory is fully characterized. **Phase 2's entity-spawn path is unblocked at the mod-code level today.** The plan's framing was anchored on a now-obsolete recon error.

---

## v2 plan timeline revision (per-phase, not multiplier)

| Phase | v2 estimate | Revised | Rationale |
|---|---|---|---|
| 0A entity factory | 2-3 weeks | **DONE (1 hr)** | Recon misidentified target. |
| 0B save format | 1 week | 1 week | Genuinely uncharted; no recon error to correct. |
| 0C .sx catalog | 1 week | **3 days** | Partial inventory already in `script_command_dispatch_giant`. |
| 0D test harness | "few days" | **few days, gate on Phase 1** | Move to before Phase 1, not parallel. |
| 1 UDP transport | 3 weeks | **2-2.5 weeks** | Existing thread/log/mutex infra reduces scope. |
| 2 second player | 4 weeks | **2-3 weeks** | Spawn = 1-2 days now (factory clear). Input routing dominates. Split 2A/2B. |
| 3 player replication | 4 weeks | **3 weeks** | Player layout known. `Mode::CLIENT` is a well-scoped design change. |
| 4 NPC + props | 6 weeks | **6 weeks** | NOT compress. AI behavior-tree RE is real new scope. |
| 5 script VM | 8 weeks | **8 weeks** | Highest complexity; misclassification risk real. |
| 6 audio + UI | 4 weeks | **4 weeks** | Implementation-heavy, well-defined. |
| 7 save/stability | 4 weeks | **4 weeks** | No evidence of recon error here. |

**Revised total: ~32 weeks (~8 months)** vs v2's 34-week estimate. Most savings concentrated in Phase 0/1/2; Phase 4/5 hold their weight.

---

## Sequencing changes to the v2 plan

1. **Parallelize 0A-derisk with 0B and 0C.** After Option A succeeds, start Phase 1 immediately while 0B/0C run in background. Phase 1 has zero dependency on save format or .sx catalog.
2. **Reorder 0D before Phase 1** (not parallel). 0D is the critical-path gate to Phase 1 being testable.
3. **Split Phase 2 into 2A (spawn) → 2B (input).** 2A unblocks Phase 3 design; 2B can drag without blocking everything.
4. **Phase 0C can be deferred** to just-before Phase 5 prep. No dependency on having the catalog before Phase 4 ships.

---

## Engineer's concrete first-step sketch

**File:** `src/mtr-asi/src/coop/coop_spawn_probe.cpp` (new directory mirroring existing pattern).
**Header:** `include/mtr/coop_spawn_probe.h`.
**UI:** Debug-tab button in menu (later: dedicated Coop tab when more probes/features land).

**Order of operations** for the hotkey/button handler:

1. Gate on in-game screen (not main menu) via `mtr::screen_push::current_top_name`. Bail if menu-state.
2. Record transform-list count at `dword_724DE4` BEFORE.
3. Declare `void* bag = nullptr;` on the stack. Call `bag_init_from_template_THUNK(&bag, "class=protagonist; name=player2;")`.
4. Call `entity_factory_construct(0, 0, &bag, 0, 0.0f)` — try ebp=0/esi=0 first per Phase 0A analysis.
5. Record transform-list count AFTER. Log delta. Delta=1 = active path; Delta=0 = queued or null return.
6. If non-null, call `entity_kv::get(entity, "class")` and `("name")` to verify bag round-trip.
7. **SEH-wrap the entire block** — same pattern as `entity_kv::get`.

**Pre-step:** before running the probe, install a PRE-logger hook on `sub_55AF00` so we know if/when it's reached and what it receives. This is the top-candidate crash site.

**Bag is consumed by the factory** — it gets merged into the entity via `bag_merge_into(entity, bag)` at the factory's tail. Caller does NOT need to explicitly destroy. Stack handle goes out of scope normally.

---

## v2 plan downplayed concern: PRE-sim restore ordering

The v2 plan proposes adding `Mode::CLIENT` to disable PRE-sim restore for networked entities. This is necessary but **not sufficient**.

[interp_player.cpp:169-201](../../src/mtr-asi/src/interp/interp_player.cpp) and [sim_decouple_throttle.cpp:197-198](../../src/mtr-asi/src/sim_decouple/sim_decouple_throttle.cpp) run PRE-sim restore unconditionally for all entities with saved state. The shadow→engine swap (network state) ALSO fires at PRE-sim. **Both happen in `hk_sim_aggregator` at the same fence point.** Whichever runs *last* wins.

The plan needs to be explicit:

> On `Mode::CLIENT`: skip `pre_sim_restore_player` entirely for networked entities AND make the shadow→engine swap the **final step** before `g_orig_sim_aggregator(this_, 0)`.

This is an ordering-within-one-hook invariant, not just a mode-flag. It must go in the Phase 3 design, not be discovered during Phase 3 implementation.

---

## What this audit changed in the v2 plan

- **Phase 0 cap:** 3-4 weeks → **~1 week + 3 days** (after factor-by-phase revision).
- **Total project ETA:** 9-10 months → **~8 months**.
- **Sequencing:** strict 0 → 1 → 2 → parallel-after-A.
- **Phase 2 architecture:** "starts with offline RE" claim removed — entity spawn path is mod-callable today.
- **Bag wire-format:** locked at design time as template-strings + executable-hash handshake.
- **Mode::CLIENT design:** ordering invariant added (skip PRE-sim restore + shadow-swap last).

## Files

- Pre-audit checkpoint: [memory/project_state_2026-05-10_post_phase_0a_pre_audit.md](../../memory/project_state_2026-05-10_post_phase_0a_pre_audit.md).
- Phase 0A findings: [coop-phase-0a-entity-factory-2026-05-10.md](coop-phase-0a-entity-factory-2026-05-10.md).
- v2 plan: [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md).
- Next deliverable: `src/mtr-asi/src/coop/coop_spawn_probe.cpp` + `sub_55AF00` PRE-logger.
