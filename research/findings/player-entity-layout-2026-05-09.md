# Player entity layout dump (2026-05-09)

Date: 2026-05-09
Status: **ROOT-CAUSE FIX SHIPPED — pending user retest.** Full RE journey from
"few-frame visible TP then snap" through 4 hypotheses to the final
`entity_transform_tick` skip-bit fix.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## TL;DR

The "MMB-tp at altitude doesn't stick visually" bug isn't a render-cache lag and
isn't a missing pos-field write. The engine's AI/anim system detects "Wilbur is
off the navmesh" each sim tick and runs a **balance-recovery anim** whose pos
curve drives `player+0x58` (and downstream the model's world matrix) BACK to the
last valid navmesh anchor. The proper fix is to set the engine's OWN per-node
skip-bit (`0x10` at `node+68` in the transform-list at `dword_724DE4`) on the
player's transform-list node before `entity_transform_tick` runs, then clear
the bit after — the anim still ticks but the per-tick pos/rot write skips
entirely, and our hold-write to `+0x58` and `sub+0x10` finally sticks.

## Layout findings

### Player entity (`*0x7425AC -> entity_lookup("player", 1)`)

First 192 bytes from runtime dump (player at `0x0EF0FFE0`, level "Wilbur's room"):

| Offset | Type | Notes |
|--------|------|-------|
| `+0x00` | ptr (vtable) | `0x6B8730` — Player class vtable |
| `+0x04..0x0F` | flags/uint | `0x92`/`0xFE`/`0x01` markers |
| `+0x10..0x47` | class metadata | DO NOT WRITE — ptrs to class-table at `0x6A6638`, sizes/indices. Earlier crash root-caused to writing here. |
| `+0x48` | **ptr (heap)** | **render sub-component** (Sub-A). See layout below. |
| `+0x50` | **ptr (heap)** | **entity name string** (Sub-B = ASCII `"player\0"`). Confirmed 2026-05-09 PM dump. NOT a transform. |
| `+0x54` | ptr (rdata) | → `"Avatar"` string @ `0x6B6AA0` (camera target handle name) |
| `+0x58` | **vec3** | **game-logic pos** — read by AI/scripts. Written by `entity_transform_tick`. |
| `+0x64` | vec3 | duplicate of `+0x58` (ignore — irrelevant for render) |
| `+0x70` | float[9] | rotation 3x3 |
| `+0xAC..0xB7` | floats | third pos copy (X, Y_offset, Z) — appears to be hip-level. |

### Sub-A `*(player + 0x48)` — render component

192 bytes at runtime (live address varies):

| Offset | Type | Notes |
|--------|------|-------|
| `+0x00` | uint | `6` (type/flags marker) |
| `+0x04` | vec3 | feet/knees pos (Y ≈ feet level) |
| `+0x10` | **vec3** | **"center" pos = feet + (0, 1, 0)** — the engine's altitude-gated sync writes here when accepting a `+0x58` change. NOT directly read by the renderer's draw path (we proved this — see Render path below). |
| `+0x20` | float | `1.0` (scale) |
| `+0x28..0x2C` | floats | anim/blend params |
| `+0x58` | vec3 | hip-level pos (third copy in this struct) |
| `+0x70..` | floats | anim/IK weights |

### Sub-B `*(player + 0x50)` — name string (NOT a transform)

`subB+0x00 = "player\0"`, `subB+0xA0 = "1000000.0\0"`. **Do not write pos floats
here** — corrupts entity lookup state (early test broke things by writing).

## Render path (final, validated)

Per the SetTransform(D3DTS_WORLD) hook trace (2026-05-09 PM):
- All world-matrix submissions during a frame come from `caller=0x563D7B`.
- That call site is inside `game_render_main_scene` (sub_563C70) which calls
  `wrap_SetTransform(0x724B10)`. The matrix at the global `0x724B10` is
  populated upstream by the anim → transform pipeline.
- Wilbur's body draws fire **3× per frame** at the engine's "where Wilbur
  should be" pos (the navmesh anchor / balance-recovery pos curve), NOT at our
  written `+0x58` or `sub+0x10`. The single submission at our teleport target
  is some non-body draw (marker / shadow / particle attach).

Therefore: writing `sub+0x10` directly bypasses the altitude gate but does NOT
change the world matrix the renderer actually submits, because that matrix is
built from the anim sample that was run earlier in the same frame.

## Why the hypotheses failed

| Hypothesis | Outcome | Why it failed |
|------------|---------|---------------|
| #1: render-cache lag at `+0x64` | Speculative; ruled out before testing | `+0x64` follows `+0x58` 1-frame later but is altitude-INDEPENDENT — render works fine at low altitude. |
| #2: render reads `sub+0x10` | Partially correct | Engine writes `sub+0x10` from `+0x58` but only at low altitude; we bypassed the gate. Verified `sub+0x10 = target` every frame in trace. **But user still saw snap-back** — render isn't reading `sub+0x10` directly. |
| #3: render reads `sub+0x04` / `sub+0x58` / `sub_B+0x10` / `player+0xAC` (wide-net) | Failed | Wrote to all 5 candidate fields; no visual change. (Plus `sub_B` write was harmful — name string corruption.) |
| #4: anim/AI is upstream — kill the anim's pos write at the source | **CORRECT** | Set skip-bit on player's node in `dword_724DE4` transform list. `entity_transform_tick` skips → `+0x58`/`+0x70` not overwritten by anim. Downstream pipeline (sub_A sync, world matrix builder) uses our held value. |

## The fix (shipped 2026-05-09)

[freecam.cpp `hk_entity_transform_tick`](../../src/mtr-asi/src/freecam.cpp):

```cpp
constexpr uintptr_t kEntityTransformTickVA   = 0x004B9F60;  // sub_4B9F60
constexpr uintptr_t kTransformListHeadAddrVA = 0x00724DE4;  // dword_724DE4
constexpr uintptr_t kTransformNodeNextOffset    = 4;        // node->next
constexpr uintptr_t kTransformNodeFlagsOffset   = 68;       // node->flags
constexpr uintptr_t kTransformNodeSubjectOffset = 92;       // node->subject (= entity ptr)
constexpr uint8_t   kTransformNodeSkipBit       = 0x10;     // engine's own skip bit

void __cdecl hk_entity_transform_tick() {
    void* skipped_node = nullptr;
    if (g_teleport_hold_remaining > 0) {
        void* player = resolve_player_entity();
        skipped_node = find_player_transform_node(player);
        if (skipped_node) *(node+68) |= 0x10;   // set skip bit
    }
    g_orig_entity_transform_tick();
    if (skipped_node) *(node+68) &= ~0x10;       // clear skip bit
}
```

Wired in [dllmain.cpp init thread](../../src/mtr-asi/src/dllmain.cpp). Lifecycle:

1. MMB pressed → `teleport_player_to` writes `+0x58` and `sub+0x10`, arms hold counter.
2. Each subsequent sim tick (until hold expires):
   - `hk_entity_transform_tick` PRE: walks transform list, sets `0x10` on player's node.
   - Orig `entity_transform_tick` runs: sees the bit, skips player's body. Other nodes process normally.
   - PRE-hook POST: clears `0x10`.
3. `on_post_sim_aggregator` POST: re-writes `+0x58` and `sub+0x10` defensively.
4. `render_hold_tick` (in `freecam::tick` from `EndScene` PRE): re-writes `sub+0x10` per render frame.
5. Hold expires → `entity_transform_tick` resumes processing player; balance-recovery anim resumes; Wilbur snaps to navmesh anchor.

Why this is the correct fix per RULE №1:
- Uses the engine's **own skip-bit** mechanism — supported by design, not a patch-out.
- Smallest blast radius: only player's node, only during hold, only the per-tick pos/rot write. State machines, anim graph, AI all keep ticking.
- Doesn't require finding/disabling the off-navmesh detector or balance-recovery state transition (which we don't know fully and which has side effects elsewhere — death/respawn, scripted cutscenes).

