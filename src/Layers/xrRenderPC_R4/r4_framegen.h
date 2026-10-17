#pragma once
// FSR 3.1 frame generation (DX12 exe, r4_framegen, vid_restart): AMD's frame interpolation swap chain presents a
// generated frame between two rendered ones. Interpolation only: the upscalers stay as they are, frame interpolation
// dilates depth and motion vectors with its own prepare pass. Needs an upscaler on (its motion vectors and depth).
#ifdef USE_DX12

extern int ps_r4_framegen;

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
// phase_upscale's inputs kept for frame() after the post-processing (set when the upscaler ran this frame)
extern FrameParams g_pending;
extern bool g_pending_ready;
// Before the swap chain goes: the GPU idle, contexts and resources released
void shutdown();
} // namespace framegen

#endif
