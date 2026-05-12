#!/usr/bin/env pwsh
# Two-process coop test orchestrator. Phase 0D skeleton (2026-05-11).
#
# Wraps the existing per-process tools/run-test.ps1 contract for a coop
# test where one Wilbur instance is the host and another is the client.
#
# Key design points (from research/findings/coop-multiplayer-plan-2026-05-10.md,
# v2 audit item 6 — "Two-process test harness fails on 3 counts"):
#
#   * The mod's result JSON path is parameterized by -mtrasi-coop-port=<N>
#     since Phase 0D — host writes Game/mtr-asi-test-result-<HostPort>.json,
#     client writes Game/mtr-asi-test-result-<ClientPort>.json. Two
#     processes on different machines can't race on a single file.
#
#   * Same-machine dual-launch is now unblocked (2026-05-12): the
#     `coop_dual_launch` MinHook renames the Disney mutex to a per-PID
#     variant whenever `-mtrasi-coop-host` or `-mtrasi-coop-connect` is
#     present on the cmdline. This script's `dual-local` mode launches
#     two Wilburs locally, one host + one client, on different UDP ports
#     of 127.0.0.1, polls both result JSONs concurrently, and aggregates.
#
# Modes:
#   - mock           : no game launch. Writes fixture JSONs to Game/.
#                      Exercises orchestrator logic only. Default for CI.
#   - single-process : launches ONE Wilbur locally with -mtrasi-coop-port,
#                      observes the suffixed result JSON. Proves the mod-
#                      side parameterization. Same machine, single process.
#   - dual-local     : launches TWO Wilburs locally — host on -HostPort,
#                      client connecting to 127.0.0.1:-HostPort — polls
#                      both result JSONs concurrently, aggregates results.
#                      The Phase 1.6 end-to-end LAN soak runs here with
#                      -Scenario coop-lan-soak.
#   - dual-machine   : launches THE LOCAL SIDE (specify via -Role host or
#                      -Role client). Caller is responsible for launching
#                      the remote side; orchestrator only waits for its
#                      OWN result JSON. For cross-machine validation.
#
# Usage:
#   pwsh -File tools/run-coop-test.ps1 -Mode mock
#   pwsh -File tools/run-coop-test.ps1 -Mode mock -HostResult fail
#   pwsh -File tools/run-coop-test.ps1 -Mode single-process -HostPort 31415 \
#         -Scenario boot-to-main-menu -Redeploy
#   pwsh -File tools/run-coop-test.ps1 -Mode dual-local -Redeploy \
#         -Scenario coop-lan-soak -TimeoutSec 180
#   pwsh -File tools/run-coop-test.ps1 -Mode dual-machine -Role host \
#         -HostPort 31415 -ClientPort 31416 -Scenario coop-ping
#
# Exit codes:
#   0 = both sides reported "pass" (or the configured success criterion)
#   1 = at least one side reported "fail" or "crash"
#   2 = at least one side hung
#   3 = at least one side timed out
#   4 = launch failure
#   5 = build failure
#   6 = mode/argument error (e.g. dual-machine without -Role)

