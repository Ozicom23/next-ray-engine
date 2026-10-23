#include "stdafx.h"
#include "lua_sampler.h"
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrCore/crash_report.h"
#include "ai_space.h"
#include "script_engine.h"
#include <luabind/detail/object_rep.hpp>
#include <atomic>
#include <thread>
#include <algorithm>

namespace lua_sampler
{
	namespace
	{
		const u32 kDepth = 24;		 // Lua frames kept per sample
		const u32 kStackFrames = 10; // innermost frames in a stack's key
		const u32 kRing = 4096;
		const u32 kNative = 40;			  // C++ frames kept per sample
		const u32 kStackCopy = 64 * 1024; // of the sampled thread's stack, unwound after it runs again
		const float kLongMs[kinds] = {30.f, 15.f, 20.f}; // a long update
		LPCSTR const kTitles[kinds] = {"A-Life updates", "NPC updates", "The actor's updates"};

		double qpc_ms(u64 d) { return double(d) * 1000.0 / double(CPU::qpc_freq); }

		struct group
		{
			u32 objects = 0, most_objects = 0;
			u32 updates = 0;
			double update_ms = 0, sampled_ms = 0;
			float longest = 0;
			xr_map<xr_string, double> sections, self, total, lines, stacks, native_self, native_total;
		};

		// An updating thread's side, one per kind (the sampler reads it while that thread is suspended, so whole)
		struct region
		{
			std::atomic<bool> in_update{false};
			std::atomic<u32> serial{0};
			std::atomic<u32> done_serial{0};
			u64 start = 0;
			DWORD thread_id = 0;
			u64 stack_base = 0; // the updating thread's
			char section[64] = {};
			u32 objects = 0;
			float update_ms[kRing] = {}; // by serial % kRing
			group all, long_ones;
			// The sampler's
			DWORD th_id = 0;
			HANDLE th = nullptr;
			u32 last_serial = 0;
			u64 last_qpc = 0;
		};
		region s_regions[kinds];

		std::atomic<bool> s_active{false};
		std::atomic<bool> s_stop{false};
		u64 s_end_qpc = 0;
		HANDLE s_wake = nullptr;
		std::thread s_thread;

		struct sample
		{
			u32 serial;
			float ms; // since the sample before in that update (or its start)
			char section[64];
			int n;
			lua_XRFrame frames[kDepth];
			u32 native_n;
			u64 native[kNative];
		};

		// The sampled thread's stack as it was, unwound with the modules' unwind data once it runs again (looking unwind
		// data up may take the loader's lock, which the suspended thread may hold)
		u8 s_stack_copy[kStackCopy];

