// FSR 3.1 frame generation for the DX12 exe (see r4_framegen.h), on AMD's FidelityFX SDK v1.1.4 (MIT): the frame
// interpolation swap chain, its D3D12 backend, optical flow and frame interpolation. This file does what AMD's FSR3
// context does in interpolation-only mode: the contexts and their shared resources, the per-frame prepare pass
// (depth and motion vectors dilated at render resolution) and the frame generation callback the swap chain calls at
// Present with its own command list. The prepare pass and the swap chain configuration are recorded through
// ExecuteNative, so they run in order with the frame's commands (on the context's recording thread when threaded),
// and the callback runs in that thread's Present.
#include "stdafx.h"

#ifdef USE_DX12
#include "r4_framegen.h"
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

// The frame interpolation swap chain's pacing (DWM timing, timer resolution)
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

int ps_r4_framegen = 0;

namespace framegen
{
namespace
{
const size_t kMaxContexts = 4; // optical flow, frame interpolation, shared resources, and one to spare

FfxSwapchain s_swap = nullptr;
FfxInterface s_iface = {};
void* s_scratch = nullptr;
bool s_ready = false;
u32 s_render_w = 0, s_render_h = 0, s_display_w = 0, s_display_h = 0;
DXGI_FORMAT s_format = DXGI_FORMAT_UNKNOWN;
FfxOpticalflowContext s_of = {};
FfxFrameInterpolationContext s_fi = {};
FfxUInt32 s_shared = 0;
FfxResourceInternal s_of_vector = {}, s_of_scd = {}, s_dilated_depth = {}, s_dilated_mv = {}, s_prev_depth = {};
u64 s_frame_id = 0;
bool s_had_inputs = false; // frame() ran since the last Present
bool s_configured_on = false;
std::atomic<u32> s_logged{ 0 };

// Set where the commands are recorded, read by the callback in the same thread's Present
FfxFrameInterpolationPrepareDescription s_prepared[2] = {};
FfxResource s_hudless = {};

ID3DTexture2D* s_hudless_tex = nullptr;

void log_error(const char* what, FfxErrorCode e)
{
	if (s_logged++ < 16)
		Msg("! FSR frame generation: %s failed (0x%08x)", what, u32(e));
}

FfxResource ffx_resource(ID3DResource* r, const wchar_t* name)
{
	ID3D12Resource* native = r ? r->Native() : nullptr;
	if (!native)
		return FfxResource{};
	return ffxGetResourceDX12(native, ffxGetResourceDescriptionDX12(native), name, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
}

// The swap chain's frame generation callback: optical flow from the HUD-less image, then the interpolated frame
FfxErrorCode dispatch(const FfxFrameGenerationDispatchDescription* d, void*)
{
	const FfxFrameInterpolationPrepareDescription& prep = s_prepared[d->frameID & 1];

	FfxOpticalflowDispatchDescription of = {};
	of.commandList = d->commandList;
	of.color = s_hudless.resource ? s_hudless : d->presentColor;
	of.reset = d->reset;
	of.backbufferTransferFunction = d->backBufferTransferFunction;
	of.minMaxLuminance.x = d->minMaxLuminance[0];
	of.minMaxLuminance.y = d->minMaxLuminance[1];
	of.opticalFlowVector = s_iface.fpGetResource(&s_iface, s_of_vector);
	of.opticalFlowSCD = s_iface.fpGetResource(&s_iface, s_of_scd);
	FfxErrorCode e = ffxOpticalflowContextDispatch(&s_of, &of);
	if (e != FFX_OK)
		log_error("optical flow", e);

	FfxFrameInterpolationDispatchDescription fi = {};
	fi.commandList = d->commandList;
	fi.displaySize.width = d->presentColor.description.width;
	fi.displaySize.height = d->presentColor.description.height;
	fi.currentBackBuffer = d->presentColor;
	fi.currentBackBuffer_HUDLess = s_hudless;
	fi.renderSize = prep.renderSize;
	fi.output = d->outputs[0];
	fi.opticalFlowVector = of.opticalFlowVector;
	fi.opticalFlowSceneChangeDetection = of.opticalFlowSCD;
	fi.opticalFlowBlockSize = 8;
	fi.opticalFlowScale = { 1.f / float(fi.displaySize.width), 1.f / float(fi.displaySize.height) };
	fi.frameTimeDelta = prep.frameTimeDelta;
	fi.reset = d->reset;
	fi.cameraNear = prep.cameraNear;
	fi.cameraFar = prep.cameraFar;
	fi.viewSpaceToMetersFactor = prep.viewSpaceToMetersFactor;
	fi.cameraFovAngleVertical = prep.cameraFovAngleVertical;
	fi.interpolationRect = d->interpolationRect;
	fi.frameID = d->frameID;
	fi.dilatedDepth = prep.dilatedDepth;
	fi.dilatedMotionVectors = prep.dilatedMotionVectors;
	fi.reconstructedPrevDepth = prep.reconstructedPrevDepth;
	fi.backBufferTransferFunction = d->backBufferTransferFunction;
	fi.minMaxLuminance[0] = d->minMaxLuminance[0];
	fi.minMaxLuminance[1] = d->minMaxLuminance[1];
	const FfxErrorCode fe = ffxFrameInterpolationDispatch(&s_fi, &fi);
	if (fe != FFX_OK)
		log_error("frame interpolation", fe);
	return e | fe;
}

void configure(bool on, u64 frame_id)
{
	FfxFrameGenerationConfig c = {};
	c.swapChain = s_swap;
	c.frameGenerationCallback = on ? dispatch : nullptr;
	c.frameGenerationEnabled = on;
	c.allowAsyncWorkloads = false;
	c.HUDLessColor = on ? s_hudless : FfxResource{};
	c.onlyPresentInterpolated = false;
	c.interpolationRect = { 0, 0, int(s_display_w), int(s_display_h) };
	c.frameID = frame_id;
	const FfxErrorCode e = ffxSetFrameGenerationConfigToSwapchainDX12(&c);
	if (e != FFX_OK)
		log_error("swap chain configuration", e);
}

void destroy_contexts()
{
	if (!s_ready)
		return;
	FfxResourceInternal* shared[] = { &s_of_vector, &s_of_scd, &s_dilated_depth, &s_dilated_mv, &s_prev_depth };
	for (FfxResourceInternal* r : shared)
		s_iface.fpDestroyResource(&s_iface, *r, s_shared);
	s_iface.fpDestroyBackendContext(&s_iface, s_shared);
	ffxFrameInterpolationContextDestroy(&s_fi);
	ffxOpticalflowContextDestroy(&s_of);
	free(s_scratch);
	s_scratch = nullptr;
	s_ready = false;
}

bool create_contexts(const FrameParams& p, DXGI_FORMAT format)
{
	const size_t size = ffxGetScratchMemorySizeDX12(kMaxContexts);
	s_scratch = calloc(1, size);
	FfxErrorCode e = ffxGetInterfaceDX12(&s_iface, ffxGetDeviceDX12(HW.pDevice->Native()), s_scratch, size, kMaxContexts);
	if (e != FFX_OK)
	{
		log_error("backend interface", e);
		free(s_scratch);
		s_scratch = nullptr;
		return false;
	}

	FfxOpticalflowContextDescription of = {};
	of.backendInterface = s_iface;
	of.resolution = { p.display_w, p.display_h };
	if ((e = ffxOpticalflowContextCreate(&s_of, &of)) != FFX_OK)
	{
		log_error("optical flow context", e);
		free(s_scratch);
		s_scratch = nullptr;
		return false;
	}

	// Regular depth, render resolution motion vectors without jitter, SDR back buffer (as the upscalers get them)
	FfxFrameInterpolationContextDescription fi = {};
	fi.flags = 0;
	fi.maxRenderSize = { p.render_w, p.render_h };
	fi.displaySize = { p.display_w, p.display_h };
	fi.backBufferFormat = ffxGetSurfaceFormatDX12(format);
	fi.previousInterpolationSourceFormat = fi.backBufferFormat;
	fi.backendInterface = s_iface;
	if ((e = ffxFrameInterpolationContextCreate(&s_fi, &fi)) != FFX_OK)
	{
		log_error("frame interpolation context", e);
		ffxOpticalflowContextDestroy(&s_of);
		free(s_scratch);
		s_scratch = nullptr;
		return false;
	}

	s_iface.fpCreateBackendContext(&s_iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &s_shared);
	FfxOpticalflowSharedResourceDescriptions ofr = {};
	ffxOpticalflowGetSharedResourceDescriptions(&s_of, &ofr);
	FfxFrameInterpolationSharedResourceDescriptions fir = {};
	ffxFrameInterpolationGetSharedResourceDescriptions(&s_fi, &fir);
	s_iface.fpCreateResource(&s_iface, &ofr.opticalFlowVector, s_shared, &s_of_vector);
	s_iface.fpCreateResource(&s_iface, &ofr.opticalFlowSCD, s_shared, &s_of_scd);
	s_iface.fpCreateResource(&s_iface, &fir.dilatedDepth, s_shared, &s_dilated_depth);
	s_iface.fpCreateResource(&s_iface, &fir.dilatedMotionVectors, s_shared, &s_dilated_mv);
	s_iface.fpCreateResource(&s_iface, &fir.reconstructedPrevNearestDepth, s_shared, &s_prev_depth);

	s_render_w = p.render_w;
	s_render_h = p.render_h;
	s_display_w = p.display_w;
	s_display_h = p.display_h;
	s_format = format;
	s_ready = true;
	Msg("* FSR frame generation: %ux%u -> %ux%u, back buffer format %d", p.render_w, p.render_h, p.display_w, p.display_h, int(format));
	return true;
}
} // namespace

bool create_swap_chain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** out)
{
	if (!ps_r4_framegen)
		return false;
	FfxSwapchain swap = nullptr;
	const FfxErrorCode e = ffxCreateFrameinterpolationSwapchainForHwndDX12(hwnd, desc, fullscreen, queue, factory, swap);
	IDXGISwapChain4* swap4 = swap ? ffxGetDX12SwapchainPtr(swap) : nullptr;
	if (e != FFX_OK || !swap4 || FAILED(swap4->QueryInterface(IID_PPV_ARGS(out))))
	{
		Msg("! FSR frame generation: the frame interpolation swap chain could not be created (0x%08x), DXGI's is used", u32(e));
		if (swap4)
			swap4->Release();
		return false;
	}
	swap4->Release(); // the engine's swap chain pointer holds it
	s_swap = swap;
	s_frame_id = 0;
	Msg("* FSR frame generation: frame interpolation swap chain");
	return true;
}

FrameParams g_pending = {};
bool g_pending_ready = false;

bool active() { return s_swap != nullptr; }

ID3DResource* hudless_target(u32 width, u32 height)
{
	dx12::Texture2D* bb = HW.pDevice->BackBufferAlias();
	if (!bb || !width || !height)
		return nullptr;
	if (s_hudless_tex && s_hudless_tex->m_desc.Width == width && s_hudless_tex->m_desc.Height == height &&
	    s_hudless_tex->m_desc.Format == bb->m_format)
		return s_hudless_tex;
	_RELEASE(s_hudless_tex);
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = width;
	d.Height = height;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = bb->m_format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &s_hudless_tex)))
		return nullptr;
	s_hudless_tex->m_debug_name = "framegen_hudless";
	return s_hudless_tex;
}