[CmdletBinding()]
param(
    [ValidateSet("mock","single-process","dual-local","dual-machine")]
    [string]$Mode = "mock",

    # Coop ports. Default values are arbitrary but stable so test fixtures
    # have a known path.
    [int]$HostPort   = 31415,
    [int]$ClientPort = 31416,

    # For dual-machine, identifies WHICH side this invocation is launching.
    # Ignored by mock + single-process.
    [ValidateSet("host","client")]
    [string]$Role = "host",

    # Scenario name (must match a registered scenario in test_harness.cpp).
    [string]$Scenario = "boot-to-main-menu",

    # How long the in-mod watchdog gets before it auto-fails the scenario.
    [int]$TimeoutSec = 90,

    # Outer-process log-stall watchdog.
    [int]$LogStallSec = 20,

    # mock-mode result injection: what fake outcome to write per side.
    [ValidateSet("pass","fail","timeout","crash")]
    [string]$HostResult   = "pass",
    [ValidateSet("pass","fail","timeout","crash")]
    [string]$ClientResult = "pass",

    # For dual-machine mode: peer's hostname/IP. The host side ignores this
    # (it binds 0.0.0.0:HostPort and learns peer addr from first packet).
    # The client side connects to ${RemoteHost}:${HostPort}.
    [string]$RemoteHost = "127.0.0.1",

    [switch]$Redeploy,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

function Log { param([string]$m) if (-not $Quiet) { Write-Host "[run-coop-test] $m" } }

# === Paths ===================================================================

$RepoRoot   = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir    = Join-Path $RepoRoot "Game"
$BuildDir   = Join-Path $RepoRoot "src\mtr-asi\build"
$BuiltAsi   = Join-Path $BuildDir "Release\mtr-asi.asi"
$DeployedAsi= Join-Path $GameDir  "mtr-asi.asi"
$WilburExe  = Join-Path $GameDir  "Wilbur.exe"
$RunStamp   = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$RunDir     = Join-Path $RepoRoot "tools\test-runs\$RunStamp-coop-$Scenario"

function Result-Path([int]$port) {
    if ($port -gt 0) {
        return Join-Path $GameDir "mtr-asi-test-result-$port.json"
    } else {
        return Join-Path $GameDir "mtr-asi-test-result.json"
    }
}

$HostJson   = Result-Path $HostPort
$ClientJson = Result-Path $ClientPort

# === Build + redeploy (shared with real-launch modes) ========================

function Ensure-Build {
    if ($Redeploy) {
        Log "building mtr-asi (Release)..."
        & cmake --build $BuildDir --config Release | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Log "FATAL: build failed (exit=$LASTEXITCODE)"
            exit 5
        }
        if (-not (Test-Path $BuiltAsi)) {
            Log "FATAL: build succeeded but $BuiltAsi missing"
            exit 5
        }
        Log "deploying $(Split-Path $BuiltAsi -Leaf) -> $(Split-Path $DeployedAsi -Leaf)"
        Copy-Item -Path $BuiltAsi -Destination $DeployedAsi -Force
    }
    if (-not (Test-Path $DeployedAsi)) {
        Log "FATAL: $DeployedAsi missing (run with -Redeploy or copy manually)"
        exit 4
    }
}

# === Result JSON parsing =====================================================

