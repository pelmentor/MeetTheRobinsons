# Coop orphan filter — (b7.10) + (b7.11) audit brief

**Date:** 2026-05-12.
**Purpose:** Independent verification of the smart filter (b7.10) and the
audited-safe vtable allowlist (b7.11) before we build more on top of
them.

This brief consolidates the key claims that two review agents (one RE-side,
one code-side) should challenge. **Do not trust this document — that's the
whole point of the audit. Verify each claim against the raw artifacts.**

---

## What the orphan filter does (in one paragraph)

The mod spawns a "coop orphan" Wilbur entity via `entity_factory_construct`.
The orphan is structurally a real Wilbur but its per-subsystem registry at
`+0xCCC` is empty, so any per-frame tick that consults the registry crashes.
The engine ticks per-Wilbur subsystems by walking a linked list at
`wilbur+0xD04` inside `sub_5AD9B0` and calling each node's `wrapper->vtable[13]`
on the wrapper.

(b7.5..b7.9) installed a PRE-trampoline hook on `sub_5AD9B0` that **unlinks**
any node whose wrapper points at the orphan, lets the trampoline tick the
remaining nodes, then **re-links** the saved nodes after return. This kept
the orphan alive but contributed no tick activity to it.

(b7.10) added a "smart" mode (`-mtrasi-coop-filter-smart`): orphan-owned
nodes whose `wrapper->vtable[13]` equals the single-instruction `ret` stub
at `0x0059AAF0` are **left linked**. 21 of 40 subscriber classes hit this
path — they tick as no-ops on the orphan, zero crash risk.

(b7.11) added an **audited-safe vtable allowlist** on top of smart mode:
classes whose `vt[13]` does real work but has been hand-audited as safe to
run on the orphan are also left linked. First entry: GroundFollower
(vtable `0x006D0F00`, tick `0x005E93A0`).

Result after (b7.11): 40 = 18 unlinked + 21 no-op-stub + 1 audited.
Live test: `result=pass frames=1871`.

---

## Files to audit

**Implementation:**
- [src/mtr-asi/include/mtr/coop_orphan_filter.h](../../src/mtr-asi/include/mtr/coop_orphan_filter.h)
- [src/mtr-asi/src/coop/coop_orphan_filter.cpp](../../src/mtr-asi/src/coop/coop_orphan_filter.cpp)

**Findings doc (sections (b7.10) at line ~2891 and (b7.11) at line ~3034):**
- [research/findings/coop-phase0-input-separation-point-2026-05-11.md](coop-phase0-input-separation-point-2026-05-11.md)

**Live test traces:**
- `research/findings/coop-phase0-b710-smart-filter-trace-2026-05-12.log` (b7.10 baseline)
- `research/findings/coop-phase0-b711-groundfollower-trace-2026-05-12.log` (b7.11 GreenFollower)

**Build artifact:** `Game/mtr-asi.asi` = 714,752 bytes.

**Repro:**
```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker -mtrasi-coop-filter-smart'
```

---

## Claims to verify (RE side)

These are the static-RE claims that gate the whole filter. **If any of these
is wrong, the filter is unsafe.**

### Claim R1: 0x0059AAF0 is a single-instruction `ret` stub

Used by 21 of 40 wrapper classes as their `vt[13]`. The filter's smart mode
assumes invoking it has zero side effects.

**Verify:** read the bytes at `0x0059AAF0`. Expected: `C3` (single `ret`)
followed by alignment padding. If it's anything more than `ret` (e.g.
`xor eax, eax; ret`), the claim still holds. If it's a multi-instruction
function that touches memory or globals, the claim is FALSE and (b7.10) is
unsafe.

### Claim R2: 40-class inventory labels are correct

The findings doc at line ~2946 lists 40 wrapper classes by name (Rider,
ViewDriver, ..., WeaponInventory). The labels come from reading each
vtable's `slot[1]` (offset 0x4), which is a class-name accessor with the
pattern `B8 imm32 C3` = `mov eax, str_ptr; ret`.

