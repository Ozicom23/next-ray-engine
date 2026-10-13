#include "stdafx.h"
#include "r4_hiz.h"

namespace hiz
{
namespace
{
	const u32 kSlots = 3; // copies in flight: read back two frames later, the GPU has finished them by then
	const u32 kTile = 8; // pixels per tile side

	struct Slot
	{
		ID3D11Texture2D* staging = nullptr;
		Fmatrix view_proj;
		bool pending = false;
	};
	Slot s_slots[kSlots];
	u32 s_width = 0, s_height = 0;
	DXGI_FORMAT s_format = DXGI_FORMAT_UNKNOWN;
	u32 s_write = 0;

	// Tiles of the frame read back last
	xr_vector<float> s_tiles;
	u32 s_tiles_w = 0, s_tiles_h = 0;
	Fmatrix s_tiles_view_proj;
	bool s_tiles_valid = false;

	// Measuring
	bool s_active = false;
	u32 s_end_time = 0, s_frames = 0;
	u64 s_tested[2] = {}, s_occluded[2] = {}; // [0] other static, [1] trees
	u32 s_frames_with_tiles = 0;

	void release_slots()
	{
		for (Slot& s : s_slots)
		{
			_RELEASE(s.staging);
			s.pending = false;
		}
		s_width = s_height = 0;
		s_tiles_valid = false;
	}

	bool ensure_slots(ID3D11Texture2D* src)
	{
		D3D11_TEXTURE2D_DESC desc;
		src->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1)
			return false;
		if (desc.Width == s_width && desc.Height == s_height && desc.Format == s_format && s_slots[0].staging)
			return true;

		release_slots();
		D3D11_TEXTURE2D_DESC sd = desc;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		// Staging can't have a depth format: the typeless one of its group
		if (sd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
			sd.Format = DXGI_FORMAT_R24G8_TYPELESS;
		else if (sd.Format == DXGI_FORMAT_D32_FLOAT)
			sd.Format = DXGI_FORMAT_R32_TYPELESS;
		if (sd.Format != DXGI_FORMAT_R24G8_TYPELESS && sd.Format != DXGI_FORMAT_R32_TYPELESS)
			return false;

		for (Slot& s : s_slots)
		{
			if (FAILED(HW.pDevice->CreateTexture2D(&sd, nullptr, &s.staging)))
			{
				release_slots();
				return false;
			}
		}
		s_width = desc.Width;
		s_height = desc.Height;
		s_format = desc.Format;
		return true;
	}

	float depth_at(const BYTE* row, u32 x, bool d24)
	{
		const u32 v = ((const u32*)row)[x];
		if (d24)
			return float(v & 0xFFFFFF) * (1.f / 16777215.f);
		float f;
		CopyMemory(&f, &v, 4);
		return f;
	}

	void build_tiles(const D3D11_MAPPED_SUBRESOURCE& mapped, const Fmatrix& view_proj)
	{
		s_tiles_w = (s_width + kTile - 1) / kTile;
		s_tiles_h = (s_height + kTile - 1) / kTile;
		s_tiles.assign(s_tiles_w * s_tiles_h, 0.f);
		const bool d24 = s_format == DXGI_FORMAT_D24_UNORM_S8_UINT || s_format == DXGI_FORMAT_R24G8_TYPELESS;
		for (u32 y = 0; y < s_height; y++)
		{
			const BYTE* row = (const BYTE*)mapped.pData + y * mapped.RowPitch;
			float* tiles = &s_tiles[(y / kTile) * s_tiles_w];
			for (u32 x = 0; x < s_width; x++)
			{
				const float d = depth_at(row, x, d24);
				float& t = tiles[x / kTile];
				if (d > t)
					t = d;
			}
		}
		s_tiles_view_proj = view_proj;
		s_tiles_valid = true;
	}

