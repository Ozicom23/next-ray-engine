#include "stdafx.h"
#include <dinput.h>
#include "../xrEngine/xr_ioconsole.h"
#include "../xrEngine/xr_input.h"
#include "../xrEngine/xr_ioc_cmd.h"
#include "../xrEngine/gamepad_glyphs.h"
#include "xr_level_controller.h"
#include "string_table.h"

_binding g_key_bindings [bindings_count];
_key_group g_current_keygroup = _sp;

_action actions[] = {
	{"left", kLEFT, _both},
	{"right", kRIGHT, _both},
	{"up", kUP, _both},
	{"down", kDOWN, _both},
	{"jump", kJUMP, _both},
	{"crouch", kCROUCH, _both},
	{"accel", kACCEL, _both},
	{"sprint_toggle", kSPRINT_TOGGLE, _both},

	{"forward", kFWD, _both},
	{"back", kBACK, _both},
	{"lstrafe", kL_STRAFE, _both},
	{"rstrafe", kR_STRAFE, _both},

	{"llookout", kL_LOOKOUT, _both},
	{"rlookout", kR_LOOKOUT, _both},

	{"cam_1", kCAM_1, _both},
	{"cam_2", kCAM_2, _both},
	{"cam_3", kCAM_3, _both},
	{"cam_zoom_in", kCAM_ZOOM_IN, _both},
	{"cam_zoom_out", kCAM_ZOOM_OUT, _both},

	{"torch", kTORCH, _both},
	{"night_vision", kNIGHT_VISION, _both},
	{"show_detector", kDETECTOR, _sp},

	{"wpn_1", kWPN_1, _both},
	{"wpn_2", kWPN_2, _both},
	{"wpn_3", kWPN_3, _both},
	{"wpn_4", kWPN_4, _both},
	{"wpn_5", kWPN_5, _both},
	{"wpn_6", kWPN_6, _both},
	{"artefact", kARTEFACT, _both/*_mp*/},
	{"wpn_next", kWPN_NEXT, _both}, // means next ammo type
	{"wpn_fire", kWPN_FIRE, _both},
	{"wpn_zoom", kWPN_ZOOM, _both},
	{"wpn_zoom_inc", kWPN_ZOOM_INC, _both},
	{"wpn_zoom_dec", kWPN_ZOOM_DEC, _both},
	{"wpn_reload", kWPN_RELOAD, _both},
	{"wpn_func", kWPN_FUNC, _both},
	{"wpn_firemode_prev", kWPN_FIREMODE_PREV, _both},
	{"wpn_firemode_next", kWPN_FIREMODE_NEXT, _both},

	{"pause", kPAUSE, _both},
	{"drop", kDROP, _both},
	{"use", kUSE, _both},
	{"scores", kSCORES, _both},
	{"chat", kCHAT, _both},
	{"chat_team", kCHAT_TEAM, _both},
	{"screenshot", kSCREENSHOT, _both},
	{"quit", kQUIT, _both},
	{"console", kCONSOLE, _both},
	{"inventory", kINVENTORY, _both},
	{"buy_menu", kBUY, _both},
	{"skin_menu", kSKIN, _both},
	{"team_menu", kTEAM, _both},
	{"active_jobs", kACTIVE_JOBS, _sp},

	{"vote_begin", kVOTE_BEGIN, _both},
	{"show_admin_menu", kSHOW_ADMIN_MENU, _both},
	{"vote", kVOTE, _both},
	{"vote_yes", kVOTEYES, _both},
	{"vote_no", kVOTENO, _both},

	{"next_slot", kNEXT_SLOT, _both},
	{"prev_slot", kPREV_SLOT, _both},

	{"speech_menu_0", kSPEECH_MENU_0, _both},
	{"speech_menu_1", kSPEECH_MENU_1, _both},

	{"quick_use_1", kQUICK_USE_1, _both},
	{"quick_use_2", kQUICK_USE_2, _both},
	{"quick_use_3", kQUICK_USE_3, _both},
	{"quick_use_4", kQUICK_USE_4, _both},

	{"quick_save", kQUICK_SAVE, _sp},
	{"quick_load", kQUICK_LOAD, _sp},
	//	{ "alife_command",		kALIFE_CMD				,_sp},		
	{"custom1", kCUSTOM1, _sp},
	{"custom2", kCUSTOM2, _sp},
	{"custom3", kCUSTOM3, _sp},
	{"custom4", kCUSTOM4, _sp},
	{"custom5", kCUSTOM5, _sp},
	{"custom6", kCUSTOM6, _sp},
	{"custom7", kCUSTOM7, _sp},
	{"custom8", kCUSTOM8, _sp},
	{"custom9", kCUSTOM9, _sp},
	{"custom10", kCUSTOM10, _sp},
	{"custom11", kCUSTOM11, _sp},
	{"custom12", kCUSTOM12, _sp},
	{"custom13", kCUSTOM13, _sp},
	{"custom14", kCUSTOM14, _sp},
	{"custom15", kCUSTOM15, _sp},
	{"custom16", kCUSTOM16, _sp },
	{"custom17", kCUSTOM17, _sp },
	{"custom18", kCUSTOM18, _sp },
	{"custom19", kCUSTOM19, _sp },
	{"custom20", kCUSTOM20, _sp },
	{"custom21", kCUSTOM21, _sp },
	{"custom22", kCUSTOM22, _sp },
	{"custom23", kCUSTOM23, _sp },
	{"custom24", kCUSTOM24, _sp },
	{"custom25", kCUSTOM25, _sp },
	{"safemode", kSAFEMODE, _both},
	{"freelook", kFREELOOK, _both},
	{"cam_autoaim", kCAM_AUTOAIM, _sp},
	{"editor", kEDITOR, _both},
	{"use_or_reload", kUSE_OR_RELOAD, _sp},
	{"wpn_swap", kWPN_SWAP, _sp},
	{"wpn_holster", kWPN_HOLSTER, _sp},
	{"radial_menu", kRADIAL_MENU, _sp},
	{"action_wheel", kACTION_WHEEL, _sp},
	{NULL, kLASTACTION, _both}
};

