param(
    [Parameter(Mandatory = $false)]
    [string] $PactRoot = "C:\Program Files (x86)\World of Warcraft\_classic_era_\Data\Pact"
)

$ErrorActionPreference = "Stop"

function Read-UInt16([byte[]] $Bytes, [int] $Offset) { [BitConverter]::ToUInt16($Bytes, $Offset) }
function Read-UInt32([byte[]] $Bytes, [int] $Offset) { [BitConverter]::ToUInt32($Bytes, $Offset) }
function Read-UInt64([byte[]] $Bytes, [int] $Offset) { [BitConverter]::ToUInt64($Bytes, $Offset) }

function Read-PackedFile([IO.FileStream] $Stream, [UInt64] $Offset, [UInt32] $Size) {
    if ($Size -gt [int]::MaxValue) { throw "PACT entry is too large for the validator: $Size" }
    $bytes = [byte[]]::new([int] $Size)
    $Stream.Position = [int64] $Offset
    $read = 0
    while ($read -lt $bytes.Length) {
        $count = $Stream.Read($bytes, $read, $bytes.Length - $read)
        if ($count -eq 0) { throw "Unexpected end of packed data" }
        $read += $count
    }
    return ,$bytes
}

$manifestPath = Join-Path $PactRoot "material_program_manifest.json"
if (!(Test-Path -LiteralPath $manifestPath)) { throw "Missing material program manifest: $manifestPath" }
$programManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($programManifest.schemaVersion -ne 1 -or $programManifest.materialABIVersion -ne 1) {
    throw "Unsupported material program manifest version"
}
if ($programManifest.parameterBlockSize -ne 96 -or $programManifest.parameterBlockAlignment -ne 16) {
    throw "Unexpected material parameter block contract"
}
if ($programManifest.parameters.Count -ne 19) { throw "Expected 19 material parameters" }
if ($programManifest.programCount -ne $programManifest.programs.Count) { throw "Manifest programCount mismatch" }

$programsByKey = @{}
foreach ($program in $programManifest.programs) {
    if ($program.canonicalKey -notmatch '^wow/([0-9a-f]{16})$') { throw "Invalid canonicalKey: $($program.canonicalKey)" }
    $keyHex = $Matches[1]
    if ($programsByKey.ContainsKey($keyHex)) { throw "Duplicate canonical program key: $keyHex" }
    if (!$program.canonicalDefinition) { throw "Program $keyHex has no canonical definition" }
    if ($null -ne $program.executionGroupID -or $null -ne $program.materialExecutionGroupID) {
        throw "Program $keyHex contains converter-selected execution group routing"
    }
    foreach ($unit in $program.units) {
        foreach ($field in @("authoredShaderID", "textureCount", "layer", "flags", "blendMode", "sourceBlendMode",
            "sourceMaterialKind", "sourceMaterialFlags", "isUnlit", "isUnfogged", "isTwoSided")) {
            if ($null -eq $unit.$field) { throw "Program $keyHex unit is missing $field" }
        }
    }
    $programsByKey[$keyHex] = $program
}

$materials = @{}
$materialLayoutBytes = $null
$materialCount = 0
$instanceCount = 0
$textureCount = 0
$entryCount = 0
$instanceLayoutHash = $null

