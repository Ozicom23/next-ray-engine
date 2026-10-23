#include "stdafx.h"
#include "r4_frame_pacing.h"
#include "../xrRender/HW.h"
#include "../../xrEngine/Stats.h"
#include "../../xrEngine/xr_ioconsole.h"
#include <algorithm>
#include <atomic>
#include <thread>

extern int ps_framelimiter; // r__framelimit (xrEngine)

int ps_r4_swap_wait = 1;              // the swap chain's frame latency waitable object, waited at the frame start (vid_restart)
int ps_r4_frame_latency = 2;          // frames the CPU may run ahead of the display (vid_restart)
int ps_r4_frame_jit = 0;              // just-in-time frame start (off until it proves itself on more scenes)
float ps_r4_frame_jit_margin = 1.f;   // ms of queued work kept ahead of the GPU

namespace frame_pacing
{
	static double qpc_ms(u64 qpc) { return double(qpc) * 1000.0 / double(CPU::qpc_freq); }

	// The engine's stage timers (Device.Statistic, gathered while recording), per frame in the CSV
	static const u32 kStages = 9;
	static const char* kStageNames = "st_engine_ms,st_scheduler_ms,st_objects_ms,st_physics_ms,st_ai_ms,st_animation_ms,st_render_ms,st_calc_ms,st_hom_ms";

	// Frames by present id: the frame being built has id s_id; its Present signals the fence with that id
	static const u32 kRing = 32;
	struct frame_info
	{
		u64 id;
		u64 start;      // frame start after the latency wait and the sleep (CPU QPC); input and game logic follow
		u64 begin;      // the first render Begin
		u64 present0;   // Present called
		u64 present1;   // Present returned
		u64 done;       // the GPU finished the frame (fence, CPU QPC); 1 when the wait timed out
		double gpu_begin, gpu_end; // GPU timestamps (ms on the GPU clock), 0 until known
		float wait, sleep;
		float step;     // the game's time step of the frame (Device.fTimeDelta, ms)
		float msg_ms;   // the message loop before the frame (g_msg_loop_ms)
		u32 msgs;       // window messages it handled
		u32 mouse;      // mouse events DirectInput gave the frame
		float kb_ms, mouse_ms, mouse_read_ms; // the input step: keyboard read, mouse update, its DirectInput read
		float stage[kStages]; // the engine's stage timers of the frame (kStageNames), from the next frame's start
		float mt[mt_t_count]; // its secondary tasks (MT_TIMER_NAMES), from the next frame's start too
		bool jit;       // went through the just-in-time path
		u8 part;        // the recording's part when the frame started (stat_frame_pacing mark)
	};
	static frame_info s_frames[kRing];
	static u64 s_id = 1;
	static frame_info& frame(u64 id) { return s_frames[id % kRing]; }
	static frame_info& cur()
	{
		frame_info& f = frame(s_id);
		if (f.id != s_id)
		{
			f = frame_info();
			f.id = s_id;
		}
		return f;
	}

	// GPU timestamps: frame begin (first render commands) and end (before Present)
	static const u32 kQueries = 8;
	struct query
	{
		ID3DQuery* disjoint;
		ID3DQuery* begin;
		ID3DQuery* end;
		u64 id;
		bool pending;
	};
	static query s_q[kQueries] = {};
	static u32 s_q_write = 0;
	static bool s_q_active = false;
	static bool s_q_failed = false;

	// GPU completion: a fence signalled before each Present, a thread that stamps the CPU time each value completes
#ifdef USE_DX12
	// D3D12: signalled on the queue right after the frame's commands are submitted (in Present)
	static ID3D12Fence* s_fence = nullptr;
#else
	static ID3D11Fence* s_fence = nullptr;
	static ID3D11DeviceContext4* s_ctx4 = nullptr;
#endif
	static bool s_fence_failed = false;
	static HANDLE s_done_event = nullptr;
	static HANDLE s_work_sem = nullptr;
	static std::thread s_worker;
	static std::atomic<bool> s_quit{false};
	static const u32 kWork = 64;
	static u64 s_work_ids[kWork];
	static u64 s_work_tail = 0; // main thread
	static u64 s_work_head = 0; // worker thread
	static std::atomic<u64> s_done_id[kRing];
	static u64 s_done_qpc[kRing];
	static u64 s_last_signaled = 0;

