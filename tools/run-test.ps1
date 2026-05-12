#!/usr/bin/env pwsh
# Autonomous test loop for mtr-asi.
#
# Wraps Wilbur.exe launch + log polling + clean shutdown so an AI assistant
# (or CI) can iterate "build → launch → test → exit" without user input.
#
# Pairs with the in-mod test_harness module. The mod runs a scenario
# state-machine driven by the -mtrasi-test=<name> cmdline flag and writes
# Game/mtr-asi-test-result.json on terminal. This script:
#   1. Optionally rebuilds + redeploys mtr-asi.asi.
#   2. Cleans previous run artifacts (log + result JSON).
#   3. Launches Wilbur.exe direct (NOT Launcher.exe — Disney's MFC shim adds
#      no DRM, just resolution args, which the mod's cmdline_hook synthesizes
#      anyway).
#   4. Polls every 250ms for the result JSON. Force-stops only on watchdog
#      conditions (log silent >LogStallSec, total >TimeoutSec).
#   5. Archives all artifacts to tools/test-runs/<UTC-stamp>/.
#
# Usage:
#   pwsh -File tools/run-test.ps1                                    # default scenario
#   pwsh -File tools/run-test.ps1 -Scenario boot-to-main-menu -Redeploy
#   pwsh -File tools/run-test.ps1 -Scenario freecam-toggle -TimeoutSec 120
#
# Exit codes: 0=pass 1=fail 2=hang 3=timeout 4=launch-failure 5=build-failure
#
# Architecture: research/findings/autonomous-test-loop-design.md.

[CmdletBinding()]
param(
    [string]$Scenario     = "boot-to-main-menu",
    [string[]]$ExtraArgs  = @(),
    [int]   $TimeoutSec   = 90,
    [int]   $LogStallSec  = 20,
    [switch]$Redeploy,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$script:Started        = Get-Date

function Log { param([string]$m) if (-not $Quiet) { Write-Host "[run-test] $m" } }

# === Paths ===================================================================

$RepoRoot   = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir    = Join-Path $RepoRoot "Game"
$BuildDir   = Join-Path $RepoRoot "src\mtr-asi\build"
$BuiltAsi   = Join-Path $BuildDir "Release\mtr-asi.asi"
$DeployedAsi= Join-Path $GameDir  "mtr-asi.asi"
$WilburExe  = Join-Path $GameDir  "Wilbur.exe"
$ModLog     = Join-Path $GameDir  "mtr-asi.log"
$ResultJson = Join-Path $GameDir  "mtr-asi-test-result.json"
$RunStamp   = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$RunDir     = Join-Path $RepoRoot "tools\test-runs\$RunStamp-$Scenario"

# === Pre-flight ==============================================================

if (-not (Test-Path $WilburExe)) {
    Log "FATAL: $WilburExe not found"
    exit 4
}

# Ensure no previous Wilbur is alive — otherwise the global mutex
# Global\Disney_s_Meet_The_Robinsons blocks our launch.
$existing = Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue
if ($existing) {
    Log "previous Wilbur.exe alive (pids: $($existing.ProcessId -join ', ')); killing via WMI Terminate"
    foreach ($p in $existing) {
        $null = Invoke-CimMethod -InputObject $p -MethodName Terminate
    }
    Start-Sleep -Milliseconds 500
    if (Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue) {
        Log "FATAL: Wilbur.exe still alive after WMI Terminate; manual intervention needed"
        exit 4
    }
}

# === Build + redeploy ========================================================

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
    # Also deploy the PDB so dbghelp's SymFromAddr in crash_handler can
    # resolve our own ASI symbols at fault time. The PDB path embedded in
    # the .asi is the absolute build-dir path, so on-disk dbghelp lookup
    # will already succeed on this dev machine — but having a copy next to
    # the .asi keeps things working if the build dir is later moved/wiped.
    $BuiltPdb    = Join-Path $BuildDir "Release\mtr-asi.pdb"
    $DeployedPdb = Join-Path $GameDir  "mtr-asi.pdb"
    if (Test-Path $BuiltPdb) {
        Copy-Item -Path $BuiltPdb -Destination $DeployedPdb -Force
    }
}

if (-not (Test-Path $DeployedAsi)) {
    Log "FATAL: $DeployedAsi missing (run with -Redeploy or copy manually)"
    exit 4
}

$asiSize = (Get-Item $DeployedAsi).Length
Log "deployed mtr-asi.asi = $asiSize bytes"

