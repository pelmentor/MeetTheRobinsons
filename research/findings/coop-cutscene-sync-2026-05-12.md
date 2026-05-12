# Coop scope: Cutscene sync + skip semantics

**Date**: 2026-05-12
**Phase**: 3.x (alongside unified entity replication)
**Status**: Scope decision documented. RE work pending.

## Context

MTR cutscenes are scripted camera + character animation sequences
triggered by `.sx` script events: entering a trigger zone, finishing a
combat, talking to an NPC, completing a quest objective. They typically
run 30–60 s and play a fixed canned sequence (no per-player branching).

The scripted content section of `COOP_SCOPE.md` already says BOTH
players see the cutscene when the engine triggers one. This finding
fills in the missing details: who triggers, how it syncs, and how skip
works.

## Decision

### Trigger ownership

- **The host's `.sx` VM is authoritative for cutscene triggers.** Per
  Phase 3.x design (host-authoritative AI sim), only the host runs the
  scripts that contain `cutscene_play(id)` commands.
- **Triggers fire when EITHER player crosses the trigger zone.** The
  zone-check runs on the host against the host's view of BOTH wilburs'
  positions (P1 = local, P2 = replicated). Whichever wilbur enters
  first → trigger fires once on the host → cutscene start command
  emitted.

### Sync

When the host's script calls `cutscene_play(id)`:

1. Host calls the engine's local `cutscene_play(id)` immediately —
   cutscene starts on host's screen at host-local time T0.
2. Host emits RPC `coop_cutscene_start { id, host_time_T0,
   server_seq }` to the client.
3. Client receives RPC. Computes its own local start time accounting
   for network latency: `client_T0 = recv_time - half_RTT_estimate`.
4. Client calls its engine's `cutscene_play(id)` locally at
   `client_T0` (or immediately + seek-into if already past T0).
5. Both engines play the same canned sequence locally. No per-frame
   streaming needed — the cutscene is fully scripted assets, just
   driven by a local clock.

If client doesn't have the cutscene cached (level not yet streamed
or cutscene-id unrecognized): client sends `coop_cutscene_ack { id,
status: missing }` and host's RPC retries with prefetch. Defensive
fallback only — in normal play both clients have the same level
loaded so this should never fire.

### Skip

**Either player presses skip → cutscene ends for BOTH.**

Flow:
1. Player presses skip (engine's cutscene-skip input, currently
   START/B/Esc).
2. The skip input is intercepted by a hook on the engine's cutscene
   input-poll path.
3. If the local side is the host: host immediately calls the
   engine's `cutscene_end(id)` locally AND broadcasts
   `coop_cutscene_end { id }` to client.
4. If the local side is the client: client sends RPC
   `coop_cutscene_skip { id }` to host. Host processes as if it had
   pressed skip itself → calls `cutscene_end(id)` locally and
   broadcasts `coop_cutscene_end { id }` back.
5. Client receives `coop_cutscene_end` → calls its engine's
   `cutscene_end(id)` locally → both screens snap to post-cutscene
   gameplay simultaneously (modulo half-RTT lag for the client-side
   trigger).

### Why "any player skips, both skip"

- Simplest UX: one RPC, no state ambiguity, no waiting indicator.
- Cutscenes are short (30–60 s). If one player has seen them and
  the other hasn't, the worst case is the rookie misses a cutscene
  they could have rewatched anyway (if MTR has a cutscene gallery —
  RE TODO to confirm).
- The alternative ("both must press skip") requires a synchronization
  UI ("waiting for the other player...") that is annoying for fast
  sessions.
- The alternative ("only host can skip") creates the exact UX bug
  the user reported in a different context — clicking and nothing
  happens.

This matches MTA/Borderlands/CoD-coop precedent (any player can skip
shared cutscenes).

## Edge cases

### Cutscene starts but client never gets the RPC

Network drop during cutscene start. Client keeps playing gameplay
while host plays cutscene. Recovery options:
- Host periodically rebroadcasts `coop_cutscene_state` (current id +
  elapsed time) while a cutscene is playing. Client eventually
  catches up by seeking-into-cutscene.
- After timeout, client gives up and stays in gameplay until host's
  cutscene ends naturally + the next `coop_state_sync` resyncs game
  state.

### Client disconnects mid-cutscene

Host doesn't stop the cutscene — it's playing the host's script and
the script must run to completion to advance the game state. Host
finishes cutscene as if SP. When client reconnects, the post-cutscene
state is synced via normal entity-replication.

### Client crashes during cutscene

Same as disconnect — host plays through, client reconnects to
post-state.

### Both players skip simultaneously

Both RPCs arrive at host within a few frames. Host has already
called `cutscene_end` after the first skip. Second skip is a no-op
(check `is_cutscene_playing(id)` before processing). Broadcast
already sent once.

### Cutscene pauses the gameplay sim

If MTR's cutscene path pauses the world simulation (TBD via RE), the
pause is automatically synchronized because host is authoritative.
Client's engine pause/unpause follows the cutscene start/end
naturally.

If MTR's cutscene path runs sim in parallel (less likely for canned
sequences), then sim entities keep replicating per Phase 3.x.

