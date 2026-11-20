# Nintendo DS 2D graphics pipeline

## Implemented stages

- `NdsDisplayControl` retains Engine A/B display registers and per-BG scroll/window/blend state.
- `Nds2dTextBgFetcher` produces 256-pixel source lines for supported text backgrounds, using live mapped BG VRAM and palette RAM. It supports 4bpp/8bpp, tile-map size, flips, scroll and BG mosaic.
- `Nds2dObjFetcher` reads the two 1 KiB OAM tables (Engine A at `0x07000000`, Engine B at `0x07000400`) and rasterizes tiled sprites from the corresponding OBJ VRAM/palette windows. It supports 4bpp/8bpp, 1D/2D tile mapping, size/shape, clipping, non-affine flips, signed 8.8 affine matrices, double-size bounds, mosaic, palette transparency, semi-transparent OBJ metadata and OBJ-window coverage.
- `Nds2dBgCompositor` combines BG and OBJ candidates per pixel. It applies WIN0/WIN1/OBJ-window/outside masks, BG/OBJ priority, the OAM-index OBJ tie-break, backdrop selection and alpha/brightness effects. Semi-transparent OBJ forces the first-target alpha path and still requires a valid second-target layer.

For equal BG priority values, BG0 precedes BG1, BG2 and BG3. An OBJ with the same 2-bit priority is in front of a BG; among OBJs with equal priority, lower OAM index wins. The character mapping and OAM bit fields follow the DS video description in [GBATEK](https://mgba-emu.github.io/gbatek/).

## Explicit limits

Bitmap OBJ returns an explicit unsupported status. Affine matrices are read from the 32 interleaved PA/PB/PC/PD OAM groups selected by Attr1[9:13]. Each output pixel is reverse-mapped from the destination bounding box around its center into source tile coordinates; transformed texels outside the source rectangle remain transparent. Affine/extended/bitmap BGs, extended palettes, 3D BG0, forced blank, the per-scanline OBJ/pixel evaluation budgets, and hardware-accurate scanline arbitration are not implemented. OBJ-window coverage is supported as a mask input, but full real-game frame output and SDL/Android presentation are not yet connected. A line that encounters an enabled unsupported OBJ mode is rejected without changing the caller's output buffer.
