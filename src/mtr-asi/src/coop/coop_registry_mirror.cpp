// Coop registry mirror — Phase 2 step (b7.12, Tier-3 dump probe).
//
// See mtr/coop_registry_mirror.h for module purpose.
//
// This file implements ONLY the read-only dump (b7.12). The mutator
// (b7.13+) lands in a follow-up session once the enumerate logic is
// validated by live test.

#include "mtr/coop_registry_mirror.h"

#include "mtr/cmdline_utils.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::coop_registry_mirror {

namespace {

// === Registry struct layout =================================================
//
// Decoded from sub_5CB310 / sub_5CB420 / sub_5D3730. The struct at
// `entity + 0xCCC` is:
//
//   wilbur + 0xCCC ──► [registry]
//                        ├─ +0x00  vector_handle*   ─► [vector]
//                        │                              ├─ +0x00 data* (slot ptr array)
//                        │                              ├─ +0x04 size
//                        │                              ├─ +0x08 capacity
//                        │                              └─ +0x0C ...
//                        └─ +0x08  [hash table]
//                             ├─ +0x04 top-bucket shift     (this[1])
//                             ├─ +0x08 bottom-bucket mask   (this[2])
//                             ├─ +0x10 top-bucket array*    (this[4])
//                             ├─ +0x1C used count           (this[7])
//                             ├─ +0x24 resize threshold     (this[9])
//                             └─ +0x28 capacity mask        (this[10])
//
// Hash-table nodes are 4 dwords + payload:
//   node[0] = next-in-chain
//   node[1] = name hash
//   node[2] = key_ptr (char*)
//   node[3] = key_len
//   node[4] = slot_ptr (= the 28-byte slot record)
//
// Slot record (28 bytes):
//   slot+0x00  owner backref (the wilbur entity address)
//   slot+0x04  byte flag (storage allocated)
//   slot+0x08  flag2
//   slot+0x0C  storage cell* (size depends on type)
//   slot+0x10  type code (0/1/5 → 4-byte cell, 2 → 12, 3 → 1, 4 → 36, 6 → 4 zeroed)
//   slot+0x14  name hash (= same as node[1])
//   slot+0x18  byte flag

constexpr uintptr_t kRegistryOffsetInEntity   = 0xCCC;
constexpr uintptr_t kVectorHandleOffset       = 0x00;  // *(registry) = handle
constexpr uintptr_t kVectorDataOffset         = 0x00;  // handle[0]    = data*
constexpr uintptr_t kVectorSizeOffset         = 0x04;  // handle[1]    = size
constexpr uintptr_t kVectorCapacityOffset     = 0x08;  // handle[2]    = capacity
constexpr uintptr_t kHashTableOffset          = 0x08;  // registry + 8 = hash table
constexpr uintptr_t kHashTopShiftOffset       = 0x04;  // ht[1]
constexpr uintptr_t kHashBotMaskOffset        = 0x08;  // ht[2]
constexpr uintptr_t kHashTopArrayOffset       = 0x10;  // ht[4]
constexpr uintptr_t kHashCapMaskOffset        = 0x28;  // ht[10]
constexpr uintptr_t kSlotStorageOffset        = 0x0C;
constexpr uintptr_t kSlotTypeOffset           = 0x10;
constexpr uintptr_t kSlotHashOffset           = 0x14;
constexpr uintptr_t kNodeNextOffset           = 0x00;
constexpr uintptr_t kNodeHashOffset           = 0x04;
constexpr uintptr_t kNodeKeyPtrOffset         = 0x08;
constexpr uintptr_t kNodeKeyLenOffset         = 0x0C;
constexpr uintptr_t kNodeSlotPtrOffset        = 0x10;

// Walk safety caps. Engine lists are tens of nodes — these are generous
// upper bounds so any cycle / corrupt walk bails rather than hangs.
constexpr uint32_t kMaxTopBuckets   = 256;
constexpr uint32_t kMaxBotPerBucket = 256;
constexpr uint32_t kMaxChainDepth   = 64;
constexpr uint32_t kMaxVectorSize   = 1024;

// === Name resolution via hash-table walk ====================================
//
// To recover a slot's key name, walk the hash table at `registry + 8` and
// match by slot_ptr (or hash). We walk it ONCE up front, building a small
// linear table of (slot_ptr → name_ptr) pairs. Then enumerate the slot
// vector and look up each slot's name in the table.

struct HashEntry {
    uint32_t    slot_ptr;
    uint32_t    name_ptr;
    uint32_t    name_len;
    uint32_t    name_hash;
};

constexpr uint32_t kMaxHashEntries = 256;

uint32_t safe_read_u32(uint32_t addr) {
    __try {
        return *reinterpret_cast<uint32_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Walk the hash table at registry+8, collect (slot_ptr, name) entries
// into out[]. Returns the number of entries collected. SEH-guarded.
uint32_t collect_hash_entries_seh(uint32_t registry,
                                   HashEntry out[],
                                   uint32_t out_cap) {
    uint32_t collected = 0;
    __try {
        const uint32_t ht = registry + kHashTableOffset;
        const uint32_t top_array = safe_read_u32(ht + kHashTopArrayOffset);
        const uint32_t bot_mask  = safe_read_u32(ht + kHashBotMaskOffset);
        const uint32_t cap_mask  = safe_read_u32(ht + kHashCapMaskOffset);
        // top_shift is implicit: (cap_mask+1) / (bot_mask+1) gives n_top,
        // assuming the engine's 2-level decomposition is power-of-two clean.

        if (top_array == 0 || cap_mask == 0) {
            return 0;
        }

        // n_top × n_bot = cap_mask + 1 (capacity is power-of-two).
        const uint32_t n_bot = bot_mask + 1;
        const uint32_t total_caps = cap_mask + 1;
        if (n_bot == 0) return 0;
        const uint32_t n_top = total_caps / n_bot;

        const uint32_t safe_n_top = (n_top > kMaxTopBuckets)
            ? kMaxTopBuckets : n_top;
        const uint32_t safe_n_bot = (n_bot > kMaxBotPerBucket)
            ? kMaxBotPerBucket : n_bot;

        for (uint32_t top_idx = 0; top_idx < safe_n_top; ++top_idx) {
            const uint32_t top_bucket = safe_read_u32(top_array + 4 * top_idx);
            if (top_bucket == 0) continue;

            for (uint32_t bot_idx = 0; bot_idx < safe_n_bot; ++bot_idx) {
                uint32_t node = safe_read_u32(top_bucket + 4 * bot_idx);
                uint32_t depth = 0;
                while (node != 0 && depth < kMaxChainDepth) {
                    if (collected >= out_cap) {
                        return collected;
                    }
                    const uint32_t next     = safe_read_u32(node + kNodeNextOffset);
                    const uint32_t hash     = safe_read_u32(node + kNodeHashOffset);
                    const uint32_t key_ptr  = safe_read_u32(node + kNodeKeyPtrOffset);
                    const uint32_t key_len  = safe_read_u32(node + kNodeKeyLenOffset);
                    const uint32_t slot_ptr = safe_read_u32(node + kNodeSlotPtrOffset);

                    out[collected].slot_ptr  = slot_ptr;
                    out[collected].name_ptr  = key_ptr;
                    out[collected].name_len  = key_len;
                    out[collected].name_hash = hash;
                    ++collected;

                    node = next;
                    ++depth;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best-effort; return whatever we collected before the fault.
    }
    return collected;
}

// Linear search: find the hash-entry whose slot_ptr matches `target`.
// Falls back to matching by name_hash if slot_ptr doesn't match (in case
// the hash table stores the node-as-slot, i.e. slot_ptr == node + 0x10).
const HashEntry* find_name_for_slot(uint32_t slot_ptr,
                                     uint32_t slot_hash,
                                     const HashEntry entries[],
                                     uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].slot_ptr == slot_ptr) return &entries[i];
    }
    // Fallback: hash match (cheap, but ambiguous if two keys collide —
    // unlikely for the 10-40 keys we expect on a wilbur).
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].name_hash == slot_hash) return &entries[i];
    }
    return nullptr;
}

