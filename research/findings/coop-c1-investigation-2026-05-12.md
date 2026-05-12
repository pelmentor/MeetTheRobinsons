# C1 (sub_58D330(NULL)) — Investigation Findings 2026-05-12

**Status: FIXED 2026-05-12 via strategy (b).** New module
`src/mtr-asi/src/coop/coop_vibrate_route.{h,cpp}` installs a PRE MinHook
on VibrateJoystick::vt[13] @ `0x00532B40` that early-returns when
`*(this+4) == coop_spawn_probe::live_orphan_entity()`. VibrateJoystick
(vtable `0x006C1AB0`) was promoted to
`coop_orphan_filter::kAuditedSafeVtables`. The diagnostic flag
`-mtrasi-coop-confirm-vibratejoystick` and its plumbing were deleted per
RULE №2 (~140 lines removed from `coop_orphan_filter.{h,cpp}`).

Verification: route-validation `frames=4220 result=pass` (route caught at
call #122 even with filter passing VibrateJoystick through); final
baseline `frames=4218 result=pass`. Both traces in
`tools/test-runs/20260512-08{2741,3138}-load-save-1-show-ingame/`.

**Design consensus (three agents, 2026-05-12):**
- code-architect: MTA's pattern is per-site re-routing, not
  fake-registration (`reference/mtasa-blue/.../multiplayer_keysync.cpp`).
- general-purpose RE: strategies (a)/(d) blocked by SecuROM stolen-byte
  thunk wall (sub_51F960 / sub_51EB40 are runtime-resolved IAT thunks to
  DLL target 0x10059D0 via slot 0xF8E718; static RE cannot reach the
  manager-insert API).
- code-explorer: both wilburs go through the same factory + ctor +
  sub_58CA80("Avatar"); orphan does NOT bypass any registration.

**Empirical evidence that invalidated strategy (a) "init missing data":**
- `*(engine_wilbur+0x1A8) = 0x21A` (538)
- `*(orphan+0x1A8) = 0x21B` (539)
Both valid, sequential. The field is initialized; the failure is
downstream behind the SecuROM thunk wall.

**Architectural insight:** per-site routing for "this subsystem doesn't
apply to this kind of entity" is NOT a RULE №1 crutch when the subsystem
genuinely doesn't apply. VibrateJoystick is local-hardware feedback;
remote-player ghost has no local controller; the proper behavior is to
skip it. See `memory/feedback_per_site_route_not_crutch.md`.

---

## Original investigation log (archaeology)

Sections below preserved as-is for context on how the static-RE
identification + audit-sweep correction + live confirmation led to the
strategy-b ship. Status fields below refer to the state DURING
investigation, before the fix shipped.

---

## Empirical confirmation (2026-05-12 late, post-audit)

After the audit sweep identified VibrateJoystick statically, a
**selective filter disable** was added to `coop_orphan_filter` to test
the hypothesis live. Mechanism: new flag
`-mtrasi-coop-confirm-vibratejoystick` leaves only VibrateJoystick
(vtable `0x006C1AB0`) linked on the orphan, every other unsafe wrapper
remains filtered. Expected outcome with flag ON: C1 fires (NULL ECX +
"Avatar" lookup). Expected with flag OFF: GREEN (baseline regression).

**Result, 2026-05-12 ~13:36:**

- Baseline run (flag OFF, all four standard filter flags):
  `result=pass frames=951` — no regression.
- Confirmation run (flag ON, same four standard filter flags):
  `process exited without result JSON (exit=-1073741819)` — = STATUS_
  ACCESS_VIOLATION 0xC0000005. **C1 fired on call #1 of `sub_5AD9B0`**,
  exactly as predicted.

**Filter stats at the moment of the crash:**
```
seen=40 filtered=17 skipped_safe=21 skipped_audited=1
confirm_let_through=1 hit_cap=0 smart=1
```
40 = 17 unlinked + 21 noop + 1 audited (GroundFollower) + 1 confirmed
(VibrateJoystick). One fewer node filtered vs. baseline = the
VibrateJoystick we deliberately left in.

**Crash signature (VEH FAULT #0):**
```
code=0xC0000005 eip=0x0058D331 av_op=0 av_addr=0x00000050
eax=0x00745B70 ebx=0x00000011 ecx=0x00000000 edx=0x00728A48
esi=0x10F2B990 edi=0x00728A40 ebp=0x030FFDA0 esp=0x030FF818
stack[00..03] = 10F2B990 004CD7F0 006B6AA0 00532B64
stack[04..07] = FFFFFFFF 00000011 10F2C9A0 10F2B990
stack[08..11] = 005AD9DC 00000154 5495CBC5 10F2DF20
```

**Stack tells the full call chain back-to-front:**
- `stack[00] = 0x10F2B990` = ESI saved by `push esi` (1st insn of
  sub_58D330 at `0x58D330`). Confirms `0x58D330` byte is `0x56`
  (push esi), not `push ecx` — a small disasm correction.
- `stack[01] = 0x004CD7F0` = return addr pushed by 0x4CD7E1's
  `push 0x4CD7F0; push 0x58D330; ret` obfuscated-wrapper trick.
- `stack[02] = 0x006B6AA0` = `"Avatar"` literal. **First arg** to
  sub_58D330 — the lookup key.
- `stack[03] = 0x00532B64` = return addr inside VibrateJoystick
  `vt[13]` body at `0x00532B40 + 0x24`. Locates the `call sub_51EB40`
  inside the body precisely (insn = 5-byte `call rel32` at `0x532B5F`,
  return to `0x532B64`). **Direct proof the active wrapper is
  VibrateJoystick.**
- `stack[08] = 0x005AD9DC` = return addr inside `sub_5AD9B0`
  (subscriber list walker) — confirms the call came from the
  `wilbur+0xD04` walker on the orphan.
- `stack[11] = 0x10F2DF20` = orphan entity pointer (matches
  `(orphan=0x10F2DF20)` in filter log).

**ECX=0 origin:** Some site in the IAT-resolved chain
(`[0xF8E718]` → runtime target → `0x4CD7E1`) clears ECX before the
obfuscated wrapper jumps to sub_58D330. Either a defensive `xor ecx,
ecx` in the resolved target, OR sub_58D330 is called via a code path
that doesn't carry `this` through. ESI=`0x10F2B990` (= wrapper's own
`this`) survived from before the chain — so the wrapper was a valid
non-NULL pointer entering vt[13]; the NULL is introduced deeper.

**EAX=0x00745B70** is interesting — that's a global. Could be the
"current Avatar lookup result cache" that the resolved target tail-
calls into sub_58D330 to fill in. Investigating EAX's role is part of
the next-session IAT slot probe.

**What this rules in and rules out:**
- RULES IN: VibrateJoystick's vt[13] is the immediate caller into the
  IAT chain that lands on `sub_58D330(NULL, "Avatar")`. The 4 fix
  strategies — (a) init-the-data, (b) per-site route, (c) byte patch,
  (d) ordering fix — are now all applicable to a *known* call site.
- RULES OUT: Any hypothesis where the C1 chain reaches `sub_58D330`
  through a different wrapper (e.g. the 17 other unsafe wrappers
  filtered in this run). They tick after the crash kills the process,
  so their behavior is independent of C1.

**Trace:** `tools/test-runs/20260512-073601-load-save-1-show-ingame/
mtr-asi.log:1674-1699`.

---

## Audit corrections (2026-05-12 late, post-audit sweep)

Two parallel audit agents (RE-fidelity + doc-review) caught three
corrections to the initial investigation. Applied below; original wording
retained where it doesn't conflict, but all addresses and the wrapper
identification are corrected.

1. **Function start is `0x00532B40`, NOT `0x00532B3E`.** Bytes at
   `0x532B3E..0x532B3F` are `CC CC` alignment filler between
   `sub_532ABE`'s SEH tail and the actual function entry. IDA hadn't
   auto-analyzed `0x532B40` either, which is why the `find_bytes
   "3E 2B 53 00"` search returned 0 — the real vtable pointer is
   `40 2B 53 00`, and that IS in a vtable.

2. **The wrapper is `VibrateJoystick`** (b7.10 audit row 28, vtable
   `0x006C1AB0`, `vt[13] = 0x00532B40`). The original investigation
   missed the cross-reference because it searched for the 2-byte-off
   address. With `0x532B40` as the search key, the b7.10 enumeration
   already had the answer:
   - `research/findings/coop-phase0-input-separation-point-2026-05-11.md:2978`
   - `research/findings/coop-phase0-input-separation-point-2026-05-11.md:3202`
   - Body-class characterization in b7.10: *"huge function + likely
     touches global input subsystem"* — consistent with the chain going
     through an IAT thunk to an input/registry lookup.

3. **First call target is `0x51F960`, not `0x51F95E`** (call instruction
   at `0x532B57` with rel32 `0xFFFECE04`, so target = next-instr
   `0x532B5C + 0xFFFECE04 = 0x51F960`). The second call target
   `0x51EB40` (the IAT thunk) is correct.

Also: the field read at the body of the wrapper is at instruction
`0x532B47` (`mov eax, [eax+0x1A8]`), not `0x532B45` as originally
written. The candidate missing-field expression remains
`*(*(this+4) + 0x1A8)` — i.e. read the "owner wilbur" pointer at
`this+4`, then read the field at `+0x1A8`. Whether `this+4` corresponds
to `this[+0x14]` in the C1 hypothesis text below requires verifying
the VibrateJoystick class layout (open).

**Why no fix shipped this session:** Per **RULE №1 + Principle 4**, the only
properly-targeted fixes are (a) init-the-missing-data, (b) per-site route
at the *specific* call site, or (c) byte patch the *specific* call. All
three require knowing the exact subscriber wrapper. The investigation
narrowed the path to a chain but didn't isolate the wrapper. Shipping a
broad NULL-skip on `sub_58D330` is explicitly out-of-scope per the
retirement plan — that is "general 'if this == NULL, skip'", which the
plan documents as masking unrelated NULL-deref bugs.

---

## Crash anchor

From `research/findings/coop-phase0-b76-fix-trace-2026-05-11.log:1651` and
`coop-phase0-b77-bypass-trace-2026-05-11.log:1635`:

```
STEP2J VEH FAULT #2 code=0xC0000005 eip=0x0058D331 av_op=0
                    av_addr=0x00000050 sym_eip=<unresolved>
#2 regs eax=0x00745B70 ebx=0x76E94500 ecx=0x00000000
        edx=0x00728A48 esi=0x10D65C90 edi=0x00728A40
        ebp=0x11744360 esp=0x030FFD7C
#2 stack[00..03]=10D65C90 004CD7F0 006B6AA0 00532B64
#2 stack[04..07]=FFFFFFFF 10D68A20 10D664E8 10D65C90
#2 stack[08..11]=005AD9DC 10F882D0 005C85D5 11381550
#2 stack[12..15]=11381530 005AD646 117E47C0 117E3D00
```

The faulting instruction:

```asm
sub_58D330:
58d330  push    esi
58d331  mov     esi, [ecx+50h]    ; <-- CRASH: ecx=NULL, reads [0x50]
58d334  test    esi, esi
58d336  push    edi
58d337  jz      short loc_58D358  ; <-- would return 0 if [ecx+0x50] is 0
```

`sub_58D330` is a **leaf linked-list lookup-by-key**:
- `this+0x50` = list head
- `node+0x04` = key field (compared via `sub_63599D`)
- `node+0x48` = next pointer
- Returns matching node, or 0 if not found

Decompile (`mcp__ida-pro-mcp__decompile 0x58D330`):
```c
int __thiscall sub_58D330(_DWORD *this, int a2)
{
  int v2 = *(this + 20);  // 0x58d331 — crashes when this=NULL
  if (!v2) return 0;
  while (sub_63599D(a2, v2 + 4)) {
    v2 = *(_DWORD *)(v2 + 72);
    if (!v2) return 0;
  }
  return v2;
}
```

---

## The lookup key — `"Avatar"`

`stack[02] = 0x006B6AA0`. Reading the string at that address:

```
mcp__ida-pro-mcp__get_string 0x6B6AA0 → "Avatar"
```

**This is the Wilbur player class name.** The failed lookup is
`find("Avatar")` in a name-keyed registry owned by some manager
(`dword_7287F0` = `0x007287F0`, currently holds `0x7295C0` = another
global).

---

## Call chain (top → bottom of stack)

```
entity_manager_tick_components @ 0x5AD4D0   <-- stack[15] = 0x5AD646 inside this fn
  -> ... 
    -> sub_5AD9B0 (subscriber list walker)  <-- stack[08] = 0x5AD9DC inside this fn
        walks wilbur+0xD04 list, calls vt[13](wrapper) for each node
      -> VibrateJoystick::vt[13] @ 0x00532B40
         (vtable 0x006C1AB0; row 28 of the b7.10 audit table — identified
          post-audit, originally searched for as 0x532B3E due to CC CC filler)
        -> tick body @ 0x00532B40         <-- stack[03] = 0x532B64
           __thiscall preamble:  push ecx; push esi; mov esi, ecx;
           mov eax, [esi+4]; mov eax, [eax+0x1A8]; mov ecx, dword_7287F0;
           push edi; push eax; mov edi, ecx; call rel32 → 0x51F960;
           push eax; mov ecx, edi; call sub_51EB40
          -> sub_51EB40 — IAT thunk at 0x51EB40, jumps via [g_iat+0x2EEA2]
             = [0xF8E718]; current value 0x10059D0 (external/runtime-resolved)
            -> resolved target eventually invokes the obfuscated
               sub_58D330+sub_58C9C0 wrapper at 0x4CD7E1
              -> 0x4CD7E1:  push 0x4CD7F0          <-- stack[01] = 0x4CD7F0
                            push 0x58D330
                            ret                    ; tail-jmp into sub_58D330
                                                   ; with ECX=NULL (set somewhere
                                                   ; in the IAT-resolved target)
                -> sub_58D330 — CRASH
                            (after sub_58D330 returns, would continue at
                             0x4CD7F0: test eax, eax; jz +0x14; mov ecx, eax;
                                       push 0x4CD805; push 0x58C9C0; ret)
```

`0x4CD7E1` is an **obfuscated inline-fused wrapper** with the exact shape
of `sub_58D370` (lookup-then-apply): `result = sub_58D330(...); if (result)
sub_58C9C0(result)`. The 12-byte "push retaddr; push target; ret" pattern
that confuses static analysis.

---

## How does ECX become NULL?

`VibrateJoystick::vt[13]` (at `0x00532B40`) sets
`ECX = dword_7287F0 = 0x7295C0` (a real, non-NULL global) before
calling `sub_51EB40`. But at `sub_58D330` entry, ECX = 0.

**Caveat flagged by doc-audit:** `dword_7287F0 = 0x7295C0` is treated
as "the right manager" without verifying that `0x7295C0` actually
contains an entry for the `"Avatar"` key. An alternative explanation
of the crash is that `0x7295C0` is the WRONG manager for an Avatar
lookup — the defensive `xor ecx, ecx` path then fires because the
target subsystem couldn't find Avatar in this manager. Distinguishes
fix strategy: if it is the right manager missing an entry, the fix is
to mirror the entry; if it is the wrong manager, the fix is correcting
which manager pointer is fetched. **Open: verify what `0x7295C0`'s
registry actually contains, and whether `"Avatar"` should be in it.**

**Hypothesis:** The IAT thunk's resolved target executes a defensive
`xor ecx, ecx` path before jumping to `sub_58D330`. Two known
functions exhibit this exact pattern (and both look up the same
`0x6B6AA0` = `"Avatar"` key):

### Candidate A — `sub_567C4D`
```asm
567c97  cmp     esi, [eax]
567c99  jge     short loc_567CA3
567c9b  mov     edx, [eax+4]
567c9e  mov     ecx, [edx+esi*4]
567ca1  jmp     short loc_567CA5
567ca3  xor     ecx, ecx                ; <-- ECX = NULL
567ca5  push    6B6AA0h                 ; "Avatar"
567caa  push    offset loc_567CC0
567caf  jmp     sub_58D330              ; <-- crashes
```

### Candidate B — `sub_51FF30`
```c
v11 = (int *)(*(this + 5))[241];        // this[5][0x3C4] = registry
if (v11 && v5 >= 0 && v5 < *v11 && *(_DWORD *)(v11[1] + 4 * v5)) {
  if (v5 >= *v11) v12 = 0;              // "dead" path that fires anyway
  else v12 = *(_DWORD **)(v11[1] + 4 * v5);
  v13 = sub_58D330(v12, 7039648);       // 7039648 = 0x6B6AA0 = "Avatar"
  ...
}
```

Both check `v4 < *v5` post-loop after `break` already guaranteed it — the
defensive `v6 = 0` branch should be unreachable in a correct invariant.
The crash means the post-loop check IS firing FALSE on the orphan, which
means **either** `[eax]` (the count) **or** `esi` (the iterator) takes
different values than at the break point — which itself would require
either a memory aliasing or some control-flow path the decompiler hides.

This is the open question. To close it: the next session needs to find
**which IAT-resolved target** the thunk at `0x51EB40` jumps to at
runtime — then we know whether it's `sub_567C4D`, `sub_51FF30`, or a
third lookup function with the same shape.

---

## Subscriber wrapper — VibrateJoystick (identified post-audit)

The b7.10 audit (`research/findings/coop-phase0-input-separation-point-2026-05-11.md`
sections "subscriber wrapper inventory" / vt[13] table) labels 40
subscriber wrapper classes. 21 are no-op (`vt[13] = 0x0059AAF0 = ret`),
1 is audited-safe (`GroundFollower`), 18 are "unsafe" and currently
filtered.

**Identified wrapper:** row 28 of the b7.10 audit:
```
| 28 | 0x006C1AB0 | VibrateJoystick | 0x00532B40 | no |
```

The audit's body-class column already characterised `VibrateJoystick`
as *"huge function + likely touches global input subsystem"* (b7.10
audit, "wrappers worth deeper triage" section). The chain through
the IAT thunk to an Avatar-name lookup is consistent with that
characterisation — input subsystem looks up the avatar to send vibrate
commands to its controller.

**Why the original investigation missed it:** searched for
`3E 2B 53 00` (little-endian of `0x532B3E`), which returned 0 matches —
because the actual vtable pointer is `40 2B 53 00` (`0x532B40`). The
2-byte gap is `CC CC` alignment filler between `sub_532ABE`'s SEH tail
(ending at `0x532B3D`) and the `VibrateJoystick` tick body. Lesson:
when a "function start" address from a stack trace lies 0..3 bytes
after a known function's end, suspect alignment filler and search the
b7.x audit table for the next-aligned address before declaring "no
xrefs."

---

## What's CONFIRMED

1. **The crash key is `"Avatar"`** (lookup of the player class by name).
2. **The crash function is `sub_58D330` at first instruction** (`mov esi,
   [ecx+0x50]` with ECX=NULL).
3. **Reached via `sub_5AD9B0` subscriber list walker** (`stack[08] =
   inside sub_5AD9B0`), confirming this is on the orphan-tick path.
4. **Entered via obfuscated push/ret wrapper at `0x4CD7E1`** (an
   inline-fused `sub_58D330+sub_58C9C0` lookup-then-apply pattern,
   inside the outer function previously labelled `sub_4CD7B0`).
5. **The wrapper's tick body is at `0x00532B40`** with ECX pre-set to
   `dword_7287F0` (= `0x7295C0`) before the chained call to the IAT
   thunk `sub_51EB40` at body-instruction `0x532B5F`.
6. **The subscriber wrapper is `VibrateJoystick`** (vtable `0x006C1AB0`,
   `vt[13] = 0x00532B40` per b7.10 audit row 28). Post-audit
   cross-reference; was originally listed as UNKNOWN due to 2-byte
   address offset.
7. **The function start was masked by alignment filler.** `0x532B3E`
   and `0x532B3F` are `CC CC`; real entry is `0x532B40`.

---

## What's UNKNOWN (next session)

1. **What populates the IAT slot at `0xF8E718` at runtime.** Static
   value `0x10059D0` is outside Wilbur's `.text` (range
   ~`0x401000..0xAFxxxx`). Likely a heap allocation or a DLL mapping
   address. **Cheapest path:** runtime-read `[0xF8E718]` during orphan
   tick first (1-line probe in `coop_orphan_filter`) before doing
   static xref search. Audit B caveat: if the resolved target is in a
   DLL, the candidate Wilbur-VA paths (`sub_567C4D` / `sub_51FF30`)
   only apply if the DLL calls back into Wilbur — not guaranteed.

