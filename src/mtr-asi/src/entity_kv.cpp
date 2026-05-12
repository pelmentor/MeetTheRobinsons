// SEH-guarded wrapper around the engine's kv-bag accessor.
// See include/mtr/entity_kv.h for the contract.

#include "mtr/entity_kv.h"

#include <windows.h>
#include <cstdint>

namespace mtr::entity_kv {

namespace {

// Engine VA. Decompile shows it's a stolen-byte IAT thunk forwarding to
// `g_securom_thunk_table_base + 178618`. The forwarder itself is __thiscall:
//   ECX = entity ptr
//   stack arg 0 = const char* key
//   returns const char* value (or null)
// Calling convention via MSVC: pointer-to-member-function-ish, but the
// cleanest way to express it without a pmf is __fastcall with EDX as an
// unused dummy register (binary-compatible with __thiscall in MSVC x86).
constexpr uintptr_t kEntityKvGetVA = 0x004B8F00;

using PFN_KvGet = const char* (__fastcall*)(void* this_, void* edx_dummy,
                                            const char* key);

const char* call_kv_unsafe(void* entity, const char* key) {
    auto fn = reinterpret_cast<PFN_KvGet>(kEntityKvGetVA);
    return fn(entity, nullptr, key);
}

} // namespace

const char* get(void* entity, const char* key) {
    if (!entity || !key) return nullptr;
    const char* result = nullptr;
    __try {
        result = call_kv_unsafe(entity, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

bool has(void* entity, const char* key) {
    return get(entity, key) != nullptr;
}

} // namespace mtr::entity_kv
