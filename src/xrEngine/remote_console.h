#pragma once

// Remote console: lets a script outside the game run console commands, for automated testing.
// Only active with the -remote_console command line flag.
//   $app_data_root$\remote_cmd.txt     commands, one per line; the file is read and deleted.
//                                       Write it under another name and rename it, so it is never read half written.
//                                       Extra commands: rc_wait <seconds>, rc_wait_level [timeout seconds, default 300]
//                                       (until a level is loaded and running), rc_clear (drops the queued commands, right away)
//   $app_data_root$\remote_status.txt  state (menu, loading, level, idle), fps, queue, the last command; rewritten twice a second
namespace remote_console
{
	// Once per main loop iteration, also during loading
	void update();
	// At start, after the file system: the hang watchdog. Once the main loop runs, a main thread that makes no progress
	// (no main loop pass, no log line of its own, no step of a long task: crash_report::beat) for hang_watchdog seconds
	// (60; three times that while loading; 0 off) writes every thread's stack to $logs$\hang_<time>.txt and a crash
	// report with the main thread's stack, says so and closes the game. Not under a debugger, while a fatal error is
	// reported, or across a system sleep. With -remote_console and the watchdog off, the stacks alone after two minutes
	void start_watchdog();
}