_keyboard keyboards[] = {
	{"kESCAPE", DIK_ESCAPE}, {"k1", DIK_1},
	{"k2", DIK_2}, {"k3", DIK_3},
	{"k4", DIK_4}, {"k5", DIK_5},
	{"k6", DIK_6}, {"k7", DIK_7},
	{"k8", DIK_8}, {"k9", DIK_9},
	{"k0", DIK_0}, {"kMINUS", DIK_MINUS},
	{"kEQUALS", DIK_EQUALS}, {"kBACK", DIK_BACK},
	{"kTAB", DIK_TAB}, {"kQ", DIK_Q},
	{"kW", DIK_W}, {"kE", DIK_E},
	{"kR", DIK_R}, {"kT", DIK_T},
	{"kY", DIK_Y}, {"kU", DIK_U},
	{"kI", DIK_I}, {"kO", DIK_O},
	{"kP", DIK_P}, {"kLBRACKET", DIK_LBRACKET},
	{"kRBRACKET", DIK_RBRACKET}, {"kRETURN", DIK_RETURN},
	{"kLCONTROL", DIK_LCONTROL}, {"kA", DIK_A},
	{"kS", DIK_S}, {"kD", DIK_D},
	{"kF", DIK_F}, {"kG", DIK_G},
	{"kH", DIK_H}, {"kJ", DIK_J},
	{"kK", DIK_K}, {"kL", DIK_L},
	{"kSEMICOLON", DIK_SEMICOLON}, {"kAPOSTROPHE", DIK_APOSTROPHE},
	{"kGRAVE", DIK_GRAVE}, {"kLSHIFT", DIK_LSHIFT},
	{"kBACKSLASH", DIK_BACKSLASH}, {"kZ", DIK_Z},
	{"kX", DIK_X}, {"kC", DIK_C},
	{"kV", DIK_V}, {"kB", DIK_B},
	{"kN", DIK_N}, {"kM", DIK_M},
	{"kCOMMA", DIK_COMMA}, {"kPERIOD", DIK_PERIOD},
	{"kSLASH", DIK_SLASH}, {"kRSHIFT", DIK_RSHIFT},
	{"kMULTIPLY", DIK_MULTIPLY}, {"kLMENU", DIK_LMENU},
	{"kSPACE", DIK_SPACE}, {"kCAPITAL", DIK_CAPITAL},
	{"kF1", DIK_F1}, {"kF2", DIK_F2},
	{"kF3", DIK_F3}, {"kF4", DIK_F4},
	{"kF5", DIK_F5}, {"kF6", DIK_F6},
	{"kF7", DIK_F7}, {"kF8", DIK_F8},
	{"kF9", DIK_F9}, {"kF10", DIK_F10},
	{"kNUMLOCK", DIK_NUMLOCK}, {"kSCROLL", DIK_SCROLL},
	{"kNUMPAD7", DIK_NUMPAD7}, {"kNUMPAD8", DIK_NUMPAD8},
	{"kNUMPAD9", DIK_NUMPAD9}, {"kSUBTRACT", DIK_SUBTRACT},
	{"kNUMPAD4", DIK_NUMPAD4}, {"kNUMPAD5", DIK_NUMPAD5},
	{"kNUMPAD6", DIK_NUMPAD6}, {"kADD", DIK_ADD},
	{"kNUMPAD1", DIK_NUMPAD1}, {"kNUMPAD2", DIK_NUMPAD2},
	{"kNUMPAD3", DIK_NUMPAD3}, {"kNUMPAD0", DIK_NUMPAD0},
	{"kDECIMAL", DIK_DECIMAL}, {"kF11", DIK_F11},
	{"kF12", DIK_F12}, {"kF13", DIK_F13},
	{"kF14", DIK_F14}, {"kF15", DIK_F15},
	{"kKANA", DIK_KANA}, {"kCONVERT", DIK_CONVERT},
	{"kNOCONVERT", DIK_NOCONVERT}, {"kYEN", DIK_YEN},
	{"kNUMPADEQUALS", DIK_NUMPADEQUALS}, {"kCIRCUMFLEX", DIK_CIRCUMFLEX},
	{"kAT", DIK_AT}, {"kCOLON", DIK_COLON},
	{"kUNDERLINE", DIK_UNDERLINE}, {"kKANJI", DIK_KANJI},
	{"kSTOP", DIK_STOP}, {"kAX", DIK_AX},
	{"kUNLABELED", DIK_UNLABELED}, {"kNUMPADENTER", DIK_NUMPADENTER},
	{"kRCONTROL", DIK_RCONTROL}, {"kNUMPADCOMMA", DIK_NUMPADCOMMA},
	{"kDIVIDE", DIK_DIVIDE}, {"kSYSRQ", DIK_SYSRQ},
	{"kRMENU", DIK_RMENU}, {"kHOME", DIK_HOME},
	{"kUP", DIK_UP}, {"kPRIOR", DIK_PRIOR},
	{"kLEFT", DIK_LEFT}, {"kRIGHT", DIK_RIGHT},
	{"kEND", DIK_END}, {"kDOWN", DIK_DOWN},
	{"kNEXT", DIK_NEXT}, {"kINSERT", DIK_INSERT},
	{"kDELETE", DIK_DELETE}, {"kLWIN", DIK_LWIN},
	{"kRWIN", DIK_RWIN}, {"kAPPS", DIK_APPS},
	{"kPAUSE", DIK_PAUSE}, {"mouse1", MOUSE_1},
	{"mouse2", MOUSE_2}, {"mouse3", MOUSE_3},
	{"mouse4", MOUSE_4}, {"mouse5", MOUSE_5},
	{"mouse6", MOUSE_6}, {"mouse7", MOUSE_7},
	{"mouse8", MOUSE_8}, 
	// Gamepad keys (xr_input.h): the buttons, held (_hold), pressed while LT is held (_aim), the left stick's directions
	{"gpA", GP_A},
	{"gpB", GP_B},
	{"gpX", GP_X},
	{"gpY", GP_Y},
	{"gpLB", GP_LB},
	{"gpRB", GP_RB},
	{"gpLT", GP_LT},
	{"gpRT", GP_RT},
	{"gpBACK", GP_BACK},
	{"gpSTART", GP_START},
	{"gpLS", GP_LS},
	{"gpRS", GP_RS},
	{"gpDPAD_UP", GP_DPAD_UP},
	{"gpDPAD_DOWN", GP_DPAD_DOWN},
	{"gpDPAD_LEFT", GP_DPAD_LEFT},
	{"gpDPAD_RIGHT", GP_DPAD_RIGHT},
	{"gpA_hold", GP_A + GP_HOLD},
	{"gpB_hold", GP_B + GP_HOLD},
	{"gpX_hold", GP_X + GP_HOLD},
	{"gpY_hold", GP_Y + GP_HOLD},
	{"gpLB_hold", GP_LB + GP_HOLD},
	{"gpRB_hold", GP_RB + GP_HOLD},
	{"gpLT_hold", GP_LT + GP_HOLD},
	{"gpRT_hold", GP_RT + GP_HOLD},
	{"gpBACK_hold", GP_BACK + GP_HOLD},
	{"gpSTART_hold", GP_START + GP_HOLD},
	{"gpLS_hold", GP_LS + GP_HOLD},
	{"gpRS_hold", GP_RS + GP_HOLD},
	{"gpDPAD_UP_hold", GP_DPAD_UP + GP_HOLD},
	{"gpDPAD_DOWN_hold", GP_DPAD_DOWN + GP_HOLD},
	{"gpDPAD_LEFT_hold", GP_DPAD_LEFT + GP_HOLD},
	{"gpDPAD_RIGHT_hold", GP_DPAD_RIGHT + GP_HOLD},
	{"gpA_aim", GP_A + GP_AIM},
	{"gpB_aim", GP_B + GP_AIM},
	{"gpX_aim", GP_X + GP_AIM},
	{"gpY_aim", GP_Y + GP_AIM},
	{"gpLB_aim", GP_LB + GP_AIM},
	{"gpRB_aim", GP_RB + GP_AIM},
	{"gpRT_aim", GP_RT + GP_AIM},
	{"gpBACK_aim", GP_BACK + GP_AIM},
	{"gpSTART_aim", GP_START + GP_AIM},
	{"gpLS_aim", GP_LS + GP_AIM},
	{"gpRS_aim", GP_RS + GP_AIM},
	{"gpDPAD_UP_aim", GP_DPAD_UP + GP_AIM},
	{"gpDPAD_DOWN_aim", GP_DPAD_DOWN + GP_AIM},
	{"gpDPAD_LEFT_aim", GP_DPAD_LEFT + GP_AIM},
	{"gpDPAD_RIGHT_aim", GP_DPAD_RIGHT + GP_AIM},
	{"gpLS_up", GP_LS_UP},
	{"gpLS_down", GP_LS_DOWN},
	{"gpLS_left", GP_LS_LEFT},
	{"gpLS_right", GP_LS_RIGHT},
	{"gpVIRTUAL", GP_VIRTUAL},

	// RavenAscendant: additional 100 entries, will let the bind console command bind actions to values that won't get triggered by keyboard input
	// but can be triggered from the level.press_action functions
	{"kCUSTOM0", 1000},
	{"kCUSTOM1", 1001},
	{"kCUSTOM2", 1002},
	{"kCUSTOM3", 1003},
	{"kCUSTOM4", 1004},
	{"kCUSTOM5", 1005},
	{"kCUSTOM6", 1006},
	{"kCUSTOM7", 1007},
	{"kCUSTOM8", 1008},
	{"kCUSTOM9", 1009},
	{"kCUSTOM10", 1010},
	{"kCUSTOM11", 1011},
	{"kCUSTOM12", 1012},
	{"kCUSTOM13", 1013},
	{"kCUSTOM14", 1014},
	{"kCUSTOM15", 1015},
	{"kCUSTOM16", 1016},
	{"kCUSTOM17", 1017},
	{"kCUSTOM18", 1018},
	{"kCUSTOM19", 1019},
	{"kCUSTOM20", 1020},
	{"kCUSTOM21", 1021},
	{"kCUSTOM22", 1022},
	{"kCUSTOM23", 1023},
	{"kCUSTOM24", 1024},
	{"kCUSTOM25", 1025},
	{"kCUSTOM26", 1026},
	{"kCUSTOM27", 1027},
	{"kCUSTOM28", 1028},
	{"kCUSTOM29", 1029},
	{"kCUSTOM30", 1030},
	{"kCUSTOM31", 1031},
	{"kCUSTOM32", 1032},
	{"kCUSTOM33", 1033},
	{"kCUSTOM34", 1034},
	{"kCUSTOM35", 1035},
	{"kCUSTOM36", 1036},
	{"kCUSTOM37", 1037},
	{"kCUSTOM38", 1038},
	{"kCUSTOM39", 1039},
	{"kCUSTOM40", 1040},
	{"kCUSTOM41", 1041},
	{"kCUSTOM42", 1042},
	{"kCUSTOM43", 1043},
	{"kCUSTOM44", 1044},
	{"kCUSTOM45", 1045},
	{"kCUSTOM46", 1046},
	{"kCUSTOM47", 1047},
	{"kCUSTOM48", 1048},
	{"kCUSTOM49", 1049},
	{"kCUSTOM50", 1050},
	{"kCUSTOM51", 1051},
	{"kCUSTOM52", 1052},
	{"kCUSTOM53", 1053},
	{"kCUSTOM54", 1054},
	{"kCUSTOM55", 1055},
	{"kCUSTOM56", 1056},
	{"kCUSTOM57", 1057},
	{"kCUSTOM58", 1058},
	{"kCUSTOM59", 1059},
	{"kCUSTOM60", 1060},
	{"kCUSTOM61", 1061},
	{"kCUSTOM62", 1062},
	{"kCUSTOM63", 1063},
	{"kCUSTOM64", 1064},
	{"kCUSTOM65", 1065},
	{"kCUSTOM66", 1066},
	{"kCUSTOM67", 1067},
	{"kCUSTOM68", 1068},
	{"kCUSTOM69", 1069},
	{"kCUSTOM70", 1070},
	{"kCUSTOM71", 1071},
	{"kCUSTOM72", 1072},
	{"kCUSTOM73", 1073},
	{"kCUSTOM74", 1074},
	{"kCUSTOM75", 1075},
	{"kCUSTOM76", 1076},
	{"kCUSTOM77", 1077},
	{"kCUSTOM78", 1078},
	{"kCUSTOM79", 1079},
	{"kCUSTOM80", 1080},
	{"kCUSTOM81", 1081},
	{"kCUSTOM82", 1082},
	{"kCUSTOM83", 1083},
	{"kCUSTOM84", 1084},
	{"kCUSTOM85", 1085},
	{"kCUSTOM86", 1086},
	{"kCUSTOM87", 1087},
	{"kCUSTOM88", 1088},
	{"kCUSTOM89", 1089},
	{"kCUSTOM90", 1090},
	{"kCUSTOM91", 1091},
	{"kCUSTOM92", 1092},
	{"kCUSTOM93", 1093},
	{"kCUSTOM94", 1094},
	{"kCUSTOM95", 1095},
	{"kCUSTOM96", 1096},
	{"kCUSTOM97", 1097},
	{"kCUSTOM98", 1098},
	{"kCUSTOM99", 1099},
	{NULL, 0}
};

