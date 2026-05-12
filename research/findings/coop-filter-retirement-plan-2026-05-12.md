# Coop orphan filter retirement plan (Principle 4)

**Date:** 2026-05-12.
**Status:** **RETIRED 2026-05-12.** `coop_orphan_filter.{h,cpp}` deleted.
Install call removed from `dllmain.cpp`. Source dropped from
`CMakeLists.txt`. Cmdline flags `-mtrasi-coop-filter-list-walker` and
`-mtrasi-coop-filter-smart` no longer parsed (parser lived in the deleted
module). Replaced by:

- `coop_registry_mirror` (b7.13) — populates orphan's +0xCCC registry
  with 21 keys cloned from engine_wilbur. Gated by
  `-mtrasi-coop-mirror-registry`.
- `coop_vibrate_route` (C1, 2026-05-12 strategy b) — per-site PRE hook on
  `VibrateJoystick::vt[13]` @ 0x00532B40. Always-on.

**Retirement evidence (2026-05-12 morning):**
- **Soak-A** (filter+route+mirror): `frames=4550 result=pass`,
  soaked 3600 frames clean. Trace:
  `tools/test-runs/20260512-085750-load-save-1-show-ingame/`.
- **Soak-B** (route+mirror, NO filter): `frames=6649 result=pass`,
  soaked 3600 frames clean. C2/C3 did NOT surface. Trace:
  `tools/test-runs/20260512-085940-load-save-1-show-ingame/`.
- **Post-retirement** (route+mirror, filter module deleted):
  `frames=4551 result=pass`, soaked 3600 frames clean. mtr-asi.asi
  shrank 705536 → 700928 bytes. Trace:
  `tools/test-runs/20260512-090348-load-save-1-show-ingame/`.

Three-agent consensus on retirement timing (2026-05-12 morning) noted
that the prior gating criterion "orphan ticks ≥1800 frames" was being
mis-measured against scenario-total-frame-count instead of orphan-alive
frames. Extending `tick_load_save_1_show_ingame` with a `kSoakOrphan`
phase (3600 frames after probe, ~15s @ 240Hz) provided the real
sustained-tick validation.

This document is preserved as archaeology for future per-site routes
(C2, C3, ... when they surface). The retirement gating section below
is the historical decision framework; new crashes follow the same
per-crash template.

---

## Original plan (archaeology)

This document enumerated EVERY crash the filter currently masks and
assigns each one a targeted-fix strategy. Filter removal landed when
the only known crash (C1) had a proper fix shipped AND sustained
soak validation confirmed no other crashes within 3600 frames.

> Principle 4 (CLAUDE.md): *"Each NULL-deref or single-player-assumption
> gets its own patch/init/route at THAT call site. NEVER a broader 'unlink
> all candidates' mechanism."*

---

## What the filter does today

`src/mtr-asi/src/coop/coop_orphan_filter.cpp` hooks `sub_5AD9B0` (engine
list walker over `wilbur+0xD04` subscriber list) and unlinks orphan-owned
nodes before the trampoline runs, then re-links after. With smart filter +
audited allowlist (b7.10/b7.11) the breakdown is:

- 21/40 nodes left linked because their `vt[13]` is the engine no-op stub
  `0x0059AAF0` — these tick safely as ret-immediately.
- 1/40 (GroundFollower) left linked because audit-confirmed safe.
- 18/40 unlinked because their tick body would crash on the orphan.

Without the filter, the orphan tick produces at least one observed crash:

- **C1 (known)**: `sub_4CD7B0 → sub_58D330(NULL)` — render/LOD path
  dereferences `this+0x50` with `this=NULL`. Observed when filter is OFF
  + b7.13 mirror is ON.

Other crashes may surface only when C1 is fixed and a later wrapper's
tick is reached. The plan handles them in discovery order.

---

## Retirement gating

Filter can be deleted when **all** the following are true:

