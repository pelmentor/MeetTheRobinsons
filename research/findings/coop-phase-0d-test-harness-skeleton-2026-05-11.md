# Coop Phase 0D — Two-process test harness skeleton (2026-05-11)

**Status:** SHIPPED (mock-mode smoke tests green; live game regression deferred to user).
**Build size:** 671,744 bytes (+512 from step-2k baseline).
**Predecessor:** [coop-phase-0a-audit-2026-05-10.md](coop-phase-0a-audit-2026-05-10.md), [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md) item 6.
**Governing rule:** RULE №1 — proper-fix path. Test infrastructure is foundational, not a crutch.

---

## Why

v2 plan audit item 6 ("Two-process test harness fails on 3 counts") flagged:

1. `mtr-asi-test-result.json` path is hardcoded in `test_harness.cpp:1378` AND `run-test.ps1:51`. Two simultaneous processes race on this single file.
2. `run-test.ps1` is single-process structure; orchestrating two requires a separate script.
3. `Global\Disney_s_Meet_The_Robinsons` mutex prevents same-machine dual-launch. Two-process testing requires two physical machines or two isolated VMs.

The v2 plan committed a Phase 1.5 sub-task `coop-test-harness` to address all three. Phase 0D ships the minimum-viable skeleton so Phase 1+ work can lean on it.

---

## What shipped

### Mod-side: port-parameterized result JSON path

[src/mtr-asi/src/test_harness.cpp](../../src/mtr-asi/src/test_harness.cpp):

- New cmdline flag: `-mtrasi-coop-port=<N>` parsed in `install()`. Valid range 1..65535; out-of-range is silently ignored (single-process path stays in effect).
- New atomic `g_coop_port` (default 0 = single-process / unsuffixed path).
- New helper `resolve_result_path_with_port(out, size, port)` shared between the harness writer and the public path-getter.
- Existing `resolve_result_path(out, size)` now defers to it with the current `g_coop_port` value.
- Result JSON gets a new `"coop_port": N` field so the orchestrator can verify the right process wrote each file.
- `TESTHARNESS:` log line now includes `coop_port=N`.

[src/mtr-asi/include/mtr/test_harness.h](../../src/mtr-asi/include/mtr/test_harness.h):

- New public API:
  - `int  coop_port()` — atomic read of the parsed flag.
  - `bool result_path(char* out, size_t out_size)` — port-suffixed result JSON path used by THIS process.

[src/mtr-asi/src/crash_handler.cpp](../../src/mtr-asi/src/crash_handler.cpp):

- Crash sentinel writer now uses `mtr::test_harness::result_path()` instead of hardcoding `mtr-asi-test-result.json`. This keeps host/client crash sentinels in their own lanes during coop runs.
- Sentinel JSON now includes `"coop_port": N` mirroring the success-path JSON.

### Orchestrator: `tools/run-coop-test.ps1`

[tools/run-coop-test.ps1](../../tools/run-coop-test.ps1) — three modes:

| Mode | What it does | Why |
|---|---|---|
| `mock` (default) | No game launch. Writes fixture JSONs for both ports, runs orchestrator parse/aggregate logic. | CI-safe smoke test of the orchestrator itself. Default so `pwsh tools/run-coop-test.ps1` always works. |
| `single-process` | Launches ONE Wilbur locally with `-mtrasi-coop-port=<HostPort>`. Polls the suffixed JSON. | Proves the mod-side port-suffix wiring against a real game on one machine. No mutex issue (single process). |
| `dual-machine` | Launches THE LOCAL SIDE (`-Role host` or `-Role client`). | Real two-process coop testing. Caller is responsible for launching the remote side on a separate machine; orchestrator only polls its own port. |

Aggregate exit code in `mock` mode is the worst-of-two by exit code: pass-pass → 0, anything-fail → 1, anything-timeout → 3, etc. The mock fixture writer + the aggregate logic are testable in <1 second with no game launch.

---

## Verification

