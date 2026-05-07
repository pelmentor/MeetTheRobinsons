# Per-element control v2 — engineering plan (2026-05-06)

Follow-up to the v1 sprite_xform system shipped earlier today. v1 keys
transforms by raw `state_key` only, which we now know is too coarse:
shared atlases (one font texture serving menu+HUD text) collapse
logically-distinct UI into a single addressable bucket. This plan adds
richer identity, makes the editor usable on dynamic UI, fixes edit-time
micro-lags, and lays the groundwork for auto-naming and direct-manip
gizmos.

See [sprite-per-element-architecture.md](sprite-per-element-architecture.md)
for the v1 architecture this builds on, and `memory/project_per_element_first_light_findings.md`
for the live-test findings driving this work.

## Goals (from user feedback)

1. Separate sprites that share a `state_key` (font atlases, sprite sheets)
   so labelling one doesn't move others.
2. Make labelling tractable on dynamic / animated UI (the F1 robot's
   sprites currently "dance" in the live list as they animate in/out).
3. Eliminate edit-time micro-lags.
4. Eventually: auto-populate names from asset paths so manual labelling
   isn't required for the asset axis.
5. Eventually: direct visual manipulation (click sprite → drag handles).

## Constraints / non-goals

- Existing user labels must NOT be lost. Manual work invested in v1 is
  the foundation; v2 specialises it.
- Per-frame cost of `process_list()` must stay sub-millisecond on the
  ~250-entry typical batch.
- Persistence is still best-effort across sessions until phase D ships
  (then by-name, which is robust).
- No new RE work in phases A-C; phase D is the texture-loader RE phase.

## Phased delivery

| Phase | Deliverable                                | Status      | User-visible win |
| ----- | ------------------------------------------ | ----------- | ---------------- |
| A     | Performance: hash slot lookup + debounced ini writes | **SHIPPED 2026-05-06** | Smooth dragging during edit |
| B     | Pin / Capture mode for the live list       | **SHIPPED 2026-05-06** | Can label dancing-robot sprites |
| C     | Composite identity (uv_bucket, screen_ctx, bbox_quad) | **SHIPPED 2026-05-06** | Menu text vs HUD text are separately controllable |
| D     | Auto-naming via texture-loader RE          | **D.3a SHIPPED 2026-05-06** (probe) — D.3b/D.4 blocked on user-captured probe data | Names auto-populate from asset paths |
| E     | Picking + ImGui gizmos                     | pending     | Click sprite → drag handles |
| F     | Per-group bulk operations (interim)        | **SHIPPED 2026-05-06** | "Hide all HUD" with one click |

Phases are roughly independent; A+B can ship in any order, then C, then
D and E. Phase C is the foundation for "real" granularity — D and E are
multipliers on top of it.

## Phase A — Performance

### A.1 Hash slot lookup

**Current:** `find_or_insert(state_key)` (in [sprite_xform.cpp](../../src/mtr-asi/src/sprite_xform.cpp))
linear-scans 64 slots × ~250 entries/frame ≈ 16 K comparisons/frame, plus
LRU search. Bounded but not free, and grows once we add composite keys
in phase C.

**Plan:** add an `unordered_map<KeyTuple, int>` (slot index by key)
maintained alongside the slot array. Insert / evict updates both. The
slot array stays as the storage of record (it's iterated for snapshot
and persistence); the map is just an accelerator.

Phase C's composite key replaces the `uint32_t state_key` map key with
a 64-bit packed tuple (or a small POD with a custom hash). Same
architecture, wider key.

### A.2 Debounced ini writes

**Current:** `save_to_ini()` is called from 22 sites in [menu.cpp](../../src/mtr-asi/src/menu.cpp).
Each call does ~7 `WritePrivateProfileStringA` ops per xform slot
(stat + open + parse + write + close per call). With 20 user-labelled
slots that's ~140 file ops per save. Called on every
`IsItemDeactivatedAfterEdit` — micro-lag confirmed.

**Plan:** introduce a "dirty flag" model.
- New `mtr::ui_aspect_rules::request_save()` — sets `g_save_dirty = true`
  and timestamps the request.
- New `mtr::ui_aspect_rules::flush_pending_save()` — called once per
  frame from menu draw. If dirty AND ≥250 ms since last save attempt,
  do `save_to_ini()` and clear the flag.
- All 22 callsites swap `save_to_ini()` → `request_save()`. The fence
  on tab-close / menu-close / DLL-detach calls `flush_pending_save()`
  unconditionally so nothing is lost on exit.
- Keep `save_to_ini()` public for the "save now" semantics in flush /
  detach paths.

Net effect: a 30-second drag session that touches a slider 200× still
ends with one ini write at the end of the drag, not 200.

