#include "stdafx.h"
#include "remote_console.h"
#include "xr_ioconsole.h"
#include "igame_persistent.h"
#include "IGame_Level.h"

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

	void update()
	{
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
