@echo off
REM ============================================================================
REM Meet the Robinsons - LAN co-op CLIENT (windowed)
REM ----------------------------------------------------------------------------
REM Launches Wilbur as 1280x720 windowed and connects to a co-op host. Default
REM host is 127.0.0.1 (same-machine testing); pass a different IP as the first
REM argument for real LAN.
REM
REM Usage:
REM   coop-client.bat                 (defaults to 127.0.0.1 — same machine)
REM   coop-client.bat 192.168.1.50    (host's LAN IP)
REM
REM For real two-machine LAN (fullscreen on each PC): remove the two
REM `-dxresolution` / `-mtrasi-keep-dxresolution` lines below.
REM ============================================================================

cd /d "%~dp0"

set HOST_IP=%~1
if "%HOST_IP%"=="" set HOST_IP=127.0.0.1

echo Connecting to %HOST_IP%:31415 (windowed 1280x720)...
start "Wilbur CLIENT" Wilbur.exe -launchit ^
    -dxresolution=1280x720 ^
    -mtrasi-keep-dxresolution ^
    -mtrasi-coop-connect %HOST_IP%:31415 ^
    -mtrasi-coop-port=31416
