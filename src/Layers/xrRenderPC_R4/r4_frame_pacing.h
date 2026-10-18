#pragma once

// Frame pacing: when a frame starts, relative to the display queue and the GPU.
//
// Without a limit the CPU runs frames ahead and blocks inside Present with the next frame already simulated, so the
// input sampled for it is several frames old when it is shown.
//   r4_swap_wait 0/1: the swap chain's frame latency waitable object (1), waited at the frame start before input and
//     game logic instead of inside Present; windowed and borderless modes only, DXGI does not allow it on exclusive
//     fullscreen swap chains. Needs vid_restart
//   r4_frame_latency 1..3: frames the CPU may run ahead of the display (2); applied with vid_restart
//   r4_frame_jit 0/1: just-in-time frame start (0). The frame also sleeps at its start so that its first GPU work
//     reaches the GPU just as the GPU finishes the frame before it: the GPU stays busy, but a frame's commands no
//     longer wait in the queue, and the input it sampled is that much younger when it is shown. A feedback loop sets
//     the sleep from two measurements per frame: how long the frame's first commands waited before the GPU began them
//     (lengthens the sleep while above r4_frame_jit_margin) and how long the GPU sat idle before them (shortens it).
//     A GPU completion fence gives the CPU-side time of each frame's end, which ties the GPU clock to the CPU clock
//   r4_frame_jit_margin: ms of queued work kept ahead of the GPU (1.0); more absorbs CPU spikes, less cuts more lag
//   stat_frame_pacing [seconds]: each frame's interval, latency wait, sleep, CPU, Present, GPU time, GPU idle, queue
//     time, latency (frame start to GPU completion), time in the message loop before the frame with its message count and
//     the mouse events read, to logs/frame_pacing_<time>.csv, and a summary to the log (10 s)
namespace frame_pacing
{
	void start(float seconds);
	// At the start of a frame of the game loop, after the latency wait (wait_ms spent in it). game_frame: not the
	// loading screen, the just-in-time sleep may apply. Sleeps if due, then stamps the frame start (input comes after)
	void frame_begin(float wait_ms, bool game_frame);
	// dxRenderDeviceRender::Begin: the frame's first render commands (the GPU timestamp, once per presented frame)
	void render_begin();
	// Just before Present: the frame's GPU end timestamp and its completion fence
	void present_begin();
	// After Present
	void present_end();
	void destroy();
}
