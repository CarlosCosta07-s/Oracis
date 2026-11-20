# Nintendo DS 2D background pipeline

## Implemented stages

- NdsDisplayControl retains the Engine A/B display registers and per-BG scroll state.
- Nds2dTextBgFetcher produces one 256-pixel source line for supported text backgrounds. It reads live mapped VRAM and standard BG palette RAM, including tile-map size, tile flips, 4bpp/8bpp pixels and BG mosaic.
- Nds2dBgCompositor combines those four fetched text-BG lines. It applies enabled layer masks and WIN0/WIN1/outside-window rules, sorts by BG priority and BG index, inserts the backdrop palette color, and applies the existing alpha/brightness helpers to the top two non-transparent pixels.

For equal BG priority values, the lower BG index is placed above the higher BG index (BG0 before BG1, then BG2 and BG3). Priority values 0 through 3 are ordered from highest to lowest. This matches the Nintendo DS BG control contract. See GBATEK's DS video BG modes and control at https://mgba-emu.github.io/gbatek/#ds-video-bg-modes-control and the Nintendo DS background documentation in libnds at https://github.com/devkitPro/libnds/blob/master/include/nds/arm9/background.h.

## Explicit limits

The compositor currently consumes text BG fetch results only. Affine, extended and bitmap BGs return an unsupported-mode result; the compositor does not silently omit them. OBJ evaluation, OBJ-window coverage, 3D BG0 pixels, extended BG palettes, forced-blank behavior, line timing/capture, and final SDL/Android screen presentation remain separate work. If an enabled BG requires one of those unimplemented sources, the scanline is rejected without changing the caller's output buffer.
