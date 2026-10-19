// Intel XeSS frame generation for the DX12 exe (see r4_xefg.h). The tagging, the frame generation switch and the
// present id are recorded through ExecuteNative, so they run in order with the frame's commands and before its Present
// (on the context's recording thread when threaded). XeLL's sleep and its simulation/render markers come from the main
// thread, its present markers from where Present runs; all carry the same frame id as the present id.
#include "stdafx.h"

#ifdef USE_DX12
#include "r4_xefg.h"

int ps_r4_xefg_ui = 1; // 1: the UI composited over generated frames from the HUD-less image, 0: interpolated with it

#ifdef XR_XESS_SDK
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell_d3d12.h>

#pragma comment(lib, "libxess_fg.lib")
#pragma comment(lib, "libxell.lib")

namespace xefg
{
namespace
{
xefg_swapchain_handle_t s_fg = nullptr;
xell_context_handle_t s_ll = nullptr;
bool s_swap = false;    // the proxy swap chain exists (the engine holds it)
u32 s_frame_id = 1;     // main thread: the frame being simulated and rendered
bool s_had_inputs = false;
bool s_enabled = false; // frame generation switched on (as recorded)
// XeLL wants each frame id once, in order: sleep, then the markers. Frames presented outside the frame loop (the
// loading screen draws from the loading steps) get their sleep and simulation/render markers at their Present
u32 s_slept = 0;     // the id XeLL last slept for
bool s_rendering = false; // render submit started for s_frame_id
std::atomic<u32> s_logged{ 0 };

void log_error(const char* what, int r)
{
	if (s_logged++ < 16)
		Msg("! XeSS frame generation: %s failed (%d)", what, r);
}

void fg_log(const char* message, xefg_swapchain_logging_level_t level, void*)
{
	if (s_logged < 64 || level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR)
	{
		s_logged++;
		Msg("%s[XeSS-FG] %s", level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING ? "! " : "", message ? message : "");
	}
}

void ll_log(const char* message, xell_logging_level_t level)
{
	Msg("%s[XeLL] %s", level >= XELL_LOGGING_LEVEL_WARNING ? "! " : "", message ? message : "");
}

void marker(u32 id, xell_latency_marker_type_t m)
{
	if (s_ll)
		xellAddMarkerData(s_ll, id, m);
}

void destroy_contexts()
{
	if (s_fg)
	{
		const xefg_swapchain_result_t r = xefgSwapChainDestroy(s_fg);
		if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
			log_error("destroy", r);
		s_fg = nullptr;
	}
	if (s_ll)
	{
		xellDestroyContext(s_ll);
		s_ll = nullptr;
	}
}
} // namespace

bool create_swap_chain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** out)
{
	// Delay-loaded: missing DLLs must not reach the first call
	if (!LoadLibraryW(L"libxess_fg.dll") || !LoadLibraryW(L"libxell.dll"))
	{
		Msg("! XeSS frame generation: libxess_fg.dll or libxell.dll not found next to the exe");
		return false;
	}

	ID3D12Device* device = HW.pDevice->Native();
	xell_result_t lr = xellD3D12CreateContext(device, &s_ll);
	if (lr != XELL_RESULT_SUCCESS || !s_ll)
	{
		Msg("! XeSS frame generation: XeLL not supported (%d)", int(lr));
		s_ll = nullptr;
		return false;
	}
	xellSetLoggingCallback(s_ll, XELL_LOGGING_LEVEL_WARNING, ll_log);
	xell_sleep_params_t sleep = {};
	sleep.bLowLatencyMode = 1;
	xellSetSleepMode(s_ll, &sleep);

	xefg_swapchain_result_t r = xefgSwapChainD3D12CreateContext(device, &s_fg);
	if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !s_fg)
	{
		Msg("! XeSS frame generation: not supported on this device (%d, needs Shader Model 6.4)", int(r));
		s_fg = nullptr;
		destroy_contexts();
		return false;
	}
	xefgSwapChainSetLoggingCallback(s_fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, fg_log, nullptr);
	xefgSwapChainSetLatencyReduction(s_fg, s_ll);

	IDXGIFactory2* factory2 = nullptr;
	factory->QueryInterface(IID_PPV_ARGS(&factory2));
	// Render resolution motion vectors in pixels (scaled per frame) without jitter, regular depth, SDR back buffer
	xefg_swapchain_d3d12_init_params_t init = {};
	init.maxInterpolatedFrames = 1;
	init.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
	r = xefgSwapChainD3D12InitFromSwapChainDesc(s_fg, hwnd, desc, fullscreen, queue, factory2, &init);
	_RELEASE(factory2);
	IDXGISwapChain1* swap = nullptr;
	if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS)
		r = xefgSwapChainD3D12GetSwapChainPtr(s_fg, IID_PPV_ARGS(&swap));
	if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !swap)
	{
		Msg("! XeSS frame generation: the proxy swap chain could not be created (%d), DXGI's is used", int(r));
		_RELEASE(swap);
		destroy_contexts();
		return false;
	}
	*out = swap; // the engine's swap chain pointer holds it
	xefgSwapChainSetUiCompositionState(s_fg, ps_r4_xefg_ui ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED :
	                                                         XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED);
	s_swap = true;
	s_enabled = false;
	s_had_inputs = false;

	xefg_swapchain_version_t ver = {};
	xefgSwapChainGetVersion(&ver);
	Msg("* XeSS frame generation %u.%u.%u: proxy swap chain, UI %s", ver.major, ver.minor, ver.patch,
	    ps_r4_xefg_ui ? "composited" : "interpolated");
	return true;
}

