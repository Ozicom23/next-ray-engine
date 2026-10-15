#include "stdafx.h"
#include "r4_frame_pacing.h"
#include "../xrRender/HW.h"
#include <algorithm>

extern int ps_framelimiter; // r__framelimit (xrEngine)

int ps_r4_swap_wait = 1;     // the swap chain's frame latency waitable object, waited at the frame start (vid_restart)
int ps_r4_frame_latency = 2; // frames the CPU may run ahead of the display (vid_restart)

namespace frame_pacing
{
	struct sample
	{
		float t;        // ms since the start of the recording, at the end of Present
		float interval; // ms since the previous Present ended
		float wait;     // ms in the frame latency wait at the frame start
		float cpu;      // ms from the frame start (after the wait) to Present
		float present;  // ms inside Present
		float gpu;      // the last GPU frame time returned by the timestamp queries (a few frames old)
		float gpu_interval; // ms between the GPU's completion of that frame and of the one before it, 0 when unknown
	};
	static double s_gpu_end = 0.0;
	static u32 s_gpu_results = 0;

	static xr_vector<sample> s_samples;
	static bool s_recording = false;
	static float s_end_ms = 0.f;
	static CTimer s_timer;
	static float s_frame_begin = -1.f;
	static float s_wait = 0.f;
	static float s_last_present = -1.f;

	bool recording() { return s_recording; }

	void start(float seconds)
	{
		s_samples.clear();
		s_samples.reserve(u32(seconds * 300.f));
		s_end_ms = seconds * 1000.f;
		s_frame_begin = -1.f;
		s_last_present = -1.f;
		s_wait = 0.f;
		s_gpu_end = 0.0;
		s_gpu_results = 0;
		s_timer.Start();
		s_recording = true;
		Msg("* [Frame pacing] recording %.0f s...", seconds);
	}

	void frame_begin(float wait_ms)
	{
		if (!s_recording)
			return;
		s_frame_begin = s_timer.GetElapsed_sec() * 1000.f;
		s_wait += wait_ms; // frames that do not present (the 3D scope's view) add up to the next sample
	}

