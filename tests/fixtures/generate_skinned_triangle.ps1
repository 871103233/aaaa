# Generates tests/fixtures/skinned_triangle.gltf -- a minimal SKINNED glTF used by the T68 unit tests.
# Pure ASCII (PowerShell 5.1 reads .ps1 with the system code page).
# Run: powershell -ExecutionPolicy Bypass -File tests\fixtures\generate_skinned_triangle.ps1
#
# Layout (one buffer, 272 bytes, embedded as a base64 data URI):
#   0   .. 35   POSITION   3 x vec3 float
#   36  .. 47   JOINTS_0   3 x vec4 ubyte
#   48  .. 95   WEIGHTS_0  3 x vec4 float
#   96  .. 101  indices    3 x ushort   (+2 padding -> 104)
#   104 .. 231  inverse bind matrices  2 x mat4 float (column major)
#   232 .. 239  anim input times       2 x float (0, 1)
#   240 .. 271  anim output rotations  2 x vec4 quat (xyzw)
# Model: 3 vertices, 1 triangle, 2 joints (BoneA -> BoneB), 1 rotation clip "Wave".

$ErrorActionPreference = 'Stop'

$bytes = New-Object System.Collections.Generic.List[byte]

function Add-F32([double]$v) { $bytes.AddRange([BitConverter]::GetBytes([single]$v)) }
function Add-U16([int]$v)    { $bytes.AddRange([BitConverter]::GetBytes([uint16]$v)) }
function Add-U8([int]$v)     { $bytes.Add([byte]$v) }

# POSITION
foreach ($p in @(@(0.0, 0.0, 0.0), @(1.0, 0.0, 0.0), @(0.5, 1.0, 0.0))) {
    Add-F32 $p[0]; Add-F32 $p[1]; Add-F32 $p[2]
}
# JOINTS_0 (joint 0 = BoneA, joint 1 = BoneB)
foreach ($j in @(@(0, 0, 0, 0), @(0, 0, 0, 0), @(1, 0, 0, 0))) {
    foreach ($x in $j) { Add-U8 $x }
}
# WEIGHTS_0
foreach ($w in @(@(1.0, 0.0, 0.0, 0.0), @(1.0, 0.0, 0.0, 0.0), @(1.0, 0.0, 0.0, 0.0))) {
    foreach ($x in $w) { Add-F32 $x }
}
# indices
foreach ($i in @(0, 1, 2)) { Add-U16 $i }
# padding to a 4-byte boundary
Add-U8 0; Add-U8 0
# inverse bind matrices (column major): identity, translate(0, -1, 0)
foreach ($m in @(
        @(1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0),
        @(1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, -1.0, 0.0, 1.0))) {
    foreach ($x in $m) { Add-F32 $x }
}
# animation input times
Add-F32 0.0; Add-F32 1.0
# animation output rotations (xyzw): identity, 90 deg about Z
foreach ($q in @(@(0.0, 0.0, 0.0, 1.0), @(0.0, 0.0, 0.7071067811865476, 0.7071067811865476))) {
    foreach ($x in $q) { Add-F32 $x }
}

$buf = $bytes.ToArray()
if ($buf.Length -ne 272) { throw "unexpected buffer size: $($buf.Length)" }
$b64 = [Convert]::ToBase64String($buf)

$json = @"
{
  "asset": { "version": "2.0", "generator": "voxel-engine T68 fixture" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [
    { "name": "Mesh", "mesh": 0, "skin": 0, "children": [ 1 ] },
    { "name": "BoneA", "children": [ 2 ] },
    { "name": "BoneB", "translation": [ 0.0, 1.0, 0.0 ] }
  ],
  "meshes": [
    { "name": "Tri", "primitives": [
      { "attributes": { "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2 }, "indices": 3, "material": 0 } ] }
  ],
  "materials": [ { "name": "Default", "pbrMetallicRoughness": { "baseColorFactor": [ 1.0, 1.0, 1.0, 1.0 ] } } ],
  "skins": [ { "inverseBindMatrices": 4, "joints": [ 1, 2 ], "skeleton": 1 } ],
  "animations": [
    {
      "name": "Wave",
      "channels": [ { "sampler": 0, "target": { "node": 1, "path": "rotation" } } ],
      "samplers": [ { "input": 5, "output": 6, "interpolation": "LINEAR" } ]
    }
  ],
  "accessors": [
    { "bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ 0.0, 0.0, 0.0 ], "max": [ 1.0, 1.0, 0.0 ] },
    { "bufferView": 1, "byteOffset": 0, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 2, "byteOffset": 0, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 3, "byteOffset": 0, "componentType": 5123, "count": 3, "type": "SCALAR" },
    { "bufferView": 4, "byteOffset": 0, "componentType": 5126, "count": 2, "type": "MAT4" },
    { "bufferView": 5, "byteOffset": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [ 0.0 ], "max": [ 1.0 ] },
    { "bufferView": 6, "byteOffset": 0, "componentType": 5126, "count": 2, "type": "VEC4" }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,   "byteLength": 36,  "target": 34962 },
    { "buffer": 0, "byteOffset": 36,  "byteLength": 12,  "target": 34962 },
    { "buffer": 0, "byteOffset": 48,  "byteLength": 48,  "target": 34962 },
    { "buffer": 0, "byteOffset": 96,  "byteLength": 6,   "target": 34963 },
    { "buffer": 0, "byteOffset": 104, "byteLength": 128 },
    { "buffer": 0, "byteOffset": 232, "byteLength": 8 },
    { "buffer": 0, "byteOffset": 240, "byteLength": 32 }
  ],
  "buffers": [ { "byteLength": 272, "uri": "data:application/octet-stream;base64,$b64" } ]
}
"@

$out = Join-Path $PSScriptRoot 'skinned_triangle.gltf'
[System.IO.File]::WriteAllText($out, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "written: $out ($($buf.Length) byte buffer)"
