// Phase 0.5 RE: locate widget m_pcName offset on widget objects.
//
// Hooks the engine's central SubmitSprite (sub_4E9350). At each call,
// scans the caller's stack frame for pointer-typed dwords. For each
// candidate that looks like a heap pointer, scans the first 0x100 bytes
// of the pointed-to object for ASCII strings matching widget identifier
// patterns ("IDS_*", "IDT_*", "IDG_*", etc). Both inline char[] fields
// and char* fields (single-deref) are scanned.
//
// One-shot capture: stops after `budget` distinct findings written to
// the log. The log lives next to the ASI as `mtr-asi-widget-probe.log`.
//
// Goal: from the log, identify the universal offset where Sprite (and
// related widget) objects store their string identifier — needed for
// the UI element identity refactor MVP (research/findings/
// ui-widget-system-phase0-2026-05-09.md).

#pragma once

namespace mtr::widget_probe {

bool install();
bool installed();
bool armed();
int  findings_count();
unsigned long long dispatch_call_count();

void arm(int budget);
void disarm();

// Production side-table: SpriteEntry* -> widget_name (string lifetime =
// engine widget object lifetime; treat as borrowed const char*). Filled
// per frame from the always-on PRE/POST hook on sub_4E9350. Use
// `widget_name_for_entry` from sprite_xform's process_list (or wherever
// SpriteEntries are walked). Caller MUST call `clear_frame_table()` at
// frame start to drop stale entries.
const char*   widget_name_for_entry(void* entry_ptr);
void          clear_frame_table();
unsigned int  frame_table_size();
void          debug_dump_frame_table();
void          debug_dump_widget_map();

// Request a one-shot dump of the side-table at the END of the next render
// frame (just before clear_frame_table() empties it). Cheap: no new
// per-frame work — only adds a single atomic bool check to clear_frame_table.
void          request_dump_next_frame();

// Phase 0B caller-PC audit: arms a dedupping logger that pairs every
// sub_4E9350 caller's return address with the resulting SpriteEntry's
// state_key and sort_key. Writes "[caller_audit] caller_pc=...
// state_key=... sort_key=... entry=..." lines to mtr-asi.log; auto-
// disarms after `budget` unique pairs. Used to identify which Render
// method submits a given widget so we can find the right vtable slot to
// hook for engine-name capture (Phase 0 of the widget-name capture plan).
void          caller_audit_arm(int budget);
void          caller_audit_disarm();
bool          caller_audit_armed();
int           caller_audit_count();

} // namespace mtr::widget_probe
