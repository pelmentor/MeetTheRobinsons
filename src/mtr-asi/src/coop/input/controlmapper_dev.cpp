// ControlMapper dev-pointer module — Step 1: one-shot dev_p1 capture + dump.
// See mtr/coop/input/controlmapper_dev.h for the design rationale.

#include "mtr/coop/input/controlmapper_dev.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop::controlmapper_dev {

namespace {

// === Engine VAs ============================================================
//
// ControlMapper singleton's static vtable lives in .rdata at this VA. Slot
// layout (audited at research/findings/coop-phase0-input-separation-point-
// 2026-05-11.md): 0 = dtor, 1 = nullsub, 2 = RET stub, 3 = Tick (single
// caller at 0x0056F361, fires once per sim frame), 4..7 = active input
// readers, 8..12 = profile setters that never fire in gameplay.
constexpr uintptr_t kCmVtableVA = 0x006A639C;
constexpr int       kSlotTick   = 3;

// Per the research doc, `dev` lives at `this+4` on the ControlMapper.
constexpr uintptr_t kDevPtrOffset = 0x04;

// Step 1 dumps this many bytes from dev_p1+0 to characterise the device
// struct layout. 512 B is enough to cover the button ring (typically
// dev+12..dev+47) and the analog axes (dev+48..dev+63) with margin for
// trailing fields.
constexpr int kDevDumpDwords = 128;  // 128 dwords = 512 bytes

// Step 2: dev_p2 clone size. The live capture (2026-05-12) showed dev_p1
// activity through offset +0x1A4 (~420 bytes). 4096 bytes overshoots by 9x
// to capture any trailing fields we haven't observed. Cheap — single
// per-process allocation. Larger than necessary but small enough to be
// negligible.
constexpr size_t kDevP2Size = 4096;

// === Phase 1.6 gap-close: per-frame state offsets (RE'd 2026-05-12) ========
//
// Per the IDA decompile of CM::vt[5] (WasButtonJustPressed at 0x0056E940),
// the button state is at `*(dev + 12 + 2*idx)` (prev byte) and
// `*(dev + 13 + 2*idx)` (curr byte) for idx 0..17. Range: dev+0x0C..0x2F
// (36 bytes for 18 button-state pairs).
//
// Per the IDA decompile of CM::vt[4] (GetAnalog at 0x0056E8D0), the analog
// state is at `*(float*)(dev + 48 + 4*idx)`. The poll-and-write fn at
// 0x00572340 writes 4 axes at dev+52..+64 (= +0x34..+0x40), and CM::vt[4]
// reads up to idx=4 (= +0x40). Range: dev+0x30..0x43 (20 bytes).
//
// Combined per-frame state range: dev+0x0C..0x43 inclusive = 0x38 bytes
// (56 bytes covering buttons + analog).
constexpr uintptr_t kPerFrameStateOffset = 0x0C;
constexpr size_t    kPerFrameStateBytes  = 0x38;

// The mode_flag at dev+0xB4 enables the synthetic-input fallback. Per
// ControlMapper::Tick_body decompile: `if (*(_DWORD*)(dev + 180) && axis == 0)
// axis = sub_41A620(idx, 0)`. The fallback fires only when this flag is
// non-zero AND the dev-poll returned 0 for the axis. Zeroing this DWORD on
// dev_p2 fully disables the fallback during the P2 tick window. Width is
// 4 bytes per the dword load — a single-byte clear is insufficient.
constexpr uintptr_t kModeFlagOffset = 0xB4;

// === sub_572340 poll-fn probe (Phase 1.6 gap-close empirical verifier) ====
//
// The dev poll-and-write fn at this VA queries the global subdevice at
// dev+0xCC via vtable[8] for each keymap entry (dev+0xD0..0x130) and writes
// per-frame state into dev+0x0C..0x44. IDA finds zero direct xrefs — the
// function is reached via vtable-indirect dispatch (likely dev's own vtable
// slot 1 or 8 routed through securom-thunks at ControlMapper's tick path).
// MinHook patches the function prologue itself, so the indirect dispatch
// is not an obstacle to hooking. Prologue verified 2026-05-12:
//   0x572340: 53           push ebx
//   0x572341: 56           push esi
//   0x572342: 8B F1        mov  esi, ecx
//   0x572344: 8B 86 D0 00  mov  eax, [esi+0xD0]
//   ... 16 clean bytes before the first branch — plenty for MinHook's
//   5-byte JMP.
//
// The probe logs the first N fires per `this` value and reports counts
// via poll_probe_stats(). If fires_p2 > 0 in a normal autonomous test,
// the zero-state shim is being overwritten each frame and we need to add
// a NOP-when-this==dev_p2 gate inside hook_poll.
constexpr uintptr_t kPollFnVA = 0x00572340;

// === Trampolines ===========================================================
//
// Engine Tick is __thiscall(void*). With MinHook this is reached via the
// __fastcall convention: ECX = this, EDX = unused.
using PFN_Tick = void (__fastcall*)(void* this_, void* edx_dummy);
PFN_Tick g_orig_tick = nullptr;

// Engine poll-fn at 0x00572340 is __thiscall(dev*). Returns the last
// button-state byte written (al), which we forward as-is.
using PFN_Poll = char (__fastcall*)(void* this_, void* edx_dummy);
PFN_Poll g_orig_poll = nullptr;

// === State =================================================================

std::atomic<bool>     g_captured    {false};
std::atomic<uint32_t> g_instance    {0};
std::atomic<uint32_t> g_dev_p1      {0};
std::atomic<uint32_t> g_dev_p2      {0};  // heap clone, populated in try_capture

// Poll-fn fire counters (populated by hook_poll). Relaxed loads/stores are
// sufficient: heart-beat logging tolerates a 1-frame stale count.
std::atomic<uint64_t> g_poll_fires_p1    {0};
std::atomic<uint64_t> g_poll_fires_p2    {0};
std::atomic<uint64_t> g_poll_fires_other {0};

// Forward decl — body is below try_capture so the install path stays linear.
void install_poll_probe();

// === SEH-guarded read ======================================================

uint32_t seh_read_dword(uint32_t addr) {
    if (!addr || addr < 0x10000) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool seh_memcpy(void* dst, uint32_t src_addr, size_t bytes) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src_addr), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_write_dword(uint32_t addr, uint32_t value) {
    if (!addr || addr < 0x10000) return false;
    __try {
        *reinterpret_cast<uint32_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// === Cold-path one-shot capture ===========================================

void try_capture(void* this_) {
    const uint32_t instance = reinterpret_cast<uint32_t>(this_);
    const uint32_t vt       = seh_read_dword(instance);
    if (vt != kCmVtableVA) {
        // Not the ControlMapper singleton — different class invoked our hook.
        // Should not happen since we hooked Tick by its function VA, but
        // a paranoia bail is cheap and keeps the dump truthful.
        return;
    }

    const uint32_t dev = seh_read_dword(instance + kDevPtrOffset);
    if (!dev) {
        // ControlMapper exists but `dev` hasn't been attached yet. The Tick
        // body itself reads `dev` so it must be non-null by the time it
        // executes; if we see 0 here, the engine is about to crash anyway.
        // Don't latch; let the next call retry.
        return;
    }

    bool expected = false;
    if (!g_captured.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
        return;  // another thread won; nothing to do
    }

    g_instance.store(instance, std::memory_order_release);
    g_dev_p1  .store(dev,      std::memory_order_release);

    mtr::log::info("[cm_dev] CAPTURED instance=0x%08X vt=0x%08X dev_p1=0x%08X",
                   instance, vt, dev);

    // Dump dev_p1+0..(kDevDumpDwords*4) as dwords. Log only non-zero entries
    // to keep the log readable; the layout will reveal itself as offsets
    // with non-zero scratch values (recent button presses, ring indices,
    // device-alive byte).
    int nonzero = 0;
    for (int i = 0; i < kDevDumpDwords; ++i) {
        const uint32_t off = static_cast<uint32_t>(i) * 4u;
        const uint32_t v   = seh_read_dword(dev + off);
        if (v != 0) {
            mtr::log::info("[cm_dev]   dev_p1+%04X = 0x%08X (%u)",
                           off, v, v);
            ++nonzero;
        }
    }
    mtr::log::info("[cm_dev] dev_p1 dump complete: %d non-zero dwords in "
                   "first %d bytes.", nonzero, kDevDumpDwords * 4);

    // Allocate dev_p2 + memcpy from dev_p1. The clone preserves vtable,
    // keymap, and subdevice descriptor metadata — the engine expects these
    // valid when a consumer reads from `*(instance+4)`. The per-frame state
    // (button pairs at dev+0x0C..0x2F, analog axes at dev+0x30..0x44, and
    // the synthetic-fallback mode_flag at dev+0xB4) is THEN zeroed below
    // so that consumers reading dev_p2 see "no input" during the P2 tick
    // window. Phase 1.6 gap-close fix per
    // research/findings/coop-phase-1-6-input-routing-2026-05-12.md.
    //
    // Allocated via std::malloc — heap memory, RW, naturally aligned. No
    // VirtualAlloc needed since we don't execute from this buffer.
    void* p2_buf = std::malloc(kDevP2Size);
    if (!p2_buf) {
        mtr::log::info("[cm_dev] dev_p2 alloc FAILED (out of memory). "
                       "swap_to_player(1) will be a no-op.");
        return;
    }
    std::memset(p2_buf, 0, kDevP2Size);
    if (!seh_memcpy(p2_buf, dev, kDevP2Size)) {
        // Source isn't 4096 bytes — fault past the end. Fall back to a
        // smaller clone size (the known-active region from the dump).
        std::memset(p2_buf, 0, kDevP2Size);
        if (!seh_memcpy(p2_buf, dev, 512)) {
            mtr::log::info("[cm_dev] dev_p2 clone FAILED (SEH at 512 B too). "
                           "Releasing buffer; swap_to_player(1) will be a no-op.");
            std::free(p2_buf);
            return;
        }
        mtr::log::info("[cm_dev] dev_p2 clone fell back to 512 B (4096 B "
                       "faulted past dev_p1 end).");
    }

    // Phase 1.6 gap-close: zero per-frame state in dev_p2. The shallow
    // memcpy left engine-side button/analog values cloned from dev_p1; we
    // need them ZERO so the P2 tick window observes no input. Per the IDA
    // decompile of CM::vt[4] (GetAnalog), CM::vt[5] (WasButtonJustPressed),
    // and ControlMapper::Tick_body, all per-frame state reads go directly
    // through `*(instance+4)+0x0C..0x44` — no subdevice-pointer chase. The
    // descriptor array at +0x148+ (which the original audit flagged as the
    // sharing site) is metadata, not state. See research-finding section
    // "Phase 1.6 gap-close" for the IDA verification record.
    //
    // ACCEPTED KNOWN-UNKNOWN (audit 2026-05-12): the unzeroed range
    // dev_p2+0x44..+0xB3 retains dev_p1's clone-time values, which the
    // 2026-05-12 capture dump showed as: +0x80 = 0xBF800000 (-1.0f),
    // +0x8C = 0xBF800000, +0x98 = 0xBF800000, +0xA8 = 0x04000100. These
    // are likely device-class metadata / calibration sentinels rather than
    // per-frame state. None of CM::vt[3..7] (the decompiled active slots)
    // were observed reading from +0x44..+0xB3 during the autonomous test.
    // If P2 ever exhibits axis-inversion / scale-distortion symptoms during
    // LAN dual-launch, re-RE the CM slots and widen the zero range. Until
    // then, accept the risk to avoid overwriting calibration data.
    auto* p2_bytes = reinterpret_cast<uint8_t*>(p2_buf);
    std::memset(p2_bytes + kPerFrameStateOffset, 0, kPerFrameStateBytes);

    // Zero the mode_flag dword at dev+0xB4. The synthetic-fallback path
    // (sub_41A620 / sub_41A5E0) is gated on `*(_DWORD*)(dev+180)`; a byte
    // clear would leave the upper 3 bytes of the dword set and the fallback
    // could fire spuriously. Write a full 4-byte zero to be width-correct.
    *reinterpret_cast<uint32_t*>(p2_bytes + kModeFlagOffset) = 0u;

    g_dev_p2.store(reinterpret_cast<uint32_t>(p2_buf),
                   std::memory_order_release);
    mtr::log::info("[cm_dev] dev_p2 cloned + zero-state shimmed: addr=0x%08X "
                   "size=%zu. per-frame state +0x%02X..+0x%02X cleared (%zu B); "
                   "mode_flag dword @+0x%02X = 0 (synthetic-fallback disabled).",
                   reinterpret_cast<uint32_t>(p2_buf), kDevP2Size,
                   static_cast<unsigned>(kPerFrameStateOffset),
                   static_cast<unsigned>(kPerFrameStateOffset
                                         + kPerFrameStateBytes - 1),
                   kPerFrameStateBytes,
                   static_cast<unsigned>(kModeFlagOffset));

    // Install the poll-fn probe now that dev_p1 and dev_p2 are both known.
    // The probe counts fires per `this` value so we can verify empirically
    // that the engine does NOT poll dev_p2 during the autonomous test (if
    // it does, our zero-state shim is overwritten each frame and we need to
    // add a NOP-when-this==dev_p2 gate inside hook_poll).
    install_poll_probe();
}

// === Hook body =============================================================
//
// Step 6 — ControlMapper::Tick is GLOBAL, not per-player (2026-05-12).
// ControlMapper::Tick fires exactly ONCE per sim frame (caller at
// 0x0056F361 inside the engine's sim aggregator). It reads `*(this+4)`,
// which is the `dev` pointer at the moment of the call.
//
// Per-player routing happens at the wilbur-tick layer, NOT here. The
// per_entity_tick_hook brackets each wilbur's tick with
// `swap_to_player(idx)` PRE / `swap_to_player(0)` POST. Since the engine
// only invokes Tick once per sim frame — and that invocation lives
// OUTSIDE any wilbur-tick window (it runs from the sim aggregator,
// not from inside a wilbur tick) — Tick always observes dev_p1 (the
// engine's hardware-fed device).
//
// Implication for dev_p2: dev_p2 is NEVER read during Tick. Its
// per-frame state is observed only by `sub_572340` (the poll-and-write
// fn) which is what hook_poll instruments. Empirically (gap-close ship,
// 2026-05-12), fires_p2 stays at 0 across 3600+ poll invocations in
// single-player. The only writes to dev_p2's per-frame state come from
// `write_p2_state` (called by the network thread when remote P2 input
// arrives) — see header for the Phase 2.0 API. There is no "double
// tick" — there is only one global Tick per frame, and the engine
// never runs it against dev_p2. Documenting this explicitly because the
// intuition from MTA's per-ped CRemoteDataSA would suggest two reads
// per frame; in MTR's architecture, dev_p2 is a write-only buffer from
// the engine's perspective and a read-only buffer for the network side.
void __fastcall hook_tick(void* this_, void* edx_dummy) {
    // Hot path: relaxed load, branch-predict-friendly. After the first
    // successful capture, this branch is never taken again.
    if (!g_captured.load(std::memory_order_relaxed)) {
        try_capture(this_);
    }
    g_orig_tick(this_, edx_dummy);
}

// === Poll-fn hook body =====================================================
//
// PRE-hook on the dev poll-and-write fn at 0x00572340. Classifies `this`
// against {dev_p1, dev_p2, other} for counting, logs first N fires per
// class plus heartbeats, then forwards to the trampoline.
//
// Current Phase 1.6 state: observation-only forward. We expect fires_p2
// to stay at 0 throughout the autonomous test (the engine should only
// poll dev_p1, not dev_p2 — the swap happens AFTER the global poll). If
// fires_p2 > 0, the gap-close zero-state shim is being overwritten and we
// need to add a NOP-when-this==dev_p2 gate here.
char __fastcall hook_poll(void* this_, void* edx_dummy) {
    const uint32_t addr = reinterpret_cast<uint32_t>(this_);
    const uint32_t p1   = g_dev_p1.load(std::memory_order_acquire);
    const uint32_t p2   = g_dev_p2.load(std::memory_order_acquire);

    constexpr uint64_t kFirstLogPerClass = 5;
    constexpr uint64_t kHeartbeatPeriod  = 1800;  // ~30s @ 60Hz

    if (addr == p1) {
        const uint64_t n = g_poll_fires_p1.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= kFirstLogPerClass || (n % kHeartbeatPeriod) == 0) {
            mtr::log::info("[cm_dev/poll] this=dev_p1 (0x%08X) fire#%llu",
                           addr, static_cast<unsigned long long>(n));
        }
    } else if (p2 != 0 && addr == p2) {
        const uint64_t n = g_poll_fires_p2.fetch_add(
            1, std::memory_order_relaxed) + 1;
        // ALWAYS log fires_p2 — this is the RED FLAG case. Heartbeat
        // suppression kicks in only after the gap is unambiguously real.
        if (n <= 20 || (n % 600) == 0) {
            mtr::log::info("[cm_dev/poll] ** RED FLAG ** this=dev_p2 "
                           "(0x%08X) fire#%llu — zero-state shim being "
                           "overwritten; consider adding NOP-when-dev_p2 "
                           "gate.", addr, static_cast<unsigned long long>(n));
        }
    } else {
        const uint64_t n = g_poll_fires_other.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= kFirstLogPerClass || (n % kHeartbeatPeriod) == 0) {
            mtr::log::info("[cm_dev/poll] this=other (0x%08X) fire#%llu "
                           "(neither dev_p1=0x%08X nor dev_p2=0x%08X)",
                           addr, static_cast<unsigned long long>(n), p1, p2);
        }
    }

    return g_orig_poll(this_, edx_dummy);
}

// === Install the poll-fn probe ============================================
//
// Called from try_capture after dev_p2 is allocated. The prologue is
// verified against the expected bytes — if mismatch (e.g. someone else
// already hooked this VA), we log and skip rather than corrupting the
// engine. MinHook's 5-byte JMP fits in the first 5 bytes of clean
// prologue (push ebx; push esi; mov esi, ecx — 4 bytes — with the next
// instruction's first byte hitting at offset 5 inside `mov eax, [esi+0xD0]`).
void install_poll_probe() {
    static bool installed = false;
    if (installed) return;

    // Prologue sanity check. Bytes 0..7 should be `53 56 8B F1 8B 86 D0 00`
    // (push ebx; push esi; mov esi, ecx; mov eax, [esi+0xD0]). The first
    // dword is 0xF18B5653 little-endian.
    const uint32_t prologue0 = seh_read_dword(kPollFnVA);
    const uint32_t prologue1 = seh_read_dword(kPollFnVA + 4);
    constexpr uint32_t kExpectedPrologue0 = 0xF18B5653u;
    constexpr uint32_t kExpectedPrologue1 = 0x00D0868Bu;
    if (prologue0 != kExpectedPrologue0 || prologue1 != kExpectedPrologue1) {
        mtr::log::info("[cm_dev/poll] probe SKIPPED: prologue mismatch at "
                       "0x%08X (got 0x%08X 0x%08X, want 0x%08X 0x%08X)",
                       kPollFnVA, prologue0, prologue1,
                       kExpectedPrologue0, kExpectedPrologue1);
        return;
    }

    void* target = reinterpret_cast<void*>(kPollFnVA);
    void* tramp  = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_poll), &tramp)
            != MH_OK) {
        mtr::log::info("[cm_dev/poll] probe install FAILED: MH_CreateHook on "
                       "poll-fn @ 0x%08X", kPollFnVA);
        return;
    }
    g_orig_poll = reinterpret_cast<PFN_Poll>(tramp);

    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("[cm_dev/poll] probe install FAILED: MH_EnableHook on "
                       "poll-fn @ 0x%08X", kPollFnVA);
        return;
    }

    installed = true;
    mtr::log::info("[cm_dev/poll] probe installed @ 0x%08X. Will classify "
                   "fires by this value vs dev_p1=0x%08X / dev_p2=0x%08X.",
                   kPollFnVA,
                   g_dev_p1.load(std::memory_order_acquire),
                   g_dev_p2.load(std::memory_order_acquire));
}

} // anonymous namespace

