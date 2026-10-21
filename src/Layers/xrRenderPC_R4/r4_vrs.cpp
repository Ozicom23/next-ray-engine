#include "stdafx.h"
#include "r4_vrs.h"
#include "r4_rendertarget.h"
#include <D3DCompiler.h>

int ps_r4_vrs = 0;
int ps_r4_vrs_debug = 0;
float ps_r4_vrs_threshold = 0.03f;

namespace vrs
{
namespace
{
	// One thread per tile: the largest difference between neighbouring pixels across and down (luma of the final
	// image), a D3D12_SHADING_RATE out (2x1: 0x4, 1x2: 0x1, 2x2: 0x5)
	const char* kBuildShader =
		"Texture2D<float4> color : register(t0);\n"
		"RWTexture2D<uint> rate : register(u0);\n"
		"cbuffer c : register(b0) { uint tile; float threshold; uint w; uint h; };\n"
		"float luma(uint2 p) { return dot(color.Load(int3(min(p, uint2(w - 1, h - 1)), 0)).rgb, float3(0.299, 0.587, 0.114)); }\n"
		"[numthreads(8, 8, 1)]\n"
		"void main(uint3 id : SV_DispatchThreadID)\n"
		"{\n"
		"	uint2 base = id.xy * tile;\n"
		"	if (base.x >= w || base.y >= h) return;\n"
		"	float dx = 0, dy = 0;\n"
		"	for (uint y = 0; y < tile; y++)\n"
		"		for (uint x = 0; x < tile; x++)\n"
		"		{\n"
		"			uint2 p = base + uint2(x, y);\n"
		"			float l = luma(p);\n"
		"			dx = max(dx, abs(l - luma(p + uint2(1, 0))));\n"
		"			dy = max(dy, abs(l - luma(p + uint2(0, 1))));\n"
		"		}\n"
		"	rate[id.xy] = (dx < threshold ? 0x4 : 0) | (dy < threshold ? 0x1 : 0);\n"
		"}\n";

	// r4_vrs_debug: the coarse tiles tinted over the frame
	const char* kDebugShader =
		"Texture2D<uint> rate : register(t0);\n"
		"cbuffer c : register(b0) { uint tile; float threshold; uint w; uint h; };\n"
		"float4 vs(uint id : SV_VertexID) : SV_Position\n"
		"{\n"
		"	float2 uv = float2((id << 1) & 2, id & 2);\n"
		"	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"}\n"
		"float4 ps(float4 pos : SV_Position) : SV_Target\n"
		"{\n"
		"	uint r = rate.Load(int3(uint2(pos.xy) / tile, 0));\n"
		"	if (r == 0) discard;\n"
		"	return r == 0x5 ? float4(1, 0, 0, 0.35) : float4(1, 1, 0, 0.25);\n"
		"}\n";

	ID3DComputeShader* s_build = nullptr;
	ID3DVertexShader* s_vs = nullptr;
	ID3DPixelShader* s_ps = nullptr;
	ID3DBuffer* s_cb = nullptr;
	ID3DRasterizerState* s_rs = nullptr;
	ID3DDepthStencilState* s_ds = nullptr;
	ID3DBlendState* s_bs = nullptr;
	bool s_failed = false;

	ID3DTexture2D* s_image = nullptr;
	ID3DUnorderedAccessView* s_uav = nullptr;
	ID3DShaderResourceView* s_srv = nullptr;
	u32 s_w = 0, s_h = 0, s_tile = 0;
	bool s_valid = false; // the image holds a frame of the current size
	bool s_on = false; // begin() set it

