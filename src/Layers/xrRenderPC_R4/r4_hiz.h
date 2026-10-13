#pragma once

// Occlusion culling against the scene depth of a recent frame (Hi-Z), main view static objects.
// The main view's depth after the scene geometry (before HUD) is reduced on the GPU to 8x8 pixel tiles holding their
// farthest depth and read back two frames later. A static object the main view would draw is culled when, in that
// frame's view, every tile its box covers (one tile wider on each side) is nearer than the box's nearest point.
//   r4_hiz_cull 0/1: culling on or off
//   stat_hiz <seconds>: counts of tested and culled (or, culling off, would be culled) objects, in the log
namespace hiz
{
	void start_stats(float seconds);
	// Start of the main view. usable: this view is the one the depth comes from (not the 3D scope's)
	void frame_begin(bool usable);
	// Main view, after the scene geometry: reduce its depth for a later frame
	void capture(ID3D11DepthStencilView* depth, const Fmatrix& view_proj);
	// Main view static object about to be drawn: true to cull it
	bool test_static(const Fbox& box, bool tree);
	void destroy();
}
