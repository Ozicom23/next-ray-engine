#pragma once

// Gamepad play (XInput, read by CInput): in the game its buttons are keys bound by gp_bind (xr_level_controller.cpp),
// the left stick moves, the right one turns the camera; in menus and dialogs the left stick moves the cursor, A and X
// click, B closes; a radial menu for the weapon slots and devices

// Console commands (gp_*)
void gamepad_register_commands();
// CGamePersistent::OnAppStart / OnAppEnd: the handler CInput calls every frame
void gamepad_start();
void gamepad_stop();
// CHUDManager::RenderUI: the radial menu over the HUD
void gamepad_render_ui();
// The left stick for the actor's movement, after the dead zone: x right, y forward, length 0..1. False when the
// gamepad doesn't move the actor
bool gamepad_move(Fvector2& move);
// The radial menu is open: the right stick chooses, the camera doesn't turn
bool gamepad_radial_open();
// Rumble: low (heavy) and high (light) motors 0..1 for time seconds, scaled by gp_vibration
void gamepad_rumble(float low, float high, float time);
