#include "stdafx.h"
#include "gamepad.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/gamepad_glyphs.h"
#include "xr_level_controller.h"
#include "Level.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "WeaponMagazined.h"
#include "UIGameCustom.h"
#include "UI/UIDialogWnd.h"
#include "UI/UIGameTutorial.h"
#include "UICursor.h"
#include "MainMenu.h"
#include "UI/UIStatic.h"
#include "UI/UIInventoryUtilities.h"
#include "ui_base.h"
#include "string_table.h"
#include "../Include/xrRender/UIRender.h"
#include "../Include/xrRender/UIShader.h"
#include <typeinfo>

extern ENGINE_API float psMouseSens;
extern ENGINE_API float psMouseSensScale;
extern ENGINE_API float psMouseSensVerticalK;
extern ENGINE_API Flags32 psMouseInvert;
extern ENGINE_API float psGamepadVibration;
extern ENGINE_API int psGamepadSony;
extern int g_gp_glyphs; // xr_level_controller.cpp
void gamepad_layout_update();

// Settings (Options > Modded Exes > Controls > Gamepad)
int g_gp_enabled = 1;
int g_gp_invert_y = 0;
float g_gp_look_sens_x = 1.f; // x full turn speed: 3 rad/s (~170 degrees) to the side, 2 rad/s up and down
float g_gp_look_sens_y = 1.f;
float g_gp_aim_sens = 0.6f; // x the turn speed while aiming
float g_gp_look_curve = 2.f; // the turn speed by the stick's push: push ^ curve
float g_gp_look_boost = 1.6f; // the stick pushed fully for 0.2 s turns faster, up to this in 0.3 s more
float g_gp_deadzone_left = 0.15f;
float g_gp_deadzone_right = 0.12f;
float g_gp_walk_threshold = 0.6f; // the left stick pushed less than this walks
float g_gp_cursor_speed = 1.f; // x 700 UI units (the screen is 1024 x 768 of them) per second
float g_gp_hold_time = 0.3f; // a button held this long is its _hold key, if bound
int g_gp_gyro = 1; // gyro aiming (DualSense, DualShock 4): 0 off, 1 while aiming, 2 always
float g_gp_gyro_sens = 1.5f; // the camera's turn per the controller's
int g_gp_gyro_invert_y = 0;
int g_gp_adaptive_triggers = 1; // the DualSense's right trigger resists like a firearm's while one is in hands

namespace
{
enum EMode
{
	mOff, // disabled, no gamepad or the console is open
	mGame,
	mCursor, // a menu or a dialog: the cursor
};

struct pad_button
{
	bool down = false;
	bool ignore = false; // held when the mode changed: nothing until released
	bool tap_pending = false; // its _hold key is bound: the press waits for the release or the hold time
	float held = 0.f;
	int sent = 0; // the key pressed for it, 0 none
};

EMode s_mode = mOff;
pad_button s_buttons[GP_BUTTON_COUNT];
bool s_stick_keys[4]; // GP_LS_UP.. pressed
Fvector2 s_move = {0.f, 0.f};
Fvector2 s_look_rest = {0.f, 0.f}; // mouse counts not sent yet
float s_boost_time = 0.f;
int s_sprint_key = 0; // a sprint key kept pressed (sprint by holding, AF_SPRINT_TOGGLE off) until the stick stops
bool s_mouse_down[2] = {false, false};
int s_keys_down[GP_BUTTON_COUNT]; // cursor mode: the key pressed for each button, 0 none
Fvector2 s_cursor = {0.f, 0.f};
bool s_cursor_valid = false;
float s_wheel_rest = 0.f;
bool s_sim_log = false;

u32 bit(int code) { return 1u << (code - GP_A); }

bool bound(int code)
{
	return get_binded_action(code) != kNOTBINDED;
}

// Radial dead zone, the rest scaled to 0..1
Fvector2 dead_zone(float x, float y, float zone)
{
	Fvector2 v = {x, y};
	const float m = v.magnitude();
	if (m <= zone || m < EPS)
		return Fvector2().set(0.f, 0.f);
	const float scaled = _min(1.f, (m - zone) / (1.f - zone));
	return v.mul(scaled / m);
}

// A released stick springs back past the centre for a moment: a push against its last direction within 150 ms of
// that direction is that, not a push
struct stick_rebound
{
	Fvector2 dir = {0.f, 0.f}; // the last push's direction, normalized
	float since = 1.f; // seconds since it

