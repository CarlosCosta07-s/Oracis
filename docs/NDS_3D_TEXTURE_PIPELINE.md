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

`EDGE_COLOR` (`0x04000330..0x0400033F`) is modeled as eight ARM9 write-only
RGB555 entries. `DISP3DCNT.Bit5` selects edge marking, and a polygon uses
entry `polygon_id >> 3` (eight consecutive polygon IDs per color). Opaque
perimeter fragments flag candidate pixels when the pixel footprint touches a
polygon boundary; fan-triangulation diagonals are excluded. The final pass
waits for the next scanline, then replaces candidate RGB only when at least
one of the four orthogonal neighbors has a different polygon ID and strictly
greater depth. Screen-border comparisons use the rear-plane ID from
`CLEAR_COLOR` and `CLEAR_DEPTH` as the outside neighbor. The cache producer
therefore holds one composed line and accepts scanline 192 as a tail flush for
line 191. Fog runs after edge-color replacement in this cached output path.
The perimeter coverage rule is a deterministic pixel-footprint approximation
and still needs comparison against physical DS captures, especially for
excluded lower/right polygon edges and wireframe/antialias interactions.

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
- implements polygon texture modes 0 (modulation), 1 (decal) and 2
  (toon/highlight), using the 32-entry TOON_TABLE selected by the integrated
  display-control path;
- renders alpha-zero wireframe polygons as opaque perimeter pixels, using the
  actual clipped polygon perimeter and excluding fan-triangulation diagonals.
  The current coverage approximation filters pixel centers already covered
  by the triangle rasterizer; it is not a cycle- or pixel-exact hardware model.

