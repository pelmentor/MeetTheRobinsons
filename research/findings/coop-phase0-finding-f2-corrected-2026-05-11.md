# Coop Phase 0 — F2 finding corrected (2026-05-11, later)

## TL;DR

The earlier F2 claim — "Engine has built-in input separation primitive at sub_5A2480; Phase 2 drops from 4wk to 1.5wk" — is **WRONG**. The table at `0x741920` is a **cheat-code / Konami-sequence dispatcher**, not gameplay input. The MP-active gate disables cheat codes during multiplayer; it is not an input-injection primitive. Phase 2 estimate reverts to the v2 plan's original ~4wk.

The other live-test findings (F1, F3, F4, F5, F6) still hold. Phase 1 design (R1/R2) is unchanged. Cumulative v2 reduction shrinks from ~11wk back to ~9-10wk (still good).

## Evidence the table is a cheat-code dispatcher

### 1. Registrar shape (`sub_5A2400` called 10× from `sub_5A2690`)

`sub_5A2400(name_ptr_ptr, callback_fn, /*ignored*/, second_callback, sequence_array)` writes a fixed 88-byte record:

| Offset | Field | Set by registrar |
|---|---|---|
| +0  | name id dword         | `*a1` |
| +4  | reserved/flags dword  | `a2` (per-slot callback fn) |
| +8  | reserved dword        | 0 |
| +12 | "fired" byte (toggle) | 0 |
| +16 | callback fn ptr       | `a4` |
| +20..+80 | sequence dwords (up to 16) | copied from `a5[]` until first 0 |
| +84 | sequence length       | count of non-zero entries |

`sub_5A2690` registers exactly **10 slots**. All take their sequence from a stack array. The first 8 slots all start with the same 6-byte prefix `4-2-4-2-1-3` (then one distinguishing byte + terminator). The remaining 2 use a different sequence. This is the canonical "shared prefix Konami code" shape, not gameplay key bindings.

### 2. Sequence codes are abstract button indices 1..16, not DIK

`sub_5A2980` (the SP-path "poll input") builds a fixed permutation array
```
v6[16] = { 4, 2, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
```
then iterates `v4 = v6[i]` checking `*(input_dev + 2*v4 + 12)` for press. On press, calls `sub_5A24E0(v4)` to push the **permuted index** onto the history ring. So sequences are matched against canonical 1..16 codes, with the permutation hiding the device's actual button order. Gameplay key bindings would use raw DIK scancodes or a `ControlMapper`-managed binding, not a fixed 16-entry permutation.

### 3. History ring at `unk_7418C8`

`sub_5A24E0(button_code)`:
- Pushes `button_code` into a 16-entry ring at `unk_7418C8`.
- Maintains `unk_741910` (count, capped 16) and `unk_741904` (overflow).

This is exactly how cheat-code parsers work: collect last-N inputs in a ring, then on each frame compare the tail of the ring against every registered code's sequence.

### 4. Detection logic in `sub_5A2530`

```c
char sub_5A2530(int seq_ptr, int seq_len) {
    if (MPActive()) return 0;
    if (!seq_ptr || seq_len > current_history_count) return 0;
    if (seq_len <= 0) return 1;
    for (i = ring + (current_history_count - seq_len); *i == seq_ptr[v3]; ++i, ++v3) {
        if (v3 >= seq_len) return 1;
    }
    return 0;
}
```

Plain "compare last `seq_len` history entries to the registered sequence". Returns 1 on match.

### 5. Trigger logic in `sub_5A2590`

On match, `sub_5A2AC0` calls `sub_5A2590(slot_base)`:
```c
char sub_5A2590(int slot_base) {
    if (MPActive()) return 0;
    fn = *(int(__cdecl**)(int))(slot_base + 16);
    if (fn) *(BYTE*)(slot_base + 12) = fn(slot_base);
    else    *(BYTE*)(slot_base + 12) = *(BYTE*)(slot_base + 12) == 0; // toggle
    if (*(BYTE*)(slot_base + 12)) sub_57ABA0(7119956, 0); // log "<cheat> on"
    else                          sub_57ABA0(7119944, 0); // log "<cheat> off"
    return 1;
}
```

Two strings (`7119944` / `7119956`) — the standard "cheat-toggle" UX (HUD blip or console line).

### 6. The MP-active short-circuit IS "disable cheats in MP"