function Parse-Result([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    try {
        $raw = Get-Content $path -Raw
        if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
        return ($raw | ConvertFrom-Json)
    } catch {
        Log "WARN: result JSON malformed at $path : $($_.Exception.Message)"
        return $null
    }
}

# Maps a parsed result object to (exitCode, label). Returns a hashtable.
function Result-ExitCode($obj) {
    if (-not $obj) { return @{ Code = 4; Label = "no-json" } }
    switch ($obj.result) {
        "pass"    { return @{ Code = 0; Label = "pass" } }
        "fail"    { return @{ Code = 1; Label = "fail" } }
        "timeout" { return @{ Code = 3; Label = "timeout" } }
        "crash"   { return @{ Code = 1; Label = "crash" } }
        default   { return @{ Code = 1; Label = "unknown:$($obj.result)" } }
    }
}

# === Mock mode ===============================================================

function Write-MockResult([int]$port, [string]$outcome, [string]$detail) {
    $path = Result-Path $port
    $obj = @{
        scenario   = $Scenario
        result     = $outcome
        elapsed_ms = 1234
        frames     = 567
        coop_port  = $port
        detail     = $detail
    }
    ($obj | ConvertTo-Json -Compress) | Set-Content -Path $path -Encoding utf8
    Log "wrote mock result port=$port outcome=$outcome -> $path"
}

function Run-Mock {
    Log "MOCK MODE — no game launch. Fixtures: host=$HostResult client=$ClientResult"
    Remove-Item -Path $HostJson,$ClientJson -Force -ErrorAction SilentlyContinue
    Write-MockResult $HostPort   $HostResult   "mock-host-side"
    Write-MockResult $ClientPort $ClientResult "mock-client-side"

    $hostObj   = Parse-Result $HostJson
    $clientObj = Parse-Result $ClientJson

    Log "host: result=$($hostObj.result) coop_port=$($hostObj.coop_port)"
    Log "client: result=$($clientObj.result) coop_port=$($clientObj.coop_port)"

    # Sanity: ports in the JSON must match what we asked for. Catches a
    # category of regression where the mod stops honoring the cmdline flag.
    if ($hostObj.coop_port -ne $HostPort) {
        Log "FAIL: host JSON coop_port=$($hostObj.coop_port) != $HostPort"
        exit 1
    }
    if ($clientObj.coop_port -ne $ClientPort) {
        Log "FAIL: client JSON coop_port=$($clientObj.coop_port) != $ClientPort"
        exit 1
    }

    $h = Result-ExitCode $hostObj
    $c = Result-ExitCode $clientObj
    # Aggregate: worst-of-two by exit code, with pass-pass = 0.
    $agg = [Math]::Max($h.Code, $c.Code)
    Log "aggregate: host=$($h.Label) client=$($c.Label) -> exit=$agg"
    exit $agg
}

# === Real-launch helpers (shared by single-process + dual-machine) ===========

function Kill-AllWilbur {
    $existing = Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue
    if ($existing) {
        Log "previous Wilbur.exe alive (pids: $($existing.ProcessId -join ', ')); killing"
        foreach ($p in $existing) {
            $null = Invoke-CimMethod -InputObject $p -MethodName Terminate
        }
        Start-Sleep -Milliseconds 500
        if (Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue) {
            Log "FATAL: Wilbur.exe still alive after Terminate"
            exit 4
        }
    }
}

# Launch a single Wilbur. NetRole:
#   "" (empty)  = no networking flag (single-process scenarios)
#   "host=PORT" = -mtrasi-coop-host PORT
#   "connect=HOST:PORT" = -mtrasi-coop-connect HOST:PORT
# WindowMode: "fullscreen" or "windowed-1024x768". dual-local must use
# windowed so two Wilburs can coexist on the same primary screen.
function Launch-Wilbur([int]$port, [string]$netRole = "", [string]$windowMode = "fullscreen") {
    $jsonPath = Result-Path $port
    Remove-Item -Path $jsonPath -Force -ErrorAction SilentlyContinue

    Add-Type -AssemblyName System.Windows.Forms
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $cmdArgs = @(
        "-launchit"
    )
    if ($windowMode -eq "windowed-1024x768") {
        # Two side-by-side Wilburs on one screen — windowed at 1024x768
        # (smaller than half a typical 2560x1440 screen so both fit).
        $cmdArgs += "-dxresolution=1024x768"
    } else {
        $cmdArgs += @(
            "-dxfullscreen"
            "-dxadapter=0"
            "-dxresolution=$($bounds.Width)x$($bounds.Height)"
        )
    }
    $cmdArgs += @(
        "-mtrasi-test=$Scenario"
        "-mtrasi-test-timeout=$TimeoutSec"
        "-mtrasi-coop-port=$port"
    )
    if ($windowMode -eq "windowed-1024x768") {
        # Opt out of cmdline_hook's native-dims rewrite AND windowmode's
        # borderless-monitor resize. Required for dual-local — two Wilburs
        # must coexist as true small windows on one screen rather than
        # fighting for fullscreen-borderless focus.
        $cmdArgs += "-mtrasi-keep-dxresolution"
    }
    if ($netRole.StartsWith("host=")) {
        $hostPortArg = $netRole.Substring(5)
        $cmdArgs += "-mtrasi-coop-host"
        $cmdArgs += $hostPortArg
    } elseif ($netRole.StartsWith("connect=")) {
        $connArg = $netRole.Substring(8)
        $cmdArgs += "-mtrasi-coop-connect"
        $cmdArgs += $connArg
    }
    Log "launching Wilbur.exe scenario=$Scenario port=$port netRole='$netRole' window=$windowMode timeout=${TimeoutSec}s"
    Log "argv: $($cmdArgs -join ' ')"

    return Start-Process -FilePath $WilburExe `
                         -WorkingDirectory $GameDir `
                         -ArgumentList $cmdArgs `
                         -PassThru
}

function Wait-ForResult([int]$port, $proc) {
    $jsonPath = Result-Path $port
    # log.cpp writes mtr-asi-<port>.log when -mtrasi-coop-port=N>0 is on the
    # cmdline (always true for single-process + dual-machine since
    # Launch-Wilbur always passes it). Stall watchdog must watch the same
    # path or it sees zero growth on a healthy process and false-triggers.
    $modLog = if ($port -gt 0) {
        Join-Path $GameDir "mtr-asi-$port.log"
    } else {
        Join-Path $GameDir "mtr-asi.log"
    }
    $started  = Get-Date
    $lastLogSize = 0
    $lastLogGrow = $started
    $pollMs = 250

    while ($true) {
        Start-Sleep -Milliseconds $pollMs

        if (Test-Path $jsonPath) {
            Log "result JSON appeared at $(((Get-Date) - $started).TotalSeconds.ToString("0.0"))s"
            $waitFor = (Get-Date).AddSeconds(8)
            while ((Get-Date) -lt $waitFor -and -not $proc.HasExited) {
                Start-Sleep -Milliseconds 100
            }
            if (-not $proc.HasExited) {
                Log "process still alive 8s after result JSON; force-stopping"
                $null = Invoke-CimMethod -InputObject (
                    Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.Id)"
                ) -MethodName Terminate
            }
            return @{ Status = "ok" }
        }

        if ($proc.HasExited) {
            Log "process exited without result JSON (exit=$($proc.ExitCode))"
            return @{ Status = "no-json" }
        }

        if (Test-Path $modLog) {
            $sz = (Get-Item $modLog).Length
            if ($sz -gt $lastLogSize) {
                $lastLogSize = $sz
                $lastLogGrow = Get-Date
            }
        }
        $stallSec = ((Get-Date) - $lastLogGrow).TotalSeconds
        if ($stallSec -gt $LogStallSec -and -not (Test-Path $jsonPath)) {
            Log "log stalled for ${stallSec}s; assuming hang, force-stopping"
            $null = Invoke-CimMethod -InputObject (
                Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.Id)"
            ) -MethodName Terminate
            return @{ Status = "hang" }
        }
        $totalSec = ((Get-Date) - $started).TotalSeconds
        if ($totalSec -gt $TimeoutSec) {
            Log "hard timeout after ${totalSec}s; force-stopping"
            $null = Invoke-CimMethod -InputObject (
                Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.Id)"
            ) -MethodName Terminate
            return @{ Status = "timeout" }
        }
    }
}

# === Single-process mode =====================================================
#
# Verifies the mod-side -mtrasi-coop-port wiring against a real game. Same
# machine, single Wilbur. The Disney mutex isn't an issue because we never
# launch a second instance. Useful as a smoke test in CI before committing
# to dual-machine work.

function Run-SingleProcess {
    if (-not (Test-Path $WilburExe)) {
        Log "FATAL: $WilburExe not found"
        exit 4
    }
    Ensure-Build

    $port = $HostPort
    Log "SINGLE-PROCESS MODE — launching host only on port $port"
    Kill-AllWilbur
    Remove-Item -Path (Join-Path $GameDir "mtr-asi.log") -Force -ErrorAction SilentlyContinue
    $proc = Launch-Wilbur $port
    if (-not $proc) {
        Log "FATAL: Start-Process returned null"
        exit 4
    }
    Log "Wilbur.exe pid=$($proc.Id)"

    $waitInfo = Wait-ForResult $port $proc
    Start-Sleep -Milliseconds 500

    $obj = Parse-Result (Result-Path $port)
    if (-not $obj) {
        switch ($waitInfo.Status) {
            "hang"    { exit 2 }
            "timeout" { exit 3 }
            default   { exit 4 }
        }
    }
    Log "host: result=$($obj.result) elapsed_ms=$($obj.elapsed_ms) frames=$($obj.frames) coop_port=$($obj.coop_port)"
    if ($obj.coop_port -ne $port) {
        Log "FAIL: result JSON coop_port=$($obj.coop_port) != $port  (mod-side wiring regression)"
        exit 1
    }
    $r = Result-ExitCode $obj
    Log "single-process: $($r.Label) -> exit=$($r.Code)"
    exit $r.Code
}

# === Dual-machine mode =======================================================
#
# Launches the LOCAL side only — the other machine must be started
# separately by a human (or by a remote-invoke wrapper we don't have yet).
# This invocation polls its own port's JSON and reports.
#
# Future work for Phase 1+: a remote-orchestrator hook (PSRemoting, SSH, or
# a small TCP rendezvous server) that triggers the second invocation
# remotely with synchronized start. Out of scope for the Phase 0D skeleton.

function Run-DualMachine {
    if (-not (Test-Path $WilburExe)) {
        Log "FATAL: $WilburExe not found"
        exit 4
    }
    Ensure-Build

    $port = if ($Role -eq "host") { $HostPort } else { $ClientPort }
    $netRole = if ($Role -eq "host") { "host=$HostPort" } else { "connect=$RemoteHost`:$HostPort" }
    Log "DUAL-MACHINE MODE — role=$Role local port=$port netRole=$netRole"
    Log "Reminder: the OTHER side must be launched on a separate machine."
    Kill-AllWilbur
    Remove-Item -Path (Join-Path $GameDir "mtr-asi.log") -Force -ErrorAction SilentlyContinue
    $proc = Launch-Wilbur $port $netRole
    if (-not $proc) {
        Log "FATAL: Start-Process returned null"
        exit 4
    }
    Log "Wilbur.exe pid=$($proc.Id)"

    $waitInfo = Wait-ForResult $port $proc
    Start-Sleep -Milliseconds 500

    $obj = Parse-Result (Result-Path $port)
    if (-not $obj) {
        switch ($waitInfo.Status) {
            "hang"    { exit 2 }
            "timeout" { exit 3 }
            default   { exit 4 }
        }
    }
    Log "${Role}: result=$($obj.result) elapsed_ms=$($obj.elapsed_ms) frames=$($obj.frames) coop_port=$($obj.coop_port)"
    $r = Result-ExitCode $obj
    Log "local side ($Role): $($r.Label) -> exit=$($r.Code)"
    exit $r.Code
}

# === Dual-local mode =========================================================
#
# Launch two Wilburs on the same machine — host listening on -HostPort,
# client connecting to 127.0.0.1:$HostPort. The mod's coop_dual_launch
# hook renames the Disney mutex per-PID whenever either networking flag
# is present, so the second launch is no longer blocked. Each Wilbur
# writes to its own port-suffixed result JSON AND its own port-suffixed
# log file (log.cpp scans -mtrasi-coop-port= at init). Both processes
# poll concurrently; aggregate result requires both to pass AND both to
# report fires_p2 == 0 (the Phase 1.6 invariant: engine must never poll
# dev_p2 even with a real network peer present).
#
# Window layout: both Wilburs run windowed 1024x768. They both end up
# placed at (0,0) by the engine; the second window stacks on top of the
# first. For autonomous testing this is fine — DirectInput is per-process
# and the test harness doesn't depend on visible focus.

function Wait-ForResults($hostInfo, $clientInfo) {
    $modLogHost   = Join-Path $GameDir "mtr-asi-$HostPort.log"
    $modLogClient = Join-Path $GameDir "mtr-asi-$ClientPort.log"
    $started      = Get-Date
    $lastLogSize  = @{ host = 0; client = 0 }
    $lastLogGrow  = @{ host = $started; client = $started }
    $pollMs       = 250
    $hostDone     = $false
    $clientDone   = $false
    $statusHost   = "running"
    $statusClient = "running"

    while (-not ($hostDone -and $clientDone)) {
        Start-Sleep -Milliseconds $pollMs

        # Check JSONs.
        if (-not $hostDone -and (Test-Path $hostInfo.JsonPath)) {
            $hostDone   = $true
            $statusHost = "ok"
            Log "host result JSON appeared at $(((Get-Date) - $started).TotalSeconds.ToString('0.0'))s"
        }
        if (-not $clientDone -and (Test-Path $clientInfo.JsonPath)) {
            $clientDone   = $true
            $statusClient = "ok"
            Log "client result JSON appeared at $(((Get-Date) - $started).TotalSeconds.ToString('0.0'))s"
        }

        # Check process liveness.
        if (-not $hostDone -and $hostInfo.Proc.HasExited) {
            $hostDone   = $true
            $statusHost = "no-json"
            Log "host process exited without result JSON (exit=$($hostInfo.Proc.ExitCode))"
        }
        if (-not $clientDone -and $clientInfo.Proc.HasExited) {
            $clientDone   = $true
            $statusClient = "no-json"
            Log "client process exited without result JSON (exit=$($clientInfo.Proc.ExitCode))"
        }

        # Check log stalls per process.
        if (-not $hostDone -and (Test-Path $modLogHost)) {
            $sz = (Get-Item $modLogHost).Length
            if ($sz -gt $lastLogSize.host) {
                $lastLogSize.host = $sz
                $lastLogGrow.host = Get-Date
            }
        }
        if (-not $clientDone -and (Test-Path $modLogClient)) {
            $sz = (Get-Item $modLogClient).Length
            if ($sz -gt $lastLogSize.client) {
                $lastLogSize.client = $sz
                $lastLogGrow.client = Get-Date
            }
        }
        if (-not $hostDone) {
            $stall = ((Get-Date) - $lastLogGrow.host).TotalSeconds
            if ($stall -gt $LogStallSec) {
                Log "host log stalled for ${stall}s; force-stopping"
                $null = Invoke-CimMethod -InputObject (
                    Get-CimInstance Win32_Process -Filter "ProcessId=$($hostInfo.Proc.Id)"
                ) -MethodName Terminate
                $hostDone   = $true
                $statusHost = "hang"
            }
        }
        if (-not $clientDone) {
            $stall = ((Get-Date) - $lastLogGrow.client).TotalSeconds
            if ($stall -gt $LogStallSec) {
                Log "client log stalled for ${stall}s; force-stopping"
                $null = Invoke-CimMethod -InputObject (
                    Get-CimInstance Win32_Process -Filter "ProcessId=$($clientInfo.Proc.Id)"
                ) -MethodName Terminate
                $clientDone   = $true
                $statusClient = "hang"
            }
        }

        # Hard timeout on either side.
        $totalSec = ((Get-Date) - $started).TotalSeconds
        if ($totalSec -gt $TimeoutSec) {
            Log "outer hard timeout after ${totalSec}s; force-stopping survivors"
            if (-not $hostDone) {
                $null = Invoke-CimMethod -InputObject (
                    Get-CimInstance Win32_Process -Filter "ProcessId=$($hostInfo.Proc.Id)"
                ) -MethodName Terminate
                $hostDone   = $true
                $statusHost = "timeout"
            }
            if (-not $clientDone) {
                $null = Invoke-CimMethod -InputObject (
                    Get-CimInstance Win32_Process -Filter "ProcessId=$($clientInfo.Proc.Id)"
                ) -MethodName Terminate
                $clientDone   = $true
                $statusClient = "timeout"
            }
        }
    }

    # Both sides done. Give 2s for any post-JSON cleanup the test_harness
    # epilogue might be doing (it normally TerminateProcess's itself).
    Start-Sleep -Milliseconds 2000
    Kill-AllWilbur

    return @{
        HostStatus   = $statusHost
        ClientStatus = $statusClient
    }
}

function Run-DualLocal {
    if (-not (Test-Path $WilburExe)) {
        Log "FATAL: $WilburExe not found"
        exit 4
    }
    Ensure-Build

    Log "DUAL-LOCAL MODE — host=:$HostPort client=connect 127.0.0.1:$HostPort"
    Kill-AllWilbur

    # Clean prior artifacts so we know we are reading fresh ones.
    Remove-Item -Path `
        (Join-Path $GameDir "mtr-asi-$HostPort.log"), `
        (Join-Path $GameDir "mtr-asi-$ClientPort.log"), `
        (Join-Path $GameDir "mtr-asi-test-result-$HostPort.json"), `
        (Join-Path $GameDir "mtr-asi-test-result-$ClientPort.json"), `
        (Join-Path $GameDir "mtr-asi.log") `
        -Force -ErrorAction SilentlyContinue

    # Launch host first; give it 2s to bind the UDP socket. Client connect()
    # in UDP doesn't handshake, but if the host hasn't bound its socket the
    # first few client datagrams go nowhere. The MVP NetSession will retry
    # naturally as MtrPlayerManager keeps emitting pose snapshots, so this
    # gap is forgiving — 2s is generous.
    $hostProc = Launch-Wilbur $HostPort "host=$HostPort" "windowed-1024x768"
    if (-not $hostProc) {
        Log "FATAL: failed to launch host"
        exit 4
    }
    Log "host Wilbur.exe pid=$($hostProc.Id) — waiting 4s for window + socket bind"
    Start-Sleep -Milliseconds 4000

    # Move host window to top-left so the client window has space to its right.
    Add-Type -Namespace Win32 -Name Win32API -MemberDefinition '
        [DllImport("user32.dll", SetLastError=true)]
        public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    ' -ErrorAction SilentlyContinue

    # Wait for the process's main window to appear, then move it. .NET Process
    # caches MainWindowHandle on first access — refresh via Refresh() before
    # reading. Polls up to ~6s.
    function Move-WilburWindow($procId, $x, $y) {
        $proc = $null
        for ($i = 0; $i -lt 30; $i++) {
            $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
            if (-not $proc) {
                Log "warn: pid=$procId is gone before window appeared"
                return $false
            }
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 200
        }
        if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
            Log "warn: pid=$procId MainWindowHandle never appeared"
            return $false
        }
        # SWP_NOZORDER=4 | SWP_NOACTIVATE=0x10 — move + resize, don't steal focus
        $null = [Win32.Win32API]::SetWindowPos($proc.MainWindowHandle, [IntPtr]::Zero, $x, $y, 1024, 768, 0x14)
        # SW_SHOWNOACTIVATE=4 — ensure window is shown without stealing focus.
        # Required because if the window was hidden behind another, Windows may
        # throttle its render thread until it's visible.
        $null = [Win32.Win32API]::ShowWindow($proc.MainWindowHandle, 4)
        Log "moved pid=$procId hwnd=$($proc.MainWindowHandle.ToString('X8')) to ($x,$y)"
        return $true
    }
    $null = Move-WilburWindow $hostProc.Id 0 0

    $clientProc = Launch-Wilbur $ClientPort "connect=127.0.0.1:$HostPort" "windowed-1024x768"
    if (-not $clientProc) {
        Log "FATAL: failed to launch client; killing host"
        $null = Invoke-CimMethod -InputObject (
            Get-CimInstance Win32_Process -Filter "ProcessId=$($hostProc.Id)"
        ) -MethodName Terminate
        exit 4
    }
    Log "client Wilbur.exe pid=$($clientProc.Id) — waiting 4s for window"
    Start-Sleep -Milliseconds 4000
    # Move client window to right of host (host at 0,0 sized 1024x768).
    $null = Move-WilburWindow $clientProc.Id 1030 0

    $hostInfo   = @{ Proc = $hostProc;   JsonPath = $HostJson   }
    $clientInfo = @{ Proc = $clientProc; JsonPath = $ClientJson }
    $wait = Wait-ForResults $hostInfo $clientInfo

    $hostObj   = Parse-Result $HostJson
    $clientObj = Parse-Result $ClientJson

    if ($hostObj) {
        Log ("host:   result={0} elapsed_ms={1} frames={2} peer_known={3} has_remote={4} fires_p2={5}" `
            -f $hostObj.result, $hostObj.elapsed_ms, $hostObj.frames,
               $hostObj.peer_known, $hostObj.has_remote, $hostObj.fires_p2)
        Log ("host detail:   {0}" -f $hostObj.detail)
    } else {
        Log "host: NO RESULT JSON ($($wait.HostStatus))"
    }
    if ($clientObj) {
        Log ("client: result={0} elapsed_ms={1} frames={2} peer_known={3} has_remote={4} fires_p2={5}" `
            -f $clientObj.result, $clientObj.elapsed_ms, $clientObj.frames,
               $clientObj.peer_known, $clientObj.has_remote, $clientObj.fires_p2)
        Log ("client detail: {0}" -f $clientObj.detail)
    } else {
        Log "client: NO RESULT JSON ($($wait.ClientStatus))"
    }

    # Archive: copy both logs + both result JSONs + screenshots if any to RunDir.
    New-Item -ItemType Directory -Path $RunDir -Force | Out-Null
    foreach ($f in @(
        (Join-Path $GameDir "mtr-asi-$HostPort.log"),
        (Join-Path $GameDir "mtr-asi-$ClientPort.log"),
        $HostJson, $ClientJson
    )) {
        if (Test-Path $f) {
            Copy-Item -Path $f -Destination $RunDir -Force
        }
    }
    $shotDir = Join-Path $GameDir "screenshots"
    if (Test-Path $shotDir) {
        $shots = Get-ChildItem -Path $shotDir -Filter "*.bmp" -ErrorAction SilentlyContinue
        if ($shots) {
            $shotDest = Join-Path $RunDir "screenshots"
            New-Item -ItemType Directory -Path $shotDest -Force | Out-Null
            foreach ($s in $shots) { Move-Item -Path $s.FullName -Destination $shotDest -Force }
            Log "archived $($shots.Count) screenshot(s) to $shotDest"
        }
    }
    Log "artifacts archived to $RunDir"

    # Aggregate.
    if (-not $hostObj -or -not $clientObj) {
        Log "FAIL: at least one side missing result JSON"
        exit 1
    }
    $hostExit   = (Result-ExitCode $hostObj).Code
    $clientExit = (Result-ExitCode $clientObj).Code
    if ($hostExit -ne 0 -or $clientExit -ne 0) {
        Log "FAIL: aggregate=fail (host=$($hostObj.result) client=$($clientObj.result))"
        exit [Math]::Max($hostExit, $clientExit)
    }
    if ([int]$hostObj.fires_p2 -ne 0 -or [int]$clientObj.fires_p2 -ne 0) {
        Log "FAIL: Phase 1.6 invariant broken — fires_p2 must be 0 on both sides"
        Log "      host fires_p2=$($hostObj.fires_p2) client fires_p2=$($clientObj.fires_p2)"
        exit 1
    }
    Log "PASS: both sides pass; fires_p2=0 on both; peer_known=$($hostObj.peer_known),$($clientObj.peer_known) has_remote=$($hostObj.has_remote),$($clientObj.has_remote)"
    exit 0
}

# === Dispatch ================================================================

switch ($Mode) {
    "mock"           { Run-Mock }
    "single-process" { Run-SingleProcess }
    "dual-local"     { Run-DualLocal }
    "dual-machine"   { Run-DualMachine }
    default {
        Log "FATAL: unknown mode '$Mode'"
        exit 6
    }
}
