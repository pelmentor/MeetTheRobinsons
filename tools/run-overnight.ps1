#requires -version 5
<#
.SYNOPSIS
  Overnight autonomous-work orchestrator. Runs a list of scenarios via
  run-test.ps1, collects exit codes, writes a top-level summary JSON.

.DESCRIPTION
  Wraps run-test.ps1 in a sequential loop. Each scenario gets its own
  per-run-test.ps1 timeout. The outer script enforces a wall-clock cap
  (kOuterTimeoutSec) per scenario in case run-test.ps1 itself hangs —
  defense-in-depth on top of the in-mod + log-stall + hard-timeout
  watchdogs already in run-test.ps1.

  On any scenario crash (result=crash from the SUEF) or non-zero exit,
  the orchestrator logs but continues to the next scenario. The next
  morning's review reads the summary JSON to find which scenarios
  failed and where to look for evidence (.dmp files + archived logs are
  already in tools/test-runs/<UTC-stamp>-<scenario>/).

.PARAMETER Scenarios
  Comma-separated scenario list. Defaults to a known-good shortlist.

.PARAMETER ScenarioTimeoutSec
  Per-scenario timeout passed to run-test.ps1. Default 90.

.PARAMETER Redeploy
  Pass -Redeploy to run-test.ps1 (rebuild + deploy mtr-asi.asi). Default
  on, since overnight runs are typically post-code-change.

.NOTES
  Exit 0 if all scenarios pass; 1 if any fail; 2 if the orchestrator
  itself hits its own watchdog. Designed to be the single entry point
  for autonomous overnight runs.
#>
param(
    [string]$Scenarios = "boot-to-main-menu,overlay-phase1-verify",
    [int]$ScenarioTimeoutSec = 90,
    [switch]$Redeploy = $true
)

$ErrorActionPreference = 'Stop'
$RepoRoot   = Resolve-Path (Join-Path $PSScriptRoot "..")
$RunTest    = Join-Path $PSScriptRoot "run-test.ps1"
$RunStamp   = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$SummaryDir = Join-Path $RepoRoot "tools/overnight-runs"
New-Item -ItemType Directory -Force -Path $SummaryDir | Out-Null
$SummaryPath = Join-Path $SummaryDir "overnight-${RunStamp}.json"

# Per-scenario hard ceiling (orchestrator-level watchdog). 30s headroom
# above the per-scenario internal timeout so we don't false-positive.
$kOuterTimeoutSec = $ScenarioTimeoutSec + 30

$scenarioList = $Scenarios -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$results = @()
$startedAt = Get-Date

Write-Host "[overnight] starting $($scenarioList.Count) scenarios at $RunStamp"
Write-Host "[overnight] scenario list: $($scenarioList -join ', ')"
Write-Host "[overnight] per-scenario timeout: ${ScenarioTimeoutSec}s + ${kOuterTimeoutSec}s outer watchdog"
Write-Host ""

foreach ($scn in $scenarioList) {
    Write-Host "================================================================"
    Write-Host "[overnight] scenario: $scn"
    Write-Host "================================================================"

    $scnStart = Get-Date
    $args = @(
        "-NoProfile", "-File", $RunTest,
        "-Scenario", $scn,
        "-TimeoutSec", $ScenarioTimeoutSec
    )
    if ($Redeploy) { $args += "-Redeploy" }

    $proc = Start-Process -FilePath "pwsh" -ArgumentList $args `
        -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $env:TEMP "overnight-${scn}-stdout.log") `
        -RedirectStandardError (Join-Path $env:TEMP "overnight-${scn}-stderr.log")

    $hung = $false
    if (-not $proc.WaitForExit($kOuterTimeoutSec * 1000)) {
        Write-Host "[overnight] OUTER WATCHDOG: $scn exceeded ${kOuterTimeoutSec}s; force-killing"
        try { $proc.Kill($true) } catch { }
        $hung = $true
    }

    $exitCode = if ($hung) { 99 } else { $proc.ExitCode }
    $scnEnd = Get-Date
    $elapsedSec = [int](($scnEnd - $scnStart).TotalSeconds)

    Write-Host "[overnight] $scn exit=$exitCode elapsed=${elapsedSec}s"

    $results += [ordered]@{
        scenario   = $scn
        exit_code  = $exitCode
        elapsed_sec = $elapsedSec
        hung_outer = $hung
        started_at = $scnStart.ToString("o")
    }
    Write-Host ""
}

$totalElapsed = [int](((Get-Date) - $startedAt).TotalSeconds)
$passed = ($results | Where-Object { $_.exit_code -eq 0 }).Count
$failed = $results.Count - $passed

$summary = [ordered]@{
    started_at_utc      = $startedAt.ToUniversalTime().ToString("o")
    finished_at_utc     = (Get-Date).ToUniversalTime().ToString("o")
    total_elapsed_sec   = $totalElapsed
    scenarios_total     = $results.Count
    scenarios_passed    = $passed
    scenarios_failed    = $failed
    per_scenario_timeout_sec = $ScenarioTimeoutSec
    outer_watchdog_sec  = $kOuterTimeoutSec
    redeploy            = [bool]$Redeploy
    results             = $results
    artifacts_root      = (Resolve-Path (Join-Path $RepoRoot "tools/test-runs")).Path
    morning_review_hint = "Check failed scenarios first. Per-scenario archive: tools/test-runs/<UTC>-<scenario>/. Crash dumps land alongside mtr-asi.log."
}
$summary | ConvertTo-Json -Depth 5 | Out-File $SummaryPath -Encoding utf8
Write-Host "[overnight] summary written: $SummaryPath"
Write-Host "[overnight] pass=$passed fail=$failed total_elapsed=${totalElapsed}s"

if ($failed -eq 0) { exit 0 } else { exit 1 }
