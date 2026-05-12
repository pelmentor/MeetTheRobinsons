# Coop Phase 0C — .sx script command catalog (2026-05-11)

**Status:** SHIPPED (catalog extracted; classification table drafted; **major new finding**: engine has vestigial multi-player scaffolding in the script VM).
**Predecessor:** [ai-script-vm.md](ai-script-vm.md) (partial — turns out to be wrong about format), [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md) v2 plan item "Phase 0C — Full .sx script command catalog".
**Governing rule:** RULE №1 — root-cause-first. The classification is the input to Phase 5 design.

---

## TL;DR

1. **Plan's assumption was wrong**: `.sx` is **NOT a text-based command language**. The files in `Game/data/scripts/*.sx` start with `SLNG` magic + 8-byte header + bytecode. The `console_run_text_script`/`console_dispatch_line` pipeline documented in [ai-script-vm.md](ai-script-vm.md) handles a *different* surface (inline event strings embedded in C++); the on-disk `.sx` is compiled bytecode whose source `.sla` files are not shipped.

2. **Catalog extracted by string-mining**: 184 files, **7,640 distinct identifier-shaped tokens** across the bytecode tables. Tool: [tools/extract_sx_identifiers.py](../../tools/extract_sx_identifiers.py). Full ranked dump: [tools/sx_identifiers.txt](../../tools/sx_identifiers.txt).

3. **Major new finding — engine has more multi-player than v2 plan assumed**. The .sx layer exposes:
   - `IsMultiPlayer` (2 files) — explicit MP mode check.
   - `GenericNetActor`, `PathFollowerNetActor`, `ActorGetNetMaster` (5/1/13 files) — networked actor infrastructure.
   - `Players_GetActor`, `Players_GetAvatar`, `Players_GetActorHandle`, `Players_GetClosestAvatar` (51/31/13/2 files) — plural-player API with player-index addressing.
   - `Async__player`, `Async__playerDir/Pos/Vel/Dist`, `Async__readyToAttack`, `Async__stopMoving` — explicit async player-tracking state machine.
   - Mini-game multi-player placements: `SchoolYardPlacePlayers`, `CyrogenicPlacePlayers`, `GardenPlacePlayers`, `Gym_player0..3`, `Cyro_player0..2`, `ShclYrd_player1..2` — up to 4-player support in the Gym mini-game.
   - `GetPlayerIndex`, `SetPlayerLost`, `SetPlayerWon`, `MiniHamsterCreatePlayer`, `HidePlayers`, `ClearPlayerStatuses`, `MiniHamsterRestorePlayers` — multi-player session management.
   - `recv` symbol (1 file, name only) — vestigial network primitive.

   **This changes Phase 2 (second-player spawn) and Phase 3 (replication) cost estimates downward.** Before the v2 plan committed "1 wk → 4 wk Phase 2 scope of 'patch every input dispatch site'", we now know the script VM already addresses players by index, so the C++ side may have more wired than the audit's `g_input_mgr` analysis revealed. Phase 2 needs a fresh look.

4. **Classification table below** sorts ~150 distinguished verbs by category (Replicate / Cosmetic / Forbidden / Local / Pure / Net-primitive). This is the starting input for Phase 5 design.

---

## Format recovery

Each `.sx` file starts with:

```
00000000: 534c 4e47 0a10 0000 7856 3412 ....
          SLNG  hdr=10 magic= 12345678
```

- Bytes 0..3: `SLNG` (Script LaNGuage).
- Bytes 4..7: header size or version (`0x100a` consistently).
- Bytes 8..11: byte-order check (`0x12345678`).
- Bytes 12..end: bytecode + interned string tables + reference paths to the original `.sla` source (e.g. `E:\dev_Wilbur\Wilbur\assets\scripts\levels\holobri.sla`).

The `.sla` sources are NOT shipped with the game. Online dev archives may have them; for now we extract identifiers from the interned-string tables, which is enough for classification.

---

## How to read the catalog table

Each row is a script-callable symbol observed in at least one `.sx` file. Columns:

- **n** = number of distinct `.sx` files containing this symbol (max 184).
- **Category** = the classification (see legend below).
- **Why** = one-line rationale.

**Classification legend:**

