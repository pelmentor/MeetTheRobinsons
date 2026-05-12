// Coop spawn probe — Phase 0C derisk experiment. SHIPPED + GREEN (2026-05-11).
//
// Full archaeology / step-by-step trail:
//   research/findings/coop-phase-0b-breadcrumb-trail-2026-05-10.md
//
// Latest-state checkpoint (read FIRST after /compact):
//   memory/project_state_2026-05-11_coop_phase_0c_step2k_complete.md
//
// === Current state (Phase 0C COMPLETE) =====================================
//
// What this code does: from a UI button (Insert → Debug → Coop spawn probe),
// builds the engine's exact wilbur bag (`model_name=avatars/wilbur_low` +
// `class=wilbur`, captured at runtime via step-2g KV walker), calls
// `entity_factory_construct` (0x5B96F0), observes all 9 breadcrumb hooks
// firing along the construction ladder, then tears down the spawned orphan
// via `vtable[0](entity, 1)` (the MSVC scalar deleting destructor — same
// pattern the factory's own fail-paths use). Engine remains stable.
//
// Verified end-to-end by autonomous test loop:
//   pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy
//   → result=pass elapsed_ms=9109 frames=1870 exit=0
//
// === Hooks installed permanently at install() ==============================
//
// All five are PRE-only no-mutation diagnostic loggers. No side effects on
// engine normal-flow paths. Counters cap per-session to avoid log flood.
//
//   sub_5B1E10  actor_init        (step-2c)  global STEP2C log
//   sub_5A04F0  registry_lookup   (step-2e)  global STEP2E log
//   sub_5B96F0  entity_factory_construct (step-2f) global STEP2F PRE+POST log
//   sub_4B95A0  bag_merge_into    (step-2g)  factory-RA-filtered STEP2G KV dump
//   (VEH)       crash logger      (step-2j)  AddVectoredExceptionHandler,
//                                            one-shot, fatal CPU exceptions
//                                            only, logs EIP+regs+ESP[0..15]
//
// Step-2g KV walker fires only when bag_merge is called from inside the
// factory body (RA == 0x005B977D, the 5-byte CALL site at 0x5B9778).
//
// === Probe-scoped hooks (g_observing-gated) ================================
//
// Installed/uninstalled around try_spawn_p2's factory call. Some mutate
// (validate1 force-pass for diagnostic when needed; not needed now since
// the correct bag produces a native validate1=1). All scoped via the
// g_observing atomic.
//
//   bc[0]   0x5A04F0  class_registry_lookup_by_name  (extra probe sentinel)
//   bc[1/2] 0x5B71C0  protagonist_ctor (vtable[+4])  PRE/POST
//   bc[3]   0x4B95A0  bag_merge_into                 (extra probe sentinel)
//   bc[3.5/3.6] 0x55AD20  validate1                  PRE/POST + result
//   bc[3.7/3.8] 0x5B20F0  transform_setup            PRE/POST + result
//   bc[3.9/3.95] 0x5B1E10  actor_init                (probe sentinel mode)
//   bc[3.91] 0x5BBD10  sub_5BBD10                    NULL-arg short-circuit
//   bc[4a]  0x5AD410  scene register active          PRE
//   bc[4b]  0x5AD3E0  scene register queued          PRE
//   bc[5]   0x55AF00  post-init                      PRE (__cdecl)
//
// === Load-bearing facts (preserved from earlier RE) ========================
//
// 1. Factory signature: `entity_factory_construct(bag*, init_pos_vec3*,
//    init_rot_radians)`. a2 is a 3-float pos vector; NULL crashes sub_5B20F0.
//
// 2. Bag descriptor: single `void**`, head pointer at offset +0.
//    bag_init_from_template + factory both take the same address.
//
// 3. bag_init_from_template parser is single-KV. Use single-KV
//    `bag_init_from_template(&bag, "class=X")`, then chain
//    `bag_set_kv_THUNK(&bag, key, val)` for additional keys.
//
// 4. sub_55AF00 (post-init) is `__cdecl(entity, prop_value)` — caller
//    cleanup of 8 bytes. NOT __thiscall. ECX is NOT preset by either of
//    the two engine call sites (entity_factory_construct@0x5B9807 and
//    sub_542DB0@0x54309E). Probe was mis-typing this before step-2i —
//    see `PFN_PostInit` definition below for the corrected signature.
//
// 5. Wilbur entity vtable @ 0x6CC9A8. vtable[0] is the MSVC scalar
//    deleting destructor; called as `vtable[0](entity, 1)` to also free
//    memory. Used by step-2k teardown AND by the factory's own fail-paths.
//
// 6. The engine's "natural" wilbur creation goes through the SAME factory
//    (step-2f STEP2F log captured 4 calls during boot+load-save-1; call
//    #1 was wilbur from an .sx script at RA=0x020FD13B — JIT trampoline
//    in user-allocated memory). Our probe replicates that exact bag and
//    succeeds the same way the engine does.
//
// === Phase 0C known limitations (= Phase 2 work) ===========================
//
// Keeping the orphan ALIVE persistently (instead of immediate teardown)
// causes a downstream crash inside sub_5CB160 (future/promise resolver)
// during the next scene tick. Step-2j VEH captured:
//   sub_5AD4D0 (scene tick) → vtable[1] → sub_5AD9B0 walks (this+833)
//   list with vtable[13] dispatch → sub_55D8F0 vtable[17] → unanalyzed
//   fn at 0x5454xx → sub_5CB160(this=NULL).
// Faulting entity ≠ our orphan — our orphan disturbed shared state.
// Resolving this is required for Phase 2 (input routing) but NOT for
// Phase 0C closure.
//
// === Step archaeology (full detail in findings doc) ========================
//
// 2a (2026-05-10):  sub_5BBD10 NULL-arg bypass — disproven as crash root cause
// 2c (2026-05-10):  actor_init promoted to permanent global logger
// 2d (2026-05-10):  stack-walk on actor_init PRE — engine path divergence
// 2e (2026-05-10):  registry_lookup permanent — confirmed class=wilbur (not protagonist)
// 2f (2026-05-11):  factory permanent PRE+POST — engine wilbur call succeeded
// 2g (2026-05-11):  bag_merge permanent + RA-filtered KV walker — captured
//                   engine wilbur bag = 2 KVs only
// 2h (2026-05-11):  probe replicated engine bag — got 8 breadcrumbs deep
// 2i (2026-05-11):  sub_55AF00 hook re-typed __fastcall→__cdecl — factory
//                   returns success, try_spawn_p2 returned true
// 2j (2026-05-11):  VEH installed, captured post-probe scene-tick crash
// 2k (2026-05-11):  vtable[0](entity, 1) teardown — engine stable, test passes

#include "mtr/coop_spawn_probe.h"
#include "mtr/coop_registry_mirror.h"
#include "mtr/coop/engine_player.h"
#include "mtr/coop/remote_player_manager.h"
#include "mtr/coop/remote_player.h"
#include "mtr/coop/net/net_session.h"
#include "mtr/interp.h"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>     // _ReturnAddress

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }
namespace mtr::crash_handler {
    bool resolve_symbol(uintptr_t addr, char* out, size_t out_size);
}
namespace mtr::screen_push {
    bool current_top_name(char* out, size_t out_size);
}