void initialize_bindings()
{
#ifdef DEBUG
	int i1=0;
	while(true)
	{
		_keyboard& _k1			= keyboards[i1];
		if(_k1.key_name==NULL)	break;
		int i2 = i1;
		while (true)
		{
			_keyboard& _k2			= keyboards[i2];
			if(_k2.key_name==NULL)	break;
			if(_k1.dik==_k2.dik && i1!=i2)
			{
				Msg("%s==%s",_k1.key_name,_k2.key_name);
			}
			++i2;
		}
		++i1;
	}
#endif

	for (int idx = 0; idx < bindings_count; ++idx)
		g_key_bindings[idx].m_action = &actions[idx];
}

bool g_gamepad_playstation = false;

// A gamepad button's name, an Xbox controller's or a PlayStation one's
static LPCSTR gamepad_button_name(int b)
{
	static LPCSTR xbox[GP_BUTTON_COUNT] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "L3", "R3",
		"D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
	static LPCSTR playstation[GP_BUTTON_COUNT] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2",
		"Create", "Options", "L3", "R3", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
	return (g_gamepad_playstation ? playstation : xbox)[b];
}

extern int g_gp_glyphs;

LPCSTR gamepad_button_text(int b)
{
	static char glyph[GP_BUTTON_COUNT][2];
	if (!g_gp_glyphs)
		return gamepad_button_name(b);
	glyph[b][0] = gamepad_glyphs::to_char(b);
	glyph[b][1] = 0;
	return glyph[b];
}

