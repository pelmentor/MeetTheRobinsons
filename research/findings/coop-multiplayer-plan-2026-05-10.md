# Functional coop multiplayer plan (LAN-first, authoritative-host)

**Date:** 2026-05-10
**Revision:** v2 (post-audit; architect `a3a7df5c`, engineer `ade1d0e2`)
**Governing rule:** RULE №1 — proper fix, no shortcuts. Weeks/months are OK; quick wins that paper over real problems are RULE-№1-violating.
**Recon:** [Networking surface map by code-explorer agent `a054c3d6`] (summarized below)

---

## v2 changes from v1 — audit findings incorporated

The audits caught 15+ items spanning correctness bugs, missing scope, and underspecified design. Listed roughly in severity order:

**Critical correctness bugs caught:**

1. **PRE-sim restore fence is HOSTILE to network delivery.** v1 said "M0–M6 infrastructure naturally feeds network snapshots." False. The M2.3 fence in `interp_player.cpp:169-201` and `interp_npc.cpp:135-153` saves pre-interp values BEFORE write and restores them at PRE-sim. On the client this clobbers any host snapshot delivered mid-frame by the network thread. **Fix**: introduce `sim_decouple::Mode::CLIENT` (alongside OFF/THROTTLE) which DISABLES the PRE-sim restore for networked entities. POST-sim capture on client reads from network shadow buffer, not engine state. Phase 3 design item.

2. **Bandwidth estimate was wrong by ~6×.** v1 said "30 KB/s host uplink" but 60 NPCs × 100 bytes × 30 Hz = 180 KB/s for NPCs alone. Realistic worst-case full-state: ~200 KB/s. LAN-fine; **internet-problematic** (above many residential upload caps). **Fix**: commit to per-field skip-if-unchanged delta encoding at Phase 1 DESIGN time (not Phase 4). Phase 1 transport ships with the delta machinery, not hand-waved.

3. **Entity lifecycle (spawn/despawn) MISSING from Phase 4 replication.** RNG-driven NPC spawns desync immediately. **Fix**: add explicit `entity_spawn` and `entity_despawn` events to Phase 4's replication scope (not Phase 5 scripts).

4. **`g_input_mgr` orphan is a TRAP, not a reuse opportunity.** One xref total; every dispatch site hardcodes `player_idx=0`. Phase 2 work is "trace + patch every input dispatch site to take a player_idx parameter," not "populate the orphan slot." This significantly enlarges Phase 2's scope.

5. **`connect` IS NOT IN THE IAT.** v1 implicitly assumed UDP "connect" available. **Fix**: Phase 1 transport uses `sendto`/`recvfrom` (connectionless UDP) only — IAT extension not required.

**Underspecified / blocking design gaps:**

6. **Two-process test harness fails on 3 counts.**
   - `mtr-asi-test-result.json` path is hardcoded in both `test_harness.cpp:764` AND `run-test.ps1:51`; two simultaneous processes race on this file.
   - `run-test.ps1` is single-process structure; orchestrating two requires a complete rewrite.
   - **`Global\Disney_s_Meet_The_Robinsons` mutex prevents same-machine dual-launch** (`run-test.ps1:63-75` already documents this). Two-process test = two machines or two VMs.
   - **Fix**: explicit Phase 1.5 sub-task `coop-test-harness`. Result JSON path includes port number; new `run-coop-test.ps1` orchestrator; test cases require either two physical machines or VM-isolated processes.

7. **Network thread engine-memory writes are data races.** v1 didn't specify the synchronization. **Fix**: explicit double-buffer design — network thread writes to a shadow buffer; sim-PRE hook swaps shadow→engine under existing sim-tick fence (mirrors M4/M5 save/write/restore pattern). No mutex around engine memory.

8. **Phase 0 time estimate of 1 week is unrealistic.** RE of `sub_5B40C0` (11KB factory behind SecuROM thunks) alone is 2-3 weeks. Save system is uncharted. **Fix**: Phase 0 cap raised to 3-4 weeks; downstream phases pushed back accordingly. **Total v2 estimate: ~9–10 months for v1 (was 8 months in v1).**

