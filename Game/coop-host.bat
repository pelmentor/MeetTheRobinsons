@echo off
REM ============================================================================
REM Meet the Robinsons - LAN co-op HOST (windowed)
REM ----------------------------------------------------------------------------
REM Launches Wilbur in a normal draggable window (1280x720) with caption,
REM system menu, and resizable frame. Binds UDP 31415 and waits for the
REM client to send the first datagram. Once that arrives, both sides
REM auto-spawn the other player's character (P2 orphan).
REM
REM Drive your own player normally. Same-machine testing: run this, then
REM coop-client.bat 127.0.0.1 in a separate console — both windows are
REM regular Windows windows so you can drag them side-by-side by the
REM title bar and click whichever you want to give keyboard focus.
REM
REM For real two-machine LAN (fullscreen on each PC): remove the two
REM `-dxresolution` / `-mtrasi-keep-dxresolution` lines below.
REM
REM Firewall: Windows Defender will prompt the first time. Allow UDP 31415
REM on the Private network profile.
REM ============================================================================

cd /d "%~dp0"
echo Starting Wilbur as co-op HOST on UDP 31415 (windowed 1280x720)...
start "Wilbur HOST" Wilbur.exe -launchit ^
    -dxresolution=1280x720 ^
    -mtrasi-keep-dxresolution ^
    -mtrasi-coop-host 31415 ^
    -mtrasi-coop-port=31415
