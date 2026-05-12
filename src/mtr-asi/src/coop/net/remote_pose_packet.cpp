// Wire format encode/decode for co-op pose snapshots. See header for
// design rationale and field-by-field semantics.

#include "mtr/coop/net/remote_pose_packet.h"

#include <cstring>

namespace mtr::coop::net {

bool parse_header(const uint8_t* bytes, std::size_t len, PacketHeader& out) {
    if (bytes == nullptr) return false;
    if (len < sizeof(PacketHeader)) return false;

    PacketHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(PacketHeader));

    if (hdr.magic != kPacketMagic) return false;
    if (hdr.version != kProtocolVersion) return false;

    out = hdr;
    return true;
}

bool parse_pose_snapshot(const uint8_t* bytes, std::size_t len,
                         PoseSnapshotBody& out_body) {
    if (bytes == nullptr) return false;
    if (len < kPoseSnapshotPacketSize) return false;

    std::memcpy(&out_body, bytes + sizeof(PacketHeader),
                sizeof(PoseSnapshotBody));
    return true;
}

std::size_t encode_pose_snapshot(uint8_t* bytes, std::size_t cap,
                                 const PoseSnapshotBody& body) {
    if (bytes == nullptr) return 0;
    if (cap < kPoseSnapshotPacketSize) return 0;

    PacketHeader hdr{};
    hdr.magic   = kPacketMagic;
    hdr.version = kProtocolVersion;
    hdr.type    = static_cast<uint8_t>(PacketType::PoseSnapshot);
    hdr._pad    = 0;

    // Zero the body's reserved padding before sending. Caller may pass a
    // body filled field-by-field from live engine data, leaving _pad with
    // whatever was on the stack — deterministic wire bytes are worth a
    // 52-byte stack copy, and this closes a tiny stack-bytes-over-network
    // disclosure channel.
    PoseSnapshotBody wire_body = body;
    wire_body._pad = 0;

    std::memcpy(bytes, &hdr, sizeof(PacketHeader));
    std::memcpy(bytes + sizeof(PacketHeader), &wire_body,
                sizeof(PoseSnapshotBody));
    return kPoseSnapshotPacketSize;
}

} // namespace mtr::coop::net
