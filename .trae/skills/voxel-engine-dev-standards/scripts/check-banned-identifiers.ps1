#Requires -Version 5.1
<#
.SYNOPSIS
    Banned-identifier gate: scans source files for identifiers that must not appear
    in this project, and fails with a non-zero exit code when any is found.

.DESCRIPTION
    Execution-layer safeguard for the voxel-engine-dev-standards skill.
    Purpose: turn structural prohibitions from "documented convention" into a hard
    build failure, so enforcement no longer depends on the reader inverting a negation.

    The rule table in $rules below is the single source of truth for banned identifiers.
    The prose guidance stays positive; only this script names the banned tokens.

    Scope: only mechanically detectable prohibitions live here. Red lines that need
    semantic judgement (blocking IO on the main thread, float world coordinates,
    new/delete on hot paths, meshing before neighbours are lit) are NOT covered by this
    gate; they rely on the DoD checklist and code review. The boundary is documented in
    references/build-and-tests.md section 6.

    Matching is line based. When a rule genuinely must be mentioned (unit tests,
    generators, migration tools), waive it with an inline comment on the same line:

        // vx-allow: <rule-name>      waive one rule for this line
        // vx-allow: *                waive every rule for this line

.PARAMETER RepoRoot
    Repository root. When omitted, the script walks up from the current directory
    looking for .git / CMakePresets.json / CMakeLists.txt / vcpkg.json.

.PARAMETER IncludeDir
    Directories to scan, relative to the repository root. Default: engine voxel game editor tests.

.PARAMETER SelfTest
    Run the built-in fixture cases only (validates the rule regexes) and do not scan.

.EXAMPLE
    pwsh -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1

.EXAMPLE
    pwsh -File .trae/skills/voxel-engine-dev-standards/scripts/check-banned-identifiers.ps1 -SelfTest

.NOTES
    Exit codes: 0 = pass; 1 = violations found; 2 = usage or path error.
