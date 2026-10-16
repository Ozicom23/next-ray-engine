#include "stdafx.h"
#include "r4_hiz.h"
#include "r4_sun_receivers.h"

int ps_r4_hiz_cull = 1;

namespace hiz
{
namespace
{
	const u32 kSlots = 3; // tile copies in flight: read back two frames later, the GPU has finished them by then
	const u32 kTile = 8; // pixels per tile side (the shader's loop)
	const int kMargin = 1; // tiles added around a box: the depth is two frames old

	const char* kReduceShader =
		"Texture2D<float> depth : register(t0);\n"
		"RWTexture2D<float> tiles : register(u0);\n"
		"[numthreads(8, 8, 1)]\n"
		"void main(uint3 id : SV_DispatchThreadID)\n"
		"{\n"
		"	uint w, h;\n"
		"	depth.GetDimensions(w, h);\n"
		"	uint2 base = id.xy * 8;\n"
		"	if (base.x >= w || base.y >= h) return;\n"
		"	float m = 0;\n"
		"	for (uint y = 0; y < 8; y++)\n"
		"		for (uint x = 0; x < 8; x++)\n"
		"			m = max(m, depth.Load(int3(min(base + uint2(x, y), uint2(w - 1, h - 1)), 0)));\n"
		"	tiles[id.xy] = m;\n"
		"}\n";

	ID3DComputeShader* s_reduce = nullptr;
	bool s_reduce_failed = false;

	// Copy of the scene depth the shader can read, the tiles it writes, their CPU copies
	ID3DTexture2D* s_depth = nullptr;
	ID3DShaderResourceView* s_depth_srv = nullptr;
	ID3DTexture2D* s_tiles_gpu = nullptr;
	ID3DUnorderedAccessView* s_tiles_uav = nullptr;
	struct Slot
	{
		ID3DTexture2D* staging = nullptr;
		Fmatrix view_proj;
	};
	Slot s_slots[kSlots];
	u32 s_width = 0, s_height = 0, s_tiles_w = 0, s_tiles_h = 0;
	// Ring of copies: written in order, read in order, at most kSlots in flight
	u32 s_write = 0, s_read = 0;

	// Tiles of the frame read back last
	xr_vector<float> s_tiles;
	Fmatrix s_tiles_view_proj;
	bool s_tiles_valid = false;

	// This frame's copy
	u32 s_copy_frame = u32(-1);
	Fmatrix s_copy_view_proj;

	// Measuring
	bool s_stats = false;
	u32 s_end_time = 0, s_frames = 0, s_frames_with_tiles = 0;
	enum { kind_static, kind_tree, kind_dynamic, kinds };
	u64 s_tested[kinds] = {}, s_occluded[kinds] = {};

	void release_targets()
	{
		_RELEASE(s_tiles_uav);
		_RELEASE(s_tiles_gpu);
		_RELEASE(s_depth_srv);
		_RELEASE(s_depth);
		for (Slot& s : s_slots)
		{
			_RELEASE(s.staging);
		}
		s_width = s_height = s_tiles_w = s_tiles_h = 0;
		s_write = s_read = 0;
		s_tiles_valid = false;
	}

	bool create_shader()
	{
		if (s_reduce)
			return true;
		if (s_reduce_failed)
			return false;
		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(kReduceShader, xr_strlen(kReduceShader), "hiz_reduce", nullptr, nullptr, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s_reduce);
		if (FAILED(hr))
		{
			Msg("! Hi-Z: depth reduction shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "");
			s_reduce_failed = true;
		}
		_RELEASE(code);
		_RELEASE(errors);
		return s_reduce != nullptr;
	}

	bool ensure_targets(ID3DTexture2D* src)
	{
		D3D11_TEXTURE2D_DESC desc;
		src->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1)
			return false;
		if (desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && desc.Format != DXGI_FORMAT_R24G8_TYPELESS)
			return false;
		if (desc.Width == s_width && desc.Height == s_height && s_depth)
			return true;

		release_targets();
		D3D11_TEXTURE2D_DESC dd = {};
		dd.Width = desc.Width;
		dd.Height = desc.Height;
		dd.MipLevels = 1;
		dd.ArraySize = 1;
		dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
		dd.SampleDesc.Count = 1;
		dd.Usage = D3D11_USAGE_DEFAULT;
		dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;

		const u32 tw = (desc.Width + kTile - 1) / kTile, th = (desc.Height + kTile - 1) / kTile;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = tw;
		td.Height = th;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		bool ok = SUCCEEDED(HW.pDevice->CreateTexture2D(&dd, nullptr, &s_depth)) &&
			SUCCEEDED(HW.pDevice->CreateShaderResourceView(s_depth, &sv, &s_depth_srv)) &&
			SUCCEEDED(HW.pDevice->CreateTexture2D(&td, nullptr, &s_tiles_gpu)) &&
			SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(s_tiles_gpu, nullptr, &s_tiles_uav));
		for (Slot& s : s_slots)
			ok = ok && SUCCEEDED(HW.pDevice->CreateTexture2D(&sd, nullptr, &s.staging));
		if (!ok)
		{
			release_targets();
			return false;
		}
		s_width = desc.Width;
		s_height = desc.Height;
		s_tiles_w = tw;
		s_tiles_h = th;
		return true;
	}

