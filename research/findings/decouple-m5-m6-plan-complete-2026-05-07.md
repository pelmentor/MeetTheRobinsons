# M5 + M6 — plan completion

Closes out the high-fps decouple project plan. M5 NPC interp ships;
M6 polish ships; M3.2 aim-mode auto-snap is documented as deferred.

## M5 — NPC transform interp

### Approach (transform-list walk, simpler than chasing the thunk)

The original plan §M5.1 called for walking the entity registry hash at
`dword_7427C0`. That registry's iterator (`sub_4B6730`) is a
stolen-byte thunk — the body is in our unpacked dump, but reached
through `g_securom_thunk_table_base + N` indirection so reproducing
the iteration semantics from outside is more work than the alternative.
Instead, M5 walks the engine's per-frame
**transform list** at `dword_724DE4`:

- Every entity that has an active transform is in this list.
- The list is a plain singly-linked list with no thunk indirection; nodes have a stable
  layout (verified via the `entity_transform_tick` decompile at 0x4B9F60).
- The engine populates / drains the list each sim tick, so any entity
  we sample from it is by definition "transform-active" — exactly the
  set we want to interpolate.

Node layout we rely on:

```
node + 0x04   next ptr
node + 0x44   flags    (bit 0x10 = "skip transform" — the engine sets
                       this to short-circuit; we mirror it)
node + 0x40   entity ptr (used to skip the player, since M4 covers it)
node + 0x5C   inner-transform ptr (where pos+0x58 + rot+0x70 live)
```

### Storage

- Fixed-size `NpcSlot[64]`, hashed by `(inner_ptr >> 4) & 63` with linear
  probing. Cap chosen so a busy combat scene (typically 20–30 visible
  entities) fits comfortably with headroom.
- Per-slot: `prev` + `curr` snapshots, `saved` (fence pre-image),
  `last_seen_tick`, teleport flag.
- Stale slots aged out after 6 sim ticks of absence (entity left the
  list, was destroyed, etc.).

### Hook flow (mirrors M4)

```
hk_sim_aggregator (PRE orig):
    pre_sim_restore_player()
    pre_sim_restore_npcs()        # M5: copy each saved -> entity inner

    # throttle gate (skip / proceed)

    rc = orig
    on_sim_tick_post()
    post_sim_capture_player()
    post_sim_capture_npcs()       # M5: walk dword_724DE4, snapshot each

hk_camera_apply_all_active (POST orig):
    [M2 view snapshot]
    [M3.1 view interp write]
    [M4 player save + interp write]
    [M5 NPC save + interp write]   # for each valid slot, save -> write lerp+slerp
```

### Per-NPC write protected by SEH

NPCs come and go between sim ticks (bullets despawn, enemies die, etc.).
By the time we `pre_sim_restore_npcs()` an entity may already be freed.
Wrapping the entity-memory writes in `__try / __except
(EXCEPTION_EXECUTE_HANDLER)` catches the access violation, clears the
slot, and continues. Cost is negligible on x86 (no per-call overhead;
just registers a frame handler at function entry).

### Teleport reuse

NPC teleports use the same threshold as the player (default 10 units).
A separate slider could be added later if NPCs commonly translate
faster than 10/tick during normal play.

### Toggle + UI

Tools → Decouple → "Enable NPC transform interp (M5)" + counters
(active slots / writes / teleports). FPS overlay flag `NPC:ON` /
`NPC:off`. Default OFF — user opts in.

## M6 — Polish

### M6.1 Pause / load handling

The engine pauses `simulation_tick_aggregator` invocations during
loading screens and the pause menu (verified by behaviour: throttle
counters stop incrementing). Our throttle naturally inherits this — no
explicit pause handler needed. ALPHA clamps to 1.0 once `now -
curr.qpc` exceeds sim_step, so render pauses on the freshest sim state
without "drift".

### M6.2 Cutscene handling

Covered by M2.2's cut detection (translation threshold 5.0u, rotation
threshold 30°). Both thresholds are live-tunable from the menu — if a
particular cinematic glides through a cut, the user lowers them.
Explicit cutscene-flag detection (per the plan) was investigated but
the only screen-level signal in retail is `ScreenLevelIntro`, which
isn't reliable across all cuts. Cut-threshold detection covers all
observed cases.

### M6.3 Mini-game handling

Already shipped in M1.5 — screen-stack substring detection
(`DigDug` / `MiniHamster` / `ChargeBall` / `WilburMiniGames`) →
`effective_is_throttling()` returns false → throttle and all interp
auto-disable.

### M6.4 UI polish — preset profiles

Three one-click presets at the top of the Decouple section:

- **Quality**: throttle ON (target 60), all interp on (camera+wilbur+NPCs),
  mini-game auto-disable on
