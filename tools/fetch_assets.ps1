<#
.SYNOPSIS
  取回本项目的美术资源（**资源不入库**）—— 全部为 CC0，由本脚本按需下载。

.DESCRIPTION
  依据：docs/plans/v0.3.md §1.1 第 2 条（所有者 2026-09-29 裁定：**仓库只放下载脚本 + 校验和 + 许可台账**，
  资源文件落地到被 `.gitignore` 排除的目录）。

  行为（**幂等**）：
    1. 逐项把压缩包下载到 `build/_assets_cache/`（已存在且**校验通过**则跳过）；
    2. 解压并只抽出需要的贴图（地表 PBR = albedo / normal / roughness / AO 四件套；HDRI = 单文件）；
       角色模型 = **单文件 GLB**（不解压，直接落到 `assets/models/`）；
       **整包模型**（V8）= 解压 ZIP 后**只抽选定的 GLB**（`Models/GLTF format/<name>.glb`）落到 `assets/models/nature/`；
    3. 校验 `tools/assets.sha256`（**这份校验和文件进仓库**）；`-Record` 为首次运行：写入该校验和文件；
    4. 打印台账（路径 + SHA-256），供 `NOTICE.md` 的「美术资源台账」引用。

  许可：ambientCG 与 Poly Haven 的资源均为 **CC0 1.0**（可商用、无需署名；本项目仍逐项登记来源）。
  **角色模型**（T69 起）：Quaternius（经 Cinevva 分发）为 **CC0**；同样逐项登记来源与 SHA-256。
  **自然物整包**（V8 起）：Kenney《Nature Kit》为 **CC0 1.0**。
  **Quixel Megascans 不可用**（UE-Only Content）—— 见 NOTICE.md。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1 -Record
  # 首次：下载 + 写 tools\assets.sha256（随后把该文件一起提交）

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\fetch_assets.ps1
  # 之后：校验（缺文件时补齐；上游文件变化会明确报错，不静默接受）
