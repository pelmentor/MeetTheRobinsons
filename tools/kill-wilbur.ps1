# Kill Wilbur.exe completely, including the zombies that survive taskkill /F.
#
# Symptom this fixes: launching Wilbur.exe shows "Another instance of the Meet
# The Robinsons already exists. Exiting." even though Task Manager shows no
# Wilbur process. The named mutex is held by a half-terminated kernel process
# state that Stop-Process and taskkill /F sometimes can't finish.
#
# WMI Win32_Process::Terminate (Invoke-CimMethod) goes through a different
# kernel path that completes the cleanup.
#
# Usage:
#   pwsh -File tools\kill-wilbur.ps1
#
# See research/findings/known-issues.md issue #4.

$procs = Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue
if (-not $procs) {
    Write-Output "no Wilbur.exe processes"
    exit 0
}
foreach ($p in $procs) {
    $r = Invoke-CimMethod -InputObject $p -MethodName Terminate
    Write-Output "PID $($p.ProcessId) terminate rc=$($r.ReturnValue)"
}
Start-Sleep -Milliseconds 500

# Verify
$still = Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue
if ($still) {
    Write-Output "WARNING: Wilbur.exe still showing -- may need reboot"
    exit 1
}

# Also clean up any leftover .old.NNNN file-lock workaround leftovers from deploys
$gameDir = Join-Path $PSScriptRoot "..\Game"
if (Test-Path $gameDir) {
    Get-ChildItem -Path $gameDir -Filter "mtr-asi.asi.old.*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

Write-Output "all clear"