9. **Snapshot timing must hook `on_sim_tick_post()` directly, not reuse camera_apply.** v1 said "snapshot at sim-tick boundaries"; the existing M2 snapshot fires at `hk_camera_apply_all_active` POST which is several pump steps later. **Fix**: new callback `on_sim_tick_post_for_network()` in `sim_decouple_throttle.cpp:255` (sim_aggregator POST). Phase 3 design item.

10. **AI behavior-tree state has NO RE backing.** v1 listed it as part of Phase 4 NPC snapshot. The AI structure is uncharted. **Fix**: Phase 4 snapshot replicates `transform + anim controller state + HP` only. Behavior-tree state moves to Phase 5 (after AI RE) OR is replaced by host-driven event replication ("NPC X switched to behavior Y") so the client never has to model the tree.

11. **Physics-active prop replication strategy unspecified.** v1 said "props with state changes." Throwables and push-pullables have continuous physics state. Snap-on-receipt = visible rubber-banding. **Fix**: explicit Phase 4 commitment — physics-active props are snap-on-receipt at 30 Hz (rubber-banding accepted as v1 tradeoff); reconsidered if user complains in stress-test.

12. **Audio event boundary undefined.** v1's "small set of replicated audio events" is unbounded. **Fix**: Phase 6 commits to three categories:
    - **Replicate**: world-positioned sounds at a given world coord (host says "play sound X at pos P").
    - **Replicate via entity**: sounds attached to a specific entity (host says "play sound X on entity Y"; client spatializes against entity's current replicated transform).
    - **Local-only**: 2D HUD sounds (UI clicks, menu navigation).
    - Footstep / NPC voice / mission cues land in categories 1 or 2 explicitly.

**Smaller corrections:**

13. **Client-side own-player prediction is dead-reckoning, NOT re-running physics.** v1 said "client predicts own input" without specifying. **Fix**: pos+velocity dead-reckoning + snap-correct on each host snapshot. Re-running the physics integrator is rejected (client doesn't run sim).

14. **Cutscene-end sync event explicit.** v1 said "client sees overlay during cutscenes." **Fix**: Phase 5 ships a `cutscene_end` network event so the client knows when control returns. Without this, missions that flow cutscene→gameplay will hang on the client.

15. **Crash-handling client-procedure on host crash specified.** v1 said "disconnect on divergence" without specifying client UX. **Fix**: heartbeat timeout (5 s) → `screen_push` to main menu via `level_select.cpp` infrastructure → "Connection lost" error screen → offer reconnect (Phase 7 ships full-state resync).

16. **`log.cpp` is already thread-safe** — confirmed by both audits reading the file. v1's worry about logger thread-safety was a non-issue. (`std::mutex s_mu` + `std::scoped_lock` in every public function.) No change needed; mention preserved here for the record.

**Confirmed correct as stated** (don't change in v2): authoritative-host over lockstep; rejection of `connect`-based protocol; rejection of rollback netcode; Phase 5 ordering (scripts AFTER players + NPCs + props because scripts react to those subsystems); host-save-only; mini-games host-only; rejection of split-screen for v1; per-screen camera networked.

---

## TL;DR

The engine has zero functional multiplayer. ws2_32 sockets are linked but only for the (dead) online-DRM module; the multiplayer main menu in `mainmenu.sc` is dead data with no compiled screen ctors; the player is name-keyed (`"player"`) with no second-instance precedent. Building coop requires building everything: transport, protocol, second-player spawn, state replication, script-VM determinism strategy, camera/UI for two players, and save-state handling.

This plan proposes **LAN-first, 2-player authoritative-host coop** as the realistic minimum. Scope: ~6–12 months for a focused single-developer effort, 3–6 for a small team. Phased so a "joinable but mostly-broken" demo lands at the end of Phase 4 (~3 months in), and "stable through one full level" lands at the end of Phase 7.

**Out of scope for v1:** internet matchmaking, NAT punch-through, anti-cheat, dedicated servers, more than 2 players, mid-mission rejoin, mid-mission save in coop, the `mainmenu.sc` Xbox-Live UI revival.

---

## Current state of multiplayer infrastructure

### What is present

- **14 ws2_32 IAT entries** at `0x6A62C4..0x6A62F8`: `WSAStartup, WSACleanup, socket, bind, listen, accept, recv, send, shutdown, closesocket, ioctlsocket, htonl, htons, WSAGetLastError`. Notable absences: no `connect`, no `getaddrinfo`/`gethostbyname`, no `select`/`WSAEventSelect`. The set looks like a server-mode listener (likely the activation server in `encrypted_drm_unlock_online.cpp`).
- **Per-player input dispatch architecture** at `g_input_mgr` (`0x745B70`) with per-player state array. `input_get_active_source_for_player(player_idx)` exists at `0x56F290` but has one orphan xref. Hard-coded `player_idx=0` everywhere it's actually called.
- **Multiplayer screen DEFINITIONS** in `Game/data/screens/mainmenu.sc`: `ID_MAINMENU_ONLINE`, `ID_MAINMENU_SYSTEMLINK`, `ScreenXboxLiveLogin`, `ScreenOnlineProfile`. **Zero compiled screen ctors** for these classes — confirmed via screen-registry log (56 ctors registered at boot; none is online/profile).
- **Family-album mini-game characters** (Franny, Tallulah, GrandpaBud, etc.) — these are single-player swap-skin unlocks for the hamster-ball mini-game. NOT multiplayer slots.

### What is absent

- No DirectPlay (`dpnet.dll`), no winhttp, no wininet, no wsock32.
- No replication / RPC / netvar systems (zero matches on those terms).
- No lockstep / determinism / rollback strings.
- No `mp_` / `net_` / `cl_` / `sv_` / `server` / `client` cvar groups.
- No `player2` / `player_2` entity name. The entity registry resolves the player by literal `"player"` (single instance per scene, name-keyed via `entity_lookup_by_name_retry` at `0x5AC8F0`).
- No "second character is controllable" infrastructure. The companion strings (`helper`, `partner`, `sidekick`) don't appear in any code path that grants a separate entity controllable via second input device.
- No save system per-player branching (saves are linear single-player progression).

### What this means

Coop has to be **bolted on top of** the engine, not built from existing scaffolding. Every system below is greenfield with respect to multiplayer.

---

## Architecture decision: authoritative host + state replication (NOT lockstep)

Three viable models for coop in a single-player engine:

| Model | What it does | Why for / against this engine |
|---|---|---|
| **Authoritative host** | Host (P1) runs full sim. Client (P2) receives state snapshots, runs prediction for own input only. Mismatches are corrected by host snapshots. | **Recommended.** No determinism requirement on float / RNG / animation timing. Works with engine-as-shipped. Bandwidth proportional to active-entity count × snapshot rate. |
| **Lockstep** | Both sides run identical sim from same seed; only inputs are exchanged. Bandwidth tiny. | Requires bit-exact float determinism across machines, fixed RNG seed propagation, deterministic anim+physics+script-VM execution. The engine is NOT deterministic across runs (FPU rounding state, alt-pump timer drift, screen-push event order). Making it deterministic is a project bigger than the rest of coop combined. **Reject.** |
| **Rollback netcode** | Lockstep + snapshot every input frame + re-simulate forward when remote input arrives late. The model used for fighting games (GGPO, RollbackCaster). | Same determinism requirement as lockstep, plus complete game-state serialize-and-restore at each frame. Fundamentally incompatible with this engine's decoupled sim/render architecture (M0–M6). **Reject.** |

The plan commits to **authoritative host**. P1 is host, P2 is client. Host is the source of truth for every entity, every script-VM state, every mission flag. Client extrapolates own-player input for instant feedback, then reconciles against host snapshots when they arrive.

---

## Topology: 2-player LAN, peer-to-peer over UDP

- **2 players** for v1 (single host + single client). Engine has no per-player-array machinery beyond the orphan `g_input_mgr`; expanding to 4 players later is a separate phase that needs to also decide split-screen vs networked-only.
- **LAN only** for v1. No NAT punch-through. The host listens on a UDP port; the client is given the host's IP and port directly (in-game text input or `mtrasi-coop-host=<ip>:<port>` cmdline flag). Internet play is Phase 8+.
- **Transport: UDP** (the engine has the syscalls already). Custom protocol on top — small fixed-size header, sequence numbers, ack-based reliability for important messages, fire-and-forget for tick-snapshot deltas. No SCTP, no QUIC, no RakNet, no GameNetworkingSockets at first — minimize external deps; revisit in Phase 8 if internet play is added.
- **Tick rate: 30 Hz network sim**, 60 Hz game sim. Network ticks are every 2nd sim tick. Snapshot delta-compressed against last-acked snapshot per client.
- **Encryption: none for LAN**. AES if internet play happens.

---

## What needs to be built — the 7 hard problems

These are the systems that don't exist and must be built. Each is its own phase.

### Hard problem 1 — Network transport + protocol scaffolding

A DLL-side networking module: hooks ws2_32 (or just calls it directly — we already have an in-mod thread architecture from `dllmain.cpp`'s init thread pattern). UDP socket on a configurable port. Frame-size limit, sequence numbers, simple acks for reliable channel, unreliable channel for snapshots. Connection state machine: NOT_CONNECTED → CONNECTING → CONNECTED → DISCONNECTING. Heartbeat + timeout (5 seconds default).

### Hard problem 2 — Second-player entity spawn

The engine spawns the player by name from `.sc` script during level load. There is exactly one "player" instance per scene. To get a second controllable avatar, we need to:

- Either: spawn a second entity via the `.sc` factory dispatcher at `sub_5B40C0` with a different name (`"player2"`), then bind input source 1 to it.
- Or: use the family-album character class (which already exists for the hamster-ball mini-game — Franny / Tallulah / etc.) as the second-player avatar.

Both paths require entity-factory RE that hasn't been done yet. The factory is an 11KB function with class-registry lookups behind SecuROM thunks ([entity-system.md](entity-system.md) §"What's blocked by SecuROM"). Phase 2 starts with the offline RE work.

### Hard problem 3 — Input separation

`g_input_mgr` (`0x745B70`) has the per-player state array architecturally but only player 0 is wired. We need:

- Player 0: keyboard + mouse + (gamepad 1 if present), routed to `player` entity.
- Player 1: gamepad 2 (or some other device), routed to `player2` entity.

This means hooking the input pump (DirectInput is already proxied by the mod's `dinput_hook.cpp`) and routing per-device events to per-player slots in `g_input_mgr`. Separately, the in-game UI (HUD, dialogues, menus) needs to know which player a given action came from — a per-player active-cursor / active-button concept the engine doesn't have today.

### Hard problem 4 — State replication

For each tick, the host needs to send the client a delta snapshot of:

- **Both players**: pos `+0x58`, rotation `+0x70`, anim controller state (current track, time, rate), mode flags, weapon state, HP. Per [player-entity-layout-2026-05-09.md](player-entity-layout-2026-05-09.md), the player layout is mostly known. ~200 bytes per player per snapshot, delta-compressed.
- **NPCs** within a configurable proximity radius around either player (~30 units). Pos, rotation, anim state, hp, AI behavior-tree state. ~100 bytes per NPC; expect 20–60 NPCs in scope per scene.
- **Props** with state changes (disassembled, picked-up, broken, opened). Per-frame-changed props only — most props are static.
- **Trigger volumes**: state (active/disarmed/fired). Smaller than 100 bytes.
- **Scene flags**: mission-state booleans, scripted-event flags. The engine has these in `(scene+104) bit 0` and similar (per memory). Need full RE of which scene-flag set is "mission state" vs "FX state".

Authoritative snapshot rate 30Hz; delta-compressed against last-acked. Estimated bandwidth: ~30KB/sec uplink from host, ~5KB/sec uplink from client (just input). Within LAN budget; tight for low-end internet.

### Hard problem 5 — Script VM determinism (the big one)

The mission system is script-driven (.sx). When a mission starts, the script VM dispatches `mission_X_start` callback. Mission progression is gated on script-evaluated conditions (player crossed trigger, NPC died, dialog finished). For coop:

- **Host runs the script VM authoritatively.** Every script command that modifies world state (`set_visibility`, `spawn_entity`, `play_anim`, `play_sound`, `change_scene_flag`) generates a network event that's broadcast to the client.
- **Client does NOT run gameplay scripts.** Client receives "scene flag X set to Y" and applies it. Local cosmetic scripts (HUD anims, sound localization) can run on both, but the source of truth is the host.

The hard part: identifying the EXACT set of script commands that modify replicable state vs cosmetic state. The .sx script command catalog is partially in [ai-script-vm.md](ai-script-vm.md). Phase 5 includes a full pass across all .sx files in `Game/data/scripts/` to enumerate every command verb and classify each as Replicate / Cosmetic / Forbidden (don't run on client at all).

Risk: if even one script command is misclassified as cosmetic when it's actually mission-state, the two clients desync and the mission becomes uncompletable on the client side.

### Hard problem 6 — Camera and UI

- **Camera options:**
  - **Split-screen** (each player gets half the viewport): cleanest for couch coop, but requires hooking the engine's camera_apply pipeline to render twice with different view matrices, double the GPU cost, plus split-screen UI rebuild.
  - **Shared camera** (camera frames both players, zoom out when far apart): simpler graphically, but the engine's camera is single-target (PathCam follows `"player"` entity). Needs a virtual midpoint target.
  - **Per-screen camera, networked** (each player's PC shows their own camera): no camera pipeline changes, but each side needs to render-cull its own POV — engine's static camera-cache strategies (memory: per-camera projection cache at `outer+0xD4`) won't cope cleanly with two simultaneous active cameras even on the same machine.
  
  v1 commits to **per-screen camera, networked** (each PC = one player's POV). Split-screen is a separate Phase 9 if anyone asks for it.

- **HUD:** the in-game HUD is single-player. Coop needs per-player HUD overlay (own HP, own weapon, partner indicator showing "P2 is at distance D, direction →"). The HUD is a sprite-batcher pipeline ([sprite-per-element-architecture.md](sprite-per-element-architecture.md)); per-player HUD requires per-player sprite-state separation, which the engine doesn't have. Use mtr-asi's existing per-element-control + ImGui overlay layer to draw partner indicators OVER the engine's HUD.

- **Pause:** in single-player, pause halts everything. In coop, "pause" stops only one player's UI and freezes the host's sim. Or: shared pause (either player can pause; both freeze). v1: shared pause via a network-replicated `paused` flag.

### Hard problem 7 — Save state in coop

Each player has a save game in single-player. In coop:

- **Linear progression:** both players progress through the same level set. Whose save advances? If P1's save is the source of truth and P2 joins, P2's solo progress is irrelevant in this session.
- **Mid-coop save:** if P1 saves mid-mission while P2 is connected, what does P2 get? Answer: P1's save now reflects "this level reached"; P2's save is unchanged.
- **Resuming a coop save:** P1 loads, opens lobby, P2 joins → continue from where P1 was. Standard model in coop campaigns.

v1 commits to **host-save-only** (only the host's save advances). The client is "joining for the duration"; their own save is untouched. Phase 7 (stability) covers edge cases (host quits mid-mission with client connected; client connects mid-mission and resyncs).

---

## Phase plan

Each phase ships an independently runnable build. Capping phases is "direction-eval prompt" not "stop-and-revert" per RULE №1.

| Phase | Cap | Cumulative | Goal |
|---|---|---|---|
| **0** | 1 wk | 1 wk | RE prerequisites (entity factory `sub_5B40C0`, save-system internals, the .sx script command catalog) |
| **1** | 3 wk | 4 wk | UDP transport scaffolding: socket, send/recv, ping/pong, connection state machine. No game state yet. `pwsh tools/run-test.ps1 -Scenario coop-ping` exits 0 across two processes. |
| **2** | 4 wk | 8 wk | Second player entity spawned + bound to gamepad 2 input. Both players can move independently. No replication yet — both clients see only their own player. |
| **3** | 4 wk | 12 wk | Player-state replication. Host broadcasts P1+P2 transforms; client renders both. Movement is host-authoritative; client predicts own input. **First "playable" demo here** — can walk around with a friend, no other game state shared. |
| **4** | 6 wk | 18 wk | NPC + prop state replication. NPCs visible to both players; they react to whoever's nearest. **First "joinable, mostly-broken" demo** — combat works for both, but missions desync because scripts haven't been replicated. |
| **5** | 8 wk | 26 wk | Script VM replication. Host authoritative; full .sx command catalog audited; replicate vs cosmetic vs forbidden classification per command. Missions complete cleanly for both players. |
| **6** | 4 wk | 30 wk | Save state, pause, UI: host-save model, networked pause flag, partner indicator overlay. **End of v1 functional coop.** |
| **7** | 4 wk | 34 wk | Stability: desync detection (CRC32 of replicated state per N ticks), automatic disconnect-on-divergence, re-sync on reconnect. Edge cases: host quits, client disconnects mid-mission, network spike. |

**v1 ships at end of Phase 7 (~8 months)** as: 2-player LAN coop through one full mission, with stability sufficient to demo to non-developers. Internet play, dedicated servers, 4-player, split-screen, anti-cheat, NAT punch-through are post-v1.

---

## Risks

- **R1: Lockstep/determinism creep.** When state replication is too slow, the temptation is "just run sim deterministically on both sides." Reject. The engine is not deterministic; making it so is bigger than the rest of the project. Authoritative host with bandwidth optimization is the answer.
- **R2: Script VM has hidden state we don't know about.** Mid-Phase 5 we discover that some script command class controls a system we hadn't audited. Worst-case: scrap that command's replication and gate the relevant gameplay (e.g. specific puzzles) behind "host-only" content for v1.
- **R3: Input-routing breaks single-player.** Adding per-player input routing on top of the existing single-player path risks regressions. Phase 2 ships the routing behind a `mtrasi.coop_enabled` flag default-off; single-player path is unchanged.
- **R4: NPCs dual-target.** AI is built around "the player" (singular). When there are two, AI needs to pick a target. Easiest: AI targets the closest. Has consequences (gameplay balance becomes 2v1 instead of 1v1). v1 accepts this tradeoff; difficulty rebalancing is post-v1.
- **R5: Camera pipeline modifications break freecam.** mtr-asi's freecam ([freecam.cpp](../../src/mtr-asi/src/freecam.cpp)) operates on a single global view matrix. Per-screen-camera-networked model is fine (each PC has one camera); but if Phase 9 adds split-screen, freecam needs major rework.
- **R6: Save format incompat.** The single-player save format includes mission flags, level state, etc. ([decouple-saves.md](../../docs/decouple-saves.md) covers some). Adding a "this save was made in coop" flag is a save-format break. v1 commits to no save-format change; coop uses a separate file (e.g. `coop_save.dat`) until the format is forward-compatibly extended.
- **R7: Two players in one cutscene.** Cutscenes lock player input and run scripted-camera. In coop: both players' cameras get locked to the cutscene? Or only the host? v1: cutscenes are host-driven; client sees "cutscene playing — your input is suspended" overlay. Bad UX for the client; accept for v1.
- **R8: Mini-games are designed for single-player.** DigDug, ChargeBall, hamster-ball — none was designed for two players. v1: mini-games are host-only; client sees "P1 is in a mini-game" overlay and waits.
- **R9: Combat balancing.** With two players the game is too easy. Out of scope for the technical plan; flag as "v1 is mechanically broken in coop without rebalance."
- **R10: Audio.** Each player needs to hear sounds positionally relative to their own camera. Engine audio is host-side. v1: client receives a small set of replicated audio events ("play sound X at world pos P") and locally spatializes against own camera. Simpler than audio routing across the network.

---

## What we get for free from existing infra

- **Crash handler** ([crash_handler.cpp](../../src/mtr-asi/src/crash_handler.cpp)): same minidumps + `[CRASH]` log lines + result-JSON sentinel for any coop-related faults.
- **Autonomous test pipeline** ([AUTONOMOUS_TESTING.md](../../docs/AUTONOMOUS_TESTING.md)): scenarios for `coop-ping`, `coop-second-player-spawn`, `coop-replication-roundtrip`, etc. The harness can run two game processes simultaneously (different `-mtrasi-test=` names + different ports).
- **Sim/render decouple** (M0–M6 shipped): the per-tick discipline already in place is exactly what an authoritative host needs for snapshot generation. Snapshots can be emitted at sim-tick boundaries.
- **Per-element UI control + sprite_xform**: per-player HUD overlays via the existing per-element matcher. Just bind some sprite-state to player-1 vs player-2 categories.
- **dinput_hook**: per-device routing already partly exists (we inject keypresses; can extend to "tag each event with source device").

---

## Alternatives considered

- **A1: Two instances on one machine, split-screen.** Doable (nothing requires network), but the engine's camera pipeline only has one active camera per scene. Major work; punted to Phase 9. Same complexity for one-machine vs two-machine because the engine doesn't separate "rendering" from "client identity" — both sides need a full game state.
- **A2: GeForce Experience / Steam Remote Play Together.** External tool that streams the host's screen to a client; client's input is fed back. Zero engine work. Limitation: client has full input lag, no per-player camera, and the experience is "watching P1 play with you holding a controller." Discounted as "not real coop."
- **A3: Lockstep with float-deterministic patches.** Compile a determinism-mode that pins FPU control word, replaces engine RNG with seeded version, etc. Roughly half a year of pure determinism work before the first multiplayer feature. Higher risk, lower bandwidth ceiling. Reject.
- **A4: Use a game-networking middleware** (RakNet, GameNetworkingSockets, ENet). External dep but solves transport + reliability + connection management for free. Reduces Phase 1 from 3 weeks to 1. Worth re-considering at Phase 1 kickoff; for now plan assumes raw UDP for portability + zero-dep + small mod size.

---

## What the audits need to evaluate

1. **Architecture audit** — Is authoritative-host the right call, or is there a hybrid model I'm missing? Phase ordering — should Phase 5 (scripts) be split out (do players + props + NPCs first, scripts last)? Cross-system: does the existing sim/render decouple's snapshot infrastructure naturally feed network snapshots, or is it the wrong shape? `g_input_mgr` orphan code reuse — is it really there or is the recon overestimating? Internet play later — does this plan paint us into a corner LAN-wise, requiring rework for internet?

2. **Engineering audit** — Is the bandwidth estimate realistic (30KB/sec uplink for state)? Is the desync-detection model (CRC32 of replicated state) sufficient? Are the SEH-guard and crash-handler patterns we use today extensible to the network thread (which is a NEW thread we'd add)? RULE №1 compliance — is anything in this plan a quick-win that should be done properly (e.g. is "host-save-only" a crutch that should be replaced with proper merged-save semantics)? Is "client doesn't run gameplay scripts" actually correct, or is it papering over the determinism problem in a way that creates worse desync risk?

---

## Phase 0 outcome (empty until executed)

(Populated as the prerequisite RE work proceeds.)
