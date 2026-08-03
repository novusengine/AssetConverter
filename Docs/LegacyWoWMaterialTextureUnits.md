# Legacy WoW material TextureUnit investigation

This note records the August 2026 scan of the Classic Era PACT and the resulting material-layout discussion. It is design input, not a finalized contract.

## Measured content

The scan parsed all 11,184 serialized `.complexmodel` assets through the Engine `ComplexModel::Read` implementation. No assets failed to parse and no extraction was performed.

Across 82,025 serialized render batches:

| TextureUnits on one batch | Batches | Share |
| ---: | ---: | ---: |
| 1 | 81,398 | 99.24% |
| 2 | 540 | 0.66% |
| 3 | 87 | 0.11% |

- Maximum TextureUnits on one M2 batch: 3.
- Maximum TextureUnits on one WMO batch: 1.
- Three-unit batches occur in 35 unique models.
- Maximum textures referenced by one TextureUnit: 3.
- Maximum total texture references on one batch: 4.
- Seven batches use four textures, in either `2 + 1 + 1` or `2 + 2` arrangements.
- Of the 627 multi-unit batches, 609 (97.1%) use different blend modes between units.
- Observed material-layer indices are 0, 1, and 2.

Representative three-unit assets include Arthas/Lich King, Kel'Thuzad, water and poison elementals, Frostmourne-related weapons, and several raid items.

These counts describe this Classic Era corpus. They are not a promise about later WoW clients; the source format can represent more units.

## Data represented by a TextureUnit

The source M2 batch associates the following material-relevant data with each ordered TextureUnit:

- WoW shader/combiner ID.
- TextureUnit flags, including projected/transform-related behavior.
- Material-layer index.
- Material flags and blend mode.
- Offset and count into the texture-combination list.
- Color-animation index.
- UV-source/texgen lookup.
- Transparency/weight-animation lookup.
- UV-transform-animation lookup.

Each referenced texture additionally needs a texture binding, sampler/wrap state, and its resolved UV/transform association. Geometry selection, skin-section indices, and geoset indices are not material parameters. Priority-plane information is CPU draw-order metadata rather than shader parameter data.

## Candidate fixed legacy ABI

A general engine-wide layered-material graph is not required just to represent this content. One dedicated legacy-WoW material program can contain an ordered fixed-capacity unit array, with a one-unit fast path.

For the observed worst case, a clean static block fits in 128 bytes:

| Data | Worst case | Bytes |
| --- | ---: | ---: |
| Draw/material header | counts, flags, alpha cutoff | 16 |
| Base color | `float4` | 16 |
| Texture and sampler descriptors | 4 x `uint2` | 32 |
| TextureUnit descriptors | 3 x `uint4` | 48 |
| Unrounded total | | 112 |
| Suggested aligned allocation | | 128 |

A 16-byte packed TextureUnit can accommodate:

1. shader ID, unit flags, blend mode, and material flags;
2. texture offset/count, material layer, and other small routing fields;
3. color-animation and UV-source lookup indices;
4. transparency and texture-transform lookup indices.

AssetIDs do not need to enter this GPU block. Game can resolve MaterialInstance resource bindings to 32-bit bindless texture/sampler descriptors.

The current 96-byte placeholder ABI has enough raw capacity for three units and four textures, but its packed legacy words preserve only shader ID, texture count, material layer, and four flag bits. It currently omits blend-mode-per-unit, UV selection, color lookup, transform lookup, and transparency lookup.

## Per-instance animated state

Resolved animated values cannot live in the shared MaterialInstance when model instances may have independent animation clocks. The shared material should store lookup indices; optional per-model-instance storage should hold sampled values.

A deliberately conservative resolved-state upper bound is approximately 192 bytes per independently animated instance:

- four padded 2D UV transforms: 128 bytes;
- three animated color multipliers: 48 bytes;
- three transparency weights, aligned: 16 bytes.

Static instances should not pay this cost.

## Known conversion gaps

The existing converted output is sufficient for the cardinality measurements but is not a lossless source for all TextureUnit semantics:

- Legacy `ComplexModel::FromM2` does not assign `M2Batch::textureUnitLookupID` to the converted TextureUnit.
- `textureTransparencyLookupID` exists in the legacy in-memory type but is omitted by ComplexModel serialization.
- Model V2 currently retains shader ID, material, layer, flags, and texture references, but does not retain UV selection, color lookup, texture-transform lookup, or transparency-animation lookup.

Those fields should be recovered directly from M2 source data before finalizing the production legacy-WoW material ABI.
