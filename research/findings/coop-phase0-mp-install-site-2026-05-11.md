# Coop Phase 0 — MP install site + activation gate found (2026-05-11)

Follow-up to [coop-phase0-replication-primitive-found-2026-05-11.md](coop-phase0-replication-primitive-found-2026-05-11.md). Continues the next-session candidates from that doc.

## Status: GREEN — MP path fully characterized at the install layer.

Goal of this session was to answer "if/how can we enable MP from the mod side?" The answer: yes, the path is clear and concrete. The infrastructure exists, but the activation has been compiled out at two specific points.

## Install site: `entity_install_network_manager` @ 0x5B0C70

```c
void entity_install_network_manager(Entity* this, int arg) {
    // GATE 1: global MP coordinator must exist
    if (!g_mp_coordinator_ptr) return;             // 0x745BE8
    // GATE 2: coordinator must report MP active
    if (!g_mp_coordinator_ptr->vtable[5]()) return;
    // INSTALL: per-class factory creates the manager
    NetworkManager* mgr = this->vtable[49](this);  // offset 196
    this->net_mgr = mgr;                           // entity+216
    if (mgr) mgr->vtable[3](mgr, this, arg);       // post-install register
}
```

Called from at least 7 sites (entity-construction paths), including `sub_4FF310` (an entity-spawn function allocating 3724 bytes).

## Activation gate: `g_mp_coordinator_ptr` @ 0x745BE8

A single dword global at `g_scene_cvar_block + 0xA90` (absolute `0x745BE8`).

**12 readers, ZERO writers in the binary.**

The slot is reserved and consumed everywhere it matters (publish guards, time-source selection, election logic), but **no code ever writes a non-null value into it**. Therefore in single-player it stays at its zero-initialized value forever, and every gate fails.

Consumer functions (12 read sites):
- `entity_install_network_manager` (0x5B0C70) — install gate.
- `sub_59D7A0` (0x59D81C) — result-mask gate.
- `sub_59DAA0` (0x59DB06) — same gate.
- `sub_5A24E0` (0x5A24E0) — used in input-history buffer logic.
- `sub_5A2530` (0x5A253C) — used in input-history compare.
- `sub_5A2590`, `sub_5A2600`, `sub_5A2980`, `sub_5A2AC0` (twice) — input/election logic.
- `sub_5A6240` (0x5A6328) — time-source selector: if MP, use coordinator's vtable[9] (offset 36) for network time; else use `dword_6FFCB4`.
- `sub_5A6380` (0x5A6460) — same.

This is a textbook stripped-MP pattern: the activation code (the line that wrote a coordinator pointer at engine init) was removed, but all the consumers remain in the final binary.

## MultiplayerCoordinator interface (partial reconstruction)

From consumer call sites, the global coordinator class has at minimum:

| vtable offset | Slot | Purpose |
|---|---|---|
| 20 | 5 | `bool IsMPActive()` — global activation check |
| 36 | 9 | `int GetNetworkTime()` — used in lieu of `dword_6FFCB4` for time-sync |
| 60 | 15 | `int Method_15()` — used in input election (sub_5A2980) |
| 76 | 19 | `bool ShouldElectInput(int, int)` — input gate in sub_5A2980 |
| 92 | 23 | `int Method_23()` — alternative input election path |

Other slots almost certainly exist but were not exercised by the consumer paths I sampled.

## Per-class factory: vtable[49]

The per-entity-class factory is at `vtable[49]` (offset 196). It returns the network manager for the class (or null to opt out).

| Class | vtable[49] | Implementation |
|---|---|---|
| **Protagonist** | `sub_474E90` | `xor eax,eax; ret` — **null stub** |
| **wilbur** | `sub_5AD9F0` | Different function (list walker — NOT a network factory; means slot 49 has a different meaning in Wilbur's vtable layout) |
| (others) | varied | Not all verified — likely most use `sub_474E90` |

`sub_474E90` is referenced from 200+ vtable slots across many classes as a generic "intentionally null" placeholder.

**The Protagonist confirmation is decisive**: the player class's network-manager factory is the null stub. In SP, even if Gate 1 (`g_mp_coordinator_ptr`) was somehow satisfied, the factory would return null and `entity+216` would stay null — replication still inert.

The vtable[49] meaning seems class-specific (different non-network functions in some classes), which suggests the original MP build either patched these factory slots in-place at link time or used a different mechanism we haven't seen. The simplest interpretation: the factories were originally MP-only code paths that were stripped, with the slot reused or left as the null stub.

## Per-frame vs reset behavior of publish/receive

- `entity_publish_distributed_state` — 1 caller: `entity_reset_and_publish` (slot 13 — fires on respawn/init, not per-frame).
- `entity_receive_distributed_state` — 2 xrefs, both vtable entries, not direct calls. Receive is called from elsewhere when state arrives — managed by the network manager's commit cycle.
- `entity_publish_netactor_transforms` — 1 caller: `entity_reset_and_publish` only.

**Bulk replication is event-driven, not tick-driven.** Publish fires on entity reset; per-frame transform delta is presumably handled inside the network manager's commit cycle (vtable[10] commit, which the engine code calls per-frame from the entity's reset flow). The network manager owns its own tick discipline.

