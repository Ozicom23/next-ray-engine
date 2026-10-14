#include "stdafx.h"
#include "r4_tex_stream.h"
#include "r4_gpu_memory.h"
#include "../xrRender/SH_Texture.h"

namespace tex_stream
{
namespace
{
	bool s_active = false;
	u32 s_end_time = 0, s_frames = 0;
	float s_px_per_ssa = 0.f; // projected diameter in output pixels = sqrt(ssa) * this
	xr_unordered_map<u64, float> s_needed; // CTexture* -> largest projected diameter in pixels

	void finish()
	{
		s_active = false;
		gpu_memory::totals totals;
		gpu_memory::get_totals(totals);

		u32 count = 0, sharper = 0, cubes = 0;
		u64 now = 0, at_needed = 0, at_needed_x2 = 0;
		for (auto& it : s_needed)
		{
			CTexture* T = (CTexture*)size_t(it.first);
			if (!T->flags.bLoaded || T->flags.MemoryUsage == 0)
				continue;
			const u32 w = T->get_Width(), h = T->get_Height();
			if (!w || !h)
				continue;
			const int skip = T->m_mip_skip;
			const float full = float(_max(w, h) << skip);
			const float mem = float(T->flags.MemoryUsage);
			count++;
			now += u64(mem);
			// Needed level: full size over the size on screen, rounded down (the sharper level)
			auto level = [&](float px)
			{
				int m = iFloor(log2f(full / _max(px, 1.f)));
				return clampr(m, 0, 12);
			};
			const int m1 = level(it.second), m2 = level(it.second * 2.f);
			at_needed += u64(mem * powf(4.f, float(skip - m1)));
			at_needed_x2 += u64(mem * powf(4.f, float(skip - m2)));
			if (m1 < skip)
				sharper++;
		}
		const double gb = 1024.0 * 1024.0 * 1024.0;
		const u64 all = totals.textures();
		Msg("* [Texture streaming] over %u frames: %u textures drawn by world objects in the main view", s_frames, count);
		Msg("*   as loaded now              %.2f GB (all textures %.2f GB, not used by world objects %.2f GB)",
			double(now) / gb, double(all) / gb, double(all > now ? all - now : 0) / gb);
		Msg("*   at the level they need     %.2f GB, %u of them sharper than now", double(at_needed) / gb, sharper);
		Msg("*   needing twice the size     %.2f GB (textures tiling twice over their object)", double(at_needed_x2) / gb);
		xrLogger::FlushLog();
		s_needed.clear();
	}
}

void start_stats(float seconds)
{
	s_needed.clear();
	s_frames = 0;
	s_end_time = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
	s_active = true;
	Msg("* [Texture streaming] measuring for %.0f s, look around...", seconds);
}

void frame_begin()
{
	if (!s_active)
		return;
	if (Device.dwTimeGlobal >= s_end_time)
	{
		finish();
		return;
	}
	s_frames++;
	// Output resolution: the upscaler brings the image to it
	s_px_per_ssa = 2.f * float(Device.dwHeight) * 0.5f / tanf(deg2rad(Device.fFOV) * 0.5f);
}

void note_draw(const STextureList* textures, float ssa)
{
	if (!s_active || !textures)
		return;
	const float px = _sqrt(ssa) * s_px_per_ssa;
	for (const auto& it : *textures)
	{
		CTexture* T = &*it.second;
		if (!T)
			continue;
		float& v = s_needed[u64(size_t(T))];
		if (px > v)
			v = px;
	}
}
}
