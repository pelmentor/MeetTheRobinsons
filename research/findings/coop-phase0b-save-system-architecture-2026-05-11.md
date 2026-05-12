# Coop Phase 0B — save-system architecture map (2026-05-11)

## Status: PARTIAL — key coop-relevant findings characterized; full save-format RE deferred.

The Phase 0B "save-system RE" item from the v2 plan was estimated at ~1wk of uncharted IDA work. This session does the **coop-relevant subset** — enough to confirm that the save subsystem won't interfere with the replication work, without doing the full save-format RE.

## Bottom-line for coop work

**Save and DistributedState replication use independent serialization mechanisms.** Implementing the NetworkManager (Phase 1) and the replication path (Phase 3) will not interact with save/load.

Evidence: the string `"DistributedState"` at 0x6CBD98 has exactly **4 xrefs in the entire binary**, and all 4 are inside the publish/receive pair (`entity_publish_distributed_state` @ 0x5AFDB0 and `entity_receive_distributed_state` @ 0x5AFE90). No save-system code references it.

This means: when the engine save handler serializes an entity, it does NOT call into `entity_publish_distributed_state`. Save uses its own pipeline. The DistributedState mechanism is purely a network-replication channel.

## Save subsystem architecture (renamed in IDB)

The save subsystem is initialized as a sub-object of the game-app singleton inside `game_App_ctor` @ 0x56FFA0:

| Address | Symbol | Notes |
|---|---|---|
| 0x72F824 | `unk_72F824` | Static array. unk_72F824[0] = heap state buffer pointer (93,468 bytes alloc'd in game_App_ctor). |
| 0x72F828 | `dword_72F828` | Pointer to save subsystem object — initialized as `&App[+624]`. Engine code accesses its vtable as `*(*dword_72F828 + offset)`. |
| 0x575D60 | `save_pump_dispatcher` | Per-operation pump thread entry. Reads opcode from state buffer, dispatches via switch on opcode. |
| 0x574C60 | `save_handler_write` | Case 4 — SAVE handler (~1KB function, no hexrays decompile, complex flow). |
| 0x575090 | `save_handler_load` | Case 5 — LOAD handler. Already driven by `src/mtr-asi/src/save_system.cpp`. |
| 0x575B00 | `save_handler_enumerate` | Case 12 — slot enumeration. |

### Dispatcher cases (from save_pump_dispatcher)

| Opcode | Handler | Purpose |
|---|---|---|
| 1, 2 | (inline) | Generic operation via dword_72F828 vtable[1] |
| 4 | `save_handler_write` | SAVE to slot |
| 5 | `save_handler_load` | LOAD from slot |
| 6 | sub_574AB0 | Unknown |
| 7 | sub_574700 | Unknown |
| 9 | sub_5757E0 | Unknown |
| 0xB | sub_574850 | Unknown |
| 0xC | `save_handler_enumerate` | Enumerate slots |
| 0xD | sub_575880 | Unknown |
| 0x11 | sub_574BA0 | Unknown |

## State buffer layout (consolidating earlier save_system.cpp RE)

The 93,468-byte heap buffer at `unk_72F824[0]` holds the operation state. Known fields (dword indices into the buffer + static-array indices into `unk_72F824` itself):

| Field | Index type | Offset | Purpose |
|---|---|---|---|
| heap-buffer ptr | static array | [0] | Points to 93,468-byte heap buffer |
| request opcode | static [170] AND v1 [170] | both | LOAD/SAVE/etc. opcode |
| done flag | static [172] LOBYTE | | pump exits when != 0 |
| slot index | v1 [190] | | which save slot for op |
| result code | static [196] | | 0 = success |
| valid-slot count | v1 [253] | | populated by enumerate |
| autosave-popup gate | static [218] | | set 0 to skip popup loop in load handler |
| skip-success popup | static [247] | | set 1 to skip success popup |
| op-in-progress | static [321] | | engine-set during dispatch |
| mid-load-popup gate | static [324] BYTE1 | | set != 1 to skip popup |

The state buffer is huge (93KB) — it holds not just the operation control state but presumably the actual save data being read/written via the dword_72F828 vtable methods.

## What is NOT yet RE'd (= remaining Phase 0B work, deferred)

1. **Save (write) format on disk.** sub_574C60 was too complex for hexrays decompile; would need patient disassembly walk.
2. **Entity serialization pipeline.** Each entity class presumably has a serialize/deserialize method on its vtable. The slot index is unknown. Could be discovered by hooking the save handler at runtime and tracing which vtable slots fire.
3. **Save engine vtable interface.** `dword_72F828` has many readers but the full method set isn't characterized — calls at offsets 4, 0x4C, 0x60, etc. dispatch through it.
4. **MemCard popup string flow.** sub_574C60 pushes `"MemCard_Popup_Title_Load"` (likely a UI/localization key) — suggests the same popup infrastructure handles save and load.
5. **DistributedState alternative — does an "EntitySerialize" or similar string exist?** Could be another string-keyed mechanism with a different key. Not searched exhaustively this session.

These items are not blocking for the coop work. They become relevant for:
- Phase 2 (input separation): when saves need to handle 2-player presence.
- Future MP enhancements: if we want to make MP sessions save-resumable.

## Implications for the coop plan

| Item | Resolution |
|---|---|
| Will replication interfere with save? | **No.** Independent paths. |
| Will save-resume work in MP? | **Unknown — needs Phase 2 RE pass.** Defer. |
| Are save and DistributedState formats compatible? | **No.** Different mechanisms. Don't try to reuse. |

The coop work can proceed with replication design independent of save/load. Phase 0B as it pertains to **prerequisite RE for replication design** is now closed.

## Engine VAs (renamed in IDB this session)

| Address | Symbol |
|---|---|
| 0x575D60 | `save_pump_dispatcher` |
| 0x574C60 | `save_handler_write` |
| 0x575090 | `save_handler_load` |
| 0x575B00 | `save_handler_enumerate` |