# Clean per-run artifacts.
Remove-Item -Path $ModLog,$ResultJson -Force -ErrorAction SilentlyContinue
# Move existing screenshots to a `_pre-run` archive dir so this run's
# screenshots stand alone in Game/screenshots/.
$shotsDirPre = Join-Path $GameDir "screenshots"
if (Test-Path $shotsDirPre) {
    $existing = Get-ChildItem -Path $shotsDirPre -Filter "mtr_*.bmp" -ErrorAction SilentlyContinue
    if ($existing) {
        $stash = Join-Path $shotsDirPre "_pre-run-$RunStamp"
        New-Item -ItemType Directory -Force -Path $stash | Out-Null
        foreach ($e in $existing) {
            Move-Item -Path $e.FullName -Destination $stash -Force -ErrorAction SilentlyContinue
        }
    }
}

# === Launch Wilbur ===========================================================

# Native primary monitor dims for -dxresolution. The mod's cmdline_hook would
# rewrite anything to native anyway, but supplying native upfront skips one
# log line and matches what the user's normal Launcher.exe does.
Add-Type -AssemblyName System.Windows.Forms
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$nativeW = $bounds.Width
$nativeH = $bounds.Height

$cmdArgs = @(
    "-launchit"
    "-dxfullscreen"
    "-dxadapter=0"
    "-dxresolution=${nativeW}x${nativeH}"
    "-mtrasi-test=$Scenario"
    "-mtrasi-test-timeout=$TimeoutSec"
)
foreach ($extra in $ExtraArgs) { $cmdArgs += $extra }

Log "launching Wilbur.exe with scenario=$Scenario timeout=${TimeoutSec}s"
Log "argv: $($cmdArgs -join ' ')"

