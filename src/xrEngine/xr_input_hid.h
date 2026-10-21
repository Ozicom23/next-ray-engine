#pragma once

struct gamepad_state;

// A Sony controller (DualSense, DualSense Edge, DualShock 4) read as raw HID, USB or Bluetooth: its buttons, sticks
// and triggers like XInput's plus the gyro; rumble and the DualSense's trigger resistance. Windows doesn't show these
// controllers as XInput ones (only Steam Input or a driver does, without the gyro). A thread reads the reports and looks
// for a controller every 2 s while none is connected
namespace sonypad
{
// The controller's state, the gyro's turn since the last read included: false when none is connected
bool read(gamepad_state& state);
// Motor speeds 0..1 (low: the left, heavy motor; high: the right, light one)
void vibrate(float low, float high);
// The DualSense's right trigger: 0 free, 1 a firearm's trigger (resistance, then a break)
void trigger_effect(int effect);
// Stops the thread and closes the controller
void shutdown();
}