		u32 copy_stack(u64 rsp, u64 base)
		{
			if (base <= rsp)
				return 0;
			const u32 n = u32(_min(u64(kStackCopy), base - rsp));
			__try
			{
				memcpy(s_stack_copy, reinterpret_cast<const void*>(rsp), n);
				return n;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
		}

		u32 unwind(const CONTEXT& from, u32 copied, u64* out)
		{
			CONTEXT ctx = from;
			const u64 lo = ctx.Rsp, hi = ctx.Rsp + copied;
			const u64 copy_lo = u64(s_stack_copy), copy_hi = copy_lo + copied;
			// Registers pointing into the stack point into the copy
			DWORD64* const regs[] = {&ctx.Rsp, &ctx.Rbp, &ctx.Rbx, &ctx.Rsi, &ctx.Rdi, &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15};
			u32 n = 0;
			__try
			{
				for (DWORD64* r : regs)
					if (*r >= lo && *r < hi)
						*r = *r - lo + copy_lo;
				while (n < kNative && ctx.Rip && ctx.Rsp >= copy_lo && ctx.Rsp + 8 <= copy_hi)
				{
					out[n++] = ctx.Rip;
					DWORD64 image = 0;
					PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
					if (!fn)
					{
						// A leaf function: the return address is on top of the stack
						ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
						ctx.Rsp += 8;
						continue;
					}
					void* handler_data = nullptr;
					DWORD64 establisher = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
					for (DWORD64* r : regs)
						if (*r >= lo && *r < hi)
							*r = *r - lo + copy_lo;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			return n;
		}

		// A C++ function's name, looked up once per address
		const xr_string& native_name(u64 address)
		{
			static xr_unordered_map<u64, xr_string> names;
			auto it = names.find(address);
			if (it != names.end())
				return it->second;
			string512 name;
			crash_report::function_name(address, name, sizeof(name));
			return names.emplace(address, name).first->second;
		}

		xr_string frame_key(const lua_XRFrame& f)
		{
			if (f.currentline == -2)
			{
				string256 b;
				xr_sprintf(b, "%s:%d (compiled)", f.source, f.linedefined);
				return b;
			}
			if (f.linedefined < 0)
				return "[C]";
			string256 b;
			xr_sprintf(b, "%s:%d", f.source, f.linedefined);
			return b;
		}

		void add(group& g, const sample& s)
		{
			g.sampled_ms += s.ms;
			if (s.native_n)
			{
				g.native_self[native_name(s.native[0])] += s.ms;
				xr_vector<const xr_string*> seen;
				for (u32 i = 0; i < s.native_n; ++i)
				{
					const xr_string* k = &native_name(s.native[i]);
					if (std::find(seen.begin(), seen.end(), k) == seen.end())
					{
						seen.push_back(k);
						g.native_total[*k] += s.ms;
					}
				}
			}
			g.sections[s.section[0] ? s.section : "[none: A-Life's switch or between objects]"] += s.ms;
			if (s.n <= 0)
			{
				const char* k = s.n < 0 ? "[unreadable]" : "[engine, no Lua]";
				g.self[k] += s.ms;
				g.total[k] += s.ms;
				g.stacks[k] += s.ms;
				return;
			}
			// The innermost Lua function (a C frame on top: the function calling it, the C call named after it)
			int top = 0;
			while (top < s.n - 1 && s.frames[top].linedefined < 0 && s.frames[top].currentline != -2)
				++top;
			xr_string self = frame_key(s.frames[top]);
			if (top > 0)
				self += " -> " + frame_key(s.frames[0]);
			g.self[self] += s.ms;
			string256 line;
			xr_sprintf(line, "%s:%d%s%s", s.frames[top].source, s.frames[top].currentline, top > 0 ? " -> " : "",
			           top > 0 ? frame_key(s.frames[0]).c_str() : "");
			g.lines[line] += s.ms;
			xr_vector<xr_string> seen;
			xr_string stack;
			for (int i = 0; i < s.n; ++i)
			{
				const xr_string k = frame_key(s.frames[i]);
				if (std::find(seen.begin(), seen.end(), k) == seen.end())
				{
					seen.push_back(k);
					g.total[k] += s.ms;
				}
				if (u32(i) < kStackFrames)
					stack = stack.empty() ? k : k + " <- " + stack;
			}
			g.stacks[stack] += s.ms;
		}

		void flush(int k, xr_vector<sample>& pending, bool all)
		{
			region& r = s_regions[k];
			const u32 done = r.done_serial.load();
			auto it = pending.begin();
			for (; it != pending.end(); ++it)
			{
				if (!all && it->serial > done)
					break;
				add(r.all, *it);
				if (r.update_ms[it->serial % kRing] >= kLongMs[k])
					add(r.long_ones, *it);
			}
			pending.erase(pending.begin(), it);
		}

		int read_stack(lua_State* L, lua_XRFrame* out)
		{
			__try
			{
				return lua_xr_sample(L, out, kDepth);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return -1;
			}
		}

		void print(IWriter* w, LPCSTR title, const xr_map<xr_string, double>& m, double total, u32 count)
		{
			xr_vector<std::pair<double, const xr_string*>> v;
			for (const auto& it : m)
				v.emplace_back(it.second, &it.first);
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			w->w_printf("\r\n%s\r\n", title);
			for (u32 i = 0; i < v.size() && i < count; ++i)
				w->w_printf("%9.1f ms %5.1f%%  %s\r\n", v[i].first, total > 0 ? 100.0 * v[i].first / total : 0.0,
				            v[i].second->c_str());
		}

		void report(group& g, IWriter* w, LPCSTR title)
		{
			w->w_printf("\r\n==== %s: %u updates, %.0f ms (sampled %.0f ms), longest %.1f ms, objects %.1f per update (most %u)\r\n",
			            title, g.updates, g.update_ms, g.sampled_ms, g.longest, g.updates ? float(g.objects) / float(g.updates) : 0.f,
			            g.most_objects);
			print(w, "Objects (section):", g.sections, g.sampled_ms, 40);
			print(w, "Lua functions, with what they call:", g.total, g.sampled_ms, 60);
			print(w, "Lua functions, their own time (a C function they call included):", g.self, g.sampled_ms, 60);
			print(w, "Lines:", g.lines, g.sampled_ms, 40);
			print(w, "Stacks (outermost first):", g.stacks, g.sampled_ms, 40);
			print(w, "C++ functions, with what they call:", g.native_total, g.sampled_ms, 80);
			print(w, "C++ functions, their own time:", g.native_self, g.sampled_ms, 50);
		}

		void write_report()
		{
			string64 stamp;
			SYSTEMTIME t;
			GetLocalTime(&t);
			xr_sprintf(stamp, "%02u-%02u-%02u_%02u-%02u-%02u", t.wMonth, t.wDay, t.wYear % 100, t.wHour, t.wMinute, t.wSecond);
			string_path name, path;
			xr_sprintf(name, "lua_sample_%s.txt", stamp);
			FS.update_path(path, "$logs$", name);
			IWriter* w = FS.w_open("$logs$", name);
			if (!w)
			{
				Msg("! [Lua sampler] can't write %s", path);
				return;
			}
			w->w_printf("A-Life's, NPCs' and the actor's updates, sampled about once a millisecond. Lua frames as of Lua's last call out "
			            "of the VM: a pure Lua loop is counted in the function that last called the engine. A line 0: a C "
			            "function not called by a line (a metamethod: a field of an engine object). C++ functions from the "
			            "exe's .sym file (inlined ones counted in their caller).\r\n");
			for (int k = 0; k < kinds; ++k)
			{
				string128 title;
				report(s_regions[k].all, w, kTitles[k]);
				xr_sprintf(title, "%s of %.0f ms or more", kTitles[k], kLongMs[k]);
				report(s_regions[k].long_ones, w, title);
			}
			FS.w_close(w);
			Msg("* [Lua sampler] A-Life %u updates (%u long), NPCs %u (%u long), the actor %u (%u long), written to %s",
			    s_regions[alife].all.updates, s_regions[alife].long_ones.updates, s_regions[npc].all.updates,
			    s_regions[npc].long_ones.updates, s_regions[actor].all.updates, s_regions[actor].long_ones.updates, path);
		}

		// One sample of the region's update, if one runs
		void take(int k, lua_State* L, xr_vector<sample>& pending, sample& s)
		{
			region& r = s_regions[k];
			if (!r.in_update.load())
				return;
			const DWORD id = r.thread_id;
			if (id != r.th_id)
			{
				if (r.th)
					CloseHandle(r.th);
				r.th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
				r.th_id = id;
			}
			if (!r.th || SuspendThread(r.th) == DWORD(-1))
				return;
			// Nothing that may take a lock while the thread is held: no allocation, no log
			bool have = false;
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			u32 copied = 0;
			if (r.in_update.load())
			{
				const u64 now = CPU::QPC();
				s.serial = r.serial.load();
				const u64 from = s.serial == r.last_serial && r.last_qpc > r.start ? r.last_qpc : r.start;
				s.ms = float(qpc_ms(now - from));
				memcpy(s.section, r.section, sizeof(s.section));
				s.section[sizeof(s.section) - 1] = 0;
				s.n = read_stack(L, s.frames);
				if (GetThreadContext(r.th, &ctx))
					copied = copy_stack(ctx.Rsp, r.stack_base);
				r.last_serial = s.serial;
				r.last_qpc = now;
				have = true;
			}
			ResumeThread(r.th);
			if (have)
			{
				s.native_n = copied ? unwind(ctx, copied, s.native) : 0;
				pending.push_back(s);
			}
		}

		void run()
		{
			lua_State* L = ai().script_engine().lua();
			xr_vector<sample> pending[kinds];
			for (auto& p : pending)
				p.reserve(4096);
			sample s;
			while (!s_stop.load() && CPU::QPC() < s_end_qpc)
			{
				bool any = false;
				for (int k = 0; k < kinds; ++k)
					any |= s_regions[k].in_update.load();
				if (!any)
				{
					for (int k = 0; k < kinds; ++k)
						flush(k, pending[k], false);
					WaitForSingleObject(s_wake, 20);
					continue;
				}
				Sleep(1);
				for (int k = 0; k < kinds; ++k)
				{
					take(k, L, pending[k], s);
					flush(k, pending[k], false);
				}
			}
			s_active = false;
			for (int k = 0; k < kinds; ++k)
			{
				while (s_regions[k].in_update.load())
					Sleep(1);
				flush(k, pending[k], true);
				if (s_regions[k].th)
					CloseHandle(s_regions[k].th);
				s_regions[k].th = nullptr;
				s_regions[k].th_id = 0;
			}
			write_report();
		}

		void stop()
		{
			s_stop = true;
			if (s_wake)
				SetEvent(s_wake);
			if (s_thread.joinable())
				s_thread.join();
		}

		class CCC_LuaSample : public IConsole_Command
		{
		public:
			CCC_LuaSample(LPCSTR N) : IConsole_Command(N) {}
			virtual void Execute(LPCSTR args)
			{
				const float seconds = float(atof(args));
				if (s_thread.joinable())
					stop();
				if (seconds <= 0.f)
					return;
				if (!g_pGameLevel || !ai().get_alife())
				{
					Msg("! [Lua sampler] no level");
					return;
				}
				for (region& r : s_regions)
				{
					r.all = group();
					r.long_ones = group();
				}
				if (!s_wake)
					s_wake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				s_stop = false;
				s_end_qpc = CPU::QPC() + u64(double(seconds) * double(CPU::qpc_freq));
				s_active = true;
				s_thread = std::thread(run);
				Msg("* [Lua sampler] %.0f s", seconds);
			}
			virtual void Info(TInfo& I) { xr_strcpy(I, "seconds to sample A-Life's, NPCs' and the actor's updates for, a report to logs (0 ends it)"); }
		};
	}

	// stat_luabind_objects 1: count the luabind objects scripts make and the GC collects, by class; 0: the counts to
	// the log, most made first
	class CCC_LuabindObjects : public IConsole_Command
	{
	public:
		CCC_LuabindObjects(LPCSTR N) : IConsole_Command(N) {}
		virtual void Execute(LPCSTR args)
		{
			if (atoi(args))
			{
				luabind::detail::xr_count_objects(true);
				m_start = CPU::QPC();
				Msg("* [luabind] counting objects");
				return;
			}
			luabind::detail::xr_count_objects(false);
			struct row
			{
				xr_string name;
				unsigned made, collected;
			};
			xr_vector<row> rows;
			luabind::detail::xr_object_counts([](const char* name, unsigned made, unsigned collected, void* ud)
			{
				static_cast<xr_vector<row>*>(ud)->push_back({name ? name : "?", made, collected});
			}, &rows);
			std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) { return a.made > b.made; });
			const double s = m_start ? double(CPU::QPC() - m_start) / double(CPU::qpc_freq) : 0.0;
			u64 made = 0, collected = 0;
			for (const row& r : rows)
				made += r.made, collected += r.collected;
			Msg("* [luabind] %.0f s: %llu objects made, %llu collected", s, made, collected);
			for (u32 i = 0; i < rows.size() && i < 30; ++i)
				Msg("* [luabind] %9u made %9u collected  %s", rows[i].made, rows[i].collected, rows[i].name.c_str());
		}
		virtual void Info(TInfo& I) { xr_strcpy(I, "1 counts luabind objects by class, 0 logs the counts"); }
		virtual void Save(IWriter*) {}

	private:
		u64 m_start = 0;
	};

