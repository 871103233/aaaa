# Input-only hitch localization smoke (config untouched, no code change).
#
# Modes:
#   stand     control: stand still for 20 s
#   rot       yaw sweep only (400 x mouse move)
#   fly       F then Space for 15 s
#   flyfwd    F then W for 15 s
#   walk      W for 20 s
#   walkback  W 20 s -> settle 6 s -> S 20 s -> settle 6 s   (build path: window grows)
#   flybound  T80/T81 evidence: fly out of the diggable region and back (window shrinks then grows)
#   settle    T81 evidence with settle segments (prints the residency closing line)
#   shot      one light orb (carve -> dirty remesh -> upload fast path)
#
# Outputs: build/perf/input_<Mode>.out.log (stdout) and input_<Mode>.err.log (stderr).
# Prints one RESULT line with hitch counts + logic/untimed buckets + residency events.
# Keep this file PURE ASCII - repo rule (PowerShell 5.1 decodes .ps1 with the system code page).
param([string]$Mode = 'stand')
$ErrorActionPreference = 'Stop'

# Repository root is the parent of this script's folder (tools/), so the script works
# from any current directory and carries no machine-specific absolute path.
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\debug\bin\voxel_game.exe'
$work = Join-Path $root 'build\debug\bin'
$perf = Join-Path $root 'build\perf'
New-Item -ItemType Directory -Force -Path $perf | Out-Null

if (-not (Test-Path $exe)) {
    throw "game executable not found: $exe  (build the debug preset first: cmake --build --preset debug)"
}

Add-Type -Namespace Win32 -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
[DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, UIntPtr dwExtraInfo);
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
'@

# Focus guard: keybd_event is a SYSTEM-level injection and only reaches the CURRENT FOREGROUND
# window, so a background game window silently drops every key the script sends (known weak spot,
# see docs/devlog.md T81: one whole smoke run produced a "looks fine" but empty log because of it).
# Therefore: re-claim the foreground before every key press and VERIFY with GetForegroundWindow();
# if it cannot be claimed, report it instead of pretending the input landed.
function Ensure-GameForeground([IntPtr]$hwnd) {
    if (-not $hwnd -or $hwnd -eq [IntPtr]::Zero) { return $false }
    for ($i = 0; $i -lt 20; $i++) {
        [Win32.Native]::ShowWindow($hwnd, 9) | Out-Null          # 9 = SW_RESTORE (also un-minimizes)
        [Win32.Native]::BringWindowToTop($hwnd) | Out-Null
        [Win32.Native]::SetForegroundWindow($hwnd) | Out-Null
        Start-Sleep -Milliseconds 150
        if ([Win32.Native]::GetForegroundWindow() -eq $hwnd) { return $true }
    }
    return $false
}

$out = Join-Path $perf "input_$Mode.out.log"
$err = Join-Path $perf "input_$Mode.err.log"
Get-Process voxel_game -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
if (Test-Path $out) { Remove-Item $out -Force }
if (Test-Path $err) { Remove-Item $err -Force }

