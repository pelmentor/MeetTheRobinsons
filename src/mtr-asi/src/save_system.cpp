// Direct save-system driver — see include/mtr/save_system.h for rationale.
//
// Engine internals (RE'd 2026-05-10):
//
//   unk_72F824 (0x0072F824)
//     Static array. unk_72F824[0] holds a pointer to a heap-allocated save
//     buffer. unk_72F824[1] (= dword_72F828, the SecuROM-resolved vtable)
//     holds engine entry points for save I/O. Many other static fields
//     hold operation flags / result codes:
//       static[170] — request opcode (cleared after operation)
//       static[172] — done flag (LOBYTE — pump exits when set non-zero)
//       static[196] — last result code (0 = success)
//       static[218] — gates the autosave-conflict popup loop in
//                     sub_575090 (set 0 to skip it)
//       static[247] — set 1 to skip the success popup at end of operation
//       static[324] BYTE1 — gates the mid-load popup in sub_575090
//                            (set != 1 to skip)
//       static[321] — operation-in-progress flag set by the engine
//
//   v1 = unk_72F824[0]  (the heap buffer)
//     v1[170] — request opcode, READ by sub_575D60's switch dispatcher
//                (this is DIFFERENT from static[170] despite sharing the
//                index — both must be set for a load)
//     v1[190] — slot index for the operation
//     v1[253] — count of valid slots (engine populates during enumerate)
//
//   sub_575D60 (0x00575D60)
//     Per-operation pump thread entry. Engine itself spawns it via
//     CreateThread. We can call it directly from our worker thread.
//     Pseudocode:
//       v1 = unk_72F824[0];
//       enumerate slots (writes v1[253], etc.);
//       loop:
//         dispatch(v1[170]);   // case 5 = sub_575090 (load)
//         Sleep(1ms);
//       while (!LOBYTE(unk_72F824[172]));
//
//   sub_575090 (0x00575090, case-5 LOAD handler)
//     Reads slot index from static[190] (NOT v1[190] — careful, both
//     coexist). Reads the file via dword_72F828[0]+vtable_offset
//     functions. Sets static[196] (result) when done.

#include "mtr/save_system.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::save_system {

