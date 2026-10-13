#pragma once

// Sun shadow caster culling (receiver mask). A caster only needs to be in a sun cascade if its shadow can fall on
// something the main view sees. After the cascades are set up, a compute shader projects every pixel of the main
// view's depth (r4_hiz.h's copy) into each cascade and marks its cell in a 256x256 grid per cascade. Two frames later
// the grids are read back with that frame's cascade transforms and the footprint of its view frustum from the sun.
// A static caster is culled from a cascade when its footprint (light space XY: the shadow goes along light space Z)
// lies inside that frustum footprint and covers no marked cell, two cells wider on each side. Outside it (the view
// turned since) it is drawn.
//   r4_sun_caster_cull 0/1: culling on or off
//   stat_sun_casters <seconds>: tested and culled (or, culling off, would be culled) casters per cascade, in the log
namespace sun_receivers
{
	bool wanted(); // culling or a measurement on: the depth copy is needed
	void start_stats(float seconds);
	// Start of the main view (with the Hi-Z readback). usable: not the 3D scope's view, whose cascades cull nothing
	void frame_begin(bool usable);
	// Cascades set up, before their casters are captured
	void dispatch(const Fmatrix* cascade_xforms, u32 count);
	// Static caster for a cascade: true to cull it. Called from several cascades' threads at once
	bool test_caster(const Fbox& box, u32 cascade);
	void destroy();
}