Write-Host "=== mode $Mode ==="
# T85: launch the game with --auto-test so the F1 panel shows the "automated test, do NOT touch
# the keyboard/mouse" banner (proof of which mode this run is in).
$proc = Start-Process -FilePath $exe -ArgumentList '--auto-test' -WorkingDirectory $work `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru

Start-Sleep -Seconds 26
$h = (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue).MainWindowHandle
if (-not $h -or $h -eq 0) { $h = (Get-Process voxel_game | Select-Object -First 1).MainWindowHandle }
$focusOk = Ensure-GameForeground $h
Write-Host "focus(initial)=$focusOk hwnd=$h"
Start-Sleep -Milliseconds 800

$MOUSEMOVE = 0x0001; $KEYUP = 0x0002
$VK_W = 0x57; $VK_F = 0x46; $VK_SPACE = 0x20; $VK_S = 0x53; $VK_F1 = 0x70

# NOTE: the mouse is captured automatically on window activation (see the
# "mouse capture: ON" log at load end), so NO click is sent here - a click would fire an orb.
switch ($Mode) {
    'rot' {
        for ($i = 0; $i -lt 400; $i++) {
            [Win32.Native]::mouse_event($MOUSEMOVE, 30, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 50
        }
    }
    'fly' {
        [Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        [Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 700
        [Win32.Native]::keybd_event($VK_SPACE, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 15
        [Win32.Native]::keybd_event($VK_SPACE, 0, $KEYUP, [UIntPtr]::Zero)
    }
    'flyfwd' {
        [Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        [Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 700
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 15
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
    }
    'shot' {
        # The mouse is already captured when the window activates (see the "mouse capture: ON"
        # log), so this click really fires one light orb -> it deterministically covers the
        # "carve -> dirty remesh -> upload (T75 fast path)" chain.
        [Win32.Native]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        [Win32.Native]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 8
    }
    'walk' {
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 20
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
    }
    'walkback' {
        # T81 evidence: walk OUT first (window shrinks => unload + tile handover), then walk BACK
        # (window grows => block CREATE). Only the way back triggers CreateBlock => that is where
        # the worker build path (T81 / ADR 0022) can be observed. 20 s because under debug the
        # simulated time runs slower than wall clock (fixed step has a per-frame cap);
        # measured: 9 s does not cross one tile.
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 20
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 6
        [Win32.Native]::keybd_event($VK_S, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 20
        [Win32.Native]::keybd_event($VK_S, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 6
    }
    'flybound' {
        # T80 + T81 evidence run (deterministic boundary crossing).
        # Facts (measured, docs/devlog.md T81):
        #   * diggable region = central 3x3 tiles (region tiles x/z in [-1,2]); residency window =
        #     activity (+-2) + prefetch ring (1) = +-3 tiles => while the player tile is in [-1,2]
        #     the window fully covers the table: 441 blocks, ZERO builds, ZERO unloads (that is why
        #     every earlier smoke saw nothing).
        #   * the player leaves W = +X+Z, S = -X-Z (spawn yaw); ground-level speed varies a lot with
        #     terrain (measured 4.5..15 blocks/s), while in OPEN AIR it is ~24 blocks/s
        #     (two runs, same key, same direction) => climb first, then travel.
        #   * x < -64 (tile -2) or x >= 192 (tile 3) is needed to drop the outer 63-block rings;
        #     coming back re-creates them => that is the ONLY path into the worker build pipeline.
        # Recipe: hide the F1 panel (see below) -> flight -> climb 4 s (get out of the terrain) ->
        # fly OUT 20 s (>= 90 diagonal blocks even at the slowest measured speed => outside) ->
        # settle -> fly BACK in two legs with settles (whatever the speed, two legs must cross the
        # covered tiles) -> long settle so Step() (1 block/frame) finishes creating + installing.
        # Prerequisite (the real reason automated input "did not work" for T72/T75): the F1 debug
        # panel is shown at start and SUPPRESSES gameplay keys (T15: suppression.keyboardGameplay
        # swallows F / W / S; only F1 itself is exempt) => press F1 first to hide the panel.
        [Win32.Native]::keybd_event($VK_F1, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
        [Win32.Native]::keybd_event($VK_F1, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 600
        [Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 150
        [Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 700
        Write-Host "focus(fly-out)=$(Ensure-GameForeground $h)"
        [Win32.Native]::keybd_event($VK_SPACE, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 4
        [Win32.Native]::keybd_event($VK_SPACE, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 300
        [Win32.Native]::keybd_event($VK_S, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 20
        [Win32.Native]::keybd_event($VK_S, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 8
        Write-Host "focus(fly-back)=$(Ensure-GameForeground $h)"
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 10
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 6
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 10
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 18
    }
    'settle' {
        # T81 evidence run (with settle segments): only "move -> stop and let the residency plan
        # finish" prints the closing line "residency set adjusted for the window", which contains
        # "worker built N blocks / per-block compute peak". Fly out ~2 tiles (the window starts
        # excluding the region edge => unload) -> settle -> fly back (=> build) -> settle.
        [Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        [Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 600
        [Win32.Native]::keybd_event($VK_W, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 9
        [Win32.Native]::keybd_event($VK_W, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 9
        [Win32.Native]::keybd_event($VK_S, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 9
        [Win32.Native]::keybd_event($VK_S, 0, $KEYUP, [UIntPtr]::Zero)
        Start-Sleep -Seconds 12
        [Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        [Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
    }
    default {
        Start-Sleep -Seconds 20   # stand still: control
    }
}
Start-Sleep -Seconds 3
# Input-registration marker: pressing F logs "flight mode: ON" - proof the automated input DID reach the game.
[Win32.Native]::keybd_event($VK_F, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 150
[Win32.Native]::keybd_event($VK_F, 0, $KEYUP, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 1200
# Graceful exit: stdout is redirected to a PIPE => the C runtime BLOCK-buffers it, so a hard
# Stop-Process DISCARDS the tail (that is why earlier smokes "ended" around t=16 s and why the
# worker/movement lines were missing). Closing the window goes through SDL's quit path =>
# normal exit => the CRT flushes. Only if that fails do we hard-kill.
[Win32.Native]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # 0x0010 = WM_CLOSE
for ($i = 0; $i -lt 40; $i++) { Start-Sleep -Milliseconds 150; $proc.Refresh(); if ($proc.HasExited) { break } }
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800

$outLines = [System.IO.File]::ReadAllLines($out, [System.Text.Encoding]::UTF8)
$errLines = [System.IO.File]::ReadAllLines($err, [System.Text.Encoding]::UTF8)

$hc = 0; $hm = 0.0; $ls = 0.0; $lm = 0.0; $us = 0.0; $um = 0.0
foreach ($ln in $errLines) {
    if ($ln -match '([0-9.]+) ms')       { $hc++; $f = [double]$Matches[1]; if ($f -gt $hm) { $hm = $f } }
    if ($ln -match '([0-9.]+) \+ UI')    { $g = [double]$Matches[1]; $ls += $g; if ($g -gt $lm) { $lm = $g } }
    if ($ln -match '([0-9.]+)\*\* ms')   { $u = [double]$Matches[1]; $us += $u; if ($u -gt $um) { $um = $u } }
}
$res = ($outLines | Where-Object { $_ -match 'ADR 0020' }).Count
$exp = ($outLines | Where-Object { $_ -match 'ms / .*?([0-9.]+) ms' }).Count
$ftg = ($outLines | Where-Object { $_ -match 'Space' }).Count
$swp = ($outLines | Select-String -Pattern '2560x' | Select-Object -First 1)

"RESULT mode=$Mode hitches=$hc hitchMaxMs=$hm logicSumMs=$([math]::Round($ls,1)) logicMaxMs=$lm unaccSumMs=$([math]::Round($us,1)) unaccMaxMs=$um residencyEvents=$res explosions=$exp inputRegistered=$ftg errLines=$($errLines.Count) swingchain=$($swp.Line -replace '.*(2560x[0-9]+).*','$1')"
