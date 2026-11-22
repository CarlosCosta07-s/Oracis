# NDS 3D Texture Pipeline

## Scope

This document records texture lookup, scanline rasterization, fragment composition, the 48-line BG0 cache and the Engine A desktop presentation path. It does **not** claim cycle-accurate 3D scanout, complete DS rendering or a real-game first frame.

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
It bypasses blending when `DISP3DCNT.Bit3` is disabled, the incoming pixel is opaque, or the destination alpha is zero; output alpha is the maximum of source and destination. `render_scanline_composited` performs the optional final-alpha comparison (`alpha > ALPHA_TEST_REF`) before depth writes, then runs depth testing and color composition. `NdsDisplayControl` models the ARM9 MMIO registers `DISP3DCNT` (`0x04000060`), write-only `ALPHA_TEST_REF` (`0x04000340`), `CLEAR_COLOR` (`0x04000350`), `CLEAR_DEPTH` (`0x04000354`) and `CLRIMAGE_OFFSET` (`0x04000356`). The rasterizer overload reads the alpha controls directly from the display device. Underflow/overflow status bits 12/13 remain unimplemented and read zero. It currently accepts only buffers with manual translucent sorting selected; automatic Y-sorting (`SWAP_BUFFERS.Bit0=0`) returns `TranslucentSortingNotSupported` rather than rendering in an incorrect order. The `initialize_rear_plane` supports both rear-plane modes. With `DISP3DCNT.Bit14=0`, `CLEAR_COLOR` initializes RGB6/A5, fog-enable and polygon ID, while the 15-bit `CLEAR_DEPTH` is expanded to 24-bit with `raw × 0x200 + ((raw + 1) / 0x8000) × 0x1FF` (`0x7FFF → 0xFFFFFF`). With Bit14 set, the renderer reads two 256×256 halfword bitmaps from the physical VRAM image slots: slot 2 provides RGB555 and one-bit alpha (bit15, 0=transparent/31=solid); slot 3 provides 15-bit depth plus the per-pixel fog-enable flag in bit15. The polygon ID continues to come from `CLEAR_COLOR`. Only the visible 256×192 window is copied to the software staging surfaces. `CLRIMAGE_OFFSET` is a full 16-bit pair of offsets: low byte is horizontal/column (0=left edge), high byte is vertical/row (0=top edge); each wraps modulo 256. Both slots are preflighted over the visible window before either output buffer is changed, and a missing/overlapping/invalid source returns `TextureFailure` without leaving a partial rear-plane. The color/depth buffers remain software 256×192 staging surfaces. `render_scanline_to_cache` exports a composed scanline into `Nds3dScanlineCache`: it checks producer order and capacity before rendering, converts RGB6 channels to BG0 RGB555 by dropping each channel's low bit, preserves alpha5 and enqueues one 256-pixel line. The queue is a bounded ring with capacity 48. Engine A's 2D compositor now has a cache-aware BG0 overload: it peeks the line matching the requested scanline and removes it only after the entire 2D composition succeeds. On cache underflow or failed BG0 composition, caller output remains unchanged; failed composition also leaves the pending 3D line queued. `NdsMachine::prepare_3d_bg0_scanline` now owns the texture sampler, rasterizer, color/depth staging buffers and 48-line cache. When Engine A uses BG0 as 3D in graphics display mode, the SDL desktop host prepares the requested line plus up to 47 following lines and passes the cache to the cache-aware 2D compositor. Each successful composition consumes one line; the next host scanline request advances the lookahead window. A skipped line or late activation of 3D BG0 resets the queue and rear-plane at the requested line. Rasterizer failures have a separate diagnostic field from 2D compositor failures. This connects 3D output to the desktop display path, but production is requested by the host scanline loop: it is not scheduled from VBlank pre-render or VCOUNT, and missed-line timing, hardware FIFO underflow/overflow flags and RDLINES_COUNT remain unimplemented. This is a functional software lookahead pipeline, not cycle-accurate hardware scanout.

## Regression coverage

The synthetic sampler tests cover A3I5, 2bpp, 4bpp, 8bpp, A5I3, Direct Color and all four 4x4 compressed modes; alpha decoding; the format-2 palette-base unit; F-bank palette slot 4; clamp and repeat+flip; no-texture; slot-0/slot-1 and slot-2 compressed blocks (including the upper-half metadata route); unallocated slots; and ambiguous image/palette allocations.

