# Coop Phase 1.6 — Per-player input routing (Steps 1+2+4+5 SHIPPED 2026-05-12)

Predecessor: [coop-phase-1-4d-auto-spawn-2026-05-12.md](coop-phase-1-4d-auto-spawn-2026-05-12.md).
Successor (next session): close the dev_p2 shared-subdevice gap (audit-flagged 88%), then run user-driven LAN dual-launch live test.

## TL;DR

The MTA SwitchContext analogue lands as a `dev`-pointer swap inside
ControlMapper. Two new modules under `src/coop/input/`:

- `controlmapper_dev` — PRE-hooks engine's ControlMapper::Tick at
  vtable[3] (function pointer read from `[0x006A639C+12]`), captures the
  singleton instance + `dev_p1 = *(instance+4)`, dumps the dev layout
  for inspection, then `malloc(4096) + memcpy(dev_p2, dev_p1, 4096)`
  to clone an idle-state device. Exposes `swap_to_player(int idx)` that
  writes `*(instance+4) = (idx==0 ? dev_p1 : dev_p2)` under SEH guard.
- `per_entity_tick_hook` — PRE/POST hooks `sub_5AD9B0` at `0x005AD9B0`
  (the wilbur+0xD04 subscriber-list walker, IDA-verified hookable with
  8 clean prologue bytes, `__thiscall(wilbur*)`, single caller at
  `0x00552500`). PRE: `idx = protagonist_registry::player_idx_for(ECX)`,
  if `idx >= 0` calls `swap_to_player(idx)`. POST inside `__finally`:
  `swap_to_player(0)` to restore — but only if PRE actually swapped
  (idx >= 0). Audit-fix-applied design.

Result: 4 of 6 design steps shipped. Autonomous test passes through 7819
frames with full Phase 1.6 path active in single-player mode (P1 only,
since `protagonist_registry` registers no second wilbur in autonomous
test). Phase 1.6 end-to-end correctness (P2 path actually exercised)
is **blocked on the shared-subdevice gap below** — to be fixed before
LAN dual-launch live test.

## 3-agent design consensus

Architect (Option C — dev-pointer swap, third option beyond research-
doc's Option A/B), code-reviewer (RULE №1/№2 + 7-principles cross-
check), MTA-precedent explorer (verified `SwitchContext`/
`ReturnContextToLocalPlayer` + `CClientPed::m_bIsLocalPlayer` branch).

The architect drifted on one factual claim (`coop_vibrate_route.cpp`
"confirms `sub_5AD9B0`" was wrong — that file hooks
`0x00532B40` `VibrateJoystick::vt[13]`, not `sub_5AD9B0`). IDA
verification before Step 4 caught it and confirmed `sub_5AD9B0` is
indeed the right hook site independently.

### Option C — pointer-swap, not state-copy

MTA's `multiplayer_keysync.cpp:325` does `MemCpyFast(pLocalPadInterface,
&data->m_pad, sizeof(CPadSAInterface))` — a per-tick struct copy from
the per-ped buffer into the engine's singleton pad. We chose to swap
the `dev` pointer instead: cheaper, equivalent for the
"consumer-sees-player-N's-input" invariant. Trade-off documented in
the audit (Q1) — both achieve the same effect; MTA's copy lets
engine writes during ProcessControl flow back into the per-ped
buffer (via `Restore`), ours doesn't (writes land in dev_p2 and stay
there). Fine for MVP since dev_p2 is frozen-idle.

### MTA precedent verified

- `multiplayer_keysync.cpp:325` — SwitchContext memcpy.
- `multiplayer_keysync.cpp:584` — Restore inside the ProcessControl
  hook (POST), with abort path at :595-624.
- `CRemoteDataSA.h:54` — `CPadSAInterface m_pad;` value-typed (key
  divergence — see Gap below).
- `CClientPed.cpp:876, 938` — `if (m_bIsLocalPlayer)` branch driving
  local vs remote pad reads.

## Build sequence + per-step verification

| Step | Shipped | Verified | Artifact |
|---|---|---|---|
| 1 | ✓ | autonomous test pass, 4550 frames | `[cm_dev] CAPTURED instance=0x10DA6120 vt=0x006A639C dev_p1=0x1174B9E0` + dev_p1 layout dump (77 non-zero dwords in 512 B) |
| IDA verify of sub_5AD9B0 | ✓ | decompile + disasm | __thiscall(wilbur*), 8 hookable bytes, single caller 0x00552500 |
| 4 | ✓ | autonomous test pass, 7823 frames | `[petk] entity=0x10F930B0 idx=0 (P1) fire#1..3600` heartbeats; 5 early `idx=-1` non-wilbur passes |
| 2 | ✓ | autonomous test pass | `[cm_dev] dev_p2 cloned: addr=0x1AD14C58 size=4096` |
| 5 | ✓ | autonomous test pass, 7819 frames | `swap_to_player(0)` PRE+POST fires for 3600 P1 ticks, zero crash |
| Audit-fix | ✓ | autonomous test pass, 4551 frames | POST gated on idx>=0; `__try/__finally` around trampoline call |