// "Pad A", "Pad A (hold)", "Pad LT + A", "Pad left stick up"
static bool gamepad_key_name(int dik, LPSTR dest, int dest_sz)
{
	static LPCSTR stick[4] = {"up", "down", "left", "right"};
	auto names = [](int b) { return gamepad_button_name(b); };
	if (dik >= GP_LS_UP && dik <= GP_LS_RIGHT)
		xr_sprintf(dest, dest_sz, "Pad left stick %s", stick[dik - GP_LS_UP]);
	else if (dik >= GP_A + GP_AIM && dik < GP_A + GP_AIM + GP_BUTTON_COUNT)
		xr_sprintf(dest, dest_sz, "Pad %s + %s", names(GP_LT - GP_A), names(dik - GP_A - GP_AIM));
	else if (dik >= GP_A + GP_HOLD && dik < GP_A + GP_HOLD + GP_BUTTON_COUNT)
		xr_sprintf(dest, dest_sz, "Pad %s (hold)", names(dik - GP_A - GP_HOLD));
	else if (dik >= GP_A && dik < GP_A + GP_BUTTON_COUNT)
		xr_sprintf(dest, dest_sz, "Pad %s", names(dik - GP_A));
	else
		return false;
	return true;
}

void remap_keys()
{
	int idx = 0;
	string128 buff;
	while (keyboards[idx].key_name)
	{
		buff[0] = 0;
		_keyboard& kb = keyboards[idx];
		bool res = is_gamepad_key(kb.dik) ? gamepad_key_name(kb.dik, buff, sizeof(buff)) :
			pInput->get_dik_name(kb.dik, buff, sizeof(buff));
		if (res)
			kb.key_local_name = buff;
		else
			kb.key_local_name = kb.key_name;

		//.		Msg("[%s]-[%s]",kb.key_name, kb.key_local_name.c_str());
		++idx;
	}
}

LPCSTR id_to_action_name(EGameActions _id)
{
	int idx = 0;
	while (actions[idx].action_name)
	{
		if (_id == actions[idx].id)
			return actions[idx].action_name;
		++idx;
	}
	Msg("! cant find corresponding [action_name] for id");
	return NULL;
}

EGameActions action_name_to_id(LPCSTR _name)
{
	_action* action = action_name_to_ptr(_name);
	if (action)
		return action->id;
	else
		return kNOTBINDED;
}

_action* action_name_to_ptr(LPCSTR _name)
{
	int idx = 0;
	while (actions[idx].action_name)
	{
		if (!stricmp(_name, actions[idx].action_name))
			return &actions[idx];
		++idx;
	}
	Msg ("! cant find corresponding [id] for action_name %s", _name);
	return NULL;
}

LPCSTR dik_to_keyname(int _dik, bool bLocalize)
{
	_keyboard* kb = dik_to_ptr(_dik, true);
	if (kb)
		if (bLocalize)
			return kb->key_local_name.c_str();
		else
			return kb->key_name;
	else
		return NULL;
}

