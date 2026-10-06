# Bake the offline premade maps declared by the world manifests (docs/adr/0026).
#
# For every assets/maps/*.toml whose `source = "premade"`, run voxel_bake with the SAME terrain
# params the game loads (assets/config/terrain.toml) and the manifest's own terrain preset, writing
# the manifest's `premade_file` right next to it.
#
# The baked files are NOT committed (see .gitignore) - this script is the reproducible way to
# (re)create them, exactly like tools/fetch_assets.ps1 does for the CC0 art assets.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\bake_premade_maps.ps1
#
# Keep this file PURE ASCII (repo rule - PowerShell 5.1 decodes .ps1 with the system code page).
$ErrorActionPreference = 'Stop'

# Repository root is the parent of this script's folder (tools/), so the script works from any cwd.
$root    = Split-Path -Parent $PSScriptRoot
$exe     = Join-Path $root 'build\debug\bin\voxel_bake.exe'
$terrain = Join-Path $root 'assets\config\terrain.toml'
$mapDir  = Join-Path $root 'assets\maps'

if (-not (Test-Path $exe)) {
    throw "bake tool not found: $exe  (build the debug preset first: cmake --build --preset debug)"
}
if (-not (Test-Path $terrain)) { throw "terrain params not found: $terrain" }

$baked = 0
Get-ChildItem -Path $mapDir -Filter '*.toml' | ForEach-Object {
    $file = $_
    # Only world manifests declare `source`; terrain presets do not -> skip them.
    $text = Get-Content $file.FullName -Raw
    if ($text -notmatch '(?m)^\s*source\s*=\s*"premade"') { return }

    $premadeMatch = [regex]::Match($text, '(?m)^\s*premade_file\s*=\s*"([^"]+)"')
    if (-not $premadeMatch.Success) { throw "$($file.Name): source = premade but premade_file is missing" }
    $premade = $premadeMatch.Groups[1].Value

    $presetMatch = [regex]::Match($text, '(?m)^\s*terrain_preset\s*=\s*"([^"]+)"')
    if (-not $presetMatch.Success) { throw "$($file.Name): terrain_preset is missing" }
    $preset = $presetMatch.Groups[1].Value

    $presetPath = Join-Path $mapDir $preset
    $outPath    = Join-Path $mapDir $premade
    Write-Host "=== bake $($file.Name): $preset -> $premade ==="

    & $exe $terrain $presetPath $outPath
    if ($LASTEXITCODE -ne 0) { throw "voxel_bake failed for $($file.Name) (exit $LASTEXITCODE)" }

    $info = Get-Item $outPath
    $sha  = (Get-FileHash -Algorithm SHA256 -Path $outPath).Hash.ToLower()
    Write-Host ("    {0} bytes  sha256={1}" -f $info.Length, $sha)
    $baked++
}

if ($baked -eq 0) { Write-Host 'no world manifest declares source = "premade" (nothing to bake)' }
else { Write-Host "baked $baked premade map file(s)" }