## 2-agent audit findings (reviewer + MTA-domain-fidelity)

### CRITICAL — shared-subdevice gap (88%, both auditors converged)

`dev_p1` contains heap pointers at multiple offsets after +0x144 (visible
in the layout dump: high bytes `0x11`/`0x10` = typical heap-allocation
range). `memcpy(dev_p2, dev_p1, 4096)` is a shallow clone — those
pointer fields in dev_p2 still point into dev_p1's subdevice objects.
When the swap is active for P2 and a ControlMapper consumer chases one
of those pointers, they read **live dev_p1 state** through the shared
subdevice → P2 sees P1's input → swap defeated.

The autonomous test masked this because `protagonist_registry` registers
no second wilbur in single-player mode (no `idx=1` fires logged, so
`swap_to_player(1)` is never exercised). LAN dual-launch live test will
surface this immediately.

MTA's `m_pad` is value-typed (no shared subdevices — see
`CRemoteDataSA.h:54`). This is the principle-7-boundary
clarification owed before declaring Phase 1.6 done.

**Two fix options (next session)**:

(a) **Recursive clone** — heap-allocate + memcpy each subdevice pointed
to by dev_p1, recurse for nested pointers. Most faithful to MTA's
value-typed isolation.

(b) **Zero per-frame-state offsets** — leave subdevices shared but
zero the button-ring + analog offsets in dev_p2 so ControlMapper
consumers reading from dev_p2 directly return zero. Requires
RE'ing exact button-ring offsets. Cheaper, but only works for
consumers that don't chase subdevice pointers.

Recommendation: try (a) first; fall back to (b) only if (a) is
intractable.

### Applied this session

**Code-reviewer #1 (82%) + MTA #6**: Unconditional POST
`swap_to_player(0)` writes `instance+4 = dev_p1` even when PRE didn't
swap (idx < 0). Benign today (idempotent write), but design-fragile
against future re-entrancy. **Fixed**: POST gated on `idx >= 0` to
match PRE — non-wilbur entities never spuriously write `instance+4`.

**MTA #5 (medium)**: No SEH around the trampoline call. If `g_tramp`
faults (SEH), `instance+4` stays pinned to `dev_p2` forever — next
ControlMapper consumer reads garbage. MTA wraps its abort path at
`multiplayer_keysync.cpp:595-624`. **Fixed**: `__try/__finally` around
the trampoline call; POST restore runs even on SEH unwind.

### Documented as design debt (no code change)

**MTA #3 (Phase 2.0+)**: dev_p2 is cloned ONCE at first-Tick and never
refreshed. Fine for Phase 1.6 (pose-only replication, frozen-idle is
intentional). Phase 2.0+ network input replication needs a
`controlmapper_dev::write_p2_state(...)` API that the network thread
calls each time a remote input packet lands. Without it, stuck-button
bugs on disconnect/state-change are guaranteed. Design owed during
Phase 2.0 planning while this layer is fresh.

**MTA #2 (Phase 2.x)**: Camera rotation, gravity, etc. — GTA SA's
SwitchContext swaps multiple pieces. MTR camera is per-player too but
out of Phase 1.6 scope. Flag for Phase 2.x once we sync remote camera.

**Code-reviewer #3 (80%, benign)**: `static bool installed`
idempotency guard is non-atomic; benign because dllmain calls all
installs sequentially. Document; do not refactor.

## Files shipped