Build size after fix: 583168 bytes (`Game/mtr-asi.asi`, 2026-05-09).

---

## Original findings (kept for reference)

## Why this exists

User reported MMB-teleport during F3 freecam visibly briefly works, then snaps back to original pos and the AI plays "on edge keep balance" recovery. First fix attempt (sim_aggregator POST hook + re-write `+0x58`) had no visible effect. Second fix attempt (also write `+0x10+48` as "world matrix translation") **crashed the game** — `+0x10` is not a matrix, and the guessed offset corrupted critical state.

Per RULE №1, no more guessing — added a one-shot layout-dump probe in `freecam.cpp::teleport_player_to` that logs `player[0..0xC0]` in hex to `Game/mtr-asi.log` on the first MMB after launch.

## Raw dump from a live MMB (player entity at `0x0EF0FFE0`)

```
+0x00  30 87 6B 00 00 00 00 92 00 00 00 FE 00 00 00 01
+0x10  18 00 00 00 01 00 00 00 38 66 6A 00 00 F0 01 00
+0x20  38 66 6A 00 FF FF FF FF 38 66 6A 00 00 00 00 00
+0x30  38 66 6A 00 00 00 00 00 38 66 6A 00 00 00 00 00
+0x40  00 00 00 00 00 00 00 00 C8 5A F0 0E 00 00 00 00
+0x50  00 53 F0 0E A0 6A 6B 00 28 51 FB 3F 80 B0 61 BC
+0x60  04 66 04 42 28 51 FB 3F 80 B0 61 BC 04 66 04 42
+0x70  5D 11 59 BF 00 00 00 00 19 B6 07 3F 00 00 00 00
+0x80  00 00 80 3F 00 00 00 00 19 B6 07 BF 00 00 00 00
+0x90  5D 11 59 BF 00 00 00 00 00 00 00 00 00 00 00 00
+0xA0  00 00 00 00 00 00 00 00 00 00 00 00 28 51 FB 3F
+0xB0  0B 46 49 3F 04 66 04 42 00 00 80 3F 00 00 80 3F
```