The rasterizer integration test constructs Geometry Engine triangles, swaps their buffer at VBlank, samples direct-color VRAM texels and verifies the resulting center fragments. Its fixture uses different clip-space W values and texel colors chosen to distinguish perspective-correct sampling from affine interpolation. Overlapping polygons sample a direct-color texel with its alpha bit clear in both Modulation and Decal: Modulation is discarded because its final alpha is zero, while Decal preserves the vertex color/alpha. A separate swapped frame verifies that `POLYGON_ATTR` alpha zero is reported as unsupported wireframe instead of emitted as an interior fragment. The rasterizer regression checks that the first render buffer retains its old Z mode even when the same SWAP command selects W for the next build, then verifies perspective-correct W depth and the depth-tested callback on the second frame. The depth-buffer test covers Less, exact-equal rejection, Z ±0x200 and W ±0xFF Depth Equal boundaries, opaque writes, optional translucent writes and fully transparent rejection. The color-buffer test covers the RGB6/A5 blending equation, alpha max, blend-disable overwrite, transparent-rear-plane overwrite and alpha-zero rejection. The rasterizer integration test verifies auto-sort fail-closed behavior, MMIO alpha-test rejection before depth writes and manual-order W-buffer composition. Rear-plane tests cover both clear modes, slots 2/3, RGB555-to-RGB6, binary alpha, per-pixel fog, depth conversion and both scroll wraps. The sampler tests aligned slot halfword reads and invalid/unmapped bounds. `CLRIMAGE_OFFSET` tests preserve all 16 bits. The new cache test covers 48-line capacity, full/empty states, producer/consumer order, ring wrap, reset boundaries and invalid pixels. Rasterizer integration checks RGB6-to-RGB555/alpha5 export; compositor tests cover BG0 FIFO consumption, underflow, unchanged output and retaining a line after composition failure. CI #1070 passed all four jobs (Desktop Linux, ARM runtime, Android ARM64 and pinned NDSRecomp); Desktop and ARM runtime each reported 54/54 CTests, and the optional static-bank build/audit job reported 1/1. No ROM, BIOS, firmware or extracted game data is used.

## Remaining work

1. Implement automatic Y-sorting of translucent polygons; manual-order composition is currently the only enabled path.
2. Replace host-requested lookahead with a producer scheduled from VBlank pre-render and synchronized VCOUNT/scanline timing; model missed lines, FIFO underflow/overflow behavior and `RDLINES_COUNT`.
3. Extend the now-integrated Engine A BG0 path to Android presentation and validate it against reference frames.
4. Complete DISP3DCNT underflow/overflow status producers, toon/highlight, shadow, real wireframe, fog and edge marking.
5. Add texture/fragment cache policy, cycle-accurate FIFO fill timing, malformed-size/boundary vectors and more conformance tests.

The 48-line FIFO and BG0 seam are implemented and tested, but cycle-accurate scheduling, real-game frames and boot/playability have not been demonstrated.

## Hardware reference

Register bitfields, texture formats and VRAM slot allocation are cross-checked against [GBATEK — Nintendo DS 3D Video](https://mgba-emu.github.io/gbatek/#ds-3d-video).


## Scanline-clock producer

The 48-line Engine A BG0 cache is no longer dependent solely on a host call to
render a lookahead batch. NdsDisplayControl can notify the owning machine at
each synchronized scanline boundary. NdsSystemClock now advances the display
and geometry devices in scanline-sized quanta, processing geometry time before
the display boundary callback and releasing SWAP_BUFFERS at the line-192
VBlank edge. The renderer starts at scanline 214: the machine initializes the
rear plane and primes the first 48 lines during VBlank, before the 262-to-0
frame wrap begins output at line zero. Subsequent visible-line boundaries
refill available FIFO slots while a full FIFO applies back-pressure.

This is event-synchronized at scanline granularity, not cycle-accurate polygon
rasterization. The callback does not claim to model the hardware's internal
rendering latency, RDLINES_COUNT history, or DISP3DCNT underflow/overflow
status. A host request can still initialize the pipeline when BG0-3D is enabled
mid-frame.


## DISP3DCNT sticky status

`DISP3DCNT` now retains bits 12/13 as sticky status and applies the
hardware write-one-to-clear acknowledgement rule. A host-side BG0 composition
failure caused by a missing 3D scanline source sets the color-buffer underflow
bit (12), which software can observe and acknowledge through MMIO. The polygon/
vertex RAM overflow flag (13) has the same register semantics and a device-side
report hook, but the Geometry Engine overflow producer is not connected yet.


The Geometry Engine now reports clipping/vertex-RAM and polygon-RAM exhaustion
to its owning machine at the point of failure. The machine latches
`DISP3DCNT` bit 13, so the overflow flag is produced by the actual polygon
submission path rather than only by a manual test hook. The machine integration
test submits 2,049 valid triangles to a 2,048-entry polygon RAM and verifies the
status register. The bit remains sticky and software-cleared using write-one
acknowledgement.


## RDLINES_COUNT low-water reporting

RDLINES_COUNT at 0x04000320 is exposed as an ARM9-only, read-only byte. At natural frame wrap, the machine starts tracking the minimum number of queued BG0 lines after each successful consumption. At the next scanline-214 pre-render boundary it publishes max(minimum - 2, 0), clamped to the documented 0..46 register range. This is a queue-level previous-frame low-water approximation; it does not reproduce the hardware's internal sub-scanline sampling latency or cycle-exact FIFO accounting.
