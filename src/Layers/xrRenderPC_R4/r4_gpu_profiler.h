#pragma once

// GPU time per render pass for the performance overlay's detailed mode. Every PIX_EVENT scope (dx10EventWrapper)
// gets a pair of timestamp queries, so the passes nest the way the scopes do in the code. Results arrive a few
// frames late and are averaged over the overlay's refresh interval. Only records on the thread that renders the
// frame and only while enabled.
namespace gpu_profiler
{
	struct pass
	{
		string64 name;
		u32 depth; // 0 for the outermost scopes of the frame
		float ms;  // average per frame over the last interval
	};

	// From the overlay's GpuFrameBegin / GpuFrameEnd
	void frame_begin(bool enabled);
	void frame_end();

	// From dxPixEventWrapper
	void event_begin(LPCWSTR name);
	void event_end();

	// Averages since the last call, in the order the passes first ran; clears the sums
	void collect(xr_vector<pass>& out);

	// stat_gpu_passes <seconds>: averages over a longer time, written to the log when done. Records even
	// without the detailed overlay
	void start_capture(float seconds);

	void destroy();
}
