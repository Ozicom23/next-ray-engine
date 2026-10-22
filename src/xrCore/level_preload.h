#pragma once

// Level preloading (level_preload): while the actor is near a level changer, a background thread reads the next level's
// files (geometry, AI map, details, lightmaps... from its archive or the mods' loose files) into memory, so its load
// takes them from there instead of the disk. The collision model is left out (appdata\cform_cache has it). Once that
// level is loaded what it didn't take is freed; so is all of it when another level is asked for
namespace level_preload
{
XRCORE_API extern int enabled; // level_preload: 1 on (the default), 0 off
XRCORE_API extern int budget_mb; // level_preload_budget: the most read ahead, MB

// The level's folder name (l02_garbage). Again for the same level: nothing
XRCORE_API void start(LPCSTR level);
// FS: the file at this full path (lower case), read ahead: its reader, nullptr when not
IReader* take(LPCSTR full_path);
// The level loaded: what wasn't used is freed, a line to the log
XRCORE_API void loaded(LPCSTR level);
// The level asked for, "" none
XRCORE_API LPCSTR pending();
// Freed when not asked for again (the actor away from its level changer) for ms
XRCORE_API void expire(u32 ms);
// The game's end: the thread stopped, everything freed
XRCORE_API void shutdown();
}
