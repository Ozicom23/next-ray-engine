#pragma once

#ifdef USE_DX11
// Lamp shadow maps kept between frames (r4_light_smap_keep), see lights_render.cpp.
// A shadowed lamp that doesn't move, whose static casters hold no trees or bushes (they sway in the wind) and that has
// no dynamic object or grass shadow in its volume this frame gets its shadow map drawn once into a depth texture of
// its own. While nothing changes, the lamp's lighting samples that texture in place of the atlas: no copy, nothing
// drawn. It is drawn again when the lamp's size, transform or static capture change. Same shadows.
// A lamp with trees among its casters is kept too when it is far enough from the camera, its map drawn again at an
// interval (one such lamp per frame): the trees' sway in its shadow holds still in between.
//   r4_light_smap_keep 0/1
//   r4_light_smap_keep_trees <meters>: lamps with trees kept beyond this distance (20), 0 never
//   r4_light_smap_keep_trees_refresh <seconds>: their maps drawn again after this long (1), 0 never
//   r4_light_smap_keep_dynamic 0/1: lamps with dynamic casters kept too while those casters stay as they were drawn
//     (each draw's visual, shader, world matrix and detail level, each skeleton's bone transforms)
//   stat_light_keep <seconds>: per-frame counts of kept, drawn and not-kept lamps (with the reason), in the log
class light;
namespace light_smap_keep
{
	extern int ps_keep;
	extern int ps_keep_trees;
	extern float ps_keep_trees_refresh;
	extern int ps_keep_dynamic;
	void start_stats(float seconds);
	// The light is being destroyed
	void forget(light* L);
	// The renderer is being destroyed
	void destroy();
}
#endif
