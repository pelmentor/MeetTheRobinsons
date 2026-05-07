# Testing checklist — what to verify in `mtr-asi.asi` (2026-05-05)

A user-facing test plan for verifying current builds + diagnosing the
remaining "corner cull" + "UI aspect" issues. Walk through these in
order; each step has a clear pass/fail signal.

## Pre-flight

1. Launch game with `Game/mtr-asi.asi` deployed.
2. After title screen / first level loads, press `Insert` to open the
   mtr-asi menu.
3. Confirm the menu opens without crashing the game.

## Test 1 — Cvar dump (works)

**Expected:** Insert → Tools → "Cvar dump" section shows
"Registered so far: 13000+" after a level loads. Click "Dump cvars to
mtr_cvars.txt". File appears at `Game/mtr_cvars.txt`.

**Why:** this is the RE infrastructure that surfaced our globals. Verify
it still works to capture future-build snapshots.

## Test 2 — Fog disable (works)

**Expected:** Insert → World → "Fog" section → check "Disable fog". Fog
visibly disappears. Toggle off → fog returns.

**Why:** confirms the data-write override mechanism works for cvars the
engine reads each frame. This is the canonical "working knob" baseline.

## Test 3 — Draw distance (works)

**Expected:** Insert → World → "Draw distance" → set to 50000 (or
similar large value). Pan FreeCam (F3 to enter freecam) — far geometry
that previously disappeared at the horizon should now be visible.

**Why:** confirms per-camera frustum buffer write works.

## Test 4 — vis_test_probe (DIAGNOSTIC — main test for this session)

**This is the high-value test for diagnosing the corner-cull symptom.**

**Steps:**
1. Insert → Tools → "[diagnostic] vis_test probe (IAT-slot patch)"
   section.
2. Verify "Probe: ARMED" shows in green. If it shows "not armed
   (waiting for SecuROM unpack)", wait 30 seconds — the probe polls
   the IAT slot for up to 30s at startup. After that, if still not
   armed, check `Game/mtr-asi.log` for "vis_test_probe: gave up..."
   (means SecuROM never resolved the slot — that would be a
   problem to investigate separately).
3. Note the per-frame counters update each frame (you may need to
   move the camera or wait a frame to see updates).
4. **Baseline** — note "Last frame: N calls, M pass (X.X%)" while
   in normal gameplay (no FreeCam, looking at scenery).