| Category | Meaning |
|---|---|
| **REPLICATE** | Mutates world state visible to both players. Host runs authoritatively; sends a `script_event(name, args)` packet to the client which re-runs the same call (or a "applied-result" message). Misclassifying as cosmetic = mission desync. |
| **COSMETIC** | Visible/audible but no gameplay consequence. Both sides run locally against their own replicated entity state. Misclassifying as replicate just wastes bandwidth. |
| **FORBIDDEN** | Must NOT run on the client. Either host-authoritative-only (e.g. save writes) or already replicated via a different mechanism (e.g. transform snapshots). |
| **LOCAL** | Computed from each side's local state. Always-safe to run on both sides; never replicate. |
| **PURE** | Math / utility / data-only. No state mutation. Safe everywhere. |
| **NET-PRIMITIVE** | Existing engine networking primitive. Investigate as part of Phase 1/2 — may already provide the mechanism this plan is designing. |
| **UNKNOWN** | Needs runtime hook to disambiguate. Documented for follow-up. |

---

## Pre-classification: noise drops

These tokens appear high-frequency but are bytecode artifacts or pure infrastructure, not script commands. Excluded from the classification:

- `SLNG`, `xV4`, `GJC`, `k4X`, `KuD`, `UwD`, `Y2v`, `vuA`, `JZk`, `hg_4cc`, `hv14cc`, `lg14cc`, `cg_4cc`, `cg14cc`, `disa4cc` — 3-char bytecode opcode tags or compiler-emitted markers.
- `sender`, `data`, `state`, `time`, `value`, `func`, `act`, `pos`, `dist`, `disa`, `alpha`, `class`, `type`, `delay`, `delta`, `deltaTime`, `prevTime`, `prevGameTime`, `gameTime`, `targetPos`, `myPos`, `playerPos`, `tempVec`, `distVec`, `upVec`, `toFace`, `toTarget`, `body`, `target`, `lev`, `frame`, `iState`, `retVal`, `uVal`, `lVal`, `endDot`, `dead`, `hidden`, `active`, `isActive`, `start_active`, `found`, `lean`, `alert`, `Speaking`, `sound`, `hash`, `offset`, `scale`, `speed`, `acceleration`, `sleepTime`, `timer`, `headControl`, `curState`, `lastState`, `stateName`, `curFollow`, `followBlend`, `lastFollowAngle`, `playerVel`, `playerVelocity`, `playerSpeed`, `playerPosition`, `playerA`, `playerAStatus`, `playerIndex`, `Wait`, `Idle`, `model_name`, `levelName`, `initlevel`, `weaponType`, `deathMessage`, `irrHandle`, `emitterHandle`, `emitterPos`, `particlePos`, `particleEmitterHandle`, `endFade`, `fadeInterval`, `inReact`, `reHit`, `hitByWeapon`, `impactPoint`, `impactFromDir`, `impactWeaponFourCCInt`, `surfaceType`, `STANCE`, `Stance`, `WALKSTOP`, `IDLE`, `True`, `all`, `ASSERT`, `ASSERTS`, `parmset`, `sdata`, `msg`, `senderName`, `sendReceiver`, `sendIt`, `Sending`, `Tutorial`, `players`, `Match`, `Match__*` (ChargeBall mini-game internal state, single mini-game, not a verb). Variables, parameters, constants, and one-off bytecode artifacts.

The remaining identifiers are real script-callable verbs / class names / event names.

---

## Classification table — high-frequency verbs (n >= 10 files)