1. With `-mtrasi-coop-keep-orphan -mtrasi-coop-mirror-registry` set and
   NO filter flags, the orphan ticks for ≥1800 frames without crash.
2. Each crash that was masked by the filter has a documented targeted
   fix in this file with a referenced commit/build.
3. Two consecutive green sessions of (1) without regression.

Until then, the filter STAYS INSTALLED as a backstop. It's a transitional
crutch, but RULE №1 says proper retirement, not panic removal.

---

## Per-crash plan

Add entries below as crashes are observed-and-fixed. Each entry follows
the same template.

---

### C1 — `sub_58D330(NULL, "Avatar")` — **FIXED 2026-05-12 via strategy (b)**

**Status:** SHIPPED. `src/mtr-asi/src/coop/coop_vibrate_route.cpp` installs a
MinHook PRE on `VibrateJoystick::vt[13]` @ `0x00532B40`. When the wrapper's
owner (`*(this+4)`) equals `coop_spawn_probe::live_orphan_entity()`, the
hook early-returns without calling the trampoline. VibrateJoystick (vtable
`0x006C1AB0`) was promoted to `kAuditedSafeVtables` in the filter; the
filter no longer unlinks it. The `-mtrasi-coop-confirm-vibratejoystick`
diagnostic flag and its plumbing were deleted per RULE №2 (no migration
baggage).

**Verification:**
- Test with route + confirm-flag (filter let VibrateJoystick through):
  `frames=4220 result=pass` — route intercepted at call #122, owner matched
  orphan, short-circuited. Trace:
  `tools/test-runs/20260512-082741-load-save-1-show-ingame/mtr-asi.log`.
- Final baseline (route + audited-safe promotion, no diagnostic flag):
  `frames=4218 result=pass`. `filtered=17` (was 18 — VibrateJoystick now
  audited-safe), `skipped_audited=2` (was 1, now GroundFollower +
  VibrateJoystick), route fired at call #122. Trace:
  `tools/test-runs/20260512-083138-load-save-1-show-ingame/mtr-asi.log`.

**Design consensus (three agents, 2026-05-12):**
- code-architect: MTA's actual pattern is per-site re-routing, not
  fake-registration (`reference/mtasa-blue/.../multiplayer_keysync.cpp`).
- general-purpose RE: strategies (a) and (d) blocked by SecuROM
  stolen-byte thunk wall — sub_51F960 / sub_51EB40 resolve at runtime to
  DLL targets; static RE cannot reach the manager-insert API.
- code-explorer: orphan does NOT bypass any registration; both wilburs
  call sub_58CA80("Avatar") via wilbur_class_factory_alloc_ctor. The
  +0x1A8 field is a valid sequential ID (engine=0x21A, orphan=0x21B);
  difference is an unregistered intermediate id→name binding behind a
  SecuROM thunk.

**Empirical data from the diagnostic dump** (one-shot in
coop_orphan_filter, ran once, then removed per RULE №2):
- `*(engine_wilbur+0x1A8) = 0x21A`
- `*(orphan+0x1A8) = 0x21B`
- Sequential IDs assigned by the engine's allocator. Both valid.

**Original investigation (kept for archaeology):**

### C1 — `sub_58D330(NULL, "Avatar")` (wrapper confirmed, fix design open)

**Crash:** `mov esi, [ecx+0x50]` at `0x0058D331` reads from `NULL+0x50`.
`av_op=0`, `av_addr=0x00000050`.

**Lookup key:** `0x6B6AA0` = string `"Avatar"` (the Wilbur player class
name). The failed lookup is `find("Avatar")` in the name-keyed registry
owned by manager `dword_7287F0` (= global at `0x7287F0`, currently
holding `0x7295C0`).

**Confirmed call chain (2026-05-12 investigation + audit sweep):**

