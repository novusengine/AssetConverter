# WoW Material Program Manifest

AssetConverter writes `material_program_manifest.json` in `Data/Pact` for the
offline material cooker. The PACT `.material` assets remain the runtime source
of truth; the JSON is deterministic cook input generated from the same program
registry.

## Identity

`programKey` is XXHash64 of `canonicalDefinition`. Programs are sorted by this
key, and extraction fails if one key is registered with different canonical
bytes. `canonicalKey` is the lossless textual form `wow/<16 hex digits>`.
`programID` is only the diagnostic XOR-fold of the 64-bit key.

The canonical byte definition is little-endian and contains, in order:

1. manifest schema version, material ABI version, parameter-block size, and
   texture-unit count;
2. for each authored texture unit: shader ID bit pattern, layer, complete raw
   source material flags, raw source blend mode, authored texture count, full
   unit flags, normalized blend mode, source kind, and normalized unlit,
   unfogged, and two-sided values;
3. raster class, runtime material flags, and lighting model.

Converter-selected execution groups are intentionally excluded from both the
canonical definition and JSON. The offline cooker owns execution-group
assignment.

## Semantics

Each unit contains both raw source values and normalized values. `sourceMaterialKind`
identifies whether `sourceMaterialFlags` and `sourceBlendMode` use the M2 or WMO
definitions. This preserves flags that AssetConverter does not currently act
on while providing the normalized values used by the current Model V2 output.

The root `parameters` array is emitted in serialized order without sorting,
normalization, or repacking. `parameterLayoutHash` is calculated with
`FileFormat::Material::CalculateParameterLayoutHash` using that array and
`parameterBlockSize`.

`Batch/ValidateMaterialPact.ps1` independently checks the loose manifest
against every packed Material and MaterialInstance, including the 56-byte
MaterialAsset header, section offsets, parameter definitions, program keys,
diagnostic IDs, references, layout hash, and 96-byte parameter blocks.
