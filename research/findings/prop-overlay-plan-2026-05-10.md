# Prop visual debug overlay plan — disassembleable / scannable / targetable props

**Date:** 2026-05-10
**Revision:** v2 (post-audit; architect `afbf879e`, engineer `aa16864a`)
**Governing rule:** RULE №1 — proper fix, no shortcuts.
**Sister to:** [trigger-box-overlay-plan-2026-05-09.md](trigger-box-overlay-plan-2026-05-09.md), [npc-overlay-plan-2026-05-09.md](npc-overlay-plan-2026-05-09.md). Reuses the projection scaffold, overlay_math.h, autonomous-validation pipeline.

**v2 changes from v1 (5 blocking items both audits caught):**
- **Walker offset must be VERIFIED, not inherited.** The npc_overlay walker uses `node+0x5C` for the entity pointer, but `interp_npc.cpp:92-93` reads BOTH `+0x40` (entity, used for player-skip) AND `+0x5C` (inner). The offsets are not yet fully reconciled. Phase 0A now logs both for one prop entity and confirms which one carries the `+0x50` name + `+0x58` pos and accepts the `entity_property_get_thunk` ECX argument with valid kv resolution. If `+0x5C` is wrong for prop nodes, the kv ECX is garbage and every kv call returns null silently — overlay appears to work but shows nothing.
- **Phase 1 single-key default committed.** v1 said Phase 1 calls "other prop keys per show-toggles" while Risk R1 mitigation said "start with single-key". Resolved: Phase 1 = `propDisassembleable` only. Other tags gated to Phase 2 with profiling instrumentation in Phase 1 export mode (per-frame `walk_time_us` log line) to drive Phase 2 budgeting decisions.
- **Per-entity SEH scope.** Walker-scope SEH (npc_overlay's pattern) is acceptable for NPCs but for props where mid-disassembly faults are first-class, per-entity scope is correct. v2 implements: pre-read `node->next` BEFORE the per-entity body, then `__try` the body, on fault `__except` resumes at the next node instead of stranding the walker.
- **Phase 0A test target changed.** Player entity doesn't exist at the main menu; calling `entity_lookup_by_name_retry("player", 1)` returns null. v2's Phase 0A: enable `npc_overlay` first to populate its diag log with first-16 transform-list entities at main menu; pick a real entity ptr from that log and call `entity_get(real_entity, "class")` to verify the kv ABI. If main menu has zero entities, scenario boots to a level via DI navigation (TODO if needed).
- **Static Phase 0B path** via IDA — find xrefs to the `disassemble` verb string at 0x6B8E70 and inspect the completion handler statically for list-unlink / flag-set evidence, instead of relying on a 30-min interactive gameplay session. Architect's specific recommendation; faster + more reliable.
- **UI surface restructured.** v1's 13 flat controls become: master enable + distance slider + 2 collapsed TreeNodes ("Tag filters" with 8 checkboxes, "Label fields" with 4). Section default footprint = 4 controls. Mirrors the peripheral-cull-probe TreeNode pattern at tab_debug.cpp:416.
- **Render order locked.** `trigger → npc → prop`. Prop draws LAST so disassembly targets emphasize over NPC labels (prop labels are the high-value signal per stated use case).
- **`const char*` return type committed** (not `void*`) — engine call sites uniformly treat kv result as string-or-null.
- **`entity_kv.cpp` confirmed as separate TU**, not header-only or inline. SEH-wrapping the __thiscall trampoline is a single edit point.

---

## TL;DR

Render a 3D-projected debug overlay showing each PROP entity in the world (scannable / targetable / disassembleable / climbable / push-pullable / levitate / lock-to-path), labeled with its name + which prop tags apply + (when applicable) disassembleable level / scanner name. **Death is free**: when Wilbur's disassembler destroys a prop, the engine removes the entity from the transform list at `dword_724DE4`. The overlay re-walks the list every frame, so a dead prop's label naturally disappears next frame. No "death event" hook needed.

Useful for: locating disassemble targets in a complex scene (some are very small / behind geometry), debugging puzzle-state ("which crate is `propPushPullable` and which is decorative geometry"), validating that a script's `propDisassembleable` is actually set on the right entity, and rapid iteration on level design.

---

## Why we want it

- The disassembler is one of Wilbur's signature mechanics. Finding which world objects respond to it is a core gameplay-debug need that currently requires reading `.sc` level files by hand.
- Props are a large taxonomy (50+ `propXxx` properties enumerated in the string table at 0x6A8814..0x6C166C) with overlapping memberships. A visual overlay that shows the union of prop tags per entity makes the entire system legible.
- We already have the projection scaffold (trigger + NPC overlays shipped 2026-05-09), the entity walker pattern, and the autonomous validation pipeline. Adding the prop channel is composition, not new infrastructure.

---

## What we know already

### Prop properties (string evidence, verified in IDA)

| String VA | Property | Meaning |
|---|---|---|
| 0x6A8830 | `propScannable` | Scanner-targetable; lights up with `propScannableName` + `propScannableIcon` + 3D offset |
| 0x6A8840 | `propTargetable` | Weapon-lock targetable; `propTargetablePriority` and `propTargetablePriorityOverride` for selection |
| 0x6C1648 | `propDisassembleable` | Disassembler-destroyable; `propDisassembleableLevel` for tier gating |
| 0x6A9EC0 | `propClimbable` / `propClimbMarker` | Climbable; with marker pos |
| 0x6AC9DC | `propPushPullable` | Physics-interactable |
| 0x6ACAE8 | `propLockToPath` | Path-constrained motion |
| 0x6AD164 | `propSSTarget` | Superstep-attack target |
| 0x6AEC10..0x6AEE6C | `propLevitate*` (~14 sub-keys) | Levitate (Mega Doris weapon) parameters |

### Disassembler weapon (string evidence)

- Variants: `disassembler` / `disassembler1` / `disassemblerMegaDoris`
- Actions: `DisassemblerShoot`, `DisassemblerShootGroup`, `DisassemblerIdle`
- FX callbacks: `DisassemblerFX{Start,Reset,SetAlpha,Fail}`, `DisassemblerFXAddActor`, `DisassemblerCenterBoundingSphere`, `DisassemblerCrateCableHack`
- Verb: `disassemble` at 0x6B8E70 — likely the script command name dispatched on hit-confirm

**Death model (assumed; verified empirically by Phase 1 testing):** when the disassembler completes its destroy on a prop, the entity is removed from the engine's active list (and from the transform list). The overlay's per-frame walk naturally stops drawing that prop. If the engine instead just *flags* a prop as dead while keeping it in the list, Phase 1 will show this as "labels persist on dead props" and we add a flag-check in Phase 2.

### Entity property accessor (verified in IDA)

`entity_property_get_thunk` at `0x004B8F00`. Calling convention `__thiscall(entity, const char* key) → const char*`:
- ECX = entity pointer
- Stack: pointer to NUL-terminated key string
- Returns a value pointer (typically a `const char*`); NULL if key not present.
- Reached via stolen-byte IAT thunk (`g_securom_thunk_table_base + 178618`) — destination is decompilable per project memory; the thunk is itself just a forwarder.

171+ existing call sites in the engine. Safe to call from the render thread (engine is single-threaded sim/render).

### Existing reusable infrastructure

- [`include/mtr/overlay_math.h`](../../src/mtr-asi/include/mtr/overlay_math.h) — shared math (`row_mul`, `mat_mul`, `point_in_frustum`, `ndc_to_screen` with D3D9 Y-flip, `clip_segment`).
- [`src/mtr-asi/src/npc_overlay.cpp`](../../src/mtr-asi/src/npc_overlay.cpp) — closest sibling. Same walker, same matrix capture, same SEH guard pattern. Differs only in (a) what it filters and (b) what fields it shows.
- Walker template ([interp_npc.cpp:84-100](../../src/mtr-asi/src/interp/interp_npc.cpp#L84-L100)): `dword_724DE4` head, node `+0x04`=next, `+0x44`=flags (skip-bit `0x10`), `+0x5C`=entity pointer.
- Entity offsets confirmed by [research/findings/entity-system.md](entity-system.md) + [research/findings/player-entity-layout-2026-05-09.md](player-entity-layout-2026-05-09.md): `+0x50`=name (heap char*), `+0x58`=pos vec3, `+0x48`=render sub.
- Autonomous validation: [`tools/run-test.ps1`](../../tools/run-test.ps1), `tools/validate-overlay-frames.ps1` pattern, `test_harness.cpp` scenario template.

---

## Architecture

A third sister module, mirroring `npc_overlay.cpp` exactly. The only structural difference is the per-entity filter (kv check for one or more prop properties) and what the label shows.

```
                 menu::on_end_scene → ImGui::NewFrame
                            │
                            ▼
       ┌──────────────────────────────────────────┐
       │  trigger_overlay::tick(dev)              │  (shipped)
       │  npc_overlay::tick(dev)                  │  (shipped)
       │  prop_overlay::tick(dev)                 │  (NEW — sister)
       └──────────────────────────────────────────┘
                            │
                            ▼
       Read view + proj (SEH-guarded; reuse existing pattern)
       Read cam_pos from world matrix row 3 (0x00724C50)
                            │
                            ▼
       walk_transform_list:
         for each entity at node+0x5C:
           - SEH guard the per-entity body
           - Validate name at +0x50 (strict ASCII, NUL within 64)
           - Read pos via *(entity+0x48)+0x10 with +0x58 fallback
           - Call entity_property_get_thunk(entity, "propDisassembleable")
                                         and other prop keys
           - SKIP if no prop tags found
                            │
                            ▼
       For each prop entity:
         - Project pos to clip space
         - point_in_frustum (full 6-plane test)
         - NDC → screen with Y-flip
         - Build label: "<name> [tags...] (pos) d=<dist>"
         - ImGui::AddText() at projected screen pos
         - Optional: distance fade
                            ▼
       ImGui::Render() → next frame walks again
       (dead prop = removed from list = no label = "info dies")
```

### Module layout

- `src/mtr-asi/src/prop_overlay.cpp` — new module, ~400 LOC (very close to npc_overlay structure)
- `include/mtr/prop_overlay.h` — public API
- Wire into [menu.cpp](../../src/mtr-asi/src/menu.cpp): `prop_overlay::tick()` called between `npc_overlay::tick()` and `ImGui::EndFrame()`. `any_draw` flag OR-includes `prop_overlay::enabled()`.
- UI section in [tab_debug.cpp](../../src/mtr-asi/src/menu/tab_debug.cpp): toggle + per-tag-show toggles + distance limit.

### kv accessor wrapper (NEW shared helper)

Small new file `src/mtr-asi/src/entity_kv.cpp` (~50 LOC) wrapping the `__thiscall` invocation in a SEH guard with the calling convention spelled out in C++ inline assembly or via a typedef. Exposed as:

```cpp
namespace mtr::entity_kv {
    // Returns the value string for `key` on `entity`, or nullptr if the
    // key is absent / fault occurs / entity is invalid.
    const char* get(void* entity, const char* key);
}
```

Used by `prop_overlay` (and future modules) so we have one centrally-tested call site for the SecuROM-thunk'd accessor.

### Public API sketch

```cpp
namespace mtr::prop_overlay {

bool enabled();
void set_enabled(bool v);
int  visible_prop_count();   // last-frame count for UI

// Per-tag visibility filters. Each is checked per-entity per-frame via
// kv-get. Default: only disassembleable shown (the user's primary use case);
// other tags off by default to avoid label clutter on dense scenes.
bool show_disassembleable();   void set_show_disassembleable(bool v);
bool show_scannable();         void set_show_scannable(bool v);
bool show_targetable();        void set_show_targetable(bool v);
bool show_climbable();         void set_show_climbable(bool v);
bool show_push_pullable();     void set_show_push_pullable(bool v);
bool show_levitate();          void set_show_levitate(bool v);
bool show_lock_to_path();      void set_show_lock_to_path(bool v);
bool show_ss_target();         void set_show_ss_target(bool v);

// Per-prop label fields.
bool show_name();              void set_show_name(bool v);
bool show_pos();               void set_show_pos(bool v);
bool show_distance();          void set_show_distance(bool v);
bool show_tags();              void set_show_tags(bool v);   // bracketed [tag] suffix

// Distance fade (engine units). 0 = always opaque.
float distance_limit();        void set_distance_limit(float v);

// Autonomous-validation export.
void set_export_frames(int n);
int  export_frames_remaining();

void tick(IDirect3DDevice9* dev);

} // namespace mtr::prop_overlay
```

---

## Phase plan

### Phase 0 — Fact-finding (cap: 1 hr direction-eval)

#### 0A — `entity_property_get_thunk` calling-convention ABI verification (cap: 20 min)
- Write a tiny test: call `entity_get(player, "class")` from inside our scenario harness on the main menu. Player is at `entity_lookup_by_name_retry("player", 1)`.
- Verify EAX is non-null (player has `class = "player"`).
- Verify the returned string is readable.
- **Output:** typed wrapper `mtr::entity_kv::get(entity, key)` with confirmed signature. Saves us from re-discovering this for every future feature.

#### 0B — Death model verification (cap: 30 min)
- Manual or scripted test: load a level with disassembleable props (e.g. WilburHome). Observe transform list contents before disassembling a target. Disassemble the target. Observe transform list contents after.
- **Outcome A (preferred):** the destroyed prop is removed from the transform list. Phase 1 ships unchanged.
- **Outcome B (fallback):** the prop stays in the list with a flag set. Identify the flag offset. Phase 1 adds a flag-skip in the walker.

This phase is gameplay-required (autonomous testing can't disassemble a prop). User runs the test interactively; alternative is Phase 1 ships and the user reports back whether labels persist on dead props.

#### 0C — `propDisassembleable` value semantics (cap: 10 min)
- Read the string returned by `entity_get(some_disassembleable_prop, "propDisassembleable")`. Is it a level/tier integer-as-string ("1", "2"), a boolean ("true"), or just non-null-vs-null as a flag?
- **Output:** documented value semantics for the label format string.

### Phase 1 — Core overlay (cap: 3 hr)

Acceptance: in a level with disassembleable props (e.g. Robinson manor, WilburHome), each prop renders a yellow label at its world position showing name + (pos) + distance. Disassembling a prop makes its label disappear within 1 frame. Performance: < 1 fps drop with 50+ props in scene.

Implementation steps:
1. Create `entity_kv` wrapper module (Phase 0A output).
2. Create `prop_overlay.cpp` based on `npc_overlay.cpp` structure (copy+adapt; both are sister modules so duplication is ~70%, but the differences are real semantic differences not RULE-№1-violating duplication).
3. Walker calls `entity_kv::get(entity, "propDisassembleable")` (or other prop keys per show-toggles). If any returns non-null, render label.
4. Default: only `show_disassembleable=true` and `show_name=true`, `show_pos=false`, `show_distance=false`, `show_tags=true` so the label reads `<name> [disassembleable]`.
5. UI toggle in tab_debug.
6. Autonomous scenario `prop-overlay-phase1-verify` boots to main menu, enables overlay, arms 60-frame export. Main menu has 0 entities so this only validates walker safety + `entity_kv` no-fault path.

### Phase 2 — Tag-set rendering + click-pin (cap: 3 hr)

Acceptance: each prop label shows ALL applicable tags (e.g. "[disassembleable, targetable, scannable]"). Click on a label to pin a sticky panel showing all kv values for that entity (full kv enumeration via... TBD — kv enumeration may need an iterator function we haven't found yet; if not, dump the known prop tags).

### Phase 3 — Cross-system polish (cap: 2 hr)

- Per-tag color coding (disassembleable = yellow, scannable = cyan, targetable = red, ...).
- Off-screen indicators? (consistent with NPC overlay decision — maybe not.)
- Filter by prop level (`propDisassembleableLevel`) — show only level-N+ props for "what can my current weapon hit" view.

### Phase 4 — Stress + edge cases (cap: 1 hr)

- Test in 5+ levels.
- Verify SEH guards fire on corrupted entities mid-disassembly animation.
- Performance check: 200+ entities × 8 prop keys = 1600 kv calls per frame. Profile.

---

## Risks

- **R1: kv calls per frame are expensive.** 200 entities × 8 prop keys = 1600 kv calls. Each kv call is a hash lookup + bucket scan. If profiling shows this >0.5ms/frame, cache: per-entity per-key result with invalidation on entity-pointer change (entities don't change kv during their lifetime). Mitigation: structural — start with single-key (`propDisassembleable` only), add others as proven-needed.
- **R2: Death model is "flag set, not removed".** Then walker draws labels on dead props. Phase 0B catches this; Phase 1 implements the flag check. The flag is likely the visibility flag at `+0x50` byte (per entity-system.md) or a separate "destroyed" flag at a TBD offset.
- **R3: kv accessor faults on a corrupted entity.** Mitigation: SEH-wrap the per-entity body (already standard pattern from npc_overlay).
- **R4: Some prop entities have no `+0x50` name.** Per audit feedback on npc_overlay: heterogeneous entity layouts mean `+0x50` may not always be a name pointer. Strict ASCII validator from npc_overlay handles this — labels just show `(unnamed)` or fall back to entity-pointer hex.
- **R5: Disassembler removes entity from transform list at `+0x44 |= 0x10` instead of unlinking from `+0x04 next` chain.** Then walker sees entity but skips it because of the flag — also fine; overlay correctly stops rendering.
- **R6: Two sister modules with 70% overlap.** A2 in alternatives below considers merging into a single "world overlay" module. Decided against per the npc_overlay audit (different filter surfaces, different update cadences). Audit this v1 plan to confirm.

---

## Alternatives considered

- **A1: Read prop tags from `.sc` level files at boot.** Static; doesn't reflect destroyed props. Rejected.
- **A2: Merge npc_overlay + prop_overlay into one module.** Considered. Rejected — different filter surfaces (npc = "everything in transform list except player", prop = "filtered by kv tags"), different update cadences, different per-tag UX. Cleaner as siblings.
- **A3: Hook the disassembler success function and unregister the prop's overlay slot.** Premature — the per-frame walk handles death naturally if outcome A holds. Only needed if outcome B + we want zero-frame-latency on label removal.
- **A4: Use the engine's built-in scanner UI / disassembler-target-FX as the prop indicator.** No — those are designed for in-game use, not debug. We want labels visible regardless of weapon equipped.

---

## What the audit needs to evaluate

1. **Architecture audit** — is `entity_kv.cpp` the right factoring (vs inline in prop_overlay)? Is the per-tag-toggle UI surface coherent? Should `prop_overlay::tick()` come BEFORE `npc_overlay::tick()` so prop labels draw under NPC labels (better z-order for player-character overlap), or AFTER so disassembly targets are emphasized? Cross-system: does kv-reading from EndScene race with anything?
2. **Engineering audit** — kv-call performance estimate (1600 calls/frame). SEH guard on the per-entity body — sufficient, or does kv itself need its own narrower SEH? `propDisassembleable` non-null-vs-null check semantics — is this RULE-№1-correct, or is there a cleaner predicate? Death-model assumption (entity-removed-from-list) — is there a cheap way to verify without disassembling a prop in autonomous testing?

---

## Phase 0 outcome (empty until executed)

(Populated as Phase 0 progresses.)