## Phase B — Pin / Capture mode

**Current:** the rendered list comes from `snapshot_keys()` every frame,
sorted by `frame_count` desc. As animations play, rows enter and leave
and reorder. Trying to type into a row's `name` field while it's
moving is the dancing-robot problem.

**Plan:** add a frozen-snapshot mode to the UI layer.
- "Pin list" toggle in the per-element TreeNode. When OFF, behaviour
  matches today (live snapshot every frame).
- When ON, the menu uses a saved `Slot[]` snapshot taken at the moment
  of toggling. Edits to name/group/transform via the editor still go
  through `set_name` / `set_transform` etc., so they hit the live slot
  table immediately. Only the *displayed list* is frozen.
- "Capture frame" button — re-snapshots the current frame into the
  frozen list while keeping pin on. This is how the user grabs the
  robot at peak visibility.
- Visual cue: when pinned, the section header gets a (PINNED) tag
  and a bright-yellow border so the user can't forget.

Optionally (cheap add): a "Pause animations" hotkey that gates the
engine's anim tick. Out of scope for B but worth a memory note for
later.

### Implementation surface

Add to [sprite_xform.cpp](../../src/mtr-asi/src/sprite_xform.cpp):

```cpp
// Snapshot the entire slot table (not just live keys) into a caller-
// supplied buffer. Returns count.
int snapshot_slots_full(SlotInfo* out, int max_out);
```

In [menu.cpp](../../src/mtr-asi/src/menu.cpp), keep a static `g_pinned_snapshot[]`
buffer; render from it when pin is on, from live `snapshot_keys()` when
pin is off. UI handles the merge of "edits made via editor while pinned"
by re-pulling each row's transform/name/group through the live getters
inside `render_one_key`, so user edits show up immediately even on
pinned rows.

## Phase C — Composite identity

**The big one.** Replaces `uint32_t state_key` with a 4-tuple:

```cpp
struct CompositeKey {
    uint32_t state_key;       // existing — texture/asset pointer
    uint16_t uv_bucket;       // hash of inline_uvs[8] quantised to 8-bit per axis
    uint8_t  screen_context;  // active screen_push depth bucket at render time
    uint8_t  bbox_quadrant;   // which screen quadrant the centroid falls in (0..8)
};
// Packed to uint64_t for hash key.
```

### Why each component

- **state_key** — the asset axis. Survives from v1; everything we know
  about it stays valid.
- **uv_bucket** — separates glyphs/icons that share an atlas. Two text
  strings drawing from the same font texture have different UVs; one
  number to compare instead of 8 floats.
- **screen_context** — separates the same asset rendered in different
  screens (HUD over gameplay vs same UI in pause overlay). Comes from
  the [screen_push](../../src/mtr-asi/src/screen_push.cpp) mirror — top
  of the stack at the moment `process_list` walks the entry. Coarse
  bucket (active screen depth + a hash of the top name) so we get one
  bucket per "screen state".
- **bbox_quadrant** — last-resort separator for the same asset+UV+screen
  rendered at different positions. The robot's left arm vs right arm,
  if they share atlas + UV (mirroring is common). Cheap to compute, and
  if it's identical across two entries that's evidence they really are
  the same UI element repeated.

### Quantisation choices

- `uv_bucket` = `crc16(inline_uvs[0..7] quantised to 1/256)`. 65 K
  buckets — collisions astronomically rare.
- `screen_context` = top 4 bits = stack_depth (0..15), bottom 4 bits =
  `crc8(top_screen_name) & 0xF`. 256 buckets — sufficient to separate
  "WilburMainMenu @ depth 7" from "WilburGameplay @ depth 4".
- `bbox_quadrant` = bin centroid_x and centroid_y into 3×3 = 9 buckets
  (0..8). Coarse on purpose — fine bbox separation belongs to a
  later feature (a "spatial gate" filter), not the identity tuple.

### Migration of existing v1 data

v1 entries are keyed by `state_key` only. On load:
1. Read each v1 `[sprite_xform]` slot.
2. Insert as a "wildcard" entry: `state_key=K, uv_bucket=*, screen_context=*, bbox_quadrant=*`.
3. Wildcard slots match any composite key with the same state_key, until
   the user disambiguates by adding a more specific variant.

Lookup priority (most specific match wins):
1. Exact composite key match.
2. State_key + screen_context match (uv_bucket and bbox_quadrant wild).
3. State_key only (full v1 fallback).

So a v1 user with "global font @ state 0x19770D00 = scale 0.9" still
gets that transform applied to ALL text using that atlas. When they
later add a more-specific entry "global font + screen=WilburMainMenu =
scale 1.1", the menu screen gets the 1.1, everything else still gets
the 0.9.

### UI changes