Polygon mode 2 now performs Toon or Highlight shading. The vertex color's
five-bit red component selects one of 32 RGB555 entries in the write-only
TOON_TABLE at 0x04000380..0x040003BF. The selected color is expanded to RGB6
and modulated with the texture using the DS six-bit equation; when
DISP3DCNT.Bit1 selects Highlight, the selected toon RGB is added to the
modulated result with saturation at 63. Fragment alpha continues to combine
texture alpha and vertex alpha. With no texture, the selected table color is
the fragment RGB. POLYGON_ATTR alpha zero (wireframe) and line-segment geometry remain
explicitly unsupported rather than silently substituted. Polygon mode 3
(Shadow) uses a separate per-pixel stencil buffer: ID 0 marks depth-passing
mask fragments without changing color; IDs 1..63 render the second pass only
when the stencil bit is clear and the receiver Attribute Buffer ID differs.
The second pass consumes/clears the stencil bit, blends vertex color using
polygon alpha, and stores the shadow ID in the Attribute Buffer so repeated
volumes with the same ID do not shade the pixel twice. The mode requires
manual polygon order, translucent alpha 1..30, ID 0 Back=Render/Front=Hide
mask polygons, followed by ID 1..63 Back=Render/Front=Render polygons. The
machine clears the stencil at the start of a new 3D scanline sequence and
keeps it across all scanlines of that frame. The direct compositor overload
without a stencil buffer fails closed for Shadow mode. Alpha-zero wireframe polygons emit only perimeter-band pixels and force their output alpha to 31; the optional alpha test runs on that fixed alpha, while their interiors remain transparent. The current wireframe coverage uses covered pixel centers and is an approximation pending hardware captures. Separate line-segment primitives remain unsupported. Fragments with final alpha zero are discarded after
texture-mode blending; a zero-alpha texel in Decal mode still resolves to the
vertex color and vertex alpha. `Nds3dDepthBuffer` is a separate 256×192, 24-bit depth stage, initialized to the far-depth value (`0xFFFFFF`) on construction; callers may override it with `clear(depth)` when a rear-plane clear value is supplied. It implements the normal Less comparison, Depth Equal with the documented inclusive ±0x200 tolerance, unconditional depth writes for opaque fragments and optional writes for translucent fragments controlled by `POLYGON_ATTR.Bit11`. Fully transparent fragments are rejected. The Geometry Engine now stores the two `SWAP_BUFFERS` option bits with the buffer being handed to the renderer, so a newly issued swap cannot retroactively change the depth mode of the frame already submitted. The rasterizer reads the render buffer's own bit 1 and emits either normalized Z depth or perspective-correct W depth. `render_scanline_depth_tested` connects fragment generation to depth rejection before forwarding surviving pixels. Depth Equal uses ±0x200 in Z mode and ±0xFF in W mode; the Z tolerance is documented by GBATEK, while the W-mode margin is a community reverse-engineered observation and remains a hardware-conformance target. The depth stage consumes opaque polygons first, followed by translucent polygons in their original submission order when manual SWAP_BUFFERS ordering is selected; opaque-only frames retain original order. Automatic Y-sort is still not implemented. `Nds3dColorBuffer` now applies the DS polygon/framebuffer blend equation in RGB6/A5:
`(source * (alpha + 1) + destination * (31 - alpha)) / 32`.
It bypasses blending when `DISP3DCNT.Bit3` is disabled, the incoming pixel is opaque, or the destination alpha is zero; output alpha is the maximum of source and destination. `render_scanline_composited` performs the optional final-alpha comparison (`alpha > ALPHA_TEST_REF`) before depth writes, then runs depth testing and color composition. `NdsDisplayControl` models the ARM9 MMIO registers `DISP3DCNT` (`0x04000060`), write-only `ALPHA_TEST_REF` (`0x04000340`), `CLEAR_COLOR` (`0x04000350`), `CLEAR_DEPTH` (`0x04000354`), `CLRIMAGE_OFFSET` (`0x04000356`) and `TOON_TABLE` (`0x04000380..0x040003BF`). `DISP3DCNT.Bit1` selects Toon (0) or Highlight (1); the MMIO-aware compositor supplies the table and selection to the rasterizer. The low-level scanline overload without display-control state remains fail-closed for polygon mode 2. The rasterizer overload reads the alpha controls directly from the display device. `DISP3DCNT` bits 12/13 are sticky status: a BG0 source failure latches bit 12 and actual Geometry Engine vertex/polygon RAM exhaustion latches bit 13; software acknowledges either with write-one-to-clear. With SWAP_BUFFERS bit 0 set (manual ordering), the renderer first submits all opaque polygons in their original relative order, then all translucent polygons in their original relative order. Translucent classification includes POLYGON_ATTR alpha 1..30 and A3I5/A5I3 texture formats. This preserves manually ordered Shadow mask/render pairs and matches the documented rule that translucent polygons are rendered after opaque targets even in manual mode. With SWAP_BUFFERS bit 0 clear, the automatic DS Y-sort rule remains fail-closed as TranslucentSortingNotSupported whenever a translucent polygon is present; it is not approximated by submission order or a guessed depth key. Alpha zero remains the separately unsupported wireframe mode. The `initialize_rear_plane` supports both rear-plane modes. With `DISP3DCNT.Bit14=0`, `CLEAR_COLOR` initializes RGB6/A5, fog-enable and polygon ID, while the 15-bit `CLEAR_DEPTH` is expanded to 24-bit with `raw × 0x200 + ((raw + 1) / 0x8000) × 0x1FF` (`0x7FFF → 0xFFFFFF`). With Bit14 set, the renderer reads two 256×256 halfword bitmaps from the physical VRAM image slots: slot 2 provides RGB555 and one-bit alpha (bit15, 0=transparent/31=solid); slot 3 provides 15-bit depth plus the per-pixel fog-enable flag in bit15. The polygon ID continues to come from `CLEAR_COLOR`. Only the visible 256×192 window is copied to the software staging surfaces. `CLRIMAGE_OFFSET` is a full 16-bit pair of offsets: low byte is horizontal/column (0=left edge), high byte is vertical/row (0=top edge); each wraps modulo 256. Both slots are preflighted over the visible window before either output buffer is changed, and a missing/overlapping/invalid source returns `TextureFailure` without leaving a partial rear-plane. The color/depth buffers remain software 256×192 staging surfaces. `render_scanline_to_cache` exports a composed scanline into `Nds3dScanlineCache`: it checks producer order and capacity before rendering, converts RGB6 channels to BG0 RGB555 by dropping each channel's low bit, preserves alpha5 and enqueues one 256-pixel line. The queue is a bounded ring with capacity 48. Engine A's 2D compositor now has a cache-aware BG0 overload: it peeks the line matching the requested scanline and removes it only after the entire 2D composition succeeds. On cache underflow or failed BG0 composition, caller output remains unchanged; failed composition also leaves the pending 3D line queued. `NdsMachine` owns the texture sampler, rasterizer, color/depth staging buffers and 48-line BG0 cache. The synchronized system clock drives pre-render at scanline 214, fills the first 48 lines during VBlank, preserves them across the natural 262-to-0 frame wrap, and refills free slots at subsequent visible-line boundaries. The SDL desktop host and Android SDL Activity share `oracis_host_runtime_render_scanline` through the same C bridge; the bridge passes Engine A BG0 3D through the cache-aware compositor and applies master brightness after composition. A host request can still initialize or recover the queue when BG0-3D is enabled late or the host begins another frame at line zero. The C bridge regression renders two complete synthetic 192-line frames and verifies RGB555 output and absence of the BG0 underflow flag. This is event-synchronized at scanline granularity, not cycle-accurate hardware scanout; geometry latency, internal FIFO sample points and missed-line timing remain approximate.