2. **What `dword_7287F0 = 0x7295C0` is the manager OF.** Open whether
   `0x7295C0` actually owns an `"Avatar"` entry or whether it is the
   wrong manager for an Avatar lookup. Differentiates fix strategy
   (mirror-the-entry vs. correct-the-manager-pointer).

3. **Why the orphan's tick triggers the `xor ecx, ecx` defensive path
   while the engine_wilbur tick does not.** Likely a per-entity
   field that's NULL or 0-valued on the orphan but populated on
   engine_wilbur. Candidate: `*(*(this+4) + 0x1A8)` — i.e. read
   `[esi+4]` to get the owner-pointer, then `[eax+0x1A8]` to get the
   field. The field-read instruction is at `0x532B47`. Verify the
   VibrateJoystick class layout before treating `this+4` as a stable
   offset.

4. **Construction → mirror-clone → first-tick ordering.** Open whether
   the orphan's VibrateJoystick wrapper is registered before or after
   the b7.13 mirror runs. If registration happens first, the wrapper
   ticks against an un-mirrored owner-pointer. This is a separate
   axis from "which field is missing" and may dominate the fix.

---

## Recommended fix strategy (post-investigation, NOT yet shipped)

Wrapper now identified as `VibrateJoystick` (vt[13] = `0x00532B40`).
Strategy ranking:

