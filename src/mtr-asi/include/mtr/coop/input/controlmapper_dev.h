// ControlMapper `dev` pointer swap — engine-wrapper layer for Phase 1.6.
//
// The engine's ControlMapper singleton at static vtable VA 0x006A639C holds
// its per-frame device state behind `this+4` (the `dev` pointer). All 5
// active vtable slots (3=Tick, 4=GetAnalog, 5=IsPressed/WasJustPressed-style,
// 6, 7) read button/analog state through that pointer. For coop per-player
// input routing, this module swaps `this+4` between two `dev` buffers around
// each wilbur entity's per-tick invocation:
//
//   - `g_dev_p1` = the engine's real hardware-fed device captured at first
//                  observation. Used while the Local wilbur is being ticked.
//   - `g_dev_p2` = a zero-state clone allocated on the heap. Used while the
//                  Remote wilbur (orphan) is being ticked, so the orphan's
//                  component chain sees no input.
//
// Analogue of MTA's SwitchContext / Return-to-LocalPlayer pattern at
// reference/mtasa-blue/Client/multiplayer_sa/multiplayer_keysync.cpp:325 —
// except we swap a pointer (cheaper) rather than memcpy a struct, AND we
// must additionally zero per-frame state in dev_p2 because MTR's dev is not
// the value-typed POD that MTA's CPadSAInterface is. See gap-close section
// of research/findings/coop-phase-1-6-input-routing-2026-05-12.md for the
// per-frame offsets confirmed via IDA decompile of CM::vt[4] (GetAnalog),
// CM::vt[5] (WasButtonJustPressed), and sub_572340 (the poll-and-write fn).

#pragma once

#include <cstdint>

namespace mtr::coop::controlmapper_dev {

// Install the engine Tick PRE-hook for first-instance observation. Idempotent.
// Returns true if the hook was placed (or was already in place). The hook is
// a no-op on the hot path once `instance_addr()` becomes non-zero.
bool install();

// Once the first Tick has fired and capture has completed, these return the
// ControlMapper instance address and the engine's real hardware-fed `dev`
// pointer. Both are 0 before capture.
uint32_t instance_addr();
uint32_t dev_p1_addr();

// After capture, dev_p2 is a 4096-byte heap clone of dev_p1 with per-frame
// state zeroed: dev+0x0C..0x43 (18 button pairs + 4 analog axes) cleared to
// zero, and dev+0xB4 (mode_flag dword) cleared so the engine's synthetic-
// input fallback (sub_41A620/sub_41A5E0) does not fire during the P2 tick
// window. The clone preserves dev_p1's vtable (+0), keymap (+0xC0..+0x130),
// and subdevice descriptor metadata (+0x148..0x208) — none of which carry
// per-frame state per the IDA decompile (CM::vt[4] reads `*(dev+0x30+4*idx)`
// directly; CM::vt[5] reads `*(dev+12+2*idx)` and `*(dev+13+2*idx)` directly).
//
// dev_p2 stays at frozen idle until either (a) the network thread calls
// `write_p2_state(...)` in Phase 2.0 to replicate remote P2 input, or
// (b) the engine's poll-and-write function sub_572340 fires against dev_p2.
// The latter is monitored by the install-time PRE-hook on 0x00572340; see
// `poll_probe_stats()` below.
uint32_t dev_p2_addr();

// Per-player swap of *(instance+4). Caller must ensure
// `instance_addr() != 0` (otherwise the swap is a no-op).
//   - idx == 0: restore *(instance+4) = dev_p1 (engine's hardware-fed device).
//   - idx == 1: set    *(instance+4) = dev_p2 (zero-state clone).
//   - other: no-op (defensive).
// Called from per_entity_tick_hook PRE/POST. Single-threaded sim-thread
// invariant — no lock needed.
void swap_to_player(int idx);

// === Poll-fn probe (Phase 1.6 gap-close empirical verification) ===========
//
// PRE-hook on sub_572340 (the dev poll-and-write function — reads keymap at
// dev+0xD0+, queries global subdevice at dev+0xCC via vtable[8], writes per-
// frame state into dev+0x0C..0x44). Counts fires per `this` value to verify
// that the engine does NOT poll dev_p2. If `fires_p2 > 0`, the zero-state
// shim is being overwritten each frame and the swap is broken until we add
// a NOP-when-this==dev_p2 gate.
//
// In the default success case (fires_p2 == 0 after gameplay enters), the
// zero-state shim is sufficient.
struct PollProbeStats {
    uint64_t fires_p1;     // poll fired with this == dev_p1 (expected primary)
    uint64_t fires_p2;     // poll fired with this == dev_p2 (RED FLAG if > 0)
    uint64_t fires_other;  // poll fired with this == some other address
};
PollProbeStats poll_probe_stats();

// === Phase 2.0 P2 input write (network thread) ============================
//
// Called by the network thread when a P2 input snapshot arrives. Writes
// 18 button "curr" bytes (with engine's prev/curr cycle preserved) and 4
// analog axis floats into dev_p2's per-frame state region. Trade-off vs
// MTA's full-struct memcpy at multiplayer_keysync.cpp:325 — we write only
// the per-frame fields, leaving keymap and subdevice pointers untouched
// (cheaper, and avoids writing to engine-owned regions of dev_p2).
//
// Thread safety: writes are to dev_p2 which is our heap allocation. The
// sim thread reads dev_p2 only inside the per-entity-tick swap window;
// writes from the network thread outside that window are safe. A write
// arriving during a sim-thread read of the same float field gives a torn
// read of one snapshot (recovers next frame, snapshot protocol is fully
// idempotent). Single-byte button writes are atomic on x86.
//
// Currently a Phase 2.0 stub: writes the inputs but no remote caller is
// wired yet (no network protocol for P2 input has been designed). See
// research/findings/coop-phase-1-6-input-routing-2026-05-12.md "Phase 2.0
// forward-compat" for the API rationale.
struct P2InputState {
    // 18 button-state "curr" bytes, written to dev_p2+0x0D + 2*i for i=0..17.
    // The engine's prev/curr cycle (used by CM::vt[5] = WasButtonJustPressed
    // for edge detection) is preserved by `write_p2_state` shifting the old
    // curr into prev BEFORE writing the new curr. 1 = pressed, 0 = released.
    uint8_t buttons[18];
    // 4 analog axes, written to dev_p2+0x34 + 4*i for i=0..3. These map to
    // the engine's v4 mapping values 1..4 (the only values used in normal
    // gameplay; v4=0 at dev+0x30 is never read). Range -1.0..+1.0; 0.0 =
    // centered. CM::vt[4] (GetAnalog) reads these as the engine's
    // controller-axis input source.
    float   analogs[4];
};
void write_p2_state(const P2InputState& s);

} // namespace mtr::coop::controlmapper_dev