namespace {

// === Engine VAs ============================================================

constexpr uintptr_t kSaveStateBaseVA = 0x0072F824;  // unk_72F824
constexpr uintptr_t kSavePumpVA      = 0x00575D60;  // sub_575D60

// State field indices (DWORDS into unk_72F824[]). Mapped from sub_575090
// + sub_575D60 decompilation; see file-header comment.
constexpr int kFieldDynBufPtr        = 0;    // [0]   = heap save buffer ptr
constexpr int kFieldRequestOp        = 170;  // [170] = LOAD/SAVE opcode
constexpr int kFieldDoneFlag         = 172;  // [172] = LOBYTE: done if !=0
constexpr int kFieldSlotIdx          = 190;  // [190] = slot index
constexpr int kFieldResultCode       = 196;  // [196] = 0 = success
constexpr int kFieldAutosavePopup    = 218;  // [218] = 0 to skip popup loop
constexpr int kFieldSkipSuccessPopup = 247;  // [247] = 1 for headless
constexpr int kFieldOpInProgress     = 321;  // [321] = engine-set flag
constexpr int kFieldMidLoadPopup     = 324;  // [324] BYTE1: != 1 to skip

constexpr uint32_t kOpcodeLoad = 5;

// === State accessors =======================================================

inline uint32_t* static_field(int idx) {
    return reinterpret_cast<uint32_t*>(kSaveStateBaseVA + idx * 4);
}
inline uint8_t* static_byte(int idx, int byte_off) {
    return reinterpret_cast<uint8_t*>(kSaveStateBaseVA + idx * 4 + byte_off);
}
inline uint8_t* dyn_buffer() {
    return *reinterpret_cast<uint8_t**>(kSaveStateBaseVA);  // unk_72F824[0]
}
inline uint32_t* dyn_field(uint8_t* buf, int idx) {
    return reinterpret_cast<uint32_t*>(buf + idx * 4);
}

// === Pump invocation =======================================================

// sub_575D60 is __usercall(EDI). The engine's CreateThread call site
// passes ebx (which is 0/null) as lpParameter, so EDI = 0 on entry. We
// match that. The pump's a1@<edi> propagates as the third arg to
// sub_575090's vtable[120] call — passing 0 mirrors the engine's
// invocation.
DWORD WINAPI pump_thread(LPVOID /*arg*/) {
    __try {
        __asm {
            xor edi, edi
            mov eax, kSavePumpVA
            call eax
            // sub_575D60 returns 0 in EAX; ignore.
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("save_system: pump thread faulted (SEH caught) — "
                       "engine state likely inconsistent or pump pre-conditions"
                       " not met");
    }
    return 0;
}

// === In-progress guard =====================================================

std::atomic<bool> g_load_in_progress{false};

}  // namespace

bool load_in_progress() {
    return g_load_in_progress.load(std::memory_order_relaxed);
}

bool load_slot(int slot_idx) {
    if (g_load_in_progress.exchange(true, std::memory_order_acq_rel)) {
        mtr::log::info("save_system: load_slot(%d) refused — load already "
                       "in progress", slot_idx);
        return false;
    }

    // Ensure the dynamic buffer has been allocated by the engine. If
    // unk_72F824[0] is null, the save subsystem hasn't been initialized
    // (very early boot, before WinMain finishes setup). Bail rather than
    // dereference null.
    uint8_t* buf = dyn_buffer();
    if (!buf) {
        mtr::log::info("save_system: load_slot(%d) refused — dynamic buffer "
                       "unk_72F824[0] is null (engine save subsystem not yet "
                       "initialized)", slot_idx);
        g_load_in_progress.store(false, std::memory_order_release);
        return false;
    }

    mtr::log::info("save_system: load_slot(%d) — dyn_buf=%p, "
                   "writing state and spawning pump", slot_idx, buf);

    // === Set state ===
    // Both static and dynamic [190] (slot index) — the load handler reads
    // the static one but the pump's enumerator writes to v1[190] in its
    // pre-load slot scan. Set both to avoid surprise.
    *static_field(kFieldSlotIdx)         = static_cast<uint32_t>(slot_idx);
    *dyn_field(buf, kFieldSlotIdx)       = static_cast<uint32_t>(slot_idx);
    // Skip popups.
    *static_byte(kFieldAutosavePopup, 0)    = 0;
    *static_byte(kFieldSkipSuccessPopup, 0) = 1;
    *static_byte(kFieldMidLoadPopup, 1)     = 0;
    // Clear result + done flag from any previous operation.
    *static_field(kFieldResultCode)         = 0;
    *static_byte(kFieldDoneFlag, 0)         = 0;
    // Set the LOAD opcode in BOTH locations. The pump's switch reads
    // v1[170] (dynamic); other code paths read static[170].
    *dyn_field(buf, kFieldRequestOp)        = kOpcodeLoad;
    *static_field(kFieldRequestOp)          = kOpcodeLoad;

    // === Spawn the pump ===
    HANDLE t = CreateThread(nullptr, 0, pump_thread, nullptr, 0, nullptr);
    if (!t) {
        mtr::log::info("save_system: load_slot(%d) — CreateThread failed "
                       "GLE=%lu", slot_idx, GetLastError());
        g_load_in_progress.store(false, std::memory_order_release);
        return false;
    }

    // === Wait for done ===
    constexpr DWORD kTimeoutMs   = 10000;
    constexpr DWORD kPollSleepMs = 50;
    const DWORD start = GetTickCount();
    bool timed_out = true;
    while (GetTickCount() - start < kTimeoutMs) {
        if (*static_byte(kFieldDoneFlag, 0) != 0) {
            timed_out = false;
            break;
        }
        Sleep(kPollSleepMs);
    }

    // Give the thread a chance to exit cleanly before we close the handle.
    DWORD wait_result = WaitForSingleObject(t, 2000);
    if (wait_result == WAIT_TIMEOUT) {
        mtr::log::info("save_system: load_slot(%d) — pump thread didn't exit "
                       "within 2s after done flag (or timeout); leaking "
                       "thread handle for safety", slot_idx);
    }
    CloseHandle(t);

    const uint32_t result = *static_field(kFieldResultCode);
    const bool success = !timed_out && (result == 0);

    mtr::log::info("save_system: load_slot(%d) finished: timed_out=%d "
                   "result_code=%u success=%d",
                   slot_idx, timed_out ? 1 : 0, result, success ? 1 : 0);

    g_load_in_progress.store(false, std::memory_order_release);
    return success;
}

}  // namespace mtr::save_system