_keyboard* dik_to_ptr(int _dik, bool bSafe)
{
	int idx = 0;
	while (keyboards[idx].key_name)
	{
		_keyboard& kb = keyboards[idx];
		if (kb.dik == _dik)
			return &keyboards[idx];
		++idx;
	}
	if (!bSafe)
		Msg("! cant find corresponding [_keyboard] for dik");
	return NULL;
}

int keyname_to_dik(LPCSTR _name)
{
	_keyboard* _kb = keyname_to_ptr(_name);
	return _kb->dik;
}

_keyboard* keyname_to_ptr(LPCSTR _name)
{
	int idx = 0;
	while (keyboards[idx].key_name)
	{
		_keyboard& kb = keyboards[idx];
		if (!stricmp(_name, kb.key_name))
			return &keyboards[idx];
		++idx;
	}

	Msg("! cant find corresponding [_keyboard*] for keyname %s", _name);
	return NULL;
}

bool is_group_not_conflicted(_key_group g1, _key_group g2)
{
	return ((g1 == _sp && g2 == _mp) || (g1 == _mp && g2 == _sp));
}

bool is_group_matching(_key_group g1, _key_group g2)
{
	return ((g1 == g2) || (g1 == _both) || (g2 == _both));
}

bool is_binded(EGameActions _action_id, int _dik)
{
	if (is_gamepad_key(_dik) && gamepad_binding(_dik) != kNOTBINDED)
		return gamepad_binding(_dik) == _action_id;

	_binding* pbinding = &g_key_bindings[_action_id];
	if (pbinding->m_keyboard[0] && pbinding->m_keyboard[0]->dik == _dik)
		return true;

	if (pbinding->m_keyboard[1] && pbinding->m_keyboard[1]->dik == _dik)
		return true;

	return false;
}

int get_action_dik(EGameActions _action_id, int idx)
{
	_binding* pbinding = &g_key_bindings[_action_id];

	if (idx == -1)
	{
		if (pbinding->m_keyboard[0])
			return pbinding->m_keyboard[0]->dik;

		if (pbinding->m_keyboard[1])
			return pbinding->m_keyboard[1]->dik;
	}
	else
	{
		if (pbinding->m_keyboard[idx])
			return pbinding->m_keyboard[idx]->dik;
	}
	return 0;
}

EGameActions get_binded_action(int _dik)
{
	if (is_gamepad_key(_dik) && gamepad_binding(_dik) != kNOTBINDED)
		return gamepad_binding(_dik);

	for (int idx = 0; idx < bindings_count; ++idx)
	{
		_binding* binding = &g_key_bindings[idx];

		bool b_is_group_matching = is_group_matching(binding->m_action->key_group, g_current_keygroup);

		if (!b_is_group_matching) continue;

		if (binding->m_keyboard[0] && binding->m_keyboard[0]->dik == _dik && b_is_group_matching)
			return binding->m_action->id;

		if (binding->m_keyboard[1] && binding->m_keyboard[1]->dik == _dik && b_is_group_matching)
			return binding->m_action->id;
	}
	return kNOTBINDED;
}

bool g_gamepad_prompts = false;
int g_gp_glyphs = 1; // prompts show the buttons' glyphs (gamepad_glyphs.h), else their names

// The gamepad key that does the action, as a prompt names it: "X", "Hold Y", "LT + LB" (or their glyphs). Buttons
// first, then while aiming, then held, then the left stick; an action decided at the press counts for the actions it
// can be (use or reload)
static bool gamepad_action_prompt(EGameActions action, LPSTR dest, int dest_sz)
{
	static char glyph[GP_BUTTON_COUNT + 1][2];
	auto names = [](int b) -> LPCSTR
	{
		if (!g_gp_glyphs)
			return gamepad_button_name(b);
		glyph[b][0] = gamepad_glyphs::to_char(b);
		glyph[b][1] = 0;
		return glyph[b];
	};
	auto does = [action](EGameActions bound)
	{
		return bound == action ||
			(bound == kUSE_OR_RELOAD && (action == kUSE || action == kWPN_RELOAD));
	};
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
		if (does(gamepad_binding(GP_A + b)))
			return xr_sprintf(dest, dest_sz, "%s", names(b)), true;
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
		if (does(gamepad_binding(GP_A + GP_AIM + b)))
			return xr_sprintf(dest, dest_sz, g_gp_glyphs ? "%s+%s" : "%s + %s", names(GP_LT - GP_A), names(b)), true;
	for (int b = 0; b < GP_BUTTON_COUNT; ++b)
		if (does(gamepad_binding(GP_A + GP_HOLD + b)))
			return xr_sprintf(dest, dest_sz, "Hold %s", names(b)), true;
	for (int key = GP_LS_UP; key <= GP_LS_RIGHT; ++key)
		if (does(gamepad_binding(key)))
			return g_gp_glyphs ? xr_sprintf(dest, dest_sz, "%c", gamepad_glyphs::to_char(gamepad_glyphs::left_stick)) :
				xr_sprintf(dest, dest_sz, "Left Stick"), true;
	return false;
}

void GetActionAllBinding(LPCSTR _action, char* dst_buff, int dst_buff_sz)
{
	int action_id = action_name_to_id(_action);
	if (g_gamepad_prompts && action_id != kNOTBINDED &&
		gamepad_action_prompt(EGameActions(action_id), dst_buff, dst_buff_sz))
		return;
	_binding* pbinding = &g_key_bindings[action_id];

	string128 prim;
	string128 sec;
	prim[0] = 0;
	sec[0] = 0;

	if (pbinding->m_keyboard[0])
	{
		xr_strcpy(prim, pbinding->m_keyboard[0]->key_local_name.c_str());
	}
	if (pbinding->m_keyboard[1])
	{
		xr_strcpy(sec, pbinding->m_keyboard[1]->key_local_name.c_str());
	}
	if (NULL == pbinding->m_keyboard[0] && NULL == pbinding->m_keyboard[1])
	{
		xr_sprintf(dst_buff, dst_buff_sz, "%s", CStringTable().translate("st_key_notbinded").c_str());
	}
	else
		xr_sprintf(dst_buff, dst_buff_sz, "%s%s%s", prim[0] ? prim : "", (sec[0] && prim[0]) ? " , " : "",
		           sec[0] ? sec : "");
}