	bool create_shaders()
	{
		if (s_build)
			return true;
		if (s_failed)
			return false;
		ID3DBlob* cs = nullptr;
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompile(kBuildShader, xr_strlen(kBuildShader), "vrs_build", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
		if (SUCCEEDED(hr))
			hr = D3DCompile(kDebugShader, xr_strlen(kDebugShader), "vrs_debug", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
		if (SUCCEEDED(hr))
			hr = D3DCompile(kDebugShader, xr_strlen(kDebugShader), "vrs_debug", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &s_build);
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
			D3D11_DEPTH_STENCIL_DESC dd = {};
			hr = HW.pDevice->CreateDepthStencilState(&dd, &s_ds);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_BLEND_DESC bl = {};
			D3D11_RENDER_TARGET_BLEND_DESC& b = bl.RenderTarget[0];
			b.BlendEnable = TRUE;
			b.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			b.BlendOp = D3D11_BLEND_OP_ADD;
			b.SrcBlendAlpha = D3D11_BLEND_ZERO;
			b.DestBlendAlpha = D3D11_BLEND_ONE;
			b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			hr = HW.pDevice->CreateBlendState(&bl, &s_bs);
		}
		if (FAILED(hr))
		{
			Msg("! VRS: shader setup failed: %s", err ? (const char*)err->GetBufferPointer() : "");
			s_failed = true;
		}
		_RELEASE(cs);
		_RELEASE(vs);
		_RELEASE(ps);
		_RELEASE(err);
		return !s_failed;
	}

	void release_image()
	{
		_RELEASE(s_uav);
		_RELEASE(s_srv);
		_RELEASE(s_image);
		s_w = s_h = 0;
		s_valid = false;
	}

	bool ensure_image(u32 w, u32 h, u32 tile)
	{
		if (s_image && s_w == w && s_h == h && s_tile == tile)
			return true;
		release_image();
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = (w + tile - 1) / tile;
		d.Height = (h + tile - 1) / tile;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8_UINT;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &s_image)) ||
		    FAILED(HW.pDevice->CreateUnorderedAccessView(s_image, nullptr, &s_uav)) ||
		    FAILED(HW.pDevice->CreateShaderResourceView(s_image, nullptr, &s_srv)))
		{
			Msg("! VRS: the shading rate image (%ux%u) failed", d.Width, d.Height);
			release_image();
			s_failed = true;
			return false;
		}
		s_w = w;
		s_h = h;
		s_tile = tile;
		return true;
	}
}

void build(CRenderTarget* t)
{
#ifdef USE_DX12
	const u32 tile = HW.pDevice->ShadingRateTileSize();
	if (!ps_r4_vrs || !tile || Device.m_SecondViewport.IsSVPFrame())
	{
		if (!ps_r4_vrs)
			s_valid = false;
		return;
	}
	CRT* color = t->rt_Color._get();
	if (!color || !color->pTexture || !color->pTexture->get_SRView() || !create_shaders() ||
	    !ensure_image(color->dwWidth, color->dwHeight, tile))
		return;

	ID3DDeviceContext* ctx = HW.pContext;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return;
	u32* c = (u32*)m.pData;
	c[0] = tile;
	*(float*)&c[1] = ps_r4_vrs_threshold;
	c[2] = s_w;
	c[3] = s_h;
	ctx->Unmap(s_cb, 0);

	// Raw D3D11 like the upscaler's mask: RCache's state is reset afterwards
	RCache.OnFrameEnd();
	ID3DShaderResourceView* src = color->pTexture->get_SRView();
	ctx->CSSetShader(s_build, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &src);
	ctx->CSSetUnorderedAccessViews(0, 1, &s_uav, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &s_cb);
	ctx->Dispatch(((s_w + tile - 1) / tile + 7) / 8, ((s_h + tile - 1) / tile + 7) / 8, 1);
	ID3DShaderResourceView* no_srv = nullptr;
	ID3DUnorderedAccessView* no_uav = nullptr;
	ctx->CSSetShaderResources(0, 1, &no_srv);
	ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);
	s_valid = true;

	if (ps_r4_vrs_debug)
	{
		ctx->OMSetRenderTargets(1, &color->pRT, nullptr);
		D3D11_VIEWPORT vp = { 0.f, 0.f, float(s_w), float(s_h), 0.f, 1.f };
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(s_vs, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShader(s_ps, nullptr, 0);
		ctx->PSSetShaderResources(0, 1, &s_srv);
		ctx->PSSetConstantBuffers(0, 1, &s_cb);
		ctx->RSSetState(s_rs);
		ctx->OMSetDepthStencilState(s_ds, 0);
		const float blend[4] = {};
		ctx->OMSetBlendState(s_bs, blend, 0xffffffff);
		ctx->Draw(3, 0);
		ctx->PSSetShaderResources(0, 1, &no_srv);
		ID3DRenderTargetView* no_rtv = nullptr;
		ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
	}
	RCache.OnFrameEnd();
#endif
}

void begin()
{
#ifdef USE_DX12
	// The image of the last frame, made at this render size
	if (!ps_r4_vrs || !s_valid || s_w != Device.dwRenderWidth || s_h != Device.dwRenderHeight ||
	    Device.m_SecondViewport.IsSVPFrame())
		return;
	HW.pContext->SetShadingRateImage(s_image);
	s_on = true;
#endif
}

void end()
{
#ifdef USE_DX12
	if (!s_on)
		return;
	HW.pContext->SetShadingRateImage(nullptr);
	s_on = false;
#endif
}

void release()
{
	end();
	release_image();
	_RELEASE(s_build);
	_RELEASE(s_vs);
	_RELEASE(s_ps);
	_RELEASE(s_cb);
	_RELEASE(s_rs);
	_RELEASE(s_ds);
	_RELEASE(s_bs);
	s_failed = false;
}
}