	// The depth's tiles on the GPU, leaving the compute stage as it was
	void reduce()
	{
		ID3DComputeShader* prev_cs = nullptr;
#ifdef USE_DX12
		HW.pContext->CSGetShader(&prev_cs, nullptr, nullptr);
#else
		ID3D11ClassInstance* prev_inst[256];
		UINT prev_inst_count = 256;
		HW.pContext->CSGetShader(&prev_cs, prev_inst, &prev_inst_count);
#endif
		ID3DShaderResourceView* prev_srv = nullptr;
		ID3DUnorderedAccessView* prev_uav = nullptr;
		HW.pContext->CSGetShaderResources(0, 1, &prev_srv);
		HW.pContext->CSGetUnorderedAccessViews(0, 1, &prev_uav);

		HW.pContext->CSSetShader(s_reduce, nullptr, 0);
		HW.pContext->CSSetShaderResources(0, 1, &s_depth_srv);
		HW.pContext->CSSetUnorderedAccessViews(0, 1, &s_tiles_uav, nullptr);
		HW.pContext->Dispatch((s_tiles_w + 7) / 8, (s_tiles_h + 7) / 8, 1);

		ID3DShaderResourceView* no_srv = nullptr;
		HW.pContext->CSSetShaderResources(0, 1, &no_srv);
#ifdef USE_DX12
		HW.pContext->CSSetShader(prev_cs, nullptr, 0);
#else
		HW.pContext->CSSetShader(prev_cs, prev_inst, prev_inst_count);
#endif
		HW.pContext->CSSetShaderResources(0, 1, &prev_srv);
		UINT keep = UINT(-1);
		HW.pContext->CSSetUnorderedAccessViews(0, 1, &prev_uav, &keep);
		_RELEASE(prev_cs);
#ifndef USE_DX12
		for (UINT i = 0; i < prev_inst_count; i++)
			_RELEASE(prev_inst[i]);
#endif
		_RELEASE(prev_srv);
		_RELEASE(prev_uav);
	}

	bool active()
	{
		return ps_r4_hiz_cull || s_stats || sun_receivers::wanted();
	}

	void finish_stats()
	{
		s_stats = false;
		const double f = double(_max(1u, s_frames_with_tiles));
		Msg("* [Hi-Z] per frame over %u frames (%u with depth to test against), culling %s, objects the main view draws:",
			s_frames, s_frames_with_tiles, ps_r4_hiz_cull ? "on" : "off");
		static const char* names[] = { "static", "trees", "dynamic" };
		for (int i = 0; i < kinds; i++)
		{
			Msg("*   %-6s tested %7.1f, %s %7.1f (%.0f%%)", names[i], double(s_tested[i]) / f,
				ps_r4_hiz_cull ? "culled" : "would be culled", double(s_occluded[i]) / f,
				s_tested[i] ? 100.0 * double(s_occluded[i]) / double(s_tested[i]) : 0.0);
		}
		xrLogger::FlushLog();
	}
}

void start_stats(float seconds)
{
	s_stats = true;
	s_end_time = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
	s_frames = 0;
	s_frames_with_tiles = 0;
	for (int i = 0; i < kinds; i++)
		s_tested[i] = s_occluded[i] = 0;
	Msg("* [Hi-Z] measuring for %.0f s...", seconds);
}

