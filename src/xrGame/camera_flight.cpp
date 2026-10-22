#include "stdafx.h"
#include "camera_flight.h"
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/CameraManager.h"
#include "../xrEngine/FDemoRecord.h"
#include "../xrEngine/FDemoPlay.h"
#include "../xrEngine/CustomHUD.h"
#include "../xrEngine/Environment.h"
#include "Level.h"
#include "Actor_Flags.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "game_sv_single.h"
#include "xrServer.h"

extern xr_unordered_set<CDemoRecord*> pDemoRecords; // console_commands.cpp

namespace camera_flight
{
	namespace
	{
		const float kKey = 0.1f;		// a key every 0.1 s, played back at the same rate
		const float kHoldSec = 5.f;		// held at the start so the view there streams in
		const u32 kWeatherMs = 15000;
		LPCSTR const kWeathers[] = {"w_clear1", "w_partly1", "w_cloudy1", "w_foggy1", "w_rain1", "w_storm1"};

		enum class phase { off, recording, holding, flying };

		struct
		{
			phase now = phase::off;
			string64 name;
			u32 hour;
			u32 weather;
			u32 weather_at;
			u32 began;
			bool seen;		// the camera manager adds effectors a frame later
			bool god;
			bool hud;
			float time_factor;
		} s;

		void file_path(string_path& fn, LPCSTR name)
		{
			string_path n;
			xr_sprintf(n, sizeof(n), "%s.xrdemo", name);
			FS.update_path(fn, "$game_saves$", n);
		}

		void set_weather(u32 i)
		{
			s.weather = i % (sizeof(kWeathers) / sizeof(kWeathers[0]));
			g_pGamePersistent->Environment().SetWeather(kWeathers[s.weather], true);
			s.weather_at = Device.dwTimeGlobal;
		}

		// The game time to the next <hour>:00
		void set_hour(u32 hour)
		{
			game_sv_Single* game = smart_cast<game_sv_Single*>(Level().Server ? Level().Server->game : nullptr);
			if (!game || !ai().get_alife())
				return;
			float now = Level().GetGameDayTimeSec();
			float add = float(hour * 3600) - now;
			if (add <= 0)
				add += 24 * 3600;
			g_pGamePersistent->Environment().ChangeGameTime(add);
			game->alife().time_manager().change_game_time(u32(add) * 1000);
		}

		void begin(bool recording)
		{
			s.god = !!psActorFlags.test(AF_GODMODE);
			psActorFlags.set(AF_GODMODE, TRUE);
			s.now = recording ? phase::recording : phase::holding;
			s.began = Device.dwTimeGlobal;
			s.seen = false;
		}

		void end()
		{
			if (s.now == phase::flying)
			{
				string256 note;
				xr_sprintf(note, sizeof(note), "flight %s, hour %u, weather every %u s: w_clear1 w_partly1 w_cloudy1 "
					"w_foggy1 w_rain1 w_storm1", s.name, s.hour, kWeatherMs / 1000);
				string512 cmd;
				xr_sprintf(cmd, sizeof(cmd), "stat_frame_pacing note %s", note);
				Console->Execute(cmd);
				Console->Execute("stat_frame_pacing stop");
			}
			if (s.now == phase::holding || s.now == phase::flying)
			{
				psHUD_Flags.set(HUD_DRAW, s.hud);
				Level().SetGameTimeFactor(s.time_factor);
			}
			psActorFlags.set(AF_GODMODE, s.god);
			Msg("* Flight: %s ended", s.name);
			s.now = phase::off;
		}
	}

	bool playing() { return s.now == phase::holding || s.now == phase::flying; }