$proc = Start-Process -FilePath $WilburExe `
                      -WorkingDirectory $GameDir `
                      -ArgumentList $cmdArgs `
                      -PassThru
if (-not $proc) {
    Log "FATAL: Start-Process returned null"
    exit 4
}
Log "Wilbur.exe pid=$($proc.Id)"

# Title-screen dismissal happens in-mod (test_harness.cpp::tick) — the
# scenario sends synthetic keybd_event keypresses every ~1s while
# screen_push hasn't captured yet. PowerShell SendKeys is unreliable for
# this because of Windows focus-steal protection (we'd need
# AllowSetForegroundWindow with a chain of foreground-app permissions
# from the calling process tree). In-process keybd_event has no such
# gate.

# === Watchdog loop ===========================================================

$pollMs       = 250
$started      = Get-Date
$lastLogSize  = 0
$lastLogGrow  = $started

while ($true) {
    Start-Sleep -Milliseconds $pollMs

    # Result JSON appeared → terminal.
    if (Test-Path $ResultJson) {
        Log "result JSON appeared at $(((Get-Date) - $started).TotalSeconds.ToString("0.0"))s"
        # Wait briefly for the mod's PostQuitMessage path to land cleanly.
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
        break
    }

    # Process died on its own without leaving a JSON → crash.
    if ($proc.HasExited) {
        Log "process exited without result JSON (exit=$($proc.ExitCode))"
        break
    }

    # Log-stall watchdog: ASI failed to load or game hung.
    if (Test-Path $ModLog) {
        $sz = (Get-Item $ModLog).Length
        if ($sz -gt $lastLogSize) {
            $lastLogSize = $sz
            $lastLogGrow = Get-Date
        }
    }
    $stallSec = ((Get-Date) - $lastLogGrow).TotalSeconds
    if ($stallSec -gt $LogStallSec -and -not (Test-Path $ResultJson)) {
        Log "log stalled for ${stallSec}s; assuming hang, force-stopping"
        $null = Invoke-CimMethod -InputObject (
            Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.Id)"
        ) -MethodName Terminate
        $script:exitCode = 2
        break
    }

    # Hard timeout.
    $totalSec = ((Get-Date) - $started).TotalSeconds
    if ($totalSec -gt $TimeoutSec) {
        Log "hard timeout after ${totalSec}s; force-stopping"
        $null = Invoke-CimMethod -InputObject (
            Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.Id)"
        ) -MethodName Terminate
        $script:exitCode = 3
        break
    }
}

# Give the OS a moment to release the file handle on mtr-asi.asi.
Start-Sleep -Milliseconds 500

# === Result reporting ========================================================

if (-not $script:exitCode) { $script:exitCode = 0 }

$result = $null
if (Test-Path $ResultJson) {
    try {
        $result = Get-Content $ResultJson -Raw | ConvertFrom-Json
    } catch {
        Log "WARN: result JSON malformed: $($_.Exception.Message)"
    }
}

if ($result) {
    Log "scenario=$($result.scenario) result=$($result.result) elapsed_ms=$($result.elapsed_ms) frames=$($result.frames)"
    Log "detail: $($result.detail)"
    if ($result.result -eq "pass")        { $script:exitCode = 0 }
    elseif ($result.result -eq "fail")    { $script:exitCode = 1 }
    elseif ($result.result -eq "timeout") { $script:exitCode = 3 }
    elseif ($result.result -eq "crash")   { $script:exitCode = 1; Log "CRASH DETECTED — see [CRASH] log line + .dmp file in archive" }
    else                                  { $script:exitCode = 1 }
} elseif (-not $script:exitCode) {
    Log "no result JSON written; treating as launch failure"
    $script:exitCode = 4
}

# === Archive run artifacts ===================================================

New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
foreach ($f in @($ModLog, $ResultJson, (Join-Path $GameDir "Wilbur_d3d9.log"))) {
    if (Test-Path $f) { Copy-Item -Path $f -Destination $RunDir -Force }
}
# Archive screenshots taken during this run. test_harness fires periodic
# captures every ~5s; F12 also triggers manual ones. We move (not copy)
# so the next run starts with a clean screenshots dir.
$shotsDir = Join-Path $GameDir "screenshots"
if (Test-Path $shotsDir) {
    $shots = Get-ChildItem -Path $shotsDir -Filter "mtr_*.bmp" -ErrorAction SilentlyContinue
    if ($shots) {
        $runShots = Join-Path $RunDir "screenshots"
        New-Item -ItemType Directory -Force -Path $runShots | Out-Null
        foreach ($s in $shots) {
            Move-Item -Path $s.FullName -Destination $runShots -Force
        }
        Log "archived $($shots.Count) screenshot(s) to $runShots"

        # Convert giant raw BMPs to small viewable PNG thumbnails. Saves
        # ~22 MB per run on disk and produces images that fit in chat / web
        # tools without resampling. Keeps the originals; pass -DeleteBmps
        # to the script if you ever want to drop them.
        $thumbScript = Join-Path $PSScriptRoot "bmp-to-png-thumb.ps1"
        if (Test-Path $thumbScript) {
            try {
                & pwsh -NoProfile -File $thumbScript -Directory $runShots -MaxWidth 1024
            } catch {
                Log "WARN: bmp-to-png-thumb failed: $($_.Exception.Message)"
            }
        }
    }
}
# Archive any crash dumps the SUEF wrote (they live next to mtr-asi.asi
# in Game/, not in the run-test working dir).
$dumpFiles = Get-ChildItem -Path $GameDir -Filter "mtr-asi-crash-*.dmp" -ErrorAction SilentlyContinue
if ($dumpFiles) {
    foreach ($d in $dumpFiles) {
        Move-Item -Path $d.FullName -Destination $RunDir -Force
        Log "archived crash dump: $($d.Name)"
    }
}

# Per-scenario post-processing: overlay-phase1-verify runs the offline
# math validator against the archived log. Validator overrides $exitCode
# only on failure (a pass-from-harness can become fail-from-validator if
# the projection math diverged; a fail-from-harness stays fail).
if ($Scenario -eq "overlay-phase1-verify" -and $script:exitCode -eq 0) {
    $validatorPath = Join-Path $PSScriptRoot "validate-overlay-frames.ps1"
    $archivedLog   = Join-Path $RunDir "mtr-asi.log"
    if ((Test-Path $validatorPath) -and (Test-Path $archivedLog)) {
        Log "running offline math validator: $validatorPath"
        & pwsh -NoProfile -File $validatorPath -LogPath $archivedLog
        $vCode = $LASTEXITCODE
        Log "validator exit code: $vCode"
        if ($vCode -ne 0) {
            $script:exitCode = 1
            Log "validator FAILED — overriding harness pass to fail"
        }
    } else {
        Log "validator or archived log missing; skipping post-processing"
    }
}

"argv: $($cmdArgs -join ' ')`nexit_code: $script:exitCode`nstarted_utc: $RunStamp" | `
    Out-File (Join-Path $RunDir "harness-summary.txt") -Encoding utf8

Log "artifacts archived to $RunDir"
Log "exit code: $script:exitCode"
exit $script:exitCode