bool active() { return s_swap; }

void frame_begin()
{
	if (!s_ll || s_slept == s_frame_id)
		return;
	s_slept = s_frame_id;
	xellSleep(s_ll, s_frame_id);
	marker(s_frame_id, XELL_SIMULATION_START);
}

void render_begin()
{
	if (!s_ll || s_slept != s_frame_id || s_rendering)
		return;
	s_rendering = true;
	marker(s_frame_id, XELL_SIMULATION_END);
	marker(s_frame_id, XELL_RENDERSUBMIT_START);
}

void frame(const framegen::FrameParams& p)
{
	if (!s_swap || !p.depth || !p.motion || !p.hudless)
		return;
	s_had_inputs = true;

	xefg_swapchain_frame_constant_data_t c = {};
	// Row-major, unjittered (the jitter goes into the shaders' matrices only); the motion vectors carry no jitter
	CopyMemory(c.viewMatrix, &p.view, sizeof(c.viewMatrix));
	CopyMemory(c.projectionMatrix, &p.project, sizeof(c.projectionMatrix));
	c.motionVectorScaleX = p.mv_scale_x;
	c.motionVectorScaleY = p.mv_scale_y;
	c.resetHistory = p.reset ? 1 : 0;
	c.frameRenderTime = p.frame_time_ms;

	const u32 id = s_frame_id;
	const framegen::FrameParams q = p;
	HW.pContext->ExecuteNative([q, c, id](dx12::Context::NativeRecorder& r) {
		// Copied by XeSS-FG on this command list now: the engine may change them before the Present
		r.Transition(q.depth, D3D12_RESOURCE_STATE_COPY_SOURCE);
		r.Transition(q.motion, D3D12_RESOURCE_STATE_COPY_SOURCE);
		r.Transition(q.hudless, D3D12_RESOURCE_STATE_COPY_SOURCE);
		const struct
		{
			ID3DResource* res;
			xefg_swapchain_resource_type_t type;
			u32 w, h;
		} tags[] = {
			{ q.depth, XEFG_SWAPCHAIN_RES_DEPTH, q.render_w, q.render_h },
			{ q.motion, XEFG_SWAPCHAIN_RES_MOTION_VECTOR, q.render_w, q.render_h },
			{ q.hudless, XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, q.display_w, q.display_h },
		};
		for (const auto& t : tags)
		{
			xefg_swapchain_d3d12_resource_data_t d = {};
			d.type = t.type;
			d.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
			d.resourceBase = { 0, 0 };
			d.resourceSize = { t.w, t.h };
			d.pResource = t.res->Native();
			d.incomingState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			const xefg_swapchain_result_t e = xefgSwapChainD3D12TagFrameResource(s_fg, r.List(), id, &d);
			if (e < XEFG_SWAPCHAIN_RESULT_SUCCESS)
				log_error("tagging", e);
		}
		const xefg_swapchain_result_t e = xefgSwapChainTagFrameConstants(s_fg, id, &c);
		if (e < XEFG_SWAPCHAIN_RESULT_SUCCESS)
			log_error("frame constants", e);
	});
}

void before_present()
{
	if (!s_swap)
		return;
	if (s_slept != s_frame_id)
	{
		frame_begin();
		render_begin();
	}
	else if (!s_rendering)
		render_begin();
	marker(s_frame_id, XELL_RENDERSUBMIT_END);
	// Frames without inputs (menus, loading, the upscaler off) present as they are
	const bool on = s_had_inputs;
	s_had_inputs = false;
	const bool change = on != s_enabled;
	s_enabled = on;
	const u32 id = s_frame_id;
	HW.pContext->ExecuteNative([on, change, id](dx12::Context::NativeRecorder&) {
		if (change)
			xefgSwapChainSetEnabled(s_fg, on ? 1 : 0);
		xefgSwapChainSetPresentId(s_fg, id);
		marker(id, XELL_PRESENT_START);
	});
}

void after_present()
{
	if (!s_swap)
		return;
	const u32 id = s_frame_id++;
	s_rendering = false;
	// Recorded after the Present, so it runs once the swap chain has presented
	HW.pContext->ExecuteNative([id](dx12::Context::NativeRecorder&) { marker(id, XELL_PRESENT_END); });
}

void shutdown()
{
	if (!s_swap)
		return;
	HW.pContext->Synchronize();
	if (s_enabled)
		xefgSwapChainSetEnabled(s_fg, 0);
	HW.pDevice->WaitIdle();
	s_enabled = false;
	s_had_inputs = false;
}

void release()
{
	if (!s_swap)
		return;
	// The engine's swap chain pointer is gone: the context (and the native swap chain), then XeLL
	destroy_contexts();
	s_swap = false;
}
} // namespace xefg

#else // XR_XESS_SDK

namespace xefg
{
bool create_swap_chain(IDXGIFactory*, ID3D12CommandQueue*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**)
{
	Msg("! XeSS frame generation: this build was compiled without the Intel XeSS SDK");
	return false;
}
bool active() { return false; }
void frame_begin() {}
void render_begin() {}
void frame(const framegen::FrameParams&) {}
void before_present() {}
void after_present() {}
void shutdown() {}
void release() {}
} // namespace xefg

#endif // XR_XESS_SDK
#endif // USE_DX12