LPCSTR gamepad_key_prompt(int dik)
{
	static string64 prompt;
	prompt[0] = 0;
	if (g_gamepad_prompts && !is_gamepad_key(dik))
	{
		const EGameActions action = get_binded_action(dik);
		if (action == kNOTBINDED || !gamepad_action_prompt(action, prompt, sizeof(prompt)))
			prompt[0] = 0;
	}
	return prompt;
}

ConsoleBindCmds bindConsoleCmds;
BOOL bRemapped = FALSE;

class CCC_Bind : public IConsole_Command
{
	int m_work_idx;
public:
	CCC_Bind(LPCSTR N, int idx) : IConsole_Command(N), m_work_idx(idx)
	{
	};

	virtual void Execute(LPCSTR args)
	{
		string256 action;
		string256 key;
		*action = 0;
		*key = 0;

		sscanf(args, "%s %s", action, key);
		if (!*action)
			return;

		if (!*key)
			return;

		if (!bRemapped)
		{
			remap_keys();
			bRemapped = TRUE;
		}

		if (!action_name_to_ptr(action))
			return;

		int action_id = action_name_to_id(action);
		if (action_id == kNOTBINDED)
			return;

		_keyboard* pkeyboard = keyname_to_ptr(key);
		if (!pkeyboard)
			return;

		_binding* curr_pbinding = &g_key_bindings[action_id];

		curr_pbinding->m_keyboard[m_work_idx] = pkeyboard;

		{
			for (int idx = 0; idx < bindings_count; ++idx)
			{
				_binding* binding = &g_key_bindings[idx];
				if (binding == curr_pbinding) continue;

				bool b_conflict = !is_group_not_conflicted(binding->m_action->key_group,
				                                           curr_pbinding->m_action->key_group);

				if (binding->m_keyboard[0] == pkeyboard && b_conflict)
					binding->m_keyboard[0] = NULL;

				if (binding->m_keyboard[1] == pkeyboard && b_conflict)
					binding->m_keyboard[1] = NULL;
			}
		}


		CStringTable::ReparseKeyBindings();
	}

	virtual void Save(IWriter* F)
	{
		if (m_work_idx == 0)
			F->w_printf("default_controls\r\n");

		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			if (pbinding->m_keyboard[m_work_idx])
			{
				F->w_printf("%s %s %s\r\n",
				            cName,
				            pbinding->m_action->action_name,
				            pbinding->m_keyboard[m_work_idx]->key_name);
			}
		}
	}
};

class CCC_UnBind : public IConsole_Command
{
	int m_work_idx;
public:
	CCC_UnBind(LPCSTR N, int idx) : IConsole_Command(N), m_work_idx(idx)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		int action_id = action_name_to_id(args);
		_binding* pbinding = &g_key_bindings[action_id];
		pbinding->m_keyboard[m_work_idx] = NULL;

		CStringTable::ReparseKeyBindings();
	}
};

class CCC_ListActions : public IConsole_Command
{
public:
	CCC_ListActions(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		Log("- --- Action list start ---");
		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			Log("-", pbinding->m_action->action_name);
		}
		Log("- --- Action list end   ---");
	}
};

class CCC_UnBindAll : public IConsole_Command
{
public:
	CCC_UnBindAll(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			pbinding->m_keyboard[0] = NULL;
			pbinding->m_keyboard[1] = NULL;
		}
		bindConsoleCmds.clear();
	}
};

class CCC_DefControls : public CCC_UnBindAll
{
public:
	CCC_DefControls(LPCSTR N) : CCC_UnBindAll(N)
	{
	}

	virtual void Execute(LPCSTR args)
	{
		CCC_UnBindAll::Execute(args);
		string_path _cfg;
		string_path cmd;
		FS.update_path(_cfg, "$game_config$", "default_controls.ltx");
		strconcat(sizeof(cmd), cmd, "cfg_load", " ", _cfg);
		Console->Execute("bind editor kF11"); //Lucy : ImGui default key
		Console->Execute(cmd);
	}
};

class CCC_BindList : public IConsole_Command
{
public:
	CCC_BindList(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = TRUE;
	};

	virtual void Execute(LPCSTR args)
	{
		Log("- --- Bind list start ---");
		string512 buff;

		for (int idx = 0; idx < bindings_count; ++idx)
		{
			_binding* pbinding = &g_key_bindings[idx];
			xr_sprintf(buff, "[%s] primary is[%s] secondary is[%s]",
			           pbinding->m_action->action_name,
			           (pbinding->m_keyboard[0]) ? pbinding->m_keyboard[0]->key_local_name.c_str() : "NULL",
			           (pbinding->m_keyboard[1]) ? pbinding->m_keyboard[1]->key_local_name.c_str() : "NULL");
			Log(buff);
		}
		Log("- --- Bind list end   ---");
	}
};

class CCC_BindConsoleCmd : public IConsole_Command
{
public:
	CCC_BindConsoleCmd(LPCSTR N) : IConsole_Command(N)
	{
	};

	virtual void Execute(LPCSTR args)
	{
		string512 console_command;
		string256 key;
		int cnt = _GetItemCount(args, ' ');
		_GetItems(args, 0, cnt - 1, console_command, ' ');
		_GetItem(args, cnt - 1, key, ' ');

		int dik = keyname_to_dik(key);
		bindConsoleCmds.bind(dik, console_command);
	}

	virtual void Save(IWriter* F)
	{
		bindConsoleCmds.save(F);
	}
};


class CCC_UnBindConsoleCmd : public IConsole_Command
{
public:
	CCC_UnBindConsoleCmd(LPCSTR N) : IConsole_Command(N)
	{
		bEmptyArgsHandled = FALSE;
	};

	virtual void Execute(LPCSTR args)
	{
		int _dik = keyname_to_dik(args);
		bindConsoleCmds.unbind(_dik);
	}
};

