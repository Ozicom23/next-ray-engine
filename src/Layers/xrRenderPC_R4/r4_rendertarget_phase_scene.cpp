#include "stdafx.h"

// startup
void CRenderTarget::phase_scene_prepare()
{
	PIX_EVENT(phase_scene_prepare);

		//	TODO: DX10: Check if we need to set RT here.
		if (!RImplementation.o.dx10_msaa)
			u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, rt_Position->pRT,NULL,NULL, HW.pBaseZB);
		else
			u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, rt_Position->pRT,NULL,NULL, rt_MSAADepth->pZRT);

		FLOAT ColorRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		HW.pContext->ClearRenderTargetView(rt_Position->pRT, ColorRGBA);

		//--DSR-- HeatVision_start
		HW.pContext->ClearRenderTargetView(rt_Heat->pRT, ColorRGBA);
		//--DSR-- HeatVision_end

		if (!RImplementation.o.dx10_msaa)
			HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);
		else
		{
			HW.pContext->ClearDepthStencilView(rt_MSAADepth->pZRT, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);
			HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);
		}

	//	Igor: for volumetric lights
	m_bHasActiveVolumetric = false;
	m_bHasActiveVolumetric_spot = false;
	//	Clear later if try to draw volumetric
}

// begin
void CRenderTarget::phase_scene_begin()
{
	// Upscaler texture LOD bias applies to scene materials only, not to what is drawn after upscaling
	SSManager.SetMaxAnisotropyAndMipLODBias(ps_r__tf_Anisotropic, ps_r__tf_Mipbias + r__upscaler_mip_bias);
	
	ID3DDepthStencilView* pZB = HW.pBaseZB;

	if (RImplementation.o.dx10_msaa)
		pZB = rt_MSAADepth->pZRT;

	// Targets, use accumulator for temporary storage
	{
		//--DSR-- HeatVision_start
		if (RImplementation.o.albedo_wo) u_setrt(rt_Position, rt_Accumulator, rt_Heat, rt_ssfx_motion_vectors, pZB);
		else u_setrt(rt_Position, rt_Color, rt_Heat, rt_ssfx_motion_vectors, pZB);		
		//--DSR-- HeatVision_end
	}


	// Stencil - write 0x1 at pixel pos
	RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE,
	                   D3DSTENCILOP_KEEP);

	// Misc		- draw only front-faces
	//	TODO: DX10: siable two-sided stencil here
	//CHK_DX(HW.pDevice->SetRenderState	( D3DRS_TWOSIDEDSTENCILMODE,FALSE				));
	RCache.set_CullMode(CULL_CCW);
	RCache.set_ColorWriteEnable();
}

void CRenderTarget::disable_aniso()
{
	SSManager.SetMaxAnisotropyAndMipLODBias(1, ps_r__tf_Mipbias);
}

// end
void CRenderTarget::phase_scene_end()
{
	disable_aniso();

	RCache.set_RT(NULL, 3); // Always reset the 4th RT ( Motion Vectors )

	if (!RImplementation.o.albedo_wo) return;

	// transfer from "rt_Accumulator" into "rt_Color"
	if (!RImplementation.o.dx10_msaa)
		u_setrt(rt_Color, 0, 0, HW.pBaseZB);
	else
		u_setrt(rt_Color, 0, 0, rt_MSAADepth->pZRT);
	RCache.set_CullMode(CULL_NONE);
	RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00); // stencil should be >= 1
	if (RImplementation.o.nvstencil) u_stencil_optimize(CRenderTarget::SO_Combine);
	RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00); // stencil should be >= 1
	RCache.set_ColorWriteEnable();

	// common calc for quad-rendering
	u32 Offset;
	u32 C = color_rgba(255, 255, 255, 255);
	float _w = float(Device.dwRenderWidth);
	float _h = float(Device.dwRenderHeight);
	Fvector2 p0, p1;
	p0.set(.5f / _w, .5f / _h);
	p1.set((_w + .5f) / _w, (_h + .5f) / _h);
	float d_Z = EPS_S, d_W = 1.f;

	// Fill vertex buffer
	FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, g_combine->vb_stride, Offset);
	pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
	pv++;
	pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
	pv++;
	pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
	pv++;
	pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
	pv++;
	RCache.Vertex.Unlock(4, g_combine->vb_stride);

	// if (stencil>=1 && aref_pass)	stencil = light_id
	RCache.set_Element(s_accum_mask->E[SE_MASK_ALBEDO]); // masker
	RCache.set_Geometry(g_combine);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