| n | Symbol | Category | Why |
|---|---|---|---|
| 184 | (`SLNG` magic) | — | bytecode header, not a verb |
| 120 | `ParmGetString` | LOCAL | reads a parameter from the incoming message; both sides do this when handling their own messages |
| 105 | `ParmGetInt` | LOCAL | same |
| 88 | `ParmGetFloat` | LOCAL | same |
| 108 | `SendMessageToAll` | **NET-PRIMITIVE** | broadcasts a message. Existing engine primitive; investigate whether it already has a network path or is purely local |
| 78 | `SendMessageToActor` | **NET-PRIMITIVE** | directed send; same investigation needed |
| 71 | `RouteMessageToFunction` | LOCAL | message-handler binding; pure local table mutation |
| 36 | `SetMessageHandlerRules` | LOCAL | same |
| 33 | `SetMessageDataTranslator` | LOCAL | message-format translator setup; local config |
| 11 | `RemoveMessageHandler` | LOCAL | unbinding handler; local table mutation |
| 125 | `Init` | LOCAL | per-actor init entry point; runs locally as actors come online on each side |
| 101 | `Main` | LOCAL | per-actor main loop entry; same |
| 65 | `Terminate` | LOCAL | per-actor teardown; same (host driven via spawn/despawn replication) |
| 83 | `InitializeComponents` | LOCAL | builds the entity's component stack; runs per-side at spawn |
| 81 | `AttachComponent` | LOCAL | adds a component to local entity; runs per-side |
| 49 | `ActivateComponent` | **REPLICATE** | enables a component — gameplay-visible (e.g. activates a collider). Host runs and emits component-state event for client |
| 33 | `DeactivateComponent` | **REPLICATE** | same |
| 28 | `DeactivateComponentInClass` | **REPLICATE** | bulk deactivate; same |
| 26 | `ActivateComponentInClass` | **REPLICATE** | bulk activate; same |
| 17 | `DetachComponent` | **REPLICATE** | removes a component — same |
| 13 | `IsComponentActive` | LOCAL | pure query of local entity state |
| 77 | `ActorGetPosition` | LOCAL | reads own/observed entity pos; safe both sides |
| 77 | `ActorMarkForDeath` | **REPLICATE** | marks entity for despawn — must replicate or client never removes the actor. Host authoritative |
| 55 | `ActorHide` | **REPLICATE** | hide flag is gameplay-relevant (turns off interaction). Host authoritative |
| 55 | `ActorSetCategoryBits` | **REPLICATE** | category mask drives collision/visibility; gameplay-relevant. Host authoritative |
| 53 | `FindActorByInstanceName` | LOCAL | name-keyed lookup in local entity table; safe both sides |
| 51 | `Players_GetActor` | LOCAL | reads local player table (which is replicated separately) |
| 31 | `Players_GetAvatar` | LOCAL | same |
| 13 | `Players_GetActorHandle` | LOCAL | same |
| 47 | `NO_SUPPORT_CATEGORY` | PURE | category constant |
| 16 | `NO_CATEGORY` | PURE | category constant |
| 44 | `player` | LOCAL | the singular-player-handle convention; will need plural extension in Phase 2 |
| 43 | `ParticleEffectCreate` | COSMETIC | particle FX; both sides run their own |
| 37 | `ParticleEmitterKill` | COSMETIC | same |
| 22 | `ParticleEmitterCreate` | COSMETIC | same |
| 16 | `ParticleEmitterSetPosition` | COSMETIC | same |
| 13 | `ParticleEmitterActivate` | COSMETIC | same |
| 13 | `ParticleEmitterDeactivate` | COSMETIC | same |
| 10 | `ParticleEmitterCreateFromActor` | COSMETIC | same |
| 73 | `SfxPlay` | COSMETIC | sound effect; both sides spatialize against local camera. Per v2 plan, world-positioned sounds with spatial coords replicate via host event |
| 37 | `SfxStop` | COSMETIC | same |
| 21 | `SfxInit` | LOCAL | sound-system init; local |
| 73 | `GameTime` | LOCAL | local frame-clock query; might need host-time-sync if used for gameplay timing (UNKNOWN follow-up) |
| 39 | `GetFrameSec` | LOCAL | local-frame-delta query; safe |
| 35 | `time` | PURE | usually a parameter name, not a verb |
| 42 | `ActionHandler` | LOCAL | per-actor action-state component; local instance |
| 38 | `RegisterActionAnim` | LOCAL | per-actor animation binding table; local |
| 37 | `StartAction` | **REPLICATE** | starts a gameplay action (animation + state transition). Host authoritative; client mirrors via animation snapshot |
| 27 | `EndAction` | **REPLICATE** | same |
| 12 | `GetAction` | LOCAL | query of local action state |
| 29 | `ActionInProgress` | LOCAL | same |
| 41 | `SetCollisionToCapsule` | LOCAL | collision-shape setup; local at spawn |
| 41 | `SetPersistentGlobalData` | **FORBIDDEN** | writes save-state global. **Host-only**; client must never run this. v1 commits to host-save-only |
| 40 | `GetPersistentGlobalData` | LOCAL | reads save-state global from local cache; safe both sides (clients get a replicated read-only view) |
| 17 | `UnsetPersistentGlobalData` | **FORBIDDEN** | same scope as Set; host-only |
| 11 | `usePersistentData` | LOCAL | flag query |
| 40 | `GetRandomReal` | **FORBIDDEN** | RNG. **Host-only** for replicate-affecting reasons; cosmetic-only RNG (e.g. particle jitter) is COSMETIC. Default: forbidden, opt-in to COSMETIC per call site |
| 21 | `GetRandomInt` | **FORBIDDEN** | same default; same opt-in note |
| 40 | `Handle_KillAllAtCheckpoint` | **FORBIDDEN** | mission-progress handler. Host-only; client receives mission-state replicated separately |
| 34 | `KillAllAtCheckpoint` | **FORBIDDEN** | same |
| 39 | `checkPoint` | LOCAL | checkpoint state query/data |
| 36 | `SetMessageDataTranslator` | LOCAL | already covered above |
| 35 | `Vector3Normalize` | PURE | math |
| 52 | `Vector3Sub` | PURE | math |
| 34 | `Vector3Add` | PURE | math |
| 29 | `Vector3Scale` | PURE | math |
| 34 | `Vector3Length` | PURE | math |
| 28 | `Vector3DotProduct` | PURE | math |
| 20 | `Vector3LengthSquared` | PURE | math |
| 14 | `Vector3CrossProduct` | PURE | math |
| 9 | `Vector3Rotate` | PURE | math |
| 14 | `Deg2Rad` | PURE | math |
| 13 | `atan2` | PURE | math |
| 13 | `sin` | PURE | math |
| 10 | `Clamp` | PURE | math |
| 54 | `sprintf` | PURE | string format |
| 51 | `SimpleCollision` | LOCAL | per-actor collision component |
| 31 | `SimpleGroundController` | LOCAL | per-actor ground physics — runs locally, host replicates resulting transform |
| 25 | `KinematicController` | LOCAL | same |
| 19 | `KinematicDriver` | LOCAL | same |
| 20 | `BasicMover` | LOCAL | same |
| 15 | `VelocityController` | LOCAL | same |
| 12 | `MoveAllowForward`, etc. | LOCAL | local-physics gate |
| 27 | `ActorSetPosition` | **FORBIDDEN** | bypasses controllers; gameplay-relevant teleport. Host-only; client receives via transform snapshot |
| 17 | `ActorSetVelocity` | **FORBIDDEN** | same |
| 18 | `ActorGetVelocity` | LOCAL | velocity query of local cached state |
| 27 | `StartMotion` | **REPLICATE** | starts a scripted motion (animation/keyframe). Host emits event, client mirrors |
| 13 | `Stop`, 25 `MotionEnded` | LOCAL | local motion state events |
| 15 | `StopMotion` | **REPLICATE** | same as StartMotion |
| 24 | `SetGroundMaxForwardSpeed` | LOCAL | tuning of local ground controller |
| 22 | `SetGroundMaxTurnSpeed` | LOCAL | same |
| 16 | `Handle_Activate` | **REPLICATE** | "actor was activated" event handler. Host fires; replicate event to client |
| 16 | `Handle_ActivateBot` | **REPLICATE** | same |
| 16 | `HandleD_Activate` | **REPLICATE** | "deactivate" handler variant (same root) |
| 15 | `Handle_Deactivate` | **REPLICATE** | same |
| 15 | `HandleD_Deactivate` | **REPLICATE** | same |
| 53 | `Handle_MunitionImpactActor` | **REPLICATE** | weapon-hit handler. Host runs damage; replicate damage event to client |
| 26 | `GetMunitionImpactIntersection` | LOCAL | helper query |
| 36 | `GetMunitionImpactFourCCInt` | LOCAL | helper query |
| 37 | `GetFourCCIntFromString` | PURE | string→fourcc |
| 23 | `Handle_ActorEntered` | **REPLICATE** | trigger-volume-entered. Host fires, client mirrors (trigger state replicated) |
| 16 | `Handle_ActorExited` | **REPLICATE** | same |
| 22 | `Handle_ManDown` | **REPLICATE** | NPC-down event |
| 15 | `Handle_ActorDied` | **REPLICATE** | actor death event; host authoritative |
| 14 | `Handle_LoadPersistentData` | **FORBIDDEN** | save-load event; host-only |
| 21 | `AipSetVisualRadius` | LOCAL | per-NPC AI perception tuning |
| 21 | `AipSetHearingRadius` | LOCAL | same |
| 21 | `AiPerception` | LOCAL | per-NPC AI perception component |
| 18 | `AipSetAlert` | **REPLICATE** | AI alert state — gameplay-visible (NPC reacts to threat). Host authoritative, replicate via NPC behavior state |
| 13 | `AiwSelectWeapon` | **REPLICATE** | AI weapon choice — same |
| 14 | `AiwAddWeapon` | LOCAL | AI weapon-inventory setup at spawn |
| 14 | `AiwWeaponControlEnabled` | LOCAL | flag |
| 11 | `AiwUseWeaponForRounds` | **REPLICATE** | timed AI action — same |
| 11 | `AinmSetNavMode` | **REPLICATE** | NPC nav mode (e.g. attack vs patrol) — gameplay |
| 11 | `AiBodyControl` | LOCAL | body-control component |
| 17 | `AiVoice` | COSMETIC | NPC voice playback; both sides hear via spatial audio |
| 17 | `SetVoiceSet` | LOCAL | voice-pack selection (per-NPC config) |
| 21 | `AnimFrame` | LOCAL | animation-frame query |
| 18 | `RotateToPoint` | LOCAL | per-actor rotation control |
| 32 | `ActorEnableInteraction` | **REPLICATE** | interaction-flag toggle; gameplay-visible |
| 28 | `propTargetable` | **REPLICATE** | targeting flag (gameplay) |
| 28 | `AddInventory` | **REPLICATE** | inventory mutation; host authoritative |
| 25 | `GetInventory` | LOCAL | inventory query (client gets replicated copy) |
| 13 | `InventoryList` | LOCAL | inventory component |
| 24 | `BaseWeaponInventory` | LOCAL | weapon-inventory component |
| 32 | `ViewComponent` | LOCAL | per-actor render component |
| 20 | `ActorSetAlpha` | COSMETIC | actor transparency (fades, etc.) — visual only |
| 20 | `ActorSetJuggernaut` | **REPLICATE** | invulnerability flag — gameplay |
| 20 | `ActorSetCollisionSurfaceName` | LOCAL | collision-surface tag (rarely changes; usually setup-only) |
| 27 | `ActorGetInstanceName` | LOCAL | name read; safe |
| 27 | `ActorGetFacingVector` | LOCAL | facing read; safe |
| 24 | `GetNamedPoint`, 23 `GetBonePosition`, 15 `DoesNamedPointExistByName` | LOCAL | per-actor spatial queries |
| 27 | `CallNamedFunction` | LOCAL | indirect call within own script context |
| 22 | `Hash*` (`HashAdd`, `HashCreate`, `HashDestroy`, `HashFind`) | LOCAL | hash-table data structure |
| 27 | `State__current/next/time/...` (`State__*`) | LOCAL | per-actor FSM state |
| 15 | `Idle__Enter/Update/Exit` | LOCAL | FSM "Idle" sub-state |
| 14 | `Wait__Update/Enter/Exit/time` | LOCAL | FSM "Wait" sub-state |
| 12 | `ChangeState` | LOCAL | FSM transition; per-actor local |
| 13 | `Players_GetActorHandle` | LOCAL | already covered |
| 13 | `GetHealth` | LOCAL | HP query (replicated read-only on client) |
| 12 | `HealthTracker` | LOCAL | per-actor health component |
| 13 | `ActorGetNetMaster` | **NET-PRIMITIVE** | **THE ENGINE ALREADY HAS A NET-MASTER CONCEPT.** Investigate the C++ side before Phase 3 designs replication from scratch |
| 12 | `replace_model` | **REPLICATE** | model swap; visual+gameplay |
| 12 | `AttachEmitterToBone` | COSMETIC | particle attachment |
| 12 | `DistanceToActor`, 14 `DistanceToPlayer` | LOCAL | pure-math queries |
| 12 | `PrintState` | LOCAL | debug |
| 12 | `Shadow` | COSMETIC | shadow component (visual) |
| 11 | `Die` | **REPLICATE** | death action; host authoritative |
| 11 | `Handle_FinishedRespawn` | **REPLICATE** | respawn event; host authoritative |
| 11 | `DeathFade` | COSMETIC | fade-out FX on death |
| 11 | `ActorSetAnimatedTextureFPS` | COSMETIC | animated-texture speed |
| 11 | `FollowStart`, 11 `IdleAnim` | LOCAL | local follow/anim cue |
| 11 | `IRRApplyRenderState`, `IRRApplyDiffuseColor` | COSMETIC | renderer immediate-mode state |
| 10 | `RegisterIRREffect`, `UnRegisterIRREffect` | COSMETIC | IRR effect registration |
| 11 | `AngleDelta` | PURE | math |
| 11 | `MoveStop` | **REPLICATE** | stops movement; gameplay |
| 11 | `RestoreMissionStates` | **FORBIDDEN** | save-load specific; host-only |
| 10 | `SurfaceType` | LOCAL | surface-type lookup |
| 10 | `SwitchLevel` | **FORBIDDEN** | level transition; host-only (drives mission progression); client follows via replicated level-change event |
| 10 | `LevelId_GetCurrent` | LOCAL | current-level query |
| 10 | `IsCutScenePlaying` | LOCAL | cutscene state query |
| 10 | `Handle_ActorStartCutScene` | **REPLICATE** | cutscene-start event; host authoritative; replicate to lock client camera |
| 10 | `Handle_ActorEndCutScene` | **REPLICATE** | cutscene-end event; same |
| 10 | `AimSetAiSpyText` | COSMETIC | UI text |
| 10 | `BlendLook` | COSMETIC | look-blending (animation-control) |
| 10 | `ActorSetInstanceVisRegionStatic` | LOCAL | vis-region binding (level-setup time) |
| 10 | `IdleAnimCycle`, `encAnim`, `Speak`, `Talk`, `TalkCycle`, `TalkEnd` | COSMETIC | speech / talk animations |
| 10 | `StartReact` | **REPLICATE** | "react to event" action — gameplay |
| 9 | `cbPowerupFreeze`, `cbPowerupKillBricks`, `cbPowerupResetBricks`, `cbPowerup3Shot`, `cbPickupShield`, `cbDefaultWeapons`, `cbPowerupInstances`, `cbag1..4` | — | ChargeBall mini-game internals; mini-games are HOST-ONLY in v1 per plan |
| 9 | `Async__Update`, `Async__player`, `Async__playerPos`, `Async__playerDir`, `Async__playerDist`, `Async__playerVel`, `Async__readyToAttack`, `Async__stopMoving`, `Async__promDown`, `Async__moving`, `Async__thisPos` | LOCAL | async player-tracking state; per-NPC observation of player. Pure consumer; replicates implicitly via player transform snapshot |
| 5 | `GenericNetActor` | **NET-PRIMITIVE** | actor class that has networking built in. Investigate what it provides before Phase 3 |
| 1 | `PathFollowerNetActor` | **NET-PRIMITIVE** | networked variant of PathFollower. Same investigation |
| 5 | `transremote` | **NET-PRIMITIVE** | transform-remote — strongly suggests existing transform-replication primitive |
| 2 | `IsMultiPlayer` | **NET-PRIMITIVE** | **THIS EXISTS.** The script language has an MP-mode check. Investigate C++ implementation; this is the gate the rest of MP code branches on |
| 2 | `Players_GetClosestAvatar` | LOCAL | reads local player list (replicated) |
| 1 | `MiniHamsterCreatePlayer` | LOCAL | mini-game player setup (mini-games host-only in v1) |
| 1 | `GetPlayerIndex`, `SetPlayerLost`, `SetPlayerWon`, `GetPlayerStatus`, `ClearPlayerStatuses`, `SetPlayerLost` | **NET-PRIMITIVE** | competitive-multi-player primitives in mini-games. May not extend to full-game coop but worth investigating |
| 1 | `PlacePlayers`, `SchoolYardPlacePlayers`, `CyrogenicPlacePlayers`, `GardenPlacePlayers`, `Gym_player0/1/2/3`, `Cyro_player0/1/2`, `ShclYrd_player1/2` | **NET-PRIMITIVE** | up to 4-player placement in mini-games. Up-to-4P scaffolding exists |
| 1 | `recv` | **NET-PRIMITIVE** | network receive primitive. Single occurrence — investigate |
| 1 | `MiniHamsterRestorePlayers`, `MiniHamsterHudSetItPlayer`, `MiniHamsterHudStartPlayerTimer`, `MiniHamsterHudSetPlayerStatus` | LOCAL | mini-game per-player UI; host-only in v1 |