void ConsoleBindCmds::bind(int dik, LPCSTR N)
{
	_conCmd& c = m_bindConsoleCmds[dik];
	c.cmd = N;
}

void ConsoleBindCmds::unbind(int dik)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.find(dik);
	if (it == m_bindConsoleCmds.end())
		return;

	m_bindConsoleCmds.erase(it);
}

void ConsoleBindCmds::clear()
{
	m_bindConsoleCmds.clear();
}

bool ConsoleBindCmds::execute(int dik)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.find(dik);
	if (it == m_bindConsoleCmds.end())
		return false;

	Console->Execute(it->second.cmd.c_str());
	return true;
}

void ConsoleBindCmds::save(IWriter* F)
{
	xr_map<int, _conCmd>::iterator it = m_bindConsoleCmds.begin();

	for (; it != m_bindConsoleCmds.end(); ++it)
	{
		LPCSTR keyname = dik_to_keyname(it->first, false);
		F->w_printf("bind_console %s %s\n", *it->second.cmd, keyname);
	}
}


static EGameActions s_gamepad_bindings[GP_KEY_COUNT];
static EGameActions s_gamepad_resolved[GP_KEY_COUNT];
static int s_gamepad_keys[GP_KEY_COUNT]; // a keyboard or mouse key instead of an action, 0 none

EGameActions gamepad_binding(int gp_key)
{
	if (!is_gamepad_key(gp_key))
		return kNOTBINDED;
	const int i = gp_key - GP_FIRST;
	return s_gamepad_resolved[i] != kNOTBINDED ? s_gamepad_resolved[i] : s_gamepad_bindings[i];
}

int gamepad_key_binding(int gp_key)
{
	return is_gamepad_key(gp_key) ? s_gamepad_keys[gp_key - GP_FIRST] : 0;
}

int action_key_held(EGameActions action)
{
	for (int idx = 0; idx < 2; ++idx)
	{
		const int dik = get_action_dik(action, idx);
		if (dik && pInput->iGetAsyncKeyState(dik))
			return dik;
	}
	for (int key = GP_FIRST; key <= GP_LAST; ++key)
		if (pInput->iGetAsyncKeyState(key) && gamepad_binding(key) == action)
			return key;
	return 0;
}

bool is_action_key_held(EGameActions action) { return action_key_held(action) != 0; }

void gamepad_resolve(int gp_key, EGameActions action)
{
	if (is_gamepad_key(gp_key))
		s_gamepad_resolved[gp_key - GP_FIRST] = action;
}

static void gamepad_unbind_all()
{
	for (int i = 0; i < GP_KEY_COUNT; ++i)
	{
		s_gamepad_bindings[i] = kNOTBINDED;
		s_gamepad_keys[i] = 0;
	}
}

// The layout's version: a layout saved by an older one gets the bindings added since, on keys left free
// (gamepad_layout_update). 2: LT + R3 the alternative aim; 3: RB held the actions wheel (it was night vision, which
// the radial menu has)
static const int kLayoutVersion = 3;
static int s_layout_version = kLayoutVersion;

void gamepad_layout_update()
{
	// Once, after the saved settings are loaded: a gp_unbind_all typed later stays as typed
	static bool s_checked = false;
	if (s_checked)
		return;
	s_checked = true;
	if (s_layout_version >= kLayoutVersion)
		return;
	if (s_layout_version < 2 && s_gamepad_bindings[GP_RS + GP_AIM - GP_FIRST] == kNOTBINDED)
		s_gamepad_bindings[GP_RS + GP_AIM - GP_FIRST] = kWPN_FUNC;
	const int rb_hold = GP_RB + GP_HOLD - GP_FIRST;
	if (s_layout_version < 3 && !s_gamepad_keys[rb_hold] &&
		(s_gamepad_bindings[rb_hold] == kNOTBINDED || s_gamepad_bindings[rb_hold] == kNIGHT_VISION))
		s_gamepad_bindings[rb_hold] = kACTION_WHEEL;
	s_layout_version = kLayoutVersion;
}

// Like STALKER 2's: the left stick moves (L3 sprints), the right one looks (R3 the knife, LT + R3 the alternative aim:
// the weapon's other sight or aiming position), LT aims, RT fires, A jumps,
// B crouches, X uses what's looked at or reloads (held: the next ammo type), Y swaps weapons (held: hides it), LB held
// opens the radial menu (pressed: the detector), RB the torch (held: the actions wheel), LT + LB / RB lean, the D-pad: up the
// fire mode (held: the weapon's function), down the inventory, left / right the quick slots (held: the other two),
// View the PDA, Menu the main menu
void gamepad_default_layout()
{
	static const struct
	{
		int key;
		EGameActions action;
	} layout[] = {
		{GP_LS_UP, kFWD}, {GP_LS_DOWN, kBACK}, {GP_LS_LEFT, kL_STRAFE}, {GP_LS_RIGHT, kR_STRAFE},
		{GP_LS, kSPRINT_TOGGLE}, {GP_RS, kWPN_1}, {GP_RS + GP_AIM, kWPN_FUNC},
		{GP_LT, kWPN_ZOOM}, {GP_RT, kWPN_FIRE},
		{GP_A, kJUMP}, {GP_B, kCROUCH},
		{GP_X, kUSE_OR_RELOAD}, {GP_X + GP_HOLD, kWPN_NEXT},
		{GP_Y, kWPN_SWAP}, {GP_Y + GP_HOLD, kWPN_HOLSTER},
		{GP_LB, kDETECTOR}, {GP_LB + GP_HOLD, kRADIAL_MENU}, {GP_LB + GP_AIM, kL_LOOKOUT},
		{GP_RB, kTORCH}, {GP_RB + GP_HOLD, kACTION_WHEEL}, {GP_RB + GP_AIM, kR_LOOKOUT},
		{GP_DPAD_UP, kWPN_FIREMODE_NEXT}, {GP_DPAD_UP + GP_HOLD, kWPN_FUNC},
		{GP_DPAD_DOWN, kINVENTORY},
		{GP_DPAD_LEFT, kQUICK_USE_1}, {GP_DPAD_LEFT + GP_HOLD, kQUICK_USE_3},
		{GP_DPAD_RIGHT, kQUICK_USE_2}, {GP_DPAD_RIGHT + GP_HOLD, kQUICK_USE_4},
		{GP_BACK, kACTIVE_JOBS}, {GP_START, kQUIT},
	};
	gamepad_unbind_all();
	for (const auto& b : layout)
		s_gamepad_bindings[b.key - GP_FIRST] = b.action;
}

