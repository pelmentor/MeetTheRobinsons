# pe-info.ps1 — print PE headers + section table for a Windows binary.
#
# Usage:
#   pwsh tools\pe-info.ps1 Game\Wilbur.exe
#
# Output identifies the format, image base, entry point, and every section's
# virtual / raw layout — useful for spotting packers (UPX, ASPack, SecuROM, …).

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$Path
)

if (-not (Test-Path $Path)) {
    Write-Error "File not found: $Path"
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($Path)
if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    Write-Error "Not an MZ executable: $Path"
    exit 1
}

$peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
$peSig    = [System.Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4)
if ($peSig.Substring(0,2) -ne "PE") {
    Write-Error "No PE signature at 0x$($peOffset.ToString('X')): got '$peSig'"
    exit 1
}

$machine          = [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
$numSections      = [System.BitConverter]::ToUInt16($bytes, $peOffset + 6)
$sizeOfOptHeader  = [System.BitConverter]::ToUInt16($bytes, $peOffset + 20)
$characteristics  = [System.BitConverter]::ToUInt16($bytes, $peOffset + 22)

$optStart   = $peOffset + 24
$magic      = [System.BitConverter]::ToUInt16($bytes, $optStart)
$entryPoint = [System.BitConverter]::ToUInt32($bytes, $optStart + 16)
$imageBase  = if ($magic -eq 0x20B) {
    [System.BitConverter]::ToUInt64($bytes, $optStart + 24)
} else {
    [System.BitConverter]::ToUInt32($bytes, $optStart + 28)
}
$sizeOfImage = [System.BitConverter]::ToUInt32($bytes, $optStart + 56)
$subsystem   = [System.BitConverter]::ToUInt16($bytes, $optStart + 68)

$arch = switch ($machine) {
    0x014C { "i386 (PE32, 32-bit)" }
    0x8664 { "x86_64 (PE32+, 64-bit)" }
    0x01C4 { "ARM (PE32, 32-bit)" }
    0xAA64 { "ARM64 (PE32+, 64-bit)" }
    default { "machine 0x{0:X4}" -f $machine }
}

Write-Output "=== $Path ==="
Write-Output "Size:               $($bytes.Length) bytes"
Write-Output "MD5:                $((Get-FileHash $Path -Algorithm MD5).Hash)"
Write-Output "SHA256:             $((Get-FileHash $Path -Algorithm SHA256).Hash)"
Write-Output ("PE header offset:   0x{0:X}" -f $peOffset)
Write-Output "Architecture:       $arch"
Write-Output "Subsystem:          $(if ($subsystem -eq 2) {'GUI'} elseif ($subsystem -eq 3) {'CUI'} else {"$subsystem"})"
Write-Output ("Characteristics:    0x{0:X4}" -f $characteristics)
Write-Output ("ImageBase:          0x{0:X}" -f $imageBase)
Write-Output ("EntryPoint (RVA):   0x{0:X}" -f $entryPoint)
Write-Output ("EntryPoint (VA):    0x{0:X}" -f ($imageBase + $entryPoint))
Write-Output ("SizeOfImage:        0x{0:X}" -f $sizeOfImage)
Write-Output "NumberOfSections:   $numSections"
Write-Output ""
Write-Output ("{0,-9} {1,-12} {2,-12} {3,-12} {4,-12} {5}" -f "Section", "VirtAddr", "VirtSize", "RawOff", "RawSize", "Char")
Write-Output ("-" * 70)

$secStart = $optStart + $sizeOfOptHeader
for ($i = 0; $i -lt $numSections; $i++) {
    $off    = $secStart + $i * 40
    $name   = [System.Text.Encoding]::ASCII.GetString($bytes, $off, 8).TrimEnd([char]0)
    $vsize  = [System.BitConverter]::ToUInt32($bytes, $off + 8)
    $vaddr  = [System.BitConverter]::ToUInt32($bytes, $off + 12)
    $rsize  = [System.BitConverter]::ToUInt32($bytes, $off + 16)
    $roff   = [System.BitConverter]::ToUInt32($bytes, $off + 20)
    $charac = [System.BitConverter]::ToUInt32($bytes, $off + 36)
    Write-Output ("{0,-9} 0x{1:X8}   0x{2:X8}   0x{3:X8}   0x{4:X8}   0x{5:X8}" -f $name, $vaddr, $vsize, $roff, $rsize, $charac)
}

# Overlay (data after the last section's raw bytes — common for SFX archives).
$maxFileEnd = 0
for ($i = 0; $i -lt $numSections; $i++) {
    $off  = $secStart + $i * 40
    $rsz  = [System.BitConverter]::ToUInt32($bytes, $off + 16)
    $rof  = [System.BitConverter]::ToUInt32($bytes, $off + 20)
    $end  = $rof + $rsz
    if ($end -gt $maxFileEnd) { $maxFileEnd = $end }
}
$overlay = $bytes.Length - $maxFileEnd
if ($overlay -gt 0) {
    $first16 = $bytes[$maxFileEnd..($maxFileEnd + 15)]
    $hex     = ($first16 | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    Write-Output ""
    Write-Output ("Overlay: {0} bytes at offset 0x{1:X}" -f $overlay, $maxFileEnd)
    Write-Output ("Overlay first 16 bytes: $hex")
    if ($first16[0] -eq 0x37 -and $first16[1] -eq 0x7A -and $first16[2] -eq 0xBC -and $first16[3] -eq 0xAF) {
        Write-Output "  → looks like a 7-Zip SFX archive (signature '7z\xBC\xAF\x27\x1C'). Extract with: 7z x <file>"
    }
}