## Fog registers and scanline pass

The ARM9 MMIO model now exposes the write-only `FOG_COLOR` (`0x04000358`),
`FOG_OFFSET` (`0x0400035C`) and 32-byte `FOG_TABLE`
(`0x04000360..0x0400037F`). The table accepts aligned byte, halfword and word
writes; each density is masked to seven bits. `FOG_COLOR` contains RGB555 and
A5, and `FOG_OFFSET` contains a 15-bit value.

When `DISP3DCNT.Bit7` is set, the renderer applies fog after composing the
complete scanline, including rear-plane pixels. Only pixels whose framebuffer
fog flag is set are changed. Opaque fragments replace that flag with
`POLYGON_ATTR.Bit15`; translucent fragments AND the previous flag with that
attribute. The density boundaries are
`FOG_OFFSET + (0x400 >> FOG_SHIFT) * (index + 1)`; intermediate values are
linearly interpolated. The current 24-bit staging depth is converted to the
15-bit fog domain by dropping its nine low bits, consistent with the existing
`CLEAR_DEPTH` expansion. Density 127 is treated as 128. RGB is blended unless
`DISP3DCNT.Bit6` selects alpha-only mode; alpha is blended in both modes.

The scanline pass and MMIO behavior are covered by synthetic tests for
register widths and masks, packed table writes, density interpolation,
full-density handling, disabled per-pixel fog flags, alpha-only mode and
opaque/translucent flag handling. This remains a scanline-granular software
model, not a cycle-accurate or physical-hardware capture match. The documented
near-boundary fog-alpha glitch remains unimplemented; Z/W-specific depth
behavior still needs dedicated conformance vectors.

## Regression coverage

The synthetic sampler tests cover A3I5, 2bpp, 4bpp, 8bpp, A5I3, Direct Color and all four 4x4 compressed modes; alpha decoding; the format-2 palette-base unit; F-bank palette slot 4; clamp and repeat+flip; no-texture; slot-0/slot-1 and slot-2 compressed blocks (including the upper-half metadata route); unallocated slots; and ambiguous image/palette allocations.

The rasterizer integration test constructs Geometry Engine triangles, swaps their buffer at VBlank, samples direct-color VRAM texels and verifies the resulting center fragments. Shadow tests submit the ordered mask/render passes in one geometry buffer, then exercise a standalone spotlight volume; they check back/front face selection, no-color masking, stencil consumption, receiver-ID suppression, blending, repeated-ID suppression and fail-closed behavior without stencil state. Its fixture uses different clip-space W values and texel colors chosen to distinguish perspective-correct sampling from affine interpolation. Overlapping polygons sample a direct-color texel with its alpha bit clear in both Modulation and Decal: Modulation is discarded because its final alpha is zero, while Decal preserves the vertex color/alpha. A separate swapped frame verifies that `POLYGON_ATTR` alpha zero is reported as unsupported wireframe instead of emitted as an interior fragment. The rasterizer regression checks that the first render buffer retains its old Z mode even when the same SWAP command selects W for the next build, then verifies perspective-correct W depth and the depth-tested callback on the second frame. The depth-buffer test covers Less, exact-equal rejection, Z ±0x200 and W ±0xFF Depth Equal boundaries, opaque writes, optional translucent writes and fully transparent rejection. The color-buffer test covers the RGB6/A5 blending equation, alpha max, blend-disable overwrite, transparent-rear-plane overwrite and alpha-zero rejection. The rasterizer integration test verifies that solid-only geometry renders with auto-sort selected, while alpha 1..30 is still fail-closed until translucent Y-sorting exists; it also covers MMIO alpha-test rejection before depth writes, manual-order W-buffer composition, opaque-first/translucent-after manual grouping with two blended layers and stable translucent submission order, Toon table lookup and texture modulation, Highlight additive saturation, and wireframe rejection. Display-control tests cover the eight-entry EDGE_COLOR RGB555 table, write-only access, alignment, ARM9 ownership and DISP3DCNT.Bit5. Rasterizer regressions check perimeter candidate tagging, the four-neighbor ID/depth condition, strict-depth rejection, CLEAR_COLOR/CLEAR_DEPTH border comparison and one-line FIFO lookahead. Rear-plane tests cover both clear modes, slots 2/3, RGB555-to-RGB6, binary alpha, per-pixel fog, depth conversion and both scroll wraps. The sampler tests aligned slot halfword reads and invalid/unmapped bounds. `CLRIMAGE_OFFSET` tests preserve all 16 bits. The new cache test covers 48-line capacity, full/empty states, producer/consumer order, ring wrap, reset boundaries and invalid pixels. Rasterizer integration checks RGB6-to-RGB555/alpha5 export; compositor tests cover BG0 FIFO consumption, underflow, unchanged output and retaining a line after composition failure. CI #1123 passed all four jobs (Desktop Linux, ARM runtime, Android ARM64 and pinned NDSRecomp); Desktop and ARM runtime each reported 54/54 CTests, and the optional static-bank link smoke test reported 1/1. No ROM, BIOS, firmware or extracted game data is used.

