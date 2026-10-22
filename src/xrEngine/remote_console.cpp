#include "stdafx.h"
#include "remote_console.h"
#include "xr_ioconsole.h"
#include "igame_persistent.h"
#include "IGame_Level.h"
#include <atomic>
#include <thread>
#include <tlhelp32.h>
#include "../xrCore/crash_report.h"

int g_hang_watchdog = 60; // hang_watchdog: seconds without progress on the main thread before a hung game closes, 0 off

namespace remote_console
{
	namespace
	{
		bool s_initialized = false;
		bool s_enabled = false;
		string_path s_cmd_path;
		string_path s_status_path;
		string_path s_status_tmp_path;

		xr_deque<xr_string> s_queue;
		xr_string s_last;
		u32 s_executed = 0;

		u32 s_next_poll = 0;
		u32 s_next_status = 0;
		u32 s_wait_until = 0;
		bool s_wait_level = false;
		u32 s_wait_level_until = 0;

		u32 s_fps_frame = 0;
		u32 s_fps_time = 0;
		float s_fps = 0.f;

		const char* state()
		{
			if (g_loading_events.size() || (g_pGameLevel && !g_pGameLevel->bReady) || Device.dwPrecacheFrame)
				return "loading";
			if (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive())
				return "menu";
			if (g_pGameLevel)
				return "level";
			return "idle";
		}

		void read_commands()
		{
			if (GetFileAttributes(s_cmd_path) == INVALID_FILE_ATTRIBUTES)
				return;

			FILE* f = fopen(s_cmd_path, "rb");
			if (!f)
				return;
			xr_string text;
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				text.append(buf, n);
			fclose(f);
			DeleteFile(s_cmd_path);

			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == xr_string::npos)
					end = text.size();
				xr_string line = text.substr(pos, end - pos);
				pos = end + 1;

