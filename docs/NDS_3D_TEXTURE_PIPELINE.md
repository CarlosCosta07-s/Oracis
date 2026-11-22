# NDS 3D Texture Pipeline

## Scope

This document records the first independently tested texture-sampling stage. It does **not** claim that the Nintendo DS 3D renderer or first-frame presentation is complete.

The current implementation is a deterministic, nearest-texel lookup. It receives:

- the captured `TEXIMAGE_PARAM` command value;
- the captured `PLTT_BASE` command value;
- signed S/T coordinates in the DS 1.11.4 texel format;
- the current `NdsVramControl` allocation state.

The sampler is deliberately separate from `NdsGeometryEngine`. Each emitted polygon now retains both `TEXIMAGE_PARAM` and the 13-bit `PLTT_BASE` value so a later renderer can sample using the state belonging to that polygon.

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
| 5 | 4x4 compressed | Explicitly unsupported; returns `UnsupportedFormat` |
| 6 | A5I3 | Implemented |
| 7 | Direct RGB555 | Implemented, including texel alpha bit |

Palette color-zero transparency is applied for formats 2, 3 and 4 when TEXIMAGE_PARAM bit 29 is set. A3I5 and A5I3 use their encoded alpha fields. Direct Color uses bit 15 as alpha.

## Coordinate addressing

The sampler converts signed 1.11.4 coordinates to integer texel coordinates using floor semantics. Each axis independently supports clamp, repeat and repeat+flip according to TEXIMAGE_PARAM.

This stage uses nearest-texel lookup only. It does not interpolate polygon coordinates, perform perspective correction, filter neighboring texels or composite texture output with vertex color.

## Regression coverage

The synthetic tests cover A3I5, 2bpp, 4bpp, 8bpp, A5I3 and Direct Color; alpha decoding; the format-2 palette-base unit; F-bank palette slot 4; clamp and repeat+flip; no-texture and compressed-format fail-closed results; unallocated slots; and ambiguous image/palette allocations. No ROM, BIOS, firmware or extracted game data is used.

## Remaining work

1. Decode 4x4 compressed texture blocks and their separate block/palette-index metadata.
2. Make texture-coordinate interpolation and perspective correction part of a scanline/pixel rasterizer.
3. Add texture/fragment cache policy and measured timing.
4. Blend texels with interpolated vertex color and polygon alpha according to the DS polygon mode.
5. Add depth, fog, translucency and final 3D framebuffer output.

The sampler alone does not prove that any game boots or renders correctly.

## Hardware reference

Register bitfields, texture formats and VRAM slot allocation are cross-checked against [GBATEK — Nintendo DS 3D Video](https://mgba-emu.github.io/gbatek/#ds-3d-video).
