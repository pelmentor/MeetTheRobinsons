// Per-frame sprite quad capture for click-picking and gizmo-driven editing.
//
// sprite_xform::process_list submits each entry's post-transform 4-corner
// quad here (engine-space, before the matrix-builder pipeline applies the
// pillarbox factor + pos_offset). The menu hit-tests the cursor against
// these quads to translate "user clicked here on screen" into "this slot
// owns that pixel", then drives gizmo overlays from the same data.
//
// Threading: both submit (render thread, sprite_xform) and read (render
// thread, ImGui menu) happen on the same thread within one frame — submit
// runs first via the sprite_probe wrapper, draw runs later via ImGui. No
// lock needed for the quad buffer. Selection state and pick-mode toggle
// are accessed only from UI/render thread too; kept as atomics for
// hygiene since they're cross-call state.

#include <atomic>
#include <cstdint>
#include <vector>

namespace mtr::sprite_picking {

namespace {

struct Quad {
    int      slot_idx;
    int      render_order;
    uint32_t state_key;
    float    x[4], y[4];      // engine-space (typically [0,1]² for menu UI)
};

std::vector<Quad> g_quads;
int g_render_counter = 0;

std::atomic<int>  g_selected_slot{-1};
std::atomic<bool> g_pick_mode{false};

// 2D point-in-quad via the standard cross-product test. The 4 corners
// come from the engine's sprite triangle pair — we don't know the
// winding a priori (engines mix CW/CCW for sprite Y-flip), so accept a
// hit if either winding's "all-same-sign" rule passes.
bool point_in_quad(float px, float py, const Quad& q) {
    auto cross = [](float ax, float ay, float bx, float by,
                    float px_, float py_) -> float {
        return (bx - ax) * (py_ - ay) - (by - ay) * (px_ - ax);
    };
    // Order vertices around the quad. The engine emits the 4 inline_positions
    // as two triangles sharing an edge (v0,v1,v2) + (v0,v2,v3) typically,
    // so the perimeter is v0→v1→v2→v3 (or its reverse). Cross-check both.
    const float c01 = cross(q.x[0], q.y[0], q.x[1], q.y[1], px, py);
    const float c12 = cross(q.x[1], q.y[1], q.x[2], q.y[2], px, py);
    const float c23 = cross(q.x[2], q.y[2], q.x[3], q.y[3], px, py);
    const float c30 = cross(q.x[3], q.y[3], q.x[0], q.y[0], px, py);
    const bool all_pos = c01 >= 0 && c12 >= 0 && c23 >= 0 && c30 >= 0;
    const bool all_neg = c01 <= 0 && c12 <= 0 && c23 <= 0 && c30 <= 0;
    return all_pos || all_neg;
}

} // namespace

void begin_frame() {
    g_quads.clear();
    g_render_counter = 0;
}

void submit(int slot_idx, uint32_t state_key, const float* inline_positions) {
    if (slot_idx < 0 || !inline_positions) return;
    Quad q{};
    q.slot_idx     = slot_idx;
    q.render_order = g_render_counter++;
    q.state_key    = state_key;
    for (int v = 0; v < 4; ++v) {
        q.x[v] = inline_positions[v * 3 + 0];
        q.y[v] = inline_positions[v * 3 + 1];
    }
    g_quads.push_back(q);
}

void end_frame() {
    // No swap — single-buffered, render and UI run sequentially per frame.
}

int quad_count() { return static_cast<int>(g_quads.size()); }

void quad_at(int i, int* slot_idx_out, uint32_t* state_key_out,
             float* xy8_out, int* render_order_out) {
    if (i < 0 || i >= static_cast<int>(g_quads.size())) return;
    const auto& q = g_quads[i];
    if (slot_idx_out)     *slot_idx_out     = q.slot_idx;
    if (state_key_out)    *state_key_out    = q.state_key;
    if (render_order_out) *render_order_out = q.render_order;
    if (xy8_out) {
        for (int v = 0; v < 4; ++v) {
            xy8_out[v * 2 + 0] = q.x[v];
            xy8_out[v * 2 + 1] = q.y[v];
        }
    }
}

int pick_engine(float ex, float ey) {
    // Topmost wins: scan back-to-front by render_order. The engine renders
    // sprites in submission order, so later submissions draw on top.
    int best_slot   = -1;
    int best_order  = -1;
    for (const auto& q : g_quads) {
        if (!point_in_quad(ex, ey, q)) continue;
        if (q.render_order > best_order) {
            best_order = q.render_order;
            best_slot  = q.slot_idx;
        }
    }
    return best_slot;
}

// Layered pick. Returns the slot at a given depth under the cursor:
//   layer_index = 0 → topmost (same as pick_engine)
//   layer_index = 1 → second-from-top
//   layer_index = N → Nth from top, or -1 if N >= unique-slot-hit-count.
//
// Atlas-based UI puts text glyphs on top of button frames on top of
// backgrounds; layered picking is what makes "click-the-button-not-its-
// text" reachable without removing topmost-wins as the default.
//
// Multiple quads may map to the SAME slot (one wildcard slot matches
// many entries); we treat repeated slot hits as one layer to keep the
// cycle intuitive — each click yields a visibly-different selection.
int pick_engine_at(float ex, float ey, int layer_index) {
    if (layer_index < 0) return -1;
    // Collect all hits, render_order descending. Cap to a reasonable
    // ceiling — UI never has hundreds of overlapping sprites at one
    // pixel in practice.
    struct Hit { int slot_idx; int render_order; };
    Hit hits[64];
    int n = 0;
    for (const auto& q : g_quads) {
        if (!point_in_quad(ex, ey, q)) continue;
        if (n >= 64) break;
        hits[n++] = {q.slot_idx, q.render_order};
    }
    // Insertion sort by render_order descending (small N, keeps stable
    // tie-break: equal render_orders preserve scan order).
    for (int i = 1; i < n; ++i) {
        Hit h = hits[i];
        int j = i;
        while (j > 0 && hits[j-1].render_order < h.render_order) {
            hits[j] = hits[j-1];
            --j;
        }
        hits[j] = h;
    }
    // Walk in topmost-first order, skipping consecutive duplicates of
    // the same slot_idx so each layer is visibly distinct.
    int unique_count = 0;
    int last_slot = -1;
    for (int i = 0; i < n; ++i) {
        if (hits[i].slot_idx == last_slot) continue;
        if (unique_count == layer_index) return hits[i].slot_idx;
        last_slot = hits[i].slot_idx;
        ++unique_count;
    }
    return -1;
}

int  selected()                  { return g_selected_slot.load(); }
void set_selected(int slot_idx)  { g_selected_slot.store(slot_idx); }
void clear_selection()           { g_selected_slot.store(-1); }

bool pick_mode()                 { return g_pick_mode.load(); }
void set_pick_mode(bool on)      { g_pick_mode.store(on); }

} // namespace mtr::sprite_picking
