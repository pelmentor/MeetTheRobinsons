#pragma once

#include <cstdint>

namespace mtr {

// Bumped manually per release.
inline constexpr const char kModVersion[] = MTR_ASI_VERSION;

// Identifier for a known build of Wilbur.exe. Lookup is by SHA256 of the
// .rdata section (cheap to compute at runtime, immune to SecuROM mutating
// rr01 in memory). When you add a new build, also append its offsets to
// kKnownBuilds in offsets.h and bump kBuildCount.
struct BuildId {
    const char*   label;       // human-readable name, e.g. "retail-eu-2007-03-28"
    std::uint8_t  rdataSha256[32];
};

} // namespace mtr
