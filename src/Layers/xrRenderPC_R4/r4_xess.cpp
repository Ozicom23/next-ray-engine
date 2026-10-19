#include "stdafx.h"
#include "r4_xess.h"

CXeSS g_XeSS;

// Render scale per quality mode: XeSS 1.2's factors (xessForceLegacyScaleFactors), the same as DLSS's and FSR's, so a
// mode renders at the same resolution with every upscaler
static const float s_xess_scale[] = { 1.f / 3.f, 0.5f, 1.f / 1.7f, 1.f / 1.5f, 1.f };

#if defined(USE_DX12) && defined(XR_XESS_SDK)

#include <xess/xess_d3d12.h>

#pragma comment(lib, "libxess.lib")

static xess_quality_settings_t to_xess_quality(u32 quality)
{
	switch (quality)
	{
	case upscaler_quality_ultra_performance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
	case upscaler_quality_performance: return XESS_QUALITY_SETTING_PERFORMANCE;
	case upscaler_quality_balanced: return XESS_QUALITY_SETTING_BALANCED;
	case upscaler_quality_native: return XESS_QUALITY_SETTING_AA;
	default: return XESS_QUALITY_SETTING_QUALITY;
	}
}

static void xess_log_callback(const char* message, xess_logging_level_t level)
{
	Msg("%s[XeSS] %s", level >= XESS_LOGGING_LEVEL_WARNING ? "! " : "", message ? message : "");
}

// Input LDR (the tonemapped scene), motion vectors at render resolution without jitter, regular depth
static const uint32_t s_init_flags = XESS_INIT_FLAG_LDR_INPUT_COLOR;

bool CXeSS::Init()
{
	if (m_initialized)
		return m_available;
	m_initialized = true;
	m_available = false;

	// Delay-loaded: a missing DLL must not reach the first XeSS call
	if (!LoadLibraryW(L"libxess.dll"))
	{
		Msg("! XeSS: libxess.dll not found next to the exe");
		return false;
	}

	xess_version_t ver = {};
	xessGetVersion(&ver);

	xess_context_handle_t ctx = nullptr;
	const xess_result_t res = xessD3D12CreateContext(HW.pDevice->Native(), &ctx);
	if (res != XESS_RESULT_SUCCESS || !ctx)
	{
		Msg("! XeSS %u.%u.%u: not supported on this device (%d, needs Shader Model 6.4)", ver.major, ver.minor, ver.patch,
		    int(res));
		return false;
	}
	m_context = ctx;
	xessSetLoggingCallback(ctx, XESS_LOGGING_LEVEL_WARNING, xess_log_callback);
	xessForceLegacyScaleFactors(ctx, true);
	if (xessIsOptimalDriver(ctx) != XESS_RESULT_SUCCESS)
		Msg("! XeSS: the driver is older than XeSS recommends");

	Msg("* XeSS %u.%u.%u: Super Resolution available", ver.major, ver.minor, ver.patch);
	m_available = true;
	return true;
}

void CXeSS::Shutdown()
{
	ReleaseFeature();
	if (m_context)
	{
		xessDestroyContext((xess_context_handle_t)m_context);
		m_context = nullptr;
	}
	m_available = false;
	m_initialized = false;
}

void CXeSS::GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h)
{
	quality = _min(quality, u32(upscaler_quality_native));
	render_w = u32(iFloor(float(out_w) * s_xess_scale[quality] + 0.5f));
	render_h = u32(iFloor(float(out_h) * s_xess_scale[quality] + 0.5f));

	if (m_context)
	{
		const xess_2d_t out = { out_w, out_h };
		xess_2d_t in = {};
		if (xessGetInputResolution((xess_context_handle_t)m_context, &out, to_xess_quality(quality), &in) ==
		        XESS_RESULT_SUCCESS && in.x && in.y)
		{
			render_w = in.x;
			render_h = in.y;
		}
	}

	render_w = _max(render_w, 1u);
	render_h = _max(render_h, 1u);
}

void CXeSS::ClampRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h)
{
	if (!m_context || quality == upscaler_quality_native)
		return;
	const xess_2d_t out = { out_w, out_h };
	xess_2d_t opt = {}, lo = {}, hi = {};
	if (xessGetOptimalInputResolution((xess_context_handle_t)m_context, &out, to_xess_quality(quality), &opt, &lo, &hi) !=
	    XESS_RESULT_SUCCESS)
		return;
	if (lo.x && lo.y)
	{
		render_w = _max(render_w, u32(lo.x));
		render_h = _max(render_h, u32(lo.y));
	}
	if (hi.x && hi.y)
	{
		render_w = _min(render_w, u32(hi.x));
		render_h = _min(render_h, u32(hi.y));
	}
}