				size_t first = line.find_first_not_of(" \t\r");
				if (first == xr_string::npos)
					continue;
				size_t last = line.find_last_not_of(" \t\r");
				line = line.substr(first, last - first + 1);
				if (line[0] == '#')
					continue;
				if (line == "rc_clear")
				{
					// Right away, also while a wait is blocking the queue
					Msg("[remote] rc_clear: dropped %u commands", u32(s_queue.size()));
					s_queue.clear();
					s_wait_until = 0;
					s_wait_level = false;
					continue;
				}
				s_queue.push_back(line);
			}
		}

		// One command per frame, so each sees the result of the one before
		void execute_next(u32 now, const char* st)
		{
			if (s_queue.empty() || now < s_wait_until)
				return;
			if (s_wait_level)
			{
				if (xr_strcmp(st, "level"))
				{
					if (now < s_wait_level_until)
						return;
					Msg("! [remote] rc_wait_level: timed out, state %s", st);
				}
				s_wait_level = false;
			}
			if (!xr_strcmp(st, "loading"))
				return;

			xr_string cmd = s_queue.front();
			s_queue.pop_front();
			s_last = cmd;
			s_executed++;
			Msg("[remote] %s", cmd.c_str());

			if (cmd.compare(0, 8, "rc_wait ") == 0)
				s_wait_until = now + iFloor(float(atof(cmd.c_str() + 8)) * 1000.f);
			else if (cmd.compare(0, 13, "rc_wait_level") == 0)
			{
				float timeout = cmd.size() > 13 ? float(atof(cmd.c_str() + 13)) : 0.f;
				s_wait_level = true;
				s_wait_level_until = now + iFloor((timeout > 0.f ? timeout : 300.f) * 1000.f);
			}
			else
				Console->Execute(cmd.c_str());

			xrLogger::FlushLog();
		}

		void write_status(u32 now, const char* st)
		{
			if (now > s_fps_time)
				s_fps = float(Device.dwFrame - s_fps_frame) * 1000.f / float(now - s_fps_time);
			s_fps_frame = Device.dwFrame;
			s_fps_time = now;

			FILE* f = fopen(s_status_tmp_path, "wb");
			if (!f)
				return;
			fprintf(f, "state=%s\n", st);
			fprintf(f, "active=%d\n", Device.b_is_Active ? 1 : 0);
			fprintf(f, "paused=%d\n", Device.Paused() ? 1 : 0);
			fprintf(f, "frame=%u\n", Device.dwFrame);
			fprintf(f, "time_ms=%u\n", now);
			fprintf(f, "fps=%.1f\n", s_fps);
			fprintf(f, "queue=%u\n", u32(s_queue.size()));
			fprintf(f, "waiting=%d\n", (s_wait_level || now < s_wait_until) ? 1 : 0);
			fprintf(f, "executed=%u\n", s_executed);
			fprintf(f, "last=%s\n", s_last.c_str());
			fclose(f);
			MoveFileEx(s_status_tmp_path, s_status_path, MOVEFILE_REPLACE_EXISTING);

			xrLogger::FlushLog();
		}
	}

	namespace
	{
		std::atomic<u32> s_log_lines{ 0 };
		string_path s_hang_dir;
		DWORD s_main_thread = 0;
		std::atomic<bool> s_armed{ false }; // the main loop runs
		std::atomic<bool> s_loading{ false }; // a level loads (or its precache frames)

		void on_log(const char*)
		{
			s_log_lines.fetch_add(1, std::memory_order_relaxed);
			if (GetCurrentThreadId() == s_main_thread)
				crash_report::beat();
		}

		// Return addresses of a suspended thread, unwound with the modules' unwind data (no allocation: the thread may
		// hold the heap lock)
		u32 walk_stack(HANDLE thread, u64* frames, u32 max)
		{
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_FULL;
			if (!GetThreadContext(thread, &ctx))
				return 0;
			u32 n = 0;
			__try
			{
				while (n < max && ctx.Rip)
				{
					frames[n++] = ctx.Rip;
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
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			return n;
		}

		void dump_threads()
		{
			string64 stamp;
			SYSTEMTIME t;
			GetLocalTime(&t);
			xr_sprintf(stamp, "%02u-%02u-%02u_%02u-%02u-%02u", t.wMonth, t.wDay, t.wYear % 100, t.wHour, t.wMinute, t.wSecond);
			string_path path;
			xr_sprintf(path, "%shang_%s.txt", s_hang_dir, stamp);
			FILE* f = fopen(path, "w");
			if (!f)
				return;
			fprintf(f, "frame %u, log lines %u\n", Device.dwFrame, s_log_lines.load());
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			THREADENTRY32 te = {};
			te.dwSize = sizeof(te);
			const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
					continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!th)
					continue;
				u64 frames[48];
				u32 n = 0;
				if (SuspendThread(th) != DWORD(-1))
				{
					n = walk_stack(th, frames, 48);
					ResumeThread(th);
				}
				CloseHandle(th);
				fprintf(f, "thread %u:", te.th32ThreadID);
				for (u32 i = 0; i < n; ++i)
				{
					HMODULE mod = nullptr;
					char name[MAX_PATH] = "?";
					if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					                       reinterpret_cast<LPCSTR>(frames[i]), &mod) && mod)
					{
						GetModuleFileNameA(mod, name, sizeof(name));
						const char* base = strrchr(name, '\\');
						fprintf(f, " %s+0x%llx", base ? base + 1 : name, frames[i] - u64(mod));
					}
					else
						fprintf(f, " 0x%llx", frames[i]);
				}
				fprintf(f, "\n");
			}
			CloseHandle(snap);
			fclose(f);
		}

		// The hang: the stacks, a crash report with the main thread's stack (on a thread of its own: the main thread may
		// hold a lock the report needs, the log's, so it gets 20 s), a message, the end
		char s_hang_what[256];
		string_path s_hang_report;

		void report_hang()
		{
			HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s_main_thread);
			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_FULL;
			bool have = false;
			if (th && SuspendThread(th) != DWORD(-1))
			{
				have = !!GetThreadContext(th, &ctx);
				ResumeThread(th);
			}
			xr_strcpy(s_hang_report, crash_report::write(s_hang_what, have ? &ctx : nullptr, have ? th : nullptr));
			if (th)
				CloseHandle(th);
		}

		void on_hang(u32 seconds)
		{
			dump_threads();
			xr_sprintf(s_hang_what, "The game stopped responding: the main thread made no progress for %u s (hang_watchdog)",
			           seconds);
			s_hang_report[0] = 0;
			std::thread reporter(report_hang);
			if (WaitForSingleObject(reporter.native_handle(), 20000) == WAIT_OBJECT_0)
				reporter.join();
			else
				reporter.detach();
			char box[600];
			xr_sprintf(box, "The game stopped responding for %u seconds and will close.%s%s", seconds,
			           s_hang_report[0] ? "\n\nCrash report: " : "", s_hang_report);
			if (!strstr(Core.Params, "-remote_console")) // a test session doesn't wait for a click
				MessageBoxA(nullptr, box, "neXt-Ray", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
			TerminateProcess(GetCurrentProcess(), 3);
		}

		void watchdog()
		{
			const bool tests = strstr(Core.Params, "-remote_console") != nullptr;
			u32 last = 0;
			ULONGLONG quiet_since = GetTickCount64(), prev = quiet_since;
			bool dumped = false;
			for (;;)
			{
				Sleep(1000);
				const ULONGLONG now = GetTickCount64();
				const u32 beat = crash_report::beats();
				// A jump of the clock: the system slept, or the process was stopped
				const bool resumed = now - prev > 5000;
				prev = now;
				if (beat != last || resumed || !s_armed || IsDebuggerPresent() || crash_report::started())
				{
					last = beat;
					quiet_since = now;
					dumped = false;
					continue;
				}
				const ULONGLONG quiet = now - quiet_since;
				if (g_hang_watchdog > 0)
				{
					if (quiet >= u64(g_hang_watchdog) * 1000 * (s_loading ? 3 : 1))
						on_hang(u32(quiet / 1000));
				}
				else if (tests && !dumped && quiet >= 120000)
				{
					dump_threads();
					dumped = true;
				}
			}
		}
	}

	void start_watchdog()
	{
		s_main_thread = GetCurrentThreadId();
		FS.update_path(s_hang_dir, "$logs$", "");
		xrLogger::AddLogCallback(on_log);
		std::thread(watchdog).detach();
	}

	void update()
	{
		s_armed = true;
		s_loading = !g_loading_events.empty() || Device.dwPrecacheFrame != 0;
		if (!s_initialized)
		{
			s_initialized = true;
			s_enabled = strstr(Core.Params, "-remote_console") != nullptr;
			if (!s_enabled)
				return;
			FS.update_path(s_cmd_path, "$app_data_root$", "remote_cmd.txt");
			FS.update_path(s_status_path, "$app_data_root$", "remote_status.txt");
			FS.update_path(s_status_tmp_path, "$app_data_root$", "remote_status.tmp");
			Msg("* Remote console: %s", s_cmd_path);
		}
		if (!s_enabled || !Console)
			return;

		u32 now = GetTickCount();
		const char* st = state();

		if (now >= s_next_poll)
		{
			s_next_poll = now + 250;
			read_commands();
		}
		execute_next(now, st);

		if (now >= s_next_status)
		{
			s_next_status = now + 500;
			write_status(now, st);
		}
	}
}
