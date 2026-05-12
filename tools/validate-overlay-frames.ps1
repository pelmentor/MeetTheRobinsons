#requires -version 5
<#
.SYNOPSIS
  Offline validator for the trigger overlay's projection + clip pipeline.

.DESCRIPTION
  Reads TRIGGER_OVERLAY_* lines from a mtr-asi.log produced by the
  overlay-phase1-verify scenario, re-runs the projection math
  independently (4x4 row-vector multiply, homogeneous parametric clip
  against 6 D3D9 frustum planes, NDC->screen with Y-flip), and asserts
  the logged screen-space edge endpoints match the recomputed values
  within a fixed tolerance.

  Exits 0 on pass, 1 on fail. Writes a human-readable summary to stdout
  AND a structured validation-result.json next to the input log.

.PARAMETER LogPath
  Path to mtr-asi.log (or any log file containing TRIGGER_OVERLAY_*
  lines from the export path).

.PARAMETER Tolerance
  Per-pixel tolerance for screen-coordinate match. Default 0.5.

.NOTES
  Math reference: src/mtr-asi/src/trigger_overlay.cpp. Algorithm matches
  the in-mod implementation BIT-FOR-BIT — any drift here is a bug
  somewhere; the whole point is to detect drift.
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$LogPath,
    [double]$Tolerance = 0.5
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $LogPath)) {
    Write-Error "Log not found: $LogPath"
    exit 2
}

# === Math helpers ==========================================================

function MatMul4x4 {
    param([double[]]$A, [double[]]$B)
    # Both inputs are 16-element row-major (A[r*4+c]). Returns out = A * B.
    # NOTE: PowerShell is case-INSENSITIVE for variables, so a result var
    # named $C collides with a loop var $c. Use $out instead.
    $out = [System.Array]::CreateInstance([double], 16)
    for ($r = 0; $r -lt 4; $r++) {
        for ($col = 0; $col -lt 4; $col++) {
            $s = 0.0
            for ($k = 0; $k -lt 4; $k++) {
                $s += $A[$r * 4 + $k] * $B[$k * 4 + $col]
            }
            $out[$r * 4 + $col] = $s
        }
    }
    Write-Output -NoEnumerate $out
    return
}

function RowMul4 {
    param([double[]]$v, [double[]]$M)
    # Row-vector * matrix; matches D3D9 / trigger_overlay.cpp::row_mul.
    $out = [System.Array]::CreateInstance([double], 4)
    for ($c = 0; $c -lt 4; $c++) {
        $s = 0.0
        for ($k = 0; $k -lt 4; $k++) {
            $s += $v[$k] * $M[$k * 4 + $c]
        }
        $out[$c] = $s
    }
    Write-Output -NoEnumerate $out
    return
}

function PlaneDist {
    param([int]$plane, [double[]]$v)
    # Mirror trigger_overlay.cpp::plane_dist exactly:
    # 0: w+x  1: w-x  2: w+y  3: w-y  4: z  5: w-z
    switch ($plane) {
        0 { return $v[3] + $v[0] }
        1 { return $v[3] - $v[0] }
        2 { return $v[3] + $v[1] }
        3 { return $v[3] - $v[1] }
        4 { return $v[2] }
        5 { return $v[3] - $v[2] }
    }
    return 0.0
}

function Vec4Lerp {
    param([double[]]$a, [double[]]$b, [double]$t)
    $r = [System.Array]::CreateInstance([double], 4)
    for ($i = 0; $i -lt 4; $i++) {
        $r[$i] = $a[$i] + $t * ($b[$i] - $a[$i])
    }
    Write-Output -NoEnumerate $r
    return
}