bool install() {
    static bool installed = false;
    if (installed) return true;

    const uint32_t tick_va = seh_read_dword(kCmVtableVA + kSlotTick * 4);
    if (!tick_va) {
        mtr::log::info("[cm_dev] install FAILED: vtable[%d] @ 0x%08X reads 0",
                       kSlotTick, kCmVtableVA + kSlotTick * 4);
        return false;
    }

    void* target = reinterpret_cast<void*>(tick_va);
    void* tramp  = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_tick), &tramp)
            != MH_OK) {
        mtr::log::info("[cm_dev] install FAILED: MH_CreateHook on engine Tick "
                       "@ 0x%08X (ControlMapper vtable[3])", tick_va);
        return false;
    }
    g_orig_tick = reinterpret_cast<PFN_Tick>(tramp);

    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("[cm_dev] install FAILED: MH_EnableHook on engine Tick "
                       "@ 0x%08X", tick_va);
        return false;
    }

    installed = true;
    mtr::log::info("[cm_dev] installed. PRE-hook on engine Tick "
                   "(ControlMapper vtable[3]) @ 0x%08X. Will capture "
                   "instance + dev_p1 on first invocation, then idle.",
                   tick_va);
    return true;
}

uint32_t instance_addr() {
    return g_instance.load(std::memory_order_acquire);
}

