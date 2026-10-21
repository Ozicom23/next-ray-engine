#pragma once

// Benchmark flights: the camera flown along a recorded path while the weather changes
//   flight_record <name>          fly the free camera (demo_record's keys, ESC ends it) and record it 10 times a second
//                                 to savedgames\<name>.xrdemo; invulnerable while it records
//   flight_play <name> [hour]     fly it at that hour (12): held at its start for 5 s, then flown with the weather
//                                 changed every 20 s in a fixed order; invulnerable, no HUD. stat_frame_pacing records it,
//                                 a part per weather: logs\bench_<time>_<name>_h<hour>.txt
//   flight_stop                   end a flight
namespace camera_flight
{
	void frame();
	void register_commands();
}