void frame(const FrameParams& p)
{
	if (!s_swap || !p.depth || !p.motion || !p.hudless)
		return;
	dx12::Texture2D* bb = HW.pDevice->BackBufferAlias();
	const DXGI_FORMAT format = bb ? bb->m_format : DXGI_FORMAT_UNKNOWN;
	if (!s_ready || p.render_w != s_render_w || p.render_h != s_render_h || p.display_w != s_display_w ||
	    p.display_h != s_display_h || format != s_format)
	{
		if (s_ready)
		{
			// Sizes changed (vid_restart, upscaler mode): nothing of the old contexts may still be in flight
			HW.pContext->Synchronize();
			ffxWaitForPresents(s_swap);
			HW.pDevice->WaitIdle();
			destroy_contexts();
		}
		if (!create_contexts(p, format))
			return;
	}

	s_had_inputs = true;
	s_configured_on = true;
	const u64 id = s_frame_id++;
	const FrameParams q = p;
	HW.pContext->ExecuteNative([q, id](dx12::Context::NativeRecorder& r) {
		const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		r.Transition(q.depth, read);
		r.Transition(q.motion, read);
		r.Transition(q.hudless, read);

		FfxFrameInterpolationPrepareDescription d = {};
		d.commandList = ffxGetCommandListDX12(r.List());
		d.renderSize = { q.render_w, q.render_h };
		d.jitterOffset = { q.jitter_x, q.jitter_y };
		d.motionVectorScale = { q.mv_scale_x, q.mv_scale_y };
		d.frameTimeDelta = q.frame_time_ms;
		d.cameraNear = q.camera_near;
		d.cameraFar = q.camera_far;
		d.viewSpaceToMetersFactor = 1.f;
		d.cameraFovAngleVertical = q.camera_fov_y;
		d.depth = ffx_resource(q.depth, L"xr depth");
		d.motionVectors = ffx_resource(q.motion, L"xr motion vectors");
		d.frameID = id;
		d.dilatedDepth = s_iface.fpGetResource(&s_iface, s_dilated_depth);
		d.dilatedMotionVectors = s_iface.fpGetResource(&s_iface, s_dilated_mv);
		d.reconstructedPrevDepth = s_iface.fpGetResource(&s_iface, s_prev_depth);
		d.cameraPosition[0] = q.camera_position.x;
		d.cameraPosition[1] = q.camera_position.y;
		d.cameraPosition[2] = q.camera_position.z;
		d.cameraUp[0] = q.camera_up.x;
		d.cameraUp[1] = q.camera_up.y;
		d.cameraUp[2] = q.camera_up.z;
		d.cameraRight[0] = q.camera_right.x;
		d.cameraRight[1] = q.camera_right.y;
		d.cameraRight[2] = q.camera_right.z;
		d.cameraForward[0] = q.camera_forward.x;
		d.cameraForward[1] = q.camera_forward.y;
		d.cameraForward[2] = q.camera_forward.z;
		const FfxErrorCode e = ffxFrameInterpolationPrepare(&s_fi, &d);
		if (e != FFX_OK)
			log_error("prepare", e);
		s_prepared[id & 1] = d;
		s_hudless = ffx_resource(q.hudless, L"xr hudless");
		configure(true, id);
	});
}

void skip_frame()
{
	// A Present without frame() before it (menus, loading, the upscaler off): nothing to interpolate from
	if (!s_swap)
		return;
	if (!s_had_inputs && s_configured_on)
	{
		s_configured_on = false;
		const u64 id = s_frame_id;
		HW.pContext->ExecuteNative([id](dx12::Context::NativeRecorder&) { configure(false, id); });
	}
	s_had_inputs = false;
}

void shutdown()
{
	if (!s_swap)
		return;
	HW.pContext->Synchronize();
	configure(false, s_frame_id);
	ffxWaitForPresents(s_swap);
	HW.pDevice->WaitIdle();
	destroy_contexts();
	_RELEASE(s_hudless_tex);
	s_swap = nullptr;
	s_configured_on = false;
	s_had_inputs = false;
	g_pending_ready = false;
}
} // namespace framegen
#endif