**Verify:** sample 5 random vtables from the inventory. For each, read
`*(vtable + 4)` to get the function VA of the accessor; read 6 bytes
there; confirm they're `B8 ?? ?? ?? ?? C3`; read the string at the imm32.
Confirm the resulting string matches the class name in the doc.

### Claim R3: GroundFollower tick (0x005E93A0) is safe on the orphan

This is the audited Tier-1 entry. The audit (see (b7.11) section of the
findings doc) claims:

- `vt[13]` body = `sub_5E93A0`:
  - Calls `vt[6]` predicate first (which reads only `this+0xC` and `this+0xE`,
    the wrapper's own state — at function `0x00422D00`).
  - If predicate true: copies 3 floats from `[this+4] + 0x58..0x60`
    (wilbur+0x58..0x60) to `this+0x28..0x30`, zeros `this+0x34/0x35`.
  - No registry lookup. No global read. No vtable dispatch beyond vt[6].

**Verify:** disassemble `0x005E93A0` and `0x00422D00` in full. Confirm:
1. `sub_5E93A0` has no `mov eax, dword_XXXXXX` (global read) or `call` other
   than the single `call dword ptr [eax+18h]` (vt[6] dispatch).
2. `sub_422D00` reads only `[ecx+0Ch]` and `[ecx+0Eh]` and returns a bool.
3. The wrapper at offset `+0x28..+0x35` is NOT read by any external system
   (xref search). If something else reads it, garbage floats would propagate
   beyond the wrapper, breaking the "safe on orphan" claim.

### Claim R4: GroundFollower vtable at 0x006D0F00, vt[13] at offset 0x34

The allowlist is keyed on the vtable VA. If the VA is wrong, the lookup
never matches and the entry does nothing (false-negative — annoying but
not unsafe). If the offset is wrong (vt[13] should be at vtable+0x34),
the "is this the no-op stub?" check in `wrapper_tick_is_engine_noop_stub_seh`
reads the wrong slot — false matches or misses.

**Verify:**
1. Read bytes at `0x006D0F00` (20 slots × 4 = 80 bytes). Confirm
   `vtable[13] = *(vtable + 0x34) = 0x005E93A0`.
2. Confirm `0x006D0F00` is genuinely a vtable (xref it: it should be
   written into a `[this]` slot in a ctor — pattern `mov dword ptr [esi], 0x006D0F00`).

### Claim R5: 18 remaining real-tick classes don't fit Tier-1

Per the (b7.11-audit) section, none of the 18 remaining classes have a tick
body that's read-only from `wilbur+<small offset>`. The audit was based on
the first ~30 instructions of each tick.

**Verify (sample):** Pick 3 of the 18 — preferably the ones marked
"1.5?" or "?" in the audit table. Disassemble more deeply (60-100
instructions) and confirm the disqualifier is real and not an artifact of
the short window. Specifically:

- `Rider` (0x005EBA80) — does `sub_55D8F0` reach into the registry?
- `DamageSurfaceTracker` (0x00532860) — does `sub_5320E0` or the tail
  `jmp sub_532600` reach into the registry?
- `SlideController` (0x00450B60) — does `sub_450830` reach into the registry?

If any are actually clean → we miscategorised and an extra Tier-1 entry was
missed.

---

## Claims to verify (code side)

These are the implementation claims for the filter itself.

### Claim C1: SEH coverage is correct in the walker

`unlink_orphan_nodes_seh` and friends use `__try`/`__except`
(`EXCEPTION_EXECUTE_HANDLER`). MSVC requires that no destructible objects
live in the `__try` scope — otherwise it won't compile, but the structure
is also subtle.

**Verify:**
1. Every memory access via raw pointer (`*reinterpret_cast<uint32_t*>(...)`)
   is inside a `__try`.
2. The walker can't leak state on partial fault (e.g. if we fault mid-walk
   after unlinking 3 of 5 nodes, we should still re-link those 3 after the
   trampoline call — confirm).
