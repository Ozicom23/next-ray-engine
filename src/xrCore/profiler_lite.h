#pragma once
#include <atomic>

// Lightweight backend for the PROF_EVENT / START_PROFILE scopes when Optick is not built in (all normal builds).
// Off, a scope costs one flag check. stat_cpu_passes <seconds> captures: per scope CPU time, main thread
// (self and including nested scopes) and the other threads, averaged per frame and written to the log.
namespace xr_prof
{
	struct desc
	{
		const char* name;
		desc* next;
		std::atomic<unsigned long long> main_total{ 0 }, main_self{ 0 }, other_total{ 0 };
		std::atomic<unsigned> main_calls{ 0 }, other_calls{ 0 };

		desc(const char* function, const char* event_name);
	};

	extern std::atomic<bool> g_active;

	struct scope
	{
		desc* d;
		unsigned long long t0, child;
		scope* parent;

		explicit scope(desc& ds)
		{
			if (!g_active.load(std::memory_order_relaxed))
			{
				d = nullptr;
				return;
			}
			begin(ds);
		}
		~scope()
		{
			if (d)
				end();
		}
		scope(const scope&) = delete;
		scope& operator=(const scope&) = delete;

		void begin(desc& ds);
		void end();
	};

	// Main thread, once per frame
	void frame();
	void start_capture(float seconds);
}

#define PROF_LITE_CONCAT_(a, b) a##b
#define PROF_LITE_CONCAT(a, b) PROF_LITE_CONCAT_(a, b)
// PROF_EVENT() is named after the function
#define PROF_LITE_EVENT(...) \
	static ::xr_prof::desc PROF_LITE_CONCAT(prof_desc_, __LINE__)(__FUNCTION__, "" __VA_ARGS__); \
	::xr_prof::scope PROF_LITE_CONCAT(prof_scope_, __LINE__)(PROF_LITE_CONCAT(prof_desc_, __LINE__))