function ClipSegment {
    # Returns @{ ok = [bool]; a = [double[]]; b = [double[]] }
    param([double[]]$a, [double[]]$b)
    for ($p = 0; $p -lt 6; $p++) {
        $da = PlaneDist -plane $p -v $a
        $db = PlaneDist -plane $p -v $b
        $a_in = $da -ge 0.0
        $b_in = $db -ge 0.0
        if (-not $a_in -and -not $b_in) { return @{ ok = $false } }
        if ($a_in -and $b_in) { continue }
        $denom = $da - $db
        if ([math]::Abs($denom) -lt 1.0e-7) { return @{ ok = $false } }
        $t = $da / $denom
        if ([double]::IsNaN($t) -or [double]::IsInfinity($t)) { return @{ ok = $false } }
        if ($a_in) {
            $b = Vec4Lerp -a $a -b $b -t $t
        } else {
            $a = Vec4Lerp -a $a -b $b -t $t
        }
    }
    return @{ ok = $true; a = $a; b = $b }
}

function NdcToScreen {
    param([double[]]$clip, [double]$vp_w, [double]$vp_h)
    $inv_w = 1.0 / [double]$clip[3]
    $ndc_x = [double]$clip[0] * $inv_w
    $ndc_y = [double]$clip[1] * $inv_w
    $screenX = ($ndc_x * 0.5 + 0.5) * $vp_w
    $screenY = (1.0 - ($ndc_y * 0.5 + 0.5)) * $vp_h
    Write-Output -NoEnumerate ([double[]]@($screenX, $screenY))
    return
}

function BuildAabbCorners {
    # Mirror trigger_overlay.cpp::build_aabb_corners byte-for-byte.
    # bit 0 → +x, bit 1 → +z, bit 2 → +y. Verbose names to avoid any
    # PowerShell variable-shadowing or case-insensitive collision.
    param(
        [double]$centerX, [double]$centerY, [double]$centerZ,
        [double]$extentsX, [double]$extentsY, [double]$extentsZ
    )
    $cornersOut = [System.Array]::CreateInstance([object], 8)
    for ($i = 0; $i -lt 8; $i++) {
        if ($i -band 1) { $offsetX = $extentsX } else { $offsetX = -$extentsX }
        if ($i -band 2) { $offsetZ = $extentsZ } else { $offsetZ = -$extentsZ }
        if ($i -band 4) { $offsetY = $extentsY } else { $offsetY = -$extentsY }
        $cornerX = $centerX + $offsetX
        $cornerY = $centerY + $offsetY
        $cornerZ = $centerZ + $offsetZ
        $arr = [double[]]@($cornerX, $cornerY, $cornerZ, 1.0)
        $cornersOut[$i] = $arr
    }
    Write-Output -NoEnumerate $cornersOut
    return
}

# Edge table — matches trigger_overlay.cpp::kEdges exactly.
# Stored as parallel int[24] (a/b interleaved) to dodge PowerShell's
# array-of-arrays flattening behavior.
$kEdgeA = [int[]]@(0, 1, 3, 2, 4, 5, 7, 6, 0, 1, 2, 3)
$kEdgeB = [int[]]@(1, 3, 2, 0, 5, 7, 6, 4, 4, 5, 6, 7)

# === Log parsing ===========================================================

$frames = @{}   # frame_seq -> @{ vp = ...; view = ...; proj = ...; box = ...; edges = @{} }

function GetFrame { param($f)
    if (-not $frames.ContainsKey($f)) {
        $frames[$f] = @{
            vp    = $null
            view  = $null
            proj  = $null
            boxes = @{}
            edges = @{}
            end   = $null
        }
    }
    return $frames[$f]
}

# Patterns. The log prefix is "[HH:MM:SS.mmm] " — strip via -replace before matching.
$reFrameBegin = '^TRIGGER_OVERLAY_FRAME_BEGIN f=(\d+) vp=([\d\.\-]+),([\d\.\-]+)$'
$reView       = '^TRIGGER_OVERLAY_VIEW f=(\d+) (.+)$'
$reProj       = '^TRIGGER_OVERLAY_PROJ f=(\d+) (.+)$'
$reBox        = '^TRIGGER_OVERLAY_BOX f=(\d+) idx=(\d+) center=([\d\.\-]+),([\d\.\-]+),([\d\.\-]+) extents=([\d\.\-]+),([\d\.\-]+),([\d\.\-]+) color=0x([0-9A-Fa-f]+) clipped=(\w+)$'
$reEdge       = '^TRIGGER_OVERLAY_EDGE f=(\d+) box=(\d+) i=(\d+) a=([\d\.\-]+),([\d\.\-]+) b=([\d\.\-]+),([\d\.\-]+)$'
$reFrameEnd   = '^TRIGGER_OVERLAY_FRAME_END f=(\d+) boxes=(\d+) edges=(\d+)$'

