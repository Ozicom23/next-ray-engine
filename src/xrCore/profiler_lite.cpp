#include "stdafx.h"
#include "profiler_lite.h"
#include <algorithm>
#include <intrin.h>

namespace xr_prof
{
	std::atomic<bool> g_active{ false };

	namespace
	{
		// The CPU time stamp counter: QueryPerformanceCounter costs far more per call under Wine, and with
		// ~100k scopes a frame that made the measured frame twice as long
		IC u64 ticks() { return __rdtsc(); }

		std::atomic<desc*> s_head{ nullptr };
		thread_local scope* t_current = nullptr;
		thread_local bool t_main = false;

		float s_pending_seconds = 0.f;
		u64 s_start = 0;
		u64 s_end = 0;
		u32 s_frames = 0;

		struct sum
		{
			xr_string name;
			u64 main_total = 0, main_self = 0, other_total = 0;
			u64 main_calls = 0, other_calls = 0;
		};

		void reset()
		{
			for (desc* d = s_head.load(); d; d = d->next)
			{
				d->main_total = 0;
				d->main_self = 0;
				d->other_total = 0;
				d->main_calls = 0;
				d->other_calls = 0;
			}
		}

		void report(u64 elapsed)
		{
			// Scopes with the same name (several places, inlined copies) are summed
			xr_map<xr_string, sum> by_name;
			for (desc* d = s_head.load(); d; d = d->next)
			{
				if (!d->main_calls && !d->other_calls)
					continue;
				sum& s = by_name[d->name];
				s.name = d->name;
				s.main_total += d->main_total;
				s.main_self += d->main_self;
				s.other_total += d->other_total;
				s.main_calls += d->main_calls;
				s.other_calls += d->other_calls;
			}
			xr_vector<sum> sums;
			for (auto& it : by_name)
				sums.push_back(it.second);

			const double ms = 1000.0 / double(CPU::clk_per_second) / double(s_frames);
			const double per_frame = 1.0 / double(s_frames);
			Msg("* [CPU passes] average per frame over %.1f s (%u frames), frame %.2f ms",
				double(elapsed) / double(CPU::clk_per_second), s_frames, double(elapsed) * ms);

			std::sort(sums.begin(), sums.end(), [](const sum& a, const sum& b) { return a.main_self > b.main_self; });
			Msg("*   main thread                                   self ms  incl ms  calls");
			u32 shown = 0;
			for (const sum& s : sums)
			{
				if (!s.main_calls || shown >= 50 || double(s.main_total) * ms < 0.05)
					continue;
				Msg("*     %-42s %8.3f %8.3f %6.1f", s.name.c_str(), double(s.main_self) * ms, double(s.main_total) * ms,
					double(s.main_calls) * per_frame);
				shown++;
			}

			std::sort(sums.begin(), sums.end(), [](const sum& a, const sum& b) { return a.main_total > b.main_total; });
			Msg("*   main thread by inclusive time                 incl ms  calls");
			shown = 0;
			for (const sum& s : sums)
			{
				if (!s.main_calls || shown >= 40 || double(s.main_total) * ms < 0.1)
					continue;
				Msg("*     %-42s %8.3f %6.1f", s.name.c_str(), double(s.main_total) * ms, double(s.main_calls) * per_frame);
				shown++;
			}

			std::sort(sums.begin(), sums.end(), [](const sum& a, const sum& b) { return a.other_total > b.other_total; });
			Msg("*   other threads                                 incl ms  calls");
			shown = 0;
			for (const sum& s : sums)
			{
				if (!s.other_calls || shown >= 25 || double(s.other_total) * ms < 0.05)
					continue;
				Msg("*     %-42s %8.3f %6.1f", s.name.c_str(), double(s.other_total) * ms, double(s.other_calls) * per_frame);
				shown++;
			}
			xrLogger::FlushLog();
		}
	}

	desc::desc(const char* function, const char* event_name) : name(event_name && *event_name ? event_name : function)
	{
		next = s_head.load();
		while (!s_head.compare_exchange_weak(next, this))
		{
		}
	}

	void scope::begin(desc& ds)
	{
		d = &ds;
		child = 0;
		parent = t_current;
		t_current = this;
		t0 = ticks();
	}

	void scope::end()
	{
		const u64 dt = ticks() - t0;
		t_current = parent;
		if (parent)
			parent->child += dt;
		if (t_main)
		{
			d->main_total.fetch_add(dt, std::memory_order_relaxed);
			d->main_self.fetch_add(dt > child ? dt - child : 0, std::memory_order_relaxed);
			d->main_calls.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			d->other_total.fetch_add(dt, std::memory_order_relaxed);
			d->other_calls.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void frame()
	{
		t_main = true;
		const u64 now = ticks();

		if (g_active.load(std::memory_order_relaxed))
		{
			s_frames++;
			if (now >= s_end)
			{
				g_active = false;
				report(now - s_start);
			}
			return;
		}

		if (s_pending_seconds > 0.f)
		{
			reset();
			s_start = now;
			s_end = now + u64(double(s_pending_seconds) * double(CPU::clk_per_second));
			s_frames = 0;
			s_pending_seconds = 0.f;
			g_active = true;
		}
	}

	void start_capture(float seconds)
	{
		Msg("* [CPU passes] measuring for %.0f s, keep the view still...", seconds);
		s_pending_seconds = seconds;
	}
}