	Fvector2 filter(const Fvector2& v, float dt)
	{
		since += dt;
		const float m = v.magnitude();
		if (m < EPS)
			return v;
		if (dir.dotproduct(v) < 0.f && since < 0.15f)
			return Fvector2().set(0.f, 0.f);
		dir.set(v.x / m, v.y / m);
		since = 0.f;
		return v;
	}
};
stick_rebound s_rebound_left, s_rebound_right;

//------------------------------------------------------------------------------------------------------------------
// Radial menu: hold its key, choose a sector with the right stick, release

struct radial_item
{
	EGameActions action;
	u16 slot; // NO_ACTIVE_SLOT: none to show
	LPCSTR label;
};

// Clockwise from the top
const radial_item s_radial_items[] = {
	{kWPN_3, INV_SLOT_3, "st_gp_radial_rifle"},
	{kWPN_2, INV_SLOT_2, "st_gp_radial_pistol"},
	{kWPN_4, GRENADE_SLOT, "st_gp_radial_grenade"},
	{kWPN_6, BOLT_SLOT, "st_gp_radial_bolt"},
	{kDETECTOR, DETECTOR_SLOT, "st_gp_radial_detector"},
	{kWPN_5, BINOCULAR_SLOT, "st_gp_radial_binoculars"},
	{kWPN_1, KNIFE_SLOT, "st_gp_radial_knife"},
	{kNIGHT_VISION, NO_ACTIVE_SLOT, "st_gp_radial_night_vision"},
};
const int RADIAL_COUNT = sizeof(s_radial_items) / sizeof(s_radial_items[0]);

int s_radial_key = 0; // the key that opened it, 0 closed
int s_radial_sel = -1;
ui_shader* s_radial_shader = nullptr; // made at the first draw (the renderer exists then)
CUIStatic* s_radial_icons[RADIAL_COUNT] = {};
CUITextWnd* s_radial_labels[RADIAL_COUNT] = {};

// A key pressed for an action: sent through the level like a keyboard key (scripts get it too)
void send_action(EGameActions action)
{
	gamepad_resolve(GP_VIRTUAL, action);
	pInput->inject_key(GP_VIRTUAL, true);
	pInput->inject_key(GP_VIRTUAL, false);
	gamepad_resolve(GP_VIRTUAL, kNOTBINDED);
}

void radial_close(bool choose)
{
	const int sel = s_radial_sel;
	s_radial_key = 0;
	s_radial_sel = -1;
	if (choose && sel >= 0)
		send_action(s_radial_items[sel].action);
}

void radial_update(const gamepad_state& pad)
{
	const Fvector2 rs = {pad.rx, pad.ry};
	if (rs.magnitude() < 0.5f)
		return; // the last choice stays
	float angle = atan2f(rs.x, rs.y); // 0 up, clockwise
	if (angle < 0.f)
		angle += PI_MUL_2;
	const float sector = PI_MUL_2 / float(RADIAL_COUNT);
	s_radial_sel = iFloor((angle + sector * 0.5f) / sector) % RADIAL_COUNT;
}

//------------------------------------------------------------------------------------------------------------------
// Game: buttons as keys, the sticks move and turn

EGameActions swap_action()
{
	CInventory& inv = Actor()->inventory();
	const bool has2 = !!inv.ItemFromSlot(INV_SLOT_2), has3 = !!inv.ItemFromSlot(INV_SLOT_3);
	const u16 active = inv.GetActiveSlot();
	if (active == INV_SLOT_3)
		return has2 ? kWPN_2 : kWPN_SWAP;
	if (active == INV_SLOT_2)
		return has3 ? kWPN_3 : kWPN_SWAP;
	return has3 ? kWPN_3 : (has2 ? kWPN_2 : kWPN_SWAP);
}

// The active slot's key hides it; with nothing in hands the last one's takes it out again
EGameActions holster_action()
{
	CInventory& inv = Actor()->inventory();
	u16 slot = inv.GetActiveSlot();
	if (slot == NO_ACTIVE_SLOT || !inv.ActiveItem())
		slot = inv.GetPrevActiveSlot();
	if (slot >= KNIFE_SLOT && slot <= BOLT_SLOT)
		return EGameActions(kWPN_1 + slot - KNIFE_SLOT);
	return kWPN_HOLSTER;
}

void key_press(int code)
{
	CActor* actor = Actor();
	switch (gamepad_binding(code))
	{
	case kRADIAL_MENU:
		s_radial_key = code;
		s_radial_sel = -1;
		return;
	case kUSE_OR_RELOAD:
		if (actor)
			gamepad_resolve(code, actor->GetDefaultActionForObject() ? kUSE : kWPN_RELOAD);
		break;
	case kWPN_SWAP:
		if (actor)
			gamepad_resolve(code, swap_action());
		break;
	case kWPN_HOLSTER:
		if (actor)
			gamepad_resolve(code, holster_action());
		break;
	case kSPRINT_TOGGLE:
		// Sprint by holding its key: the click keeps it held until the stick stops or the next click
		if (!psActorFlags.test(AF_SPRINT_TOGGLE))
		{
			if (s_sprint_key)
			{
				pInput->inject_key(s_sprint_key, false);
				s_sprint_key = 0;
				return;
			}
			s_sprint_key = code;
		}
		break;
	}
	pInput->inject_key(code, true);
}

void key_release(int code)
{
	if (code == s_radial_key)
	{
		radial_close(true);
		return;
	}
	if (code == s_sprint_key)
		return;
	pInput->inject_key(code, false);
	gamepad_resolve(code, kNOTBINDED);
}

void set_stick_key(int i, bool on)
{
	if (s_stick_keys[i] == on)
		return;
	s_stick_keys[i] = on;
	const int code = GP_LS_UP + i;
	if (!bound(code))
		return;
	if (on)
		key_press(code);
	else
		key_release(code);
}

void update_game(const gamepad_state& pad, float dt)
{
	const bool aiming = !!(pad.buttons & bit(GP_LT));
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
	{
		pad_button& t = s_buttons[b];
		const bool down = !!(pad.buttons & (1u << b));
		const int code = GP_A + b;
		if (t.ignore)
		{
			t.ignore = down;
			t.down = false;
			continue;
		}
		if (down && !t.down)
		{
			t.down = true;
			t.held = 0.f;
			t.sent = 0;
			t.tap_pending = false;
			if (code != GP_LT && aiming && bound(code + GP_AIM))
				key_press(t.sent = code + GP_AIM);
			else if (bound(code + GP_HOLD))
				t.tap_pending = true;
			else if (bound(code))
				key_press(t.sent = code);
		}
		else if (down)
		{
			t.held += dt;
			if (t.tap_pending && t.held >= g_gp_hold_time)
			{
				t.tap_pending = false;
				key_press(t.sent = code + GP_HOLD);
			}
		}
		else if (t.down)
		{
			t.down = false;
			if (t.tap_pending && bound(code))
			{
				key_press(code);
				key_release(code);
			}
			else if (t.sent)
				key_release(t.sent);
			t.tap_pending = false;
			t.sent = 0;
		}
	}

	// Left stick: its directions' keys just past the dead zone by the eight sectors of its direction (a key on when its
	// axis takes over 42% of the push, off under 34%; diagonals press two), kept while the stick is near the centre
	// (a key coming and going there, as the stick is pushed or springs back, started the actor's lurch of a new
	// direction just before moving and before stopping); the vector for the actor (its direction and how far it's
	// pushed set the speed)
	s_move = s_rebound_left.filter(dead_zone(pad.lx, pad.ly, g_gp_deadzone_left), dt);
	const float on = 0.08f, push = s_move.magnitude();
	const bool any_key = s_stick_keys[0] || s_stick_keys[1] || s_stick_keys[2] || s_stick_keys[3];
	if (push <= on)
	{
		for (int i = 0; i < 4; ++i)
			set_stick_key(i, false);
	}
	else if (push >= 0.25f || !any_key)
	{
		const float axis[4] = {s_move.y / push, -s_move.y / push, -s_move.x / push, s_move.x / push};
		for (int i = 0; i < 4; ++i)
			set_stick_key(i, axis[i] > (s_stick_keys[i] ? 0.34f : 0.42f));
	}
	if (s_sprint_key && s_move.y < 0.3f)
	{
		pInput->inject_key(s_sprint_key, false);
		s_sprint_key = 0;
	}

	if (s_radial_key)
	{
		radial_update(pad);
		s_look_rest.set(0.f, 0.f);
		return;
	}

	// Right stick: turns as the mouse does (the actor scales it by the field of view, zoomed in slower)
	const Fvector2 rs = s_rebound_right.filter(dead_zone(pad.rx, pad.ry, g_gp_deadzone_right), dt);
	const float m = rs.magnitude();
	const float per_count = psMouseSens * psMouseSensScale / 50.f;
	const bool zoomed = Actor() && Actor()->IsZoomAimingMode();
	// The gyro: the camera turns as the controller does (x gp_gyro_sens), always or while aiming; zoomed in by
	// the field of view's ratio like the mouse, so the sight stays on what it points at
	const bool gyro = pad.gyro && (g_gp_gyro == 2 || (g_gp_gyro == 1 && (aiming || zoomed)));
	float yaw = gyro ? pad.gyro_yaw * g_gp_gyro_sens : 0.f; // radians this frame
	float pitch = gyro ? pad.gyro_pitch * g_gp_gyro_sens * (g_gp_gyro_invert_y ? -1.f : 1.f) : 0.f;
	if (per_count < EPS || psMouseSensVerticalK < EPS || (m < EPS && !gyro))
	{
		s_boost_time = 0.f;
		s_look_rest.set(0.f, 0.f);
		return;
	}
	if (m >= EPS)
	{
		s_boost_time = (m > 0.95f) ? s_boost_time + dt : 0.f;
		const float boost = 1.f + (g_gp_look_boost - 1.f) * clampr((s_boost_time - 0.2f) / 0.3f, 0.f, 1.f);
		float k = powf(m, g_gp_look_curve) / m * boost;
		if (zoomed)
			k *= g_gp_aim_sens;
		yaw += rs.x * k * 3.f * g_gp_look_sens_x * dt; // 3 rad/s
		pitch += rs.y * k * 2.f * g_gp_look_sens_y * (g_gp_invert_y ? -1.f : 1.f) * dt;
	}
	else
		s_boost_time = 0.f;
	s_look_rest.x += yaw / per_count;
	// The actor turns the mouse's y by psMouseSensVerticalK * 3/4 and inverts it with mouse_invert
	float dy = -pitch / (per_count * psMouseSensVerticalK * 0.75f);
	if (psMouseInvert.test(1))
		dy = -dy;
	s_look_rest.y += dy;
	const int nx = int(s_look_rest.x), ny = int(s_look_rest.y);
	s_look_rest.x -= float(nx);
	s_look_rest.y -= float(ny);
	if (nx || ny)
		pInput->inject_mouse_move(nx, ny);
}

//------------------------------------------------------------------------------------------------------------------
// Menus and dialogs: the left stick moves the cursor, A clicks, X right clicks, Y double clicks, B and Menu are Esc, the
// right stick scrolls; the other buttons are sent as their keys (D-pad down closes the inventory, View the PDA)

void mouse_button(int btn, bool down)
{
	if (s_mouse_down[btn] == down)
		return;
	s_mouse_down[btn] = down;
	pInput->inject_mouse_button(btn, down);
}

void update_cursor(const gamepad_state& pad, float dt)
{
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
	{
		pad_button& t = s_buttons[b];
		const bool down = !!(pad.buttons & (1u << b));
		const int code = GP_A + b;
		if (t.ignore)
		{
			t.ignore = down;
			t.down = false;
			continue;
		}
		if (down == t.down)
			continue;
		t.down = down;
		switch (code)
		{
		case GP_A:
			mouse_button(0, down);
			break;
		case GP_X:
			mouse_button(1, down);
			break;
		case GP_Y:
			if (down)
				for (int i = 0; i < 2; ++i)
				{
					pInput->inject_mouse_button(0, true);
					pInput->inject_mouse_button(0, false);
				}
			break;
		case GP_B:
		case GP_START:
			s_keys_down[b] = down ? DIK_ESCAPE : 0;
			pInput->inject_key(DIK_ESCAPE, down);
			break;
		default:
			s_keys_down[b] = down ? code : 0;
			pInput->inject_key(code, down);
			break;
		}
	}

	const Fvector2 ls = s_rebound_left.filter(dead_zone(pad.lx, pad.ly, g_gp_deadzone_left), dt);
	const float m = ls.magnitude();
	if (m > EPS)
	{
		// Its own position: the window cursor is in whole pixels, a slow stick moves less than one a frame
		if (!s_cursor_valid)
			s_cursor = GetUICursor().GetCursorPosition();
		s_cursor_valid = true;
		const float speed = 700.f * g_gp_cursor_speed * powf(m, 1.5f) / m;
		s_cursor.x = clampr(s_cursor.x + ls.x * speed * dt, 0.f, UI_BASE_WIDTH);
		s_cursor.y = clampr(s_cursor.y - ls.y * speed * dt, 0.f, UI_BASE_HEIGHT);
		GetUICursor().SetUICursorPosition(s_cursor);
		pInput->pad_cursor_moved();
		// A zero move: the windows under the cursor get it (hover, drag) as after a mouse move
		pInput->inject_mouse_move(0, 0);
	}
	else
		s_cursor_valid = false; // the mouse may move it meanwhile

	const Fvector2 rs = s_rebound_right.filter(dead_zone(pad.rx, pad.ry, g_gp_deadzone_right), dt);
	s_wheel_rest += rs.y * 12.f * dt; // notches, 12 a second at full
	if (_abs(rs.y) < EPS)
		s_wheel_rest = 0.f;
	while (_abs(s_wheel_rest) >= 1.f)
	{
		const int dir = s_wheel_rest > 0.f ? 1 : -1;
		s_wheel_rest -= float(dir);
		pInput->inject_mouse_wheel(dir * 120);
	}
}

//------------------------------------------------------------------------------------------------------------------

bool cursor_mode()
{
	if (MainMenu() && MainMenu()->IsActive())
		return true;
	if (!g_pGameLevel)
		return true;
	if (CurrentGameUI() && CurrentGameUI()->TopInputReceiver())
		return true;
	// A tutorial sequence that passes the input on to the level plays like the level
	IInputReceiver* ir = pInput->CurrentIR();
	if (CUISequencer* seq = dynamic_cast<CUISequencer*>(ir))
		return seq->GrabInput();
	return ir != static_cast<IInputReceiver*>(g_pGameLevel);
}

// Everything the gamepad holds is released; buttons still down do nothing until released
void release_all(const gamepad_state& pad)
{
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
	{
		pad_button& t = s_buttons[b];
		if (s_mode == mGame && t.sent && t.sent != s_radial_key)
		{
			pInput->inject_key(t.sent, false);
			gamepad_resolve(t.sent, kNOTBINDED);
		}
		if (s_mode == mCursor && s_keys_down[b])
			pInput->inject_key(s_keys_down[b], false);
		s_keys_down[b] = 0;
		t = pad_button();
		t.ignore = !!(pad.buttons & (1u << b));
	}
	for (int i = 0; i < 4; ++i)
		set_stick_key(i, false);
	if (s_sprint_key)
		pInput->inject_key(s_sprint_key, false);
	s_sprint_key = 0;
	radial_close(false);
	mouse_button(0, false);
	mouse_button(1, false);
	s_move.set(0.f, 0.f);
	s_look_rest.set(0.f, 0.f);
	s_boost_time = 0.f;
	s_cursor_valid = false;
	s_wheel_rest = 0.f;
}

void gamepad_frame(float dt)
{
	const gamepad_state& pad = pInput->pad();
	gamepad_layout_update(); // a layout saved by an older version: the bindings added since
	// Texts name the gamepad's buttons while it is the device in use, the keyboard's otherwise
	const bool prompts = g_gp_enabled && pInput->pad_in_use();
	static int glyphs = g_gp_glyphs;
	// The glyphs follow the pad connected, in use or not (the buttons overview shows them too)
	if (pad.connected)
		gamepad_glyphs::playstation = pad.playstation;
	if (prompts != g_gamepad_prompts || (prompts && (pad.playstation != g_gamepad_playstation || glyphs != g_gp_glyphs)))
	{
		g_gamepad_prompts = prompts;
		g_gamepad_playstation = pad.playstation;
		glyphs = g_gp_glyphs;
		CStringTable::ReparseKeyBindings();
	}
	EMode mode = mOff;
	if (g_gp_enabled && pad.connected && !(Console && Console->bVisible))
		mode = cursor_mode() ? mCursor : mGame;
	if (mode != s_mode)
	{
		if (s_sim_log)
		{
			IInputReceiver* ir = pInput->CurrentIR();
			CUIDialogWnd* top = CurrentGameUI() ? CurrentGameUI()->TopInputReceiver() : nullptr;
			Msg("* gamepad: %s (main menu %d, level %d, dialog %s, input to %s)",
			    mode == mGame ? "game" : (mode == mCursor ? "cursor" : "off"), MainMenu() && MainMenu()->IsActive(),
			    g_pGameLevel != nullptr, top ? typeid(*top).name() : "none", ir ? typeid(*ir).name() : "none");
		}
		release_all(pad);
		s_mode = mode;
	}
	if (mode == mGame)
		update_game(pad, dt);
	else if (mode == mCursor)
		update_cursor(pad, dt);

	// The DualSense's right trigger resists like a firearm's while one is in hands
	bool firearm = false;
	if (mode == mGame && g_gp_adaptive_triggers && Actor() && Actor()->g_Alive())
	{
		const CWeaponMagazined* weapon = smart_cast<const CWeaponMagazined*>(Actor()->inventory().ActiveItem());
		firearm = weapon && weapon->GetState() != CHUDState::eHidden && weapon->GetState() != CHUDState::eShowing;
	}
	pInput->pad_trigger_effect(firearm ? 1 : 0);
}

//------------------------------------------------------------------------------------------------------------------
// gp_sim: a simulated gamepad for tests, e.g. "gp_sim A ly=1" (A pressed, the left stick up), "gp_sim" (connected,
// nothing pressed), "gp_sim off". Buttons: A B X Y LB RB LT RT BACK START LS RS UP DOWN LEFT RIGHT; axes lx ly rx ry lt rt,
// the gyro gx gy (turn rate right and up, radians/s), "ps" names the buttons like a PlayStation controller
class CCC_GamepadSim : public IConsole_Command
{
public:
	CCC_GamepadSim(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }

