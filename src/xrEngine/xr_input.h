#ifndef __XR_INPUT__
#define __XR_INPUT__

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

class ENGINE_API IInputReceiver;

//\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\
//описание класса
const int mouse_device_key = 1;
const int keyboard_device_key = 2;
const int all_device_key = mouse_device_key | keyboard_device_key;
const int default_key = mouse_device_key | keyboard_device_key;

// Gamepad (XInput) buttons as key codes, after the mouse's (MOUSE_1..8 = 0xED+100..107): bound and sent like keys.
// A button held for gp_hold_time is its own code (+GP_HOLD), as is a button pressed while LT is held (+GP_AIM);
// the left stick's directions are codes too (movement)
enum
{
	GP_A = 400,
	GP_B,
	GP_X,
	GP_Y,
	GP_LB,
	GP_RB,
	GP_LT,
	GP_RT,
	GP_BACK,
	GP_START,
	GP_LS,
	GP_RS,
	GP_DPAD_UP,
	GP_DPAD_DOWN,
	GP_DPAD_LEFT,
	GP_DPAD_RIGHT,
	GP_BUTTON_COUNT = 16,
	GP_HOLD = 20,
	GP_AIM = 40,
	GP_LS_UP = 460,
	GP_LS_DOWN,
	GP_LS_LEFT,
	GP_LS_RIGHT,
	GP_VIRTUAL, // an action chosen by the game (the radial menu): sent as this key, its action set for the press
	GP_FIRST = GP_A,
	GP_LAST = GP_VIRTUAL,
	GP_KEY_COUNT = GP_LAST - GP_FIRST + 1,
};
IC bool is_gamepad_key(int dik) { return dik >= GP_FIRST && dik <= GP_LAST; }

// One gamepad's state: buttons as bits (1 << (code - GP_A), the triggers past half way as buttons), sticks -1..1
// (y up), triggers 0..1
struct gamepad_state
{
	bool connected = false;
	u32 buttons = 0;
	float lx = 0.f, ly = 0.f, rx = 0.f, ry = 0.f;
	float lt = 0.f, rt = 0.f;
	// The gyro (Sony controllers): how far the controller turned since the last read, radians, yaw right and pitch up
	float gyro_yaw = 0.f, gyro_pitch = 0.f;
	bool gyro = false;
	bool playstation = false; // its buttons are Cross, Circle, Square, Triangle, L1...
};

class ENGINE_API CInput
#ifndef M_BORLAND
	:
	public pureFrame,
	public pureAppActivate,
	public pureAppDeactivate
#endif
{
public:
	enum
	{
		COUNT_MOUSE_BUTTONS = 8,
		COUNT_MOUSE_AXIS = 3,
		COUNT_KB_BUTTONS = 256
	};

	struct sxr_mouse
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
		u32 mouse_dt;
	};

	struct sxr_key
	{
		DIDEVCAPS capabilities;
		DIDEVICEINSTANCE deviceInfo;
		DIDEVICEOBJECTINSTANCE objectInfo;
	};

private:
	BENCH_SEC_SCRAMBLEMEMBER1
	LPDIRECTINPUT8 pDI; // The DInput object
	LPDIRECTINPUTDEVICE8 pMouse; // The DIDevice7 interface
	LPDIRECTINPUTDEVICE8 pKeyboard; // The DIDevice7 interface
	//----------------------
	u32 timeStamp[COUNT_MOUSE_AXIS];
	u32 timeSave[COUNT_MOUSE_AXIS];
	int offs[COUNT_MOUSE_AXIS];
	// mouse_filter: the last frame's movement and the fractions not applied yet (x, y)
	int filter_prev[2];
	float filter_rest[2];
	BOOL mouseState[COUNT_MOUSE_BUTTONS];

	//----------------------
	BOOL KBState[COUNT_KB_BUTTONS];
	BOOL padKeyState[GP_KEY_COUNT];

	// Gamepad: XInput's first connected controller, or a simulated one (gp_sim); the game turns it into input
	gamepad_state m_pad;
	gamepad_state m_pad_sim;
	bool m_pad_sim_on = false;
	int m_pad_slot = -1; // the XInput user index read, -1 none
	bool m_pad_sony = false; // the pad read is a Sony controller (HID), not XInput's
	u32 m_pad_scan_time = 0; // the last look for a connected controller
	u32 m_pad_vibration_until = 0;
	bool m_pad_vibrating = false;
	bool m_pad_in_use = false; // the gamepad was used last, not the mouse or keyboard
	u32 m_pad_cursor_time = 0; // the gamepad last moved the cursor: mouse movement just after is from that
	void PadUpdate();

	HRESULT CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice,
	                          const DIDATAFORMAT* pdidDataFormat, u32 dwFlags,
	                          u32 buf_size);

	// xr_stack<IInputReceiver*> cbStack;
	xr_vector<IInputReceiver*> cbStack;

	void MouseUpdate();
	void KeyUpdate();

public:
	void resetMouseState();
	sxr_mouse mouse_property;
	sxr_key key_property;
	u32 dwCurTime;

	void SetAllAcquire(BOOL bAcquire = TRUE);
	void SetMouseAcquire(BOOL bAcquire);
	void SetKBDAcquire(BOOL bAcquire);

	void iCapture(IInputReceiver* pc);
	void iRelease(IInputReceiver* pc);
	BOOL iGetAsyncKeyState(int dik);
	BOOL iGetAsyncBtnState(int btn);
	void iGetLastMouseDelta(Ivector2& p) { p.set(offs[0], offs[1]); }

	CInput(BOOL bExclusive = true, int deviceForInit = default_key);
	~CInput();

	virtual void _BCL OnFrame(void);
	virtual void OnAppActivate(void);
	virtual void OnAppDeactivate(void);
	void DeactivateSoft();

	IInputReceiver* CurrentIR();

public:
	void exclusive_mode(const bool& exclusive);
	bool get_exclusive_mode();
	void unacquire();
	void acquire(const bool& exclusive);
	bool get_dik_name(int dik, LPSTR dest, int dest_sz);
	bool dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz);

	void feedback(u16 s1, u16 s2, float time);

	// Synthetic input for the remote console (rc_key, rc_click, rc_cursor): sent to the current receiver like
	// DirectInput's, with the key and button states kept for iGetAsyncKeyState / iGetAsyncBtnState
	void inject_key(int dik, bool press);
	void inject_mouse_button(int btn, bool press);
	void inject_mouse_move(int dx, int dy);
	void inject_mouse_wheel(int direction);

	// Gamepad
	const gamepad_state& pad() const { return m_pad; }
	bool pad_in_use() const { return m_pad.connected && m_pad_in_use; }
	void pad_simulate(const gamepad_state* state); // nullptr: back to the real controller
	void pad_cursor_moved() { m_pad_cursor_time = dwCurTime; }
	// The DualSense's right trigger: 0 free, 1 a firearm's (resistance, then a break)
	void pad_trigger_effect(int effect);
	// The game's handler, called every frame after the gamepad is read (dt in seconds): turns it into key presses
	// (inject_key with GP_ codes), mouse input and camera turns
	void (*pad_handler)(float dt) = nullptr;
	// Held gamepad keys get IR_OnKeyboardHold every frame like held keyboard keys
	void release_pad_keys();
};

extern ENGINE_API CInput* pInput;

#endif //__XR_INPUT__