	void register_commands()
	{
		CMD1(CCC_LuaSample, "stat_lua_sample");
		CMD1(CCC_LuabindObjects, "stat_luabind_objects");
	}

	void update_begin(kind k)
	{
		if (!s_active.load(std::memory_order_relaxed))
			return;
		region& r = s_regions[k];
		r.thread_id = GetCurrentThreadId();
		r.stack_base = u64(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
		r.section[0] = 0;
		r.objects = 0;
		r.start = CPU::QPC();
		r.serial.fetch_add(1);
		r.in_update = true;
		SetEvent(s_wake);
	}

	void update_end(kind k)
	{
		region& r = s_regions[k];
		if (!r.in_update.load(std::memory_order_relaxed))
			return;
		const u32 serial = r.serial.load();
		const float ms = float(qpc_ms(CPU::QPC() - r.start));
		r.update_ms[serial % kRing] = ms;
		// The sampler only reads these after the update's samples are in (done_serial)
		for (group* g : {&r.all, &r.long_ones})
		{
			if (g == &r.long_ones && ms < kLongMs[k])
				continue;
			g->updates++;
			g->objects += r.objects;
			g->most_objects = _max(g->most_objects, r.objects);
			g->update_ms += ms;
			g->longest = _max(g->longest, ms);
		}
		r.in_update = false;
		r.done_serial = serial;
	}

	void object(kind k, LPCSTR section)
	{
		region& r = s_regions[k];
		if (!r.in_update.load(std::memory_order_relaxed))
			return;
		if (section)
		{
			strncpy_s(r.section, section, _TRUNCATE);
			r.objects++;
		}
		else
			r.section[0] = 0;
	}
}