uint32_t dev_p1_addr() {
    return g_dev_p1.load(std::memory_order_acquire);
}

uint32_t dev_p2_addr() {
    return g_dev_p2.load(std::memory_order_acquire);
}

void swap_to_player(int idx) {
    const uint32_t inst = g_instance.load(std::memory_order_acquire);
    if (!inst) return;  // capture hasn't happened yet — single-player default

    uint32_t target = 0;
    if (idx == 0) {
        target = g_dev_p1.load(std::memory_order_acquire);
    } else if (idx == 1) {
        target = g_dev_p2.load(std::memory_order_acquire);
        if (!target) return;  // dev_p2 alloc failed at capture; can't swap
    } else {
        return;  // unknown idx
    }
    if (!target) return;

    // Single sim-thread invariant: no lock. SEH-guarded because instance+4
    // is heap memory we don't own — paranoia against the engine relocating
    // its singleton (it doesn't, but the guard is cheap).
    seh_write_dword(inst + kDevPtrOffset, target);
}

PollProbeStats poll_probe_stats() {
    PollProbeStats s;
    s.fires_p1    = g_poll_fires_p1   .load(std::memory_order_relaxed);
    s.fires_p2    = g_poll_fires_p2   .load(std::memory_order_relaxed);
    s.fires_other = g_poll_fires_other.load(std::memory_order_relaxed);
    return s;
}

