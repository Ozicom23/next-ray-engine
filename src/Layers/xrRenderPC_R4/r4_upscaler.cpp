#include "stdafx.h"
#include "r4_upscaler.h"
#include "r4_dlss.h"
#include "r4_fsr3.h"
#ifdef USE_DX12
#include "r4_xess.h"
#endif

static float halton(u32 index, u32 base)
{
	float f = 1.f, r = 0.f;
	while (index > 0)
	{
		f /= float(base);
		r += f * float(index % base);
		index /= base;
	}
	return r;
}

void CUpscaler::GetJitter(u32 index, u32 phase_count, float& x, float& y)
{
	const u32 i = (index % phase_count) + 1;
	x = halton(i, 2) - 0.5f;
	y = halton(i, 3) - 0.5f;
}

u32 CUpscaler::GetJitterPhaseCount(u32 render_w, u32 out_w)
{
	// At least 8 phases per output pixel covered by a render pixel (NVIDIA and AMD both recommend this)
	const float ratio = float(out_w) / float(_max(render_w, 1u));
	return _max(8u, u32(iCeil(8.f * ratio * ratio)));
}

extern float ps_r4_upscaler_scale; // xrRender_console.cpp

void CUpscaler::GetRenderSizeAndMode(u32 out_w, u32 out_h, u32& quality, u32& render_w, u32& render_h)
{
	quality = _min(quality, u32(upscaler_quality_native));
	if (ps_r4_upscaler_scale <= 0.f || quality == upscaler_quality_native)
	{
		GetRenderSize(out_w, out_h, quality, render_w, render_h);
		return;
	}
	// The mode nearest to the scale (DLSS creates its feature for a mode)
	static const float mode_scale[] = { 1.f / 3.f, 0.5f, 1.f / 1.7f, 1.f / 1.5f };
	const float scale = clampr(ps_r4_upscaler_scale, 0.3f, 0.95f);
	u32 nearest = upscaler_quality_quality;
	for (u32 q = 0; q < 4; q++)
		if (_abs(mode_scale[q] - scale) < _abs(mode_scale[nearest] - scale))
			nearest = q;
	quality = nearest;
	render_w = _max(u32(iFloor(float(out_w) * scale + 0.5f)), 1u);
	render_h = _max(u32(iFloor(float(out_h) * scale + 0.5f)), 1u);
	ClampRenderSize(out_w, out_h, quality, render_w, render_h);
}

CUpscaler* GetUpscaler(u32 type)
{
	switch (type)
	{
	case upscaler_dlss: return &g_DLSS;
	case upscaler_fsr: return &g_FSR3;
#ifdef USE_DX12
	case upscaler_xess: return &g_XeSS;
#endif
	default: return nullptr;
	}
}