### P2 visible in the cutscene framing

Cutscenes are staged for ONE Wilbur. P2's avatar may be in frame at
an unexpected position relative to the camera. Initial scope: live
with it — don't mess with cutscene staging. If a specific cutscene
breaks badly visually (e.g. P2 in the middle of a hugging scene
between NPCs), there's a per-script option to teleport P2 off-camera
just before the cutscene starts. Per-script decision in Phase 3.x.

## RE TODOs (Phase 3.x)

### TODO 1 — locate `cutscene_play` entry point

The engine function called by `.sx` scripts to start a cutscene.
Search:
- Disassemble around the .sx command parser (`sx_vm` from earlier
  RE) for the command name "cutscene" or "playcutscene".
- Search bytes for the cutscene asset file extension (probably
  `.cs` or `.cut` or `.cutscene` — TBD).
- Set a breakpoint at level start; the level intro cutscene runs
  early.

### TODO 2 — locate cutscene-skip input handler

The function that polls START/B/Esc during a playing cutscene and
calls `cutscene_end`. Likely:
- A poll in the cutscene-tick function (probably called from the
  main loop while cutscene is active).
- Or a callback hung off the cutscene screen (cutscenes likely
  push a screen via `screen_push`).

### TODO 3 — confirm sim-pause behavior

Set a breakpoint on the gameplay sim tick during a cutscene. If it
doesn't fire, cutscenes pause sim. If it does, sim runs in parallel.
This decides whether we need per-entity state replication during
cutscenes or not.

### TODO 4 — locate `cutscene_end` / abort entry

For both the natural-end case (cutscene reaches its scripted last
frame) and the skip case. Probably a single function with a flag.

### TODO 5 — enumerate trigger-zone → cutscene script mappings

Scan `.sx` scripts in the asset bundle for `cutscene_play` calls and
their surrounding trigger conditions. This lets us know which
triggers are cutscene starters vs. other trigger types (combat
spawns, ambient script, etc.).

## Phase 3.x implementation sketch

```cpp
// Host side
void on_cutscene_play_hook(uint32_t cutscene_id) {
    // Local engine call already happens (this is a POST hook on
    // engine's cutscene_play).
    if (is_coop_host_active()) {
        send_rpc_to_client({
            op: COOP_CUTSCENE_START,
            cutscene_id,
            host_time: now_ms(),
            server_seq: g_server_seq++,
        });
        g_cutscene_state = { id: cutscene_id, start_ms: now_ms() };
    }
}

// Host RPC handler
void on_coop_cutscene_skip(uint32_t cutscene_id) {
    if (g_cutscene_state.id != cutscene_id) return;  // stale
    engine_cutscene_end(cutscene_id);  // ends locally
    // The POST hook on engine_cutscene_end will broadcast cutscene_end
    // to client.
}

void on_cutscene_end_hook(uint32_t cutscene_id) {
    if (is_coop_host_active()) {
        send_rpc_to_client({
            op: COOP_CUTSCENE_END,
            cutscene_id,
        });
        g_cutscene_state = {};
    }
}

// Client side
void on_coop_cutscene_start(uint32_t cutscene_id, uint64_t host_time_ms) {
    uint64_t client_now = now_ms();
    int64_t lag = client_now - host_time_ms;  // includes half-RTT
    if (lag < kCutsceneSeekThreshold) {
        engine_cutscene_play(cutscene_id);  // start fresh
    } else {
        engine_cutscene_play_at(cutscene_id, lag);  // seek-into
    }
}

void on_local_skip_input() {
    if (is_coop_client_active() && cutscene_playing()) {
        send_rpc_to_host({
            op: COOP_CUTSCENE_SKIP,
            cutscene_id: g_local_cutscene_id,
        });
        // Don't end locally yet — wait for host's COOP_CUTSCENE_END
        // to ensure both ends synchronously.
    } else {
        // SP or host: end locally; host-side broadcast happens in
        // the POST hook on engine_cutscene_end.
        engine_cutscene_end(g_local_cutscene_id);
    }
}

void on_coop_cutscene_end(uint32_t cutscene_id) {
    engine_cutscene_end(cutscene_id);  // ends locally
}
```

## Relation to other scope

- **Phase 3.x unified entity replication**: cutscenes consume the same
  scripted-trigger fire model. The trigger-zone check is the same
  code path that fires combat spawns, dialogue NPCs, etc.
- **Phase 2.0 input replication**: client's skip input is captured by
  the same input-poll hook that captures all other input. Sending it
  is one RPC; receiving it is one handler.
- **Phase 1.7 level transition**: if a cutscene's end transitions
  levels, the level-load-completion hook (also Phase 1.7 work) resets
  the orphan-spawn latch so the orphan respawns in the new level.

## What is NOT in scope

- Per-player branching cutscenes (already stated in COOP_SCOPE.md "out
  of scope").
- Cutscene editing for P2 visibility (P2 may show up in cutscene
  framing; this is accepted).
- Vote-to-skip / require-both-players-to-skip UI.
- Server-side cutscene rewind / replay.
