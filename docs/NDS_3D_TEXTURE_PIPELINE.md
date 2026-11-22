# NDS 3D Texture Pipeline

## Scope

This document records the implemented texture lookup and the first scanline fragment-generation stage. It does **not** claim that the Nintendo DS 3D renderer, complete display composition or first-frame presentation is finished.

The `Nds3dTextureSampler` performs deterministic nearest-texel lookup. It receives the polygon-captured `TEXIMAGE_PARAM` and `PLTT_BASE`, signed S/T values in DS 1.11.4 format and current `NdsVramControl` allocation state.

The separate `Nds3dRasterizer` consumes the Geometry Engine's swapped render buffer and asks the sampler for the texel at each covered pixel. Polygon texture state is captured when the polygon is emitted, so an older polygon does not observe the latest global register values. `POLYGON_ATTR` alpha zero selects wireframe mode; it is not an ordinary transparent polygon. Transparency is evaluated after applying the polygon's texture mode: in Modulation, a zero-alpha texel produces a zero-alpha fragment that is discarded; in Decal, a zero-alpha texel selects the vertex RGB while the output alpha remains the vertex alpha.

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

The rasterizer reports modes 2/3 (toon/highlight and shadow), `POLYGON_ATTR` alpha zero (wireframe) and line-segment geometry as unsupported rather than silently substituting another mode. Fragments with final alpha zero are discarded after texture-mode blending; a zero-alpha texel in Decal mode still resolves to the vertex color and vertex alpha. `Nds3dDepthBuffer` is a separate 256×192, 24-bit depth stage, initialized to the far-depth value (`0xFFFFFF`) on construction; callers may override it with `clear(depth)` when a rear-plane clear value is supplied. It implements the normal Less comparison, Depth Equal with the documented inclusive ±0x200 tolerance, unconditional depth writes for opaque fragments and optional writes for translucent fragments controlled by `POLYGON_ATTR.Bit11`. Fully transparent fragments are rejected. The Geometry Engine now stores the two `SWAP_BUFFERS` option bits with the buffer being handed to the renderer, so a newly issued swap cannot retroactively change the depth mode of the frame already submitted. The rasterizer reads the render buffer's own bit 1 and emits either normalized Z depth or perspective-correct W depth. `render_scanline_depth_tested` connects fragment generation to depth rejection before forwarding surviving pixels. Depth Equal uses ±0x200 in Z mode and ±0xFF in W mode; the Z tolerance is documented by GBATEK, while the W-mode margin is a community reverse-engineered observation and remains a hardware-conformance target. The depth stage consumes fragments in the Geometry Engine's current polygon order. `Nds3dColorBuffer` now applies the DS polygon/framebuffer blend equation in RGB6/A5:
`(source * (alpha + 1) + destination * (31 - alpha)) / 32`.
It bypasses blending when `DISP3DCNT.Bit3` is disabled, the incoming pixel is opaque, or the destination alpha is zero; output alpha is the maximum of source and destination. `render_scanline_composited` performs the optional final-alpha comparison (`alpha > ALPHA_TEST_REF`) before depth writes, then runs depth testing and color composition. The display-control and alpha-reference values are still caller-supplied parameters, not yet wired to their MMIO registers. It currently accepts only buffers with manual translucent sorting selected; automatic Y-sorting (`SWAP_BUFFERS.Bit0=0`) returns `TranslucentSortingNotSupported` rather than rendering in an incorrect order. The staging buffer is a software 256×192 color array, not the DS hardware 48-line cache. Neither the color nor depth stage yet writes into that hardware cache. A generated fragment stream is not the same as a completed frame on the host display.

## Regression coverage

The synthetic sampler tests cover A3I5, 2bpp, 4bpp, 8bpp, A5I3, Direct Color and all four 4x4 compressed modes; alpha decoding; the format-2 palette-base unit; F-bank palette slot 4; clamp and repeat+flip; no-texture; slot-0/slot-1 and slot-2 compressed blocks (including the upper-half metadata route); unallocated slots; and ambiguous image/palette allocations.

The rasterizer integration test constructs Geometry Engine triangles, swaps their buffer at VBlank, samples direct-color VRAM texels and verifies the resulting center fragments. Its fixture uses different clip-space W values and texel colors chosen to distinguish perspective-correct sampling from affine interpolation. Overlapping polygons sample a direct-color texel with its alpha bit clear in both Modulation and Decal: Modulation is discarded because its final alpha is zero, while Decal preserves the vertex color/alpha. A separate swapped frame verifies that `POLYGON_ATTR` alpha zero is reported as unsupported wireframe instead of emitted as an interior fragment. The rasterizer regression checks that the first render buffer retains its old Z mode even when the same SWAP command selects W for the next build, then verifies perspective-correct W depth and the depth-tested callback on the second frame. The depth-buffer test covers Less, exact-equal rejection, Z ±0x200 and W ±0xFF Depth Equal boundaries, opaque writes, optional translucent writes and fully transparent rejection. The color-buffer test covers the RGB6/A5 blending equation, alpha max, blend-disable overwrite, transparent-rear-plane overwrite and alpha-zero rejection. The rasterizer integration test verifies that auto-sort mode fails closed, alpha-test rejection does not write depth, and a manual-order W-buffer scanline composites its surviving fragment. CI #1056 passed all four jobs (Desktop Linux, ARM runtime, Android ARM64 and pinned NDSRecomp); Desktop and ARM runtime each reported 53/53 CTests, including all three 3D tests. No ROM, BIOS, firmware or extracted game data is used.

## Remaining work

1. Implement automatic Y-sorting of translucent polygons and preserve manual submission order for `SWAP_BUFFERS.Bit0=1`; only manual-order composition is active.
2. Wire `DISP3DCNT` and `ALPHA_TEST_REF` MMIO registers into the compositor instead of supplying those flags as API arguments.
3. Implement toon/highlight and shadow modes, real wireframe edges, fog color/density and edge marking.
4. Connect fragment output to the DS 48-line hardware cache and the 2D Engine A BG0 3D display path.
5. Add texture/fragment cache policy, hardware timing, malformed-size/boundary vectors and more reference-driven conformance tests.

Neither the sampler nor the initial fragment producer proves that any game boots, displays a complete frame or is playable.

## Hardware reference

Register bitfields, texture formats and VRAM slot allocation are cross-checked against [GBATEK — Nintendo DS 3D Video](https://mgba-emu.github.io/gbatek/#ds-3d-video).