---

## Implications for the v2 plan

### Phase 1 (transport)

Unchanged — UDP scaffolding is still needed.

### Phase 2 (second-player spawn + input separation)

**Estimate revision warranted.** v2 plan committed 4 weeks for "trace + patch every input dispatch site to take a player_idx parameter." But:

- The script VM exposes `Players_GetActor(idx)`, `Players_GetAvatar(idx)`, `GetPlayerIndex()` already.
- Mini-games use up-to-4-player placement (`Gym_player0..3`).
- `IsMultiPlayer` is an existing branch primitive.

This means the **C++ side likely has player-index aware paths that the audit didn't surface** (because the audit looked at `g_input_mgr` not at every actor's player-index property). Phase 2 may be smaller than 4 weeks once those paths are mapped — but require their own audit first.

**Action for Phase 2 prep**: hook `Players_GetActor` / `IsMultiPlayer` at runtime, observe call sites, build a map of where player-index parameters flow in the C++ side.

### Phase 3 (state replication)

**Major revision warranted.** v2 plan committed:

- New `sim_decouple::Mode::CLIENT`.
- Shadow-buffer + sim-PRE swap.
- Hand-rolled snapshot delta encoding.

But:

- `ActorGetNetMaster` exists — engine HAS a net-master concept already. May provide host/client routing for free.
- `GenericNetActor`, `PathFollowerNetActor` — networked actor base classes exist. Inheriting from these may be cheaper than wiring per-class replication from scratch.
- `transremote` symbol exists — strongly suggests transform-replication primitive already coded.

