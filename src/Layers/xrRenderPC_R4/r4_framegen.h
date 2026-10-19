#pragma once
// FSR 3.1 frame generation (DX12 exe, r4_framegen, vid_restart): AMD's frame interpolation swap chain presents a
// generated frame between two rendered ones. Interpolation only: the upscalers stay as they are, frame interpolation
// dilates depth and motion vectors with its own prepare pass. Needs an upscaler on (its motion vectors and depth).
#ifdef USE_DX12

extern u32 ps_r4_framegen; // 0 off, 1 AMD FSR, 2 Intel XeSS (r4_xefg.h)

namespace framegen
{
struct FrameParams
{
	ID3DResource* depth; // render resolution, regular depth
	ID3DResource* motion; // render resolution
	ID3DResource* hudless; // display resolution copy of the frame before the UI, back buffer format
	float jitter_x, jitter_y;
	float mv_scale_x, mv_scale_y;
	u32 render_w, render_h;
	u32 display_w, display_h;
	float frame_time_ms;
	float camera_near, camera_far, camera_fov_y;
	bool reset;
	Fvector camera_position, camera_up, camera_right, camera_forward;
	Fmatrix view, project; // unjittered (XeSS)
};

// The swap chain for CHW: AMD's frame interpolation swap chain when r4_framegen is on (false: create DXGI's)
bool create_swap_chain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** out);
bool active();
// After the frame's 3D image and post-processing, before the UI: prepares this frame's interpolation inputs and
// configures the swap chain for its Present
void frame(const FrameParams& p);
// The copy of the frame before the UI (display size, back buffer format), made when needed
ID3DResource* hudless_target(u32 width, u32 height);
// Before each Present: frame generation off for the frames that had no frame() (menus, loading, upscaler off)
void skip_frame();
// After each Present (XeSS: its latency markers)
void after_present();
// XeSS's latency reduction (XeLL) in place of the swap chain's latency wait: true when it runs, then the frame start
// (before input) and the rendering start are marked
bool latency_managed();
void frame_begin();
void render_begin();
// After the engine released the swap chain (XeSS destroys its context and the native swap chain then)
void release();
// phase_upscale's inputs kept for frame() after the post-processing (set when the upscaler ran this frame)
extern FrameParams g_pending;
extern bool g_pending_ready;
// Before the swap chain goes: the GPU idle, contexts and resources released
void shutdown();
} // namespace framegen

#endif
