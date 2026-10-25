#pragma once

enum EGameActions
{
	kLEFT,
	kRIGHT,
	kUP,
	kDOWN,
	kJUMP,
	kCROUCH,
	kACCEL,
	kSPRINT_TOGGLE,

	kFWD,
	kBACK,
	kL_STRAFE,
	kR_STRAFE,

	kL_LOOKOUT,
	kR_LOOKOUT,

	kCAM_1,
	kCAM_2,
	kCAM_3,
	kCAM_ZOOM_IN,
	kCAM_ZOOM_OUT,

	kTORCH,
	kNIGHT_VISION,
	kDETECTOR,
	kWPN_1,
	kWPN_2,
	kWPN_3,
	kWPN_4,
	kWPN_5,
	kWPN_6,
	kARTEFACT,
	kWPN_NEXT,
	kWPN_FIRE,
	kWPN_ZOOM,
	kWPN_ZOOM_INC,
	kWPN_ZOOM_DEC,
	kWPN_RELOAD,
	kWPN_FUNC,
	kWPN_FIREMODE_PREV,
	kWPN_FIREMODE_NEXT,

	kPAUSE,
	kDROP,
	kUSE,
	kSCORES,
	kCHAT,
	kCHAT_TEAM,
	kSCREENSHOT,
	kQUIT,
	kCONSOLE,
	kINVENTORY,
	kBUY,
	kSKIN,
	kTEAM,
	kACTIVE_JOBS,

	kVOTE_BEGIN,
	kSHOW_ADMIN_MENU,
	kVOTE,
	kVOTEYES,
	kVOTENO,

	kNEXT_SLOT,
	kPREV_SLOT,

	kSPEECH_MENU_0,
	kSPEECH_MENU_1,

	kQUICK_USE_1,
	kQUICK_USE_2,
	kQUICK_USE_3,
	kQUICK_USE_4,

	kQUICK_SAVE,
	kQUICK_LOAD,
	//	kALIFE_CMD,

	kCUSTOM1,
	kCUSTOM2,
	kCUSTOM3,
	kCUSTOM4,
	kCUSTOM5,
	kCUSTOM6,
	kCUSTOM7,
	kCUSTOM8,
	kCUSTOM9,
	kCUSTOM10,
	kCUSTOM11,
	kCUSTOM12,
	kCUSTOM13,
	kCUSTOM14,
	kCUSTOM15,
	kCUSTOM16,
	kCUSTOM17,
	kCUSTOM18,
	kCUSTOM19,
	kCUSTOM20,
	kCUSTOM21,
	kCUSTOM22,
	kCUSTOM23,
	kCUSTOM24,
	kCUSTOM25,
	kSAFEMODE,
	kFREELOOK,

	kCAM_AUTOAIM,
	kEDITOR,

	// Gamepad actions (gamepad.cpp), decided when pressed: use what's looked at or reload, the other of the two weapon
	// slots, hide or take out the weapon, the radial menu (the slots; mods' keys on the buttons while held)
	kUSE_OR_RELOAD,
	kWPN_SWAP,
	kWPN_HOLSTER,
	kRADIAL_MENU,

	kLASTACTION,
	kNOTBINDED,
	kFORCEDWORD = u32(-1)
};

struct _keyboard
{
	LPCSTR key_name;
	int dik;
	xr_string key_local_name;
};

enum _key_group
{
	_both = (1 << 0),
	_sp = _both | (1 << 1),
	_mp = _both | (1 << 2),
};

extern _key_group g_current_keygroup;

bool is_group_not_conflicted(_key_group g1, _key_group g2);

struct _action
{
	LPCSTR action_name;
	EGameActions id;
	_key_group key_group;
};

LPCSTR dik_to_keyname(int _dik, bool bLocalize);
int keyname_to_dik(LPCSTR _name);
_keyboard* keyname_to_ptr(LPCSTR _name);
_keyboard* dik_to_ptr(int _dik, bool bSafe);

LPCSTR id_to_action_name(EGameActions _id);
EGameActions action_name_to_id(LPCSTR _name);
_action* action_name_to_ptr(LPCSTR _name);

extern _action actions [];
//extern _keyboard	keyboards	[];
//extern xr_vector< _keyboard >	keyboards;

#define bindings_count kLASTACTION

struct _binding
{
	_action* m_action;
	_keyboard* m_keyboard[2];
};

extern _binding g_key_bindings[];

bool is_binded(EGameActions action_id, int dik);
int get_action_dik(EGameActions action_id, int idx = -1);
EGameActions get_binded_action(int dik);

extern void CCC_RegisterInput();

struct _conCmd
{
	shared_str cmd;
};

class ConsoleBindCmds
{
public:
	xr_map<int, _conCmd> m_bindConsoleCmds;

	void bind(int dik, LPCSTR N);
	void unbind(int dik);
	bool execute(int dik);
	void clear();
	void save(IWriter* F);
};

void GetActionAllBinding(LPCSTR action, char* dst_buff, int dst_buff_sz);

extern ConsoleBindCmds bindConsoleCmds;

// Gamepad layout (gp_bind): the action of each gamepad key (GP_ codes), kNOTBINDED for none. A key pressed for an
// action decided at the press (kUSE_OR_RELOAD...) has that action until released (gamepad_resolve)
EGameActions gamepad_binding(int gp_key);
// ... or the keyboard or mouse key it presses (gp_bind gpX_aim kK: a mod's key), 0 for none
int gamepad_key_binding(int gp_key);
// Key names in texts ($$ACTION_...$$) are the gamepad's while it is the device in use (gamepad.cpp)
extern bool g_gamepad_prompts;
// The gamepad's button for the action a keyboard or mouse key is bound to, as texts name it; "" while texts name
// keyboard keys or the action has none on the gamepad (gamepad_key_prompt in scripts)
LPCSTR gamepad_key_prompt(int dik);
// ... named as a PlayStation controller's (Cross, L1...), not an Xbox one's
extern bool g_gamepad_playstation;
// A button (0..GP_BUTTON_COUNT-1) as texts name it: its glyph (gp_glyphs) or its name
LPCSTR gamepad_button_text(int b);
// A key of the action is down: its keyboard or mouse keys (primary, secondary) or a gamepad key bound to it
bool is_action_key_held(EGameActions action);
int action_key_held(EGameActions action); // the key holding it (a keyboard, mouse or gamepad key), 0: none
void gamepad_resolve(int gp_key, EGameActions action);
void gamepad_default_layout();

// 0xED - max vavue in DIK* enum
#define MOUSE_1		(0xED + 100)
#define MOUSE_2		(0xED + 101)
#define MOUSE_3		(0xED + 102)

#define MOUSE_4		(0xED + 103)
#define MOUSE_5		(0xED + 104)
#define MOUSE_6		(0xED + 105)
#define MOUSE_7		(0xED + 106)
#define MOUSE_8		(0xED + 107)
