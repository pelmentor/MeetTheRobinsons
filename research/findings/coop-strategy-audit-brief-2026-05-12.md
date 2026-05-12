# Coop strategy audit brief — "is our approach a crutch?"

**Date:** 2026-05-12.
**Purpose:** Strategy-level review of the coop multiplayer work done so
far. Two independent reviewers, two different lenses. The previous
technical audit (RE/code) already confirmed the (b7.10)+(b7.11) work is
correct as implemented. **This audit asks a different question: are we
solving the right problem the right way?**

---

## The standing rule that gates everything

The user's TOP-PRIORITY rule for this project:

> **RULE №1 — No crutches, no quick fixes (weeks/months OK)**
> Always pick the proper root-cause fix. No "good enough", no "we can fix
> later", no shortcuts. "I don't care if it takes WEEKS or MONTHS — just
> do it properly" (2026-05-08).

You must evaluate the coop work against this rule. Be willing to flag work
as a crutch even if it currently passes tests.

---

## The 10-second history

Meet the Robinsons (2007 PC, x86) is single-player. The goal is to add
2-player LAN coop. This was kicked off ~2 weeks ago.

- **Phase 0A** (1 day): Audited the architecture choice; settled on
  authoritative-host UDP, 2P LAN. Two outside-audit agents both picked
  Option A.
- **Phase 0B/0C** (~1 wk): Investigated save system, SX command catalog,
  test harness, mp_probe, mp_install_site, replication primitive. All
  documented; no code in the mod yet beyond probes.
- **Phase 1** (designed not built): 5-week network layer plan.
- **Phase 2** (active, b7.5..b7.11, days): keep-alive infra for an
  "orphan" second-wilbur entity spawned in-process via the same factory
  used by P1. Goal is to make P2 visibly present + ticking before adding
  network code on top.

The current Phase-2 architecture: spawn the orphan via
`entity_factory_construct` (factory entry, NOT runtime-thunked). The orphan
is structurally a real Wilbur but its per-subsystem registry at `+0xCCC`
is empty, so any tick that consults the registry crashes. So we hooked the
engine list walker `sub_5AD9B0` and unlink orphan-owned subscriber nodes
before the tick walks them, then re-link after. (b7.10) added smart-skip
for the 21/40 no-op-stub subscribers; (b7.11) added an audited-safe
allowlist (1 entry: GroundFollower).

State at end of (b7.11): 22 of 40 orphan subscribers tick (21 no-op + 1
real). 18 still unlinked. Documented next steps: Tier 1.5 (audit
`wilbur->vt[17]` to maybe unlock 5+ more), Tier 2 (per-key registry mirror
for ViewDriver/WeaponInventory), Tier 3 (full per-wilbur registry vector
replication).

---

## Files to read (in order)

1. **The rule**: `C:\Users\Alexgrv\.claude\projects\d--Projects-Programming-MeetTheRobinsons\memory\feedback_no_crutches.md`
2. **The 9-10mo plan**: `C:\Users\Alexgrv\.claude\projects\d--Projects-Programming-MeetTheRobinsons\memory\project_coop_multiplayer_plan_v2_ready_2026-05-10.md`
3. **Phase 0 completion**: `C:\Users\Alexgrv\.claude\projects\d--Projects-Programming-MeetTheRobinsons\memory\project_state_2026-05-11_coop_phase_0_complete_phase_1_designed.md`
4. **Phase 2 history (current state)**:
   - `project_state_2026-05-11_coop_input_separation_identified.md` (b7.6/b7.8/b7.9 baseline)
   - `project_state_2026-05-12_coop_smart_filter_shipped.md` (b7.10)
   - `project_state_2026-05-12_coop_b711_groundfollower.md` (b7.11, current)
5. **Implementation today**:
   - `d:\Projects\Programming\MeetTheRobinsons\src\mtr-asi\src\coop\coop_orphan_filter.cpp`
   - `d:\Projects\Programming\MeetTheRobinsons\src\mtr-asi\include\mtr\coop_orphan_filter.h`
