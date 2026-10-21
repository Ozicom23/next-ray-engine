#pragma once

// Far sun cascades kept between frames (r4_sun_cascade_cache). A far cascade's static casters but trees and bushes are drawn into a map of
// its own with the box grown by a margin on each side (the same texel size, more texels) and a grown depth range.
// Every frame the window of that map that matches the cascade's transform of the frame is copied into the shadow map
// atlas, at the cascade's place and scale there, and the trees and bushes (they sway), the dynamic casters (NPCs, the
// player, physics objects) and grass are drawn over it; the accumulation then samples the atlas exactly as it would after a full render. The kept
// map is drawn again when the sun turned enough to move a far caster's shadow by a fraction of a texel
// (r4_sun_cascade_cache_drift), when the cascade's receiver slab left the kept box (the view moved or turned), or
// when the map's size changed. Near shadows (cascade 0) are not kept.
//   r4_sun_cascade_cache 0/1/2: off, the far cascade, the two far cascades
//   r4_sun_cascade_cache_drift: texels of sun movement allowed before a refresh (0.5)
//   r4_sun_cascade_cache_trees 0/1/2: the trees and bushes kept in the map too in the far cascade, the two far cascades
//     (their shadows there stop swaying, they change at a refresh; the trees were ~70% of the far cascades' GPU time on
//     the Steam Deck)
//   stat_sun_cache <seconds>: frames served from the kept maps and refreshes with their reasons, in the log
namespace sun_cache
{
	enum mode { m_off = 0, m_window, m_refresh };

	// After a cascade's transform is computed for the frame. xform: the cascade's transform, used: the atlas pixels
	// it renders to (r4_sun_smap_scale), size: the box side, far_plane: its ortho far distance. Returns m_off (render as
	// usual), m_window (the kept map's window into the atlas) or m_refresh (draw the statics into the kept map first);
	// window_xform is then the transform to draw the dynamic casters and accumulate with.
	mode frame(u32 cascade, u32 cascade_count, const Fmatrix& xform, u32 used, float size, float far_plane, const Fvector& sun_dir, Fmatrix& window_xform);
	// The kept map's transform and caster frustum, for capturing the statics on a refresh
	Fmatrix& big_xform(u32 cascade);
	CFrustum& big_frustum(u32 cascade);
	// Binds the kept map as the depth target (cleared, no color target), sets its viewport and the shadow states
	void begin_refresh(u32 cascade);
	// The kept map holds the trees and bushes too (r4_sun_cascade_cache_trees): they aren't drawn over the window
	bool keeps_trees(u32 cascade, u32 cascade_count);
	// Draws the window into the atlas (bound as the depth target with the cascade's viewport). Raw device calls: the
	// backend's state is reset after it, the caller sets its targets and state anew
	void draw_window(u32 cascade, ID3DDepthStencilView* atlas, u32 used);
	void start_stats(float seconds);
	void destroy();
}