```
entity_manager_tick_components @ 0x5AD4D0
  → ... → sub_5AD9B0 (subscriber list walker on wilbur+0xD04)
    → VibrateJoystick::vt[13] @ 0x00532B40   (b7.10 audit row 28)
      → tick body sets ECX=dword_7287F0 (=0x7295C0), calls sub_51EB40
        → sub_51EB40 (IAT thunk at 0x51EB40, jmps via [0xF8E718])
          → IAT-resolved target (runtime value 0x10059D0 — out of image)
            → 0x4CD7E1 obfuscated wrapper (push 0x4CD7F0; push 0x58D330; ret)
              → sub_58D330 — CRASH with ECX=NULL
```

`0x4CD7E1` is an inline-fused `sub_58D330+sub_58C9C0` wrapper (the same
shape as `sub_58D370`). The push/ret 12-byte pattern is unanalyzed by
IDA. ECX gets zeroed by a defensive `xor ecx, ecx` somewhere in the
IAT-resolved target's chain — candidates `sub_567C4D` and `sub_51FF30`
share that pattern looking up `"Avatar"`, but are only applicable if
the IAT target dispatches back into Wilbur image.

**Confirmed:**
1. Crash key is `"Avatar"` (player class name).
2. Reached via `sub_5AD9B0` subscriber list walker (= orphan tick).
3. Obfuscated wrapper at `0x4CD7E1` is the immediate caller of
   `sub_58D330` (inside outer function previously labelled `sub_4CD7B0`).
4. **Subscriber wrapper is `VibrateJoystick`** (vtable `0x006C1AB0`,
   `vt[13] = 0x00532B40`). Tick body is on the stack at
   `stack[03] = 0x532B64` (return address of `call sub_51EB40` at
   body-instruction `0x532B5F`).
5. IAT slot `0xF8E718` is runtime-resolved (static value `0x10059D0`
   is outside Wilbur image).
6. **EMPIRICALLY CONFIRMED (2026-05-12 late):** Selective filter
   disable (flag `-mtrasi-coop-confirm-vibratejoystick` leaves only
   VibrateJoystick linked on the orphan) reproduces C1 with byte-exact
   stack/register match — `eip=0x0058D331`, `ecx=0`, `av_addr=0x50`,
   `stack[02]=0x6B6AA0` ("Avatar"), `stack[03]=0x00532B64`. Baseline
   run with the flag OFF stayed GREEN (`result=pass frames=951`).
   Wrapper identification verified end-to-end, not just statically.
   Trace: `tools/test-runs/20260512-073601-load-save-1-show-ingame/
   mtr-asi.log:1674-1699`. Investigation doc:
   `research/findings/coop-c1-investigation-2026-05-12.md`
   "Empirical confirmation" section.

**Still open:**
1. What writes IAT slot `0xF8E718` at runtime → resolved target.
   Cheapest approach: runtime-read `[0xF8E718]` during orphan tick.
2. Whether `dword_7287F0 = 0x7295C0` actually owns an `"Avatar"` entry
   (right-manager-missing-entry vs wrong-manager hypothesis).
3. What per-entity field differs between engine_wilbur and orphan that
   causes the defensive `xor ecx, ecx` path to fire on the orphan.
   Candidate: `*(*(this+4) + 0x1A8)` read at body-instruction `0x532B47`.
4. Construction → mirror-clone → first-tick ordering: whether the
   wrapper registers before mirror runs.

**Full investigation:** `research/findings/coop-c1-investigation-2026-05-12.md`.

**Fix strategy (wrapper now identified — VibrateJoystick `vt[13]` = `0x00532B40`):**

- **(a) Init-the-missing-data** *(preferred)*: extend `coop_registry_mirror`
  to clone the missing per-entity field on the orphan. Candidate:
  `*(*(this+4) + 0x1A8)` (read at body-instruction `0x532B47`).
- **(b) Per-site route**: hook `VibrateJoystick::vt[13]` at
  `0x00532B40` to early-return on orphan. MTA precedent:
  `multiplayer_keysync.cpp:108-187`.