	// The sleep controller
	static float s_jit_ms = 0.f;      // the current sleep at the frame start
	static u64 s_fb_id = 0;           // the last frame fed to the controller
	static float s_win_q[8], s_win_idle[8];
	static u32 s_win_n = 0;
	static double s_offsets[16];      // CPU ms minus GPU ms at frame ends (fence time minus end timestamp)
	static u32 s_offsets_n = 0, s_offsets_pos = 0;
	static float s_gaps[128];         // GPU time between a frame's end and the next one's begin (the present blit and idle)
	static u32 s_gaps_n = 0, s_gaps_pos = 0;
	static float s_floor = -1.f;      // queue time of a frame's first commands while the GPU waited for them (DXVK's
	                                  // command thread and submission): the part of the queue no sleep removes
	static HANDLE s_timer = nullptr;
	static bool s_timer_tried = false;

	// stat_frame_pacing
	struct sample
	{
		float t, interval, wait, sleep, cpu, present, gpu, gpu_interval, gpu_idle, queue, latency, step, msg_ms;
		u32 msgs, mouse;
		float kb_ms, mouse_ms, mouse_read_ms;
		float stage[kStages];
		float mt[mt_t_count];
		u8 part;
	};
	static xr_vector<sample> s_samples;
	// Named parts of a recording (stat_frame_pacing mark <name>): part 0 is before the first mark
	static const u32 kParts = 32;
	static xr_vector<shared_str> s_parts;
	static u8 s_part = 0;
	// A benchmark's result file (stat_frame_pacing result <name>): its name and the lines the benchmark adds
	static shared_str s_result_name;
	static xr_vector<xr_string> s_result_notes;
	static bool s_recording = false;
	static xr_string s_rec_settings; // " cmd=value;" for the result file, as the recording started
	static bool s_rec_open = false;   // frames still being added
	static u64 s_rec_first = 0, s_rec_last = 0;
	static u64 s_rec_start_qpc = 0;
	static float s_rec_seconds = 0.f;
	static u64 s_prev_present1 = 0;
	// The swap chain's own present count over the recording: with frame generation it also counts the generated frames
	static UINT s_rec_presents0 = 0;
	static bool s_rec_presents_ok = false;
	static u32 s_rec_presents = 0;
	static float s_rec_presents_ms = 0.f;
	static double s_prev_gpu_end = 0.0;
	static u64 s_prev_retired = 0;

	static bool wanted() { return ps_r4_frame_jit || s_recording; }

	bool recording() { return s_recording; }

	template <typename T, u32 N>
	static void ring_push(T (&v)[N], u32& n, u32& pos, T x)
	{
		v[pos] = x;
		pos = (pos + 1) % N;
		n = _min(n + 1, N);
	}
	template <typename T, u32 N>
	static T ring_min(const T (&v)[N], u32 n)
	{
		T r = v[0];
		for (u32 i = 0; i < n; i++)
			r = _min(r, v[i]);
		return r;
	}
	static float median(float* v, u32 n)
	{
		std::sort(v, v + n);
		return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
	}

	static double offset_ms() { return s_offsets_n ? ring_min(s_offsets, s_offsets_n) : 0.0; }
	static float blit_ms() { return s_gaps_n ? clampr(ring_min(s_gaps, s_gaps_n), 0.f, 1.5f) : 0.f; }

	// ------------------------------------------------------------------------------------------------------------
	// Completion fence

	static void worker()
	{
		for (;;)
		{
			WaitForSingleObject(s_work_sem, INFINITE);
			if (s_quit)
				return;
			const u64 id = s_work_ids[s_work_head % kWork];
			s_work_head++;
			s_fence->SetEventOnCompletion(id, s_done_event);
			const bool ok = WaitForSingleObject(s_done_event, 2000) == WAIT_OBJECT_0;
			if (s_quit)
				return;
			const u32 slot = u32(id % kRing);
			s_done_qpc[slot] = ok ? CPU::QPC() : 1;
			s_done_id[slot].store(id, std::memory_order_release);
		}
	}

	static bool create_fence()
	{
		if (s_fence)
			return true;
		if (s_fence_failed)
			return false;
#ifdef USE_DX12
		if (FAILED(HW.pDevice->Native()->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&s_fence)) || !s_fence)
		{
			Msg("! Frame pacing: D3D12 fence creation failed, no just-in-time frame start");
			_RELEASE(s_fence);
			s_fence_failed = true;
			return false;
		}
#else
		ID3D11Device5* device5 = nullptr;
		if (SUCCEEDED(HW.pDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&device5)) && device5)
		{
			device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void**)&s_fence);
			_RELEASE(device5);
		}
		if (s_fence)
			HW.pContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&s_ctx4);
		if (!s_fence || !s_ctx4)
		{
			Msg("! Frame pacing: D3D11 fences are not available, no just-in-time frame start");
			_RELEASE(s_fence);
			_RELEASE(s_ctx4);
			s_fence_failed = true;
			return false;
		}