3. No std::mutex / std::lock_guard inside a `__try` block (MSVC limitation).

### Claim C2: Unlink-then-relink correctly restores list ordering

The walker rewrites `*slot = next_addr` for each filtered node, saves
`(slot_addr, node_addr, next_addr)` to a fixed-size array, and after the
trampoline returns, replays in REVERSE order: `*node->next = saved_next; *slot = saved_node`.

**Verify:**
1. If two filtered nodes are adjacent in the list (A, B both orphan-owned),
   what state is the list in after walk? After relink? Is original ordering
   preserved?
2. Could the trampoline modify any node we saved? If yes, our re-link would
   stomp the trampoline's edit. (Currently the assumption is "trampoline only
   modifies nodes it visits, and we've removed orphan nodes from the visit
   list" — so trampoline can't see them. Confirm this is safe.)
3. `kMaxUnlinks = 64` cap: at the cap, the walker breaks. Any remaining
   orphan nodes get ticked. The log says `hit_cap=1`. Confirm we've never
   actually hit the cap in any test trace.

### Claim C3: Smart-skip is gated correctly

The smart filter requires both `-mtrasi-coop-keep-orphan` AND
`-mtrasi-coop-filter-list-walker` AND `-mtrasi-coop-filter-smart` to take
effect. The audited-safe allowlist piggybacks on `-mtrasi-coop-filter-smart`.

**Verify:**
1. Without any flag, the hook is installed but inert (passes through).
2. With only `-mtrasi-coop-filter-list-walker`, the original "unlink all
   orphan nodes" behaviour runs.
3. With smart on, the no-op-stub skip + audited-safe skip both run.
4. The cmdline parse uses `strstr` — confirm no false positive (e.g.
   `-mtrasi-coop-filter-smart-foo` shouldn't enable smart, but with strstr it
   would; is this a concern?).

### Claim C4: AuditedVtable allowlist scan is correct

`lookup_audited_vtable` does a linear scan of `kAuditedSafeVtables`.
Currently 1 entry. Scan is wrapped in nothing — relies on the vtable VA
being non-zero (checked by caller).

**Verify:**
1. The function is constexpr-safe and called only when `smart_skip == true`
   and `vtable != 0`.
2. Adding entries doesn't require any code changes elsewhere (it's truly
   data-driven).
3. The scan happens INSIDE the walk, which is inside the SEH-guarded
   `unlink_orphan_nodes_seh`. So if the vtable read faults, we fall through
   to "no match" and unlink as before. Safe.

### Claim C5: Stats are race-free

`g_stats` is a global, mutated under `g_stats_mu`. The hook may be called
from multiple threads (engine ticks via different paths).

**Verify:**
1. All Stats writes are under the lock.
2. `stats()` reads return-by-value under the lock.
3. No data race on `g_stats` outside the lock.
4. The atomic `g_inventory_dumped` and `g_engine_baseline_dumped` are
   correctly used (CAS).

---

## What an audit agent should NOT do

- **Do not re-implement.** This is read-only verification.
- **Do not rewrite the findings doc.** Just report contradictions.
- **Do not speculate about Tier 1.5 / Tier 2 design.** That's for future
  sessions.
- **Do not re-run the test.** The traces are archived; trust them or
  re-read them.

## Report format

Three buckets per claim, in order:

1. **CONFIRMED**: I checked X, it matches the brief.
2. **CONTRADICTED**: I checked X, the brief says Y but X is actually Z.
   Provide the raw artifact (decompile snippet, byte dump, etc).
3. **CANNOT VERIFY**: I tried but the artifact isn't available / tool
   limit / ambiguous. Say what would unblock.

Keep it tight. Per-claim: 1-2 sentences for CONFIRMED, ~5-10 for
CONTRADICTED. Aim for under 600 words total.
