# Terrain streaming regression guard (long-chain, self-closing).
#
# Why this exists
# ---------------
# A framing/budget mistake in the terrain streaming loop is INVISIBLE in the frame time:
# it shows up as "MORE spikes", never as fewer. Measured once (2026-10-06, P6): the install
# gate advanced only ONE tile per frame, so the load cursor never reached the unload phase,
# the unload backlog grew 67 -> 1132 monotonically, the window never settled again (zero
# "settled" lines for 46 s of flight) and the worst frame got WORSE (95.7 ms vs 63.2 ms).
# This script turns that class of bug into a FAILING exit code.
#
# Contract checked (all markers are ASCII, so this file stays pure ASCII -- repo rule:
# PowerShell 5.1 decodes .ps1 with the system code page):
#   1. during the flight the terrain residency must settle repeatedly:
#      every settle logs a line containing "ADR 0024"                  => count >= -MinSettle
#   2. every settle line must report ZERO sync fallbacks
#      (the last number on that line is "sync fallback count")         => must be 0
#   3. every backlog sample (line containing "P6-A") must keep the unload backlog bounded
#      (a monotonic climb is the signature of the bug above)           => max unload <= -MaxUnloadBacklog
#   4. V7 density legs, read from the UNTHROTTLED shutdown summary line ("hitch ... frames= over33=
#      over50= worst_ms="): over-50ms frames per minute <= -MaxOver50PerMinute, and the worst single
#      frame <= -MaxWorstFrameMs. The throttled per-frame spike lines are reported but NEVER used for
#      the density verdict (a 200 ms log throttle undercounts systematically).
#
# Preconditions (same as any long-chain measurement -- keep them identical to compare runs):
#   * the game window must NOT be minimised (a minimised window throttles the GPU/present path);
#   * the game is closed gracefully (CloseMainWindow), never killed, so the logs are flushed;
#   * --auto-test is passed automatically (F1 panel shows the "automated test" banner).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\vx_stream_regression.ps1
#   powershell -ExecutionPolicy Bypass -File tools\vx_stream_regression.ps1 -Config release
param(
    [string]$World            = 'world_a',
    [int]$AutoFlySeconds      = 60,
    [int]$MinSettle           = 5,
    [int]$MaxUnloadBacklog    = 400,
    [int]$LaunchTimeoutSeconds = 180,
    [int]$ExtraSeconds        = 10,
    [string]$Config           = 'debug',
    # V7 (density form) thresholds -- calibrated 2026-10-06 on clean debug long runs (see
    # references/performance-and-hitches.md "总判据"); the numbers live there, this is the assertion.
    [int]$MaxOver50PerMinute  = 6,
    [double]$MaxWorstFrameMs  = 80.0,
    # The V7 legs are machine-noise sensitive (this box has shown 1-9 over-50ms frames per run for
    # identical code). Default: report them as WARN so the guard never cries wolf on noise; pass
    # -StrictV7 to promote them to a hard FAIL (e.g. on a quiet machine or in CI).
    [switch]$StrictV7 = $false
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\$Config\bin\voxel_game.exe"
$work = Join-Path $root "build\$Config\bin"
$perf = Join-Path $root 'build\perf'
New-Item -ItemType Directory -Force -Path $perf | Out-Null

if (-not (Test-Path $exe)) {
    throw "game executable not found: $exe  (build the preset first: cmake --build --preset $Config)"
}

$out = Join-Path $perf "stream_regression_$Config.out.log"
$err = Join-Path $perf "stream_regression_$Config.err.log"

# Leftovers hold the exe lock and silently poison the run. Close them gracefully and wait;
# only complain (never force-kill) if they refuse to go away.
$leftover = Get-Process voxel_game -ErrorAction SilentlyContinue
if ($leftover) {
    Write-Host "closing $($leftover.Count) leftover voxel_game process(es) ..."
    foreach ($p in $leftover) { $p.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 5
    if (Get-Process voxel_game -ErrorAction SilentlyContinue) {
        throw "a voxel_game process is still running; close it and retry (it would lock $exe)"
    }
}
foreach ($f in @($out, $err)) { if (Test-Path $f) { Remove-Item $f -Force } }

Write-Host "=== terrain streaming regression: world=$World autofly=${AutoFlySeconds}s ==="
$gameArgs = @("--world=$World", '--auto-test', "--autofly=$AutoFlySeconds")
$proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $work `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
Write-Host "launched pid=$($proc.Id); waiting for the flight to start (load can take ~30-45 s) ..."

# Wait for the flight to start. "W7-S4" only appears on the flight-START line of the
# auto-fly log, never on its end line -- therefore it is a safe ASCII start marker.
$deadline = (Get-Date).AddSeconds($LaunchTimeoutSeconds)
$started  = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($proc.HasExited) { throw "game exited before the flight started (see $err)" }
    if ((Test-Path $out) -and (Select-String -Path $out -Pattern 'W7-S4' -Quiet)) { $started = $true; break }
}
if (-not $started) { throw "flight did not start within $LaunchTimeoutSeconds s (see $out)" }

Write-Host "flight started; waiting ${AutoFlySeconds}s + ${ExtraSeconds}s then closing gracefully ..."
Start-Sleep -Seconds ($AutoFlySeconds + $ExtraSeconds)
if (-not $proc.HasExited) {
    $proc.CloseMainWindow() | Out-Null
    $waitDeadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $waitDeadline -and -not $proc.HasExited) { Start-Sleep -Milliseconds 500 }
    if (-not $proc.HasExited) {
        Write-Host "WARN: game did not exit after CloseMainWindow; stopping it (logs may be truncated)"
        $proc | Stop-Process -Force
    }
}

# ---- parse -------------------------------------------------------------------------------
# NOTE: "ADR 0024" alone is NOT enough -- the one-off STARTUP line also carries that tag but has
# no sync-fallback counter (its last number is the tile count), which produced a false FAIL once.
# The settle line is the only one that also carries the ASCII word "worker" (worker build stats).
$settleLines = Select-String -Path $out -Pattern 'ADR 0024' -ErrorAction SilentlyContinue |
    Where-Object { $_.Line -match 'worker' }
$settleCount = ($settleLines | Measure-Object).Count
$fallbackBad = 0
foreach ($line in $settleLines) {
    if ($line.Line -match '([0-9]+)\D*$') { if ([int]$Matches[1] -ne 0) { $fallbackBad++ } }
}

$backlogSamples = 0
$maxUnload      = 0
$caughtUpYes    = 0
foreach ($line in (Select-String -Path $err -Pattern 'P6-A' -ErrorAction SilentlyContinue)) {
    if ($line.Line -match 'load=(\d+) \+ unload=(\d+) \+ relod=(\d+).*staged=(\d+) \+ inflight=(\d+).*caught_up=(\w+)') {
        $backlogSamples++
        $unload = [int]$Matches[2]
        if ($unload -gt $maxUnload) { $maxUnload = $unload }
        if ($Matches[6] -eq 'yes') { $caughtUpYes++ }
    }
}

$spikes = @()
foreach ($line in (Select-String -Path $err -Pattern 'draw call' -ErrorAction SilentlyContinue)) {
    if ($line.Line -match '\[WARN\s*\]\s*\S+\s+([0-9]+\.[0-9]+) ms') { $spikes += [double]$Matches[1] }
}
$spikeMax = 0.0
if ($spikes.Count -gt 0) { $spikeMax = ($spikes | Measure-Object -Maximum).Maximum }
$over50 = @($spikes | Where-Object { $_ -gt 50 }).Count

# Steady-state hitch summary (UNTHROTTLED -- the throttled spike lines above undercount and MUST NOT
# be used for a density verdict). Written once on shutdown:  frames=N over33=N over50=N worst_ms=X
$steadyFrames = -1; $steadyOver33 = -1; $steadyOver50 = -1; $steadyWorst = -1.0
foreach ($line in (Select-String -Path $out -Pattern 'hitch' -ErrorAction SilentlyContinue)) {
    if ($line.Line -match 'frames=(\d+) over33=(\d+) over50=(\d+) worst_ms=([0-9.]+)') {
        $steadyFrames = [int]$Matches[1]; $steadyOver33 = [int]$Matches[2]
        $steadyOver50 = [int]$Matches[3]; $steadyWorst  = [double]$Matches[4]
    }
}

# ---- verdict -----------------------------------------------------------------------------
$failures = @()
if ($settleCount -lt $MinSettle)     { $failures += "settle count $settleCount < $MinSettle (streaming never caught up)" }
if ($fallbackBad -gt 0)              { $failures += "$fallbackBad settle line(s) report non-zero sync fallback" }
if ($backlogSamples -gt 0 -and $maxUnload -gt $MaxUnloadBacklog) {
    $failures += "unload backlog peaked at $maxUnload > $MaxUnloadBacklog (monotonic climb)"
}
# V7 density legs (only when the shutdown summary was captured -- a force-killed run has no summary).
$over50PerMinute = 0.0
$v7warnings = @()
if ($steadyFrames -ge 0 -and $AutoFlySeconds -gt 0) { $over50PerMinute = $steadyOver50 * 60.0 / $AutoFlySeconds }
if ($steadyFrames -lt 0) {
    Write-Host 'WARN: steady-state hitch summary missing (old binary, or the run was force-killed) -- V7 legs skipped'
} else {
    if ($over50PerMinute -gt $MaxOver50PerMinute) {
        $v7warnings += ("over-50ms density {0:N2}/min > {1}" -f $over50PerMinute, $MaxOver50PerMinute)
    }
    if ($steadyWorst -gt $MaxWorstFrameMs) {
        $v7warnings += ("worst frame {0} ms > {1} ms" -f $steadyWorst, $MaxWorstFrameMs)
    }
}
foreach ($w in $v7warnings) {
    if ($StrictV7) { $failures += "V7: $w" } else { Write-Host "V7 WARN (re-run to confirm on a noisy box; -StrictV7 to fail): $w" }
}

Write-Host ("RESULT: settle={0} (min {1})  syncFallbackBad={2}  backlogSamples={3} maxUnload={4} (limit {5})  caughtUpYes={6}" -f `
    $settleCount, $MinSettle, $fallbackBad, $backlogSamples, $maxUnload, $MaxUnloadBacklog, $caughtUpYes)
Write-Host ("        V7: frames={0} over33={1} over50={2} worst={3} ms  =>  over50 density {4:N2}/min (limit {5})" -f `
    $steadyFrames, $steadyOver33, $steadyOver50, $steadyWorst, $over50PerMinute, $MaxOver50PerMinute)
Write-Host ("        spike log (throttled, informational): lines={0} maxFrameMs={1} over50ms={2}" -f `
    $spikes.Count, [math]::Round($spikeMax, 1), $over50)

if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'FAIL:'
    foreach ($f in $failures) { Write-Host "  - $f" }
    Write-Host "logs: $out  /  $err"
    exit 1
}
Write-Host ''
Write-Host 'PASS'
Write-Host "logs: $out  /  $err"
exit 0