	static void finish()
	{
		s_recording = false;
		const u32 n = s_samples.size();
		if (n < 10)
		{
			Msg("! [Frame pacing] too few frames (%u)", n);
			return;
		}

		xr_vector<float> sorted;
		sorted.reserve(n);
		double sum = 0, sum_wait = 0, sum_cpu = 0, sum_present = 0, sum_gpu = 0, change = 0;
		u32 gpu_n = 0;
		for (u32 i = 0; i < n; i++)
		{
			const sample& s = s_samples[i];
			sorted.push_back(s.interval);
			sum += s.interval;
			sum_wait += s.wait;
			sum_cpu += s.cpu;
			sum_present += s.present;
			if (s.gpu > 0.f)
			{
				sum_gpu += s.gpu;
				gpu_n++;
			}
			if (i)
				change += fabs(s.interval - s_samples[i - 1].interval);
		}
		std::sort(sorted.begin(), sorted.end());
		const float mean = float(sum / n);
		const float median = sorted[n / 2];
		double var = 0;
		u32 stutters = 0, hitches = 0;
		for (u32 i = 0; i < n; i++)
		{
			const float d = s_samples[i].interval - mean;
			var += d * d;
			if (s_samples[i].interval > 2.f * median)
				stutters++;
			if (s_samples[i].interval > 50.f)
				hitches++;
		}
		// 1% low: the average of the slowest one percent of the frames
		const u32 worst = _max(1u, n / 100);
		double low = 0;
		for (u32 i = n - worst; i < n; i++)
			low += sorted[i];
		const float low1 = float(low / worst);
		const float seconds = s_samples.back().t / 1000.f;

		Msg("* [Frame pacing] %u frames in %.1f s: %.1f fps (average %.2f ms, median %.2f, 1%% low %.1f fps = %.2f ms, longest %.1f ms), frame to frame change %.2f ms, deviation %.2f ms, %u frames over twice the median, %u over 50 ms",
			n, seconds, 1000.f / mean, mean, median, 1000.f / low1, low1, sorted[n - 1], float(change / (n - 1)), float(_sqrt(var / double(n))), stutters, hitches);

		// The GPU side: the intervals between consecutive frames' completion on the GPU
		xr_vector<float> gpu_sorted;
		gpu_sorted.reserve(n);
		double gpu_sum = 0, gpu_change = 0;
		float gpu_prev = 0.f;
		for (u32 i = 0; i < n; i++)
		{
			const float iv = s_samples[i].gpu_interval;
			if (iv <= 0.f)
				continue;
			gpu_sorted.push_back(iv);
			gpu_sum += iv;
			if (gpu_prev > 0.f)
				gpu_change += fabs(iv - gpu_prev);
			gpu_prev = iv;
		}
		if (gpu_sorted.size() >= 10)
		{
			std::sort(gpu_sorted.begin(), gpu_sorted.end());
			const u32 gn = gpu_sorted.size();
			const float gmean = float(gpu_sum / gn);
			const float gmedian = gpu_sorted[gn / 2];
			const u32 gworst = _max(1u, gn / 100);
			double glow = 0, gvar = 0;
			u32 gstutters = 0;
			for (u32 i = gn - gworst; i < gn; i++)
				glow += gpu_sorted[i];
			for (u32 i = 0; i < gn; i++)
			{
				const float d = gpu_sorted[i] - gmean;
				gvar += d * d;
				if (gpu_sorted[i] > 2.f * gmedian)
					gstutters++;
			}
			Msg("*   GPU side (%u intervals): average %.2f ms, median %.2f, 1%% low %.2f ms, longest %.1f ms, frame to frame change %.2f ms, deviation %.2f ms, %u over twice the median",
				gn, gmean, gmedian, float(glow / gworst), gpu_sorted[gn - 1], float(gpu_change / (gn - 1)), float(_sqrt(gvar / double(gn))), gstutters);
		}
		if (gpu_n)
			Msg("*   per frame: latency wait %.2f ms, CPU %.2f ms, Present %.2f ms, GPU %.2f ms (%u frames timed)",
				float(sum_wait / n), float(sum_cpu / n), float(sum_present / n), float(sum_gpu / gpu_n), gpu_n);
		else
			Msg("*   per frame: latency wait %.2f ms, CPU %.2f ms, Present %.2f ms, GPU not timed",
				float(sum_wait / n), float(sum_cpu / n), float(sum_present / n));
		Msg("*   %s, max frame latency %d, vsync %s, r__framelimit %d",
			HW.m_hFrameLatency ? "waitable swap chain (r4_swap_wait)" : "frame latency limited in Present", ps_r4_frame_latency,
			psDeviceFlags.test(rsVSync) ? "on" : "off", ps_framelimiter);

		string64 stamp;
		string_path name, path;
		xr_sprintf(name, sizeof(name), "frame_pacing_%s.csv", timestamp(stamp));
		FS.update_path(path, "$logs$", name);
		IWriter* w = FS.w_open(path);
		if (!w)
		{
			Msg("! [Frame pacing] cannot write %s", path);
			return;
		}
		w->w_printf("frame,t_ms,interval_ms,wait_ms,cpu_ms,present_ms,gpu_ms,gpu_interval_ms\r\n");
		for (u32 i = 0; i < n; i++)
		{
			const sample& s = s_samples[i];
			w->w_printf("%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\r\n", i, s.t, s.interval, s.wait, s.cpu, s.present, s.gpu, s.gpu_interval);
		}
		FS.w_close(w);
		Msg("*   written to %s", path);
		xrLogger::FlushLog();
	}

	void present_done(float present_ms, float gpu_ms, double gpu_end_ms, u32 gpu_results)
	{
		if (!s_recording)
			return;
		const float now = s_timer.GetElapsed_sec() * 1000.f;
		// A new query result one after the previous one: the GPU-side interval between those two frames
		float gpu_interval = 0.f;
		if (gpu_results == s_gpu_results + 1 && s_gpu_end > 0.0 && gpu_end_ms > s_gpu_end)
			gpu_interval = float(gpu_end_ms - s_gpu_end);
		if (gpu_results != s_gpu_results)
		{
			s_gpu_results = gpu_results;
			s_gpu_end = gpu_end_ms;
		}
		if (s_frame_begin >= 0.f && s_last_present >= 0.f)
		{
			sample s;
			s.t = now;
			s.interval = now - s_last_present;
			s.wait = s_wait;
			s.cpu = _max(0.f, now - present_ms - s_frame_begin);
			s.present = present_ms;
			s.gpu = gpu_ms;
			s.gpu_interval = gpu_interval;
			s_samples.push_back(s);
		}
		s_last_present = now;
		s_frame_begin = -1.f;
		s_wait = 0.f;
		if (now >= s_end_ms)
			finish();
	}
}