// SEH-safe printable copy of a name. The engine's key strings are static
// .rdata so they're null-terminated, but we cap defensively at 64 chars.
void copy_name_seh(uint32_t name_ptr, uint32_t name_len, char out[65]) {
    out[0] = '\0';
    if (name_ptr == 0) return;
    const uint32_t n = (name_len < 64) ? name_len : 64;
    __try {
        const char* src = reinterpret_cast<const char*>(name_ptr);
        for (uint32_t i = 0; i < n; ++i) {
            const char c = src[i];
            if (c == '\0') {
                out[i] = '\0';
                return;
            }
            // Printable ASCII guard.
            out[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        out[n] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

} // namespace

// === Public API =============================================================

bool dump_enabled() {
    static const bool s_enabled = []{
        return mtr::cmdline_utils::has_flag(GetCommandLineA(),
                                            "-mtrasi-coop-dump-registry");
    }();
    return s_enabled;
}

DumpStats dump_engine_registry(uint32_t engine_wilbur) {
    DumpStats st{};
    if (engine_wilbur == 0) {
        mtr::log::info("[coop_registry_mirror] dump: engine_wilbur=0 — skipping");
        return st;
    }

    const uint32_t registry = engine_wilbur + kRegistryOffsetInEntity;

    // 1. Walk hash table to collect (slot_ptr → name) entries.
    HashEntry hash_entries[kMaxHashEntries];
    const uint32_t hash_count =
        collect_hash_entries_seh(registry, hash_entries, kMaxHashEntries);

    mtr::log::info("[coop_registry_mirror] dump start: engine_wilbur=0x%08X"
                   " registry=0x%08X hash_table_entries=%u",
                   engine_wilbur, registry, hash_count);

    // 2. Walk the slot vector and log each entry.
    __try {
        const uint32_t vec_handle = safe_read_u32(registry + kVectorHandleOffset);
        if (vec_handle == 0) {
            mtr::log::info("[coop_registry_mirror] dump: vector_handle=NULL");
            return st;
        }
        const uint32_t data_ptr = safe_read_u32(vec_handle + kVectorDataOffset);
        const uint32_t size     = safe_read_u32(vec_handle + kVectorSizeOffset);
        const uint32_t capacity = safe_read_u32(vec_handle + kVectorCapacityOffset);
        st.vector_size = size;

        mtr::log::info("[coop_registry_mirror] dump: vector handle=0x%08X"
                       " data=0x%08X size=%u capacity=%u",
                       vec_handle, data_ptr, size, capacity);

        if (data_ptr == 0 || size == 0) {
            return st;
        }

        const uint32_t safe_size = (size > kMaxVectorSize) ? kMaxVectorSize : size;

        for (uint32_t i = 0; i < safe_size; ++i) {
            const uint32_t slot = safe_read_u32(data_ptr + 4 * i);
            if (slot == 0) {
                mtr::log::info("[coop_registry_mirror]   [%3u] slot=NULL", i);
                continue;
            }
            const uint32_t type    = safe_read_u32(slot + kSlotTypeOffset);
            const uint32_t slot_hash = safe_read_u32(slot + kSlotHashOffset);
            const uint32_t storage = safe_read_u32(slot + kSlotStorageOffset);
            const uint32_t inst    = (storage != 0) ? safe_read_u32(storage) : 0;

            const HashEntry* he = find_name_for_slot(slot, slot_hash,
                                                      hash_entries, hash_count);
            char name_buf[65];
            if (he != nullptr) {
                copy_name_seh(he->name_ptr, he->name_len, name_buf);
                ++st.names_resolved;
            } else {
                name_buf[0] = '?';
                name_buf[1] = '?';
                name_buf[2] = '\0';
            }

            // Heuristic for "did we successfully read everything?":
            // slot != 0 and we got some hash. (storage can legitimately
            // be 0 if the slot was inserted but never written.)
            if (slot != 0 && slot_hash != 0) {
                ++st.slots_dumped;
            } else {
                ++st.read_faults;
            }

            mtr::log::info("[coop_registry_mirror]   [%3u] slot=0x%08X"
                           " type=%u hash=0x%08X storage=0x%08X *storage=0x%08X"
                           " name='%s'",
                           i, slot, type, slot_hash, storage, inst, name_buf);
        }

        mtr::log::info("[coop_registry_mirror] dump end:"
                       " vector_size=%u slots_dumped=%u names_resolved=%u"
                       " (hash_entries_collected=%u)",
                       st.vector_size, st.slots_dumped, st.names_resolved,
                       hash_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("[coop_registry_mirror] dump: SEH fault mid-dump");
    }

    return st;
}

// === (b7.13) Mirror mutator ================================================

namespace {

// Engine's registry-insert function. __thiscall(this=registry, edx, value,
// name, type, flag1, flag2). Same signature as coop_spawn_probe::PFN_RegInsert.
//
// CRITICAL: the return value is NOT a slot pointer (it's a hash-table-internal
// pointer per sub_5EDF50's return path inside sub_5CB420). To get the real
// slot AFTER insertion, call PFN_RegLookup2 (sub_5CB310) with the same key.
// This is the pattern the existing b7.6 attach_engine_cm_to_orphan uses.
using PFN_RegInsert =
    uint32_t (__fastcall*)(void* this_, void* edx, uint32_t value,
                            const char* key, uint32_t type,
                            uint32_t flag1, uint32_t flag2);

// Engine's registry-lookup function. __thiscall(this=registry, edx, key,
// unused). Returns the slot pointer (or 0 if missing).
using PFN_RegLookup =
    uint32_t (__fastcall*)(void* this_, void* edx, const char* key, int unused);

constexpr uintptr_t kRegInsertVA = 0x005CB420;
constexpr uintptr_t kRegLookupVA = 0x005CB310;

// Size of the engine-allocated storage cell, in bytes, indexed by the
// `type` field (slot+0x10). From sub_5CB420's switch:
//   type 0 → 4   (single dword)
//   type 1 → 4   (single float)
//   type 2 → 12  (vec3)
//   type 3 → 1   (single byte)
//   type 4 → 36  (not seen in the 21 keys)
//   type 5 → 4   (resource pointer)
//   type 6 → 4   (single dword, zero-initialized)
uint32_t cell_size_for_type(uint32_t type) {
    switch (type) {
        case 0: return 4;
        case 1: return 4;
        case 2: return 12;
        case 3: return 1;
        case 4: return 36;
        case 5: return 4;
        case 6: return 4;
        default: return 0;
    }
}

// SEH-safe direct memcpy from engine_storage to orphan_storage. Returns
// true if the copy ran to completion.
bool copy_storage_seh(uint32_t orphan_storage,
                      uint32_t engine_storage,
                      uint32_t size_bytes) {
    if (orphan_storage == 0 || engine_storage == 0 || size_bytes == 0) {
        return false;
    }
    __try {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(engine_storage);
        uint8_t* dst       = reinterpret_cast<uint8_t*>(orphan_storage);
        for (uint32_t i = 0; i < size_bytes; ++i) {
            dst[i] = src[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool mirror_enabled() {
    static const bool s_enabled = []{
        return mtr::cmdline_utils::has_flag(GetCommandLineA(),
                                            "-mtrasi-coop-mirror-registry");
    }();
    return s_enabled;
}

MirrorStats mirror_engine_registry_to_orphan(uint32_t engine_wilbur,
                                              uint32_t orphan) {
    MirrorStats st{};
    if (engine_wilbur == 0 || orphan == 0) {
        mtr::log::info("[coop_registry_mirror] mirror: invalid args"
                       " (engine_wilbur=0x%08X orphan=0x%08X)",
                       engine_wilbur, orphan);
        return st;
    }

    const uint32_t engine_registry = engine_wilbur + kRegistryOffsetInEntity;
    const uint32_t orphan_registry = orphan + kRegistryOffsetInEntity;

    // 1. Walk hash table to collect (slot_ptr → name) entries on the engine.
    HashEntry hash_entries[kMaxHashEntries];
    const uint32_t hash_count =
        collect_hash_entries_seh(engine_registry, hash_entries, kMaxHashEntries);

    mtr::log::info("[coop_registry_mirror] mirror start:"
                   " engine_wilbur=0x%08X orphan=0x%08X"
                   " engine_registry=0x%08X orphan_registry=0x%08X"
                   " engine_hash_entries=%u",
                   engine_wilbur, orphan, engine_registry, orphan_registry,
                   hash_count);

    auto fn_insert = reinterpret_cast<PFN_RegInsert>(kRegInsertVA);
    auto fn_lookup = reinterpret_cast<PFN_RegLookup>(kRegLookupVA);

    // 2. Walk the engine slot vector and mirror each entry.
    __try {
        const uint32_t vec_handle =
            safe_read_u32(engine_registry + kVectorHandleOffset);
        if (vec_handle == 0) {
            mtr::log::info("[coop_registry_mirror] mirror: engine vector_handle=NULL");
            return st;
        }
        const uint32_t data_ptr = safe_read_u32(vec_handle + kVectorDataOffset);
        const uint32_t size     = safe_read_u32(vec_handle + kVectorSizeOffset);
        if (data_ptr == 0 || size == 0) {
            mtr::log::info("[coop_registry_mirror] mirror: empty engine registry"
                           " (data=0x%08X size=%u)", data_ptr, size);
            return st;
        }
        const uint32_t safe_size = (size > kMaxVectorSize) ? kMaxVectorSize : size;

        for (uint32_t i = 0; i < safe_size; ++i) {
            const uint32_t engine_slot = safe_read_u32(data_ptr + 4 * i);
            if (engine_slot == 0) {
                continue;
            }
            const uint32_t type            = safe_read_u32(engine_slot + kSlotTypeOffset);
            const uint32_t slot_hash       = safe_read_u32(engine_slot + kSlotHashOffset);
            const uint32_t engine_storage  = safe_read_u32(engine_slot + kSlotStorageOffset);
            ++st.engine_keys_seen;

            // Resolve the key name from the hash-table side-table.
            const HashEntry* he = find_name_for_slot(engine_slot, slot_hash,
                                                      hash_entries, hash_count);
            if (he == nullptr || he->name_ptr == 0) {
                mtr::log::info("[coop_registry_mirror]   [%2u] no name for"
                               " slot=0x%08X type=%u hash=0x%08X — SKIP",
                               i, engine_slot, type, slot_hash);
                ++st.read_or_write_faults;
                continue;
            }

            const uint32_t size_bytes = cell_size_for_type(type);
            if (size_bytes == 0) {
                ++st.unknown_type_skipped;
                mtr::log::info("[coop_registry_mirror]   [%2u] unknown type %u"
                               " — SKIP", i, type);
                continue;
            }

            const char* name_ptr = reinterpret_cast<const char*>(he->name_ptr);

            // 3. Insert into orphan's registry. flag1=0 makes sub_5CB420
            //    allocate a fresh storage cell sized to `type`. The return
            //    value of fn_insert is NOT the slot — must lookup AGAIN.
            //    (Same pattern as b7.6 attach_engine_cm_to_orphan.)
            ++st.inserts_attempted;
            __try {
                fn_insert(reinterpret_cast<void*>(orphan_registry),
                          nullptr, orphan, name_ptr, type, 0, 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // fall through; the lookup below will see no slot
            }
            uint32_t orphan_slot = 0;
            __try {
                orphan_slot = fn_lookup(reinterpret_cast<void*>(orphan_registry),
                                        nullptr, name_ptr, 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                orphan_slot = 0;
            }
            if (orphan_slot == 0) {
                ++st.read_or_write_faults;
                mtr::log::info("[coop_registry_mirror]   [%2u] insert+lookup"
                               " FAILED type=%u", i, type);
                continue;
            }
            const uint32_t orphan_storage =
                safe_read_u32(orphan_slot + kSlotStorageOffset);
            if (orphan_storage == 0) {
                ++st.read_or_write_faults;
                mtr::log::info("[coop_registry_mirror]   [%2u] insert OK but"
                               " orphan_storage=NULL"
                               " name='?' type=%u", i, type);
                continue;
            }
            ++st.inserts_succeeded;

            // 4. Copy engine's storage VALUE into orphan's storage cell.
            if (engine_storage != 0) {
                if (copy_storage_seh(orphan_storage, engine_storage, size_bytes)) {
                    ++st.values_copied;
                } else {
                    ++st.read_or_write_faults;
                }
            }

            char dbg_name[65];
            copy_name_seh(he->name_ptr, he->name_len, dbg_name);
            mtr::log::info("[coop_registry_mirror]   [%2u] mirrored '%s'"
                           " type=%u size=%u engine_slot=0x%08X"
                           " orphan_slot=0x%08X engine_storage=0x%08X"
                           " orphan_storage=0x%08X",
                           i, dbg_name, type, size_bytes,
                           engine_slot, orphan_slot,
                           engine_storage, orphan_storage);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mtr::log::info("[coop_registry_mirror] mirror: SEH fault mid-mirror");
    }

    mtr::log::info("[coop_registry_mirror] mirror end: seen=%u"
                   " attempted=%u inserted=%u copied=%u"
                   " unknown_type=%u faults=%u",
                   st.engine_keys_seen, st.inserts_attempted,
                   st.inserts_succeeded, st.values_copied,
                   st.unknown_type_skipped, st.read_or_write_faults);

    return st;
}

} // namespace mtr::coop_registry_mirror