- **(c) Byte patch**: byte-patch the obfuscated `0x4CD7E1` wrapper.
  Constraint: 12-byte sequence has no slack — adjacent bytes are the
  next obfuscation stage. Also suppresses legitimate Avatar-not-found
  returns on engine_wilbur, so this is NOT Principle-4-clean.
- **(d) Fix construction-mirror-ordering**: if the missing field
  exists on engine_wilbur and the issue is the orphan's first tick
  firing before mirror clones it, reorder registration vs mirror.
  Cleanest of all — handles a class of bugs.

**Out-of-scope strategies (per principle 4 — unchanged):**
- A broader subscriber filter (the workaround we're retiring).
- A general "if this == NULL, skip" hook on `sub_58D330` itself.

**Owner:** next session (deeper trace).
**Trace:** `research/findings/coop-phase0-b76-fix-trace-2026-05-11.log:1651`
and `coop-phase0-b77-bypass-trace-2026-05-11.log:1635` (VEH FAULT #2).

---

### C2 — (placeholder; surfaces after C1 is fixed)

Reserved for whatever the next crash is after C1 is fixed and filter is
flipped off again. Each new crash gets its own section.

---

## Decision log

- **2026-05-12 AM**: Tier-1 filter expansion (audited-safe allowlist
  growth) was REJECTED after 2-agent strategy audit. The allowlist is
  capped at 1 entry (GroundFollower) and not growing.
- **2026-05-12 PM**: Tier-3 registry mirror (b7.13) SHIPPED. Filter is
  now reduced to a backstop role, not a load-bearing mechanism. Mirror
  handles the +0xCCC-consumer class of crashes.
- **2026-05-12 PM**: MTA audit (full source at `reference/mtasa-blue/`)
  confirms the targeted-fix-per-site approach is the right pattern for
  retirement. This plan adopted.
- **2026-05-12 PM**: C1 identified as the only known remaining crash
  blocking retirement.
- **2026-05-12 evening**: C1 investigation session. Confirmed lookup
  key = `"Avatar"`, immediate caller = obfuscated wrapper at
  `0x4CD7E1`, chain reaches via `sub_5AD9B0` subscriber walker through
  func @`0x532B3E` (unanalyzed) and IAT thunk `sub_51EB40`. **Not
  fixed** — specific subscriber wrapper not yet identified; per
  RULE №1, broad NULL-skip on `sub_58D330` is out-of-scope. Full
  findings: `coop-c1-investigation-2026-05-12.md`.
- **2026-05-12 late (post-audit)**: 2-agent audit sweep on the
  investigation findings caught: (a) function start is `0x00532B40`
  not `0x00532B3E` (the 2 bytes between are `CC CC` filler), (b)
  the actual address `0x00532B40` IS in a vtable — row 28 of the
  b7.10 audit — meaning the subscriber wrapper is **VibrateJoystick**.
  The original `find_bytes 3E 2B 53 00` returning zero was a
  false-negative from searching 2 bytes off the real entry. Wrapper
  identification gap is now closed. Investigation doc refreshed with
  the correction and a revised next-session task list.
- **2026-05-12 (C1 SHIPPED via strategy b)**: After three-agent design
  consensus (code-architect citing MTA precedent, general-purpose RE
  hitting the SecuROM thunk wall on strategies a/d, code-explorer
  confirming both wilburs go through the same factory and registration),
  shipped `coop_vibrate_route.cpp`: PRE MinHook on `0x00532B40` that
  early-returns when `*(this+4) == orphan`. Promoted vtable `0x006C1AB0`
  to `kAuditedSafeVtables`. Deleted `-mtrasi-coop-confirm-vibratejoystick`
  flag and its plumbing per RULE №2. Two test runs:
  (1) route + confirm flag (filter let VibrateJoystick through) =
  `frames=4220 result=pass` — route caught it.
  (2) final baseline (route + audited-safe promotion, no confirm flag) =
  `frames=4218 result=pass`. C1 is structurally resolved per principle 4.
  Filter retirement now requires: gating (1) re-run with ≥1800 frames
  clean (currently 4218 ✓), gating (3) two consecutive green sessions
  (1/2 so far). Lesson: when a per-site fix is semantically correct
  ("subsystem doesn't apply to this entity"), it is NOT a RULE №1 crutch.
- **2026-05-12 late (Task 1 confirmation)**: Selective filter disable
  for VibrateJoystick shipped (~50 lines added to
  `src/mtr-asi/src/coop/coop_orphan_filter.cpp` + 1 stat in
  `coop_orphan_filter.h`). New cmdline flag
  `-mtrasi-coop-confirm-vibratejoystick`. Behavior: when the flag is
  set, the filter leaves the VibrateJoystick wrapper (vtable
  `0x006C1AB0`) linked on the orphan; all other unsafe wrappers stay
  filtered. Baseline run (flag OFF, four standard filter flags):
  GREEN `result=pass frames=951`. Confirmation run (flag ON, same
  filter set): C1 reproduces on call #1 of `sub_5AD9B0` with
  byte-exact stack/register match. VibrateJoystick is the C1
  culprit, end-to-end confirmed. Fix design (a/b/c/d) is now the
  next open decision — see investigation doc for the strategy menu.
  Trace: `tools/test-runs/20260512-073601-load-save-1-show-ingame/
  mtr-asi.log:1674-1699`.
- **2026-05-12 morning (gating audit + soak validation)**: Three-agent
  consensus caught that gating criterion (1) "orphan ticks ≥1800 frames"
  had been mis-measured against scenario-total-frame-count, not orphan-
  alive-frame-count. Both prior "green" runs (4218 + 4220 frames) only
  had ~60 frames of actual orphan ticking. Extended
  `tick_load_save_1_show_ingame` with a `kSoakOrphan` phase (3600 frames
  hold after probe spawns orphan, ~15s @ 240Hz). Three soak runs:
  (A) filter+route+mirror = `frames=4550 pass`,
  (B) route+mirror, NO filter = `frames=6649 pass`,
  (C) post-retirement (filter deleted) = `frames=4551 pass`.
  C2/C3 did NOT surface in 3600 frames of orphan ticking without the
  filter. Per RULE №1, filter retired same session.
- **2026-05-12 morning (RETIREMENT SHIPPED)**: Deleted
  `coop_orphan_filter.{h,cpp}`. Removed install from `dllmain.cpp`.
  Dropped from `CMakeLists.txt`. Cmdline flags
  `-mtrasi-coop-filter-list-walker` and `-mtrasi-coop-filter-smart` no
  longer parsed (parser was inside the deleted module). Comment
  references in `coop_spawn_probe.cpp`, `remote_player.cpp`, and
  `coop_registry_mirror.h` updated to reflect the retirement.
  mtr-asi.asi: 705536 → 700928 bytes (~4.6KB / 240 lines removed).
  Post-retirement validation: `frames=4551 result=pass` — route + mirror
  alone sustain orphan for 3600 frames. Trace:
  `tools/test-runs/20260512-090348-load-save-1-show-ingame/`.

---

## Post-retirement: where future per-site routes belong

When a new crash (C2, C3, ...) surfaces under sustained orphan ticking,
it gets its own per-site route module in `src/mtr-asi/src/coop/` named
after the engine subsystem that's misbehaving. Template:

- Hook the specific vtable slot at the specific VA.
- PRE check `owner == coop_spawn_probe::live_orphan_entity()`.
- Early-return if owner is orphan; otherwise call trampoline.
- One stats counter + one one-shot log line on first route.
- Install from `dllmain.cpp` near `coop_vibrate_route::install()`.

See `src/mtr-asi/src/coop/coop_vibrate_route.cpp` as the reference
implementation. MTA precedent: `reference/mtasa-blue/Client/multiplayer_sa/
multiplayer_keysync.cpp` (byte-patches `GetPlayerInfoForThisPlayerPed`,
`CCamera::SetNewPlayerWeaponMode`).
