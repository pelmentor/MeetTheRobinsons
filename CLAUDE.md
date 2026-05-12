# Project rules for Claude

## RULE №1 — No crutches, no quick fixes

Always pick the proper root-cause fix. No "good enough", no "we can fix
later", no shortcuts. Weeks or months of work to do it properly is OK.

When you find yourself adding a workaround (filter, skip-if, suppress-X,
catch-and-ignore), STOP. That's a crutch. Identify the root cause and
fix it there.

User direct quote (2026-05-08): *"I don't care if it takes WEEKS or
MONTHS — just do it properly."*

---

## RULE №2 — No migration baggage

Do things cleanly without preserving legacy compatibility or
backward-support for features being replaced. When a feature is replaced
or retired, the old code goes — fully, immediately. No "deprecated but
kept for now", no "feature flags for the old behaviour", no compatibility
shims, no parallel old + new paths "until everything migrates".

When you find yourself writing migration glue, legacy fallbacks, or
"support both modes" code, STOP. That's baggage. Make the change cleanly
and let downstream callers update.

User direct quote (2026-05-12): *"no migration baggage, we do things not
caring if we break support of some features."*

This rule complements RULE №1: RULE №1 forbids shortcuts forward (build
it properly even if slow); RULE №2 forbids shortcuts sideways (don't keep
the old broken thing alive next to the new proper thing).

**Concrete triggers (rule-violations):**
- `// deprecated, kept for now` / `// TODO: remove when X` comments on
  code paths that have a replacement.
- Cmdline flags that exist only to re-enable old behaviour
  ("`-disable-new-thing` to fall back to the old thing").
- Two implementations of the same concept compiled together (e.g.
  old-loader + new-loader, with branching at the call site).
- Type aliases / re-exports kept "for compatibility" with code we own.
- Stub functions that exist solely to satisfy old callers.

**When in tension with RULE №1:** the transitional-crutch exception in
principle 4 (filter retirement) is the bounded exception. A
"transitional crutch with a written retirement plan and gating criteria"
is RULE №2 compliant *only if* the retirement actually happens within
the scoped window. If it doesn't, escalate.

---

## The 7 architectural principles (coop & beyond)

Distilled from MTA's README ([reference/mtasa-blue/](reference/mtasa-blue/)
— 22+ years of retrofitting multiplayer onto a single-player game via
hook-only mods). Apply to every non-trivial architectural decision in
this project. Detail and source quotes in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) "Guiding principles" section.

1. **No modification of original game files.** Hooks, MinHook, vtable
   patches, runtime memory patches — yes. Editing `Wilbur.exe` / `.sx` /
   `.dbl` / assets on disk — NO.

2. **Engine-extension paradigm.** mtr-asi is an engine on top of MTR's
   engine, not "a few hooks". Modules own lifecycles; clean APIs between
   subsystems; stay behind well-defined boundaries with the base engine.

3. **Parallel class hierarchy mirroring engine structures.** For coop:
   `MtrRemotePlayer` (our class, owns network/interp/input) + orphan
   entity (engine class, owns rendering/animation) connected by pointer.
   Same shape as MTA's `CClientPed::m_pPlayerPed → CPlayerPed*`
   (CClientPlayer is a subclass of CClientPed for remote players).

4. **Targeted crash fixes, not broad suppression.** Each NULL-deref or
   single-player-assumption gets its OWN patch/init/route at THAT call
   site. NEVER a broader "unlink all candidates" mechanism. The historical
   `coop_orphan_filter` was the anti-pattern, retired 2026-05-12 (per
   [coop-filter-retirement-plan-2026-05-12.md](research/findings/coop-filter-retirement-plan-2026-05-12.md))
   and replaced by `coop_registry_mirror` (proper registry-clone fix) +
   per-site routes like `coop_vibrate_route` (proper C1 fix).

   **Transitional-crutch exception** (compatible with RULE №1):
   - You may keep a broader-suppression mechanism installed temporarily
     IF AND ONLY IF a written retirement plan exists with: (a) enumerated
     concrete crashes it masks, (b) per-crash targeted-fix strategies,
     (c) explicit retirement gating criteria. The filter qualifies; new
     additions of this shape do not.
   - "Temporarily" means measured in sessions, not months. If a
     transitional crutch hasn't shrunk in N sessions, it's becoming
     permanent — escalate to a re-design.

5. **Minimum viable subset.** Coop scope is "two players walk + interact
   through existing levels". See [docs/COOP_SCOPE.md](docs/COOP_SCOPE.md).
   Anything not in scope is NOT replicated.

6. **Augment SP, never replace it.** Where coop meets SP, prefer
   per-player routing inside the SP system over bypassing SP wholesale.
   Players will encounter SP content first; SP must keep working.

7. **Engine-wrapper layer ≠ gameplay/network layer.** MTA splits
   `Client/game_sa/` (engine-side wrappers, one C++ class per engine class,
   no network/gameplay logic) from `Client/mods/deathmatch/logic/`
   (gameplay state, network packets, scripting, interp). This boundary
   is what lets either side change without breaking the other. For
   mtr-asi: engine-wrapping code (entity layouts, vtable thunks, registry
   primitives) lives under one module subtree; gameplay/network code
   (`MtrRemotePlayer`, packet handling, input buffer) lives under a
   different module subtree. They talk via clean APIs, not shared globals.
   Concrete trigger: a new file in `src/coop/` that BOTH dereferences
   engine VAs AND owns network state is a principle-7 violation — split it.

---

## How these rules interact with everyday work

**Before adding a new module/file** — does it fit the engine-extension
paradigm (principle 2)? Does it have a clear API boundary?

**Before adding a "filter" / "skip" / "bypass" mechanism** — that's
probably principle 4 territory. Find the specific crash site and patch
THERE, not broadly.

**Before replicating engine state per-player** — check principle 5. Is
the thing being replicated actually in scope?

**Before editing a game file on disk** — STOP. Principle 1.

**Before assuming the prior session's design was right** — re-verify
against the 6 principles. The b7.10/b7.11 filter expansions were
principle-4 violations that shipped GREEN tests; an audit caught them.

---

## Other standing rules

- **Document findings + rename functions in IDB** — every RE finding:
  rename in IDA AND update `research/findings/*.md` before declaring done.
- **Verify, don't guess** — list / grep / fetch artifacts before
  describing them.
- **Don't reinvent the wheel** — use established libraries (MinHook,
  ImGui, DXVK, etc.).
- **The mod menu uses Dear ImGui** — overlay decouples display fixes
  from native-menu RE.
- **No emojis in code / files** unless explicitly requested.

---

## Reading order after `/compact`

1. `MEMORY.md` index (auto-loaded).
2. Top entry of `memory/` (latest project state).
3. `CLAUDE.md` (this file — but already loaded).
4. `docs/ARCHITECTURE.md` if doing architecture work.
5. `reference/mtasa-blue/` if doing coop architecture work.
