// Cmdline parsing helpers. Shared by every cmdline-gated module
// (coop_dual_launch, coop_registry_mirror, remote_player_manager,
//  net_session, ...). Three independent copies of `cmdline_has_flag`
// existed before this module; they were consolidated here per RULE №2
// (no parallel implementations of the same concept).
//
// Whole-word matching is required for every co-op flag because the
// host/connect flag pair share the `-mtrasi-coop-` prefix and any
// future `-mtrasi-coop-host-debug`-style additions would substring-
// match the host flag. Both helpers below match flag-terminated-by
// NUL/space/tab.
//
// Returns-only-bool form `has_flag` and value-extractor `get_flag_value`
// share their token-terminator definition: NUL / space / tab.

#pragma once

#include <cstddef>

namespace mtr::cmdline_utils {

// True iff `flag` appears as a whole word in `cmdline`. Both args may
// be NULL (returns false). The terminator after the flag must be NUL,
// space, or tab — guards against `-foo-host-x` substring traps when
// searching for `-foo-host`.
bool has_flag(const char* cmdline, const char* flag);

// If `flag` appears followed by a value token (terminated by NUL /
// space / tab), copies the value into out[0..cap-1] NUL-terminated
// and returns true. Returns false on any of:
//   - cmdline or flag is NULL
//   - flag is not present
//   - flag is present but at end-of-string (no value follows)
//   - flag's "value" starts with '-' (mis-parse — that's the next flag)
//   - value length >= cap (would truncate; safer to refuse)
// `out` is NUL-terminated only on a true return; on false return its
// contents are unspecified (caller should treat as garbage).
bool get_flag_value(const char* cmdline, const char* flag,
                    char* out, std::size_t cap);

} // namespace mtr::cmdline_utils