## What this means for Phase 1 (revised again)

The path to enable MP from the mod is concrete and three-tiered:

1. **Install a synthesized MultiplayerCoordinator at 0x745BE8.** Single dword write to the global slot. The coordinator class needs at minimum vtable[5] (IsMPActive returns true) and vtable[9] (GetNetworkTime returns synced time).

2. **Patch or override Protagonist::vtable[49].** Either:
   - Write our factory function pointer into Protagonist's vtable (in-process — patch the vtable at the correct VA before player spawn).
   - Hook `entity_install_network_manager` at 0x5B0C70 to bypass the original factory call and inject our manager.
3. **Implement the NetworkManager class.** vtable[10] commit, vtable[11] get-write-buffer-by-name, vtable[12] get-read-buffer-by-name. Underlying transport is our UDP stack from Phase 1.

Estimated effort: **NetworkManager and MultiplayerCoordinator together are ~1-2 wk of focused work**, assuming the UDP transport is already in place. They're small classes (each has under 10 vtable methods that are actually called).

## Concrete Phase 1 design implications

- **Phase 1 transport** stays at 4 wk for the UDP build.
- **Phase 1 also gains "wire up the two manager classes"** — adds ~1-2 wk to Phase 1, but **saves the corresponding 1-2 wk that would have gone into Phase 3 design**. Net: Phase 1 grows slightly, Phase 3 shrinks substantially.
- **Phase 3 (replication)** is now mostly "make sure the publish/receive sites are called at the right times and the buffers cross the wire correctly." Estimate: **1 wk** (was 4 wk).

## Risks / open questions

1. **vtable[49] semantics may vary per class.** Wilbur's slot 49 is a list-walker, not a factory. Need to verify whether `entity_install_network_manager` is called on Wilbur entities at all (if it is, the list-walker would run instead of a factory — possibly a crash or unintended behavior).
2. **Coordinator vtable may need more than 5/9.** Other vtable[15/19/23] uses suggest input-election callbacks. If we don't implement them, the input path may break in MP mode.
3. **Original MP build is unknown.** If there was a Disney-internal build that DID activate this MP path, the missing factories may have lived in code that was excised. We're reconstructing from consumer signatures only.
4. **Save system may use the same publish path.** Phase 0B (save-system RE) may interact — if `DistributedState` doubles as save-snapshot, we need to differentiate net publish from save publish.

## Engine VAs (added this session)

| Address | Symbol | Purpose |
|---|---|---|
| 0x5B0C70 | `entity_install_network_manager` | Install function — writes to entity+216 |
| 0x474E90 | (null stub) | `xor eax,eax; ret` — used at 200+ vtable slots incl. Protagonist::vtable[49] |
| 0x745BE8 | `g_mp_coordinator_ptr` | Global MP coordinator slot (12 readers, 0 writers in binary) |
| 0x745158 | `g_scene_cvar_block` | Base of scene cvar block (slot above is at base+0xA90) |

## Next-session candidates

1. **Build a stub MultiplayerCoordinator class** in mtr-asi and write its pointer to 0x745BE8. Hook the coordinator's vtable[5] to return true. See if the engine boots into "MP mode" (consumer side-effects).
2. **Verify whether entity_install_network_manager is invoked on Protagonist or only on other classes.** Determines whether the player-class factory path is the right hook point.
3. **Walk all 7 callers of entity_install_network_manager.** Map which entity classes get the install treatment.
4. **Phase 0B (save-system RE).** Last Phase 0 item. Important to understand because of potential overlap with DistributedState path.