Get-ChildItem -LiteralPath (Join-Path $PactRoot "manifests") -Filter "pack_*.manifest" | Sort-Object Name | ForEach-Object {
    $packManifest = [IO.File]::ReadAllBytes($_.FullName)
    if ([Text.Encoding]::ASCII.GetString($packManifest, 0, 4) -ne "PAMF") { throw "Invalid PACT manifest magic: $($_.FullName)" }
    $numEntries = Read-UInt32 $packManifest 36
    $entriesOffset = 48
    $stringTableOffset = $entriesOffset + $numEntries * 40
    $stringBytes = Read-UInt32 $packManifest $stringTableOffset
    if ($stringTableOffset + 4 + $stringBytes -ne $packManifest.Length) { throw "Malformed string table: $($_.FullName)" }
    $pathBlob = [Text.Encoding]::UTF8.GetString($packManifest, $stringTableOffset + 4, $stringBytes)
    $paths = $pathBlob.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)

    $packDataPath = Join-Path (Join-Path $PactRoot "data") ($_.BaseName.Replace(".manifest", "") + ".bin")
    $packData = [IO.File]::Open($packDataPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        for ($i = 0; $i -lt $numEntries; ++$i) {
            $entryOffset = $entriesOffset + $i * 40
            $flags = Read-UInt32 $packManifest ($entryOffset + 8)
            if (($flags -band 0x18) -ne 0) { throw "Compressed/encrypted PACT entry is unsupported by this validator" }
            $pathIndex = Read-UInt32 $packManifest ($entryOffset + 12)
            if ($pathIndex -ge $paths.Count) { throw "Invalid PACT path index" }
            $path = $paths[$pathIndex]
            $pathHash = Read-UInt64 $packManifest ($entryOffset + 16)
            $dataOffset = Read-UInt64 $packManifest ($entryOffset + 24)
            $dataSize = Read-UInt32 $packManifest ($entryOffset + 32)
            ++$entryCount

            if ($path.EndsWith(".dds", [StringComparison]::OrdinalIgnoreCase)) { ++$textureCount; continue }
            if ($path.EndsWith(".material", [StringComparison]::OrdinalIgnoreCase)) {
                $bytes = Read-PackedFile $packData $dataOffset $dataSize
                if ($bytes.Length -lt 56) { throw "Material header is smaller than 56 bytes: $path" }
                $programKey = Read-UInt64 $bytes 8
                $programID = Read-UInt32 $bytes 16
                $parameterBlockSize = Read-UInt32 $bytes 32
                $parameterBlockAlignment = Read-UInt32 $bytes 36
                $parametersOffset = Read-UInt32 $bytes 40
                $numParameters = Read-UInt32 $bytes 44
                $defaultDataOffset = Read-UInt32 $bytes 48
                $defaultDataSize = Read-UInt32 $bytes 52
                $keyHex = $programKey.ToString("x16")
                $foldedID = [UInt32](($programKey -band [UInt64]0xffffffffL) -bxor ($programKey -shr 32))
                if ($path -ne "material/generated/wow/program_$keyHex.material") { throw "Material path/key mismatch: $path" }
                if ($programID -ne $foldedID) { throw "Diagnostic programID mismatch: $path" }
                if (!$programsByKey.ContainsKey($keyHex)) { throw "Material is absent from aggregate manifest: $path" }
                if ([UInt32]$programsByKey[$keyHex].programID -ne $programID) { throw "Manifest programID mismatch: $path" }
                if ($parameterBlockSize -ne 96 -or $parameterBlockAlignment -ne 16 -or $numParameters -ne 19 -or $defaultDataSize -ne 96) {
                    throw "Material parameter contract mismatch: $path"
                }
                if ($parametersOffset -ne 64 -or $defaultDataOffset -ne 368 -or $bytes.Length -ne 464) {
                    throw "Material does not use the expected 56-byte header/aligned section layout: $path"
                }
                $layoutBytes = $bytes[$parametersOffset..($parametersOffset + $numParameters * 16 - 1)]
                if ($null -eq $materialLayoutBytes) { $materialLayoutBytes = $layoutBytes }
                elseif ([Convert]::ToBase64String($layoutBytes) -ne [Convert]::ToBase64String($materialLayoutBytes)) {
                    throw "Material parameter definitions differ: $path"
                }
                $materials[$pathHash] = $keyHex
                ++$materialCount
                continue
            }
            if ($path.EndsWith(".materialinstance", [StringComparison]::OrdinalIgnoreCase)) {
                $bytes = Read-PackedFile $packData $dataOffset $dataSize
                if ($bytes.Length -lt 48) { throw "MaterialInstance header is too small: $path" }
                $materialAssetID = Read-UInt64 $bytes 8
                $layoutHash = Read-UInt64 $bytes 16
                $parameterDataSize = Read-UInt32 $bytes 28
                if ($parameterDataSize -ne 96) { throw "MaterialInstance parameter block is not 96 bytes: $path" }
                if ($null -eq $instanceLayoutHash) { $instanceLayoutHash = $layoutHash }
                elseif ($instanceLayoutHash -ne $layoutHash) { throw "MaterialInstance layout hashes differ: $path" }
                if (!$materials.ContainsKey($materialAssetID)) {
                    # Materials can appear later in a pack, so resolve this after all manifests are read.
                    $script:pendingMaterialReferences += [UInt64]$materialAssetID
                }
                ++$instanceCount
            }
        }
    }
    finally { $packData.Dispose() }
}

foreach ($materialAssetID in $script:pendingMaterialReferences) {
    if (!$materials.ContainsKey($materialAssetID)) { throw "MaterialInstance references missing Material AssetID $materialAssetID" }
}
if ($materialCount -ne $programManifest.programCount) { throw "PACT has $materialCount Materials but manifest has $($programManifest.programCount) programs" }
if ($materials.Count -ne $materialCount) { throw "Duplicate Material paths/AssetIDs found" }
if ($instanceLayoutHash -ne [UInt64]$programManifest.parameterLayoutHash) { throw "MaterialInstance/manifest layout hash mismatch" }
if ($textureCount -eq 0) { throw "PACT contains no textures" }
for ($i = 0; $i -lt $programManifest.parameters.Count; ++$i) {
    $offset = $i * 16
    $serialized = $programManifest.parameters[$i]
    if ([UInt64]$serialized.nameHash -ne (Read-UInt64 $materialLayoutBytes $offset) -or
        [UInt32]$serialized.byteOffset -ne (Read-UInt32 $materialLayoutBytes ($offset + 8)) -or
        [UInt16]$serialized.byteSize -ne (Read-UInt16 $materialLayoutBytes ($offset + 12)) -or
        [byte]$serialized.type -ne $materialLayoutBytes[$offset + 14] -or
        [byte]$serialized.arrayCount -ne $materialLayoutBytes[$offset + 15]) {
        throw "Aggregate manifest parameter $i differs from the serialized Material layout"
    }
}

[pscustomobject]@{
    Entries = $entryCount
    Materials = $materialCount
    MaterialInstances = $instanceCount
    TextureDDS = $textureCount
    MaterialHeaderBytes = 56
    ParameterDefinitions = $programManifest.parameters.Count
    ParameterBlockBytes = $programManifest.parameterBlockSize
    ProgramLayoutHash = [UInt64]$programManifest.parameterLayoutHash
    Programs = $programManifest.programCount
}
