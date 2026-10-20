#pragma once

struct gamepad_state;

// XInput, loaded at the first use (xinput1_4, xinput1_3 or xinput9_1_0: Windows, Wine and Proton all have one)
namespace xinput
{
// The controller at user index slot (0..3): false when none is connected there
bool read(int slot, gamepad_state& state);
// Motor speeds 0..1 (low: the left, heavy motor; high: the right, light one)
void vibrate(int slot, float low, float high);
}