// gp_bind <gamepad key> <action or keyboard / mouse key>, e.g. gp_bind gpA jump, gp_bind gpDPAD_UP_aim kK (a mod's key,
// pressed while the button is); saved as the whole layout after gp_unbind_all
class CCC_GamepadBind : public IConsole_Command
{
public:
	CCC_GamepadBind(LPCSTR N) : IConsole_Command(N) {}

	virtual void Execute(LPCSTR args)
	{
		string256 key = "", action = "";
		sscanf(args, "%255s %255s", key, action);
		_keyboard* kb = keyname_to_ptr(key);
		if (!kb || !is_gamepad_key(kb->dik) || kb->dik == GP_VIRTUAL)
			return;
		const int i = kb->dik - GP_FIRST;
		if (_action* a = action_name_to_ptr(action))
		{
			s_gamepad_bindings[i] = a->id;
			s_gamepad_keys[i] = 0;
		}
		else if (_keyboard* pressed = keyname_to_ptr(action))
		{
			if (is_gamepad_key(pressed->dik))
				return;
			s_gamepad_bindings[i] = kNOTBINDED;
			s_gamepad_keys[i] = pressed->dik;
		}
	}

	virtual void Save(IWriter* F)
	{
		F->w_printf("gp_unbind_all\r\n");
		for (int i = 0; i < GP_KEY_COUNT; ++i)
		{
			LPCSTR key = dik_to_keyname(GP_FIRST + i, false);
			LPCSTR action = s_gamepad_keys[i] ? dik_to_keyname(s_gamepad_keys[i], false) :
				(s_gamepad_bindings[i] != kNOTBINDED ? id_to_action_name(s_gamepad_bindings[i]) : nullptr);
			if (key && action)
				F->w_printf("%s %s %s\r\n", cName, key, action);
		}
	}
};

class CCC_GamepadUnbind : public IConsole_Command
{
public:
	CCC_GamepadUnbind(LPCSTR N) : IConsole_Command(N) {}

	virtual void Execute(LPCSTR args)
	{
		_keyboard* kb = keyname_to_ptr(args);
		if (kb && is_gamepad_key(kb->dik))
		{
			s_gamepad_bindings[kb->dik - GP_FIRST] = kNOTBINDED;
			s_gamepad_keys[kb->dik - GP_FIRST] = 0;
		}
	}
};

class CCC_GamepadUnbindAll : public IConsole_Command
{
public:
	CCC_GamepadUnbindAll(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args)
	{
		gamepad_unbind_all();
		s_layout_version = 1; // a saved layout follows; gp_layout_version after it says how old (none: 1)
	}
};

class CCC_GamepadLayoutVersion : public IConsole_Command
{
public:
	CCC_GamepadLayoutVersion(LPCSTR N) : IConsole_Command(N) {}
	virtual void Execute(LPCSTR args) { s_layout_version = atoi(args); }
	virtual void Save(IWriter* F) { F->w_printf("%s %d\r\n", cName, kLayoutVersion); }
};

class CCC_GamepadDefaultLayout : public IConsole_Command
{
public:
	CCC_GamepadDefaultLayout(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
	virtual void Execute(LPCSTR args) { gamepad_default_layout(); }
};

// gp_get_bind <gamepad key>: its action's name ("none"), or the keyboard key it presses, as the command's status, for
// the options menu
class CCC_GamepadGetBind : public IConsole_Command
{
	string64 m_last;
public:
	CCC_GamepadGetBind(LPCSTR N) : IConsole_Command(N) { m_last[0] = 0; }

	virtual void Execute(LPCSTR args)
	{
		_keyboard* kb = keyname_to_ptr(args);
		const bool pad = kb && is_gamepad_key(kb->dik);
		const int dik = pad ? s_gamepad_keys[kb->dik - GP_FIRST] : 0;
		const EGameActions a = pad ? s_gamepad_bindings[kb->dik - GP_FIRST] : kNOTBINDED;
		LPCSTR dik_name = dik ? dik_to_keyname(dik, false) : nullptr;
		xr_strcpy(m_last, dik_name ? dik_name : (a == kNOTBINDED ? "none" : id_to_action_name(a)));
	}
	virtual void Status(TStatus& S) { xr_strcpy(S, m_last); }
	virtual void Save(IWriter*) {}
};

void CCC_RegisterInput()
{
	initialize_bindings();
	for (int i = 0; i < GP_KEY_COUNT; ++i)
		s_gamepad_resolved[i] = kNOTBINDED;
	gamepad_default_layout(); // also clears the keyboard keys (gamepad_unbind_all)
	CMD1(CCC_GamepadBind, "gp_bind");
	CMD1(CCC_GamepadUnbind, "gp_unbind");
	CMD1(CCC_GamepadUnbindAll, "gp_unbind_all");
	CMD1(CCC_GamepadDefaultLayout, "gp_default_layout");
	CMD1(CCC_GamepadGetBind, "gp_get_bind");
	CMD1(CCC_GamepadLayoutVersion, "gp_layout_version");
	CMD2(CCC_Bind, "bind", 0);
	CMD2(CCC_Bind, "bind_sec", 1);
	CMD2(CCC_UnBind, "unbind", 0);
	CMD2(CCC_UnBind, "unbind_sec", 1);
	CMD1(CCC_UnBindAll, "unbindall");
	CMD1(CCC_DefControls, "default_controls");
	CMD1(CCC_ListActions, "list_actions");

	CMD1(CCC_BindList, "bind_list");
	CMD1(CCC_BindConsoleCmd, "bind_console");
	CMD1(CCC_UnBindConsoleCmd, "unbind_console");
};