$lineCount = 0
$matchedCount = 0
Get-Content $LogPath -ReadCount 1024 | ForEach-Object {
    foreach ($line in $_) {
        $lineCount++
        $payload = $line -replace '^\[\d{2}:\d{2}:\d{2}\.\d{3}\] ', ''
        if ($payload -match $reFrameBegin) {
            $f = [uint64]$matches[1]
            $fr = GetFrame $f
            $fr.vp = @([double]$matches[2], [double]$matches[3])
            $matchedCount++
        } elseif ($payload -match $reView) {
            $f = [uint64]$matches[1]
            $fr = GetFrame $f
            $vals = $matches[2] -split ','
            $fr.view = [double[]]($vals | ForEach-Object { [double]$_ })
            $matchedCount++
        } elseif ($payload -match $reProj) {
            $f = [uint64]$matches[1]
            $fr = GetFrame $f
            $vals = $matches[2] -split ','
            $fr.proj = [double[]]($vals | ForEach-Object { [double]$_ })
            $matchedCount++
        } elseif ($payload -match $reBox) {
            $f = [uint64]$matches[1]
            $bidx = [int]$matches[2]
            $fr = GetFrame $f
            $fr.boxes[$bidx] = @{
                center  = @([double]$matches[3], [double]$matches[4], [double]$matches[5])
                extents = @([double]$matches[6], [double]$matches[7], [double]$matches[8])
                color   = $matches[9]
                clipped = $matches[10]
            }
            $matchedCount++
        } elseif ($payload -match $reEdge) {
            $f = [uint64]$matches[1]
            $bidx = [int]$matches[2]
            $eidx = [int]$matches[3]
            $fr = GetFrame $f
            if (-not $fr.edges.ContainsKey($bidx)) { $fr.edges[$bidx] = @{} }
            $fr.edges[$bidx][$eidx] = @{
                a = @([double]$matches[4], [double]$matches[5])
                b = @([double]$matches[6], [double]$matches[7])
            }
            $matchedCount++
        } elseif ($payload -match $reFrameEnd) {
            $f = [uint64]$matches[1]
            $fr = GetFrame $f
            $fr.end = @{ boxes = [int]$matches[2]; edges = [int]$matches[3] }
            $matchedCount++
        }
    }
}

Write-Host "validate-overlay-frames: scanned $lineCount lines, matched $matchedCount overlay records"
Write-Host "validate-overlay-frames: parsed $($frames.Count) frames"

if ($frames.Count -eq 0) {
    Write-Host "FAIL: no TRIGGER_OVERLAY_* records found in log"
    exit 1
}

# === Validation pass =======================================================

$errors = New-Object System.Collections.Generic.List[string]
$framesValidated = 0
$edgesValidated  = 0

