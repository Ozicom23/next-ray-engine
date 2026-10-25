#pragma once

// Gamepad play (XInput, read by CInput): in the game its buttons are keys bound by gp_bind (xr_level_controller.cpp),
// the left stick moves, the right one turns the camera; in menus and dialogs the left stick moves the cursor, A and X
// click, B closes; a radial menu for the weapon slots and devices

// Console commands (gp_*)
void gamepad_register_commands();
// CGamePersistent::OnAppStart / OnAppEnd: the handler CInput calls every frame
void gamepad_start();
void gamepad_stop();
// The wheel over the HUD (CHUDManager::RenderUI) and the on-screen keyboard over the level's dialogs or the main
// menu's (CMainMenu, main_menu true)
void gamepad_render_ui(bool main_menu);
// The left stick for the actor's movement, after the dead zone: x right, y forward, length 0..1. False when the
// gamepad doesn't move the actor
bool gamepad_move(Fvector2& move);
// The radial menu is open: the right stick chooses, the camera doesn't turn
bool gamepad_radial_open();
// Rumble: low (heavy) and high (light) motors 0..1 for time seconds, scaled by gp_vibration
void gamepad_rumble(float low, float high, float time);

// For scripts (the wheel's buttons, zzzz_nextray_gamepad.script):
// a button's action while the wheel is held, as it opens: the button (gpX, gpDPAD_UP...) and its name (a string id or
// the text)
void gamepad_wheel_add(LPCSTR button, LPCSTR label);
// a keyboard key pressed (a mod's key): with its modifier key held (0 none), taps times, held hold seconds (0: a tap)
void gamepad_press_key(int dik, int modifier, int taps, float hold);
// an action pressed as if by its key (key_bindings.k...)
void gamepad_send_action(int action);
class CUIWindow;
// a window the menu cursor can point at, beside the buttons, cells and rows it finds itself (a script's item cell;
// from zzzz_nextray_gamepad.menu_targets, called as the menu is looked at)
void gamepad_menu_target(CUIWindow* w);
