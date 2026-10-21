#pragma once

// The mod cost overlay (perf_overlay_mods): Lua time per frame by Mod Organizer 2 mod, measured by axr_main's callback
// dispatch (and the time events it wraps) while the overlay is on, and the time of loading each mod's scripts at the
// last load (the script engine's)
namespace mod_cost
{
struct row
{
	xr_string name; // the mod, or the script when not from a mod
	float avg_ms; // per frame
	float peak_ms; // the slowest frame
	float calls; // per frame
};

ENGINE_API extern int enabled; // perf_overlay_mods

// From Lua once a second: the rows, "name\tavg\tpeak\tcalls\n" each, slowest first, and the frames measured
ENGINE_API void set_frame_rows(LPCSTR text, float total_ms, u32 frames);
ENGINE_API const xr_vector<row>& frame_rows();
ENGINE_API float frame_total_ms();

// Script loading: a script's load time (its own, nested loads apart), added to its mod. Reset when the script engine
// starts over (every level load)
ENGINE_API void load_reset();
ENGINE_API void load_add(LPCSTR script, float ms);
// Mods by load time, slowest first
ENGINE_API void load_rows(xr_vector<row>& rows, float& total_ms);
}