foreach ($key in $frames.Keys | Sort-Object) {
    try {
        $fr = $frames[$key]
        if ($null -eq $fr.vp -or $null -eq $fr.view -or $null -eq $fr.proj -or $null -eq $fr.end) {
            $errors.Add("frame ${key}: incomplete record (vp/view/proj/end missing)")
            continue
        }
        if ($fr.view.Length -ne 16 -or $fr.proj.Length -ne 16) {
            $errors.Add("frame ${key}: matrix length wrong (view=$($fr.view.Length) proj=$($fr.proj.Length))")
            continue
        }

        $VP = MatMul4x4 -A $fr.view -B $fr.proj
        $vp_w = $fr.vp[0]
        $vp_h = $fr.vp[1]
    } catch {
        $errors.Add("frame ${key}: pre-loop exception: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))")
        continue
    }

    foreach ($bidx in $fr.boxes.Keys) {
        try {
            $box = $fr.boxes[$bidx]
            $cx = [double]$box.center[0]
            $cy = [double]$box.center[1]
            $cz = [double]$box.center[2]
            $ex = [double]$box.extents[0]
            $ey = [double]$box.extents[1]
            $ez = [double]$box.extents[2]
            $corners = BuildAabbCorners -centerX $cx -centerY $cy -centerZ $cz -extentsX $ex -extentsY $ey -extentsZ $ez
            $clip = [System.Array]::CreateInstance([object], 8)
            for ($i = 0; $i -lt 8; $i++) {
                $clip[$i] = RowMul4 -v ([double[]]$corners[$i]) -M ([double[]]$VP)
            }
        } catch {
            $errors.Add("frame ${key} box ${bidx}: setup failed: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))")
            continue
        }
        try {

        if (-not $fr.edges.ContainsKey($bidx)) { continue }  # whole-box-rejected boxes have no edges
        $loggedEdges = $fr.edges[$bidx]

        foreach ($eidx in $loggedEdges.Keys) {
            $ai = $kEdgeA[$eidx]
            $bi = $kEdgeB[$eidx]
            $a = [double[]]$clip[$ai]
            $b = [double[]]$clip[$bi]
            $clipped = ClipSegment -a $a -b $b
            if (-not $clipped.ok) {
                $errors.Add("frame ${key} box $bidx edge ${eidx}: log says drawn but our clip says rejected")
                continue
            }
            $sa = NdcToScreen -clip ([double[]]$clipped.a) -vp_w $vp_w -vp_h $vp_h
            $sb = NdcToScreen -clip ([double[]]$clipped.b) -vp_w $vp_w -vp_h $vp_h
            $logA = $loggedEdges[$eidx].a
            $logB = $loggedEdges[$eidx].b
            $da_x = $sa[0] - $logA[0]
            $da_y = $sa[1] - $logA[1]
            $db_x = $sb[0] - $logB[0]
            $db_y = $sb[1] - $logB[1]
            $maxDelta = [math]::Max(
                [math]::Max([math]::Abs($da_x), [math]::Abs($da_y)),
                [math]::Max([math]::Abs($db_x), [math]::Abs($db_y)))
            if ($maxDelta -gt $Tolerance) {
                $errors.Add(
                    ("frame {0} box {1} edge {2}: max delta {3:F4}px exceeds tolerance {4} " +
                     "(logged a=({5:F4},{6:F4}) b=({7:F4},{8:F4}); recomputed a=({9:F4},{10:F4}) b=({11:F4},{12:F4}))") -f
                    $key, $bidx, $eidx, $maxDelta, $Tolerance,
                    $logA[0], $logA[1], $logB[0], $logB[1],
                    $sa[0], $sa[1], $sb[0], $sb[1])
            } else {
                $edgesValidated++
            }
        }
        } catch {
            $errors.Add("frame ${key} box ${bidx}: edge-loop exception: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))")
            continue
        }
    }
    $framesValidated++
}

# === Result ================================================================

$resultPath = Join-Path (Split-Path $LogPath -Parent) "validation-result.json"
$result = [ordered]@{
    log_path           = $LogPath
    tolerance_px       = $Tolerance
    frames_parsed      = $frames.Count
    frames_validated   = $framesValidated
    edges_validated_ok = $edgesValidated
    error_count        = $errors.Count
    errors             = if ($errors.Count -gt 0) { @($errors | Select-Object -First 32) } else { @() }
    pass               = ($errors.Count -eq 0 -and $framesValidated -gt 0)
}
$result | ConvertTo-Json -Depth 4 | Out-File $resultPath -Encoding utf8
Write-Host "validate-overlay-frames: wrote $resultPath"

if ($result.pass) {
    Write-Host "PASS: $framesValidated frames validated, $edgesValidated edges within $Tolerance px tolerance"
    exit 0
} else {
    Write-Host "FAIL: $($errors.Count) error(s) across $framesValidated frames"
    foreach ($e in $errors | Select-Object -First 8) {
        Write-Host "  $e"
    }
    if ($errors.Count -gt 8) {
        Write-Host "  ... ($($errors.Count - 8) more in $resultPath)"
    }
    exit 1
}