	void frame()
	{
		if (s.now == phase::off)
			return;
		CEffectorCam* demo = g_pGameLevel ? g_pGameLevel->Cameras().GetCamEffector(cefDemo) : nullptr;
		if (!demo)
		{
			if (s.seen || Device.dwTimeGlobal - s.began > 2000)
				end();
			return;
		}
		s.seen = true;
		if (s.now == phase::holding && !static_cast<CDemoPlay*>(demo)->Holding())
		{
			string128 cmd;
			Console->Execute("stat_frame_pacing 3600");
			xr_sprintf(cmd, sizeof(cmd), "stat_frame_pacing result %s_h%02u", s.name, s.hour);
			Console->Execute(cmd);
			set_weather(0);
			xr_sprintf(cmd, sizeof(cmd), "stat_frame_pacing mark %s", kWeathers[s.weather]);
			Console->Execute(cmd);
			s.now = phase::flying;
		}
		else if (s.now == phase::flying && Device.dwTimeGlobal - s.weather_at >= kWeatherMs)
		{
			set_weather(s.weather + 1);
			string128 cmd;
			xr_sprintf(cmd, sizeof(cmd), "stat_frame_pacing mark %s", kWeathers[s.weather]);
			Console->Execute(cmd);
		}
	}

	class CCC_FlightRecord : public IConsole_Command
	{
	public:
		CCC_FlightRecord(LPCSTR N) : IConsole_Command(N) {}

		virtual void Execute(LPCSTR args)
		{
			if (!g_pGameLevel || s.now != phase::off || !args[0])
			{
				Msg("! flight_record <name>: in a level, with no flight going");
				return;
			}
			Console->Hide();
			string_path fn;
			file_path(fn, args);
			auto rec = xr_new<CDemoRecord>(fn, &pDemoRecords);
			rec->SetAutoKey(kKey);
			g_pGameLevel->Cameras().AddCamEffector(rec);
			xr_strcpy(s.name, args);
			begin(true);
			Msg("* Flight: recording %s (ESC ends it)", fn);
		}

		virtual void Info(TInfo& I) { xr_strcpy(I, "<name>: fly the camera and record it"); }
	};

	class CCC_FlightPlay : public IConsole_Command
	{
	public:
		CCC_FlightPlay(LPCSTR N) : IConsole_Command(N) {}

		virtual void Execute(LPCSTR args)
		{
			string64 name = "";
			u32 hour = 12;
			sscanf(args, "%63s %u", name, &hour);
			string_path fn;
			file_path(fn, name);
			if (!g_pGameLevel || s.now != phase::off || !name[0] || !FS.exist(fn))
			{
				Msg("! flight_play <name> [hour]: in a level, with no flight going, a recorded flight");
				return;
			}
			Console->Hide();
			if (Device.Paused())
				Device.Pause(FALSE, TRUE, TRUE, "flight_play");
			xr_strcpy(s.name, name);
			s.hour = hour % 24;
			s.hud = !!psHUD_Flags.test(HUD_DRAW);
			psHUD_Flags.set(HUD_DRAW, FALSE);
			s.time_factor = Level().GetGameTimeFactor();
			Level().SetGameTimeFactor(1.f);
			set_hour(s.hour);
			set_weather(0);
			g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoPlay>(fn, kKey, 1, 60 * 60 * 1000.f, kHoldSec));
			begin(false);
			Msg("* Flight: %s, hour %u", name, s.hour);
		}

		virtual void Info(TInfo& I) { xr_strcpy(I, "<name> [hour]: fly a recorded flight as a benchmark"); }
	};

	class CCC_FlightStop : public IConsole_Command
	{
	public:
		CCC_FlightStop(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

		virtual void Execute(LPCSTR args)
		{
			if (s.now == phase::off || !g_pGameLevel)
				return;
			if (s.now == phase::recording)
			{
				for (auto rec : pDemoRecords)
					rec->StopDemo();
				pDemoRecords.clear();
			}
			else
				g_pGameLevel->Cameras().RemoveCamEffector(cefDemo);
		}
	};

	void register_commands()
	{
		CMD1(CCC_FlightRecord, "flight_record");
		CMD1(CCC_FlightPlay, "flight_play");
		CMD1(CCC_FlightStop, "flight_stop");
	}
}
