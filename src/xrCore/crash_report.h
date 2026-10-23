#pragma once

// Crash reports: at a fatal error or an unhandled exception, appdata\logs\crash_<date>.txt (the game's log is overwritten
// at the next start) with what happened, the probable cause (the script and the Mod Organizer 2 mod it comes from, or the
// engine function), the engine's stack with function names, the Lua stack, the script errors before it and the log's end.
// Function names come from <exe>.sym next to the exe (written at build time by -dump_symbols: the PDB is ~200 MB)
namespace crash_report
{
// Lines the game adds (the level, the actor's position...), set by the game; called inside the crash handler
XRCORE_API extern void (*game_context)(char* out, u32 size);

// what: the error as the log has it; context: the exception's (nullptr: the stack here), thread: the thread it is from
// (nullptr: this one). Returns the report's path ("" when not written)
XRCORE_API LPCSTR write(LPCSTR what, const void* exception_context, void* thread = nullptr);
// A report was started: a fatal error is being reported (its dialog may be up)
XRCORE_API bool started();

// The hang watchdog's heartbeat: the main thread made progress (the main loop, its log lines, a long task's steps)
XRCORE_API void beat();
XRCORE_API u32 beats();

// The function an address in the exe is in, from the .sym file ("module+0x..." for other modules or no name; false then)
XRCORE_API bool function_name(u64 address, char* out, u32 size);

// -dump_symbols <file>: the exe's functions (address, size, name) from its PDB, for the reports. Called before the engine
// starts; false when the PDB isn't there
XRCORE_API bool dump_symbols(LPCSTR out);
}