#endif
		s_done_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		s_work_sem = CreateSemaphore(nullptr, 0, LONG(kWork), nullptr);
		for (auto& d : s_done_id)
			d.store(0);
		s_work_tail = s_work_head = 0;
		s_last_signaled = 0;
		s_quit = false;
		s_worker = std::thread(worker);
		return true;
	}

	static void harvest_done()
	{
		for (u32 slot = 0; slot < kRing; slot++)
		{
			const u64 id = s_done_id[slot].load(std::memory_order_acquire);
			if (!id)
				continue;
			frame_info& f = s_frames[slot];
			if (f.id == id && !f.done)
				f.done = s_done_qpc[slot];
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// GPU timestamps

	static bool create_queries()
	{
		if (s_q[0].disjoint)
			return true;
		if (s_q_failed)
			return false;
		D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
		for (query& q : s_q)
		{
			if (FAILED(HW.pDevice->CreateQuery(&disjoint, &q.disjoint)) || FAILED(HW.pDevice->CreateQuery(&stamp, &q.begin)) ||
				FAILED(HW.pDevice->CreateQuery(&stamp, &q.end)))
			{
				Msg("! Frame pacing: GPU timestamp queries are not available");
				for (query& r : s_q)
				{
					_RELEASE(r.disjoint);
					_RELEASE(r.begin);
					_RELEASE(r.end);
				}
				s_q_failed = true;
				return false;
			}
			q.pending = false;
		}
		s_q_write = 0;
		return true;
	}

	static void collect_queries()
	{
		for (u32 n = 0; n < kQueries; n++)
		{
			query& q = s_q[(s_q_write + n) % kQueries]; // oldest first
			if (!q.pending)
				continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
			if (HW.pContext->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				break;
			UINT64 t0 = 0, t1 = 0;
			if (HW.pContext->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				HW.pContext->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				break;
			q.pending = false;
			frame_info& f = frame(q.id);
			if (f.id == q.id && !dj.Disjoint && dj.Frequency && t1 >= t0)
			{
				f.gpu_begin = double(t0) * 1000.0 / double(dj.Frequency);
				f.gpu_end = double(t1) * 1000.0 / double(dj.Frequency);
			}
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// The controller

	// Feeds the frames whose GPU times and completion arrived, oldest first
	static void update_controller()
	{
		const u64 newest = s_id - 1;
		u64 id = _max(s_fb_id + 1, newest > 16 ? newest - 16 : 1);
		for (; id <= newest; id++)
		{
			const frame_info& f = frame(id);
			if (f.id != id)
				continue;
			const bool timed = f.gpu_end > 0.0;
			if (!f.done || (!timed && id + 6 > newest))
				break; // not complete yet (timestamps may still come)
			s_fb_id = id;
			if (!timed || f.done <= 1)
				continue;

			ring_push(s_offsets, s_offsets_n, s_offsets_pos, qpc_ms(f.done) - f.gpu_end);
			const frame_info& prev = frame(id - 1);
			if (prev.id != id - 1 || prev.gpu_end <= 0.0)
				continue;
			const float gap = float(f.gpu_begin - prev.gpu_end);
			ring_push(s_gaps, s_gaps_n, s_gaps_pos, gap);
			if (!f.jit || !f.begin)
				continue;

			// How long the frame's first commands waited before the GPU began them, and how long the GPU idled before
			const float queue = float(f.gpu_begin + offset_ms() - qpc_ms(f.begin));
			const float idle = gap - blit_ms();
			s_win_q[s_win_n] = queue;
			s_win_idle[s_win_n] = idle;
			if (++s_win_n < 4)
				continue;
			s_win_n = 0;
			const float mq = median(s_win_q, 4);
			const float mi = median(s_win_idle, 4);
			if (mi > 0.3f)
			{
				// Late: the GPU waited for these frames. Back off by half, and learn the queue floor from them
				s_floor = s_floor < 0.f ? mq : 0.8f * s_floor + 0.2f * mq;
				s_jit_ms = _max(0.f, 0.5f * s_jit_ms - 0.25f);
			}
			else if (s_floor < 0.f)
				s_jit_ms += 0.1f; // the floor is not known yet: probe slowly towards the first idle GPU
			else
			{
				// Early by the queue time above the floor and the margin: close a quarter of it per window
				const float slack = mq - s_floor - ps_r4_frame_jit_margin;
				if (slack > 0.f)
					s_jit_ms += clampr(0.25f * slack, 0.05f, 1.f);
			}
		}
	}

	// The sleep limit: three quarters of the recent frame interval (never a frame rate drop by itself), at most 40 ms
	static float sleep_cap()
	{
		float v[8];
		u32 n = 0;
		for (u64 id = s_id - 1; id > 1 && n < 8 && id + kRing > s_id; id--)
		{
			const frame_info& f = frame(id);
			const frame_info& p = frame(id - 1);
			if (f.id == id && p.id == id - 1 && f.present1 && p.present1)
				v[n++] = float(qpc_ms(f.present1) - qpc_ms(p.present1));
		}
		return n >= 3 ? _min(0.75f * median(v, n), 40.f) : 0.f;
	}

	static void sleep_ms(float ms)
	{
		if (!s_timer_tried)
		{
			s_timer_tried = true;
			s_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
			if (!s_timer)
				s_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		}
		const u64 target = CPU::QPC() + u64(double(ms) * double(CPU::qpc_freq) / 1000.0);
		for (;;)
		{
			const u64 now = CPU::QPC();
			if (now >= target)
				return;
			const double left = double(target - now) * 1000.0 / double(CPU::qpc_freq);
			if (left > 1.0 && s_timer)
			{
				LARGE_INTEGER due;
				due.QuadPart = -LONGLONG((left - 0.5) * 10000.0); // 100 ns units, relative
				if (SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(s_timer, DWORD(left) + 10);
				else
					Sleep(DWORD(left - 1.0));
			}
			else if (left > 2.0)
				Sleep(DWORD(left - 1.5));
			else
				SwitchToThread();
		}
	}

	// ------------------------------------------------------------------------------------------------------------
	// stat_frame_pacing

	static void retire(const frame_info& f)
	{
		if (!s_recording || f.id < s_rec_first || f.id > s_rec_last || !f.present1)
		{
			s_prev_present1 = f.present1;
			s_prev_gpu_end = f.gpu_end;
			s_prev_retired = f.id;
			return;
		}
		const bool consecutive = s_prev_retired + 1 == f.id;
		sample s = {};
		s.t = float(qpc_ms(f.present1) - qpc_ms(s_rec_start_qpc));
		s.interval = consecutive && s_prev_present1 ? float(qpc_ms(f.present1) - qpc_ms(s_prev_present1)) : 0.f;
		s.wait = f.wait;
		s.sleep = f.sleep;
		s.cpu = f.start ? float(qpc_ms(f.present0) - qpc_ms(f.start)) : 0.f;
		s.present = float(qpc_ms(f.present1) - qpc_ms(f.present0));
		if (f.gpu_end > 0.0)
		{
			s.gpu = float(f.gpu_end - f.gpu_begin);
			if (consecutive && s_prev_gpu_end > 0.0)
			{
				s.gpu_interval = float(f.gpu_end - s_prev_gpu_end);
				s.gpu_idle = _max(0.f, float(f.gpu_begin - s_prev_gpu_end) - blit_ms());
			}
			if (f.begin && s_offsets_n)
				s.queue = _max(0.f, float(f.gpu_begin + offset_ms() - qpc_ms(f.begin)));
		}
		if (f.start && f.done > 1)
			s.latency = float(qpc_ms(f.done) - qpc_ms(f.start));
		s.step = f.step;
		s.msg_ms = f.msg_ms;
		s.msgs = f.msgs;
		s.mouse = f.mouse;
		s.kb_ms = f.kb_ms;
		s.mouse_ms = f.mouse_ms;
		s.mouse_read_ms = f.mouse_read_ms;
		memcpy(s.stage, f.stage, sizeof(s.stage));
		memcpy(s.mt, f.mt, sizeof(s.mt));
		s.part = f.part;
		s_samples.push_back(s);
		s_prev_present1 = f.present1;
		s_prev_gpu_end = f.gpu_end;
		s_prev_retired = f.id;
	}

	struct stats
	{
		u32 n;
		float mean, median, low1, longest, change, deviation;
		u32 over2;
	};
	static stats compute(xr_vector<float>& v)
	{
		stats st = {};
		st.n = v.size();
		if (st.n < 10)
			return st;
		double sum = 0, change = 0;
		for (u32 i = 0; i < st.n; i++)
		{
			sum += v[i];
			if (i)
				change += fabs(v[i] - v[i - 1]);
		}
		st.mean = float(sum / st.n);
		st.change = float(change / (st.n - 1));
		xr_vector<float> sorted = v;
		std::sort(sorted.begin(), sorted.end());
		st.median = sorted[st.n / 2];
		st.longest = sorted[st.n - 1];
		const u32 worst = _max(1u, st.n / 100);
		double low = 0, var = 0;
		for (u32 i = st.n - worst; i < st.n; i++)
			low += sorted[i];
		st.low1 = float(low / worst);
		for (u32 i = 0; i < st.n; i++)
		{
			var += (v[i] - st.mean) * (v[i] - st.mean);
			if (v[i] > 2.f * st.median)
				st.over2++;
		}
		st.deviation = float(_sqrt(var / double(st.n)));
		return st;
	}

	// Each named part's frames (and the whole recording): fps, frame times, CPU and GPU per frame; to the log, and with a
	// result name to logs/bench_<time>_<name>.txt with the settings and the benchmark's notes
	static void parts_summary(const stats& all, float seconds)
	{
		if (s_parts.size() < 2 && !s_result_name.size())
			return;
		xr_vector<xr_string> lines;
		char line[512];
		auto add = [&](LPCSTR name, u32 part)
		{
			xr_vector<float> iv;
			double cpu = 0, gpu = 0;
			u32 n = 0, gpu_n = 0;
			for (const sample& s : s_samples)
			{
				if (part != u32(-1) && s.part != part)
					continue;
				n++;
				cpu += s.cpu;
				if (s.gpu > 0.f)
				{
					gpu += s.gpu;
					gpu_n++;
				}
				if (s.interval > 0.f)
					iv.push_back(s.interval);
			}
			const stats st = compute(iv);
			if (!st.n)
				return;
			xr_sprintf(line, "%-16s %6u frames  %6.1f fps  median %6.2f ms  1%% low %6.1f fps  longest %6.1f ms  stutters %4u  CPU %6.2f ms  GPU %6.2f ms",
				name, st.n, 1000.f / st.mean, st.median, 1000.f / st.low1, st.longest, st.over2, float(cpu / n),
				gpu_n ? float(gpu / gpu_n) : 0.f);
			lines.push_back(line);
		};
		for (u32 p = 0; p < s_parts.size(); ++p)
			add(s_parts[p].size() ? s_parts[p].c_str() : "(start)", p);
		add("all", u32(-1));
		Msg("* [Frame pacing] parts (stutters: frames over twice the median):");
		for (const xr_string& l : lines)
			Msg("*   %s", l.c_str());

		if (!s_result_name.size())
			return;
		string64 stamp;
		string_path name, path;
		xr_sprintf(name, sizeof(name), "bench_%s_%s.txt", timestamp(stamp), s_result_name.c_str());
		FS.update_path(path, "$logs$", name);
		IWriter* w = FS.w_open(path);
		if (!w)
		{
			Msg("! [Frame pacing] cannot write %s", path);
			return;
		}
		char exe[MAX_PATH];
		GetModuleFileNameA(nullptr, exe, sizeof(exe));
		w->w_printf("neXt-Ray benchmark: %s\r\n", s_result_name.c_str());
		w->w_printf("exe %s, built %s\r\n", strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, __DATE__);
		w->w_printf("settings:%s\r\n", s_rec_settings.c_str());
		for (const xr_string& n : s_result_notes)
			w->w_printf("%s\r\n", n.c_str());
		w->w_printf("recorded %.1f s, %u frames%s\r\n\r\n", seconds, all.n, s_rec_presents_ok ? "" : ", no present count");
		if (s_rec_presents_ok && s_rec_presents_ms > 0.f)
			w->w_printf("on the screen: %.1f fps (frame generation included)\r\n",
				float(s_rec_presents) * 1000.f / s_rec_presents_ms);
		for (const xr_string& l : lines)
			w->w_printf("%s\r\n", l.c_str());
		FS.w_close(w);
		Msg("* [Frame pacing] benchmark result: %s", path);
		s_result_name = nullptr;
		s_result_notes.clear();
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

		xr_vector<float> cpu_iv, gpu_iv, lat;
		double wait = 0, sleep = 0, cpu = 0, present = 0, gpu = 0, idle = 0, queue = 0;
		u32 gpu_n = 0, idle_n = 0, queue_n = 0;
		for (const sample& s : s_samples)
		{
			if (s.interval > 0.f)
				cpu_iv.push_back(s.interval);
			if (s.gpu_interval > 0.f)
				gpu_iv.push_back(s.gpu_interval);
			if (s.latency > 0.f)
				lat.push_back(s.latency);
			wait += s.wait;
			sleep += s.sleep;
			cpu += s.cpu;
			present += s.present;
			if (s.gpu > 0.f)
			{
				gpu += s.gpu;
				gpu_n++;
			}
			if (s.gpu_interval > 0.f)
			{
				idle += s.gpu_idle;
				idle_n++;
			}
			if (s.queue > 0.f)
			{
				queue += s.queue;
				queue_n++;
			}
		}
		const stats c = compute(cpu_iv);
		const float seconds = s_samples.back().t / 1000.f;
		if (c.n)
			Msg("* [Frame pacing] %u frames in %.1f s: %.1f fps (average %.2f ms, median %.2f, 1%% low %.1f fps = %.2f ms, longest %.1f ms), frame to frame change %.2f ms, deviation %.2f ms, %u frames over twice the median",
				n, seconds, 1000.f / c.mean, c.mean, c.median, 1000.f / c.low1, c.low1, c.longest, c.change, c.deviation, c.over2);
		if (s_rec_presents_ok && s_rec_presents_ms > 0.f)
			Msg("*   swap chain: %u presents in %.1f s, %.1f fps on the screen (frame generation included)", s_rec_presents,
				s_rec_presents_ms / 1000.f, float(s_rec_presents) * 1000.f / s_rec_presents_ms);
		const stats g = compute(gpu_iv);
		if (g.n)
			Msg("*   GPU side (%u intervals): average %.2f ms, median %.2f, 1%% low %.2f ms, longest %.1f ms, frame to frame change %.2f ms, deviation %.2f ms, %u over twice the median",
				g.n, g.mean, g.median, g.low1, g.longest, g.change, g.deviation, g.over2);
		const stats l = compute(lat);
		if (l.n)
		{
			std::sort(lat.begin(), lat.end());
			Msg("*   latency, frame start (input) to GPU done (%u frames): average %.1f ms, median %.1f, 95%% %.1f, longest %.1f",
				l.n, l.mean, l.median, lat[u32(0.95f * float(l.n - 1))], l.longest);
		}
		Msg("*   per frame: latency wait %.2f ms, sleep %.2f ms, CPU %.2f ms, Present %.2f ms, GPU %.2f ms, GPU idle %.2f ms, queued %.2f ms",
			float(wait / n), float(sleep / n), float(cpu / n), float(present / n), gpu_n ? float(gpu / gpu_n) : 0.f,
			idle_n ? float(idle / idle_n) : 0.f, queue_n ? float(queue / queue_n) : 0.f);
		{
			// The message loop between frames (Wine runs DirectInput's mouse hook there), and the frames that read
			// mouse events against the others
			double msg = 0.0, iv_mouse = 0.0, iv_still = 0.0;
			float msg_longest = 0.f;
			u64 msgs = 0;
			u32 n_mouse = 0, n_still = 0;
			for (const sample& s : s_samples)
			{
				msg += s.msg_ms;
				msg_longest = _max(msg_longest, s.msg_ms);
				msgs += s.msgs;
				if (s.interval <= 0.f)
					continue;
				if (s.mouse)
				{
					iv_mouse += s.interval;
					++n_mouse;
				}
				else
				{
					iv_still += s.interval;
					++n_still;
				}
			}
			Msg("*   message loop %.2f ms per frame (longest %.1f ms), %.1f messages per frame; frame interval with mouse events %.2f ms (%u frames), without %.2f ms (%u)",
				float(msg / n), msg_longest, float(double(msgs) / n), n_mouse ? float(iv_mouse / n_mouse) : 0.f, n_mouse,
				n_still ? float(iv_still / n_still) : 0.f, n_still);
		}
		Msg("*   %s, max frame latency %d, just-in-time start %s (margin %.1f ms, queue floor %.2f ms), vsync %s, r__framelimit %d",
			HW.m_hFrameLatency ? "waitable swap chain" : "frame latency limited in Present", ps_r4_frame_latency,
			ps_r4_frame_jit ? (s_fence ? "on" : "unavailable") : "off", ps_r4_frame_jit_margin, s_floor,
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
		w->w_printf("frame,t_ms,interval_ms,wait_ms,sleep_ms,cpu_ms,present_ms,gpu_ms,gpu_interval_ms,gpu_idle_ms,queued_ms,latency_ms,step_ms,msg_ms,msgs,mouse_events,input_kb_ms,input_mouse_ms,input_mouse_read_ms,%s," MT_TIMER_NAMES ",part\r\n", kStageNames);
		for (u32 i = 0; i < n; i++)
		{
			const sample& s = s_samples[i];
			w->w_printf("%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.3f,%.3f,%.3f", i, s.t, s.interval, s.wait, s.sleep, s.cpu,
				s.present, s.gpu, s.gpu_interval, s.gpu_idle, s.queue, s.latency, s.step, s.msg_ms, s.msgs, s.mouse, s.kb_ms, s.mouse_ms, s.mouse_read_ms);
			for (u32 k = 0; k < kStages; ++k)
				w->w_printf(",%.3f", s.stage[k]);
			for (u32 k = 0; k < mt_t_count; ++k)
				w->w_printf(",%.3f", s.mt[k]);
			w->w_printf(",%s\r\n", s.part < s_parts.size() ? s_parts[s.part].c_str() : "");
		}
		FS.w_close(w);
		Msg("*   written to %s", path);
		parts_summary(c, seconds);
		xrLogger::FlushLog();
	}

	// ------------------------------------------------------------------------------------------------------------

	void mark(LPCSTR name)
	{
		if (!s_recording)
			return;
		for (u32 i = 0; i < s_parts.size(); ++i)
			if (!xr_strcmp(s_parts[i].c_str(), name))
			{
				s_part = u8(i);
				return;
			}
		if (s_parts.size() >= kParts)
			return;
		s_parts.push_back(shared_str(name));
		s_part = u8(s_parts.size() - 1);
	}

	void stop()
	{
		if (s_rec_open)
			s_rec_seconds = 0.f; // ends at the next Present
	}

	void result(LPCSTR name)
	{
		s_result_name = name;
		s_result_notes.clear();
	}

	void note(LPCSTR text)
	{
		s_result_notes.push_back(text);
	}

	void start(float seconds)
	{
		// The settings as the recording starts: the result file takes them from here (written at the game's exit too,
		// when the console is gone)
		static const char* settings[] = { "vid_mode", "r4_upscaler", "r4_upscaler_quality", "r4_upscaler_scale", "r4_framegen",
			"r__framelimit", "rs_v_sync", "texture_lod", "r4_sun_smap_scale", "r2_ls_squality", "ssfx_ao", "ssfx_ssr",
			"ssfx_sss_quality", "ssfx_shadow_cascades", "r__detail_density", "r__detail_radius", "r__geometry_lod",
			"r4_tree_instancing", "r4_tex_stream" };
		s_rec_settings.clear();
		for (const char* cmd : settings)
			if (LPCSTR v = Console->GetString(cmd))
			{
				string256 one;
				xr_sprintf(one, " %s=%s;", cmd, v);
				s_rec_settings += one;
			}
		s_samples.clear();
		s_parts.clear();
		s_parts.push_back(shared_str(""));
		s_part = 0;
		s_samples.reserve(u32(seconds * 250.f));
		s_rec_first = s_id + 1;
		s_rec_last = u64(-1);
		s_rec_start_qpc = CPU::QPC();
		s_rec_seconds = seconds;
		s_rec_presents_ok = HW.m_pSwapChain && SUCCEEDED(HW.m_pSwapChain->GetLastPresentCount(&s_rec_presents0));
		if (!s_rec_presents_ok)
			Msg("! [Frame pacing] the swap chain's present count is not available");
		s_recording = true;
		s_rec_open = true;
		Msg("* [Frame pacing] recording %.0f s...", seconds);
	}

	void frame_begin(float wait_ms, bool game_frame)
	{
		// The frame before has ended: its stage timers are final
		if (s_recording && Device.Statistic)
		{
			frame_info& prev = frame(s_id - 1);
			const CStats* st = Device.Statistic;
			const CStatTimer* timers[kStages] = { &st->EngineTOTAL, &st->Sheduler, &st->UpdateClient, &st->Physics, &st->AI_Think,
				&st->Animation, &st->RenderTOTAL, &st->RenderCALC, &st->RenderCALC_HOM };
			if (prev.id == s_id - 1)
			{
				for (u32 i = 0; i < kStages; ++i)
					prev.stage[i] = timers[i]->last;
				for (u32 i = 0; i < mt_t_count; ++i)
					prev.mt[i] = g_mt_ms[i];
			}
		}
		frame_info& f = cur();
		if (f.start)
			return; // a frame that did not present (the 3D scope's view): one start per presented frame
		f.wait = wait_ms;
		if (wanted())
		{
			harvest_done();
			collect_queries();
		}
		const bool jit = ps_r4_frame_jit && game_frame && s_fence;
		if (wanted())
			update_controller(); // the clock offset and the GPU gaps, for the log too; the sleep moves on jit frames only
		if (jit)
		{
			const float ms = _min(s_jit_ms, sleep_cap());
			if (ms > 0.2f)
			{
				PROF_EVENT("Frame start sleep");
				sleep_ms(ms);
				f.sleep = ms;
			}
		}
		f.jit = jit;
		f.part = s_part;
		f.start = CPU::QPC();
		f.msg_ms = g_msg_loop_ms;
		f.msgs = g_msg_loop_count;
	}

	void render_begin()
	{
		frame_info& f = cur();
		if (f.begin)
			return;
		f.begin = CPU::QPC();
		f.step = Device.fTimeDelta * 1000.f;
		f.mouse = g_mouse_events; // input was read since the frame start
		f.kb_ms = g_input_kb_ms;
		f.mouse_ms = g_input_mouse_ms;
		f.mouse_read_ms = g_input_mouse_read_ms;
		s_q_active = false;
		if (!wanted() || !create_queries())
			return;
		query& q = s_q[s_q_write];
		if (q.pending)
			collect_queries();
		if (q.pending)
			return; // results not back yet, this frame goes untimed
		HW.pContext->Begin(q.disjoint);
		HW.pContext->End(q.begin);
		q.id = s_id;
		s_q_active = true;
	}

	void present_begin()
	{
		frame_info& f = cur();
		f.present0 = CPU::QPC();
		if (s_q_active)
		{
			query& q = s_q[s_q_write];
			HW.pContext->End(q.end);
			HW.pContext->End(q.disjoint);
			q.pending = true;
			s_q_write = (s_q_write + 1) % kQueries;
			s_q_active = false;
		}
		if (wanted() && create_fence() && s_work_tail - s_work_head < kWork - 1)
		{
#ifdef USE_DX12
			HW.pContext->SignalAfterSubmit(s_fence, s_id);
#else
			s_ctx4->Signal(s_fence, s_id);
#endif
			s_work_ids[s_work_tail % kWork] = s_id;
			s_work_tail++;
			s_last_signaled = s_id;
			ReleaseSemaphore(s_work_sem, 1, nullptr);
		}
	}

	void present_end()
	{
		frame_info& f = cur();
		f.present1 = CPU::QPC();

		if (s_rec_open && qpc_ms(f.present1) - qpc_ms(s_rec_start_qpc) >= s_rec_seconds * 1000.f)
		{
			s_rec_last = s_id;
			s_rec_open = false;
			UINT presents = 0;
			if (s_rec_presents_ok && HW.m_pSwapChain && SUCCEEDED(HW.m_pSwapChain->GetLastPresentCount(&presents)))
			{
				s_rec_presents = presents - s_rec_presents0;
				s_rec_presents_ms = float(qpc_ms(f.present1) - qpc_ms(s_rec_start_qpc));
			}
			else
				s_rec_presents_ok = false;
		}

		// The next frame's slot: the frame it held retires (all of its data arrived long ago)
		s_id++;
		frame_info& next = frame(s_id);
		if (next.id)
			retire(next);
		next = frame_info();
		next.id = s_id;

		if (s_recording && !s_rec_open && s_id > s_rec_last + kRing)
			finish();
	}

	void destroy()
	{
		if (s_worker.joinable())
		{
			// Every value signalled so far completes first, so no completion callback fires after the event is closed
#ifdef USE_DX12
			// A value queued for a submission that never came is never signalled: wait a bounded time
			if (s_fence && s_last_signaled && s_fence->GetCompletedValue() < s_last_signaled)
			{
				HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (e && SUCCEEDED(s_fence->SetEventOnCompletion(s_last_signaled, e)))
					WaitForSingleObject(e, 2000);
				if (e)
					CloseHandle(e);
			}
#else
			if (s_fence && s_last_signaled)
				s_fence->SetEventOnCompletion(s_last_signaled, nullptr);
#endif
			s_quit = true;
			ReleaseSemaphore(s_work_sem, 1, nullptr);
			SetEvent(s_done_event);
			s_worker.join();
		}
		for (query& q : s_q)
		{
			_RELEASE(q.disjoint);
			_RELEASE(q.begin);
			_RELEASE(q.end);
			q.pending = false;
		}
		s_q_active = false;
		_RELEASE(s_fence);
#ifndef USE_DX12
		_RELEASE(s_ctx4);
#endif
		if (s_done_event)
			CloseHandle(s_done_event);
		if (s_work_sem)
			CloseHandle(s_work_sem);
		if (s_timer)
			CloseHandle(s_timer);
		s_done_event = s_work_sem = s_timer = nullptr;
		s_timer_tried = false;
		s_fence_failed = false;
		s_q_failed = false;
		// A recording still on (the game quits, vid_restart): written with the frames it has so far
		if (s_recording)
			finish();
		for (frame_info& f : s_frames)
			f = frame_info();
		frame(s_id).id = s_id;
		s_fb_id = s_id - 1;
		s_jit_ms = 0.f;
		s_win_n = 0;
		s_offsets_n = s_offsets_pos = 0;
		s_gaps_n = s_gaps_pos = 0;
		s_floor = -1.f;
		s_recording = s_rec_open = false;
	}
}