- **Performance**: throttle ON only (low-overhead VRR feed), no interp
- **Compatibility**: full decouple OFF (baseline regression toggle)

Presets are convenience apply-buttons that set the underlying toggles —
not modes. Users can tweak anything afterward.

### M6.5 Documentation

- This doc closes M5 + M6.
- [`high-fps-decoupling.md`](high-fps-decoupling.md) updated with a
  "shipped" status banner pointing here.
- Per-milestone shipped docs in `research/findings/decouple-*.md`:
  - [`decouple-hidden-0003-sites-2026-05-07.md`](decouple-hidden-0003-sites-2026-05-07.md) — M1.4
  - [`decouple-m2-snapshot-infra-2026-05-07.md`](decouple-m2-snapshot-infra-2026-05-07.md) — M2
  - [`decouple-m3-view-interp-2026-05-07.md`](decouple-m3-view-interp-2026-05-07.md) — M3.1
  - [`decouple-m4-player-interp-2026-05-07.md`](decouple-m4-player-interp-2026-05-07.md) — M4
  - This doc — M5 + M6
- Memory entries: `project_decouple_m0_m1_shipped.md`,
  `project_decouple_m1_4_m1_5_shipped.md`,
  `project_decouple_m2_shipped.md`,
  `project_decouple_m3_shipped.md`,
  `project_decouple_m4_shipped.md`,
  `project_decouple_plan_complete.md` (final).

## Deferred — M3.2 aim-mode auto-snap

The plan §M3.2 calls for hooking `SetPathCamTargetingBehavior` script
command registration to detect when aim mode is active, then setting
`alpha = 1.0` for view + player interp during aim. The registration
path runs through stolen-byte thunks in the script-command registry — same
class of obstacle as M5's original entity-iterator approach. Static RE
of the registration site (sub_5DC750 → sub_581420 → ...) shows it's a
string-list registration (not callback-pairing); the actual callback
binding is reached via stolen-byte thunks through the runtime IAT (bodies are in our unpacked dump but tracing the registration through that indirection is a multi-day pass).

**Practical workarounds we ship instead:**

1. **Cut detection covers fast aim acquisition.** The default 30°
   rotation threshold (M2.2) snaps interp on rapid view rotations,
   which is exactly what aiming triggers. Lowering this threshold
   (slider in Tools → Decouple) trades aim responsiveness for fewer
   "looking around at constant rate" cuts.
2. **Manual toggle.** The user can flip "Enable view interp (M3.1)"
   off during aiming-heavy gameplay sequences. The Decouple section
   is one click away.
3. **Performance preset.** Sets throttle ON + all interp OFF — gives
   the latency profile of native 60 Hz with the render submission
   benefits of 240 Hz throttle.

This is an honest trade-off — auto-detect would be cleaner but isn't
worth the multi-day script-VM RE for a feature most users won't
notice. A future runtime-trace pass on the script command callbacks
could unblock it.

## Validation

1. Throttle ON, target 60, FPS limit 240
2. **Quality preset** → look for fluid camera + Wilbur + NPCs all at
   240 Hz
3. Compare against **Compatibility preset** (decouple off) for
   baseline regression check
4. Walk the M0.3 protocol (T1–T6) at 30/60/120/240 Hz to confirm
   correct sim speed across rates

## Cumulative scope (all decouple work)

| Milestone | LOC | Status |
|-----------|----:|--------|
| M0 — telemetry + saves doc + protocol + log infra | ~250 | ✅ |
| M1.1 — sim_decouple skeleton + UI | ~150 | ✅ |
| M1.2 — sim_aggregator throttle hook | ~30 | ✅ |
| M1.3 — pathcam_smooth_pretick throttle hook | ~30 | ✅ |
| M1.4 — overlay tween + UV scroll throttles | ~80 | ✅ |
| M1.5 — mini-game auto-disable | ~70 | ✅ |
| M2 — snapshot infra + cut detection | ~300 | ✅ |
| M3.1 — view interp client | ~150 | ✅ |
| M3.2 — aim-mode auto-snap | — | ⏭ deferred (workarounds in place) |
| M4 — player interp + fence | ~250 | ✅ |
| M5 — NPC interp via transform list | ~250 | ✅ |
| M6.1 — pause handling | (inherited) | ✅ |
| M6.2 — cutscene handling | (covered by M2.2) | ✅ |
| M6.3 — mini-game handling | (covered by M1.5) | ✅ |
| M6.4 — preset profiles | ~30 | ✅ |
| M6.5 — documentation | — | ✅ |
| **Total shipped** | **~1590** | **15/16** |

Plan estimate was ~2380 LOC over 27–41 days; actual ~1590 LOC over
two days of intensive work. The savings come from sharing
infrastructure across M3+M4+M5 (single quaternion utility, single fence
pattern, single snapshot model).