5. **Force-pass test** — check "Force-pass via wrapper". Enter FreeCam
   (F3) and pan to a corner. Compare:
   - If corner-cull DISAPPEARS with force_pass on → vis_test IS the
     gate, and the IAT-slot mechanism succeeds where the call-site
     rewrite failed (call-site rewrite has the same effect in theory;
     possible reason for prior failure: the toggle didn't take effect
     OR a 5th call site exists we didn't find).
   - If corner-cull PERSISTS with force_pass on → vis_test is NOT
     the gate at any of its call sites. The cull is upstream — most
     likely the `(scene+104) bit 0` flag or an entirely different
     system (sectors / occluders).
6. **Per-site distribution** — note which call sites have non-zero
   counts. If sub_4BC340 dominates, that's the upstream cull doing
   most of the work. If sub_4C3790 dominates, downstream is bigger.

**Report back:** the pass-rate %, whether force_pass closes the corner-
cull, and the per-site distribution.

## Test 5 — Periphery culling (DEAD per prior testing — re-verify with corrected math)

**Steps:**
1. Insert → World → "Periphery culling (corner-cull suspect)".
2. Verify "Angle decoded" shows ~51° initially (engine default).
3. Click "Disable periphery (angle=90°, dist=1e6)". Verify decoded
   angle now shows 90° and dist value updated.
4. Enter FreeCam, pan corners. **Check if corner-cull pattern changed.**

**Expected (per prior testing):** no observable effect.

**If different result this time:** my math was wrong. Report back.

## Test 6 — `force_vis` multi-site (DEAD per prior testing — re-verify)

**Steps:**
1. Insert → World → "[experimental] Force-pass vis_test (4 call sites)".
2. Check the box. Watch `Game/mtr-asi.log` for **four** "force_vis: ON …"
   lines confirming each site patched.
3. Compare:
   - Without force_vis: normal corner cull
   - With force_vis on: corner cull should disappear if vis_test is the
     gate
4. If render glitches (z-sort, transparency), toggle off and report.

**Expected (per prior testing):** no observable effect on corners.

**Alternative reading:** if Test 4's `vis_test_probe::set_force_pass`
DOES open corners but Test 6's `force_vis` doesn't, the issue is the
call-site rewrite mechanism (5-byte patch) not properly bypassing —
possibly a 5th call site we didn't find. The IAT-slot patch hits 100%
of callers regardless.

## Test 7 — UI aspect override (currently DEAD)

**Steps:**
1. Insert → Display → "UI aspect — default (no rule match)".
2. Set value to 4:3 (1.333). Open any in-game menu / pause-menu.
3. **Check if HUD/menu pillarboxes** to 4:3 within widescreen.

**Expected (per prior testing):** no observable effect.

**Why:** the actual HUD path doesn't use `sub_562B70` (the hook target).
See [`ui-render-investigation.md`](ui-render-investigation.md) for full
analysis.

For per-screen rules: Insert → Display → "UI aspect — per-screen rules".
Verify "Current top screen" updates when screens push (e.g., open pause
menu — should show "ScreenPauseMenu" or similar). The rules engine
itself works; just the ortho hook never matches the actual HUD.

## Test 4b — Scene-visibility tracker (NEW 2026-05-06)

**This is the runtime confirmation for the static-RE conclusion that
`(scene+104) bit 0` is not the corner-cull driver. Counters near zero
during a corner-pan = static conclusion confirmed; counters spike =
some script is doing per-frame visibility based on camera state.**

`mtr-asi.asi` now hooks `scene_set_visible` (sub_4AABC0) and
`script_set_instance_hidden` (sub_5E3DC0) — the two writers of
(scene+104) bit 0 that scripts can drive. Per-frame counters surface in
the menu.

**Steps:**
1. Insert → Tools → "[diagnostic] scene visibility tracker" section.
2. **Baseline:** read "Last frame:" while the camera is stationary
   looking at scenery. Should be 0 hides / 0 shows / low script-call
   count (< ~10 script calls/frame for typical script tick).
3. **Corner-cull test:**
   - Enter FreeCam (F3).
   - Slowly pan the camera so geometry "disappears" at the corner —
     reproduce the symptom.
   - Watch the "Last frame: N hides" number while panning.
   - **Pass condition (the test that resolves the cull-thread):**
     - If hides spike when panning → that's the cull. The "Distinct
       scenes hidden this frame" list shows the addresses we need to
       investigate. Note the addresses + report back.
     - If hides stay at 0 → `(scene+104) bit 0` is confirmed not the
       cull. The remaining suspect is the render-context list
       populator filtering scenes upstream of vis_test.
4. **Cumulative sanity check:** after a few minutes of play, "Cumulative"
   counts should be reasonable: shows ≈ initial scene-spawn count,
   hides ≈ small (only level-transition / scripted hides). If these
   numbers are wildly off, the hooks themselves are misbehaving.

**Report back:**
- Last-frame hides during corner-pan vs. baseline
- Any sticky scene addresses captured during the symptom
- Whether script_calls/frame correlates with camera motion

## Test 7b — Sprite-batcher matrix-builder diagnostic (NEW 2026-05-05)

**This is the high-value test for confirming the actual HUD path.**

`mtr-asi.asi` now logs every distinct call to the two matrix builders
the sprite batcher uses (`matrix_set_via_xform_a` at 0x562AA0,
`matrix_set_via_xform_b` at 0x562AE0). These hooks are pure logging —
no behavior change.

**Steps:**
1. Launch game, load a level.
2. After title + first level loads, open `Game/mtr-asi.log` (Notepad++,
   `tail -f`-style tools, etc.).
3. Search the log for `MatrixSetXformA` and `MatrixSetXformB`.

**Expected:**
- At least one `MatrixSetXformA` line should appear, expected args
  `a1=2.0 a2=-2.0 a3=1.0` (raw `0x40000000 0xC0000000 0x3F800000`).
- At least one `MatrixSetXformB` line, expected `a1=-2.0 a2=-2.0 a3=0.0`.
- Caller pointer for both should be inside `render_sprite_batcher`
  (around `0x4E8DE0..0x4E8DF0`).

**What this tells us:**
- **Lines present:** confirms the sprite batcher fires per-frame, and
  that path IS active during normal HUD rendering. Next iteration will
  add aspect substitution at these sites — when the user sees the HUD
  pillarbox, we know it's the correct injection point.
- **Lines absent:** the sprite batcher itself is dormant in the HUD
  case, and the actual HUD path is somewhere else entirely (XYZRHW
  vertices, 3D-near-camera, or per-screen render). We'd need to
  instrument D3D-level `DrawPrimitive` / `SetFVF` next.
- **Different args than expected:** would indicate our static RE missed
  another caller of these helpers — we'd track that down.

**Report back:** the args from each MatrixSetXform[A|B] log line and
their caller pointers. That's all needed.

## Test 7c — Sprite-batcher matrix override (NEW 2026-05-06)

**Run this only after Test 7b confirms the matrix builders fire on HUD.**

Static RE has fully decoded the matrix builders (see
[ui-render-investigation.md](ui-render-investigation.md)):
- A: builds `scale(sx, sy, sz)` — default sx=2, sy=−2, sz=1
- B: builds `translate(tx, ty, tz)` — default tx=−2, ty=−2, tz=0

The composed effect on a vertex (x,y,z) is `(sx·x + tx, sy·y + ty,
sz·z + tz)`. For HUD pillarbox to 4:3 in 16:9, multiply `sx` and `tx`
by `0.75` (= target/screen). The menu exposes this as a one-click
"Auto-pillarbox to 4:3" button.

**Steps:**
1. Verify Test 7b confirmed the path is live (log lines exist).
2. Insert → Display → "[experimental] sprite-batcher matrix override".
3. Click **"Auto-pillarbox to 4:3"** (computes 4/3 / screen_aspect and
   sets sx, tx multipliers; enables override).
4. Watch the HUD / menus.

**Pass conditions:**
- HUD content pillarboxes correctly into a 4:3 area with black bars at
  sides → SOLVED. The math derived from static RE is right.
- HUD content compresses but is **off-center** (shifted left or right)
  → tx multiplier is wrong; tweak its slider to re-center.
- HUD content scales correctly horizontally **and vertically too** (so
  it shrinks instead of pillarboxing) → both axes were affected when
  only X should be; check that mul sy and mul ty stayed at 1.0.
- HUD shows no visible change → either:
  - The matrix builders fire only on debug overlays (not HUD), in which
    case the actual HUD path is elsewhere (sub_4B5180 virtual dispatch
    or D3D-level XYZRHW vertices). Test 7b log lines tell us which.
  - Or the override path is broken; reset and check log for hook errors.
- HUD breaks (skewed, missing, garbled) → click "Reset all to 1.0 +
  disable" and report which axis broke.

**Report back:**
- Result of the Auto-pillarbox button: pillarbox / shifted /
  no-effect / broken.
- If shifted: tx slider value that recenters cleanly.
- Per-screen comparison: did the pause menu, in-game HUD, and dialog
  boxes all respond the same way?

## Test 7d — Per-screen UI aspect via auto-from-rules (NEW 2026-05-06)

**Run after Test 7c confirms manual pillarbox works.**

The mtr-asi menu now has a "Drive sx/tx from ui_aspect_rules" toggle
that wires the per-screen rules table directly into the sprite-matrix
override. When on, each frame:

1. Read current top-screen name (push-tracked, pop-tracked).
2. Find the first matching rule's target aspect.
3. Compute factor = target / screen.
4. Apply factor to scale.x AND translate.x of the sprite-batcher
   transform — pillarbox the HUD into the rule's target aspect.

**Steps:**
1. Insert → Display → "UI aspect — per-screen rules". Add rules:
   - pattern "PauseMenu" -> 1.333 (4:3 pause menu)
   - pattern "MainMenu" -> 1.333 (4:3 main menu)
   - pattern "Loading" -> 0.0 (no override during loading screens)
2. Insert → Display → "[experimental] sprite-batcher matrix override".
3. Check "Enable substitution".
4. Check "Drive sx/tx from ui_aspect_rules (per-screen, auto)".
5. The status line below shows the active rule's factor live as you
   navigate screens.
6. Open the pause menu → verify it pillarboxes to 4:3.
7. Resume gameplay → HUD should return to native (no rule match).
8. Open main menu → 4:3 again.

**Pass conditions:**
- Each screen pillarboxes to its rule's target aspect, gameplay HUD
  stays native.
- Status line in the override section shows the correct factor for
  each screen.
- Top-screen depth/name in the rules section updates correctly when
  navigating + backing out.

**Report back:**
- Whether the auto-mode pillarbox tracks screen changes correctly.
- Any screen names that the rules system fails to detect (use the
  "Screen stack" tree node to see what's on the stack).

## Test 8 — FreeCam (works)

**Steps:**
1. Press F3 to toggle FreeCam.
2. WASD to move, mouse to look, Space/C for up/down, Shift to boost,
   mouse wheel to adjust speed.
3. Verify camera detaches from player and player stops moving (DInput
   suppression).
4. Press F3 again to restore.

**Why:** sanity check FreeCam still works (it's the test vehicle for
all the cull/draw-distance investigation).

## Common gotchas

- **Probe shows "not armed"**: the SecuROM IAT slot at 0xF92F34 wasn't
  resolved by the time we tried to patch. Wait the full 30s; if still
  not armed, log an issue.

- **Game crashes on startup**: this happened before with the unified
  cvar-dump signature bug. If it recurs, check `Game/mtr-asi.log`
  immediately for the last line — that's the closest hook to the crash.
  Common pattern: a function we hook had wrong arg count → callee-cleanup
  ate too much / too little stack.

- **No log output**: `Game/mtr-asi.log` should always have at least the
  "DllMain attach" line. If absent, the .asi didn't load — check
  `dxwrapper.dll` is present and `dxwrapper.ini` has
  `EnableD3d9Wrapper=1`.

## What to report back

For the corner-cull investigation specifically:

1. **Test 4 results**: pass/fail of force_pass via wrapper, the per-site
   distribution.
2. **Whether the cvar dump file looks intact** (if you've made changes
   to cvar_dump and want to re-verify).
3. **Any new render glitches** introduced by force_vis or force_pass —
   especially transparency / z-sort issues, since those would suggest
   we're bypassing necessary culling somewhere.

These three data points narrow down the next RE direction sharply:
- If vis_test is the gate (force_pass works) → finalize a clean override
- If vis_test is NOT the gate → next is hooking writers of `(scene+104)`
  bit 0 to find what marks scenes hidden in the corner-cull pattern