- **(a) Init-the-data via `coop_registry_mirror`** *(preferred per
  Principle 4)*. If Task 4 confirms `*(*(this+4) + 0x1A8)` is the
  missing field, extend the mirror to clone that data onto the orphan.
- **(b) Per-site route on `VibrateJoystick::vt[13]`.** Hook
  `0x00532B40` directly, early-return on orphan. Narrow,
  Principle-4-compliant. Drop in `src/mtr-asi/src/coop/coop_vibrate_route.cpp`
  or similar.
- **(c) Byte-patch `0x4CD7E1`.** Last resort. The 12-byte sequence
  (`push 0x4CD7F0; push 0x58D330; ret`) has no slack — adjacent bytes
  are the next obfuscation stage. Would suppress legitimate
  Avatar-not-found returns on engine_wilbur too, so this is NOT a
  clean Principle-4 fix.
- **(d) Fix construction-mirror-ordering.** If Task 4 reveals the
  field is populated on engine_wilbur and just hasn't been cloned yet
  by the time the first tick fires, the fix is reordering or
  ensuring mirror runs before subscriber registration. Cleanest of
  all — handles a class of bugs, not just this one.

---

## Next-session task list (revised post-audit)

The wrapper-identification gap that drove the original task list is
closed. Revised tasks:

