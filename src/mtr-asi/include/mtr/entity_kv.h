// SEH-guarded wrapper around `entity_property_get_thunk` (the engine's
// kv-bag accessor at 0x004B8F00). Every entity in this engine carries a
// data-driven property bag — read via __thiscall(entity, key) -> char*.
//
// 171+ engine call sites use this for `class`, `model_name`, `mdb1`,
// `ai`, `sound`, and the `propXxx` family of properties that drive the
// disassembler / scanner / climbing / push-pullable systems.
//
// Wrapped here so we have one centrally-tested SEH-guarded call site;
// future modules (prop_overlay, npc_overlay Phase 3 click-pin, future
// kv enumeration) all route through this.

#pragma once

namespace mtr::entity_kv {

// Reads `key` from `entity`'s property bag. Returns the value string
// (engine-owned, valid for the entity's lifetime), or nullptr if:
//   - entity is null
//   - key is not present in the bag
//   - the underlying call faults (SEH catches; nullptr returned)
//
// Safe to call from any thread the engine itself uses (including
// EndScene / sim ticks). The kv bag is read-only at runtime; 171
// existing call sites in the engine treat this as a pure reader.
const char* get(void* entity, const char* key);

// Convenience predicate: true iff `entity` has `key` set in its bag
// (i.e. `get(entity, key) != nullptr`). For most prop properties this
// is the right predicate (presence = flag set).
bool has(void* entity, const char* key);

} // namespace mtr::entity_kv
