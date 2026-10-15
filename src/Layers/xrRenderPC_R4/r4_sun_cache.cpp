#include "stdafx.h"
#include "r4_sun_cache.h"

int ps_r4_sun_cascade_cache = 2;
float ps_r4_sun_cascade_cache_drift = 0.5f;

namespace sun_cache
{
namespace
{
	const u32 kCascades = 3;

	// The kept map's window copied into the atlas: a full-viewport triangle, each pixel's depth loaded from the kept map
	const char* kShader =
		"Texture2D<float> kept : register(t0);\n"
		"cbuffer params : register(b0) { int4 window; };\n"
		"float4 vs(uint id : SV_VertexID) : SV_Position\n"
		"{\n"
		"	float2 uv = float2((id << 1) & 2, id & 2);\n"
		"	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"}\n"
		"float ps(float4 pos : SV_Position) : SV_Depth\n"
		"{\n"
		"	return kept.Load(int3(int2(pos.xy) + window.xy, 0));\n"
		"}\n";

	struct entry
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11DepthStencilView* dsv = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		u32 big = 0; // the kept map's side
		bool valid = false;
		Fmatrix xform; // the kept map's transform
		CFrustum frustum; // its casters
		Fvector dir; // the sun's direction it was drawn with
		int ox = 0, oy = 0; // the window: the atlas pixel (0, 0) in the kept map
		// stats
		u32 frames = 0, refresh_sun = 0, refresh_view = 0, refresh_other = 0;
	};
	entry s_e[kCascades];

	ID3D11VertexShader* s_vs = nullptr;
	ID3D11PixelShader* s_ps = nullptr;
	ID3D11Buffer* s_cb = nullptr;
	ID3D11RasterizerState* s_rs = nullptr;
	ID3D11DepthStencilState* s_ds = nullptr;
	bool s_failed = false;

	bool s_stats = false;
	u32 s_end_time = 0, s_debug_time = 0;

	void release(entry& e)
	{
		_RELEASE(e.dsv);
		_RELEASE(e.srv);
		_RELEASE(e.tex);
		e.big = 0;
		e.valid = false;
	}

	// A depth texture in the atlas' format, big on a side
	bool ensure_texture(entry& e, u32 big)
	{
		if (e.tex && e.big == big)
			return true;
		release(e);

		D3D11_TEXTURE2D_DESC atlas = {};
		((ID3D11Texture2D*)RImplementation.Target->rt_smap_depth->pSurface)->GetDesc(&atlas);
		DXGI_FORMAT dsv_format, srv_format;
		switch (atlas.Format)
		{
		case DXGI_FORMAT_R32_TYPELESS: dsv_format = DXGI_FORMAT_D32_FLOAT; srv_format = DXGI_FORMAT_R32_FLOAT; break;
		case DXGI_FORMAT_R24G8_TYPELESS: dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT; srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
		case DXGI_FORMAT_R16_TYPELESS: dsv_format = DXGI_FORMAT_D16_UNORM; srv_format = DXGI_FORMAT_R16_UNORM; break;
		default: return false;
		}

		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = big;
		d.MipLevels = d.ArraySize = 1;
		d.Format = atlas.Format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = dsv_format;
		dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = srv_format;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &e.tex)) || FAILED(HW.pDevice->CreateDepthStencilView(e.tex, &dv, &e.dsv)) ||
			FAILED(HW.pDevice->CreateShaderResourceView(e.tex, &sv, &e.srv)))
		{
			release(e);
			return false;
		}
		e.big = big;
		return true;
	}

	bool create_shaders()
	{
		if (s_ps)
			return true;
		if (s_failed)
			return false;
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompile(kShader, xr_strlen(kShader), "sun_cache", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
		if (SUCCEEDED(hr))
			hr = D3DCompile(kShader, xr_strlen(kShader), "sun_cache", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
		if (SUCCEEDED(hr))
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = 16;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = HW.pDevice->CreateBuffer(&bd, nullptr, &s_cb);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_RASTERIZER_DESC rd = {};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			hr = HW.pDevice->CreateRasterizerState(&rd, &s_rs);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_DEPTH_STENCIL_DESC dd = {}; // every pixel's depth written
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			hr = HW.pDevice->CreateDepthStencilState(&dd, &s_ds);
		}
		if (FAILED(hr))
		{
			Msg("! Sun cascade cache: shader setup failed: %s", err ? (const char*)err->GetBufferPointer() : "");
			s_failed = true;
		}
		_RELEASE(vs);
		_RELEASE(ps);
		_RELEASE(err);
		return !s_failed;
	}

	void finish_stats()
	{
		s_stats = false;
		for (u32 c = 1; c < kCascades; c++)
		{
			const entry& e = s_e[c];
			if (!e.frames)
				continue;
			const u32 refreshes = e.refresh_sun + e.refresh_view + e.refresh_other;
			Msg("* [Sun cascade cache] cascade %u: %u frames, %u from the kept map (%.0f%%), %u refreshes (the sun turned %u, the view moved %u, other %u), kept map %u px",
				c, e.frames, e.frames - refreshes, 100.f * float(e.frames - refreshes) / float(e.frames), refreshes, e.refresh_sun, e.refresh_view, e.refresh_other, e.big);
		}
		xrLogger::FlushLog();
	}
}

