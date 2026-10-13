#pragma once

// Occlusion test against the scene depth of a recent frame (Hi-Z): measuring only for now.
// stat_hiz <seconds>: the main view's depth (scene geometry, before HUD) is copied to the CPU two frames later,
// reduced to tiles holding their farthest depth, and each static object the main view draws is tested against it.
// The log gets how many of them would be culled. Without a measurement nothing is copied or tested.
namespace hiz
{
	void start_stats(float seconds);
	// Main view, after the scene geometry: copy its depth (at most one copy in flight per slot)
	void capture(ID3D11DepthStencilView* depth, const Fmatrix& view_proj);
	// Start of the main view: read back the oldest copy and build the tiles
	void frame_begin();
	// Main view static object about to be drawn
	void test_static(const Fbox& box, bool tree);
	void destroy();
}