### Created

- `src/mtr-asi/include/mtr/coop/input/controlmapper_dev.h` (~70 lines)
- `src/mtr-asi/src/coop/input/controlmapper_dev.cpp` (~240 lines)
- `src/mtr-asi/include/mtr/coop/input/per_entity_tick_hook.h` (~40 lines)
- `src/mtr-asi/src/coop/input/per_entity_tick_hook.cpp` (~120 lines)

### Modified

- `src/mtr-asi/CMakeLists.txt` — two new source files under
  `src/coop/input/`.
- `src/mtr-asi/src/dllmain.cpp` — two new install calls, ordered:
  `controlmapper_dev::install()` after `coop_spawn_probe::install()`
  (capture path), and `per_entity_tick_hook::install()` AFTER
  `protagonist_registry::install()` (so `player_idx_for` is wired
  before the first walker fire). (Updated 2026-05-12 post-retirement:
  `controlmapper_vtable_probe::install()` previously sat between
  `coop_spawn_probe::install()` and `controlmapper_dev::install()`;
  retired alongside `coop_component_registry`, see end of this doc.)

## Cmdline contract (post-1.6 Step 1+2+4+5)

Unchanged from 1.4d. No new flags. The Phase 1.6 modules are
unconditionally enabled — there is no opt-out, by design (RULE №2:
no "disable new thing to fall back to old thing" flags).

## Build state

After audit-fix: `mtr-asi.asi = 715776 B`. Both Release builds clean
with zero warnings. Single-player autonomous test passes through
3600-frame orphan soak ("route+mirror held without crash") with the
full Phase 1.6 path active.

## Phase 1.6 gap-close (SHIPPED 2026-05-12 EOD, this session)

The shared-subdevice gap (88% audit finding) is **CLOSED**. The fix
diverges from the audit's recommendation of "recursive clone": new RE
this session showed the audit was conservatively right (shallow memcpy
preserves heap pointers) but the actual leak vector is narrower than
"chase the subdevice pointers". Real read paths read per-frame state
DIRECTLY from dev offsets, never via the +0x148+ descriptor pointers.

### RE finding — dev struct per-frame layout (IDA-verified 2026-05-12)

| Offset | Bytes | Purpose | Read by | Write by |
|---|---|---|---|---|
| `+0x00` | 4 | vtable ptr (0x006C80B8, 20 slots — 5 stubs / 5 active) | engine indirect | engine init |
| `+0x0C..0x2F` | 36 | 18 button-state PAIRS (prev,curr) | CM::vt[5] `*(dev + 12 + 2*idx)` | sub_572340 byte-pair writes |
| `+0x30..0x33` | 4 | analog "slot 0" — engine never reads (v4=0 not in mapping) | (none observed) | (none observed) |
| `+0x34..0x43` | 16 | 4 analog axes (v4=1..4) | CM::vt[4] GetAnalog `*(dev + 48 + 4*v4)` | sub_572340 writes +0x34,+0x38,+0x3C,+0x40 |
| `+0x44..0x7F` | 60 | sparse — only +0x40 had non-zero clone-time | (none observed) | (none observed) |
| `+0x80, +0x8C, +0x98` | 4 ea | -1.0f sentinels (likely analog calibration mins) | unconfirmed | (none observed) |
| `+0xA8` | 4 | 0x04000100 — likely device-class metadata | unconfirmed | (none observed) |
| `+0xB4` | 4 | mode_flag DWORD — enables synthetic-fallback path | Tick_body `*(_DWORD*)(dev+180)` | engine init |
| `+0xC0..0x130` | 116 | 21-entry DI scancode keymap (immutable per dev instance) | sub_572340 reads as scancodes | engine init |
| `+0x148..0x208` | 192 | 10-entry × 20-byte subdevice descriptor array | (none observed reading the heap pointers as state) | engine init |
| `+0xCC` | 4 | pointer to GLOBAL static subdevice (0x0071AD1C, vtable 0x006C8368) | sub_572340 calls vt[8] | engine init |

### Fix — Option (b') zero-state shim with leak-blocking

After `memcpy(dev_p2, dev_p1, 4096)`:
```cpp
std::memset(p2_bytes + 0x0C, 0, 0x38);     // buttons + analog (0x0C..0x43)
*reinterpret_cast<uint32_t*>(p2_bytes + 0xB4) = 0;  // mode_flag dword
```

