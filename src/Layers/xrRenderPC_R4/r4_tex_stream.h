#pragma once

// Texture streaming, measuring step. stat_tex_stream <seconds>: for every texture the main view's world objects
// draw, the largest size it is seen at on screen (the object's projected diameter, the texture assumed to span it
// once), and from that the mip level it needs. The log gets the textures' memory as loaded now and at the needed
// levels, how many would be sharper than now, and the texture memory no world object used in the measurement.
namespace tex_stream
{
	void start_stats(float seconds);
	void frame_begin();
	// A main view world object drawn with this pass. ssa: its radius squared over its distance squared
	void note_draw(const STextureList* textures, float ssa);
	// Any texture bound for any draw (world, grass, LODs, sky, HUD, UI)
	void note_bound(CTexture* T);
}
