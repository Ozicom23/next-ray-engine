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
#include "HUDManager.h"
#include "ai_space.h"
#include "script_engine.h"
#include "UIGameCustom.h"
#include "UI/UIDialogWnd.h"
#include "UI/UIGameTutorial.h"
#include "UICursor.h"
#include "MainMenu.h"
#include "UI/UIStatic.h"
#include "UI/UIInventoryUtilities.h"
#include "UI/UIButton.h"
#include "UI/UIComboBox.h"
#include "UI/UITrackBar.h"
#include "UI/UICustomEdit.h"
#include "UI/UIMapWnd.h"
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
float g_gp_aim_slowdown = 0.f; // the turn slower by this share while the crosshair is on a living enemy (0 off)

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
bool s_was_playing = false; // the last frame played with the gamepad (its loss pauses the game)

u32 bit(int code) { return 1u << (code - GP_A); }

bool bound(int code)
{
	return get_binded_action(code) != kNOTBINDED || gamepad_key_binding(code);
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
// The wheels: hold its key, choose a sector with the right stick, release. The radial menu takes out what's in the
// slots; the actions wheel presses mods' keys and actions, its sectors given by scripts as it opens
// (zzzz_nextray_gamepad: action_wheel_open adds them with gamepad_wheel_add, action_wheel_choose gets the one chosen)

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
const int WHEEL_MAX = 12; // the actions wheel's sectors at most

// An actions wheel's sector
struct action_entry
{
	xr_string label; // a string id or the text
	shared_str icon; // an item section: its inventory icon, or none
};
xr_vector<action_entry> s_action_entries;

int s_radial_key = 0; // the key that opened it, 0 closed
bool s_radial_actions = false; // the actions wheel is open, not the radial menu
int s_radial_sel = -1;
u32 s_radial_open_time = 0; // Device.dwTimeContinual when it opened (it runs while paused)
float s_radial_dir = 0.f; // the right stick's angle when it chose, 0 up, clockwise
ui_shader* s_radial_shader = nullptr; // made at the first draw (the renderer exists then)
CUIStatic* s_radial_icons[WHEEL_MAX] = {};
CUITextWnd* s_radial_labels[WHEEL_MAX] = {};
CUITextWnd* s_radial_title = nullptr; // the chosen one's name in the middle

int radial_count()
{
	return s_radial_actions ? int(s_action_entries.size()) : RADIAL_COUNT;
}

// Keyboard keys pressed for the actions wheel (a mod's key, its modifier, taps or a hold), at their times
struct key_event
{
	int dik;
	bool press;
	u32 at; // Device.dwTimeContinual
};
xr_vector<key_event> s_key_events;

void key_events_update()
{
	const u32 now = Device.dwTimeContinual;
	while (!s_key_events.empty() && s_key_events.front().at <= now)
	{
		const key_event e = s_key_events.front();
		s_key_events.erase(s_key_events.begin());
		pInput->inject_key(e.dik, e.press);
	}
}

// A script's function called with the arguments given; false when it doesn't exist
template <typename... Args>
bool call_script(LPCSTR name, Args... args)
{
	luabind::functor<void> f;
	if (!ai().script_engine().functor(name, f))
		return false;
	f(args...);
	return true;
}

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
	if (!choose || sel < 0)
		return;
	if (!s_radial_actions)
		send_action(s_radial_items[sel].action);
	else if (sel < int(s_action_entries.size()))
		call_script("zzzz_nextray_gamepad.action_wheel_choose", sel + 1);
}

void radial_open(int code, bool actions)
{
	if (actions)
	{
		s_action_entries.clear();
		call_script("zzzz_nextray_gamepad.action_wheel_open");
		if (s_action_entries.empty())
			return;
	}
	s_radial_actions = actions;
	s_radial_key = code;
	s_radial_sel = -1;
	s_radial_open_time = Device.dwTimeContinual;
}

void radial_update(const gamepad_state& pad)
{
	const Fvector2 rs = {pad.rx, pad.ry};
	if (rs.magnitude() < 0.5f)
		return; // the last choice stays
	float angle = atan2f(rs.x, rs.y); // 0 up, clockwise
	if (angle < 0.f)
		angle += PI_MUL_2;
	const int n = radial_count();
	const float sector = PI_MUL_2 / float(n);
	s_radial_sel = iFloor((angle + sector * 0.5f) / sector) % n;
	s_radial_dir = angle;
}

