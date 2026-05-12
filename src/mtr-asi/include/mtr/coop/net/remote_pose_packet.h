// Wire format for co-op pose snapshots. Phase 1.4a (2026-05-12).
//
// Carries one Wilbur's pos+rot from the authoritative host to each
// client every sim tick. Maps to MTA's `PACKET_ID_PLAYER_PURESYNC`
// (pose-snapshot path that bypasses the keysync buffer entirely).
// Receiver hands the decoded pose to `MtrRemotePlayer::push_interp_snapshot`.
//
// Why pose-only (and not input replication, MTA's keysync path):
//   see `research/findings/coop-phase-1-3-vs-1-4-ordering-2026-05-12.md`.
//   Short version: our Phase 1.1+1.2 scaffold already implements the
//   puresync-style interp pipeline; the buffer (Phase 1.3) is only
//   needed if/when we move to input replication, which is a separate
//   scope decision.
//
// Architectural principles applied:
//   #5 minimum viable subset  — fields = pos + rot only (matches the
//                                existing `EntityPose`). Velocity /
//                                anim / health / weapon land as
//                                separate sub-phases when each is
//                                demonstrated necessary.
//   #7 wrapper vs gameplay/net — this header lives under coop/net/;
//                                no engine VA derefs, no engine class
//                                knowledge. Pure wire types.
//
// Endianness: x86 Windows is the only supported platform, both ends.
// The wire format is host-endian (LE) by definition; no byte-swap is
// needed. If a non-x86 endpoint is ever in scope, that's a v2 of the
// protocol with explicit conversion — never a silent compat path.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mtr::coop::net {

// Bumped when ANY known packet type's wire layout changes. Receiver
// drops packets with version != kProtocolVersion (no negotiated
// downgrade; per RULE №2 there's exactly one protocol in play).
//
// v2 (2026-05-12 night): time_ctx widened from uint8_t to uint16_t to
// remove the 8-bit modular-wrap ambiguity window (4.27s at 60Hz emit
// rate, half-window at 2.13s). Phase 1.4b audit finding (85% conf).
// Wire size unchanged — the prior _pad[2] slot absorbed the widening.
constexpr uint16_t kProtocolVersion = 2;

// Magic prefix on every datagram. Defends against an unrelated UDP
// packet on the same port being parsed as a coop packet. Bytes on the
// wire: 'M','R','C','O' (Meet The Robinsons co-op). Stored as a
// little-endian uint32_t so a memcpy of the first 4 bytes equals
// this constant on x86.
constexpr uint32_t kPacketMagic = 0x4F43524D;

// Discriminant for the body that follows the header. New values are
// appended; existing values are never reused or renumbered.
enum class PacketType : uint8_t {
    PoseSnapshot = 1,
};

#pragma pack(push, 1)

// 8 bytes on the wire. Reads from offset 0 of every datagram.
struct PacketHeader {
    uint32_t magic;    // == kPacketMagic
    uint16_t version;  // == kProtocolVersion
    uint8_t  type;     // PacketType
    uint8_t  _pad;     // reserved, zero on wire
};

// 52 bytes on the wire. Reads from offset 8 of a PoseSnapshot datagram.
// player_id  : which player this pose belongs to (0 = host's own, 1 = first
//              remote, ...). Receiver looks up `MtrRemotePlayer` by this id.
// _pad       : reserved, zero on wire. Sits between player_id and the
//              naturally 2-byte-aligned time_ctx (also forces a stable
//              layout under #pragma pack regardless of compiler defaults).
// time_ctx   : rolling counter the sender bumps each emit. Used by the
//              receiver to drop out-of-order datagrams (UDP doesn't preserve
//              order). Comparison is modular: newer iff (uint16_t)(incoming
//              - last) < 32768. At 60Hz emit, the 16-bit counter wraps every
//              ~18 min and the half-window for stale-vs-fresh ambiguity is
//              ~9 min — well past any realistic packet delay.
// pos / rot  : layout-compatible with `mtr::interp::EntityPose::pos/rot`
//              (engine offsets +0x58 and +0x70). Receiver memcpy's these
//              fields into a stack-local EntityPose, then stamps
//              `qpc = qpc_now()` (sender QPC is meaningless on a different
//              machine — local-clock receive time is what interp needs).
struct PoseSnapshotBody {
    uint8_t  player_id;
    uint8_t  _pad;
    uint16_t time_ctx;
    float    pos[3];
    float    rot[9];
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 8,
              "PacketHeader wire size must be exactly 8 bytes");
static_assert(sizeof(PoseSnapshotBody) == 52,
              "PoseSnapshotBody wire size must be exactly 52 bytes");

// Total datagram size for a pose snapshot. Receiver allocates at least
// this much for its recv buffer; sender writes exactly this many bytes.
constexpr std::size_t kPoseSnapshotPacketSize =
    sizeof(PacketHeader) + sizeof(PoseSnapshotBody);  // 60

// ---------------------------------------------------------------------------
// Encode / decode. memcpy-based — no reinterpret_cast'ing of unaligned recv
// buffers, no UB. Both ends are x86-LE, so no byte-swap.
// ---------------------------------------------------------------------------

// Validate that `bytes[0..len)` starts with a header for our protocol
// (magic + version match). On success, fills `out` from the first 8
// bytes and returns true. On failure, leaves `out` untouched and
// returns false. Does NOT inspect `type` — caller dispatches on that.
bool parse_header(const uint8_t* bytes, std::size_t len, PacketHeader& out);

// After a successful parse_header that returned PacketType::PoseSnapshot,
// validate the body length and decode the 52-byte body. Returns true on
// success and fills `out_body`; returns false if `len` is too small.
bool parse_pose_snapshot(const uint8_t* bytes, std::size_t len,
                         PoseSnapshotBody& out_body);

// Serialise a pose snapshot into `bytes` for sending. Writes exactly
// `kPoseSnapshotPacketSize` bytes on success and returns that count.
// Returns 0 if `cap` is too small (caller dropped the send). Header is
// constructed internally (magic + version + PoseSnapshot type).
std::size_t encode_pose_snapshot(uint8_t* bytes, std::size_t cap,
                                 const PoseSnapshotBody& body);

} // namespace mtr::coop::net