// r4_shader_patches 0x400 (CRender::nextray_depth_latch): the G-buffer's view depth copied into rt_nextray_depth once
// the G-buffer is complete (after wall marks and rain), for the patch mod's screen-space ray marches. rt_Position.z is
// a 16 bit float and so is the copy: the same values. Raw D3D11 like the sun cache's window copy, with the backend's
// state dropped before and after (it binds its targets and state anew); viewport and scissor rects are kept
namespace nextray_depth
{
	ID3D11VertexShader* s_vs = nullptr;
	ID3D11PixelShader* s_ps = nullptr;
	ID3D11RasterizerState* s_rs = nullptr;
	ID3D11DepthStencilState* s_ds = nullptr;
	ID3D11ShaderResourceView* s_srv = nullptr;
	ID3D11Resource* s_srv_res = nullptr; // s_srv is of this
	bool s_failed = false;

	const char* kShader =
		"Texture2D<float4> s_pos : register(t0);\n"
		"float4 vs(uint id : SV_VertexID) : SV_Position\n"
		"{\n"
		"	float2 uv = float2((id << 1) & 2, id & 2);\n"
		"	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"}\n"
		"float ps(float4 pos : SV_Position) : SV_Target\n"
		"{\n"
		"	return s_pos.Load(int3(pos.xy, 0)).z;\n"
		"}\n";

	void release()
	{
		_RELEASE(s_vs);
		_RELEASE(s_ps);
		_RELEASE(s_rs);
		_RELEASE(s_ds);
		_RELEASE(s_srv);
		s_srv_res = nullptr;
		s_failed = false;
	}

	bool create(ID3D11Resource* pos)
	{
		if (s_failed)
			return false;
		HRESULT hr = S_OK;
		if (!s_ps)
		{
			ID3DBlob* vs = nullptr;
			ID3DBlob* ps = nullptr;
			ID3DBlob* err = nullptr;
			hr = D3DCompile(kShader, xr_strlen(kShader), "nextray_depth", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
			if (SUCCEEDED(hr))
				hr = D3DCompile(kShader, xr_strlen(kShader), "nextray_depth", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
			if (SUCCEEDED(hr))
				hr = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
			if (SUCCEEDED(hr))
				hr = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
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
				D3D11_DEPTH_STENCIL_DESC dd = {}; // no depth, no stencil
				hr = HW.pDevice->CreateDepthStencilState(&dd, &s_ds);
			}
			if (FAILED(hr))
				Msg("! nextray depth copy: shader setup failed: %s", err ? (const char*)err->GetBufferPointer() : "");
			_RELEASE(vs);
			_RELEASE(ps);
			_RELEASE(err);
		}
		if (SUCCEEDED(hr) && s_srv_res != pos)
		{
			_RELEASE(s_srv);
			s_srv_res = nullptr;
			hr = HW.pDevice->CreateShaderResourceView(pos, nullptr, &s_srv);
			if (SUCCEEDED(hr))
				s_srv_res = pos;
		}
		if (FAILED(hr))
		{
			Msg("! nextray depth copy: setup failed (0x%08x)", hr);
			s_failed = true;
		}
		return !s_failed;
	}
}

void nextray_depth_release() { nextray_depth::release(); }

void CRenderTarget::phase_nextray_depth()
{
	if (!RImplementation.o.nextray_depth || !rt_nextray_depth._get() || !nextray_depth::create(rt_Position->pSurface))
		return;
	PIX_EVENT(NEXTRAY_DEPTH_COPY);

	ID3D11DeviceContext* ctx = HW.pContext;
	D3D11_VIEWPORT vp_saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT vp_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	ctx->RSGetViewports(&vp_count, vp_saved);
	D3D11_RECT sc_saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT sc_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	ctx->RSGetScissorRects(&sc_count, sc_saved);

	RCache.OnFrameEnd(); // nothing of the backend's stays bound (rt_Position as a target)
	ID3D11RenderTargetView* rtv = rt_nextray_depth->pRT;
	ctx->OMSetRenderTargets(1, &rtv, nullptr);
	D3D11_VIEWPORT vp = { 0.f, 0.f, float(rt_nextray_depth->dwWidth), float(rt_nextray_depth->dwHeight), 0.f, 1.f };
	ctx->RSSetViewports(1, &vp);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(nextray_depth::s_vs, nullptr, 0);
	ctx->PSSetShader(nextray_depth::s_ps, nullptr, 0);
	ctx->PSSetShaderResources(0, 1, &nextray_depth::s_srv);
	ctx->RSSetState(nextray_depth::s_rs);
	ctx->OMSetDepthStencilState(nextray_depth::s_ds, 0);
	ctx->Draw(3, 0);
	RCache.OnFrameEnd(); // the backend sets its targets and state anew

	if (vp_count)
		ctx->RSSetViewports(vp_count, vp_saved);
	if (sc_count)
		ctx->RSSetScissorRects(sc_count, sc_saved);
}