void start_stats(float seconds)
{
	for (entry& e : s_e)
		e.frames = e.refresh_sun = e.refresh_view = e.refresh_other = 0;
	s_stats = true;
	s_end_time = Device.dwTimeGlobal + u32(seconds * 1000.f);
	Msg("* [Sun cascade cache] measuring for %.0f s...", seconds);
}

mode frame(u32 cascade, u32 cascade_count, const Fmatrix& xform, u32 used, float size, float far_plane, const Fvector& sun_dir, Fmatrix& window_xform)
{
	if (s_stats && Device.dwTimeGlobal >= s_end_time)
		finish_stats();
	if (ps_r4_sun_cascade_cache <= 0 || cascade == 0 || cascade >= kCascades)
		return m_off;
	if (ps_r4_sun_cascade_cache == 1 && cascade + 1 != cascade_count)
		return m_off;
	// Semi-transparent casters (a color map per frame), the 3D scope's view and the min/max map: as usual
	if (RImplementation.o.Tshadows || Device.m_SecondViewport.IsSVPFrame() || RImplementation.Target->use_minmax_sm_this_frame())
		return m_off;
	if (!create_shaders())
		return m_off;

	entry& e = s_e[cascade];
	const u32 pad = (used / 8 + 3) & ~3u; // the margin on each side, in texels
	const u32 big = used + 2 * pad;
	if (!ensure_texture(e, big))
		return m_off;
	if (s_stats)
		e.frames++;

	mode result = m_window;
	u32* reason = nullptr;
	// The receivers lie in a slab around the camera along the light: from twice the box above it to the far plane.
	// The cascade's depth range starts far toward the sun and its origin moves with the sun light's position, so
	// only the slab is tested against the kept box: the camera's depth in the cascade, and the range's length in
	// meters from the transform's depth scale
	Fvector cam_ndc;
	xform.transform(cam_ndc, Device.vCameraPosition);
	const float z_len = 1.f / _max(Fvector().set(xform._13, xform._23, xform._33).magnitude(), 1e-6f);
	const float z_cam = clampr(cam_ndc.z, 0.f, 1.f);
	const float z_lo = clampr(z_cam - 2.f * size / z_len, 0.f, 1.f);
	if (e.valid)
	{
		// The sun turned: the shadow of a caster up to four box sides away along the light moves by the allowed
		// texels (the sine of a small angle, a cosine of it rounds to 1)
		Fvector turned;
		turned.crossproduct(e.dir, sun_dir);
		if (turned.magnitude() > sinf(ps_r4_sun_cascade_cache_drift / (4.f * float(used))))
			reason = &e.refresh_sun;
	}
	if (e.valid && !reason)
	{
		// The cascade's slab inside the kept box (the view moved or turned, the depth range), and its window there
		Fmatrix inv; // the cascade's transform is orthographic: affine
		inv.invert(xform);
		bool inside = true;
		for (u32 i = 0; i < 8 && inside; i++)
		{
			Fvector w, l;
			inv.transform(w, Fvector().set((i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : z_lo));
			e.xform.transform(l, w);
			const float eps = 1e-4f;
			inside = l.x >= -1.f - eps && l.x <= 1.f + eps && l.y >= -1.f - eps && l.y <= 1.f + eps && l.z >= -eps && l.z <= 1.f + eps;
			if (!inside && s_stats && Device.dwTimeGlobal >= s_debug_time)
			{
				s_debug_time = Device.dwTimeGlobal + 1000;
				Fvector cam_kept;
				e.xform.transform(cam_kept, Device.vCameraPosition);
				Msg("* [Sun cascade cache] cascade %u corner %u outside the kept box: %.4f %.4f %.4f (camera depth %.4f, in the kept box %.4f, range %.0f m, size %.0f, window %d %d, kept %u used %u)",
					cascade, i, l.x, l.y, l.z, z_cam, cam_kept.z, z_len, size, e.ox, e.oy, big, used);
				Msg("*   cascade z column %.6f %.6f %.6f %.6f, kept z column %.6f %.6f %.6f %.6f, corner world %.2f %.2f %.2f",
					xform._13, xform._23, xform._33, xform._43, e.xform._13, e.xform._23, e.xform._33, e.xform._43, w.x, w.y, w.z);
			}
		}
		if (inside)
		{
			Fvector w, l;
			inv.transform(w, Fvector().set(-1.f, 1.f, z_cam)); // the atlas' top left corner, at the camera's depth
			e.xform.transform(l, w);
			const int ox = iFloor((l.x + 1.f) * 0.5f * float(big) + 0.5f);
			const int oy = iFloor((1.f - l.y) * 0.5f * float(big) + 0.5f);
			if (ox >= 0 && oy >= 0 && ox + int(used) <= int(big) && oy + int(used) <= int(big))
			{
				e.ox = ox;
				e.oy = oy;
			}
			else
				inside = false;
		}
		if (!inside)
			reason = &e.refresh_view;
	}
	if (!e.valid)
		reason = &e.refresh_other;

	if (reason)
	{
		if (s_stats)
			++*reason;
		// The kept map's transform: the cascade's with the box grown to the map and the depth range by a quarter
		// of the box, both about the center
		const float k = float(big) / float(used);
		const float z_margin = 0.25f * size;
		Fmatrix grow;
		grow.identity();
		grow._11 = 1.f / k;
		grow._22 = 1.f / k;
		grow._33 = (far_plane - 0.1f) / (far_plane + z_margin - 0.1f);
		e.xform.mul(grow, xform);
		e.frustum.CreateFromMatrix(e.xform, FRUSTUM_P_ALL);
		e.dir = sun_dir;
		e.ox = e.oy = int(pad);
		e.valid = true;
		result = m_refresh;
	}

	// The window's transform: the kept map's, with the window mapped to the whole atlas viewport
	const float s = float(big) / float(used);
	const float cx = (float(e.ox) + float(used) * 0.5f) / float(big) * 2.f - 1.f;
	const float cy = 1.f - (float(e.oy) + float(used) * 0.5f) / float(big) * 2.f;
	Fmatrix window;
	window.identity();
	window._11 = s;
	window._22 = s;
	window._41 = -cx * s;
	window._42 = -cy * s;
	window_xform.mul(window, e.xform);
	return result;
}

Fmatrix& big_xform(u32 cascade) { return s_e[cascade].xform; }
CFrustum& big_frustum(u32 cascade) { return s_e[cascade].frustum; }

void begin_refresh(u32 cascade)
{
	entry& e = s_e[cascade];
	HW.pContext->ClearDepthStencilView(e.dsv, D3D_CLEAR_DEPTH, 1.0f, 0L);
	RCache.set_RT(NULL, 0);
	RCache.set_RT(NULL, 1);
	RCache.set_RT(NULL, 2);
	RCache.set_ZB(e.dsv);
	D3D_VIEWPORT VP = { 0.f, 0.f, float(e.big), float(e.big), 0.f, 1.f };
	HW.pContext->RSSetViewports(1, &VP);
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(FALSE);
	RCache.set_ColorWriteEnable(FALSE);
}

void draw_window(u32 cascade, ID3DDepthStencilView* atlas, u32 used)
{
	entry& e = s_e[cascade];
	ID3D11DeviceContext* ctx = HW.pContext;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	int* p = (int*)m.pData;
	p[0] = e.ox;
	p[1] = e.oy;
	p[2] = p[3] = 0;
	ctx->Unmap(s_cb, 0);

	RCache.OnFrameEnd(); // nothing of the backend's stays bound (the atlas as a texture of the last accumulation)
	ctx->OMSetRenderTargets(0, nullptr, atlas);
	D3D11_VIEWPORT vp = { 0.f, 0.f, float(used), float(used), 0.f, 1.f };
	ctx->RSSetViewports(1, &vp);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(s_vs, nullptr, 0);
	ctx->PSSetShader(s_ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &e.srv);
	ctx->PSSetConstantBuffers(0, 1, &s_cb);
	ctx->RSSetState(s_rs);
	ctx->OMSetDepthStencilState(s_ds, 0);
	ctx->Draw(3, 0);
	RCache.OnFrameEnd(); // the backend sets its targets and state anew
}

void destroy()
{
	for (entry& e : s_e)
		release(e);
	_RELEASE(s_vs);
	_RELEASE(s_ps);
	_RELEASE(s_cb);
	_RELEASE(s_rs);
	_RELEASE(s_ds);
	s_failed = false;
}
}