#>
[CmdletBinding()]
param(
    [string]   $RepoRoot,
    [string[]] $IncludeDir = @('engine', 'voxel', 'game', 'editor', 'tests'),
    [string[]] $IncludeExt = @('.h', '.hpp', '.hh', '.cpp', '.cxx', '.cc'),
    [string[]] $ExcludeDir = @('third_party', 'build', 'out', '.git', '.trae', 'external', 'vendor', 'cmake-build-debug', 'cmake-build-release'),
    [switch]   $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------- Rule table: single source of truth for banned identifiers ----------
$rules = @(
    [pscustomobject]@{
        Name     = 'no-cpp-modules'
        Pattern  = '^\s*(?:import|export)\s+module\b|^\s*import\s+[\w:<>.]+;'
        Required = 'use #include instead (this project does not use C++20 Modules)'
    }
    [pscustomobject]@{
        Name     = 'no-std-jthread'
        Pattern  = '\bstd::jthread\b|\bstd::stop_token\b|\bstd::stop_source\b'
        Required = 'use std::thread plus a stop flag'
    }
    [pscustomobject]@{
        Name     = 'no-std-async'
        Pattern  = '\bstd::async\b'
        Required = 'use enkits (vcpkg port name: enkits) for task scheduling'
    }
    [pscustomobject]@{
        Name     = 'no-rtti'
        Pattern  = '\bdynamic_cast\s*<|\btypeid\s*\('
        Required = 'use a tag or std::variant to discriminate types'
    }
    [pscustomobject]@{
        Name     = 'no-serialize-lib'
        Pattern  = 'cereal'
        Required = 'use the hand-written binary format plus zstd'
    }
    [pscustomobject]@{
        Name     = 'no-poisson'
        Pattern  = 'Poisson|Bridson'
        Required = 'structure generation uses Chunked Jittered Grid'
    }
    [pscustomobject]@{
        Name     = 'no-gl-direct'
        Pattern  = '#\s*include\s*[<"](?:GL/|glad/|glew/|GLFW/|glfw3)'
        Required = 'route rendering through SDL3_gpu (GL headers belong in the platform wrapper only)'
    }
    [pscustomobject]@{
        Name     = 'no-rand'
        Pattern  = '\b(?:rand|srand)\s*\('
        Required = 'generation must be deterministic: derive from the 64-bit seed (splitmix64 / xxhash)'
    }
)

function Get-RepoRootPath {
    param([string] $Start)

    $dir = if ($Start) { Get-Item -LiteralPath $Start } else { Get-Item -LiteralPath (Get-Location).Path }
    if (-not $dir.PSIsContainer) { $dir = $dir.Directory }

    while ($null -ne $dir) {
        foreach ($marker in @('.git', 'CMakePresets.json', 'CMakeLists.txt', 'vcpkg.json')) {
            if (Test-Path -LiteralPath (Join-Path $dir.FullName $marker)) { return $dir.FullName }
        }
        $dir = $dir.Parent
    }
    return $null
}

function Invoke-SelfTest {
    $cases = @(
        @{ Line = 'import math;';                         Rule = 'no-cpp-modules';   Expect = $true  }
        @{ Line = '#include <vector>';                    Rule = 'no-cpp-modules';   Expect = $false }
        @{ Line = 'auto t = std::jthread(worker);';       Rule = 'no-std-jthread';   Expect = $true  }
        @{ Line = 'auto t = std::thread(worker);';        Rule = 'no-std-jthread';   Expect = $false }
        @{ Line = 'auto f = std::async(loadChunk);';      Rule = 'no-std-async';     Expect = $true  }
        @{ Line = 'auto p = dynamic_cast<Foo*>(base);';   Rule = 'no-rtti';          Expect = $true  }
        @{ Line = 'auto n = typeid(x).name();';           Rule = 'no-rtti';          Expect = $true  }
        @{ Line = 'struct T { int typeid_hint; };';       Rule = 'no-rtti';          Expect = $false }
        @{ Line = 'cereal::BinaryOutputArchive ar(out);'; Rule = 'no-serialize-lib'; Expect = $true  }
        @{ Line = 'bool usePoisson = false;';             Rule = 'no-poisson';       Expect = $true  }
        @{ Line = 'PoissonDiskSampler sampler;';          Rule = 'no-poisson';       Expect = $true  }
        @{ Line = 'ChunkedJitteredGrid grid(seed);';      Rule = 'no-poisson';       Expect = $false }
        @{ Line = '#include <glad/glad.h>';               Rule = 'no-gl-direct';     Expect = $true  }
        @{ Line = '#include <SDL3/SDL_gpu.h>';            Rule = 'no-gl-direct';     Expect = $false }
        @{ Line = 'int v = rand() % 4;';                  Rule = 'no-rand';          Expect = $true  }
        @{ Line = 'srand(1234);';                         Rule = 'no-rand';          Expect = $true  }
        @{ Line = 'int v = splitmix64(seed)';             Rule = 'no-rand';          Expect = $false }
        @{ Line = 'int v = RandomRange(a, b);';           Rule = 'no-rand';          Expect = $false }
    )

    $failed = 0
    Write-Host 'Gate self-test:'
    foreach ($case in $cases) {
        $rule = $rules | Where-Object { $_.Name -eq $case.Rule }
        if (-not $rule) {
            Write-Host ("  [FAIL] unknown rule: {0}" -f $case.Rule) -ForegroundColor Red
            $failed++
            continue
        }
        $hit = [bool]($case.Line -match $rule.Pattern)
        if ($hit -eq $case.Expect) {
            Write-Host ("  [ OK ] {0,-17} {1}" -f $case.Rule, $case.Line)
        }
        else {
            Write-Host ("  [FAIL] {0,-17} expected {1} but got {2}: {3}" -f $case.Rule, $case.Expect, $hit, $case.Line) -ForegroundColor Red
            $failed++
        }
    }

    if ($failed -gt 0) {
        Write-Host ("Self-test FAILED: {0} case(s)." -f $failed) -ForegroundColor Red
        return 1
    }
    Write-Host ("Self-test passed: {0} case(s)." -f $cases.Count) -ForegroundColor Green
    return 0
}

if ($SelfTest) { exit (Invoke-SelfTest) }

# ---------- Resolve repository root ----------
$rootPath = $null
if ($RepoRoot) {
    if (-not (Test-Path -LiteralPath $RepoRoot)) {
        Write-Host ("Error: -RepoRoot does not exist: {0}" -f $RepoRoot) -ForegroundColor Red
        exit 2
    }
    $rootPath = (Resolve-Path -LiteralPath $RepoRoot).Path
}
else {
    $rootPath = Get-RepoRootPath
}

if (-not $rootPath) {
    Write-Host 'Error: repository root not found (no .git / CMakePresets.json / CMakeLists.txt / vcpkg.json). Pass -RepoRoot explicitly.' -ForegroundColor Red
    exit 2
}

# ---------- Collect files to scan ----------
$files = New-Object System.Collections.Generic.List[string]
foreach ($dirName in $IncludeDir) {
    $scanRoot = Join-Path $rootPath $dirName
    if (-not (Test-Path -LiteralPath $scanRoot)) { continue }

    Get-ChildItem -LiteralPath $scanRoot -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        if ($IncludeExt -notcontains $_.Extension.ToLowerInvariant()) { return }

        $rel = $_.FullName.Substring($rootPath.Length).TrimStart('\', '/')
        foreach ($segment in ($rel -split '[\\/]')) {
            if ($ExcludeDir -contains $segment) { return }
        }
        $files.Add($_.FullName)
    }
}

# ---------- Line-by-line evaluation ----------
$violations = New-Object System.Collections.Generic.List[object]
foreach ($file in $files) {
    $lines = $null
    try { $lines = [System.IO.File]::ReadAllLines($file) }
    catch { Write-Warning ("Skipped unreadable file: {0}" -f $file); continue }

    for ($i = 0; $i -lt $lines.Length; $i++) {
        $text = $lines[$i]

        $waived = @()
        $waiverMatch = [regex]::Match($text, 'vx-allow:\s*([A-Za-z0-9_,\-\*\s]+)')
        if ($waiverMatch.Success) {
            $waived = @($waiverMatch.Groups[1].Value -split '[,\s]+' | Where-Object { $_ })
        }
        if ($waived -contains '*') { continue }

        foreach ($rule in $rules) {
            if ($waived -contains $rule.Name) { continue }
            if ($text -match $rule.Pattern) {
                $violations.Add([pscustomobject]@{
                        File     = $file.Substring($rootPath.Length).TrimStart('\', '/')
                        Line     = $i + 1
                        Rule     = $rule.Name
                        Required = $rule.Required
                        Text     = $text.Trim()
                    })
            }
        }
    }
}

# ---------- Report ----------
Write-Host ''
Write-Host ("Banned-identifier gate: scanned {0} file(s), {1} violation(s)." -f $files.Count, $violations.Count)

if ($files.Count -eq 0) {
    Write-Host 'Note: no matching source files found (expected while the source tree does not exist yet).' -ForegroundColor Yellow
}

if ($violations.Count -eq 0) {
    Write-Host 'PASS' -ForegroundColor Green
    exit 0
}

foreach ($v in $violations) {
    Write-Host ("  {0}:{1}  [{2}]" -f $v.File, $v.Line, $v.Rule) -ForegroundColor Red
    Write-Host ("        Required : {0}" -f $v.Required)
    Write-Host ("        Source   : {0}" -f $v.Text)
}

Write-Host ''
Write-Host 'If a rule genuinely must be mentioned (tests / generators / migration tools), waive it inline: // vx-allow: <rule-name>' -ForegroundColor Yellow
exit 1
