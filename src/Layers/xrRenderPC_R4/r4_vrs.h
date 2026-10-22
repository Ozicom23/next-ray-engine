#pragma once

// Variable rate shading (DX12, a GPU with shading rate images: tier 2). After the combine a compute pass looks at the
// frame at render resolution in the device's tiles (8x8 pixels on the Steam Deck): where neighbouring pixels differ
// by less than r4_vrs_threshold across or down, the next frame's sun and lamp lighting shade that tile at
// half rate that way (both ways: a quarter). Flat, dark and foggy areas and the sky get coarser, edges and texture
// detail keep full rate. Not the G-buffer (geometry bound: no gain on the Steam Deck), the UI, shadow maps or the half
// resolution passes. Deck, village at night: frame 20.13 -> 20.00 ms, the image the same.
//   r4_vrs 0|1 (on), r4_vrs_threshold (0.03), r4_vrs_debug 1: the coarse tiles tinted (yellow half, red quarter)
class CRenderTarget;

namespace vrs
{
	void build(CRenderTarget* t); // after the combine, before the upscaler: rt_Color at render resolution
	void begin(); // the draws that follow shade at the image's rates
	void end();
	void release();
}

extern int ps_r4_vrs;
extern int ps_r4_vrs_debug;
extern float ps_r4_vrs_threshold;