**Action for Phase 3 prep**: RE `GenericNetActor` and `ActorGetNetMaster` in IDA before designing Phase 3 from scratch. If the primitives are functional (even partially), Phase 3 may collapse to "wire the existing net-actor pipeline to a UDP socket" rather than "build a replication system from scratch."

### Phase 5 (script VM replication)

**Now scoped.** The catalog table above gives the starting classification. Open items requiring runtime hooks before commit:

1. `SendMessageToAll` / `SendMessageToActor` — does the existing implementation already broadcast across instances when `IsMultiPlayer = true`? Hook + observe.
2. `GetRandomReal` / `GetRandomInt` — do call sites consume the result for gameplay (forbid + host-only) or just cosmetic jitter (allow both)? Hook + observe.
3. `GameTime` — does it advance deterministically against gameplay clock or is it free-running render time? Hook + observe.

---

## Where this leaves Phase 0

| Plan-level | Status |
|---|---|
| **0A** (entity factory RE) | DONE 2026-05-10 — `entity_factory_construct` @ 0x5B96F0. |
| **0C** (.sx command catalog) | DONE 2026-05-11 (this doc). Classification table drafted. **Side finding: engine has more multi-player than the v2 audit caught.** |
| **0D** (test harness skeleton) | DONE 2026-05-11 — `tools/run-coop-test.ps1` + port-suffixed JSON. |
| **0B** (save-system RE) | PENDING — still needs ~1wk uncharted RE work. |

