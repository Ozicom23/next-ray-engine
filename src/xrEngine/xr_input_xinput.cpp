#include "stdafx.h"
#include "xr_input.h"
#include "xr_input_xinput.h"
#include <xinput.h>

namespace xinput
{
typedef DWORD (WINAPI* get_state_fn)(DWORD user, XINPUT_STATE* state);
typedef DWORD (WINAPI* set_state_fn)(DWORD user, XINPUT_VIBRATION* vibration);

static get_state_fn s_get_state = nullptr;
static set_state_fn s_set_state = nullptr;

static bool load()
{
	static bool tried = false;
	if (tried)
		return s_get_state != nullptr;
	tried = true;
	for (LPCSTR dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
	{
		HMODULE module = LoadLibrary(dll);
		if (!module)
			continue;
		s_get_state = (get_state_fn)GetProcAddress(module, "XInputGetState");
		s_set_state = (set_state_fn)GetProcAddress(module, "XInputSetState");
		if (s_get_state)
		{
			Msg("* Gamepad: %s", dll);
			return true;
		}
		FreeLibrary(module);
	}
	Msg("! Gamepad: no XInput library");
	return false;
}

static float stick(SHORT v)
{
	return _max(-1.f, float(v) / 32767.f);
}

bool read(int slot, gamepad_state& state)
{
	XINPUT_STATE xs;
	if (!load() || s_get_state(DWORD(slot), &xs) != ERROR_SUCCESS)
		return false;

	static const struct
	{
		WORD xinput;
		int code;
	} buttons[] = {
		{XINPUT_GAMEPAD_A, GP_A}, {XINPUT_GAMEPAD_B, GP_B}, {XINPUT_GAMEPAD_X, GP_X}, {XINPUT_GAMEPAD_Y, GP_Y},
		{XINPUT_GAMEPAD_LEFT_SHOULDER, GP_LB}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, GP_RB},
		{XINPUT_GAMEPAD_BACK, GP_BACK}, {XINPUT_GAMEPAD_START, GP_START},
		{XINPUT_GAMEPAD_LEFT_THUMB, GP_LS}, {XINPUT_GAMEPAD_RIGHT_THUMB, GP_RS},
		{XINPUT_GAMEPAD_DPAD_UP, GP_DPAD_UP}, {XINPUT_GAMEPAD_DPAD_DOWN, GP_DPAD_DOWN},
		{XINPUT_GAMEPAD_DPAD_LEFT, GP_DPAD_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, GP_DPAD_RIGHT},
	};
	const XINPUT_GAMEPAD& g = xs.Gamepad;
	state.connected = true;
	state.buttons = 0;
	for (const auto& b : buttons)
		if (g.wButtons & b.xinput)
			state.buttons |= 1u << (b.code - GP_A);
	state.lt = float(g.bLeftTrigger) / 255.f;
	state.rt = float(g.bRightTrigger) / 255.f;
	state.lx = stick(g.sThumbLX);
	state.ly = stick(g.sThumbLY);
	state.rx = stick(g.sThumbRX);
	state.ry = stick(g.sThumbRY);
	return true;
}

void vibrate(int slot, float low, float high)
{
	if (!load() || !s_set_state || slot < 0)
		return;
	XINPUT_VIBRATION v;
	v.wLeftMotorSpeed = WORD(iFloor(clampr(low, 0.f, 1.f) * 65535.f));
	v.wRightMotorSpeed = WORD(iFloor(clampr(high, 0.f, 1.f) * 65535.f));
	s_set_state(DWORD(slot), &v);
}
}