6. **Engineering plan + RE depth**:
   - `research/findings/coop-phase0-input-separation-point-2026-05-11.md` (huge doc; the (b7.10)/(b7.11) sections at the end are the relevant part)
   - `research/findings/coop-multiplayer-plan-2026-05-10.md`
7. **Just-completed technical audit (confirms code-correctness; orthogonal to this audit)**:
   - `research/findings/coop-b710-b711-audit-brief-2026-05-12.md`

---

## What "crutch" specifically means in this project

A *crutch* is one or more of:

1. **Symptom-suppression instead of root-cause**: We catch / unlink / mask
   something that's broken instead of fixing the underlying cause. The
   workaround stays in the codebase as permanent infrastructure that the
   real fix would have replaced.
2. **Hidden tech debt**: We mark something "good enough for now" with no
   concrete progression to "doing it properly" and no scheduled removal.
3. **Sequencing inversion**: We build infra X to support Y before
   verifying that Y is the right next thing — investing weeks/months in
   prep that won't translate to the real end-state.
4. **Architecture lock-in to a shortcut**: We pick a path because it's
   what we can implement this week, not because it's right; later phases
   inherit the suboptimal shape.

A *non-crutch* is:
- The proper root-cause fix is unavailable (e.g. sub_51F4D0 is
  SecuROM-thunked and provably non-callable — so we can't initialise the
  orphan from the engine factory entry; we have to manually mirror the
  registry).
- The work is an explicit derisk that informs (and will be replaced by)
  the proper fix.
- The choice is documented as transitional with a concrete progression.

---

## Anti-bias / "make sure you don't confuse something"

This work has shipped GREEN tests. It is tempting to call it "fine".

- Do not be lulled by the green test. RULE №1 is about ARCHITECTURE, not
  test pass/fail.
- Do not assume the documented "next step" actually happens. If a doc
  says "Tier 2 next session" with no concrete commitment date, treat it
  as aspirational. Verify the maintainer has not been adding Tier-1
  workarounds for 10 sessions and "Tier 2 next session" is in every
  checkpoint.
- Do not be intimidated by the depth of the RE. Depth ≠ correctness of
  approach. Heavy RE on a shortcut is still a shortcut.
- Do not confuse "we couldn't do it the proper way" with "we shouldn't
  have tried the proper way". The proper way might exist; check.

---

## Your assigned lens

You will be told which lens (A or B) you have. Stay in your lane:

- **Lens A — Per-step tactical crutch audit.** Walk b7.5 → b7.6 → b7.7 →
  b7.8 → b7.9 → b7.10 → b7.11 step by step. For each, ask: was this the
  proper root-cause fix, or a workaround? If workaround, is there a
  documented progression to a proper fix that will REPLACE it? Or is the
  workaround now permanent infrastructure? Flag specific lines/modules.

- **Lens B — Strategic sequencing crutch audit.** Step back from the
  per-step view. Ask: should we be doing Phase 2 (keep-alive in single
  process) at all before Phase 1 (network layer)? Are we building infra
  that will translate to the real 2-machine network case, or building
  infra that'll be thrown away once we add a real second player? Is the
  "orphan + filter" model a fundamentally different beast from
  "two real players over UDP", and if so, are we wasting weeks?

---

## What I want from you

Three short sections:

1. **Verdict (1-2 sentences)**: "Approach is a crutch in respects [X, Y]",
   or "Approach is NOT a crutch because [reason]". Take a stance.
2. **Evidence (bulleted, specific)**: For each crutch-flag, cite file:line
   or doc-section. For each defence, same.
3. **Recommendation (1 paragraph)**: What concrete change in approach
   would un-crutch the project? (If no change needed, say so.)

Aim for under 700 words. Be willing to disagree with the maintainer's
documented plan if the evidence warrants.

---

## What you should NOT do

- Re-do the technical audit (that's already done).
- Re-implement any code.
- Speculate about features beyond coop.
- Rubber-stamp because the tests pass.
- Refuse to flag things as crutches out of politeness.