The current tree-by-group view stays. Inside each group, rows are now
*per composite key* not per state_key. To avoid overwhelming the user:
- "Collapse identical state_keys" toggle (default ON): rolls all
  variants of one state_key into a single row with a `[+ N variants]`
  expander.
- When expanded, the variants show their distinguishing component
  (e.g. "in screen X", "UV-bucket A", "quad bottom-left").
- "Specialize this slot" button on a row: takes the current composite
  key being rendered and creates a slot for it, copying the wildcard
  parent's transform as the starting point.

### Persistence schema

`[sprite_xform]` ini section gains per-slot fields:
```ini
x_0_state_key=0x19770D00
x_0_uv_bucket=0xFFFF      # 0xFFFF = wildcard (matches any UV)
x_0_screen_context=0xFF   # 0xFF = wildcard
x_0_bbox_quadrant=0xFF    # 0xFF = wildcard
... (existing transform / name / group fields unchanged)
```

Forward-compatible: v1 ini files load as all-wildcard entries (the
default value of unmodified composite components is wildcard).

### Risk: chicken-and-egg with bbox_quadrant

If user transforms move the bbox, the next frame's identity might
differ → entry escapes its own slot. Solution: `bbox_quadrant` is
computed from the entry's bbox **before** apply_transform runs (it
already runs after identity tally in v1; just re-confirm the order).

## Phase D — Auto-naming via texture-loader RE

Hooked engine's texture-create path records `texture_path → state_key`
at load time. Names auto-populate from asset paths, persistence
becomes by-name (robust across sessions).

Multi-day RE — start by tracing back from `sub_565CF0` (sprite-batcher
state-setup consumer of state_key) through SecuROM thunks at runtime
to reach the loader. See `memory/project_ui_granularity.md` "Phase 3
of per-element" pending item for the original sketch.

This phase replaces the user-typed `name` field with an auto-populated
one (still user-overridable). Group field stays manual — groups encode
the user's mental model of UI structure, which the engine doesn't have.

## Phase F — Per-group bulk operations (SHIPPED 2026-05-06)

Interim feature shipped while Phase D awaits probe data. Each
CollapsingHeader for a group gets a compact "Bulk:" row inside it:

- `Hide all` / `Show all` — toggles `hidden` on every slot in the group
- `Reset all` — zeros offsets / restores scale=1 / un-hides every slot
- `Add offset: dx dy [Apply]` — bulk-deltas every slot's offset_x/y by
  user-specified amounts. Lets the user shift an entire UI category in
  one move ("the whole HUD goes 0.05 down").

Implementation lives in [menu.cpp](../../src/mtr-asi/src/menu.cpp) inside
the per-element TreeNode's group rendering loop. Bulk operations call
the `_at` slot APIs and `request_save()` like the per-row controls.

## Phase E — Picking + Gizmos

On top of phase C identity, add direct manipulation:
1. **Picking** — render an offscreen "ID buffer" once per frame: one
   color per composite key. On user click, sample the pixel under the
   cursor → composite key → editor selects that slot.
   Alternative: ray-cast against entry bboxes in screen space. Cheaper
   but less accurate for overlapping sprites.
2. **Translate gizmo** — ImGui-rendered drag-handle overlay anchored
   at selected slot's centroid. Drag horizontally → updates `offset_x`.
   Vertical → `offset_y`. Modifier for uniform scale (drag corner).
3. **Multi-select** — shift-click adds to selection; group transforms
   apply to all members.

This is the "UI editor with gizmos" the user mentioned. Big chunk of
work but well-bounded once C is stable.

## Order-of-operations recommendation

1. Phase A first — half-day, immediately fixes the lag the user is
   currently experiencing while we plan the bigger work.
2. Phase B next — half-day, makes labelling tractable on the dynamic
   UI elements (the robot) so the user can keep building up labels.
3. Phase C — the foundation. Existing v1 entries port forward as
   wildcards; user gradually specialises.
4. Phases D + E parallelisable from there.

## Cross-references

- [sprite-per-element-architecture.md](sprite-per-element-architecture.md)
  — v1 architecture (the foundation this builds on)
- [`src/mtr-asi/src/sprite_xform.cpp`](../../src/mtr-asi/src/sprite_xform.cpp)
  — slot table + `process_list`
- [`src/mtr-asi/src/menu.cpp`](../../src/mtr-asi/src/menu.cpp) — Per-element
  TreeNode (lines ~611-836)
- [`src/mtr-asi/src/ui_aspect_rules.cpp`](../../src/mtr-asi/src/ui_aspect_rules.cpp)
  — `save_to_ini` / `load_from_ini`
- `memory/project_per_element_first_light_findings.md` — what drove
  this plan (the user's live-test report)