namespace mtr::coop_spawn_probe {

namespace {

// === Engine VAs ============================================================

constexpr uintptr_t kBagInitFromTemplateVA = 0x004B9750;
constexpr uintptr_t kBagSetKVVA            = 0x004B93D0;
constexpr uintptr_t kEntityFactoryVA       = 0x005B96F0;
// `sub_55AF00` (0x0055AF00) is a 6-byte stolen-byte thunk: `JMP [0x00F8DED0]`.
// The IAT slot at 0x00F8DED0 is populated by SecuROM's startup decompressor
// (runs before WinMain) with the runtime address of the real function — at
// 0x00FAD7A0 in the post-unpack image. MinHook's prologue rewrite of the
// 6-byte thunk crashes the process at boot for reasons not fully understood
// (trampoline appears correct on paper; suspected SUEF-bypassing engine
// panic handler eats the actual exception). We hook the real function
// directly via the IAT slot at install time. See triage notes in
// research/findings/coop-spawn-probe-thunk-hook-fix-2026-05-10.md.
constexpr uintptr_t kPostInitThunkIATSlotVA = 0x00F8DED0;
constexpr uintptr_t kTransformListHeadVA    = 0x00724DE4;

// Phase 0B breadcrumb-hook VAs (see coop_spawn_probe.h ProbeResult notes).
// All __thiscall(this, one_stack_arg). retn 4 confirmed via disasm.
constexpr uintptr_t kProtagonistCtorVA = 0x005B71C0;  // protagonist vtable[+4]
constexpr uintptr_t kBagMergeIntoVA    = 0x004B95A0;  // bag_merge_into
constexpr uintptr_t kRegisterActiveVA  = 0x005AD410;  // active scene path
constexpr uintptr_t kRegisterQueuedVA  = 0x005AD3E0;  // queued/orphan path
// __cdecl(class_name_str, registry_head). Hooked with __cdecl signature
// directly. Fires after the factory's first entity_property_get_thunk call
// returns the "class" string from the bag — its presence in the breadcrumb
// log proves the bag accessor worked.
constexpr uintptr_t kRegistryLookupVA  = 0x005A04F0;  // class_registry_lookup_by_name

// First post-bag-merge validate. PRE proves we reached the validate step.
// POST capture of the return value tells us if the validate passed (1) or
// failed (0). __cdecl(entity_ptr).
constexpr uintptr_t kValidate1VA       = 0x0055AD20;  // sub_55AD20

// Transform setup. __thiscall(entity, pos_vec3, rot_radians). Writes
// position to entity+0x58 and 9-float rot matrix to entity+0x70+. Then
// calls vtable[13](entity) — a virtual post-transform init.
constexpr uintptr_t kTransformSetupVA  = 0x005B20F0;  // sub_5B20F0

// Actor post-init. Called as the FIRST step inside vtable[10] = sub_5B7010
// (which is sub_5B7010 wrapping sub_5B1E10 + a 152-byte alloc trail). Phase
// 0B's breadcrumb signature shows transform_setup POST fires but neither
// register_active/queued nor post_init fires, which puts the fault between
// transform_setup return and the next ladder rung. That gap is exactly
// the sub_5B7010 dispatch — so PRE/POST of sub_5B1E10 disambiguates: PRE=1
// POST=1 → fault is in sub_5B7010's tail (alloc + sub_5794D0 + sub_5B6E80
// + sub_579BF0); PRE=1 POST=0 → fault inside sub_5B1E10 itself, and we'll
// need to instrument deeper next iteration.
//
// __thiscall(this) — single arg via ECX, no stack args. Returns char (always
// 1 in the compiled body unless faulted). Express as __fastcall(this, edx_dummy).
constexpr uintptr_t kActorInitVA       = 0x005B1E10;  // sub_5B1E10

// First fault site inside sub_5B1E10 (Phase 0C-step-2a, 2026-05-10).
// Decompile shows the very first dereference is *(unsigned __int16 **)(a1 + 164),
// which faults at 0xA4 if a1 = NULL. Our standalone factory call passes NULL
// because the primary bag chain (entity+0x1EC) is normally seeded by the .sx
// class-load pipeline that we bypass. Hook short-circuits to return 0 when arg
// is NULL — the function returns 0 at three other places via its own null-
// branches, so 0 is a known-safe early-out value. Behavior matches the
// "v1 == 0" early return at the top of the function.
//
// __cdecl(a1). Returns _DWORD* (void*).
constexpr uintptr_t kBBD10VA           = 0x005BBD10;  // sub_5BBD10

// Transform-node offsets (mirror npc_overlay.cpp).
constexpr uintptr_t kNodeNextOffset       = 0x04;
constexpr uintptr_t kNodeFlagsOffset      = 0x44;
constexpr uintptr_t kNodeEntityOffset     = 0x5C;
constexpr uint8_t   kNodeFlagsSkipBit     = 0x10;

constexpr int kMaxIterations = 8192;

// === Engine call signatures ================================================

// __thiscall(this=&bag_head_slot, template_str). Callee cleans up stack.
// Express via __fastcall (ECX-this, EDX-dummy, stack args).
using PFN_BagInit = void (__fastcall*)(void* this_, void* edx_dummy,
                                       const char* template_str);

// __cdecl(bag_descriptor, init_pos_vec3_ptr, init_rot_radians).
// Caller cleans up.
//
// CORRECTION 2026-05-10: a4 is NOT "default_misc" (the original Phase 0A
// audit guessed this). It's actually a pointer to a 3-float position
// vector consumed by sub_5B20F0 (transform setup at factory step 9):
//   *(entity+0x58) = a2[0]; *(entity+0x5C) = a2[1]; *(entity+0x60) = a2[2];
// Passing NULL crashes (verified breadcrumb test 9).
//
// a5 is the initial rotation angle in radians (sin/cos used to fill the
// 3x3 rot matrix at entity+0x70).
using PFN_Factory = void* (__cdecl*)(void* bag_descriptor,
                                     const float* init_pos_vec3,
                                     float init_rot_radians);

// __cdecl(entity_ptr, prop_value). Caller cleans up.
//
// CONVENTION CORRECTION 2026-05-11 (Phase 0C-step-2i): originally typed as
// __thiscall(this, v13), but the disassembly at both engine call sites
// (entity_factory_construct@0x5B9808 and sub_542DB0@0x54309F) shows:
//   push  eax           ; prop_value (returned by entity_property_get_thunk)
//   push  esi           ; entity ptr
//   call  sub_55AF00
//   add   esp, 8        ; CALLER cleanup of 2 args = __cdecl
// Neither caller sets ECX before the call. The IDA decompile pseudo-`this`
// is a phantom — the real convention is __cdecl. The probe's previous
// __fastcall hook signature was:
//   - misreading random/stale ECX as "this" (= the "this=NULL" log was just
//     a stale register value, NOT a structural issue),
//   - and worse, forwarding only ONE stack arg back through the trampoline,
//     so the real function saw [esp+8] = garbage instead of prop_value,
//     which could itself be the crash trigger inside the real fn body.
using PFN_PostInit = int (__cdecl*)(void* entity, int prop_value);

// __thiscall(this=bag_handle, key, value). Both are interned-string ptrs
// (the runtime fn calls string_intern_hash on them). Callee cleans up the
// 2 stack args.
using PFN_BagSetKV = int (__fastcall*)(void* this_, void* edx_dummy,
                                       const char* key, const char* value);

// __thiscall(this=registry_entry, bag_descriptor). retn 4. Per-class ctor
// for protagonist (vtable[+4]). this is the registry entry; bag is the
// caller-provided template bag. The ctor body sub_5B6F40 ignores both —
// it just allocates 3276 bytes and returns the alloc'd ptr. SEH frame at
// SEH_5B7010 inside the wrapper catches inner faults.
using PFN_CtorWrap = void* (__fastcall*)(void* this_, void* edx_dummy, void* bag);

// __thiscall(this=dst_bag, src_bag). retn 4.
using PFN_BagMerge = void* (__fastcall*)(void* this_, void* edx_dummy, void* src);

// __thiscall(this=entity_manager, entity). retn 4.
using PFN_Register = void* (__fastcall*)(void* this_, void* edx_dummy, void* entity);

// __cdecl(class_name, registry_head). Caller cleans up. Returns void* entry.
using PFN_RegistryLookup = void* (__cdecl*)(const char* class_name, void* head);

// __cdecl(entity_ptr). Returns bool/char. Caller cleans up. The factory's
// first post-bag-merge validation; failing here destroys the entity and
// returns NULL from the factory.
using PFN_Validate1 = char (__cdecl*)(void* entity);

// __thiscall(entity, pos_vec3, rot_radians). Returns int (vtable[13] result).
using PFN_TransformSetup = int (__fastcall*)(void* this_, void* edx_dummy,
                                             const float* pos, float rot);

// __thiscall(entity). Returns char. No stack args.
using PFN_ActorInit = char (__fastcall*)(void* this_, void* edx_dummy);

// __cdecl(a1). Returns _DWORD* (void*).
using PFN_BBD10 = void* (__cdecl*)(void* a1);

// === Phase 2 step (b7.2) orphan keep-alive fix =============================
//
// Per-wilbur registry calls into the engine. Used by attach_engine_cm_to_orphan.
// See research/findings/coop-phase0-input-separation-point-2026-05-11.md
// section "(b7.1-next4)".

// sub_5CB310 — registry slot lookup. __thiscall(registry, key_str, unused).
// Returns the slot pointer (or 0).
using PFN_RegLookup2 = uint32_t (__fastcall*)(void* this_, void* edx,
                                              const char* key, int unused);

// sub_5CB420 — registry slot insert. __thiscall(registry, value, key, type,
// flag1, flag2). Returns the new (or existing) slot pointer.
using PFN_RegInsert = uint32_t (__fastcall*)(void* this_, void* edx,
                                              uint32_t value, const char* key,
                                              uint32_t type,
                                              uint32_t flag1, uint32_t flag2);

// sub_5CB220 — slot storage writer. __thiscall(slot, cm_ptr). Writes
// *(slot+12) = cm_ptr when slot[16]==5 (type=resource).
using PFN_SlotWrite = uint32_t (__fastcall*)(void* this_, void* edx, uint32_t value);

constexpr uintptr_t k5CB310VA            = 0x005CB310;
constexpr uintptr_t k5CB420VA            = 0x005CB420;
constexpr uintptr_t k5CB220VA            = 0x005CB220;
constexpr uintptr_t kControlMapperNameVA = 0x006A6EE0;
// engine_wilbur access is now via mtr::coop::engine_player::engine_wilbur_ptr()
// — the VA 0x00728A40 moved to coop/engine_player.cpp's anon namespace
// (Phase 1.2 audit 2026-05-12, architectural principle #7: engine-layout
// knowledge confined to the engine-wrapper module).

// === Phase 0B breadcrumb hook state ========================================
//
// Single g_observing flag scopes ALL hooks to the try_spawn_p2 window. The
// engine fires every one of these hooks for normal entity constructions
// during gameplay (NPC spawns, prop instantiations, etc.) — the flag
// suppresses the breadcrumb writes to keep the observation slot pristine
// for our deliberate spawn attempt.

PFN_PostInit       g_orig_post_init       = nullptr;
PFN_CtorWrap       g_orig_ctor_wrap       = nullptr;
PFN_BagMerge       g_orig_bag_merge       = nullptr;
PFN_Register       g_orig_register_active = nullptr;
PFN_Register       g_orig_register_queued = nullptr;
PFN_RegistryLookup g_orig_registry_lookup = nullptr;
PFN_Validate1      g_orig_validate1       = nullptr;
PFN_TransformSetup g_orig_transform_setup = nullptr;
PFN_ActorInit      g_orig_actor_init      = nullptr;
PFN_BBD10          g_orig_bbd10           = nullptr;
PFN_Factory        g_orig_factory_construct = nullptr;

std::atomic<bool> g_observing{false};

// Phase 0C-step-2c (2026-05-10): global actor_init logger. Default ON; flips
// off either via set_actor_init_global_log(false) or implicitly once the
// counter reaches kGlobalLogMaxLines (the hook stops emitting lines but the
// flag is still read — toggling false also resets the counter via the
// public setter).
std::atomic<bool> g_global_actor_init_log{true};
std::atomic<int>  g_global_actor_init_log_count{0};
constexpr int     kGlobalLogMaxLines = 1024;

// Phase 0C-step-2e (2026-05-10): global registry_lookup logger. Captures
// every class-name lookup at the engine factory entry point. Default ON;
// rate-limited to kGlobalLogMaxLines lines. Goal: see when (and if) the
// engine looks up "protagonist" during boot + save-load — disambiguates
// "factory IS used for protagonist creation" (= bag descriptor it receives
// must contain seed data for entity+0x1EC) vs "factory bypassed for
// protagonist" (= save-load deserializer or other path is doing it).
std::atomic<bool> g_global_registry_log{true};
std::atomic<int>  g_global_registry_log_count{0};

// Phase 0C-step-2f (2026-05-11): global entity_factory_construct PRE+POST
// logger. Captures every factory call's bag descriptor + return value +
// caller RA, so we can see which calls return NULL (= factory failed at
// validate1 etc.) vs non-NULL (= factory succeeded). Combined with
// step-2e's class log, this tells us whether the wilbur factory call
// returns NULL (= validate1 fail, engine creates wilbur via separate
// path) OR non-NULL (= factory succeeded, +0x1EC seeded post-return by
// caller).
std::atomic<bool> g_global_factory_log{true};
std::atomic<int>  g_global_factory_log_count{0};

// Phase 0C-step-2g (2026-05-11): global bag-KV dumper. When bag_merge_into
// is invoked from inside entity_factory_construct (RA == 0x005B977D, the
// 5-byte-CALL site at 0x5B9778), walk the SRC bag's linked KV list and
// log every (key, value) string pair. Bag layout per bag_merge_into
// decompile: src is a `void**`; *src is the head node; each node has
// next at [0], key char* at [1], value char* at [2]. Goal: read out the
// engine's full wilbur bag KV set so we can replicate it in our probe.
//
// Rate-limited per-factory-call (one dump per factory invocation).
// bag_merge_into has only 4 total callsites; only the factory-internal
// one fires this dump.
constexpr uintptr_t kFactoryMergeRA   = 0x005B977D;   // 0x5B9778 + 5
constexpr int       kMaxKVsPerDump    = 96;
constexpr int       kMaxBagDumps      = 16;          // ample for 4 factory calls
constexpr int       kMaxStrLen        = 96;
std::atomic<int>    g_global_bag_dump_count{0};

// Captured (this, v13) from the most recent in-window fire. Read by
// try_spawn_p2 after the factory returns.
std::atomic<void*> g_observed_this{nullptr};
std::atomic<void*> g_observed_v13{nullptr};
std::atomic<bool>  g_observed_fired{false};

// Phase 0C-step-2j (2026-05-11): vectored exception handler state.
// Step-2i confirmed try_spawn_p2 returns true (factory succeeds), but the
// process then crashes 0xC0000005 ~150ms later during the engine's
// sim/render integration of the orphan wilbur. VEH catches the first
// fatal exception so we get the faulting EIP + registers + ESP[0..15]
// in the log before the process dies. One-shot via g_veh_fired.
//
// Why VEH (vs SEH installed at hooks): VEH fires for ANY thread-level
// fault before SEH unwinds; works when the crash is deep in engine code
// the mod doesn't __try-wrap. FirstHandler=1 = called BEFORE any frame
// SEH handler. Returns EXCEPTION_CONTINUE_SEARCH so the process still
// dies normally — we just get diagnostics before it does.
//
// Filtering: only fatal CPU exceptions are logged (AV, illegal-insn,
// priv-insn, int/0, stack overflow). C++ exceptions (0xE06D7363) and
// debugger breaks are ignored.
// (b7.5) Changed from one-shot bool to N-shot counter. Original one-shot
// behavior was masking the real fatal AV: probes (read_dword_seh,
// read_writer_wilbur_seh) intentionally deref possibly-invalid memory under
// __try/__except. Those reads throw AVs that SEH catches harmlessly, but
// VEH fires *before* SEH unwinds — so the first SEH-caught AV burned the
// one-shot and the actual fatal AV later in the same run produced no VEH
// log line. With N-shot logging the LAST line in the log is the fatal one
// (after the fatal AV nothing else writes to the log).
//
// Cap kept finite to avoid unbounded log volume if the engine starts
// throwing AVs in a tight loop.
constexpr int           kVehLogCap = 32;
std::atomic<int>        g_veh_fire_count{0};
PVOID                   g_veh_handle = nullptr;

// Phase 0B breadcrumb sentinels.
std::atomic<bool>  g_bc_registry_pre{false};      // class_registry_lookup_by_name entered
std::atomic<void*> g_bc_registry_class_arg{nullptr};  // class string passed to lookup
std::atomic<bool>  g_bc_ctor_pre{false};
std::atomic<bool>  g_bc_ctor_post{false};
std::atomic<void*> g_bc_ctor_returned_ptr{nullptr};
std::atomic<bool>  g_bc_merge_pre{false};
std::atomic<bool>  g_bc_validate1_pre{false};
std::atomic<bool>  g_bc_validate1_post{false};
std::atomic<int>   g_bc_validate1_result{-1};
std::atomic<bool>  g_bc_transform_pre{false};
std::atomic<bool>  g_bc_transform_post{false};
std::atomic<bool>  g_bc_actor_init_pre{false};
std::atomic<bool>  g_bc_actor_init_post{false};
std::atomic<int>   g_bc_actor_init_result{-1};
std::atomic<bool>  g_bc_bbd10_pre{false};
std::atomic<void*> g_bc_bbd10_arg{nullptr};
std::atomic<bool>  g_bc_bbd10_null_bypass{false};
std::atomic<bool>  g_bc_register_active{false};
std::atomic<bool>  g_bc_register_queued{false};

// Phase 0C-step-2j (2026-05-11): vectored exception handler.
// One-shot: only the first fatal CPU exception is logged this session.
// Returns EXCEPTION_CONTINUE_SEARCH so the OS still handles the fault
// normally (= process still dies). We just get diagnostics first.
LONG WINAPI veh_crash_logger(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Skip non-fatal/C++/debugger exceptions.
    if (code != EXCEPTION_ACCESS_VIOLATION
        && code != EXCEPTION_ILLEGAL_INSTRUCTION
        && code != EXCEPTION_PRIV_INSTRUCTION
        && code != EXCEPTION_INT_DIVIDE_BY_ZERO
        && code != EXCEPTION_STACK_OVERFLOW
        && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // N-shot: log up to kVehLogCap fires. Past the cap, silently bail to
    // avoid runaway log volume. The LAST fire-line in the log is almost
    // always the actual fatal AV (anything that crashes harder than SEH
    // can catch terminates the process before another log line lands).
    int fire_idx = g_veh_fire_count.fetch_add(1, std::memory_order_acq_rel);
    if (fire_idx >= kVehLogCap) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT* ctx = info->ContextRecord;
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;

    // For AV, ExceptionInformation[0] = read(0)/write(1)/exec(8),
    // ExceptionInformation[1] = faulting address.
    unsigned av_op   = 0xFFFFFFFFu;
    unsigned av_addr = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        av_op   = static_cast<unsigned>(rec->ExceptionInformation[0]);
        av_addr = static_cast<unsigned>(rec->ExceptionInformation[1]);
    }

    // Try to symbolicate eip via crash_handler's dbghelp resolver (PDB-backed).
    // If the fault is inside our own .asi, this will tell us exactly which
    // function+line the engine returned into when it AV'd — saves a full
    // post-mortem WinDbg session for every new crash class.
    char sym_eip[640]    = {0};
    char sym_avaddr[640] = {0};
    mtr::crash_handler::resolve_symbol(static_cast<uintptr_t>(ctx->Eip),
                                       sym_eip, sizeof(sym_eip));
    if (av_addr) {
        mtr::crash_handler::resolve_symbol(static_cast<uintptr_t>(av_addr),
                                           sym_avaddr, sizeof(sym_avaddr));
    }
    mtr::log::info("coop_spawn_probe: STEP2J VEH FAULT #%d code=0x%08X eip=0x%08X"
                   " av_op=%u av_addr=0x%08X sym_eip=%s%s%s",
                   fire_idx,
                   static_cast<unsigned>(code),
                   static_cast<unsigned>(ctx->Eip),
                   av_op, av_addr,
                   sym_eip[0] ? sym_eip : "<unresolved>",
                   sym_avaddr[0] ? " sym_avaddr=" : "",
                   sym_avaddr[0] ? sym_avaddr   : "");
    mtr::log::info("coop_spawn_probe: STEP2J #%d regs eax=0x%08X ebx=0x%08X"
                   " ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X"
                   " ebp=0x%08X esp=0x%08X",
                   fire_idx,
                   static_cast<unsigned>(ctx->Eax),
                   static_cast<unsigned>(ctx->Ebx),
                   static_cast<unsigned>(ctx->Ecx),
                   static_cast<unsigned>(ctx->Edx),
                   static_cast<unsigned>(ctx->Esi),
                   static_cast<unsigned>(ctx->Edi),
                   static_cast<unsigned>(ctx->Ebp),
                   static_cast<unsigned>(ctx->Esp));

    // Dump ESP[0..15] (16 dwords = first 16 stack slots). Use SEH to
    // tolerate broken stack pointer.
    __try {
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(ctx->Esp);
        for (int i = 0; i < 16; i += 4) {
            mtr::log::info("coop_spawn_probe: STEP2J #%d stack[%02d..%02d]="
                           "%08X %08X %08X %08X",
                           fire_idx,
                           i, i + 3,
                           sp[i + 0], sp[i + 1], sp[i + 2], sp[i + 3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("coop_spawn_probe: STEP2J #%d stack[..] dump faulted",
                       fire_idx);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

int __cdecl hk_post_init(void* entity, int prop_value) {
    if (g_observing.load(std::memory_order_acquire)) {
        // Repurpose the existing slots: g_observed_this now stores entity,
        // g_observed_v13 stores prop_value. ProbeResult reads stay structurally
        // the same (we just renamed the meaning of the value); callers see
        // bc[5] fire correctly.
        g_observed_this.store(entity, std::memory_order_release);
        g_observed_v13.store(reinterpret_cast<void*>(
                                 static_cast<uintptr_t>(prop_value)),
                             std::memory_order_release);
        g_observed_fired.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[5] sub_55AF00 PRE (__cdecl):"
                       " entity=%p prop_value=0x%X",
                       entity, static_cast<unsigned>(prop_value));
    }
    return g_orig_post_init(entity, prop_value);
}

void* __fastcall hk_ctor_wrap(void* this_, void* /*edx*/, void* bag) {
    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        g_bc_ctor_pre.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[1] sub_5B71C0 PRE (protagonist ctor):"
                       " this=%p bag=%p", this_, bag);
    }
    void* result = g_orig_ctor_wrap(this_, nullptr, bag);
    if (obs) {
        g_bc_ctor_post.store(true, std::memory_order_release);
        g_bc_ctor_returned_ptr.store(result, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[2] sub_5B71C0 POST: returned=%p",
                       result);
    }
    return result;
}

// Phase 0C-step-2g (2026-05-11): copy up to max_len-1 chars from src into
// dst (NUL-terminated) under SEH. dst is always NUL-terminated on return,
// even on fault.
void copy_str_safe(const char* src, char* dst, int max_len) {
    if (max_len <= 0) return;
    int i = 0;
    __try {
        if (!src) { dst[0] = '\0'; return; }
        for (; i < max_len - 1 && src[i]; ++i) dst[i] = src[i];
        dst[i] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(dst, max_len, "<unreadable>");
    }
}

// Walk src bag's linked KV list and log each (key, value) pair. Bag layout
// (per bag_merge_into decompile at 0x4B95A0):
//   src is `void**`; *src is the head node; subsequent nodes via node[0].
//   each node:  [0] next  [1] key char*  [2] value char*  [3..] payload
// All deref under SEH.
void dump_factory_bag(void* src_bag, int dump_id) {
    int kv_index = 0;
    __try {
        auto** head_slot = reinterpret_cast<uintptr_t**>(src_bag);
        uintptr_t* node = *head_slot;
        while (node && kv_index < kMaxKVsPerDump) {
            const char* key = reinterpret_cast<const char*>(node[1]);
            const char* val = reinterpret_cast<const char*>(node[2]);
            char key_buf[kMaxStrLen];
            char val_buf[kMaxStrLen];
            copy_str_safe(key, key_buf, kMaxStrLen);
            copy_str_safe(val, val_buf, kMaxStrLen);
            mtr::log::info(
                "coop_spawn_probe: STEP2G dump#%d KV[%02d]:"
                " key=\"%s\" val=\"%s\"",
                dump_id, kv_index, key_buf, val_buf);
            node = reinterpret_cast<uintptr_t*>(node[0]);
            ++kv_index;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("coop_spawn_probe: STEP2G dump#%d faulted at KV[%d]",
                       dump_id, kv_index);
    }
    mtr::log::info("coop_spawn_probe: STEP2G dump#%d END: total %d KVs",
                   dump_id, kv_index);
}

void* __fastcall hk_bag_merge(void* this_, void* /*edx*/, void* src) {
    void* ra = _ReturnAddress();

    if (g_observing.load(std::memory_order_acquire)) {
        g_bc_merge_pre.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[3] sub_4B95A0 PRE (bag_merge_into):"
                       " dst=%p src=%p RA=%p", this_, src, ra);
    }

    // Phase 0C-step-2g: when called from entity_factory_construct's
    // internal merge site (RA == 0x5B977D, the 5-byte CALL at 0x5B9778),
    // dump the source bag's KV list. bag_merge_into has 4 total callsites
    // and only this one is inside the factory body — so this filter is
    // both precise (no false positives) and sufficient (catches every
    // factory call's bag).
    if (reinterpret_cast<uintptr_t>(ra) == kFactoryMergeRA) {
        const int n = g_global_bag_dump_count.fetch_add(
            1, std::memory_order_relaxed);
        if (n < kMaxBagDumps) {
            mtr::log::info("coop_spawn_probe: STEP2G factory bag dump #%d START:"
                           " dst=%p src=%p RA=%p", n + 1, this_, src, ra);
            dump_factory_bag(src, n + 1);
        } else if (n == kMaxBagDumps) {
            mtr::log::info("coop_spawn_probe: STEP2G factory bag dump:"
                           " rate limit (%d) reached — further dumps suppressed",
                           kMaxBagDumps);
        }
    }

    return g_orig_bag_merge(this_, nullptr, src);
}

void* __fastcall hk_register_active(void* this_, void* /*edx*/, void* entity) {
    if (g_observing.load(std::memory_order_acquire)) {
        g_bc_register_active.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[4a] sub_5AD410 PRE (active path):"
                       " mgr=%p entity=%p", this_, entity);
    }
    return g_orig_register_active(this_, nullptr, entity);
}

void* __fastcall hk_register_queued(void* this_, void* /*edx*/, void* entity) {
    if (g_observing.load(std::memory_order_acquire)) {
        g_bc_register_queued.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[4b] sub_5AD3E0 PRE (queued path):"
                       " mgr=%p entity=%p", this_, entity);
    }
    return g_orig_register_queued(this_, nullptr, entity);
}

int __fastcall hk_transform_setup(void* this_, void* /*edx*/,
                                  const float* pos, float rot) {
    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        g_bc_transform_pre.store(true, std::memory_order_release);
        float p0 = 0, p1 = 0, p2 = 0;
        __try {
            if (pos) { p0 = pos[0]; p1 = pos[1]; p2 = pos[2]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        mtr::log::info("coop_spawn_probe: bc[3.7] sub_5B20F0 PRE (transform_setup):"
                       " entity=%p pos=%p (%.2f,%.2f,%.2f) rot=%.4f",
                       this_, pos, p0, p1, p2, rot);
    }
    int result = g_orig_transform_setup(this_, nullptr, pos, rot);
    if (obs) {
        g_bc_transform_post.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[3.8] sub_5B20F0 POST: result=0x%X",
                       static_cast<unsigned>(result));
    }
    return result;
}

// Phase 0C-step-2f (2026-05-11): factory PRE+POST logger. Captures every
// engine call to entity_factory_construct, including its bag descriptor
// (we can't read the class name without resolving the SecuROM-thunked
// entity_property_get_thunk, but the timestamp lets us correlate with
// STEP2E lines for class identity) and its return value (NULL = factory
// failed at validate1 etc., non-NULL = success).
//
// Design: __cdecl detour. The factory's empirical ABI is __cdecl(bag,
// pos, rot) per the probe's successful invocation pattern. IDA's
// __usercall annotation (a1@ebp, a2@esi) appears to be a misanalysis —
// a1 is unused in the body and a2 is overwritten by InterlockedExchange
// before its first use. Standard __cdecl detour works.
void* __cdecl hk_factory_construct(void* bag, const float* pos, float rot) {
    void* ra = _ReturnAddress();
    const int n = g_global_factory_log_count.fetch_add(
        1, std::memory_order_relaxed);

    if (g_global_factory_log.load(std::memory_order_acquire) &&
        n < kGlobalLogMaxLines) {
        mtr::log::info("coop_spawn_probe: STEP2F factory PRE:"
                       " bag=%p pos=%p rot=%.4f RA=%p (call #%d)",
                       bag, pos, rot, ra, n + 1);
    }

    void* result = g_orig_factory_construct(bag, pos, rot);

    if (g_global_factory_log.load(std::memory_order_acquire) &&
        n < kGlobalLogMaxLines) {
        mtr::log::info("coop_spawn_probe: STEP2F factory POST:"
                       " return=%p (call #%d %s)",
                       result, n + 1,
                       result ? "SUCCESS" : "NULL — validate1 fail or earlier");
    } else if (g_global_factory_log.load(std::memory_order_acquire) &&
               n == kGlobalLogMaxLines) {
        mtr::log::info("coop_spawn_probe: STEP2F factory log:"
                       " rate limit (%d) reached — further calls suppressed",
                       kGlobalLogMaxLines);
    }

    return result;
}

// Phase 0C-step-2d (2026-05-10): stack-walk helper. Scans upward from the
// hook's saved-RA slot looking for dwords whose value falls inside the
// engine's .text section. Collects up to kStackWalkDepth such candidates
// and writes them into ras_out[]. Used by both the probe and global modes
// of hk_actor_init below — comparing the engine's path stack vs our
// standalone factory call's stack pinpoints the function that seeds
// entity+0x1EC in the engine path (the first divergence).
//
// The scan is range-based (not strict frame-pointer chain walk) because
// MSVC compiles much of the engine with /Oy (omit-frame-pointer), so
// the [ebp -> [ebp]+4 -> [[ebp]]+4 ...] chain is unreliable. Scanning
// the raw stack will produce a small number of false positives (vtable
// pointers etc.), but the consistent leading prefix across many calls
// will make the actual call chain obvious in the log.
constexpr int       kStackWalkDepth = 8;
constexpr int       kStackScanDwords = 96;   // ~384 bytes up the stack
// Wilbur.exe is loaded at 0x00400000; .text ends well before 0x00700000.
// Generous range to cover any calls from late-image addresses without
// admitting heap pointers (heap > 0x00800000 typically) or stack ptrs.
constexpr uintptr_t kTextRangeStart = 0x00401000;
constexpr uintptr_t kTextRangeEnd   = 0x00700000;

void capture_stack_chain(void* ras_out[kStackWalkDepth]) {
    for (int i = 0; i < kStackWalkDepth; ++i) ras_out[i] = nullptr;
    auto* stack = static_cast<uintptr_t*>(_AddressOfReturnAddress());
    int found = 0;
    __try {
        for (int i = 0; i < kStackScanDwords && found < kStackWalkDepth; ++i) {
            uintptr_t v = stack[i];
            if (v >= kTextRangeStart && v < kTextRangeEnd) {
                ras_out[found++] = reinterpret_cast<void*>(v);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // partial fill — leave remaining slots null
    }
}

char __fastcall hk_actor_init(void* this_, void* /*edx*/) {
    // Capture the stack chain BEFORE the trampoline call. capture_stack_chain
    // uses _AddressOfReturnAddress() so it walks the engine's actual call
    // stack at the moment sub_5B1E10 was entered.
    void* ras[kStackWalkDepth];
    capture_stack_chain(ras);

    // Peek at this+0x1EC (=*((DWORD*)this + 123)) — the bag chain head.
    // Read SEH-wrapped because the engine sometimes passes garbage here
    // during teardown / unusual states.
    uint32_t bag_chain_head = 0xDEADBEEF;
    __try {
        bag_chain_head = *reinterpret_cast<volatile uint32_t*>(
            reinterpret_cast<uint8_t*>(this_) + 0x1EC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bag_chain_head = 0xDEADBEEF;
    }

    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        // Probe-scoped breadcrumb path. Write the sentinel and log the
        // bc[3.9] line with the full stack so the probe's stack can be
        // diffed against the engine's normal-path stack.
        g_bc_actor_init_pre.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[3.9] sub_5B1E10 PRE (actor_init):"
                       " entity=%p entity+0x1EC=0x%08X"
                       " stack=[%p %p %p %p %p %p %p %p]",
                       this_, bag_chain_head,
                       ras[0], ras[1], ras[2], ras[3],
                       ras[4], ras[5], ras[6], ras[7]);
    } else if (g_global_actor_init_log.load(std::memory_order_acquire)) {
        // Phase 0C-step-2c+2d global mode. Log every engine actor_init call
        // until the cap, then suppress further lines (with one trailer).
        const int n = g_global_actor_init_log_count.fetch_add(
            1, std::memory_order_relaxed);
        if (n < kGlobalLogMaxLines) {
            mtr::log::info("coop_spawn_probe: STEP2D global sub_5B1E10:"
                           " this=%p +0x1EC=0x%08X"
                           " stack=[%p %p %p %p %p %p %p %p] (call #%d)",
                           this_, bag_chain_head,
                           ras[0], ras[1], ras[2], ras[3],
                           ras[4], ras[5], ras[6], ras[7],
                           n + 1);
        } else if (n == kGlobalLogMaxLines) {
            mtr::log::info("coop_spawn_probe: STEP2D global sub_5B1E10:"
                           " rate limit (%d) reached — further calls suppressed",
                           kGlobalLogMaxLines);
        }
    }

    char result = g_orig_actor_init(this_, nullptr);
    if (obs) {
        g_bc_actor_init_post.store(true, std::memory_order_release);
        const int r = static_cast<int>(static_cast<unsigned char>(result));
        g_bc_actor_init_result.store(r, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[3.95] sub_5B1E10 POST: result=%d", r);
    }
    return result;
}

void* __cdecl hk_bbd10(void* a1) {
    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        g_bc_bbd10_pre.store(true, std::memory_order_release);
        g_bc_bbd10_arg.store(a1, std::memory_order_release);
        // Phase 0C-step-2a bypass: sub_5BBD10 reads *(a1 + 164) without a
        // null-check at +0x1C. Returning 0 here matches one of the function's
        // own three "return 0" branches (top-of-fn !v1 path, mid-fn cleanup
        // paths). Probe-scoped only — engine paths see unhooked behavior.
        if (a1 == nullptr) {
            g_bc_bbd10_null_bypass.store(true, std::memory_order_release);
            mtr::log::info("coop_spawn_probe: bc[3.91] sub_5BBD10 PRE arg=NULL "
                           "— SHORT-CIRCUITING (return 0; bypasses *(NULL+164) "
                           "deref crash)");
            return nullptr;
        }
        mtr::log::info("coop_spawn_probe: bc[3.91] sub_5BBD10 PRE arg=%p "
                       "(non-NULL — calling original)", a1);
    }
    return g_orig_bbd10(a1);
}

char __cdecl hk_validate1(void* entity) {
    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        g_bc_validate1_pre.store(true, std::memory_order_release);

        // Per sub_55AD20 decompile, the function returns 0 for two reasons:
        //   A) all three sub-validations (sub_55AC90/unk_729748/sub_55AC40) false
        //   B) entity[+0x4C] & 4 is set (bit 2 = "skip validation" flag)
        // Capturing entity+0x4C disambiguates B vs A.
        uint32_t flags_4C = 0;
        uint32_t at_4 = 0;
        __try {
            flags_4C = *reinterpret_cast<volatile uint32_t*>(
                reinterpret_cast<uint8_t*>(entity) + 0x4C);
            at_4 = *reinterpret_cast<volatile uint32_t*>(
                reinterpret_cast<uint8_t*>(entity) + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            flags_4C = 0xDEADBEEF;
            at_4 = 0xDEADBEEF;
        }
        mtr::log::info("coop_spawn_probe: bc[3.5] sub_55AD20 PRE (validate1):"
                       " entity=%p entity+0x4C=0x%08X (bit2=%d) entity+4=0x%08X",
                       entity, flags_4C, (flags_4C & 4) ? 1 : 0, at_4);
    }
    char result = g_orig_validate1(entity);
    if (obs) {
        g_bc_validate1_post.store(true, std::memory_order_release);
        const int original = static_cast<int>(static_cast<unsigned char>(result));
        g_bc_validate1_result.store(original, std::memory_order_release);
        // DIAGNOSTIC BYPASS: validate1 returns 0 for headless protagonist
        // construction because the 3 sub-validators (sub_55AC90 / unk_729748
        // / sub_55AC40 — both SecuROM-thunked, can't statically RE) all
        // return false. The engine's level-load path passes them, so they're
        // checking some scene-state we haven't set up. Force-pass here to
        // keep the breadcrumb trail moving: see what fires next so we can
        // localize the next failure point. Probe-scoped (g_observing) so the
        // engine's normal entity construction sees the original behavior.
        if (original == 0) {
            mtr::log::info("coop_spawn_probe: bc[3.6] sub_55AD20 POST result=0 "
                           "(force-passing for probe diagnostic)");
            result = 1;
        } else {
            mtr::log::info("coop_spawn_probe: bc[3.6] sub_55AD20 POST result=%d",
                           original);
        }
    }
    return result;
}

void* __cdecl hk_registry_lookup(const char* class_name, void* head) {
    void* ra = _ReturnAddress();

    // Read class_name into a local buffer ONCE under SEH so both probe
    // and global modes can use it without re-deref'ing in each branch.
    char preview[64] = {0};
    __try {
        for (int i = 0; i < 63 && class_name && class_name[i]; ++i) {
            preview[i] = class_name[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(preview, sizeof(preview), "<unreadable>");
    }

    const bool obs = g_observing.load(std::memory_order_acquire);
    if (obs) {
        g_bc_registry_pre.store(true, std::memory_order_release);
        g_bc_registry_class_arg.store(const_cast<char*>(class_name),
                                      std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[0] sub_5A04F0 PRE (registry_lookup):"
                       " class_name=%p (\"%s\") head=%p RA=%p",
                       class_name, preview, head, ra);
    } else if (g_global_registry_log.load(std::memory_order_acquire)) {
        // Phase 0C-step-2e global mode. Log every engine factory call
        // until the cap. Capturing class name + caller RA lets us
        // identify save-load / level-load paths that construct
        // protagonist (= player class).
        const int n = g_global_registry_log_count.fetch_add(
            1, std::memory_order_relaxed);
        if (n < kGlobalLogMaxLines) {
            mtr::log::info("coop_spawn_probe: STEP2E global sub_5A04F0:"
                           " class=\"%s\" RA=%p (call #%d)",
                           preview, ra, n + 1);
        } else if (n == kGlobalLogMaxLines) {
            mtr::log::info("coop_spawn_probe: STEP2E global sub_5A04F0:"
                           " rate limit (%d) reached — further calls suppressed",
                           kGlobalLogMaxLines);
        }
    }
    return g_orig_registry_lookup(class_name, head);
}

// === Transform-list count ==================================================

int count_transform_list_unsafe() {
    auto* node = *reinterpret_cast<uint8_t**>(kTransformListHeadVA);
    int count = 0;
    int safety = kMaxIterations;
    while (node && safety-- > 0) {
        const uint8_t flags = *(node + kNodeFlagsOffset);
        uint8_t* next = *reinterpret_cast<uint8_t**>(node + kNodeNextOffset);
        if ((flags & kNodeFlagsSkipBit) == 0) {
            void* entity = *reinterpret_cast<void**>(node + kNodeEntityOffset);
            if (entity) ++count;
        }
        node = next;
    }
    return count;
}

int count_transform_list() {
    int n = -1;
    __try {
        n = count_transform_list_unsafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

// === Gate: gameplay state via entity_lookup("player",1) ===================
//
// Original gate was a screen-name blocklist (rejecting "MainMenu" etc.),
// but the engine's screen mirror keeps "WilburMainMenu" at the top of the
// stack EVEN AFTER gameplay activates (the post-load main menu doesn't
// pop, just becomes invisible while gameplay renders underneath — verified
// in autonomous run 2026-05-10 where player_entity went non-null while
// top stayed "WilburMainMenu"). Screen-name gating thus rejects valid
// gameplay state and was preventing the probe from running.
//
// New gate: query the entity manager directly. If
// entity_lookup_by_name_retry("player", 1) returns non-null, the engine
// has allocated the player entity, which only happens once a save is
// loaded and gameplay is active. This is the same signal the test_harness
// uses for its Phase D gate.

constexpr uintptr_t kEntityLookupByNameRetryVA = 0x005AC8F0;
constexpr uintptr_t kEntityManagerPtrVA        = 0x007425AC;

using PFN_EntityLookupRetry = void* (__thiscall*)(void* self, const char* name, int unused);

// === Probe state (single result; one-shot per session is the contract) ===

ProbeResult g_last{};
std::mutex  g_last_mu;

void set_last(const ProbeResult& r) {
    std::scoped_lock lk(g_last_mu);
    g_last = r;
}

} // namespace

// === Public API ===========================================================

bool is_in_gameplay() {
    void* mgr = *reinterpret_cast<void* volatile*>(kEntityManagerPtrVA);
    if (!mgr) return false;
    void* p = nullptr;
    __try {
        auto fn = reinterpret_cast<PFN_EntityLookupRetry>(kEntityLookupByNameRetryVA);
        p = fn(mgr, "player", 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p != nullptr;
}

// Resolve the runtime real-fn address from the IAT slot. The SecuROM
// startup decompressor populates this before WinMain runs, but the slot
// may differ between launches (we observed 0x00FAD7A0 in IDB, 0x010037C0
// at one runtime). Reads it fresh each call.
void* resolve_post_init_real_fn() {
    void* real_fn = nullptr;
    __try {
        real_fn = *reinterpret_cast<void**>(kPostInitThunkIATSlotVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return real_fn;
}

// Holds the address of every hook installed for the duration of one probe
// attempt. Filled by install_breadcrumb_hooks_for_probe(); drained by
// uninstall_breadcrumb_hooks(). Each entry's address is what we pass to
// MH_RemoveHook — keeping the original install address means the IAT-slot
// indirection (sub_55AF00 case) stays correct.
struct BreadcrumbHookSet {
    void* post_init        = nullptr;  // resolved real-fn (NOT the thunk VA)
    // registry_lookup (sub_5A04F0) is installed PERMANENTLY at install() —
    // see 0C-step-2e. Not part of the probe-scoped lifecycle. PRE-only,
    // no-mutation, so safe to keep installed across all engine fires.
    void* ctor_wrap        = nullptr;
    // bag_merge (sub_4B95A0) is installed PERMANENTLY at install() — see
    // 0C-step-2g. Not part of the probe-scoped lifecycle. Dual-mode (probe
    // sentinel + factory-RA-filtered KV dump), no mutation, safe permanent.
    void* validate1        = nullptr;
    void* transform_setup  = nullptr;
    // actor_init (sub_5B1E10) is installed PERMANENTLY at install() — see
    // 0C-step-2c notes there. Not part of the probe-scoped lifecycle.
    void* bbd10            = nullptr;
    void* register_active  = nullptr;
    void* register_queued  = nullptr;
};

bool create_and_enable(void* target, void* detour, void** orig, const char* tag) {
    if (MH_CreateHook(target, detour, orig) != MH_OK) {
        mtr::log::info("coop_spawn_probe: MH_CreateHook(%s @ %p) failed",
                       tag, target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        mtr::log::info("coop_spawn_probe: MH_EnableHook(%s @ %p) failed",
                       tag, target);
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

// Install all Phase 0B breadcrumb hooks PLUS the sub_55AF00 PRE-logger.
// Returns a populated BreadcrumbHookSet on success, all-null on failure.
// On partial failure, any hooks that DID install successfully are removed
// before returning, so failures leave a clean slate.
//
// As before, all hooks are installed RIGHT BEFORE the factory call and
// uninstalled RIGHT AFTER. Reasons:
//   - sub_55AF00 fires for every entity construction during boot/level-load;
//     hooking at boot crashed the engine silently.
//   - The other 4 breadcrumb hooks fire on routine engine paths too. Scoping
//     them to the probe window keeps the engine running normally outside
//     the observation slot.
BreadcrumbHookSet install_breadcrumb_hooks_for_probe() {
    BreadcrumbHookSet hs{};

    void* post_init_target = resolve_post_init_real_fn();
    if (!post_init_target) {
        mtr::log::info("coop_spawn_probe: cannot resolve sub_55AF00 real-fn "
                       "from IAT slot %p",
                       reinterpret_cast<void*>(kPostInitThunkIATSlotVA));
        return hs;
    }

    if (!create_and_enable(post_init_target, &hk_post_init,
                           reinterpret_cast<void**>(&g_orig_post_init),
                           "sub_55AF00")) {
        return hs;
    }
    hs.post_init = post_init_target;

    auto cleanup_on_fail = [&]() {
        if (hs.post_init)       { MH_DisableHook(hs.post_init);
                                  MH_RemoveHook(hs.post_init); }
        // registry_lookup is permanent — never uninstalled here.
        if (hs.ctor_wrap)       { MH_DisableHook(hs.ctor_wrap);
                                  MH_RemoveHook(hs.ctor_wrap); }
        // bag_merge is permanent — never uninstalled here.
        if (hs.validate1)       { MH_DisableHook(hs.validate1);
                                  MH_RemoveHook(hs.validate1); }
        if (hs.transform_setup) { MH_DisableHook(hs.transform_setup);
                                  MH_RemoveHook(hs.transform_setup); }
        // actor_init is permanent — never uninstalled here.
        if (hs.bbd10)           { MH_DisableHook(hs.bbd10);
                                  MH_RemoveHook(hs.bbd10); }
        if (hs.register_active) { MH_DisableHook(hs.register_active);
                                  MH_RemoveHook(hs.register_active); }
        if (hs.register_queued) { MH_DisableHook(hs.register_queued);
                                  MH_RemoveHook(hs.register_queued); }
        g_orig_post_init       = nullptr;
        // g_orig_registry_lookup kept — permanent hook stays valid across probes.
        g_orig_ctor_wrap       = nullptr;
        // g_orig_bag_merge kept — permanent hook stays valid across probes.
        g_orig_validate1       = nullptr;
        g_orig_transform_setup = nullptr;
        // g_orig_actor_init kept — permanent hook stays valid across probes.
        g_orig_bbd10           = nullptr;
        g_orig_register_active = nullptr;
        g_orig_register_queued = nullptr;
        hs = BreadcrumbHookSet{};
    };

    // registry_lookup (sub_5A04F0) was installed permanently in install().
    // Skip re-installing here — g_observing alone gates the probe-scoped
    // breadcrumb writes inside hk_registry_lookup.

    void* ctor_target = reinterpret_cast<void*>(kProtagonistCtorVA);
    if (!create_and_enable(ctor_target, &hk_ctor_wrap,
                           reinterpret_cast<void**>(&g_orig_ctor_wrap),
                           "sub_5B71C0 ctor")) {
        cleanup_on_fail();
        return hs;
    }
    hs.ctor_wrap = ctor_target;

    // bag_merge (sub_4B95A0) was installed permanently in install() —
    // see 0C-step-2g. Skip re-installing here; g_observing gates the
    // probe-scoped breadcrumb writes inside hk_bag_merge.

    void* validate1_target = reinterpret_cast<void*>(kValidate1VA);
    if (!create_and_enable(validate1_target, &hk_validate1,
                           reinterpret_cast<void**>(&g_orig_validate1),
                           "sub_55AD20 validate1")) {
        cleanup_on_fail();
        return hs;
    }
    hs.validate1 = validate1_target;

    void* transform_target = reinterpret_cast<void*>(kTransformSetupVA);
    if (!create_and_enable(transform_target, &hk_transform_setup,
                           reinterpret_cast<void**>(&g_orig_transform_setup),
                           "sub_5B20F0 transform_setup")) {
        cleanup_on_fail();
        return hs;
    }
    hs.transform_setup = transform_target;

    // actor_init (sub_5B1E10) was installed permanently in install(). Skip
    // re-installing here — g_observing alone gates the probe-scoped breadcrumb
    // writes inside hk_actor_init.

    void* bbd10_target = reinterpret_cast<void*>(kBBD10VA);
    if (!create_and_enable(bbd10_target, &hk_bbd10,
                           reinterpret_cast<void**>(&g_orig_bbd10),
                           "sub_5BBD10 NULL-arg bypass")) {
        cleanup_on_fail();
        return hs;
    }
    hs.bbd10 = bbd10_target;

    void* active_target = reinterpret_cast<void*>(kRegisterActiveVA);
    if (!create_and_enable(active_target, &hk_register_active,
                           reinterpret_cast<void**>(&g_orig_register_active),
                           "sub_5AD410 active")) {
        cleanup_on_fail();
        return hs;
    }
    hs.register_active = active_target;

    void* queued_target = reinterpret_cast<void*>(kRegisterQueuedVA);
    if (!create_and_enable(queued_target, &hk_register_queued,
                           reinterpret_cast<void**>(&g_orig_register_queued),
                           "sub_5AD3E0 queued")) {
        cleanup_on_fail();
        return hs;
    }
    hs.register_queued = queued_target;

    return hs;
}

void uninstall_breadcrumb_hooks(const BreadcrumbHookSet& hs) {
    if (hs.register_queued) { MH_DisableHook(hs.register_queued);
                              MH_RemoveHook(hs.register_queued); }
    if (hs.register_active) { MH_DisableHook(hs.register_active);
                              MH_RemoveHook(hs.register_active); }
    if (hs.bbd10)           { MH_DisableHook(hs.bbd10);
                              MH_RemoveHook(hs.bbd10); }
    // actor_init is permanent — kept across probes.
    if (hs.transform_setup) { MH_DisableHook(hs.transform_setup);
                              MH_RemoveHook(hs.transform_setup); }
    if (hs.validate1)       { MH_DisableHook(hs.validate1);
                              MH_RemoveHook(hs.validate1); }
    // bag_merge is permanent — kept across probes.
    if (hs.ctor_wrap)       { MH_DisableHook(hs.ctor_wrap);
                              MH_RemoveHook(hs.ctor_wrap); }
    // registry_lookup is permanent — kept across probes.
    if (hs.post_init)       { MH_DisableHook(hs.post_init);
                              MH_RemoveHook(hs.post_init); }
    g_orig_post_init       = nullptr;
    // g_orig_registry_lookup kept — permanent hook.
    g_orig_ctor_wrap       = nullptr;
    // g_orig_bag_merge kept — permanent hook.
    g_orig_validate1       = nullptr;
    g_orig_transform_setup = nullptr;
    // g_orig_actor_init kept — permanent hook.
    g_orig_bbd10           = nullptr;
    g_orig_register_active = nullptr;
    g_orig_register_queued = nullptr;
}

bool install() {
    // Phase 0C-step-2j (2026-05-11): install vectored exception handler FIRST
    // so any subsequent install-time fault (or post-install engine fault) is
    // captured. FirstHandler=1 means "first in the VEH chain" — fires before
    // anything else's VEH. One-shot via g_veh_fired so we only log the first
    // fatal exception per session.
    g_veh_handle = AddVectoredExceptionHandler(1, &veh_crash_logger);
    if (!g_veh_handle) {
        mtr::log::info("coop_spawn_probe: install: VEH install FAILED — crash"
                       " diagnostics unavailable (GLE=%lu)", GetLastError());
        // Not fatal — proceed with the rest of install().
    } else {
        mtr::log::info("coop_spawn_probe: install: VEH installed (one-shot,"
                       " fatal CPU exceptions only)");
    }

    // Phase 0C-step-2c (2026-05-10): install the actor_init (sub_5B1E10) hook
    // PERMANENTLY at boot. The hook serves dual purpose:
    //   (a) probe-scoped breadcrumb writes when g_observing is set (existing
    //       Phase 0C-step-1 behavior — used by try_spawn_p2);
    //   (b) global per-call logging when g_observing is false and
    //       g_global_actor_init_log is true (rate-limited to kGlobalLogMaxLines).
    //
    // Why permanent install is safe: the hook function is a thin pass-through
    // — it reads entity+0x1EC under SEH and calls the trampoline unchanged.
    // No mutation of state, no side effects on the engine's normal path.
    // Other breadcrumb hooks (post_init / registry_lookup / ctor / merge /
    // validate1 / transform_setup / bbd10 / register_a / register_q) remain
    // probe-scoped because some mutate (validate1 force-pass) or fire so
    // frequently that boot-time logs would flood. actor_init is moderate-
    // frequency and has no mutation.
    void* actor_init_target = reinterpret_cast<void*>(kActorInitVA);
    if (!create_and_enable(actor_init_target, &hk_actor_init,
                           reinterpret_cast<void**>(&g_orig_actor_init),
                           "sub_5B1E10 actor_init (permanent for step-2c)")) {
        mtr::log::info("coop_spawn_probe: install: actor_init permanent hook "
                       "FAILED — global log will not fire");
        return false;
    }

    // Phase 0C-step-2e (2026-05-10): also install registry_lookup permanently.
    // hk_registry_lookup is a PRE-only no-mutation hook (just logs), so
    // permanent install is safe — same rationale as actor_init.
    void* registry_target = reinterpret_cast<void*>(kRegistryLookupVA);
    if (!create_and_enable(registry_target, &hk_registry_lookup,
                           reinterpret_cast<void**>(&g_orig_registry_lookup),
                           "sub_5A04F0 registry_lookup (permanent for step-2e)")) {
        mtr::log::info("coop_spawn_probe: install: registry_lookup permanent hook"
                       " FAILED — global registry log will not fire");
        // Not fatal — actor_init still installed, we still have step-2d data.
    }

    // Phase 0C-step-2f (2026-05-11): install entity_factory_construct PRE+POST
    // permanently. PRE+POST log every call's bag/pos/rot/RA + return value.
    // Together with step-2e's class log, this lets us correlate factory
    // calls with class names and see which classes return NULL (= factory
    // failed) vs non-NULL (= factory succeeded). For the engine's wilbur
    // call specifically, this answers "does the factory return NULL for
    // wilbur?" — the answer determines next steps.
    void* factory_target = reinterpret_cast<void*>(kEntityFactoryVA);
    if (!create_and_enable(factory_target, &hk_factory_construct,
                           reinterpret_cast<void**>(&g_orig_factory_construct),
                           "sub_5B96F0 factory (permanent for step-2f)")) {
        mtr::log::info("coop_spawn_probe: install: factory permanent hook"
                       " FAILED — global factory log will not fire");
        // Not fatal — fallback to inferring from registry+actor_init logs.
    }

    // Phase 0C-step-2g (2026-05-11): install bag_merge_into PRE permanently.
    // When called from inside entity_factory_construct (RA == 0x5B977D),
    // dump the SRC bag's full KV linked list. Reveals the engine's wilbur
    // bag KV set so the probe can replicate it. Dual-mode: probe-scoped
    // breadcrumb sentinel (g_observing) + factory-RA-filtered global dump.
    // No mutation, safe permanent install.
    void* merge_target = reinterpret_cast<void*>(kBagMergeIntoVA);
    if (!create_and_enable(merge_target, &hk_bag_merge,
                           reinterpret_cast<void**>(&g_orig_bag_merge),
                           "sub_4B95A0 bag_merge (permanent for step-2g)")) {
        mtr::log::info("coop_spawn_probe: install: bag_merge permanent hook"
                       " FAILED — global bag-dump will not fire");
        // Not fatal — step-2f data alone still narrows the search.
    }

    mtr::log::info("coop_spawn_probe: install: actor_init permanent hook ready"
                   " (global log %s, max %d lines)."
                   " registry_lookup permanent hook %s."
                   " factory permanent hook %s."
                   " bag_merge permanent hook %s.",
                   g_global_actor_init_log.load() ? "ON" : "OFF",
                   kGlobalLogMaxLines,
                   g_orig_registry_lookup ? "ready" : "not installed",
                   g_orig_factory_construct ? "ready" : "not installed",
                   g_orig_bag_merge ? "ready" : "not installed");
    return true;
}

void set_actor_init_global_log(bool enabled) {
    g_global_actor_init_log_count.store(0, std::memory_order_release);
    g_global_actor_init_log.store(enabled, std::memory_order_release);
    mtr::log::info("coop_spawn_probe: actor_init global log %s "
                   "(counter reset; cap=%d)",
                   enabled ? "ENABLED" : "disabled", kGlobalLogMaxLines);
}

int actor_init_global_log_count() {
    return g_global_actor_init_log_count.load(std::memory_order_acquire);
}

// Phase 2 step (b7.2) — orphan keep-alive fix.
//
// The orphan from try_spawn_p2() lacks the per-wilbur registry entries that
// would normally be populated by the engine's CM-attach factory at 0x51F4D0
// (unreachable as a normal call target — it's runtime-thunked). On the next
// sim tick, ViewDriver.vtable[13] @ 0x5454B0 calls
// sub_5CB310(orphan+0xCCC, "ControlMapper", 0) → returns NULL → sub_5CB160(0)
// → AV at 0x5CB163.
//
// Fix: read the engine wilbur's ControlMapper out of its +0xCCC slot, then
// insert the SAME CM into the orphan's +0xCCC. ViewDriver gets a non-NULL
// slot, AV gone. Routing per player (P1 vs P2 input) is handled separately
// via b2-rem-2 component thunks — those don't depend on global CM tracking.
//
// SEH-wrapped end-to-end. Returns true if the slot insert succeeded and
// readback showed a non-zero slot pointer. False on any fault.
bool attach_engine_cm_to_orphan(void* orphan) {
    if (!orphan) {
        mtr::log::info("coop_spawn_probe: attach_engine_cm: NULL orphan, skipping");
        return false;
    }

    __try {
        // 1. Read engine wilbur from the player-controller manager array.
        uint32_t engine_wilbur = reinterpret_cast<uint32_t>(
            mtr::coop::engine_player::engine_wilbur_ptr());
        if (engine_wilbur == 0) {
            mtr::log::info("coop_spawn_probe: attach_engine_cm: manager[0]=NULL"
                           " — engine wilbur not yet installed?");
            return false;
        }

        // 2. Look up engine's ControlMapper slot.
        auto fn_lookup = reinterpret_cast<PFN_RegLookup2>(k5CB310VA);
        const char* cm_name = reinterpret_cast<const char*>(kControlMapperNameVA);
        void* engine_registry = reinterpret_cast<void*>(engine_wilbur + 0xCCC);
        uint32_t engine_slot = fn_lookup(engine_registry, nullptr, cm_name, 0);
        if (engine_slot == 0) {
            mtr::log::info("coop_spawn_probe: attach_engine_cm: engine wilbur"
                           " 0x%08X has no ControlMapper slot —"
                           " engine init not yet complete?",
                           engine_wilbur);
            return false;
        }
        // (b7.6) Two derefs, not one. slot+0x0C holds the ADDRESS of the
        // storage cell; the storage cell in turn holds the CM instance
        // pointer. sub_5CB160's resolution chain is `*(*(slot+0xC))` — and
        // sub_5CB220 writes its value INTO `*(slot+0xC)` (i.e. into the
        // cell, not as a replacement for the cell pointer). The previous
        // single-deref version wrote the engine's storage_cell_address
        // into the orphan's cell, so the engine's downstream
        // `cm_inst = *storage` read that address back and dereferenced it
        // as a CM instance — finding only the next CM's first few fields,
        // including uninitialized slot data interpreted as `vtable[4] = 0x2`.
        // See research/findings/coop-phase0-input-separation-point-2026-05-11.md
        // section "(b7.6) Off-by-one-deref root cause".
        uint32_t engine_storage_addr =
            *reinterpret_cast<uint32_t*>(engine_slot + 0x0C);
        if (engine_storage_addr == 0) {
            mtr::log::info("coop_spawn_probe: attach_engine_cm: engine slot"
                           " 0x%08X.storage_addr is NULL", engine_slot);
            return false;
        }
        uint32_t engine_cm =
            *reinterpret_cast<uint32_t*>(engine_storage_addr);
        if (engine_cm == 0) {
            mtr::log::info("coop_spawn_probe: attach_engine_cm: engine"
                           " storage cell at 0x%08X holds NULL",
                           engine_storage_addr);
            return false;
        }

        // 3. Insert into orphan's registry (find-or-insert; engine's sub_5CB420
        //    early-returns if the key is already present).
        auto fn_insert = reinterpret_cast<PFN_RegInsert>(k5CB420VA);
        uint32_t orphan_addr = reinterpret_cast<uint32_t>(orphan);
        void* orphan_registry = reinterpret_cast<void*>(orphan_addr + 0xCCC);
        fn_insert(orphan_registry, nullptr,
                  orphan_addr,    // value = owner backref (per engine pattern)
                  cm_name,
                  5,              // type = resource
                  0, 0);

        // 4. Read the new slot back.
        uint32_t orphan_slot = fn_lookup(orphan_registry, nullptr, cm_name, 0);
        if (orphan_slot == 0) {
            mtr::log::info("coop_spawn_probe: attach_engine_cm: orphan slot"
                           " readback returned NULL after insert");
            return false;
        }

        // 5. Wire engine's CM into the orphan's slot storage cell.
        auto fn_write = reinterpret_cast<PFN_SlotWrite>(k5CB220VA);
        fn_write(reinterpret_cast<void*>(orphan_slot), nullptr, engine_cm);

        // 6. Verify: read the storage cell back.
        uint32_t storage_cell = *reinterpret_cast<uint32_t*>(orphan_slot + 0x0C);
        uint32_t storage_val  = storage_cell != 0
                                ? *reinterpret_cast<uint32_t*>(storage_cell)
                                : 0;

        mtr::log::info("coop_spawn_probe: attach_engine_cm SUCCESS:"
                       " engine_wilbur=0x%08X engine_storage=0x%08X engine_cm=0x%08X"
                       " orphan=0x%08X orphan_slot=0x%08X"
                       " orphan_storage=0x%08X *storage=0x%08X %s",
                       engine_wilbur, engine_storage_addr, engine_cm,
                       orphan_addr, orphan_slot,
                       storage_cell, storage_val,
                       storage_val == engine_cm ? "(wired)" : "(MISMATCH)");
        return storage_val == engine_cm;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("coop_spawn_probe: attach_engine_cm: EXCEPTION");
        return false;
    }
}

// Cmdline gate: -mtrasi-coop-keep-orphan enables the b7.2 keep-alive fix
// (CM attach + teardown skip). Off by default — existing behavior unchanged.
bool keep_orphan_enabled() {
    static const bool s_enabled = []{
        LPSTR cl = GetCommandLineA();
        if (!cl) return false;
        return std::strstr(cl, "-mtrasi-coop-keep-orphan") != nullptr;
    }();
    return s_enabled;
}

bool try_spawn_p2() {
    ProbeResult r{};
    r.attempted = true;

    // 1. Gate on screen state.
    if (!mtr::screen_push::current_top_name(r.screen_name, sizeof(r.screen_name))) {
        std::snprintf(r.message, sizeof(r.message),
                      "gate-fail: could not read current screen name");
        mtr::log::info("coop_spawn_probe: %s", r.message);
        set_last(r);
        return false;
    }
    if (!is_in_gameplay()) {
        std::snprintf(r.message, sizeof(r.message),
                      "gated: entity_lookup(player,1) returned null on screen"
                      " \"%s\" — engine not in gameplay state",
                      r.screen_name);
        mtr::log::info("coop_spawn_probe: %s", r.message);
        set_last(r);
        return false;
    }

    // 2. Snapshot transform list before.
    r.list_count_before = count_transform_list();
    if (r.list_count_before < 0) {
        std::snprintf(r.message, sizeof(r.message),
                      "gate-fail: pre-walk of dword_724DE4 faulted "
                      "(transform list not initialized?)");
        mtr::log::info("coop_spawn_probe: %s", r.message);
        set_last(r);
        return false;
    }

    mtr::log::info("coop_spawn_probe: ATTEMPTING factory call. "
                   "screen=\"%s\" pre_list_count=%d",
                   r.screen_name, r.list_count_before);

    // 3. Build bag + call factory inside SEH.
    //
    // Mirror sub_43D167's stack pattern. Disassembly trace (2026-05-10) of
    // the actual ESP-relative addresses shows:
    //
    //   bag_init's ECX = &var_10 = orig_esp - 0x10
    //   factory's stack arg = &var_18 = orig_esp - 0x14
    //
    // i.e. the factory's bag pointer is 4 bytes BELOW the bag_init target
    // in memory. So the descriptor layout is:
    //   offset +0 (slots[0]): factory-arg base
    //   offset +4 (slots[1]): head pointer (bag_init writes here)
    //
    // Tested both 4-byte and 8-byte spacing — both EXCEPTION at the same
    // point (no breadcrumbs hit). The layout is not the cause; running
    // with the engine-pattern 4-byte spacing for the registry-lookup probe.
    // Allocate 4 dwords for safety in case the engine touches slots[2..3].
    void* slots[4] = { nullptr, nullptr, nullptr, nullptr };
    // Engine pattern (sub_43D167): template is a SINGLE k=v entry, e.g.
    // "class=compActor". Multi-KV templates with ';' separators are NOT
    // a feature of this parser — the engine adds extra keys via separate
    // bag_set_kv calls. Mirror that pattern.
    //
    // Phase 0C-step-2e correction (2026-05-10): the engine's actual player
    // class is "wilbur" — NOT "protagonist" as the original Phase 0A audit
    // claimed.
    //
    // Phase 0C-step-2g correction (2026-05-11): the engine's wilbur bag
    // captured at the factory's bag_merge site is EXACTLY 2 KVs:
    //   model_name = avatars/wilbur_low
    //   class      = wilbur
    // NO `name=` entry. Our prior probe used `name=player2` as the second
    // KV, which is WRONG — the engine wilbur factory call does not have
    // a `name` key. The correct second KV is `model_name`, which tells
    // the engine which .dbl avatar model to load. Replicating the engine
    // bag exactly (= step-2h test).
    static const char kTemplate[] = "class=wilbur";
    static const char kModelKey[] = "model_name";
    static const char kModelVal[] = "avatars/wilbur_low";

    // Re-arm all observation slots + install the breadcrumb hook ladder for
    // the duration of this single probe attempt. The hooks are uninstalled
    // before this function returns so the engine's normal entity-construction
    // paths see the unhooked engine fns outside the probe window.
    g_observed_this.store(nullptr, std::memory_order_release);
    g_observed_v13.store(nullptr, std::memory_order_release);
    g_observed_fired.store(false, std::memory_order_release);
    g_bc_registry_pre.store(false, std::memory_order_release);
    g_bc_registry_class_arg.store(nullptr, std::memory_order_release);
    g_bc_ctor_pre.store(false, std::memory_order_release);
    g_bc_ctor_post.store(false, std::memory_order_release);
    g_bc_ctor_returned_ptr.store(nullptr, std::memory_order_release);
    g_bc_merge_pre.store(false, std::memory_order_release);
    g_bc_validate1_pre.store(false, std::memory_order_release);
    g_bc_validate1_post.store(false, std::memory_order_release);
    g_bc_validate1_result.store(-1, std::memory_order_release);
    g_bc_transform_pre.store(false, std::memory_order_release);
    g_bc_transform_post.store(false, std::memory_order_release);
    g_bc_actor_init_pre.store(false, std::memory_order_release);
    g_bc_actor_init_post.store(false, std::memory_order_release);
    g_bc_actor_init_result.store(-1, std::memory_order_release);
    g_bc_bbd10_pre.store(false, std::memory_order_release);
    g_bc_bbd10_arg.store(nullptr, std::memory_order_release);
    g_bc_bbd10_null_bypass.store(false, std::memory_order_release);
    g_bc_register_active.store(false, std::memory_order_release);
    g_bc_register_queued.store(false, std::memory_order_release);
    g_observing.store(true, std::memory_order_release);

    BreadcrumbHookSet hooks = install_breadcrumb_hooks_for_probe();
    if (!hooks.post_init) {
        g_observing.store(false, std::memory_order_release);
        std::snprintf(r.message, sizeof(r.message),
                      "gate-fail: could not install breadcrumb hook ladder "
                      "(see preceding log lines for which target failed)");
        mtr::log::info("coop_spawn_probe: %s", r.message);
        set_last(r);
        return false;
    }

    bool exception_during_call = false;
    void* entity = nullptr;

    __try {
        auto bag_init = reinterpret_cast<PFN_BagInit>(kBagInitFromTemplateVA);
        auto bag_set  = reinterpret_cast<PFN_BagSetKV>(kBagSetKVVA);

        // bag_merge_into (sub_4B95A0) decompile shows the bag handle has its
        // head pointer at offset +0 (line: `v3 = *a2`). So bag_init and
        // factory should both receive the SAME address (the bag handle).
        // Pass &slots[0] to both. After init, *(&slots[0]) = head ptr.
        bag_init(&slots[0], nullptr, kTemplate);
        bag_set(&slots[0], nullptr, kModelKey, kModelVal);

        r.slot0_after_init = slots[0];
        r.slot1_after_init = slots[1];
        r.slot2_after_init = slots[2];

        auto factory = reinterpret_cast<PFN_Factory>(kEntityFactoryVA);
        // a4 must be a valid 3-float vector ptr (sub_5B20F0 dereferences it
        // for the entity's initial position).
        //
        // Phase 1.4d (2026-05-12): when engine_wilbur exists, spawn the
        // orphan at the local player's current world position. The peer's
        // first pose snapshot replaces this value within ~17ms of arrival
        // (1 sim tick) via push_interp_snapshot_at; the initial position
        // matters only for the brief window between factory return and
        // the first interp blend. Origin {0,0,0} is often outside level
        // bounds and can trip the engine's AABB / navmesh asserts on the
        // first sim tick — using engine_wilbur's position keeps the
        // orphan safely inside the playable space until the network
        // overrides it. Principle-4 root-cause fix (audit P2,
        // 2026-05-12). Falls back to origin if engine_wilbur is not
        // populated yet (e.g. running the manual probe before player
        // exists — unchanged from prior behavior).
        float init_pos[3] = { 0.0f, 0.0f, 0.0f };
        if (void* w = mtr::coop::engine_player::engine_wilbur_ptr()) {
            mtr::interp::EntityPose wpose{};
            if (mtr::interp::capture_entity_pose(w, wpose)) {
                init_pos[0] = wpose.pos[0];
                init_pos[1] = wpose.pos[1];
                init_pos[2] = wpose.pos[2];
            }
        }
        entity = factory(&slots[0], init_pos, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exception_during_call = true;
    }

    g_observing.store(false, std::memory_order_release);
    uninstall_breadcrumb_hooks(hooks);

    r.entity              = entity;
    r.succeeded           = (entity != nullptr) && !exception_during_call;
    r.registry_pre        = g_bc_registry_pre.load(std::memory_order_acquire);
    r.registry_class_arg  = g_bc_registry_class_arg.load(std::memory_order_acquire);
    r.ctor_pre            = g_bc_ctor_pre.load(std::memory_order_acquire);
    r.ctor_post           = g_bc_ctor_post.load(std::memory_order_acquire);
    r.ctor_returned_ptr   = g_bc_ctor_returned_ptr.load(std::memory_order_acquire);
    r.merge_pre           = g_bc_merge_pre.load(std::memory_order_acquire);
    r.actor_init_pre      = g_bc_actor_init_pre.load(std::memory_order_acquire);
    r.actor_init_post     = g_bc_actor_init_post.load(std::memory_order_acquire);
    r.bbd10_pre              = g_bc_bbd10_pre.load(std::memory_order_acquire);
    r.bbd10_arg              = g_bc_bbd10_arg.load(std::memory_order_acquire);
    r.bbd10_null_bypass_fired= g_bc_bbd10_null_bypass.load(std::memory_order_acquire);
    r.register_active     = g_bc_register_active.load(std::memory_order_acquire);
    r.register_queued     = g_bc_register_queued.load(std::memory_order_acquire);
    r.post_init_reached   = g_observed_fired.load(std::memory_order_acquire);
    r.post_init_v13_arg   = g_observed_v13.load(std::memory_order_acquire);

    // 4. Snapshot transform list after.
    r.list_count_after = count_transform_list();
    if (r.list_count_after >= 0 && r.list_count_before >= 0) {
        r.list_delta = r.list_count_after - r.list_count_before;
    }

    // 5. Build a human-readable summary message.
    //
    // Breadcrumb signature is bc=[ctor_pre,ctor_post,merge_pre,reg_a,reg_q,post_init].
    // The LAST '1' in that sequence pinpoints the deepest fn we entered before
    // the fault. For a clean spawn we expect [1,1,1,1,0,1] (active path) or
    // [1,1,1,0,1,1] (queued path).
    const bool v1_pre  = g_bc_validate1_pre.load(std::memory_order_acquire);
    const bool v1_post = g_bc_validate1_post.load(std::memory_order_acquire);
    const int  v1_res  = g_bc_validate1_result.load(std::memory_order_acquire);

    const bool tx_pre  = g_bc_transform_pre.load(std::memory_order_acquire);
    const bool tx_post = g_bc_transform_post.load(std::memory_order_acquire);
    const int  ai_res  = g_bc_actor_init_result.load(std::memory_order_acquire);

    char bc[224];
    std::snprintf(bc, sizeof(bc),
                  "bc=[reg=%d,c_pre=%d,c_post=%d,merge=%d,v1_pre=%d,v1_post=%d,"
                  "v1_res=%d,tx_pre=%d,tx_post=%d,ai_pre=%d,ai_post=%d,ai_res=%d,"
                  "bbd10_pre=%d,bbd10_byp=%d,a=%d,q=%d,pi=%d]",
                  r.registry_pre ? 1 : 0,
                  r.ctor_pre ? 1 : 0,
                  r.ctor_post ? 1 : 0,
                  r.merge_pre ? 1 : 0,
                  v1_pre ? 1 : 0,
                  v1_post ? 1 : 0,
                  v1_res,
                  tx_pre ? 1 : 0,
                  tx_post ? 1 : 0,
                  r.actor_init_pre ? 1 : 0,
                  r.actor_init_post ? 1 : 0,
                  ai_res,
                  r.bbd10_pre ? 1 : 0,
                  r.bbd10_null_bypass_fired ? 1 : 0,
                  r.register_active ? 1 : 0,
                  r.register_queued ? 1 : 0,
                  r.post_init_reached ? 1 : 0);

    if (exception_during_call) {
        std::snprintf(r.message, sizeof(r.message),
                      "EXCEPTION during factory call. %s "
                      "ctor_ret=%p slots=[%p,%p,%p]",
                      bc, r.ctor_returned_ptr,
                      r.slot0_after_init, r.slot1_after_init, r.slot2_after_init);
    } else if (!entity) {
        std::snprintf(r.message, sizeof(r.message),
                      "factory returned NULL. %s "
                      "ctor_ret=%p slots=[%p,%p,%p]",
                      bc, r.ctor_returned_ptr,
                      r.slot0_after_init, r.slot1_after_init, r.slot2_after_init);
    } else {
        std::snprintf(r.message, sizeof(r.message),
                      "factory returned %p delta=%d (%s) %s v13=%p slots=[%p,%p,%p]",
                      entity, r.list_delta,
                      r.list_delta == 1 ? "ACTIVE PATH"
                                        : (r.list_delta == 0 ? "queued/orphan"
                                                              : "unexpected"),
                      bc, r.post_init_v13_arg,
                      r.slot0_after_init, r.slot1_after_init, r.slot2_after_init);
    }
    mtr::log::info("coop_spawn_probe: %s", r.message);

    // Phase 0C-step-2k (2026-05-11): tear down the spawned orphan immediately.
    //
    // Step-2j VEH capture showed the orphan disturbs scene state — the next
    // sim tick faults inside sub_5CB160 (future/promise resolver) when
    // processing a DIFFERENT entity (edi=0x0EFB0A20 ≠ our orphan
    // 0x0EE81A20). The crash is downstream of register_active() inserting
    // our partially-initialized entity into a shared structure.
    //
    // Solution: call the entity's virtual destructor (vtable[0], the MSVC
    // scalar deleting destructor) with the "free memory" flag (=1) — the
    // same call pattern entity_factory_construct uses in its own fail
    // paths after validate1 or actor_init returns false. The destructor's
    // body handles unregistration from scene/manager lists.
    //
    // Wrapped in SEH because an orphan dtor may itself fault on
    // uninitialized fields — but even a fault here is better than letting
    // the destabilized scene tick continue running.
    //
    // ProbeResult::succeeded is set BEFORE this teardown, so the proof-of-
    // life signal stays intact. After this returns, the orphan is gone and
    // the engine can resume normal operation. Future sessions where we
    // actually need a living orphan (input routing) will skip this teardown
    // via a flag.
    // Phase 2 step (b7.2): when -mtrasi-coop-keep-orphan is set, attach the
    // engine's ControlMapper to the orphan and SKIP the teardown so we can
    // observe the orphan surviving past the prior crash chain.
    //
    // Phase 1.4d (2026-05-12): when a co-op session is active, the orphan
    // MUST survive — it's the peer's avatar. Coupling session-active to
    // keep-alive at the call site (rather than adding a new global flag
    // or a session-implies-keep-orphan side channel) keeps RULE №2 clean:
    // the cmdline flag still works for the standalone diagnostic probe
    // case, but session-driven auto-spawn doesn't need a second flag.
    bool keep_alive = keep_orphan_enabled()
                      || mtr::coop::net::NetSession::instance().active();
    if (entity && !exception_during_call && keep_alive) {
        // b7.5 bracket logging: localize 0x5375EC03 crash class. Each marker
        // gets its own log line so when the VEH fires we can see exactly
        // which bracket we were between. Markers are stamped both before
        // and after the only real work (attach_engine_cm_to_orphan); the
        // post-marker telling us whether we ever returned from try_spawn_p2
        // at all in the keep-alive path.
        mtr::log::info("coop_spawn_probe: B7.5 marker A — pre-attach (entity=%p)",
                       entity);
        bool cm_ok = attach_engine_cm_to_orphan(entity);
        mtr::log::info("coop_spawn_probe: B7.5 marker B — post-attach (cm_ok=%s)",
                       cm_ok ? "OK" : "FAIL");

        // (b7.12, Tier-3 dump probe) If -mtrasi-coop-dump-registry is set,
        // read-only walk engine_wilbur's +0xCCC registry. No mutation;
        // validates the enumerate logic before any mutator lands in b7.13.
        if (mtr::coop_registry_mirror::dump_enabled()) {
            uint32_t engine_wilbur = reinterpret_cast<uint32_t>(
                mtr::coop::engine_player::engine_wilbur_ptr());
            if (engine_wilbur != 0) {
                auto st = mtr::coop_registry_mirror::dump_engine_registry(
                    engine_wilbur);
                mtr::log::info("coop_spawn_probe: B7.12 registry dump:"
                               " vec_size=%u dumped=%u names=%u faults=%u",
                               st.vector_size, st.slots_dumped,
                               st.names_resolved, st.read_faults);
            } else {
                mtr::log::info("coop_spawn_probe: B7.12 registry dump skipped"
                               " — engine_wilbur=NULL");
            }
        }

        // (b7.13, Tier-3 mirror) If -mtrasi-coop-mirror-registry is set,
        // iterate engine_wilbur's +0xCCC registry and replicate each key
        // onto the orphan with the engine's current storage value. This is
        // the proper fix — replaces the retired coop_orphan_filter as the
        // orphan-keep-alive mechanism (filter retired 2026-05-12 after
        // 3600-frame soak validation with and without filter showed mirror
        // + per-site routes are sufficient).
        if (mtr::coop_registry_mirror::mirror_enabled()) {
            uint32_t engine_wilbur = reinterpret_cast<uint32_t>(
                mtr::coop::engine_player::engine_wilbur_ptr());
            if (engine_wilbur != 0) {
                auto ms = mtr::coop_registry_mirror::mirror_engine_registry_to_orphan(
                    engine_wilbur, reinterpret_cast<uint32_t>(entity));
                mtr::log::info("coop_spawn_probe: B7.13 registry mirror:"
                               " seen=%u inserted=%u copied=%u faults=%u",
                               ms.engine_keys_seen, ms.inserts_succeeded,
                               ms.values_copied, ms.read_or_write_faults);

                // Post-mirror verification: dump the orphan's registry. If
                // mirror succeeded, this should show the same 21 keys as
                // engine_wilbur's dump.
                if (mtr::coop_registry_mirror::dump_enabled()) {
                    auto orphan_st =
                        mtr::coop_registry_mirror::dump_engine_registry(
                            reinterpret_cast<uint32_t>(entity));
                    mtr::log::info("coop_spawn_probe: B7.13 post-mirror"
                                   " ORPHAN dump: vec_size=%u dumped=%u"
                                   " names=%u faults=%u",
                                   orphan_st.vector_size, orphan_st.slots_dumped,
                                   orphan_st.names_resolved,
                                   orphan_st.read_faults);
                }
            } else {
                mtr::log::info("coop_spawn_probe: B7.13 registry mirror skipped"
                               " — engine_wilbur=NULL");
            }
        }
        mtr::log::info("coop_spawn_probe: B7.2 keep-orphan path:"
                       " attach_engine_cm_to_orphan=%s — SKIPPING teardown."
                       " Orphan will live; observe next ~60 frames for AV.",
                       cm_ok ? "OK" : "FAIL");

        // Phase 1 skeleton (2026-05-12): hand the live orphan to the
        // MtrPlayerManager. From this point onward the orphan is "owned"
        // by an MtrRemotePlayer wrapper (parallel-class hierarchy per
        // MTA's CClientPed ↔ CPlayerPed*). The wrapper is non-intrusive
        // — it stores ptr+id+role; no engine state is modified by the
        // registration itself. Future Phase 1 work (input buffer,
        // interp, network) plugs into the wrapper rather than into
        // this probe.
        {
            auto& mgr = mtr::coop::MtrPlayerManager::instance();
            auto* rp = mgr.register_remote(entity);
            mtr::log::info("coop_spawn_probe: phase1 manager registered:"
                           " mtr_remote_player id=%u (count=%zu)",
                           rp ? static_cast<unsigned>(rp->player_id()) : 255u,
                           mgr.count());
            // Also opportunistically wrap the engine_wilbur as the local
            // player. If engine_wilbur isn't populated yet (boot ordering),
            // .local() returns nullptr and we'll catch it on the next run.
            auto* local = mgr.local();
            mtr::log::info("coop_spawn_probe: phase1 manager local lookup:"
                           " %s (manager count=%zu)",
                           local ? "wrapped" : "not-yet-available",
                           mgr.count());
        }

        mtr::log::info("coop_spawn_probe: B7.5 marker C — about to fall through"
                       " to set_last+return");
    } else if (entity && !exception_during_call) {
        using PFN_EntityDtor = void (__fastcall*)(void* this_,
                                                  void* /*edx*/,
                                                  int free_flag);
        __try {
            void** vt = *reinterpret_cast<void***>(entity);
            auto dtor = reinterpret_cast<PFN_EntityDtor>(vt[0]);
            dtor(entity, nullptr, 1);
            mtr::log::info("coop_spawn_probe: STEP2K orphan teardown:"
                           " vtable[0](%p, 1) returned cleanly",
                           entity);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mtr::log::info("coop_spawn_probe: STEP2K orphan teardown:"
                           " EXCEPTION inside vtable[0](%p, 1) —"
                           " engine may still be unstable",
                           entity);
        }
    }

    set_last(r);
    return r.succeeded;
}

ProbeResult last_result() {
    std::scoped_lock lk(g_last_mu);
    return g_last;
}

} // namespace mtr::coop_spawn_probe