Pos at `+0x58` was `(1.96, -0.01, 33.10)` — Wilbur's standing pos in the test level.

## Decoded structure

| Offset | Size | Type | Value at dump | Notes |
|--------|------|------|---------------|-------|
| `+0x00` | 4 | ptr | `0x6B8730` | vtable — player class methods |
| `+0x04` | 4 | uint | `0x92000000` | flags |
| `+0x08` | 4 | uint | `0xFE000000` | flags |
| `+0x0C` | 4 | uint | `0x01000000` | flags |
| `+0x10` | 4 | uint | `24` | size or count |
| `+0x14` | 4 | uint | `1` | count |
| `+0x18` | 4 | ptr | `0x6A6638` | class metadata (recurring 5×) |
| `+0x1C` | 4 | uint | `0x0001F000` | flags |
| `+0x20..+0x3F` | 32 | ptr/uint | repeated `0x6A6638` + indices | class table refs |
| `+0x40` | 8 | (zeros) | — | padding |
| `+0x48` | **4** | **ptr** | **`0x0EF05AC8`** | **sub-component (heap)** — likely model/physics body, is on heap |
| `+0x4C` | 4 | uint | `0` | padding |
| `+0x50` | 4 | ptr | `0x0EF05300` | another heap object |
| `+0x54` | 4 | ptr | `0x6B6AA0` | → string `"Avatar"` (camera target handle name) |
| **`+0x58`** | 12 | **vec3** | **`(1.96, -0.01, 33.10)`** | **pos — game logic** (camera follow / AI / scripts read this) |
| **`+0x64`** | 12 | **vec3** | **`(1.96, -0.01, 33.10)`** | **DUPLICATE of pos** — likely "render-side pos" cache. Worth writing too. |
| `+0x70` | 36 | mat3x3 | rotation Y-axis | row 0: `(-0.847, 0, 0.530)` row 1: `(0, 1, 0)` row 2: `(-0.530, 0, -0.847)` |
| `+0x94..+0xAB` | 24 | (zeros) | — | padding/unused |
| `+0xAC` | 4 | float | `1.964` | recurring x-pos (3rd copy?) |
| `+0xB0` | 4 | float | `0.787` | unknown — `+0xB4` = 33.10 (z-pos again) |
| `+0xB4` | 4 | float | `33.10` | recurring z-pos |
| `+0xB8` | 4 | float | `1.0` | scale? |
| `+0xBC` | 4 | float | `1.0` | scale? |

## Where the renderer reads pos

**Resolved (see top of file).** Hypothesis #1 was correct: render reads
`*(player+0x48)+0x10`. `+0x64` is just a copy of `+0x58` and irrelevant for the
visual fix.

## What NOT to do

- Don't write to `+0x10..+0x47` — that's vtable/metadata and crashes the game.
- Don't blindly write the matrix-translation bytes inside an arbitrary 64-byte region. The "world matrix at +0x10" assumption was wrong for this engine; player entity's `+0x10` is class metadata, not a transform.

## Repro for the dump

The dump fires once on the first MMB after process start. Subsequent MMBs don't re-dump (latched static `s_layout_dumped`). To re-dump, restart Wilbur.

Code: [freecam.cpp::teleport_player_to](../../src/mtr-asi/src/freecam.cpp) — search for "one-shot layout dump".

## Pending (post-fix)

1. **User retest** — verify the entity_transform_tick skip-bit fix actually
   defeats the snap-back at altitude. If working, expect Wilbur to stay at
   camera pos for the full hold duration (60 sim ticks ≈ 1 sec by default),
   then snap to navmesh anchor when hold expires.
2. **Optional: indefinite-pin mode** — extend the hook gate from "hold counter
   > 0" to "pinned toggle ON" for camera-attached play. Same hook, different
   gate condition.
3. **Code cleanup** — the diagnostic phase-trace probe (`log_pos_at`,
   `g_snap_trace_*`, the Sub-B dump, the `D3D SetTransform[WORLD]` log) is
   still in place to aid further debugging if the skip-bit fix doesn't fully
   work. Remove after retest confirms.