void write_p2_state(const P2InputState& s) {
    const uint32_t dp2 = g_dev_p2.load(std::memory_order_acquire);
    if (!dp2) return;  // capture / alloc failed; nothing to write into

    auto* p2 = reinterpret_cast<uint8_t*>(dp2);

    // Buttons: 18 (prev, curr) pairs at dev+0x0C..0x2F. The engine's prev/curr
    // cycle (shift curr -> prev BEFORE writing new curr) is done by the
    // poll-and-write fn for dev_p1 — for dev_p2, we replicate it here so
    // CM::vt[5] (WasButtonJustPressed) returns correct edge-detection results.
    __try {
        for (int i = 0; i < 18; ++i) {
            uint8_t* prev_byte = p2 + 0x0C + 2 * i;
            uint8_t* curr_byte = p2 + 0x0D + 2 * i;
            *prev_byte = *curr_byte;  // shift curr -> prev
            *curr_byte = s.buttons[i];
        }
        // Analog axes: 4 floats at dev+0x34..0x43. The engine's poll-fn at
        // sub_572340 writes exactly these four offsets (dev+52, +56, +60, +64
        // = +0x34, +0x38, +0x3C, +0x40 per the IDA decompile). CM::vt[4]
        // (GetAnalog) reads `*(float*)(dev + 48 + 4*v4)` where v4 is the
        // mapping-table value at CM+12+4*a2; the in-game mapping uses v4
        // values 1..4, mapping to dev+0x34..+0x43. dev+0x30 (v4=0) is never
        // read in normal gameplay and is left at zero by the zero-state
        // shim. We mirror the engine's write set exactly so the wire format
        // is symmetric with sub_572340's view of the device.
        for (int i = 0; i < 4; ++i) {
            *reinterpret_cast<float*>(p2 + 0x34 + 4 * i) = s.analogs[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // dev_p2 is our heap allocation, so a fault here would mean it was
        // freed/corrupted upstream. Defensive only — should never trigger.
    }
}

} // namespace mtr::coop::controlmapper_dev
