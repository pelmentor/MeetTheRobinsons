// Coop per-entity registry mirror — Phase 2 step (b7.12, Tier-3 design).
//
// Purpose: the proper fix for orphan registry lookups — populate the
// orphan's `+0xCCC` registry with all the per-subsystem entries that
// engine_wilbur has, so every subscriber tick resolves cleanly without
// unlinking. This module + per-site routes (e.g. coop_vibrate_route)
// supersede the retired coop_orphan_filter (broad-suppression unlink).
//
// This module ships in two phases:
//
//   (b7.12) — DUMP PROBE. Read-only walk of engine_wilbur's `+0xCCC`
//             registry. Logs the (key, type, slot, storage, instance) for
//             every entry. Validates the enumerate logic before any
//             mutation. Gated by `-mtrasi-coop-dump-registry`.
//
//   (b7.13+) — MIRROR. For each enumerated entry, allocate a fresh slot
//              on the orphan via sub_5CB420 and wire the engine's
//              instance via sub_5CB220. Same pattern as the existing
//              `attach_engine_cm_to_orphan` (b7.6), iterated over the
//              entire registry. Gated by `-mtrasi-coop-mirror-registry`.
//
// Strategy doc + slot layout RE in:
//   research/findings/coop-tier3-design-2026-05-12.md
//   research/findings/coop-phase0-input-separation-point-2026-05-11.md
//
// All entry points SEH-guarded. Falls through safely on any fault.

#pragma once

#include <cstdint>

namespace mtr::coop_registry_mirror {

struct DumpStats {
    uint32_t vector_size     = 0;  // *(vector_handle + 4) — total slot count
    uint32_t slots_dumped    = 0;  // # slots successfully read
    uint32_t names_resolved  = 0;  // # slots whose name was recovered from
                                    // the hash table walk
    uint32_t read_faults     = 0;  // count of slots that faulted mid-read
};

// (b7.12) Walks engine_wilbur's `+0xCCC` registry vector and logs every
// slot's (i, slot_addr, type, hash, storage_cell, instance, key_name).
// Returns the DumpStats summary. Caller is expected to have validated
// engine_wilbur is non-zero and looks like a real entity.
DumpStats dump_engine_registry(uint32_t engine_wilbur);

// Cmdline gate query. True iff `-mtrasi-coop-dump-registry` is on argv.
bool dump_enabled();

// === (b7.13) Mirror mutator =================================================
//
// Iterates engine_wilbur's registry, inserts each key into orphan via the
// engine's own `sub_5CB420`, then copies the engine's storage value into
// the orphan's freshly-allocated storage cell using type-correct size.
//
// On success the orphan's `+0xCCC` registry has 21 keys with the same
// types as engine's, each cell holding a snapshot of engine's value.
// Resource-type slots (type 5) end up SHARING the engine's instance —
// same precedent as b7.6 (ControlMapper). Phase-1 networking will later
// per-player route those via b2-rem-2 thunks.

struct MirrorStats {
    uint32_t engine_keys_seen        = 0;
    uint32_t inserts_attempted       = 0;
    uint32_t inserts_succeeded       = 0;
    uint32_t values_copied           = 0;
    uint32_t unknown_type_skipped    = 0;
    uint32_t read_or_write_faults    = 0;
};

MirrorStats mirror_engine_registry_to_orphan(uint32_t engine_wilbur,
                                              uint32_t orphan);

// Cmdline gate. True iff `-mtrasi-coop-mirror-registry` is on argv.
bool mirror_enabled();

} // namespace mtr::coop_registry_mirror
