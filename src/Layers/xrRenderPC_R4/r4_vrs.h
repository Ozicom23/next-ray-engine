#pragma once

// Variable rate shading (DX12, a GPU with shading rate images: tier 2). After the combine a compute pass looks at the
// frame at render resolution in the device's tiles (8x8 pixels on the Steam Deck): where neighbouring pixels differ
// by less than r4_vrs_threshold across or down, the next frame's G-buffer and sun and lamp lighting shade that tile at
// half rate that way (both ways: a quarter). Flat, dark and foggy areas and the sky get coarser, edges and texture
// detail keep full rate. Never the UI, the HUD weapon, shadow maps or the half resolution passes.
//   r4_vrs 0|1, r4_vrs_threshold (0.03), r4_vrs_debug 1: the coarse tiles tinted (yellow half, red quarter)
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
