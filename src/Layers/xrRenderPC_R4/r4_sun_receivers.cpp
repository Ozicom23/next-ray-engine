#include "stdafx.h"
#include "r4_sun_receivers.h"
#include "r4_hiz.h"

#include <atomic>

int ps_r4_sun_caster_cull = 0;

namespace sun_receivers
{
namespace
{
	const u32 kSlots = 3; // read back two frames later
	const u32 kCells = 256; // per side, per cascade
	const u32 kCascades = 3;
	const int kMargin = 2; // cells added around a footprint: the receivers are two frames old

	const char* kMarkShader =
		"Texture2D<float> depth : register(t0);\n"
		"RWTexture2DArray<uint> mask : register(u0);\n"
		"cbuffer params : register(b0)\n"
		"{\n"
		"	float4x4 inv_view_proj;\n"
		"	float4x4 cascade[3];\n"
		"	uint4 dims; // depth width, height, cells, cascades\n"
		"};\n"
		"[numthreads(8, 8, 1)]\n"
		"void main(uint3 id : SV_DispatchThreadID)\n"
		"{\n"
		"	if (id.x >= dims.x || id.y >= dims.y) return;\n"
		"	float d = depth.Load(int3(id.xy, 0));\n"
		"	if (d >= 1.0) return;\n"
		"	float2 ndc = float2((id.x + 0.5) / dims.x * 2 - 1, 1 - (id.y + 0.5) / dims.y * 2);\n"
		"	float4 w = mul(inv_view_proj, float4(ndc, d, 1));\n"
		"	w.xyz /= w.w;\n"
		"	for (uint c = 0; c < dims.w; c++)\n"
		"	{\n"
		"		float4 l = mul(cascade[c], float4(w.xyz, 1));\n"
		"		float2 uv = l.xy / l.w * 0.5 + 0.5;\n"
		"		if (all(uv >= 0) && all(uv < 1))\n"
		"			mask[uint3(uv * dims.z, c)] = 1;\n"
		"	}\n"
		"}\n";

	struct Params
	{
		Fmatrix inv_view_proj;
		Fmatrix cascade[kCascades];
		u32 dims[4];
	};

	ID3D11ComputeShader* s_mark = nullptr;
	bool s_mark_failed = false;
	ID3D11Buffer* s_params = nullptr;
	ID3D11Texture2D* s_mask_gpu = nullptr;
	ID3D11UnorderedAccessView* s_mask_uav = nullptr;

	struct Slot
	{
		ID3D11Texture2D* staging = nullptr;
		Fmatrix view_proj;
		Fmatrix xform[kCascades];
		u32 count = 0;
	};
	Slot s_slots[kSlots];
	u32 s_write = 0, s_read = 0;

	// Read back last: masks, the cascades' transforms and the frustum's footprint per cascade
	u8 s_mask[kCascades][kCells * kCells];
	Fmatrix s_xform[kCascades];
	xr_vector<Fvector2> s_hull[kCascades];
	u32 s_count = 0;
	bool s_valid = false;

	bool s_stats = false;
	u32 s_end_time = 0, s_frames = 0, s_frames_valid = 0;
	std::atomic<u64> s_tested[kCascades], s_culled[kCascades];

	void release()
	{
		_RELEASE(s_mask_uav);
		_RELEASE(s_mask_gpu);
		_RELEASE(s_params);
		for (Slot& s : s_slots)
			_RELEASE(s.staging);
		s_write = s_read = 0;
		s_valid = false;
	}

	bool create()
	{
		if (s_mask_gpu)
			return true;
		if (s_mark_failed)
			return false;
		if (!s_mark)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			HRESULT hr = D3DCompile(kMarkShader, xr_strlen(kMarkShader), "sun_receivers", nullptr, nullptr, "main", "cs_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (SUCCEEDED(hr))
				hr = HW.pDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s_mark);
			if (FAILED(hr))
			{
				Msg("! Sun receivers: shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "");
				s_mark_failed = true;
			}
			_RELEASE(code);
			_RELEASE(errors);
			if (!s_mark)
				return false;
		}

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(Params);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = kCells;
		td.Height = kCells;
		td.MipLevels = 1;
		td.ArraySize = kCascades;
		td.Format = DXGI_FORMAT_R8_UINT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_TEXTURE2D_DESC sd = td;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		bool ok = SUCCEEDED(HW.pDevice->CreateBuffer(&bd, nullptr, &s_params)) &&
			SUCCEEDED(HW.pDevice->CreateTexture2D(&td, nullptr, &s_mask_gpu)) &&
			SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(s_mask_gpu, nullptr, &s_mask_uav));
		for (Slot& s : s_slots)
			ok = ok && SUCCEEDED(HW.pDevice->CreateTexture2D(&sd, nullptr, &s.staging));
		if (!ok)
			release();
		return ok;
	}