// The crosshair is on a living enemy, a weapon in hands (the aim slowdown)
bool on_enemy()
{
	CActor* actor = Actor();
	if (!actor || !smart_cast<CWeapon*>(actor->inventory().ActiveItem()))
		return false;
	CEntityAlive* target = smart_cast<CEntityAlive*>(HUD().GetRQ().O);
	return target && target != actor && target->g_Alive() && actor->is_relation_enemy(target);
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
	if (const int dik = gamepad_key_binding(code))
	{
		pInput->inject_key(dik, true);
		return;
	}
	CActor* actor = Actor();
	switch (gamepad_binding(code))
	{
	case kRADIAL_MENU:
		radial_open(code, false);
		return;
	case kACTION_WHEEL:
		radial_open(code, true);
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
	if (const int dik = gamepad_key_binding(code))
	{
		pInput->inject_key(dik, false);
		return;
	}
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
		if (g_gp_aim_slowdown > 0.f && on_enemy())
			k *= 1.f - g_gp_aim_slowdown;
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
// Menus and dialogs: the left stick moves the cursor (slower over what it can point at, settling onto one near it when
// released), the D-pad steps to the next one in its direction (none there: the list under the cursor scrolls), A
// clicks, X right clicks, Y double clicks, B and Menu are Esc, the right stick scrolls (over the PDA's map it moves
// the map, the triggers zoom it); the other buttons are sent as their keys (View the PDA). An edit box taking text
// shows the on-screen keyboard instead

void release_all(const gamepad_state& pad);

void mouse_button(int btn, bool down)
{
	if (s_mouse_down[btn] == down)
		return;
	s_mouse_down[btn] = down;
	pInput->inject_mouse_button(btn, down);
}

// The dialog on top: the main menu's or the level's
CUIWindow* menu_root()
{
	if (MainMenu() && MainMenu()->IsActive())
		return MainMenu()->TopInputReceiver();
	return CurrentGameUI() ? CurrentGameUI()->TopInputReceiver() : nullptr;
}

// What the cursor can point at in it: buttons, item cells, list rows, check boxes, lists, sliders, edit boxes (their
// visible part in UI units); the PDA's map; an edit box taking text. Rects only (the windows may go before the next
// look), looked for at most every 100 ms
struct menu_scan
{
	xr_vector<Frect> targets;
	bool map = false;
	Frect map_rect;
	bool typing = false;
	Frect edit_rect;
	CUIWindow* root = nullptr;
	u32 time = 0;
};
menu_scan s_scan;

bool is_target(CUIWindow* w)
{
	if (w->ui_cast_cell_item() || w->ui_cast_list_box_item())
		return true;
	if (smart_cast<CUIButton*>(w))
		return w->IsEnabled();
	return smart_cast<CUIComboBox*>(w) || smart_cast<CUITrackBar*>(w) || smart_cast<CUICustomEdit*>(w);
}

// The window's tree: its targets, clipped by the scroll views they're in; the map (not its spots)
void scan_window(CUIWindow* w, const Frect& clip, CUIMapWnd** map)
{
	if (!w->IsShown())
		return;
	Frect r;
	w->GetAbsoluteRect(r);
	if (CUIMapWnd* m = smart_cast<CUIMapWnd*>(w))
	{
		s_scan.map = true;
		s_scan.map_rect = m->ActiveMapRect();
		if (map)
			*map = m;
		return;
	}
	Frect inner = clip;
	if (w->ui_cast_scroll_view() && !inner.intersection(clip, r))
		return;
	if (CUICustomEdit* edit = smart_cast<CUICustomEdit*>(w))
		if (edit->InputFocus())
		{
			s_scan.typing = true;
			s_scan.edit_rect = r;
		}
	if (is_target(w))
	{
		Frect v;
		if (v.intersection(r, inner) && v.width() >= 4.f && v.height() >= 4.f)
			s_scan.targets.push_back(v);
		return;
	}
	for (CUIWindow* child : w->GetChildWndList())
		scan_window(child, inner, map);
}

// Looks again (every 100 ms, at once for another dialog or with force); the map window when asked (valid this frame)
CUIMapWnd* menu_rescan(bool force, bool want_map = false)
{
	CUIWindow* root = menu_root();
	if (!force && !want_map && root == s_scan.root && Device.dwTimeContinual - s_scan.time < 100)
		return nullptr;
	s_scan.targets.clear();
	s_scan.map = false;
	s_scan.typing = false;
	s_scan.root = root;
	s_scan.time = Device.dwTimeContinual;
	CUIMapWnd* map = nullptr;
	if (root)
	{
		scan_window(root, Frect().set(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT), &map);
		// Scripts' item cells (GAMMA's inventory: statics the engine can't tell from others), gamepad_menu_target
		call_script("zzzz_nextray_gamepad.menu_targets");
	}
	return map;
}

int target_at(const Fvector2& p)
{
	for (u32 i = 0; i < s_scan.targets.size(); ++i)
		if (s_scan.targets[i].in(p.x, p.y))
			return int(i);
	return -1;
}

void move_cursor(const Fvector2& p)
{
	if (s_sim_log)
		Msg("* gamepad: cursor %.0f %.0f (%u targets)", p.x, p.y, s_scan.targets.size());
	s_cursor = p;
	GetUICursor().SetUICursorPosition(p);
	pInput->pad_cursor_moved();
	// A zero move: the windows under the cursor get it (hover, drag) as after a mouse move
	pInput->inject_mouse_move(0, 0);
}

// The D-pad: to the nearest target in its direction (d: UI units, y down), from the one under the cursor
void cursor_step(const Fvector2& d)
{
	menu_rescan(true);
	Fvector2 p = GetUICursor().GetCursorPosition();
	const int cur = target_at(p);
	if (cur >= 0)
		s_scan.targets[cur].getcenter(p);
	int best = -1;
	float best_score = flt_max;
	for (u32 i = 0; i < s_scan.targets.size(); ++i)
	{
		if (int(i) == cur)
			continue;
		Fvector2 c;
		s_scan.targets[i].getcenter(c);
		const Fvector2 v = {c.x - p.x, c.y - p.y};
		const float along = v.x * d.x + v.y * d.y, across = _abs(v.x * d.y - v.y * d.x);
		if (along < 4.f || across > along * 3.f)
			continue;
		const float score = along + across * 2.f;
		if (score < best_score)
		{
			best_score = score;
			best = int(i);
		}
	}
	if (best >= 0)
	{
		Fvector2 c;
		s_scan.targets[best].getcenter(c);
		move_cursor(c);
	}
	else if (d.y != 0.f)
	{
		pInput->inject_mouse_wheel(d.y < 0.f ? 120 : -120); // the list under the cursor shows more
		s_scan.time = 0;
	}
}

//------------------------------------------------------------------------------------------------------------------
// The on-screen keyboard, while an edit box takes text: the left stick or the D-pad picks a key, A types it, B erases,
// X types a space, Y is shift, LB / RB move the caret, Menu enters (the edit box keeps the text). It types by keyboard
// keys sent to the edit box

struct osk_key
{
	LPCSTR text, shifted;
	int dik; // 0: shift
	bool shift = false; // typed with shift ('_')
};
const int OSK_ROWS = 4, OSK_COLS = 11;
const osk_key s_osk[OSK_ROWS][OSK_COLS] = {
	{{"1", "!", DIK_1}, {"2", "@", DIK_2}, {"3", "#", DIK_3}, {"4", "$", DIK_4}, {"5", "%", DIK_5},
		{"6", "^", DIK_6}, {"7", "&", DIK_7}, {"8", "*", DIK_8}, {"9", "(", DIK_9}, {"0", ")", DIK_0},
		{"-", "_", DIK_MINUS}},
	{{"q", "Q", DIK_Q}, {"w", "W", DIK_W}, {"e", "E", DIK_E}, {"r", "R", DIK_R}, {"t", "T", DIK_T},
		{"y", "Y", DIK_Y}, {"u", "U", DIK_U}, {"i", "I", DIK_I}, {"o", "O", DIK_O}, {"p", "P", DIK_P},
		{"_", "_", DIK_MINUS, true}},
	{{"a", "A", DIK_A}, {"s", "S", DIK_S}, {"d", "D", DIK_D}, {"f", "F", DIK_F}, {"g", "G", DIK_G},
		{"h", "H", DIK_H}, {"j", "J", DIK_J}, {"k", "K", DIK_K}, {"l", "L", DIK_L}, {".", ">", DIK_PERIOD},
		{",", "<", DIK_COMMA}},
	{{"z", "Z", DIK_Z}, {"x", "X", DIK_X}, {"c", "C", DIK_C}, {"v", "V", DIK_V}, {"b", "B", DIK_B},
		{"n", "N", DIK_N}, {"m", "M", DIK_M}, {"Shift", "SHIFT", 0}, {"Space", "Space", DIK_SPACE},
		{"Del", "Del", DIK_BACK}, {"OK", "OK", DIK_RETURN}},
};
bool s_osk_open = false;
int s_osk_row = 1, s_osk_col = 0;
bool s_osk_shift = false;
float s_osk_repeat = 0.f; // the stick held in a direction: the next step's time
int s_osk_dir = -1;

void osk_press(int dik, bool shift)
{
	if (shift)
		pInput->inject_key(DIK_LSHIFT, true);
	pInput->inject_key(dik, true);
	pInput->inject_key(dik, false);
	if (shift)
		pInput->inject_key(DIK_LSHIFT, false);
}

void osk_type()
{
	const osk_key& k = s_osk[s_osk_row][s_osk_col];
	if (!k.dik)
	{
		s_osk_shift = !s_osk_shift;
		return;
	}
	osk_press(k.dik, k.shift || (s_osk_shift && k.dik != DIK_SPACE && k.dik != DIK_BACK && k.dik != DIK_RETURN));
}

// dir: 0 up, 1 down, 2 left, 3 right; around the edges
void osk_move(int dir)
{
	if (dir < 2)
		s_osk_row = (s_osk_row + (dir ? 1 : OSK_ROWS - 1)) % OSK_ROWS;
	else
		s_osk_col = (s_osk_col + (dir == 3 ? 1 : OSK_COLS - 1)) % OSK_COLS;
}

void update_osk(const gamepad_state& pad, float dt)
{
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
	{
		pad_button& t = s_buttons[b];
		const bool down = !!(pad.buttons & (1u << b));
		if (t.ignore)
		{
			t.ignore = down;
			t.down = false;
			continue;
		}
		const bool pressed = down && !t.down;
		t.down = down;
		if (!pressed)
			continue;
		switch (GP_A + b)
		{
		case GP_A: osk_type(); break;
		case GP_B: osk_press(DIK_BACK, false); break;
		case GP_X: osk_press(DIK_SPACE, false); break;
		case GP_Y: s_osk_shift = !s_osk_shift; break;
		case GP_LB: osk_press(DIK_LEFT, false); break;
		case GP_RB: osk_press(DIK_RIGHT, false); break;
		case GP_START: osk_press(DIK_RETURN, false); break;
		case GP_DPAD_UP: osk_move(0); break;
		case GP_DPAD_DOWN: osk_move(1); break;
		case GP_DPAD_LEFT: osk_move(2); break;
		case GP_DPAD_RIGHT: osk_move(3); break;
		}
	}
	// The stick: a step, then another every 0.1 s after 0.35 s held
	const Fvector2 ls = dead_zone(pad.lx, pad.ly, 0.5f);
	int dir = -1;
	if (ls.magnitude() > EPS)
		dir = _abs(ls.y) > _abs(ls.x) ? (ls.y > 0.f ? 0 : 1) : (ls.x < 0.f ? 2 : 3);
	if (dir < 0)
		s_osk_dir = -1;
	else if (dir != s_osk_dir)
	{
		s_osk_dir = dir;
		s_osk_repeat = 0.35f;
		osk_move(dir);
	}
	else if ((s_osk_repeat -= dt) <= 0.f)
	{
		s_osk_repeat = 0.1f;
		osk_move(dir);
	}
}

void update_cursor(const gamepad_state& pad, float dt)
{
	CUIMapWnd* map = nullptr;
	const Fvector2 rs = s_rebound_right.filter(dead_zone(pad.rx, pad.ry, g_gp_deadzone_right), dt);
	const Fvector2 at = GetUICursor().GetCursorPosition();
	// Over the map: it's looked for again this frame to move it
	const bool on_map = s_scan.map && s_scan.map_rect.in(at.x, at.y);
	if (on_map && (rs.magnitude() > EPS || pad.lt > 0.5f || pad.rt > 0.5f))
		map = menu_rescan(true, true);
	else
		menu_rescan(false);

	// An edit box taking text: the keyboard, the buttons held when it opens or closes do nothing until released
	if (s_scan.typing != s_osk_open)
	{
		release_all(pad);
		s_osk_open = s_scan.typing;
		s_osk_shift = false;
		s_osk_dir = -1;
	}
	if (s_osk_open)
	{
		update_osk(pad, dt);
		return;
	}

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
		case GP_DPAD_UP:
		case GP_DPAD_DOWN:
		case GP_DPAD_LEFT:
		case GP_DPAD_RIGHT:
			if (down)
			{
				const Fvector2 dirs[4] = {{0.f, -1.f}, {0.f, 1.f}, {-1.f, 0.f}, {1.f, 0.f}};
				cursor_step(dirs[code - GP_DPAD_UP]);
			}
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
			s_cursor = at;
		s_cursor_valid = true;
		float speed = 700.f * g_gp_cursor_speed * powf(m, 1.5f) / m;
		if (target_at(s_cursor) >= 0)
			speed *= 0.6f; // over something it can point at
		move_cursor(Fvector2().set(clampr(s_cursor.x + ls.x * speed * dt, 0.f, UI_BASE_WIDTH),
			clampr(s_cursor.y - ls.y * speed * dt, 0.f, UI_BASE_HEIGHT)));
	}
	else if (s_cursor_valid)
	{
		// Released near a target, off any: onto its middle
		s_cursor_valid = false; // the mouse may move it meanwhile
		menu_rescan(true);
		if (target_at(s_cursor) < 0)
		{
			int best = -1;
			float best_dist = 20.f;
			for (u32 i = 0; i < s_scan.targets.size(); ++i)
			{
				const Frect& r = s_scan.targets[i];
				const float dx = _max(_max(r.x1 - s_cursor.x, s_cursor.x - r.x2), 0.f);
				const float dy = _max(_max(r.y1 - s_cursor.y, s_cursor.y - r.y2), 0.f);
				const float dist = _sqrt(dx * dx + dy * dy);
				if (dist < best_dist)
				{
					best_dist = dist;
					best = int(i);
				}
			}
			if (best >= 0)
			{
				Fvector2 c;
				s_scan.targets[best].getcenter(c);
				move_cursor(c);
			}
		}
	}

	// The map: the right stick moves it, the triggers zoom (toward the cursor, as the mouse wheel does)
	static float zoom_wait = 0.f;
	if (map)
	{
		if (rs.magnitude() > EPS)
			map->MoveMap(Fvector2().set(-rs.x * 600.f * dt, rs.y * 600.f * dt));
		const int zoom = pad.rt > 0.5f ? 1 : (pad.lt > 0.5f ? -1 : 0);
		if (!zoom)
			zoom_wait = 0.f;
		else if ((zoom_wait -= dt) <= 0.f)
		{
			zoom_wait = 0.12f;
			pInput->inject_mouse_wheel(zoom * 120);
		}
		s_wheel_rest = 0.f;
		return;
	}
	zoom_wait = 0.f;

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
			key_release(t.sent);
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
	key_events_update();
	// The gamepad lost while playing with it: the game pauses (the main menu)
	if (s_was_playing && !pad.connected && g_pGameLevel && MainMenu() && !MainMenu()->IsActive())
		Console->Execute("main_menu on");
	s_was_playing = s_mode == mGame && pInput->pad_in_use();
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
			s_was_playing = false; // not a lost gamepad
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
	CMD4(CCC_Float, "gp_aim_slowdown", &g_gp_aim_slowdown, 0.f, 0.8f);
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
	for (int i = 0; i < WHEEL_MAX; ++i)
	{
		xr_delete(s_radial_icons[i]);
		xr_delete(s_radial_labels[i]);
	}
	xr_delete(s_radial_title);
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

void gamepad_wheel_add(LPCSTR label, LPCSTR icon)
{
	if (int(s_action_entries.size()) >= WHEEL_MAX || !label)
		return;
	action_entry e;
	e.label = label;
	if (icon && icon[0] && pSettings->section_exist(icon))
		e.icon = icon;
	s_action_entries.push_back(e);
}

void gamepad_press_key(int dik, int modifier, int taps, float hold)
{
	if (dik <= 0 || is_gamepad_key(dik))
		return;
	u32 t = s_key_events.empty() ? Device.dwTimeContinual : s_key_events.back().at + 20;
	const u32 down_time = hold > 0.f ? u32(hold * 1000.f) : 50;
	if (modifier > 0)
		s_key_events.push_back({modifier, true, t});
	for (int i = 0; i < _max(taps, 1); ++i)
	{
		if (i)
			t += 80;
		s_key_events.push_back({dik, true, t});
		t += down_time;
		s_key_events.push_back({dik, false, t});
	}
	if (modifier > 0)
		s_key_events.push_back({modifier, false, t + 20});
}

void gamepad_menu_target(CUIWindow* w)
{
	if (!w)
		return;
	Frect r;
	w->GetAbsoluteRect(r);
	// Hidden with a parent, or scrolled out of a scroll view: none
	for (CUIWindow* p = w; p; p = p->GetParent())
	{
		if (!p->IsShown())
			return;
		if (p != w && p->ui_cast_scroll_view())
		{
			Frect clip;
			p->GetAbsoluteRect(clip);
			if (!r.intersection(Frect(r), clip))
				return;
		}
	}
	if (r.width() >= 4.f && r.height() >= 4.f)
		s_scan.targets.push_back(r);
}

void gamepad_send_action(int action)
{
	if (action >= 0 && action < kLASTACTION)
		send_action(EGameActions(action));
}

// An item's inventory icon (its section, its grid rect: x, y, width, height in cells) on a static, in a box of
// box_w x box_h UI units with its proportions kept, centred at (x, y)
static void draw_item_icon(CUIStatic* icon, LPCSTR section, const Irect& grid, float x, float y, float box_w, float box_h,
	u32 color, float kx)
{
	if (pSettings->line_exist(section, "icons_texture"))
		icon->SetShader(InventoryUtilities::GetCustomIconTextureShader(pSettings->r_string(section, "icons_texture")));
	else
		icon->SetShader(InventoryUtilities::GetEquipmentIconsShader());
	Frect rect;
	rect.lt.set(float(grid.x1) * INV_GRID_WIDTH, float(grid.y1) * INV_GRID_HEIGHT);
	rect.rb.set(rect.lt.x + float(grid.x2) * INV_GRID_WIDTH, rect.lt.y + float(grid.y2) * INV_GRID_HEIGHT);
	icon->SetTextureRect(rect);
	icon->SetStretchTexture(true);
	icon->TextureOn();
	icon->SetTextureColor(color);
	const float w = rect.width(), h = rect.height();
	const float scale = (w > 0.f && h > 0.f) ? _min(box_w / w, box_h / h) : 1.f;
	const Fvector2 size = {w * scale * kx, h * scale};
	icon->SetWndSize(size);
	icon->SetWndPos(Fvector2().set(x - size.x * 0.5f, y - size.y * 0.5f));
	icon->Draw();
}

// A section's inventory grid rect, as CInventoryItem::GetInvGridRect gives an item's
static Irect section_grid(LPCSTR section)
{
	Irect r;
	r.x1 = READ_IF_EXISTS(pSettings, r_u32, section, "inv_grid_x", 0);
	r.y1 = READ_IF_EXISTS(pSettings, r_u32, section, "inv_grid_y", 0);
	r.x2 = READ_IF_EXISTS(pSettings, r_u32, section, "inv_grid_width", 1);
	r.y2 = READ_IF_EXISTS(pSettings, r_u32, section, "inv_grid_height", 1);
	return r;
}

// The on-screen keyboard: its keys in a dark panel under the edit box (above it near the screen's bottom), the one
// picked amber, shift's keys shown shifted; a line of what the buttons do under it
static void render_osk()
{
	const float kx = UI().get_current_kx();
	const float cell_w = 40.f, cell_h = 34.f, gap = 4.f, pad = 10.f, hint_h = 24.f;
	const float w = OSK_COLS * (cell_w + gap) - gap + pad * 2.f, h = OSK_ROWS * (cell_h + gap) - gap + pad * 2.f + hint_h;
	const Frect& edit = s_scan.edit_rect;
	float y = edit.y2 + 8.f;
	if (y + h > UI_BASE_HEIGHT - 8.f)
		y = _max(8.f, edit.y1 - 8.f - h);
	const float x = UI_BASE_WIDTH * 0.5f - w * 0.5f * kx;

	if (!s_radial_shader)
	{
		s_radial_shader = xr_new<ui_shader>();
		(*s_radial_shader)->create("hud\\crosshair");
	}
	auto quad = [&](float x1, float y1, float x2, float y2, u32 color)
	{
		Fvector2 p[4] = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}};
		for (Fvector2& v : p)
			UI().ClientToScreenScaled(v);
		const int order[6] = {0, 1, 2, 0, 2, 3};
		for (int i : order)
			UIRender->PushPoint(p[i].x, p[i].y, 0.f, color, 0.f, 0.f);
	};
	UIRender->StartPrimitive((OSK_ROWS * OSK_COLS + 2) * 6, IUIRender::ptTriList, UI().m_currentPointType);
	quad(x - 2.f * kx, y - 2.f, x + (w + 2.f) * kx, y + h + 2.f, color_argb(150, 150, 150, 150));
	quad(x, y, x + w * kx, y + h, color_argb(230, 8, 9, 10));
	for (int r = 0; r < OSK_ROWS; ++r)
		for (int c = 0; c < OSK_COLS; ++c)
		{
			const float cx = x + (pad + c * (cell_w + gap)) * kx, cy = y + pad + r * (cell_h + gap);
			const bool picked = r == s_osk_row && c == s_osk_col;
			const bool lit = !s_osk[r][c].dik && s_osk_shift; // shift on
			quad(cx, cy, cx + cell_w * kx, cy + cell_h,
				picked ? color_argb(235, 242, 165, 26) : (lit ? color_argb(220, 90, 70, 30) : color_argb(210, 34, 36, 38)));
		}
	UIRender->SetShader(**s_radial_shader);
	UIRender->FlushPrimitive();

	CGameFont* font = UI().Font().pFontLetterica18Russian;
	font->SetAligment(CGameFont::alCenter);
	for (int r = 0; r < OSK_ROWS; ++r)
		for (int c = 0; c < OSK_COLS; ++c)
		{
			const osk_key& k = s_osk[r][c];
			Fvector2 p = {x + (pad + c * (cell_w + gap) + cell_w * 0.5f) * kx, y + pad + r * (cell_h + gap) + cell_h * 0.5f - 9.f};
			UI().ClientToScreenScaled(p);
			const bool picked = r == s_osk_row && c == s_osk_col;
			font->SetColor(picked ? color_argb(255, 20, 14, 4) : color_argb(255, 225, 225, 225));
			font->Out(p.x, p.y, "%s", s_osk_shift ? k.shifted : k.text);
		}
	string256 hint;
	xr_sprintf(hint, "%s Type   %s Erase   %s Space   %s Shift   %s %s Move   %s Done",
		gamepad_button_text(GP_A - GP_A), gamepad_button_text(GP_B - GP_A), gamepad_button_text(GP_X - GP_A),
		gamepad_button_text(GP_Y - GP_A), gamepad_button_text(GP_LB - GP_A), gamepad_button_text(GP_RB - GP_A),
		gamepad_button_text(GP_START - GP_A));
	CGameFont* hint_font = UI().Font().pFontLetterica16Russian;
	hint_font->SetAligment(CGameFont::alCenter);
	hint_font->SetColor(color_argb(255, 190, 190, 190));
	Fvector2 p = {UI_BASE_WIDTH * 0.5f, y + h - hint_h + 2.f};
	UI().ClientToScreenScaled(p);
	hint_font->Out(p.x, p.y, "%s", hint);
}