	virtual void Execute(LPCSTR args)
	{
		static LPCSTR names[GP_BUTTON_COUNT] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "BACK", "START", "LS",
			"RS", "UP", "DOWN", "LEFT", "RIGHT"};
		if (!xr_strcmp(args, "off"))
		{
			pInput->pad_simulate(nullptr);
			s_sim_log = false;
			return;
		}
		gamepad_state st;
		st.connected = true;
		st.gyro = true;
		const int count = _GetItemCount(args, ' ');
		for (int i = 0; i < count; ++i)
		{
			string64 item;
			_GetItem(args, i, item, ' ');
			if (!item[0])
				continue;
			float v = 0.f;
			string16 axis = "";
			if (sscanf(item, "%15[a-z]=%f", axis, &v) == 2)
			{
				float* axes[] = {&st.lx, &st.ly, &st.rx, &st.ry, &st.lt, &st.rt, &st.gyro_yaw, &st.gyro_pitch};
				LPCSTR axis_names[] = {"lx", "ly", "rx", "ry", "lt", "rt", "gx", "gy"};
				for (int a = 0; a < 8; ++a)
					if (!xr_strcmp(axis, axis_names[a]))
						*axes[a] = v;
				continue;
			}
			if (!xr_strcmp(item, "ps"))
				st.playstation = true;
			for (int b = 0; b < GP_BUTTON_COUNT; ++b)
				if (!stricmp(item, names[b]))
				{
					st.buttons |= 1u << b;
					if (b == GP_LT - GP_A)
						st.lt = 1.f;
					if (b == GP_RT - GP_A)
						st.rt = 1.f;
				}
		}
		pInput->pad_simulate(&st);
		s_sim_log = true;
	}
	virtual void Save(IWriter*) {}
};