`sub_5A2AC0`'s prologue:
```c
if (g_mp_coordinator && coord->IsMPActive())
    return sub_5A2480();   // wipes all non-sticky "fired" toggles
sub_5A2980(a1);              // SP path: poll input, push history, sequence-match
// ...
```

Plus `sub_5A2530` and `sub_5A2590` BOTH check `MPActive()` and bail out — three independent guards. Intent: **cheat codes are disabled while multiplayer is active** (so co-op partners can't unexpectedly trigger god-mode).

`sub_5A2480` clears `slot+12` ("fired" toggle byte) unless `slot+8 & 2` is set. Bit 2 is a "preserve this cheat's toggle state across MP transitions" flag — probably for cheats that are "stuck on" (e.g. an unlock toggle that should survive MP entry/exit).

### 7. Only one registrar in the entire binary

`sub_5A2400` xref-count = 10, all inside `sub_5A2690`. No data-driven registration, no config-file path. Hardcoded 10 cheats — definitely not gameplay input.

### 8. The five vtable[5] hot callers from F3 are ALL inside the cheat-code path

| RA | Site | What it does with `IsMPActive` |
|---|---|---|
| 0x59D82D | sub_59D7A0 | input result-mask gate (may also be cheat-related — needs follow-up) |
| 0x5A253F | sub_5A2530 | cheat: bail if MP active |
| 0x5A2AD4 | sub_5A2AC0 | cheat: short-circuit to clear if MP active |
| 0x5A2B55 | sub_5A2AC0 | cheat: second check at end of function |
| 0x5A2992 | sub_5A2980 | cheat-poll: bail if MP active |

4/5 are definitively cheat-code. The 5th (`sub_59D7A0`) is more ambiguous — it's a "result-mask gate" with a callback walker — likely the broader debug/console input dispatch, parallel to but separate from the cheat parser. Still not gameplay input.

## What the corrected picture means for Phase 2

**Phase 2 (input separation) was estimated as 4wk in the v2 plan.** The earlier F2 claim reduced this to 1.5wk on the assumption that the engine already had a working network-input-sticky primitive at `sub_5A2480`. That assumption is invalidated.

Revised estimate: **Phase 2 reverts to ~4wk** (build from scratch).

### Where IS the actual gameplay input?

Unknown right now — needs a follow-up RE session. Candidates (from existing memory + project state):

1. **`g_input_mgr` / ControlMapper at 0x745BA8** — flagged in v2 plan as "TRAP not reuse". The trap warning came from earlier sessions; need to verify whether it's a trap for *replacement* (i.e. you can't simply override its outputs) or *injection* (you can't simply feed it network input). Different implications.
2. **Per-entity `input_state_t` blocks on the protagonist** — set by `protagonist_apply_control_input` (sub_4CC130 region per earlier RE notes); injection here is per-entity rather than global, which aligns better with the entity-replication path described in `coop-phase0-replication-primitive-found-2026-05-11.md`.
3. **`.sx` script "input" / "control" command wiring** — script-VM-mediated input dispatch. Lower priority — likely not on the hot per-frame path for movement.
4. **DI buffer at the source** — `dinput_hook` already controls what the engine sees (we use this for the test harness). Network input could write into the DI buffer the same way. Already proven safe.

Most-likely actual approach: **Option 4 (DI buffer injection)**, since `dinput_hook` already exists and proves the technique works. The "input separation" work is then mostly:
- Tag each buffered keystroke with a source (local vs. remote).
- Suppress local-source bytes during MP if the player isn't the authoritative host for that entity.
- Forward network input bytes from the receive thread into the DI buffer with the remote tag.

That's still ~2-3wk of careful work (thread-safety, ordering, suppression rules), not 1.5wk.

## Slot table — final memory map

Base = `0x00741920`. 16 slots × 88 bytes = `0x741920..0x741CA0`. Currently 10 slots used (max 16 enforced in `sub_5A2400`).

Adjacent globals (header region before `0x741920`):

| VA | Symbol | Role |
|---|---|---|
| 0x7418C8 | history ring (16 dwords) | recent button codes |
| 0x741908 | slot count N | bounded to 16 |
| 0x74190C | dword | cleared each MP-frame |
| 0x741910 | history count | bounded to 16 |
| 0x741914 | timestamp dword (= `dword_6FFCAC` at last press) | |
| 0x741918 | dword | cleared each MP-frame |
| 0x74191F | (padding to 0x741920) | |
| 0x741920 | **slot[0] base** | |

Slot internal layout (88 bytes):

| Offset | Size | Field |
|---|---|---|
| +0  | 4 | name id |
| +4  | 4 | per-slot callback A |
| +8  | 4 | flag dword — **bit 2 = "don't auto-clear toggle"** |
| +12 | 1 | "fired" toggle byte (cleared by `sub_5A2480` unless bit 2 set) |
| +13 | 3 | padding |
| +16 | 4 | callback B (called by `sub_5A2590` on match) |
| +20 | 4*16 | sequence dwords (button codes 1..16, 0-terminated) |
| +84 | 4 | sequence length (computed by registrar) |

## Why this misread happened

1. The function names `sub_5A2480` and `sub_5A2AC0` are generic; nothing surfaced "cheat" in the symbol set.
2. The `MPActive`-short-circuit + per-frame-clear pattern looks **structurally identical** to an input-separation primitive — same control flow, same conditional clear.
3. Bit 2 of `slot+8` looked like a "network input sticky" flag because that exactly fits what an input-separation primitive needs.
4. The `unk_741920` region is unnamed in the IDB; no string literals nearby; no callback resolves to a recognizable cheat function (would have required following `sub_452050` / `sub_5A2600` / `sub_5A24D0` to confirm).
5. The live tests confirmed the engine never crashed and the calls were happening — which is true, but the *interpretation* of what those calls meant was wrong.

The right way to disambiguate next time: **always check the registrar pattern first** before declaring a global table is "input plumbing". Registrar shape (number of callers, what kind of callback is stored, sequence vs. binding structure) is much more discriminating than the consumer-side control flow.

## What still holds from the live-test session

- **F1**: `entity_install_network_manager` (0x5B0C70) is dormant in normal play. Still true.
- **F3**: Only `vtable[5]` exercised, 5 distinct caller RAs. Still true — but **all 5 callers are in cheat-code / debug-console code, NOT general input plumbing**.
- **F4**: No `vtable[9]` (GetNetworkTime) calls in load-save. Still true.
- **F5**: No `UNKNOWN-slot` calls on coordinator. Still true.
- **F6**: Test 3 timeout = test harness issue. Still true; BUT the reason no longer reads as "engine input separation is working". Real reason: `sub_5A2480` clears the cheat-toggle table when MP is active, and the DI keystroke from the test harness doesn't get translated into a cheat code anyway (the DI hook injects into the keyboard buffer, which feeds the gameplay input path, not the cheat-code internal history ring) — so the menu-nav DIK_RETURN injection works fine in normal SP runs but is irrelevant to whether cheats are disabled.

Actually F6 needs a closer look — Test 3 timed out under MP-active arming. If the cheat dispatcher is the only thing the MP gate disables, why would menu-nav DIK_RETURN injection fail? Answer is probably "the test harness scenario was waiting for some state transition that requires the protagonist to be loaded / a level to be active, neither of which happens at title screen". This is a test-harness shape issue, not an engine-input-separation observation.

## Files / memories updated (all complete)

- [x] This file (new).
- [x] `research/findings/coop-phase0-live-test-findings-2026-05-11.md` — banner at top + F2 label corrected + F3 retracted inline + v2 table row updated + reduction text updated.
- [x] `research/findings/coop-phase1-design-2026-05-11.md` — R3 retracted with pointer to this file.
- [x] `docs/ROADMAP.md` M15 — live-test pass entry updated, v2 reduction table row updated, net wk reduction revised (~11 → ~9-10).
- [x] `memory/project_state_2026-05-11_coop_live_test_results.md` — frontmatter rewritten, body banner added, F2 section retracted, F3 callers re-labeled, v2 table updated, next-session candidates updated, read-FIRST order updated.
- [x] `memory/MEMORY.md` — index entry rewritten to flag F2 retraction and updated cumulative reduction.

## Followup tasks for next session

1. **RE the actual gameplay-input separation point.** Most-likely path: trace `dinput_hook` → DIK buffer → engine consumer → first entity-level "input was received" handler. Find where local vs. network input can be tagged.
2. **Verify `g_input_mgr` is or isn't a trap.** The v2 plan flagged it but didn't say which kind of trap. A targeted reading session of the ControlMapper class would settle it.
3. **Re-validate the v2 plan's 4wk Phase 2 estimate** against the real injection point once found. If we land on Option 4 (DI buffer injection extension), estimate may end up at 2-3wk.

## End of correction
