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
}