void frame_begin(bool usable)
{
	s_tiles_valid = false;
	if (s_stats && Device.dwTimeGlobal >= s_end_time)
		finish_stats();
	if (!active())
	{
		if (s_depth)
			release_targets();
		return;
	}
	sun_receivers::frame_begin(usable);
	if (!usable)
		return;
	if (s_stats)
		s_frames++;

	// Oldest copy, once it is two copies old (the GPU has finished it by then; if not, next frame)
	if (s_write - s_read < kSlots - 1)
		return;
	Slot& s = s_slots[s_read % kSlots];
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(HW.pContext->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return;
	s_read++;
	s_tiles.resize(s_tiles_w * s_tiles_h);
	for (u32 y = 0; y < s_tiles_h; y++)
		CopyMemory(&s_tiles[y * s_tiles_w], (const BYTE*)mapped.pData + y * mapped.RowPitch, s_tiles_w * sizeof(float));
	HW.pContext->Unmap(s.staging, 0);
	s_tiles_view_proj = s.view_proj;
	s_tiles_valid = true;
	if (s_stats)
		s_frames_with_tiles++;
}

void capture(ID3DDepthStencilView* depth, const Fmatrix& view_proj)
{
	if (!active() || !depth || !create_shader())
		return;
	ID3DBaseTexture* res = nullptr;
	depth->GetResource(&res);
	ID3DTexture2D* tex = nullptr;
	if (res)
		res->QueryInterface(__uuidof(ID3DTexture2D), (void**)&tex);
	_RELEASE(res);
	if (!tex)
		return;
	if (ensure_targets(tex))
	{
		Slot& s = s_slots[s_write % kSlots];
		if (s_write - s_read < kSlots)
		{
			HW.pContext->CopyResource(s_depth, tex);
			s_copy_frame = Device.dwFrame;
			s_copy_view_proj = view_proj;
			reduce();
			HW.pContext->CopyResource(s.staging, s_tiles_gpu);
			s.view_proj = view_proj;
			s_write++;
		}
	}
	_RELEASE(tex);
}

namespace
{
bool test_box(const Fbox& box, int kind)
{
	if (!s_tiles_valid)
		return false;
	if (s_stats)
		s_tested[kind]++;

	// Screen rectangle and nearest depth of the box in the depth's frame
	float min_x = 1.f, max_x = -1.f, min_y = 1.f, max_y = -1.f, min_z = 1.f;
	const Fmatrix& m = s_tiles_view_proj;
	for (int i = 0; i < 8; i++)
	{
		Fvector p;
		box.getpoint(i, p);
		const float w = p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44;
		if (w < 0.1f)
			return false; // crosses the camera plane: visible
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
		return false;

	const int tw = int(s_tiles_w), th = int(s_tiles_h);
	const int tx0 = clampr(iFloor((min_x * 0.5f + 0.5f) * float(s_width)) / int(kTile) - kMargin, 0, tw - 1);
	const int tx1 = clampr(iFloor((max_x * 0.5f + 0.5f) * float(s_width)) / int(kTile) + kMargin, 0, tw - 1);
	const int ty0 = clampr(iFloor((0.5f - max_y * 0.5f) * float(s_height)) / int(kTile) - kMargin, 0, th - 1);
	const int ty1 = clampr(iFloor((0.5f - min_y * 0.5f) * float(s_height)) / int(kTile) + kMargin, 0, th - 1);
	for (int ty = ty0; ty <= ty1; ty++)
	{
		const float* row = &s_tiles[ty * tw];
		for (int tx = tx0; tx <= tx1; tx++)
		{
			if (row[tx] >= min_z)
				return false; // something in the tile is as far as the box: visible
		}
	}
	if (s_stats)
		s_occluded[kind]++;
	return ps_r4_hiz_cull != 0;
}
}

bool test_static(const Fbox& box, bool tree)
{
	return test_box(box, tree ? kind_tree : kind_static);
}

bool test_dynamic(const Fsphere& sphere)
{
	if (!s_tiles_valid)
		return false;
	Fbox box;
	box.setb(sphere.P, Fvector().set(sphere.R, sphere.R, sphere.R));
	return test_box(box, kind_dynamic);
}

bool depth_copy(ID3DShaderResourceView*& srv, Fmatrix& view_proj, u32& width, u32& height)
{
	if (s_copy_frame != Device.dwFrame || !s_depth_srv)
		return false;
	srv = s_depth_srv;
	view_proj = s_copy_view_proj;
	width = s_width;
	height = s_height;
	return true;
}

void destroy()
{
	sun_receivers::destroy();
	release_targets();
	_RELEASE(s_reduce);
	s_reduce_failed = false;
	s_stats = false;
}
}
