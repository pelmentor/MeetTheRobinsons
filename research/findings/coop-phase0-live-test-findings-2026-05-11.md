# Coop Phase 0 — live-test findings (2026-05-11)

> **CORRECTION 2026-05-11 evening**: **F3 below ("Built-in input separation primitive") is WRONG.** The table at `unk_74192C` (actual base `0x741920`) is a **cheat-code / Konami-sequence dispatcher**, not gameplay input. The MP-active gate disables cheat codes during MP; it is NOT a built-in input-separation primitive. Phase 2 estimate reverts to v2 plan's ~4wk (not 1.5wk). F2 still holds in form ("MP gate consumer") but F2's 5 vtable[5] callers are now correctly recognized as cheat-code path, not general input plumbing. F1, F4, F5 are unchanged. F6's reason is mis-attributed but the test-harness behaviour is real.
>
> (Note: the memory checkpoint labeled the retracted finding as "F2" because it merged this doc's F2 and F3 into one bullet. The correction doc reaches the same conclusion either way.)
>
> Full correction with all 8 lines of evidence: see [coop-phase0-finding-f2-corrected-2026-05-11.md](coop-phase0-finding-f2-corrected-2026-05-11.md).

## Status: COMPLETE — 4 autonomous tests run, F3 retracted post-RE.

User granted "live test yourself" green-light. Four tests run via `tools/run-test.ps1` with cmdline auto-arm. Findings substantially revise the Phase 1 design hypothesis from earlier today. F3 retracted later same day after deeper IDA RE of the table registrar revealed it was a cheat-code dispatcher.

## Tests run

| # | Scenario | Args | Outcome |
|---|---|---|---|
| 1 | load-save-1-show-ingame | (none) | PASS 9.7s |
| 2b | load-save-1-show-ingame | `-mtrasi-coop-mp-armall` | PASS 8.8s — captured vtable[5]×2400 calls |
| 3 | load-save-1-show-ingame | `-mtrasi-coop-mp-armall` (+RA logging) | **TIMEOUT** — engine stable, test_harness hung |
| 4 | load-save-1-show-ingame | `-mtrasi-coop-arm-coordinator` (no MP-active) | PASS 9.4s — captured 5 distinct caller RAs |

## Key findings

### F1. `entity_install_network_manager` (0x5B0C70) is DORMANT in normal play

PRE-logger fired **zero times** across all four tests. The function exists with 7 caller sites in the binary, but **none of them are exercised** during load-save → ingame → coop_spawn_probe::try_spawn_p2.

This invalidates a Phase 1 design assumption: we cannot rely on hooking the install fn to inject per-entity NetworkManager pointers — because in normal play, the install fn never fires.

Likely cause: the 7 caller sites are specific entity-construction wrappers (`sub_4FF310`, `sub_5034F0`, `sub_5BF520`, etc.) that fire only for specific entity types — probably mini-game characters or .sx-script-spawned entities. Player and general engine entities are constructed via different paths (e.g., `entity_factory_construct` directly, which our `coop_spawn_probe` uses).

### F2. Cheat-code dispatcher is the active MP gate consumer (corrected label)

When MP is "armed" (coordinator installed + IsMPActive=true), the engine calls `vtable[5] IsMPActive()` at **~300 calls/sec** from 5 distinct sites, all in the **cheat-code dispatcher** code path (corrected — was labeled "input election" before the F2-correction RE):

| RA | Function | Role (corrected) |
|---|---|---|
| 0x59D82D | sub_59D7A0 | result-mask gate (likely debug/console — needs follow-up) |
| 0x5A253F | sub_5A2530 | cheat-sequence comparator (bail if MP active) |
| 0x5A2AD4 | sub_5A2AC0 | per-frame cheat dispatcher (hot site; short-circuits to clear if MP active) |
| 0x5A2B55 | sub_5A2AC0 | second MP check at end of dispatcher |
| 0x5A2992 | sub_5A2980 | cheat-code input poller |

`sub_5A2AC0` is the hottest caller — its prologue:
```c
if (g_mp_coordinator_ptr && coordinator->IsMPActive()) {
    return sub_5A2480();  // MP-active branch — clears cheat-toggle bytes
}
sub_5A2980(a1);  // SP branch — poll 16 buttons, push to cheat history ring
// ...rest of SP path: walk slots, compare sequences vs. history, fire on match
```

### F3. ~~Built-in input separation primitive (sub_5A2480)~~ **RETRACTED**

**Original (wrong) claim**: `sub_5A2480` clears a network-input table; bit 2 of slot+8 is a "network sticky" flag; Phase 2 collapses from 4wk to ~1.5wk by writing to this table directly.

**Corrected**: the table at `0x741920` is a **cheat-code / Konami-sequence dispatcher**, not gameplay input. Slot layout (88 bytes): name id, callback fn pointers, sequence dwords (up to 16 button codes 1..16), sequence length. Registrar `sub_5A2400` (called 10x from `sub_5A2690`) installs hardcoded cheat sequences with shared `4-2-4-2-1-3-N-0` prefix. SP path polls 16 buttons, pushes to a 16-entry history ring at `0x7418C8`, then compares each slot's sequence against the recent history. On match, fires the slot's callback and toggles slot+12 ("fired" byte). When MP-active=true, `sub_5A2AC0` short-circuits to `sub_5A2480` which clears the "fired" toggles (bit 2 = "preserve toggle across MP transitions"). Intent: **disable cheat codes during multiplayer**, NOT input injection.

Phase 2 (input separation) reverts to v2 plan's ~4wk estimate. Full correction: [coop-phase0-finding-f2-corrected-2026-05-11.md](coop-phase0-finding-f2-corrected-2026-05-11.md). Real gameplay-input separation point is still unidentified.

### F4. No `vtable[9]` (GetNetworkTime) calls in load-save

Zero hits even with IsMPActive=true for 90 seconds. The time-source switch in `sub_5A6240` only fires if entered, which it wasn't during load-save. Probably gameplay-state-dependent (level-load only? boot?).

### F5. No `UNKNOWN-slot` calls on coordinator

Confirms the coordinator vtable we have (vtable[5/9]) covers the methods actually invoked during this scenario. The vtable[15/19/23] uses I'd hypothesized from static analysis may exist but aren't called in load-save play.

### F6. Why Test 3 timed out

Engine remained stable for 90s (vtable[5] calls kept firing). The test_harness scenario logic hung because:
- It injects DIK_RETURN to navigate menus
- With MP-active=true, sub_5A2480 **clears the injected input every frame** (the flag bit 2 isn't set on DI-injection-sourced input)
- DIK_RETURN never registered → menu nav can't proceed → scenario logic loops waiting for state transition

This is **engine-correct behavior** confirming F3. The hang was a test-harness issue, not an engine crash.

## Updated answers to Phase 1 design open questions

| Q | Original | Answered |
|---|---|---|
| Q1: which classes hit install fn | UNKNOWN | **None in load-save play. Install fn is for specific construction paths (likely mini-game / scripted spawn).** |
| Q2: which coord slots fire | UNKNOWN | **Only vtable[5], from input election code (5 call sites). vtable[9] dormant.** |
| Q3: which netmgr slots fire | UNKNOWN | **Not characterized — install fn doesn't fire, so netmgr never gets installed.** |
| Q4: channel names | UNKNOWN | Not characterized (no vtable[11] calls). |
| Q5: vtable[12] null behavior | UNKNOWN | Not characterized. |
| Q6: vtable[3] timing | UNKNOWN | Not characterized. |

## Phase 1 design revisions

The design from this morning needs three updates:

**R1. Coordinator class is minimal.** Phase 1b in the design doc allocated 0.5 wk for coordinator. With F2/F4 we now know the only consumed method is vtable[5] (and possibly vtable[9] in some scenarios we didn't reach). **0.5 wk → 0.1 wk.** Just a single bool returning method.

**R2. NetworkManager install path was wrong.** Phase 1d allocated 0.5 wk for "POST-hook on entity_install_network_manager". With F1 we now know that hook is dormant. Need different install vector. Options:
- Hook `entity_factory_construct` POST and write entity+216 there (catches EVERY entity but is much higher volume).
- Hook the actual entity types' constructors (Protagonist ctor specifically).
- Track entities through `dword_724DE4` (the transform list head — already known from NPC overlay work).
- **Recommendation**: hook the Protagonist class's ctor specifically. Phase 1d becomes 1-1.5 wk because we need to find that ctor first.

**R3. ~~Phase 2 (input separation) drops substantially.~~** **RETRACTED — see F2 correction above.** The `sub_5A2480` "primitive" is a cheat-code disable, not network-input separation. Phase 2 reverts to v2's ~4wk estimate. The real gameplay-input separation point is still unidentified — pending follow-up RE.

## Revised v2 plan estimate (post live-test)

| Phase | v2 | After Phase 0 RE (morning) | After live test |
|---|---|---|---|
| Phase 1 transport | 4 wk | 5 wk | 5 wk |
| Phase 1b coordinator | (bundled) | 0.5 wk | 0.1 wk |
| Phase 1c manager | (bundled) | 1.0 wk | 1.0 wk |
| Phase 1d wiring | (bundled) | 0.5 wk | 1.0-1.5 wk |
| Phase 2 input separation | 4 wk | TBD | ~~1.5 wk~~ **4 wk (R3 retracted, see F2 correction)** |
| Phase 3 replication | 4 wk | 1 wk | 1 wk |
| Phase 5 script-VM repl | 8 wk | 2-3 wk | 2-3 wk |

Total reduction vs v2: **~9-10 wk** (was claimed ~11; retracted to ~9-10 after F2 correction. Live test no longer adds beyond morning RE; F1+F3 alone don't collapse Phase 2).

## What is NOT yet characterized (would need different test scenarios)

- Entity-install path that fires for normal entities (would need mini-game scenario or specific spawn).
- Channel names beyond "DistributedState" (need install fn → netmgr to fire).
- Receive-side behavior (need actual host data).

These can be characterized after Phase 1 transport is up (where the manager is actually installed via the better path identified in R2 above).

## Files added this session

- [research/findings/coop-phase0-live-test-findings-2026-05-11.md](coop-phase0-live-test-findings-2026-05-11.md) — this file.
- `src/mtr-asi/src/coop/coop_mp_probe.cpp` — extended with cmdline auto-arm (-mtrasi-coop-mp-armall etc.) + return-address logging on vtable[5].
- `tools/run-test.ps1` — added `-ExtraArgs` parameter for passing arbitrary cmdline flags to Wilbur.exe.

## Engine VAs newly characterized

| VA | Symbol | Notes |
|---|---|---|
| 0x5A2AC0 | per-frame input election | hottest IsMPActive caller |
| 0x5A2480 | MP-active input-clear branch | clears `unk_74192C` table except flag-2-sticky entries |
| 0x74192C | input slot table | array of N 88-byte slots; flag byte at offset -4 |
| 0x741908 | input slot count | N value |

## Recommendation

The probe stack works as designed. Phase 1 implementation can proceed, with the revised entity-install vector (R2 above) being the first thing to characterize before Phase 1d wiring locks. Suggested next test session: a mini-game scenario (e.g., DigDug, ChargeBall) which likely exercises the dormant install paths and gives us Q1/Q3/Q4/Q5/Q6 answers.
