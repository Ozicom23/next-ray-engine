#pragma once
// Intel XeSS frame generation (DX12 exe, r4_framegen xess, vid_restart), on the XeSS SDK: a proxy
// swap chain that presents a generated frame between two rendered ones, paced together with XeLL (Intel's latency
// reduction, required by it). Behind framegen's functions (r4_framegen.h), which call these when XeSS was chosen.
#ifdef USE_DX12

#include "r4_framegen.h"

namespace xefg
{
// The proxy swap chain for CHW; false: not available (no SDK, DLLs or support), framegen falls back to DXGI's
bool create_swap_chain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** out);
bool active();
// Frame start, before input (in place of the swap chain's latency wait): XeLL's sleep and the simulation start marker
void frame_begin();
// Rendering starts: simulation end, render submit start
void render_begin();
// This frame's inputs, tagged with the frame's id (after the post-processing, before the UI)
void frame(const framegen::FrameParams& p);
// Around the engine's Present: frame generation on for frames with inputs (off in menus and loading), the present id,
// the markers
void before_present();
void after_present();
// Before the swap chain goes (no more tagging), and after the engine released its pointer (the contexts)
void shutdown();
void release();
} // namespace xefg

#endif