1. **Selective filter disable to confirm VibrateJoystick.** Extend
   `coop_orphan_filter` to accept a per-wrapper allowlist; leave the
   filter ON for all 17 OTHER unsafe wrappers but allow
   VibrateJoystick's vt[13] (`0x00532B40`) to tick on the orphan. If
   C1 fires, identification is confirmed and the fix can proceed. If
   C1 does NOT fire, the wrapper identification is wrong and we
   re-investigate. **Cost:** ~30 lines in `coop_orphan_filter.cpp`.

2. **Runtime-read `[0xF8E718]` during orphan tick.** 1-line probe at
   `sub_5AD9B0` PRE-hook entry to log the current IAT value. Cheaper
   than xref search. Then decide:
   - If the resolved target is in Wilbur image → static analysis of
     that target reveals the `xor ecx, ecx` site.
   - If the resolved target is in a DLL → trace the DLL's entry and
     check whether it dispatches back to Wilbur. If not, the
     candidate paths (`sub_567C4D` / `sub_51FF30`) are invalidated
     and we look elsewhere for the NULL source.

3. **Verify `dword_7287F0 = 0x7295C0` registry contents.** Walk the
   linked-list at `[0x7295C0 + 0x50]` looking for an `"Avatar"` entry.
   If absent on engine_wilbur tick too, this is "wrong manager"; if
   present on engine_wilbur but somehow not reachable for orphan,
   this is "right manager, ordering issue."

4. **Verify VibrateJoystick class layout.** Confirm whether `this+4`
   is the owner-wilbur pointer. If yes, the missing field is
   `(*(this+4))[0x1A8]`. Then compare that value between engine_wilbur
   and orphan to identify the missing/zero field.

5. **Decide fix strategy:**
   - **(a) Init-the-data (preferred):** extend `coop_registry_mirror`
     to clone whatever `*(*(engine_wilbur+4) + 0x1A8)` points at.
   - **(b) Per-site route:** hook `VibrateJoystick::vt[13]` to
     early-return on orphan.
   - **(c) Byte patch:** byte-patch `0x4CD7E1` to check ECX before
     falling through. Constraint: the 12-byte sequence has no slack.
   - **(d) Fix the construction-mirror-ordering issue** if Task 4
     reveals it's actually a sequencing bug rather than a missing
     field.
