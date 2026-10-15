#pragma once

// Frame pacing: the swap chain's frame latency waitable object and a per-frame timing log.
//
// Without it the CPU runs up to three frames ahead and blocks inside Present with the next frame already simulated,
// so the input sampled for it is up to three frames old by the time it is shown. With the waitable object the frame
// (input, game logic, render) starts only when the display queue has room, and the CPU runs r4_frame_latency frames
// ahead. The same throttle, moved from the end of the frame to its start.
//   r4_swap_wait 0/1: the waitable swap chain (1); needs vid_restart; windowed and borderless modes only, DXGI does
//     not allow it on exclusive fullscreen swap chains (the frame latency limit then applies in Present)
//   r4_frame_latency 1..3: frames the CPU may run ahead of the display (2); 1 serializes the CPU and the GPU work
//     and costs frame rate when their sum is above the frame time; applied with vid_restart
//   stat_frame_pacing [seconds]: each frame's interval, latency wait, CPU, Present and GPU time to
//     logs/frame_pacing_<time>.csv and a summary (average, median, 1% low, stutters) to the log (10 s)
namespace frame_pacing
{
	void start(float seconds);
	bool recording();
	// At the start of a frame that follows a Present, after the latency wait (the ms spent in it)
	void frame_begin(float wait_ms);
	// After Present: its duration and the last GPU frame time the overlay's queries returned (0 when none)
	void present_done(float present_ms, float gpu_ms);
}