## Remaining work

1. Implement the DS-specific automatic Y-sort key for translucent polygons, including Y extent and stable ties. Manual mode now groups opaque polygons before translucent polygons while preserving relative submission order inside each group; A3I5/A5I3 texture formats are also classified as translucent.
2. Refine the scanline-event producer toward sub-scanline/cycle-accurate timing; model missed lines and FIFO underflow/overflow timing, and improve cycle-exact `RDLINES_COUNT` sampling.
3. Extend the now-integrated Engine A BG0 path to Android presentation and validate it against reference frames.
4. Refine cycle-accurate DISP3DCNT underflow/overflow status timing; implement real wireframe; validate Shadow volume stencil behavior and Edge Marking perimeter coverage, excluded lower/right edges and translucent/AA interactions against hardware captures. Validate Toon/Highlight and fog against hardware captures, including the near-boundary fog-alpha glitch and Z/W depth modes.
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
rasterization. It does not model the hardware's internal rendering latency or
the sub-scanline sampling timing used by RDLINES_COUNT and DISP3DCNT status.
RDLINES_COUNT is a previous-frame FIFO low-water approximation. A host request
can still initialize the pipeline when BG0-3D is enabled mid-frame. The machine
accepts only consecutive VCOUNT transitions as scanline scheduling edges;
software jumps directly to lines 192 or 214 cannot prematurely close the
low-water window or launch a new pre-render. Natural 262-to-0 frame wrap is
handled separately. A machine regression verifies both discontinuous jumps.


## DISP3DCNT sticky status

`DISP3DCNT` now retains bits 12/13 as sticky status and applies the
hardware write-one-to-clear acknowledgement rule. A host-side BG0 composition
failure caused by a missing 3D scanline source sets the color-buffer underflow
bit (12), which software can observe and acknowledge through MMIO. The polygon/vertex RAM overflow flag (13) is connected to actual
Geometry Engine polygon/vertex RAM submission failures. The machine latches
the bit on overflow; software clears it with write-one acknowledgement.


The Geometry Engine now reports clipping/vertex-RAM and polygon-RAM exhaustion
to its owning machine at the point of failure. The machine latches
`DISP3DCNT` bit 13, so the overflow flag is produced by the actual polygon
submission path rather than only by a manual test hook. The machine integration
test submits 2,049 valid triangles to a 2,048-entry polygon RAM and verifies the
status register. The bit remains sticky and software-cleared using write-one
acknowledgement.


## RDLINES_COUNT low-water reporting

RDLINES_COUNT at 0x04000320 is exposed as an ARM9-only, read-only byte. At natural frame wrap, the machine starts tracking the minimum number of queued BG0 lines after each successful consumption. At VBlank entry (scanline 192), it publishes max(minimum - 2, 0), clamped to the documented 0..46 register range. The following pre-render begins at scanline 214. This is a queue-level previous-frame low-water approximation; it does not reproduce the hardware's internal sub-scanline sampling latency or cycle-exact FIFO accounting.
