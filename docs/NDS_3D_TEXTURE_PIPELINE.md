# NDS 3D Texture Pipeline

## Scope

This document records the implemented texture lookup and the first scanline fragment-generation stage. It does **not** claim that the Nintendo DS 3D renderer, complete display composition or first-frame presentation is finished.

The `Nds3dTextureSampler` performs deterministic nearest-texel lookup. It receives the polygon-captured `TEXIMAGE_PARAM` and `PLTT_BASE`, signed S/T values in DS 1.11.4 format and current `NdsVramControl` allocation state.

The separate `Nds3dRasterizer` consumes the Geometry Engine's swapped render buffer and asks the sampler for the texel at each covered pixel. Polygon texture state is captured when the polygon is emitted, so an older polygon does not observe the latest global register values.

## VRAM access contract

Texture image memory is exposed as four 128 KiB slots. A slot read succeeds only when exactly one of VRAM A-D is enabled for texture use (MST=3) at the requested OFS slot.

Texture palette memory is exposed as six 16 KiB slots:

- VRAM E supplies palette slots 0-3.
- VRAM F/G can each select slot 0, 1, 4 or 5 through OFS.
- Multiple banks claiming the same logical slot are reported as an overlap; the sampler does not pick one arbitrarily.

The methods on `NdsVramControl` are internal device reads. They bypass guest-CPU visibility, but they do not bypass the bank-allocation check. Texture/palette banks are not made CPU-visible merely because the 3D engine can read them.

## Register state

`PLTT_BASE` (command 0x2B) is latched as a 13-bit value and copied into each emitted polygon. It is a unit-based register value, not a byte address:

- Format 2 uses 8-byte units.
- Other palette formats use 16-byte units.
- Format 7 (Direct Color) does not use a palette.

The polygon captures this value alongside `TEXIMAGE_PARAM`; the renderer must not read the latest global register when rendering an older polygon.

## Implemented texel formats

| TEXIMAGE_PARAM format | Encoding | Current support |
|---:|---|---|
| 0 | No texture | Explicitly reported as `NoTexture` |
| 1 | A3I5 | Implemented, including 3-bit to 5-bit alpha expansion |
| 2 | 2bpp / 4 colors | Implemented, including 8-byte PLTT_BASE units |
| 3 | 4bpp / 16 colors | Implemented |
| 4 | 8bpp / 256 colors | Implemented |
| 5 | 4x4 compressed | Implemented: 2bpp block payload, slot-1 metadata, all four transparency/interpolation modes |
| 6 | A5I3 | Implemented |
| 7 | Direct RGB555 | Implemented, including texel alpha bit |

Palette color-zero transparency is applied for formats 2, 3 and 4 when TEXIMAGE_PARAM bit 29 is set. A3I5 and A5I3 use their encoded alpha fields. Direct Color uses bit 15 as alpha.

## Coordinate addressing and scanline rasterization

The sampler converts signed 1.11.4 coordinates to integer texel coordinates using floor semantics. Each axis independently supports clamp, repeat and repeat+flip according to TEXIMAGE_PARAM.

The initial `Nds3dRasterizer` stage:
- walks the swapped Geometry Engine render buffer and triangulates clipped polygons as a fan;
- tests pixel centers with a top-left edge rule to avoid double coverage on shared edges;
- interpolates vertex RGB555 values in screen space;
- interpolates S/T with a perspective-correct barycentric ratio using each vertex's clip-space W;
- samples the polygon's captured texture state and emits RGB6/A6 fragments plus normalized depth, polygon ID and fog-enable metadata;
- implements the initial DS polygon texture modes 0 (modulation) and 1 (decal).

The rasterizer reports modes 2/3 (toon/highlight and shadow) and alpha-zero wireframe cases as unsupported rather than silently substituting another mode. Its callback is a fragment producer: it does not yet perform depth-buffer rejection, polygon sorting, final translucency composition, fog blending or write into the DS 48-line output cache. A generated fragment stream is not the same as a completed frame on the host display.

## Regression coverage

The synthetic sampler tests cover A3I5, 2bpp, 4bpp, 8bpp, A5I3, Direct Color and all four 4x4 compressed modes; alpha decoding; the format-2 palette-base unit; F-bank palette slot 4; clamp and repeat+flip; no-texture; slot-0/slot-1 and slot-2 compressed blocks (including the upper-half metadata route); unallocated slots; and ambiguous image/palette allocations.

The rasterizer integration test constructs a Geometry Engine triangle, swaps its buffer at VBlank, samples a direct-color VRAM texel and verifies the resulting center fragment. Its updated fixture uses different clip-space W values and texel colors chosen to distinguish perspective-correct sampling from affine interpolation. The test is currently being checked by CI workflow #1042. No ROM, BIOS, firmware or extracted game data is used.

## Remaining work

1. Complete and validate the perspective-correct rasterizer regression across Desktop Linux and Android ARM64.
2. Implement the DS Z/W depth-buffer compare, polygon priority/order and translucent fragment composition.
3. Implement the remaining polygon modes (toon/highlight and shadow), wireframe/edge cases, fog color/density and exact alpha behavior.
4. Connect fragment output to the DS 3D line cache and the 2D Engine A BG0 3D display path, including the 48-line handoff.
5. Add texture/fragment cache policy, hardware timing, malformed-size/boundary vectors and more reference-driven conformance tests.

Neither the sampler nor the initial fragment producer proves that any game boots, displays a complete frame or is playable.

## Hardware reference

Register bitfields, texture formats and VRAM slot allocation are cross-checked against [GBATEK — Nintendo DS 3D Video](https://mgba-emu.github.io/gbatek/#ds-3d-video).