// gp_playstation: 1 while the pad in use is a PlayStation one (its button names and glyphs), for scripts; not saved
class CCC_GamepadPlaystation : public IConsole_Command
{
public:
	CCC_GamepadPlaystation(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args) {}
	virtual void Status(TStatus& S) { xr_strcpy(S, gamepad_glyphs::playstation ? "1" : "0"); }
	virtual void Save(IWriter*) {}
};
}

void gamepad_register_commands()
{
	CMD4(CCC_Integer, "gp_enabled", &g_gp_enabled, 0, 1);
	CMD4(CCC_Integer, "gp_invert_y", &g_gp_invert_y, 0, 1);
	CMD4(CCC_Float, "gp_look_sens_x", &g_gp_look_sens_x, 0.1f, 4.f);
	CMD4(CCC_Float, "gp_look_sens_y", &g_gp_look_sens_y, 0.1f, 4.f);
	CMD4(CCC_Float, "gp_aim_sens", &g_gp_aim_sens, 0.1f, 2.f);
	CMD4(CCC_Float, "gp_look_curve", &g_gp_look_curve, 1.f, 4.f);
	CMD4(CCC_Float, "gp_look_boost", &g_gp_look_boost, 1.f, 3.f);
	CMD4(CCC_Float, "gp_deadzone_left", &g_gp_deadzone_left, 0.f, 0.5f);
	CMD4(CCC_Float, "gp_deadzone_right", &g_gp_deadzone_right, 0.f, 0.5f);
	CMD4(CCC_Float, "gp_walk_threshold", &g_gp_walk_threshold, 0.f, 1.f);
	CMD4(CCC_Float, "gp_cursor_speed", &g_gp_cursor_speed, 0.2f, 4.f);
	CMD4(CCC_Float, "gp_hold_time", &g_gp_hold_time, 0.15f, 1.f);
	CMD4(CCC_Float, "gp_vibration", &psGamepadVibration, 0.f, 1.f);
	CMD4(CCC_Integer, "gp_sony", &psGamepadSony, 0, 1);
	CMD4(CCC_Integer, "gp_gyro", &g_gp_gyro, 0, 2);
	CMD4(CCC_Float, "gp_gyro_sens", &g_gp_gyro_sens, 0.1f, 5.f);
	CMD4(CCC_Integer, "gp_gyro_invert_y", &g_gp_gyro_invert_y, 0, 1);
	CMD4(CCC_Integer, "gp_adaptive_triggers", &g_gp_adaptive_triggers, 0, 1);
	CMD4(CCC_Integer, "gp_glyphs", &g_gp_glyphs, 0, 1);
	CMD1(CCC_GamepadSim, "gp_sim");
	CMD1(CCC_GamepadPlaystation, "gp_playstation");
}

