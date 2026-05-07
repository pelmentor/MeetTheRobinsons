# Restore mouse buttons after Wilbur.exe (or any DirectInput-exclusive game) crash.
#
# Symptom: cursor moves but clicks don't register anywhere. Caused by the game
# leaving its DirectInput8 mouse device in DISCL_EXCLUSIVE state with no clean
# release. See research/findings/known-issues.md issue #1.
#
# Usage:
#   pwsh -File tools\mouse-wake.ps1
#   (or right-click in Explorer → Run with PowerShell)
#
# If this doesn't work, escalate to: Ctrl+Alt+Del → Cancel, then Win+L → log in,
# then reboot.

$src = @"
using System;
using System.Runtime.InteropServices;
public static class MouseWaker {
    [DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr lpRect);
    [DllImport("user32.dll")] public static extern bool BlockInput(bool fBlockIt);
    [DllImport("user32.dll")] public static extern int  ShowCursor(bool bShow);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, [In] INPUT[] pInputs, int cbSize);
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(4)] public MOUSEINPUT mi; }
    public const uint INPUT_MOUSE        = 0;
    public const uint MOUSEEVENTF_LEFTUP   = 0x0004;
    public const uint MOUSEEVENTF_RIGHTUP  = 0x0010;
    public const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
}
"@
Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue

# 1. Kill any Wilbur zombie processes (which often hold the DirectInput grab)
Get-CimInstance Win32_Process -Filter "Name='Wilbur.exe'" -ErrorAction SilentlyContinue |
    ForEach-Object {
        $r = Invoke-CimMethod -InputObject $_ -MethodName Terminate
        Write-Output "killed Wilbur.exe PID $($_.ProcessId) (rc=$($r.ReturnValue))"
    }

# 2. Release any cursor clipping
[MouseWaker]::ClipCursor([IntPtr]::Zero) | Out-Null

# 3. Unblock input (in case BlockInput(true) was set somewhere)
[MouseWaker]::BlockInput($false) | Out-Null

# 4. Force-release any stuck mouse buttons
$inputs = New-Object MouseWaker+INPUT[] 3
$inputs[0].type = [MouseWaker]::INPUT_MOUSE; $inputs[0].mi.dwFlags = [MouseWaker]::MOUSEEVENTF_LEFTUP
$inputs[1].type = [MouseWaker]::INPUT_MOUSE; $inputs[1].mi.dwFlags = [MouseWaker]::MOUSEEVENTF_RIGHTUP
$inputs[2].type = [MouseWaker]::INPUT_MOUSE; $inputs[2].mi.dwFlags = [MouseWaker]::MOUSEEVENTF_MIDDLEUP
[MouseWaker]::SendInput(3, $inputs, [System.Runtime.InteropServices.Marshal]::SizeOf($inputs[0])) | Out-Null

# 5. Bring cursor visibility counter to >= 0 so cursor draws
while (([MouseWaker]::ShowCursor($true)) -lt 0) { }

Write-Output "mouse-wake done. Try clicking. If still stuck: Ctrl+Alt+Del -> Cancel, or Win+L."