// The wheels: their sectors in a ring around the screen's centre (the radial menu eight, the actions wheel as many as
// scripts gave, up to twelve: a wider ring then), opening with a short zoom and fade. Each a dark glass gradient with
// a light rim on its outer edge; the chosen one amber, pushed out, its rim bright; the slot in hands marked on the
// inner edge; a disc in the middle with the chosen one's name and a pointer to the stick's direction. Each sector
// shows its slot's item (icon and name) or what it's for
static void render_wheel()
{
	if (!s_radial_key || !Actor())
		return;
	const int n = radial_count();
	if (n <= 0)
		return;

	const float kx = UI().get_current_kx(); // wide screens: UI x units are narrower
	const Fvector2 c = {UI_BASE_WIDTH * 0.5f, UI_BASE_HEIGHT * 0.5f};
	const float open = clampr(float(Device.dwTimeContinual - s_radial_open_time) / 140.f, 0.f, 1.f);
	const float ease = 1.f - (1.f - open) * (1.f - open) * (1.f - open);
	const float zoom = 0.88f + 0.12f * ease;
	const bool many = n > 8;
	const float r_in = (many ? 84.f : 76.f) * zoom, r_out = (many ? 236.f : 206.f) * zoom;
	const float r_mid = (many ? 160.f : 141.f) * zoom, pop = 10.f * zoom, r_disc = 62.f * zoom;
	const float label_w = many ? 92.f : 130.f, box_w = many ? 80.f : 100.f, box_h = many ? 36.f : 44.f;
	const float sector = PI_MUL_2 / float(n);
	const float gap = 0.022f; // radians between sectors
	const int steps = many ? 8 : 12, disc_steps = 48;
	auto argb = [&](u32 a, u32 r, u32 g, u32 b) { return color_argb(u32(float(a) * ease), r, g, b); };

	CInventory& inv = Actor()->inventory();
	const u16 active_slot = inv.GetActiveSlot();

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
	// A ring's piece between two radii and angles, its colour from the inner edge's to the outer's
	auto band = [&](float r0, float r1, float a_from, float a_to, int count, u32 c0, u32 c1)
	{
		for (int s = 0; s < count; ++s)
		{
			const float a0 = a_from + (a_to - a_from) * float(s) / float(count);
			const float a1 = a_from + (a_to - a_from) * float(s + 1) / float(count);
			point(r0, a0, c0);
			point(r1, a0, c1);
			point(r1, a1, c1);
			point(r0, a0, c0);
			point(r1, a1, c1);
			point(r0, a1, c0);
		}
	};
	UIRender->StartPrimitive(n * steps * 6 * 4 + disc_steps * 6 * 2 + 6, IUIRender::ptTriList,
		UI().m_currentPointType);
	for (int i = 0; i < n; ++i)
	{
		const bool chosen = i == s_radial_sel;
		const float a_from = float(i) * sector - sector * 0.5f + gap;
		const float a_to = float(i) * sector + sector * 0.5f - gap;
		const float out = r_out + (chosen ? pop : 0.f);
		if (chosen)
		{
			band(r_in, out - 4.f, a_from, a_to, steps, argb(150, 150, 92, 14), argb(215, 242, 165, 26));
			band(out - 4.f, out, a_from, a_to, steps, argb(255, 255, 210, 120), argb(255, 255, 210, 120));
		}
		else
		{
			band(r_in, out - 2.f, a_from, a_to, steps, argb(185, 6, 7, 8), argb(140, 30, 32, 34));
			band(out - 2.f, out, a_from, a_to, steps, argb(150, 150, 150, 150), argb(150, 150, 150, 150));
		}
		// The slot in hands: a light strip on the inner edge
		if (!s_radial_actions && s_radial_items[i].slot != NO_ACTIVE_SLOT && s_radial_items[i].slot == active_slot)
			band(r_in, r_in + 4.f, a_from, a_to, steps, argb(230, 235, 235, 235), argb(230, 235, 235, 235));
	}
	// The middle: a dark disc with a faint rim
	band(0.f, r_disc - 2.f, 0.f, PI_MUL_2, disc_steps, argb(170, 4, 4, 5), argb(150, 14, 15, 16));
	band(r_disc - 2.f, r_disc, 0.f, PI_MUL_2, disc_steps, argb(110, 150, 150, 150), argb(110, 150, 150, 150));
	// Its pointer: a small triangle at the disc's edge toward the stick
	if (s_radial_sel >= 0)
	{
		const u32 tip = argb(255, 255, 200, 90);
		point(r_disc + 9.f, s_radial_dir, tip);
		point(r_disc + 1.f, s_radial_dir - 0.09f, tip);
		point(r_disc + 1.f, s_radial_dir + 0.09f, tip);
	}
	UIRender->SetShader(**s_radial_shader);
	UIRender->FlushPrimitive();

	// A sector's name and icon: its slot's item or what it's for (the radial menu), or a script's entry
	struct view
	{
		shared_str text;
		LPCSTR section = nullptr; // the icon's
		Irect grid;
		bool dim = false; // an empty slot
	};
	auto sector_view = [&](int i)
	{
		view v;
		if (s_radial_actions)
		{
			const action_entry& e = s_action_entries[i];
			v.text = CStringTable().translate(e.label.c_str());
			if (e.icon.size())
			{
				v.section = e.icon.c_str();
				v.grid = section_grid(v.section);
			}
			return v;
		}
		const radial_item& item = s_radial_items[i];
		PIItem itm = item.slot != NO_ACTIVE_SLOT ? inv.ItemFromSlot(item.slot) : nullptr;
		v.text = itm ? shared_str(itm->NameShort()) : CStringTable().translate(item.label);
		if (itm)
		{
			v.section = itm->m_section_id.c_str();
			v.grid = itm->GetInvGridRect();
		}
		v.dim = !itm && item.slot != NO_ACTIVE_SLOT;
		return v;
	};

	for (int i = 0; i < n; ++i)
	{
		const bool chosen = i == s_radial_sel;
		const view v = sector_view(i);
		const float r = r_mid + (chosen ? pop * 0.5f : 0.f);
		const Fvector2 at = {c.x + _sin(float(i) * sector) * r * kx, c.y - _cos(float(i) * sector) * r};

		if (!s_radial_labels[i])
		{
			s_radial_labels[i] = xr_new<CUITextWnd>();
			s_radial_labels[i]->SetFont(UI().Font().pFontLetterica16Russian);
			s_radial_labels[i]->SetTextAlignment(CGameFont::alCenter);
			s_radial_labels[i]->SetTextComplexMode(true); // a long name on two lines
			s_radial_icons[i] = xr_new<CUIStatic>();
		}
		CUITextWnd* label = s_radial_labels[i];
		label->SetWndSize(Fvector2().set(label_w * kx, 36.f));
		label->SetText(v.text.c_str());
		label->SetTextColor(chosen ? argb(255, 255, 255, 255) : (v.dim ? argb(255, 110, 110, 110) : argb(255, 215, 215, 215)));
		// Without an icon the name takes the sector's middle
		const float label_y = v.section ? at.y + (many ? 18.f : 24.f) * zoom : at.y - 9.f;
		label->SetWndPos(Fvector2().set(at.x - label_w * 0.5f * kx, label_y));
		label->Draw();

		if (!v.section)
			continue;
		// The icon (the chosen one's a tenth larger)
		const float box = zoom * (chosen ? 1.1f : 1.f);
		draw_item_icon(s_radial_icons[i], v.section, v.grid, at.x, at.y - 6.f * zoom, box_w * box, box_h * box,
			argb(255, 255, 255, 255), kx);
	}

	// The chosen one's name in the middle
	if (s_radial_sel >= 0 && s_radial_sel < n)
	{
		if (!s_radial_title)
		{
			s_radial_title = xr_new<CUITextWnd>();
			s_radial_title->SetFont(UI().Font().pFontLetterica18Russian);
			s_radial_title->SetTextAlignment(CGameFont::alCenter);
			s_radial_title->SetVTextAlignment(valCenter);
			s_radial_title->SetTextComplexMode(true); // a long name on two lines
		}
		s_radial_title->SetWndSize(Fvector2().set(2.f * (r_disc - 8.f) * kx, 40.f));
		s_radial_title->SetWndPos(Fvector2().set(c.x - (r_disc - 8.f) * kx, c.y - 20.f));
		s_radial_title->SetText(sector_view(s_radial_sel).text.c_str());
		s_radial_title->SetTextColor(argb(255, 255, 214, 140));
		s_radial_title->Draw();
	}
}

void gamepad_render_ui(bool main_menu)
{
	// Drawn over the main menu's dialogs or the level's, not both
	if (main_menu != (MainMenu() && MainMenu()->IsActive()))
		return;
	if (s_osk_open && s_mode == mCursor)
		render_osk();
	if (!main_menu)
		render_wheel();
}
