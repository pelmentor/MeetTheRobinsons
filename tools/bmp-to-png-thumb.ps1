#requires -version 5
<#
.SYNOPSIS
  Convert all BMP files in a directory to small PNG thumbnails.

.DESCRIPTION
  Used by run-test.ps1 after archiving screenshots. Takes the giant
  raw 2560x1440 24-bit BMPs the screenshot module emits (~11 MB each)
  and produces side-by-side PNG thumbnails (~50-200 KB each) suitable
  for inclusion in chat logs / quick-look artifacts.

  The originals are kept by default — pass -DeleteBmps to remove them
  after successful conversion (saves ~22 MB per run).

.PARAMETER Directory
  Folder containing the .bmp files to convert.

.PARAMETER MaxWidth
  Output thumbnail max width in pixels. Aspect ratio preserved.
  Default 1024 (still useful detail; ~150-300 KB PNG).

.PARAMETER DeleteBmps
  Remove the source .bmp files after the PNG is written.

.NOTES
  Uses System.Drawing.Bitmap. Windows-only by design (matches the rest
  of the project's PowerShell tooling).
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$Directory,
    [int]$MaxWidth = 1024,
    [switch]$DeleteBmps
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Directory)) {
    Write-Host "bmp-to-png-thumb: directory not found: $Directory"
    exit 0
}

$bmps = Get-ChildItem -Path $Directory -Filter "*.bmp" -ErrorAction SilentlyContinue
if (-not $bmps) {
    Write-Host "bmp-to-png-thumb: no BMP files in $Directory"
    exit 0
}

$converted = 0
$totalBytesBefore = 0
$totalBytesAfter  = 0

foreach ($bmp in $bmps) {
    try {
        $totalBytesBefore += $bmp.Length
        $src = [System.Drawing.Bitmap]::FromFile($bmp.FullName)
        try {
            $srcW = $src.Width
            $srcH = $src.Height
            if ($srcW -le $MaxWidth) {
                $newW = $srcW
                $newH = $srcH
            } else {
                $scale = [double]$MaxWidth / [double]$srcW
                $newW = [int]($srcW * $scale)
                $newH = [int]($srcH * $scale)
            }
            $thumb = New-Object System.Drawing.Bitmap $newW, $newH
            try {
                $g = [System.Drawing.Graphics]::FromImage($thumb)
                try {
                    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $g.DrawImage($src, 0, 0, $newW, $newH)
                } finally {
                    $g.Dispose()
                }
                $pngPath = [System.IO.Path]::ChangeExtension($bmp.FullName, ".png")
                $thumb.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
                $totalBytesAfter += (Get-Item $pngPath).Length
                $converted++
            } finally {
                $thumb.Dispose()
            }
        } finally {
            $src.Dispose()
        }
        if ($DeleteBmps) {
            Remove-Item $bmp.FullName -Force
        }
    } catch {
        Write-Host "bmp-to-png-thumb: failed on $($bmp.Name): $($_.Exception.Message)"
    }
}

if ($converted -gt 0) {
    $beforeMB = [math]::Round($totalBytesBefore / 1MB, 1)
    $afterKB  = [math]::Round($totalBytesAfter  / 1KB, 1)
    Write-Host "bmp-to-png-thumb: converted $converted BMPs ($beforeMB MB) -> PNGs ($afterKB KB) at max width $MaxWidth"
}
