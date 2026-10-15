#pragma once

#ifdef USE_DX11
// Lamp shadow maps kept between frames (r4_light_smap_keep), see lights_render.cpp.
// A shadowed lamp that doesn't move, whose static casters hold no trees or bushes (they sway in the wind) and that has
// no dynamic object or grass shadow in its volume this frame gets its shadow map drawn once into a depth texture of
// its own. While nothing changes, the lamp's lighting samples that texture in place of the atlas: no copy, nothing
// drawn. It is drawn again when the lamp's size, transform or static capture change. Same shadows.
//   r4_light_smap_keep 0/1
//   stat_light_keep <seconds>: per-frame counts of kept, drawn and not-kept lamps (with the reason), in the log
class light;
namespace light_smap_keep
{
	extern int ps_keep;
	void start_stats(float seconds);
	// The light is being destroyed
	void forget(light* L);
	// The renderer is being destroyed
	void destroy();
}
#endif