void gamepad_start()
{
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
		s_keys_down[b] = 0;
	pInput->pad_handler = &gamepad_frame;
}

void gamepad_stop()
{
	if (pInput)
		pInput->pad_handler = nullptr;
	for (int i = 0; i < RADIAL_COUNT; ++i)
	{
		xr_delete(s_radial_icons[i]);
		xr_delete(s_radial_labels[i]);
	}
	xr_delete(s_radial_shader);
}

bool gamepad_move(Fvector2& move)
{
	if (s_mode != mGame || s_move.magnitude() < EPS)
		return false;
	move = s_move;
	return true;
}

bool gamepad_radial_open()
{
	return s_radial_key != 0;
}

void gamepad_rumble(float low, float high, float time)
{
	if (s_mode == mGame)
		pInput->feedback(u16(iFloor(clampr(low, 0.f, 1.f) * 65535.f)), u16(iFloor(clampr(high, 0.f, 1.f) * 65535.f)),
		                 time);
}

// The radial menu: eight sectors around the screen's centre, the chosen one amber, each with its slot's item (icon and
// name) or what it's for
void gamepad_render_ui()
{
	if (!s_radial_key || !Actor())
		return;

	const float kx = UI().get_current_kx(); // wide screens: UI x units are narrower
	const Fvector2 c = {UI_BASE_WIDTH * 0.5f, UI_BASE_HEIGHT * 0.5f};
	const float r_in = 70.f, r_out = 210.f, r_mid = 140.f;
	const float sector = PI_MUL_2 / float(RADIAL_COUNT);
	const float gap = 0.025f; // radians between sectors
	const int steps = 6;

	if (!s_radial_shader)
	{
		s_radial_shader = xr_new<ui_shader>();
		(*s_radial_shader)->create("hud\\crosshair");
	}
	auto point = [&](float r, float a, u32 color)
	{
		Fvector2 p = {c.x + _sin(a) * r * kx, c.y - _cos(a) * r};
		UI().ClientToScreenScaled(p);
		UIRender->PushPoint(p.x, p.y, 0.f, color, 0.f, 0.f);
	};
	UIRender->StartPrimitive(RADIAL_COUNT * steps * 6, IUIRender::ptTriList, UI().m_currentPointType);
	for (int i = 0; i < RADIAL_COUNT; ++i)
	{
		const u32 color = (i == s_radial_sel) ? color_argb(210, 242, 165, 26) : color_argb(160, 12, 12, 12);
		const float a_from = float(i) * sector - sector * 0.5f + gap;
		const float a_to = float(i) * sector + sector * 0.5f - gap;
		for (int s = 0; s < steps; ++s)
		{
			const float a0 = a_from + (a_to - a_from) * float(s) / float(steps);
			const float a1 = a_from + (a_to - a_from) * float(s + 1) / float(steps);
			point(r_in, a0, color);
			point(r_out, a0, color);
			point(r_out, a1, color);
			point(r_in, a0, color);
			point(r_out, a1, color);
			point(r_in, a1, color);
		}
	}
	UIRender->SetShader(**s_radial_shader);
	UIRender->FlushPrimitive();

	CInventory& inv = Actor()->inventory();
	for (int i = 0; i < RADIAL_COUNT; ++i)
	{
		const radial_item& item = s_radial_items[i];
		PIItem itm = item.slot != NO_ACTIVE_SLOT ? inv.ItemFromSlot(item.slot) : nullptr;
		const Fvector2 at = {c.x + _sin(float(i) * sector) * r_mid * kx, c.y - _cos(float(i) * sector) * r_mid};

		if (!s_radial_labels[i])
		{
			s_radial_labels[i] = xr_new<CUITextWnd>();
			s_radial_labels[i]->SetFont(UI().Font().pFontLetterica16Russian);
			s_radial_labels[i]->SetTextAlignment(CGameFont::alCenter);
			s_radial_labels[i]->SetWndSize(Fvector2().set(130.f * kx, 18.f));
			s_radial_icons[i] = xr_new<CUIStatic>();
		}
		CUITextWnd* label = s_radial_labels[i];
		label->SetText(itm ? itm->NameShort() : *CStringTable().translate(item.label));
		label->SetTextColor((itm || item.slot == NO_ACTIVE_SLOT) ? color_argb(255, 235, 235, 235) :
			color_argb(255, 120, 120, 120));
		label->SetWndPos(Fvector2().set(at.x - 65.f * kx, at.y + 24.f));
		label->Draw();

		if (!itm)
			continue;
		// The item's icon in a box of 100 x 44 UI units, its proportions kept
		CUIStatic* icon = s_radial_icons[i];
		if (pSettings->line_exist(itm->m_section_id.c_str(), "icons_texture"))
			icon->SetShader(InventoryUtilities::GetCustomIconTextureShader(
				pSettings->r_string(itm->m_section_id.c_str(), "icons_texture")));
		else
			icon->SetShader(InventoryUtilities::GetEquipmentIconsShader());
		const Irect grid = itm->GetInvGridRect();
		Frect rect;
		rect.lt.set(float(grid.x1) * INV_GRID_WIDTH, float(grid.y1) * INV_GRID_HEIGHT);
		rect.rb.set(rect.lt.x + float(grid.x2) * INV_GRID_WIDTH, rect.lt.y + float(grid.y2) * INV_GRID_HEIGHT);
		icon->SetTextureRect(rect);
		icon->SetStretchTexture(true);
		icon->TextureOn();
		const float w = rect.width(), h = rect.height();
		const float scale = (w > 0.f && h > 0.f) ? _min(100.f / w, 44.f / h) : 1.f;
		const Fvector2 size = {w * scale * kx, h * scale};
		icon->SetWndSize(size);
		icon->SetWndPos(Fvector2().set(at.x - size.x * 0.5f, at.y - size.y * 0.5f - 6.f));
		icon->Draw();
	}
}