Rationale for option (b') over the audit-recommended (a) recursive clone:
- The subdevice at dev+0xCC is a static global (0x0071AD1C, NOT heap),
  so "recursive clone" is wrong in principle — you'd get two wrappers
  pointing at the same hardware device.
- The +0x148+ descriptor pointers, while heap-allocated, are not chased
  by any RE'd consumer for per-frame state. They are configuration
  metadata.
- The actual per-frame leak vector is dev+0x0C..0x43 (buttons + analog
  read directly), which the zero-shim addresses head-on.
- The mode_flag at dev+0xB4 is a separate leak vector: when set, the
  synthetic-fallback functions (sub_41A620 / sub_41A5E0) inject input
  from a non-dev source. Zeroing the dword (NOT a byte — width matters
  per the audit) disables the fallback for the P2 tick window.

### Empirical verification — sub_572340 probe

A PRE-hook on the dev poll-and-write function (0x00572340) was installed
to verify the engine does NOT poll dev_p2. Result over 3600+ poll
invocations (76-second autonomous test):

- `fires_p1 = 3600+` (every poll is against dev_p1)
- `fires_p2 = 0`
- `fires_other = 0`

The probe is preserved as permanent observability infra for Phase 2.0
(will alert if any future engine path begins polling dev_p2 — that would
break the design contract). Hot path is one atomic load + one branch
+ one fetch_add; negligible perf cost.

### `write_p2_state(P2InputState)` — Phase 2.0 forward-compat API

A field-targeted writer for the network thread to call when remote P2
input arrives. Replicates the engine's prev/curr cycle for the 18
button pairs, then writes 4 floats at dev_p2+0x34..+0x43 (mirroring
sub_572340's exact write set).

Trade-off vs MTA: MTA's `MemCpyFast(pLocalPadInterface, &data->m_pad,
sizeof(CPadSAInterface))` at multiplayer_keysync.cpp:325 copies the
entire struct. Field-targeted writes are cheaper, but the wire format
must match the field set the engine actually reads. The IDA decompile
pinned this down: 18 button "curr" bytes + 4 analog floats.

**Phase 2.0 threading caveat** (MTA-fidelity audit, 70% concerning):
the current `write_p2_state` doc says "torn read recovers next snapshot"
which is MVP-acceptable but diverges from MTA's "all writes on main
thread inside ProcessControl hook" pattern. Before wire format ships,
decide: (a) spinlock + per-write barrier, (b) main-thread-deferred
queue (MTA-style snapshot per frame). NOT a blocker for Phase 1.6.

### Files changed this session

- `src/mtr-asi/include/mtr/coop/input/controlmapper_dev.h` — expanded
  API: `PollProbeStats poll_probe_stats()`, `P2InputState` struct,
  `void write_p2_state(const P2InputState& s)`. Module-level doc
  rewritten.
- `src/mtr-asi/src/coop/input/controlmapper_dev.cpp` — zero-state
  shim in `try_capture`, poll-fn probe (`hook_poll`, `install_poll_probe`),
  public API impls. ~120 net lines added.
- `src/mtr-asi/src/coop/input/per_entity_tick_hook.cpp` — stale
  install-log message corrected (was "Step 4: logging only; No dev
  swap yet"; now describes the active PRE/POST swap).

Build: `mtr-asi.asi = 717312 B` (was 715776), zero warnings.

## What's left for Phase 1.6 end-to-end

1. ~~**Step 3** — RULE №2 retirement of `controlmapper_vtable_probe`
   and `coop_component_registry`.~~ **SHIPPED 2026-05-12** — see
   "Step 3 — RULE №2 retirement (SHIPPED)" section at the end of
   this document. Single-player autonomous test `load-save-1-show-
   ingame` passed clean (7823 frames, 73.6s) on the retired build.
2. ~~**Step 6** — verify Tick double-tick behavior.~~ **SHIPPED
   2026-05-12** — there is no "double tick". ControlMapper::Tick fires
   exactly ONCE per sim frame from the sim aggregator (caller
   0x0056F361), which runs OUTSIDE any per-wilbur-tick window. The
   swap brackets only cover the wilbur's `sub_5AD9B0` subscriber walker;
   Tick is invoked elsewhere in the global sim schedule. So Tick
   always reads `*(this+4) == dev_p1` (engine's hardware-fed device);
   dev_p2 is NEVER read by Tick. dev_p2's per-frame state is touched
   only by `sub_572340` (the poll-and-write fn that `hook_poll`
   instruments), and empirically that fires zero times against dev_p2.
   The intuition from MTA's per-ped CRemoteDataSA would suggest two
   reads per frame; in MTR's architecture, dev_p2 is a write-only
   buffer from the engine's perspective and a read-only buffer for the
   network side (Phase 2.0 `write_p2_state`). Documented in
   `src/mtr-asi/src/coop/input/controlmapper_dev.cpp` near `hook_tick`.
3. ~~**LAN dual-launch live test (user-driven)**~~ — **PARTIALLY
   SHIPPED 2026-05-12** as `tools/run-coop-test.ps1 -Mode dual-local
   -Scenario coop-lan-soak`. See "Step 7 — autonomous LAN harness"
   section at end of this document for the design, the test pass that
   required user intervention, and the DInput-foreground blocker that
   prevents true autonomy.
4. **Phase 2.0 input-replication design** — write_p2_state threading
   model decision (MTA-style main-thread-deferred queue is the
   audit-recommended path). Wire format design uses the now-confirmed
   field set (18 button currs + 4 analog floats).

## Audit findings — what was applied this session

- **Stale install log in per_entity_tick_hook.cpp (95%, IMPORTANT)** —
  log said "Step 4: logging only; No dev swap yet" but the swap is
  shipping. FIXED: log string corrected to reflect active PRE/POST swap.
- **Analog write offsets in write_p2_state were +0x30..+0x3F (82%,
  IMPORTANT)** — engine's sub_572340 writes at +0x34..+0x40. FIXED:
  loop changed to `p2 + 0x34 + 4*i` so wire format matches engine.
- **Sentinel values at +0x80/+0x8C/+0x98/+0xA8 unzeroed (85%, doc
  gap)** — accepted-known-unknown rationale added as comment block
  next to the zero-shim. Will revisit if LAN test surfaces axis
  inversion / scale-distortion symptoms.
- **dev+0xB4 width was byte-write (88%, CRITICAL pre-implementation)**
  — would leave upper 3 bytes of the dword set, allowing
  synthetic-fallback to fire spuriously. FIXED pre-ship: written as
  full uint32_t zero.
- **Phase 2.0 threading model (70%, deferred)** — write_p2_state's
  "torn read recovers next snapshot" is MVP-acceptable but diverges
  from MTA. Documented as Phase 2.0 design decision; not a blocker.

## Patterns reinforced

- **3-agent design + 2-agent audit continues to deliver.** Architect's
  Option C was the right strategic call; reviewer + MTA-domain auditor
  independently surfaced the SAME 88% finding (shared subdevices)
  without coordination — strong signal it is real.
- **Audit-mask insight.** The autonomous test passed all four
  build-test cycles because the failing path (swap_to_player(1)) is
  never exercised in single-player. RULE №1: "works today by
  accident" is not done. End-to-end validation requires the LAN
  dual-launch live test.
- **Incremental ship + verify per step.** Each of Steps 1, 4, 2, 5,
  audit-fix shipped to `Game/mtr-asi.asi` independently with its
  own autonomous test pass. The build sequence the architect
  proposed survived contact with reality unchanged.
- **The dev-class RE was lower-yield than expected.** 5 of 13 dev
  vtable slots are stubs (`return 0` or `return 1` or `nullsub`).
  The "AttachDevice" function and the dev-class identity remain
  un-RE'd; for MVP correctness this turns out not to matter, but it's
  open work for next session if we adopt the recursive-clone fix.

## Step 3 — RULE №2 retirement (SHIPPED 2026-05-12)

Two Phase 2 modules retired cleanly per RULE №2 (no migration baggage)
after the gap-close ship demonstrated that `per_entity_tick_hook` +
`controlmapper_dev` (wilbur-tick granularity) supersedes the
component-tick granularity approach. Both modules were dead code or
populated-but-never-read state-tables; deletion was zero-behavior-change.

### `controlmapper_vtable_probe` — pure dead code

Purpose was a Phase 2 per-slot ControlMapper vtable-call logger:
allocate a heap copy of the 13-slot static vtable, replace slots 3..12
with naked logging thunks, atomically rewrite `instance[0]` to the
proxy vtable. Gated on `-mtrasi-cmvt-probe-arm`.

The single trigger function `arm_if_ready(instance, vtable, explicit)`
had **zero callers** — `controlmapper_probe` (the original intended
caller) was deleted earlier in Phase 1.6 design. The probe's
`install()` boot-call was a no-op without arm, and `dump_to_log()`
called from `test_harness::hard_kill_self()` always wrote zero slot
counts.

### `coop_component_registry` — superseded by `per_entity_tick_hook`

Purpose was a (component_ptr → player_idx) side-table populated by
walking `wilbur+0xD04` (= wilbur[833], the component chain head) at
wilbur-factory POST. The `-mtrasi-coop-router-arm` flag additionally
installed class-wide in-place vtable patches on slots 2/4 of every
component class encountered.

`player_idx_for_component()` had **zero external callers** — the
side-table was being populated for every wilbur instance, but no live
code ever read it. The class-wide thunks (~120 lines of `__asm` per-slot
naked dispatchers + an `std::map<vt_addr, OrigSlots>` for orig-slot
lookup) were superseded by `per_entity_tick_hook`'s PRE/POST swap on
`sub_5AD9B0` (wilbur+0xD04 subscriber-list walker, ONE level up from
the components). The walker-level swap bracket is cleaner: one swap
per wilbur-tick frame, against the wilbur instance whose `player_idx`
is known up-front via `protagonist_registry::player_idx_for(entity)`.
Per MTA architectural principle 7 (engine-wrapper boundary): the
component-level thunks were doing per-component RE inside what is
fundamentally a wilbur-level routing decision.

### What changed

- DELETED: `include/mtr/controlmapper_vtable_probe.h`,
  `src/coop/controlmapper_vtable_probe.cpp`,
  `include/mtr/coop_component_registry.h`,
  `src/coop/coop_component_registry.cpp`.
- `CMakeLists.txt` — 2 source lines removed.
- `dllmain.cpp` — 2 fwd-decls + 2 install-blocks removed (~24 lines).
- `protagonist_registry.cpp` — `register_wilbur_components` call site
  + include removed from `hk_wilbur_alloc` (~10 lines).
- `test_harness.cpp` — `#include`, fwd-decl of `dump_to_log`, call
  in `hard_kill_self`, entire `tick_coop_router_soak` scenario
  (~265 lines), scenario-table entry removed (~270 lines).

Retired cmdline flags (no longer scanned by anything live):
`-mtrasi-cmvt-probe-arm`, `-mtrasi-cmvt-swap-test`,
`-mtrasi-cmvt-swap-stress`, `-mtrasi-coop-router-arm`,
`-mtrasi-coop-router-dry`, `-mtrasi-coop-comp-registry-log`.

### Validation

- Release build clean (only pre-existing C4996 `strncpy` warnings,
  none introduced by this change).
- `mtr-asi.asi`: **717312 B → 706048 B** (−11264 B, ~11 KB shrink,
  consistent with deleted thunk infrastructure + scenario state
  machine).
- Autonomous test `load-save-1-show-ingame`: **PASS** (7823 frames,
  73.6 s, probe ok, 3600-frame post-load soak clean).
- 1-agent audit (`feature-dev:code-reviewer`) post-retirement:
  Q1..Q5 all clean. Only finding was a stale ordering note in this
  research doc (lines 159..163), which has now been corrected.

### Patterns reinforced

- **`arm_if_ready` patterns die quietly.** A module gated behind a
  "call me from somewhere to wake up" trigger function leaves no
  callsite once the original caller is retired. The thunk
  infrastructure stays installed but inert, looking alive in `install()`
  registrations and CMake source lists. The post-retirement grep for
  `arm_if_ready` was decisive: zero callers, retire the whole module.
- **Populated-but-unread side-tables are pure overhead.**
  `coop_component_registry::register_wilbur_components` was being
  called from `hk_wilbur_alloc` and was walking 1-256 nodes per wilbur,
  building an `unordered_map` keyed by `component_ptr`. The lookup
  function never ran. Net cost was a per-wilbur chain walk for data
  no one read. Retirement is a cheap latency win on top of the
  RULE №2 win.
- **Audit-after-retire catches doc rot.** The 70%-confidence finding
  was a stale "deferred to next session" note in the same research
  doc that documents the retirement — the kind of internal
  inconsistency that's invisible while editing line-by-line but
  obvious to an agent reading the doc cold.

## Step 7 — autonomous LAN harness (PARTIAL SHIP 2026-05-12)

Built per RULE №1 in lieu of leaving the LAN test as "user-driven".
The infrastructure shipped + functions; one live test passed with all
the right invariants. BUT: the pass required user-driven focus toggling
because DInput's `DISCL_FOREGROUND` cooperative-level mode silently
drops input on the background Wilbur. Truly-autonomous LAN testing is
blocked on a follow-up DInput-foreground-strip fix.

### What shipped (works correctly)

1. **`drive_to_gameplay` helper** in
   `src/mtr-asi/src/test_harness.cpp` — extracted the menu-nav phases
   (boot → settle → load-button → slot-confirm → continue-game →
   settle-in-gameplay) into a reusable subroutine returning
   `DriveResult::{Pending, Done, Fail}`. Both
   `tick_load_save_1_show_ingame` and the new `tick_coop_lan_soak`
   call it. The load-save scenario re-verified clean (no regression).

2. **`tick_coop_lan_soak` scenario** in test_harness.cpp — phases:
   `kDriving` (calls drive_to_gameplay) → `kWaitForPeer` (gates on
   `NetSession::peer_known() && MtrPlayerManager::has_remote()`,
   ≤3600 frames) → `kSoak` (1800 frames; fails immediately if
   `poll_probe_stats().fires_p2` grows from the soak-entry snapshot)
   → `kReportAndExit`. Asserts the Phase 1.6 invariant under a real
   peer.

3. **Two public getters** added for the scenario:
   - `mtr::coop::net::NetSession::peer_known()` —
     `m_peer_known.load(acquire)`.
   - `mtr::coop::MtrPlayerManager::has_remote()` — locks `m_mu` and
     calls existing private `has_remote_locked()`.

4. **`write_result_json` extended** with always-present coop fields:
   `net_active`, `peer_known`, `has_remote`, `packets_sent`,
   `packets_recvd`, `send_errors`, `bad_packets`, `fires_p1`,
   `fires_p2`, `fires_other`. Single-process scenarios emit zeros.

5. **`-mtrasi-keep-dxresolution` cmdline flag** in `cmdline_hook.cpp`
   and `windowmode.cpp` — opts out of the auto-rewrite to native
   monitor dims AND the borderless-monitor window resize. Required
   for two Wilburs to coexist as true small windows on one screen.

6. **Per-process log paths** in `log.cpp` — `init()` scans cmdline
   for `-mtrasi-coop-port=N`, writes to `mtr-asi-<N>.log` for
   N > 0. Otherwise default path. Two Wilburs no longer share/truncate
   one log file.

7. **`tools/run-coop-test.ps1` dual-local mode** — launches host
   (`-mtrasi-coop-host=31415 -mtrasi-coop-port=31415`) then client
   (`-mtrasi-coop-connect=127.0.0.1:31415 -mtrasi-coop-port=31416`),
   both windowed-1024x768 with `-mtrasi-keep-dxresolution`. Moves
   each window to a non-overlapping position via `Get-Process
   MainWindowHandle` + `SetWindowPos` (using `Get-Process` is the
   correct approach — earlier `FindWindowExA` enumeration failed).
   Concurrent polling loop (`Wait-ForResults`) waits on both result
   JSONs, log-stall watchdog per-process, outer hard timeout.
   Aggregate: pass iff both pass AND both `fires_p2 == 0`.

8. **Step 6 source comment** in `controlmapper_dev.cpp` near
   `hook_tick` documents the once-per-frame global Tick + that
   dev_p2 is write-only from the engine's perspective.

### The live test result and what it really means

`tools/test-runs/20260512-144616-coop-coop-lan-soak/` recorded:
- host: result=pass elapsed_ms=41781 frames=9405 peer_known=1
  has_remote=1 **fires_p2=0** packets_sent=1922 packets_recvd=2665.
- client: result=pass elapsed_ms=60375 frames=6275 peer_known=1
  has_remote=1 **fires_p2=0** packets_sent=4466 packets_recvd=1982.
- Aggregate: PASS.

The Phase 1.6 invariant **does** hold with a real LAN peer: the
engine never polled dev_p2 across 1800 frames of soak on either side.
This is the empirical end-to-end confirmation of the gap-close design.

**However**: the test pass was not fully autonomous. The user observed
that one Wilbur was stuck on the splash screen "Press Any Key" while
the other progressed through menus. When the user clicked on the
stuck window to give it focus, the test harness's
`dinput_hook::inject_kb_keypress` calls began taking effect, and that
Wilbur caught up. Without the manual focus toggle, the host would
have hung at boot indefinitely.

### Root cause of the focus dependency

`IDirectInputDevice8::SetCooperativeLevel(DISCL_EXCLUSIVE |
DISCL_FOREGROUND)` is the cooperative-level the engine uses. Per the
DirectInput contract, devices acquired with `DISCL_FOREGROUND` return
no input (and refuse Acquire() with DIERR_OTHERAPPHASPRIO) when their
owning window is not in foreground. The mtr-asi `dinput_hook` patches
the buffer that `GetDeviceState` returns — but only IF the call
reached our hook with a successful pending state. When the foreground
gate fails inside DInput, our hook sees the error path and the
injected key state is dropped on the floor.

Two Wilburs on one screen → only one is foreground at any moment →
the other's DI input is silently zeroed → the test harness can't
drive its menu nav.

### Fix path (next session)

Per RULE №1, the proper fix is to make injected DI state foreground-
independent. Two candidate approaches, picking one ahead of the next
LAN test attempt:

(a) **Hook `IDirectInputDevice8::SetCooperativeLevel`** to strip
    `DISCL_FOREGROUND` (force `DISCL_BACKGROUND`). Cooperative-level
    re-acquire is internal to DInput; the engine's later Acquire()
    calls succeed regardless of window focus. Smallest engine-visible
    change, cleanest contract.

(b) **Inject above DInput** — bypass DInput entirely for autonomous
    test input by writing directly into the engine's ControlMapper
    state. Cleaner separation from DI semantics, but requires
    extending the controlmapper_dev module with a "synthetic input"
    write path that the test harness calls instead of dinput_hook.

(a) is the smaller fix and is what the next session opens with. Gate
on a cmdline flag (`-mtrasi-di-bg-input`) so single-Wilbur runs are
unaffected and the engine contract change is scoped to autonomous
test runs.

### Audit findings — applied this session

- **Hard-kill scope (85% conf, IMPORTANT)** — `tick_coop_lan_soak`
  ends in gameplay; the in-process shutdown chose `request_shutdown`
  (WM_CLOSE) which risks autosave. Fixed: `ends_in_gameplay` flag
  now covers both `load-save-1-show-ingame` and `coop-lan-soak`.
- **Single-process log-stall watchdog (82% conf, IMPORTANT)** —
  `Wait-ForResult` in `tools/run-coop-test.ps1` hardcoded
  `mtr-asi.log`, but `Run-SingleProcess` always passes
  `-mtrasi-coop-port=N` so log.cpp writes `mtr-asi-N.log`. Fixed:
  watchdog now derives the path from the port arg (matches
  dual-local `Wait-ForResults`).

### Patterns reinforced

- **Don't trust a "pass" without verifying autonomy.** The
  aggregate-pass exit code 0 from the LAN test wasn't honest — it
  passed only because the user manually unblocked the stuck Wilbur.
  Standing rule: when a test depends on user input, declare it
  user-assisted, not autonomous, in the result.
- **DInput foreground is the next blocker for any multi-Wilbur
  autonomous work.** The `DISCL_FOREGROUND` constraint silently
  drops injected input, not just real input. Without a
  foreground-strip the autonomous harness fundamentally can't drive
  two Wilburs on one screen.
- **`Get-Process | MainWindowHandle` is the right way** to find a
  spawned process's window from PS1. `FindWindowExA` enumeration is
  fragile (child-after iteration semantics + restart logic broke
  in the first attempt).