After 0B completes, Phase 0 closes. **Recommend reopening v2 plan estimates after 0B + the Phase 2/3-prep runtime hooks** — the existing scaffolding may shave several weeks off the original 9-10 month estimate.

---

## Reproduction

```
cd d:/Projects/Programming/MeetTheRobinsons
python3 tools/extract_sx_identifiers.py --out tools/sx_identifiers.txt --top 80
```

Output:

- stdout: top 80 by frequency.
- `tools/sx_identifiers.txt`: full ranked list (7,640 rows).

---

## What this does NOT do

- **Does NOT decompile .sx bytecode.** Only string extraction. Actual opcode-level decoding is Phase 5 work; for classification we only needed verb names.
- **Does NOT verify classification via runtime hooks.** The classification reflects reasoning from symbol names. Verification requires hooking and observing each verb's effect — flagged for Phase 5 design as the "open items requiring runtime hooks" list above.
- **Does NOT cover the C++ → .sx command bindings.** [ai-script-vm.md](ai-script-vm.md) covers `script_register_command` and the SecuROM-thunked registration sites; that's the inverse direction. Phase 5 will need both halves.

---

## Anchors created this session

No new VAs — Phase 0C is data extraction, not IDA RE. Useful identifiers as IDA-rename candidates when Phase 2/3 prep starts:

- `IsMultiPlayer` — find the script-side binding via `script_register_command` hook
- `ActorGetNetMaster` — same
- `GenericNetActor` / `PathFollowerNetActor` — class registry
- `Players_GetActor` family — registration site
- `transremote` — single-occurrence symbol; capture site is uniquely identifying