bool CXeSS::InitFeature(bool mask)
{
	xess_d3d12_init_params_t init = {};
	init.outputResolution = { m_out_w, m_out_h };
	init.qualitySetting = to_xess_quality(m_quality);
	init.initFlags = s_init_flags | (mask ? XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK : 0);
	const xess_result_t res = xessD3D12Init((xess_context_handle_t)m_context, &init);
	if (res != XESS_RESULT_SUCCESS)
	{
		Msg("! XeSS: initialization failed (%d) for %ux%u, quality %u", int(res), m_out_w, m_out_h, m_quality);
		return false;
	}
	m_mask = mask;
	m_feature = true;
	return true;
}

bool CXeSS::CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality)
{
	ReleaseFeature();
	if (!m_available || !m_context)
		return false;

	m_out_w = out_w;
	m_out_h = out_h;
	m_quality = quality;
	// The reactive mask is usually on (r4_upscaler_reactive); Evaluate initializes again if a frame has none
	if (!InitFeature(true))
		return false;

	m_eval_failures = 0;
	m_eval_failed = false;
	Msg("* XeSS: initialized, %ux%u -> %ux%u, quality %u", render_w, render_h, out_w, out_h, quality);
	return true;
}

void CXeSS::ReleaseFeature()
{
	if (m_feature)
	{
		// Initializing again or destroying: the frames in flight may still use its resources
		if (HW.pDevice)
			HW.pDevice->WaitIdle();
		m_feature = false;
	}
}

bool CXeSS::Evaluate(const UpscalerEvalParams& p)
{
	if (!m_feature)
		return false;

	// The mask comes and goes with r4_upscaler_reactive: XeSS wants it in every frame when initialized with it
	if (!!p.reactive != m_mask)
	{
		HW.pContext->Synchronize();
		HW.pDevice->WaitIdle();
		if (!InitFeature(!!p.reactive))
			return false;
	}

	xess_context_handle_t ctx = (xess_context_handle_t)m_context;
	const float mv_x = p.mv_scale_x, mv_y = p.mv_scale_y;

	xess_d3d12_execute_params_t exec = {};
	exec.jitterOffsetX = p.jitter_x;
	exec.jitterOffsetY = p.jitter_y;
	exec.exposureScale = 1.f;
	exec.resetHistory = p.reset ? 1 : 0;
	exec.inputWidth = p.render_w;
	exec.inputHeight = p.render_h;

	// Recorded in order with the frame's commands (on the context's recording thread when threaded): a failure is
	// known there, and this call reports the one of the frame before
	const bool failed_before = m_eval_failed.exchange(false);
	dx12::Resource* color = p.color;
	dx12::Resource* output = p.output;
	dx12::Resource* depth = p.depth;
	dx12::Resource* motion = p.motion;
	dx12::Resource* reactive = p.reactive;
	HW.pContext->ExecuteNative([this, exec, ctx, mv_x, mv_y, color, output, depth, motion, reactive](dx12::Context::NativeRecorder& r) mutable {
		// Our motion vectors are (current - previous) in UV units: this scale makes them (previous - current) in pixels
		xessSetVelocityScale(ctx, mv_x, mv_y);
		// XeSS reads its inputs from compute and writes the output as UAV; the context's tracking follows
		r.Transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		r.Transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (reactive)
			r.Transition(reactive, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		r.Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		exec.pColorTexture = color ? color->Native() : nullptr;
		exec.pOutputTexture = output ? output->Native() : nullptr;
		exec.pDepthTexture = depth ? depth->Native() : nullptr;
		exec.pVelocityTexture = motion ? motion->Native() : nullptr;
		exec.pResponsivePixelMaskTexture = reactive ? reactive->Native() : nullptr;
		const xess_result_t res = xessD3D12Execute(ctx, r.List(), &exec);
		if (res != XESS_RESULT_SUCCESS)
		{
			// Don't flood the log if every frame fails
			if (m_eval_failures++ < 8)
				Msg("! XeSS: execute failed (%d)", int(res));
			m_eval_failed = true;
		}
	});
	return !failed_before;
}

#else // USE_DX12 && XR_XESS_SDK

bool CXeSS::Init()
{
#ifdef USE_DX12
	Msg("! XeSS: this build was compiled without the Intel XeSS SDK");
#else
	Msg("! XeSS: in the DX12 renderer only");
#endif
	m_initialized = true;
	m_available = false;
	return false;
}

void CXeSS::Shutdown()
{
	m_initialized = false;
	m_available = false;
}

void CXeSS::GetRenderSize(u32 out_w, u32 out_h, u32, u32& render_w, u32& render_h)
{
	render_w = out_w;
	render_h = out_h;
}

void CXeSS::ClampRenderSize(u32, u32, u32, u32&, u32&) {}
bool CXeSS::InitFeature(bool) { return false; }
bool CXeSS::CreateFeature(u32, u32, u32, u32, u32) { return false; }
void CXeSS::ReleaseFeature() {}
bool CXeSS::Evaluate(const UpscalerEvalParams&) { return false; }

#endif // USE_DX12 && XR_XESS_SDK