#>
[CmdletBinding()]
param(
    # 仓库根目录；缺省 = 本脚本所在目录的上一级。
    [string]$Root = '',

    # 首次运行：把算出的校验和写入 tools/assets.sha256（之后该文件应提交进仓库）。
    [switch]$Record,

    # 重新下载（忽略缓存）。
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = (Resolve-Path -LiteralPath $Root).Path

$cacheDir    = Join-Path $Root 'build\_assets_cache'
$hashFile    = Join-Path $Root 'tools\assets.sha256'
$textureRoot = Join-Path $Root 'assets\textures'
$modelRoot   = Join-Path $Root 'assets\models'

# ---------------------------------------------------------------
# 资源清单（唯一事实来源：URL + 目标位置 + 抽取规则）
# 每一项都必须在 NOTICE.md 的「美术资源台账」里有一行（来源 / 许可 / 作者 / 采集日期 / SHA-256）。
# ---------------------------------------------------------------
$items = @(
    [pscustomobject]@{
        Id      = 'grass'
        Display = 'Grass 001'
        Source  = 'ambientCG'
        Page    = 'https://ambientcg.com/view?id=Grass001'
        Url     = 'https://ambientcg.com/get?file=Grass001_2K-JPG.zip'
        Archive = 'Grass001_2K-JPG.zip'
        Kind    = 'material'
    }
    [pscustomobject]@{
        Id      = 'dirt'
        Display = 'Ground 037'
        Source  = 'ambientCG'
        Page    = 'https://ambientcg.com/view?id=Ground037'
        Url     = 'https://ambientcg.com/get?file=Ground037_2K-JPG.zip'
        Archive = 'Ground037_2K-JPG.zip'
        Kind    = 'material'
    }
    [pscustomobject]@{
        Id      = 'rock'
        Display = 'Rock 030'
        Source  = 'ambientCG'
        Page    = 'https://ambientcg.com/view?id=Rock030'
        Url     = 'https://ambientcg.com/get?file=Rock030_2K-JPG.zip'
        Archive = 'Rock030_2K-JPG.zip'
        Kind    = 'material'
    }
    [pscustomobject]@{
        Id      = 'sand'
        Display = 'Ground 093 C（沙漠 / 沙丘）'
        Source  = 'ambientCG'
        Page    = 'https://ambientcg.com/view?id=Ground093C'
        Url     = 'https://ambientcg.com/get?file=Ground093C_2K-JPG.zip'
        Archive = 'Ground093C_2K-JPG.zip'
        Kind    = 'material'
    }
    [pscustomobject]@{
        Id       = 'env-daylight'
        Display  = 'Kloofendal 48d Partly Cloudy（户外晴天）'
        Source   = 'Poly Haven'
        Page     = 'https://polyhaven.com/a/kloofendal_48d_partly_cloudy'
        Url      = 'https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_2k.hdr'
        Archive  = 'kloofendal_48d_partly_cloudy_2k.hdr'
        Kind     = 'env'
        OutDir   = 'env'
        OutName  = 'kloofendal_48d_partly_cloudy_2k.hdr'
    }
    [pscustomobject]@{
        # T69 占位主角：Quaternius《Casual Female》，CC0，23 关节，含 Idle/Walk/Run/Jump。
        Id       = 'character-casual-female'
        Display  = 'Casual Female（占位人形；含 Idle/Walk/Run/Jump）'
        Source   = 'Quaternius（经 Cinevva 分发）'
        Page     = 'https://quaternius.com/'
        Url      = 'https://cdn.cinevva.com/assets/packs/quaternius/ultimate-animated-characters/Casual_Female.glb'
        Archive  = 'Casual_Female.glb'
        Kind     = 'model'
        OutDir   = 'character'
        OutName  = 'Casual_Female.glb'
    }
    [pscustomobject]@{
        # V8：A 世界素材充实 —— Kenney《Nature Kit》（CC0），低模自然物（树 / 灌木 / 草花 / 岩石 / 营地小道具）。
        # 官方直链（kenney.nl 资源页的内联下载）；ZIP 内 GLTF 位于 `Models/GLTF format/<name>.glb`。
        # 只抽**选定**的 12 个 GLB（整包 329 个 ⇒ 不整包入库，仍守"仓库只放脚本 + 校验和 + 台账"）。
        Id      = 'kenney-nature-kit'
        Display = 'Kenney Nature Kit（低模自然物；选抽 12 个）'
        Source  = 'Kenney'
        Page    = 'https://kenney.nl/assets/nature-kit'
        Url     = 'https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip'
        Archive = 'kenney_nature-kit.zip'
        Kind    = 'modelzip'
        OutDir  = 'nature'
        Models  = @('tree_default', 'tree_pineTallB', 'tree_oak', 'plant_bush', 'grass', 'flower_redA',
                    'mushroom_red', 'plant_flatTall', 'rock_largeA', 'rock_smallB', 'campfire_logs',
                    'fence_simple')
    }
)

# 地表材质四件套：抽取时的**源后缀** → 统一输出名（口径见 docs/plans/v0.3.md T66）。
$mapSuffixes = [ordered]@{
    'albedo'    = '_Color.jpg'
    'normal'    = '_NormalGL.jpg'
    'roughness' = '_Roughness.jpg'
    'ao'        = '_AmbientOcclusion.jpg'
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RelativePath([string]$Base, [string]$Path) {
    $baseUri = New-Object System.Uri ($Base.TrimEnd('\') + '\')
    $pathUri = New-Object System.Uri $Path
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', '\')
}

# ---- 读取既有校验和（`-Record` 时不读，直接覆盖）----
$expected = @{}
if ((Test-Path -LiteralPath $hashFile) -and (-not $Record)) {
    foreach ($line in Get-Content -LiteralPath $hashFile) {
        if ($line -match '^\s*([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') {
            $expected[$matches[2].Replace('\', '/')] = $matches[1].ToLowerInvariant()
        }
    }
}

New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

$produced = New-Object System.Collections.Generic.List[string]
$ledger   = New-Object System.Collections.Generic.List[object]

function Assert-Hash([string]$Path, [string]$Label) {
    $rel    = (Get-RelativePath $Root $Path).Replace('\', '/')
    $actual = Get-Sha256 $Path
    if ($expected.ContainsKey($rel)) {
        if ($expected[$rel] -ne $actual) {
            throw "$Label 的 SHA-256 与 tools/assets.sha256 不一致（上游文件已变化？）`n  文件：$rel`n  期望：$($expected[$rel])`n  实际：$actual`n  处理：核对来源与许可后，用 -Record 重新记录，并把该文件的变更写进 NOTICE.md 与 docs/devlog.md。"
        }
    }
    return $actual
}

foreach ($item in $items) {
    Write-Host ("== {0}（{1}，{2}）" -f $item.Display, $item.Source, $item.Id) -ForegroundColor Cyan

    # ---- 1) 下载（幂等）----
    $archivePath = Join-Path $cacheDir $item.Archive
    if ($Force -or (-not (Test-Path -LiteralPath $archivePath))) {
        Write-Host ("   下载 {0}" -f $item.Url)
        Invoke-WebRequest -Uri $item.Url -OutFile $archivePath -TimeoutSec 600 -UseBasicParsing
    } else {
        Write-Host "   压缩包已在缓存中，跳过下载"
    }
    $archiveHash = Assert-Hash $archivePath ("压缩包 " + $item.Archive)

    # ---- 2) 产出 ----
    if ($item.Kind -eq 'model') {
        # 单文件模型（GLB）：不解压，直接落到 assets/models/<OutDir>/。
        $outDir = Join-Path $modelRoot $item.OutDir
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        $outPath = Join-Path $outDir $item.OutName
        Copy-Item -LiteralPath $archivePath -Destination $outPath -Force
        $produced.Add($outPath)
        $ledger.Add([pscustomobject]@{ Item = $item; File = $outPath; Hash = (Get-Sha256 $outPath) })
        continue
    }

    if ($item.Kind -eq 'modelzip') {
        # V8：从整包 ZIP 中**只抽选定的 GLB**（`Models/GLTF format/<name>.glb`）⇒ `assets/models/<OutDir>/<name>.glb`。
        # 名字**大小写敏感**比对（Kenney 用 camelCase，如 `tree_pineTallB`）⇒ 用 `-ceq`。
        # 找不到即 **throw**（上游改名 / 换包必须显式暴露，不静默少抽）。
        $tempDir = Join-Path $cacheDir ('_extract_' + $item.Id)
        if (Test-Path -LiteralPath $tempDir) {
            Remove-Item -LiteralPath $tempDir -Recurse -Force
        }
        Expand-Archive -LiteralPath $archivePath -DestinationPath $tempDir -Force

        $outDir = Join-Path $modelRoot $item.OutDir
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null

        $sources = Get-ChildItem -LiteralPath $tempDir -Recurse -File -Filter '*.glb'
        foreach ($name in $item.Models) {
            $match = $sources | Where-Object { $_.BaseName -ceq $name } | Select-Object -First 1
            if ($null -eq $match) {
                throw ("{0}：整包内找不到模型 [{1}].glb（上游改名 / 换包？）" -f $item.Id, $name)
            }
            $outPath = Join-Path $outDir ($name + '.glb')
            Copy-Item -LiteralPath $match.FullName -Destination $outPath -Force
            $produced.Add($outPath)
            $ledger.Add([pscustomobject]@{ Item = $item; File = $outPath; Hash = (Get-Sha256 $outPath) })
        }
        Remove-Item -LiteralPath $tempDir -Recurse -Force
        continue
    }

    if ($item.Kind -eq 'env') {
        $outDir = Join-Path $textureRoot $item.OutDir
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        $outPath = Join-Path $outDir $item.OutName
        Copy-Item -LiteralPath $archivePath -Destination $outPath -Force
        $produced.Add($outPath)
        $ledger.Add([pscustomobject]@{ Item = $item; File = $outPath; Hash = (Get-Sha256 $outPath) })
        continue
    }

    # material：解压 → 抽四件套 → 删临时目录
    $tempDir = Join-Path $cacheDir ('_extract_' + $item.Id)
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $tempDir -Force

    $outDir = Join-Path (Join-Path $textureRoot 'terrain') $item.Id
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $sources = Get-ChildItem -LiteralPath $tempDir -Recurse -File
    foreach ($mapName in $mapSuffixes.Keys) {
        $suffix = $mapSuffixes[$mapName]
        $match  = $sources | Where-Object { $_.Name.EndsWith($suffix, [System.StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
        if ($null -eq $match) {
            Write-Warning ("   {0}：压缩包内没有 {1} ⇒ 跳过该贴图（T66 需按缺失处理）" -f $item.Id, $suffix)
            continue
        }
        $outPath = Join-Path $outDir ($mapName + $match.Extension)
        Copy-Item -LiteralPath $match.FullName -Destination $outPath -Force
        $produced.Add($outPath)
        $ledger.Add([pscustomobject]@{ Item = $item; File = $outPath; Hash = (Get-Sha256 $outPath) })
    }
    Remove-Item -LiteralPath $tempDir -Recurse -Force
}

# ---- 3) 校验和：`-Record` 写入，否则逐项核对 ----
$lines = New-Object System.Collections.Generic.List[string]
foreach ($path in ($produced | Sort-Object)) {
    $rel = (Get-RelativePath $Root $path).Replace('\', '/')
    $lines.Add(("{0}  {1}" -f (Get-Sha256 $path), $rel))
}

if ($Record) {
    Set-Content -LiteralPath $hashFile -Value $lines -Encoding ASCII
    Write-Host ("已写入校验和：{0}（共 {1} 项）—— 请把它与 NOTICE.md 一起提交" -f $hashFile, $lines.Count) -ForegroundColor Yellow
} else {
    if (-not (Test-Path -LiteralPath $hashFile)) {
        throw "缺少 $hashFile ⇒ 请先用 -Record 运行一次（记录校验和），再提交该文件。"
    }
    $missing = $lines | Where-Object { -not $expected.ContainsKey(($_ -replace '^[0-9a-f]{64}\s+', '')) }
    foreach ($line in $lines) {
        $rel    = $line -replace '^[0-9a-f]{64}\s+', ''
        $actual = $line -replace '\s+.*$', ''
        if (-not $expected.ContainsKey($rel)) {
            throw "tools/assets.sha256 缺少条目：$rel（请用 -Record 重新记录）"
        }
        if ($expected[$rel] -ne $actual) {
            throw "文件与校验和不一致：$rel`n  期望：$($expected[$rel])`n  实际：$actual"
        }
    }
    Write-Host "校验通过：全部资源与 tools/assets.sha256 一致" -ForegroundColor Green
}

# ---- 4) 台账（供 NOTICE.md 引用）----
Write-Host ''
Write-Host '美术资源台账（逐项抄进 NOTICE.md）：' -ForegroundColor Cyan
foreach ($entry in $ledger) {
    $rel = (Get-RelativePath $Root $entry.File).Replace('\', '/')
    Write-Host ("  {0,-9} {1,-34} {2,-12} {3}" -f $entry.Item.Id, $rel, $entry.Item.Source, $entry.Hash)
}
Write-Host ''
Write-Host '来源页（登记用）：'
foreach ($item in $items) {
    Write-Host ("  {0,-12} {1,-12} {2}" -f $item.Id, $item.Source, $item.Page)
}
