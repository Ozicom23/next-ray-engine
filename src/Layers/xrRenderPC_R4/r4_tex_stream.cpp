#include "stdafx.h"
#include "r4_tex_stream.h"
#include "r4_gpu_memory.h"
#include "../xrRender/SH_Texture.h"
#include "../xrRender/ResourceManager.h"
#include "../xrRender/dxRenderDeviceRender.h"

namespace tex_stream
{
namespace
{
	bool s_active = false;
	u32 s_end_time = 0, s_frames = 0;
	float s_px_per_ssa = 0.f; // projected diameter in output pixels = sqrt(ssa) * this
	xr_unordered_map<u64, float> s_needed; // CTexture* -> largest projected diameter in pixels
	xr_unordered_set<u64> s_bound; // CTexture* bound for any draw

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
			const u32 w = T->get_Width_loaded(), h = T->get_Height_loaded();
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

		// Every loaded texture: bound for any draw in the measurement or not
		struct unused { const char* name; u32 bytes; };
		xr_vector<unused> list;
		u64 bound = 0, not_bound = 0;
		u32 bound_count = 0;
		for (const auto& it : DEV->textures())
		{
			CTexture* T = it.second;
			if (!T || !T->flags.bLoaded)
				continue;
			const u32 bytes = T->flags.MemoryUsage;
			if (s_bound.find(u64(size_t(T))) != s_bound.end())
			{
				bound += bytes;
				bound_count++;
			}
			else
			{
				not_bound += bytes;
				list.push_back({ it.first, bytes });
			}
		}
		Msg("*   bound for any draw         %.2f GB (%u textures), never bound %.2f GB (%u textures)",
			double(bound) / gb, bound_count, double(not_bound) / gb, u32(list.size()));

		// Never bound, by folder (first two path parts) and the largest ones
		xr_map<xr_string, std::pair<u64, u32>> folders;
		for (const unused& u : list)
		{
			xr_string n = u.name;
			size_t p = n.find_first_of("\\/");
			if (p != xr_string::npos)
				p = n.find_first_of("\\/", p + 1);
			auto& f = folders[p != xr_string::npos ? n.substr(0, p) : n];
			f.first += u.bytes;
			f.second++;
		}
		xr_vector<std::pair<xr_string, std::pair<u64, u32>>> fl;
		for (const auto& f : folders)
			fl.push_back(f);
		std::sort(fl.begin(), fl.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
		Msg("*   never bound, by folder:");
		for (size_t i = 0; i < fl.size() && i < 25; i++)
			Msg("*     %-40s %7.1f MB %5u", fl[i].first.c_str(), double(fl[i].second.first) / (1024.0 * 1024.0), fl[i].second.second);
		std::sort(list.begin(), list.end(), [](const unused& a, const unused& b) { return a.bytes > b.bytes; });
		Msg("*   never bound, largest:");
		for (size_t i = 0; i < list.size() && i < 25; i++)
			Msg("*     %-60s %6.1f MB", list[i].name, double(list[i].bytes) / (1024.0 * 1024.0));
		xrLogger::FlushLog();
		s_needed.clear();
		s_bound.clear();
	}
}

void start_stats(float seconds)
{
	s_needed.clear();
	s_bound.clear();
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

void note_bound(CTexture* T)
{
	if (s_active)
		s_bound.insert(u64(size_t(T)));
}
}