Smoke tests run 2026-05-11 (mock mode only — live game launch deferred to user):

```
$ pwsh tools/run-coop-test.ps1 -Mode mock
  host=pass client=pass -> exit=0  ✓

$ pwsh tools/run-coop-test.ps1 -Mode mock -HostResult fail -ClientResult pass
  host=fail client=pass -> exit=1  ✓

$ pwsh tools/run-coop-test.ps1 -Mode mock -HostResult pass -ClientResult timeout
  host=pass client=timeout -> exit=3  ✓
```

Build is clean (only pre-existing strncpy C4996 warnings).

### Pending user verification

The mod-side path is byte-identical to step-2k when `-mtrasi-coop-port` is absent (cmdline parsing short-circuits, `g_coop_port` stays 0, both `resolve_result_path` and the JSON emitter fall back to the unsuffixed path). Regression risk is therefore minimal, but the user should re-run the canonical smoke test next time they're at the PC:

```
pwsh tools/run-test.ps1 -Scenario boot-to-main-menu -Redeploy
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy
```

Both should pass identically to the step-2k state. (Result JSON path: `Game/mtr-asi-test-result.json`, unchanged.)

Additionally, `single-process` mode can be smoke-tested with a real game launch:

```
pwsh tools/run-coop-test.ps1 -Mode single-process -HostPort 31415 \
     -Scenario boot-to-main-menu -Redeploy
```

Expected: writes `Game/mtr-asi-test-result-31415.json` with `"coop_port": 31415`, scenario passes, exit 0.

---

## Files modified

| File | Change |
|---|---|
| [src/mtr-asi/src/test_harness.cpp](../../src/mtr-asi/src/test_harness.cpp) | Port flag parse, port-suffixed path helper, JSON field, log field. |
| [src/mtr-asi/include/mtr/test_harness.h](../../src/mtr-asi/include/mtr/test_harness.h) | Public `coop_port()` + `result_path()` declarations. |
| [src/mtr-asi/src/crash_handler.cpp](../../src/mtr-asi/src/crash_handler.cpp) | Crash sentinel uses shared resolver; adds `coop_port` to JSON body. |
| [tools/run-coop-test.ps1](../../tools/run-coop-test.ps1) | NEW — three-mode orchestrator. |

---

## What this does NOT do (deliberately out of scope for Phase 0D)

- **No network code.** Phase 1 work. The orchestrator deals only with JSON I/O for now.
- **No remote-invoke mechanism for `dual-machine` mode.** Future Phase 1+: a small TCP rendezvous server or PSRemoting hook to synchronize start across machines. For now, the human launches both sides separately.
- **No coop-specific scenarios** in `test_harness.cpp`. Today `single-process` mode runs any existing scenario (e.g. `boot-to-main-menu`). Coop scenarios like `coop-ping`, `coop-second-player-spawn`, `coop-replication-roundtrip` get added as Phase 1/2/3 land.
- **No screenshot/log archival in `run-coop-test.ps1`.** Current orchestrator is a polling skeleton. Archival logic from `run-test.ps1` can be lifted in when actual coop scenarios start running.

These are the Phase 1.5 sub-task scope items, not Phase 0D scope.

---

## What next session should know

1. **Phase 0D is closed.** Mock mode works; mod-side wiring shipped; orchestrator skeleton in place.
2. **Pending live verification** by the user (`run-test.ps1 -Scenario load-save-1-show-ingame` regression + `run-coop-test.ps1 -Mode single-process` mod-side wire check). Cheap to do; ~10s each.
3. **Next Phase 0 candidate** is plan-level 0C (.sx script command catalog, ~1wk text scanning) or 0B (save-system RE, ~1wk uncharted). User has not directed which to take yet.
4. **Phase 1 (transport) MUST NOT start** until 0B + 0C land per v2 plan ordering — the audit's whole point was that Phase 0 prerequisites gate Phase 1.

---

## Engine VAs

None — this is pure tooling. No new RE in Phase 0D.