	void finish()
	{
		s_active = false;
		const double f = double(_max(1u, s_frames_with_tiles));
		Msg("* [Hi-Z] per frame over %u frames (%u with depth to test against), static objects the main view draws:", s_frames, s_frames_with_tiles);
		static const char* names[] = { "other", "trees" };
		for (int i = 0; i < 2; i++)
		{
			Msg("*   %-6s tested %7.1f, would be culled %7.1f (%.0f%%)", names[i], double(s_tested[i]) / f, double(s_occluded[i]) / f,
				s_tested[i] ? 100.0 * double(s_occluded[i]) / double(s_tested[i]) : 0.0);
		}
		xrLogger::FlushLog();
		release_slots();
	}
}

void start_stats(float seconds)
{
	s_active = true;
	s_end_time = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
	s_frames = 0;
	s_frames_with_tiles = 0;
	for (int i = 0; i < 2; i++)
		s_tested[i] = s_occluded[i] = 0;
	Msg("* [Hi-Z] measuring for %.0f s, keep the view still...", seconds);
}

void capture(ID3D11DepthStencilView* depth, const Fmatrix& view_proj)
{
	if (!s_active || !depth)
		return;
	ID3D11Resource* res = nullptr;
	depth->GetResource(&res);
	ID3D11Texture2D* tex = nullptr;
	if (res)
		res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
	_RELEASE(res);
	if (!tex)
		return;
	if (ensure_slots(tex))
	{
		Slot& s = s_slots[s_write % kSlots];
		if (!s.pending)
		{
			HW.pContext->CopyResource(s.staging, tex);
			s.view_proj = view_proj;
			s.pending = true;
			s_write++;
		}
	}
	_RELEASE(tex);
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
	s_tiles_valid = false;

	// Oldest copy: written kSlots - 1 frames ago
	if (s_write < kSlots - 1)
		return;
	Slot& s = s_slots[(s_write - (kSlots - 1)) % kSlots];
	if (!s.pending)
		return;
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(HW.pContext->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return;
	build_tiles(mapped, s.view_proj);
	HW.pContext->Unmap(s.staging, 0);
	s.pending = false;
	s_frames_with_tiles++;
}

void test_static(const Fbox& box, bool tree)
{
	if (!s_active || !s_tiles_valid)
		return;
	s_tested[tree]++;

	// Screen rectangle and nearest depth of the box in the depth's frame
	float min_x = 1.f, max_x = -1.f, min_y = 1.f, max_y = -1.f, min_z = 1.f;
	for (u32 i = 0; i < 8; i++)
	{
		Fvector p;
		box.getpoint(i, p);
		const Fmatrix& m = s_tiles_view_proj;
		const float w = p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44;
		if (w < 0.1f)
			return; // crosses the camera plane: visible
		const float x = (p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41) / w;
		const float y = (p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42) / w;
		const float z = (p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43) / w;
		min_x = _min(min_x, x);
		max_x = _max(max_x, x);
		min_y = _min(min_y, y);
		max_y = _max(max_y, y);
		min_z = _min(min_z, z);
	}
	// Outside that frame's view (the camera turned since): visible
	if (min_x < -1.f || max_x > 1.f || min_y < -1.f || max_y > 1.f)
		return;

	const int tx0 = clampr(iFloor((min_x * 0.5f + 0.5f) * float(s_width)) / int(kTile), 0, int(s_tiles_w) - 1);
	const int tx1 = clampr(iFloor((max_x * 0.5f + 0.5f) * float(s_width)) / int(kTile), 0, int(s_tiles_w) - 1);
	const int ty0 = clampr(iFloor((0.5f - max_y * 0.5f) * float(s_height)) / int(kTile), 0, int(s_tiles_h) - 1);
	const int ty1 = clampr(iFloor((0.5f - min_y * 0.5f) * float(s_height)) / int(kTile), 0, int(s_tiles_h) - 1);
	for (int ty = ty0; ty <= ty1; ty++)
	{
		const float* row = &s_tiles[ty * s_tiles_w];
		for (int tx = tx0; tx <= tx1; tx++)
		{
			if (row[tx] >= min_z)
				return; // something in the tile is as far as the box: visible
		}
	}
	s_occluded[tree]++;
}

void destroy()
{
	release_slots();
	s_active = false;
}
}