	float cross(const Fvector2& o, const Fvector2& a, const Fvector2& b)
	{
		return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
	}

	// Convex hull, counter-clockwise
	void hull(xr_vector<Fvector2>& pts, xr_vector<Fvector2>& out)
	{
		std::sort(pts.begin(), pts.end(), [](const Fvector2& a, const Fvector2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
		out.assign(pts.size() * 2, Fvector2());
		size_t k = 0;
		for (size_t i = 0; i < pts.size(); i++)
		{
			while (k >= 2 && cross(out[k - 2], out[k - 1], pts[i]) <= 0) k--;
			out[k++] = pts[i];
		}
		for (size_t i = pts.size() - 1, t = k + 1; i > 0; i--)
		{
			while (k >= t && cross(out[k - 2], out[k - 1], pts[i - 1]) <= 0) k--;
			out[k++] = pts[i - 1];
		}
		out.resize(k > 1 ? k - 1 : k);
	}

	bool inside(const xr_vector<Fvector2>& h, float x, float y)
	{
		if (h.size() < 3)
			return false;
		const Fvector2 p = { x, y };
		for (size_t i = 0; i < h.size(); i++)
		{
			if (cross(h[i], h[(i + 1) % h.size()], p) < 0)
				return false;
		}
		return true;
	}

	void light_xy(const Fmatrix& m, const Fvector& p, float& x, float& y)
	{
		const float w = p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44;
		x = (p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41) / w;
		y = (p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42) / w;
	}

	void finish_stats()
	{
		s_stats = false;
		const double f = double(_max(1u, s_frames_valid));
		Msg("* [Sun casters] per frame over %u frames (%u with receivers), culling %s, static casters per cascade:",
			s_frames, s_frames_valid, ps_r4_sun_caster_cull ? "on" : "off");
		for (u32 c = 0; c < kCascades; c++)
		{
			const u64 t = s_tested[c], k = s_culled[c];
			Msg("*   cascade %u  tested %7.1f, %s %7.1f (%.0f%%)", c, double(t) / f, ps_r4_sun_caster_cull ? "culled" : "would be culled",
				double(k) / f, t ? 100.0 * double(k) / double(t) : 0.0);
		}
		xrLogger::FlushLog();
	}
}

bool wanted()
{
	return ps_r4_sun_caster_cull || s_stats;
}

void start_stats(float seconds)
{
	s_stats = true;
	s_end_time = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
	s_frames = s_frames_valid = 0;
	for (u32 c = 0; c < kCascades; c++)
		s_tested[c] = s_culled[c] = 0;
	Msg("* [Sun casters] measuring for %.0f s...", seconds);
}

void frame_begin(bool usable)
{
	s_valid = false;
	if (!usable)
		return;
	if (s_stats && Device.dwTimeGlobal >= s_end_time)
		finish_stats();
	if (!wanted())
	{
		if (s_mask_gpu)
			release();
		return;
	}
	if (s_stats)
		s_frames++;
	if (s_write - s_read < kSlots - 1)
		return;
	Slot& s = s_slots[s_read % kSlots];
	for (u32 c = 0; c < s.count; c++)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		const UINT sub = D3D11CalcSubresource(0, c, 1);
		if (FAILED(HW.pContext->Map(s.staging, sub, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			return; // not ready: next frame
		for (u32 y = 0; y < kCells; y++)
			CopyMemory(&s_mask[c][y * kCells], (const BYTE*)mapped.pData + y * mapped.RowPitch, kCells);
		HW.pContext->Unmap(s.staging, sub);
	}
	s_read++;

	// That frame's view frustum seen from the sun, per cascade
	Fmatrix inv;
	D3DXMatrixInverse((D3DXMATRIX*)&inv, nullptr, (D3DXMATRIX*)&s.view_proj);
	Fvector corners[8];
	for (int i = 0; i < 8; i++)
	{
		const Fvector ndc = { (i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : 0.f };
		inv.transform(corners[i], ndc);
	}
	s_count = s.count;
	for (u32 c = 0; c < s.count; c++)
	{
		s_xform[c] = s.xform[c];
		xr_vector<Fvector2> pts(8);
		for (int i = 0; i < 8; i++)
			light_xy(s.xform[c], corners[i], pts[i].x, pts[i].y);
		hull(pts, s_hull[c]);
	}
	s_valid = true;
	if (s_stats)
		s_frames_valid++;
}

void dispatch(const Fmatrix* cascade_xforms, u32 count)
{
	if (!wanted() || !create())
		return;
	ID3D11ShaderResourceView* depth = nullptr;
	Fmatrix view_proj;
	u32 width, height;
	if (!hiz::depth_copy(depth, view_proj, width, height))
		return; // no copy this frame
	if (s_write - s_read >= kSlots)
		return;
	count = _min(count, kCascades);

	Slot& s = s_slots[s_write % kSlots];
	s.view_proj = view_proj;
	s.count = count;

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(HW.pContext->Map(s_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	Params* p = (Params*)mapped.pData;
	D3DXMatrixInverse((D3DXMATRIX*)&p->inv_view_proj, nullptr, (D3DXMATRIX*)&view_proj);
	for (u32 c = 0; c < kCascades; c++)
	{
		p->cascade[c] = c < count ? cascade_xforms[c] : Fidentity;
		if (c < count)
			s.xform[c] = cascade_xforms[c];
	}
	p->dims[0] = width;
	p->dims[1] = height;
	p->dims[2] = kCells;
	p->dims[3] = count;
	HW.pContext->Unmap(s_params, 0);

	const UINT zero[4] = { 0, 0, 0, 0 };
	HW.pContext->ClearUnorderedAccessViewUint(s_mask_uav, zero);

	// Leave the compute stage as it was
	ID3D11ComputeShader* prev_cs = nullptr;
	ID3D11ClassInstance* prev_inst[256];
	UINT prev_inst_count = 256;
	ID3D11ShaderResourceView* prev_srv = nullptr;
	ID3D11UnorderedAccessView* prev_uav = nullptr;
	ID3D11Buffer* prev_cb = nullptr;
	HW.pContext->CSGetShader(&prev_cs, prev_inst, &prev_inst_count);
	HW.pContext->CSGetShaderResources(0, 1, &prev_srv);
	HW.pContext->CSGetUnorderedAccessViews(0, 1, &prev_uav);
	HW.pContext->CSGetConstantBuffers(0, 1, &prev_cb);

	HW.pContext->CSSetShader(s_mark, nullptr, 0);
	HW.pContext->CSSetShaderResources(0, 1, &depth);
	HW.pContext->CSSetUnorderedAccessViews(0, 1, &s_mask_uav, nullptr);
	HW.pContext->CSSetConstantBuffers(0, 1, &s_params);
	HW.pContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	ID3D11ShaderResourceView* no_srv = nullptr;
	HW.pContext->CSSetShaderResources(0, 1, &no_srv);
	HW.pContext->CSSetShader(prev_cs, prev_inst, prev_inst_count);
	HW.pContext->CSSetShaderResources(0, 1, &prev_srv);
	UINT keep = UINT(-1);
	HW.pContext->CSSetUnorderedAccessViews(0, 1, &prev_uav, &keep);
	HW.pContext->CSSetConstantBuffers(0, 1, &prev_cb);
	_RELEASE(prev_cs);
	for (UINT i = 0; i < prev_inst_count; i++)
		_RELEASE(prev_inst[i]);
	_RELEASE(prev_srv);
	_RELEASE(prev_uav);
	_RELEASE(prev_cb);

	HW.pContext->CopyResource(s.staging, s_mask_gpu);
	s_write++;
}

bool test_caster(const Fbox& box, u32 c)
{
	if (!s_valid || c >= s_count)
		return false;
	if (s_stats)
		s_tested[c]++;

	float min_x = 2.f, max_x = -2.f, min_y = 2.f, max_y = -2.f;
	for (int i = 0; i < 8; i++)
	{
		Fvector p;
		box.getpoint(i, p);
		float x, y;
		light_xy(s_xform[c], p, x, y);
		min_x = _min(min_x, x);
		max_x = _max(max_x, x);
		min_y = _min(min_y, y);
		max_y = _max(max_y, y);
	}
	// Outside that frame's cascade or view: its shadow may fall where the view is now
	if (min_x < -1.f || max_x > 1.f || min_y < -1.f || max_y > 1.f)
		return false;
	const xr_vector<Fvector2>& h = s_hull[c];
	if (!inside(h, min_x, min_y) || !inside(h, max_x, min_y) || !inside(h, min_x, max_y) || !inside(h, max_x, max_y))
		return false;

	const int n = int(kCells);
	const int x0 = clampr(iFloor((min_x * 0.5f + 0.5f) * n) - kMargin, 0, n - 1);
	const int x1 = clampr(iFloor((max_x * 0.5f + 0.5f) * n) + kMargin, 0, n - 1);
	const int y0 = clampr(iFloor((min_y * 0.5f + 0.5f) * n) - kMargin, 0, n - 1);
	const int y1 = clampr(iFloor((max_y * 0.5f + 0.5f) * n) + kMargin, 0, n - 1);
	const u8* mask = s_mask[c];
	for (int y = y0; y <= y1; y++)
	{
		for (int x = x0; x <= x1; x++)
		{
			if (mask[y * n + x])
				return false;
		}
	}
	if (s_stats)
		s_culled[c]++;
	return ps_r4_sun_caster_cull != 0;
}

void destroy()
{
	release();
	_RELEASE(s_mark);
	s_mark_failed = false;
	s_stats = false;
}
}
