#include "stdafx.h"
#include "level_preload.h"
#include "FS_internal.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace level_preload
{
// Off: on the Mac (Wine, fast SSD) transitions took the same time with it (the level's own phase is ~0.9 s either way);
// to measure where reading is slow (the Deck's card, hard disks) before turning it on
int enabled = 1;
int budget_mb = 768;

namespace
{
struct buffer
{
	void* data;
	u32 size;
};

std::mutex s_lock;
std::unordered_map<std::string, buffer> s_files; // full path, lower case
string64 s_level = "";
std::thread s_thread;
std::atomic<bool> s_stop{false};
std::atomic<bool> s_done{false};
u64 s_read_bytes = 0, s_taken_bytes = 0;
u32 s_taken = 0;
float s_read_ms = 0.f;
u64 s_last_asked = 0; // GetTickCount64 of the last start() for this level

std::string key_of(LPCSTR path)
{
	std::string k = path;
	for (char& c : k)
		c = char(tolower(u8(c)));
	return k;
}

void free_all()
{
	for (auto& it : s_files)
		xr_free(it.second.data);
	s_files.clear();
}

void stop_thread()
{
	s_stop = true;
	if (s_thread.joinable())
		s_thread.join();
}

void run(std::string level)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	CTimer timer;
	timer.Start();
	string_path folder;
	xr_sprintf(folder, "%s\\", level.c_str());
	xr_vector<LPSTR>* files = FS.file_list_open("$game_levels$", folder, FS_ListFiles | FS_RootOnly);
	u64 total = 0;
	u32 count = 0;
	const u64 budget = u64(_max(budget_mb, 0)) << 20;
	if (files)
	{
		for (LPSTR name : *files)
		{
			if (s_stop)
				break;
			// The collision model comes from appdata\cform_cache; spawn and graph data aren't per level
			if (strstr(name, ".cform") || strchr(name, '\\'))
				continue;
			string_path rel, full;
			xr_sprintf(rel, "%s\\%s", level.c_str(), name);
			FS.update_path(full, "$game_levels$", rel);
			IReader* R = FS.r_open(full);
			if (!R)
				continue;
			const u32 size = u32(R->length());
			if (size && total + size <= budget)
			{
				void* data = xr_malloc(size);
				CopyMemory(data, R->pointer(), size);
				total += size;
				++count;
				std::lock_guard<std::mutex> lock(s_lock);
				s_files[key_of(full)] = {data, size};
			}
			FS.r_close(R);
		}
		FS.file_list_close(files);
	}
	std::lock_guard<std::mutex> lock(s_lock);
	s_read_bytes = total;
	s_read_ms = timer.GetElapsed_sec() * 1000.f;
	s_done = true;
	Msg("* Level preload: %s, %u files, %.0f MB read ahead in %.0f ms%s", level.c_str(), count, double(total) / 1048576.0,
	    s_read_ms, s_stop ? " (stopped)" : "");
}
}

void start(LPCSTR level)
{
	if (!enabled || !level || !level[0])
		return;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_last_asked = GetTickCount64();
		if (!xr_strcmp(s_level, level))
			return;
	}
	stop_thread();
	std::lock_guard<std::mutex> lock(s_lock);
	free_all();
	xr_strcpy(s_level, level);
	s_stop = false;
	s_done = false;
	s_read_bytes = s_taken_bytes = 0;
	s_taken = 0;
	Msg("* Level preload: reading %s ahead (near its level changer)", level);
	s_thread = std::thread(run, std::string(level));
}

IReader* take(LPCSTR full_path)
{
	if (!s_level[0])
		return nullptr;
	std::lock_guard<std::mutex> lock(s_lock);
	auto it = s_files.find(key_of(full_path));
	if (it == s_files.end())
		return nullptr;
	const buffer b = it->second;
	s_files.erase(it);
	++s_taken;
	s_taken_bytes += b.size;
	return xr_new<CTempReader>(b.data, int(b.size), 0);
}

void loaded(LPCSTR level)
{
	if (!s_level[0])
		return;
	stop_thread();
	std::lock_guard<std::mutex> lock(s_lock);
	if (!xr_strcmp(s_level, level))
		Msg("* Level preload: %s loaded, %u files (%.0f MB) came from memory, %.0f MB unused", level, s_taken,
		    double(s_taken_bytes) / 1048576.0, double(s_read_bytes - _min(s_read_bytes, s_taken_bytes)) / 1048576.0);
	free_all();
	s_level[0] = 0;
}

LPCSTR pending()
{
	return s_level;
}

void shutdown()
{
	stop_thread();
	std::lock_guard<std::mutex> lock(s_lock);
	free_all();
	s_level[0] = 0;
}

void expire(u32 ms)
{
	if (!s_level[0] || GetTickCount64() - s_last_asked < ms)
		return;
	stop_thread();
	std::lock_guard<std::mutex> lock(s_lock);
	Msg("* Level preload: %s not needed for %u s, freed", s_level, ms / 1000);
	free_all();
	s_level[0] = 0;
}
}
