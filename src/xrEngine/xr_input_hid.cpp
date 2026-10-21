#include "stdafx.h"
#include "xr_input.h"
#include "xr_input_hid.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <atomic>
#include <mutex>
#include <thread>

// Report layouts as SDL's HIDAPI drivers for the PS4 and PS5 controllers read them
namespace sonypad
{
namespace
{
const u16 SONY = 0x054C;
const u16 DUALSENSE[] = {0x0CE6, 0x0DF2}; // DualSense, DualSense Edge
const u16 DUALSHOCK4[] = {0x05C4, 0x09CC, 0x0BA0}; // v1, v2, the USB wireless adaptor

// hid.dll and setupapi.dll are loaded at the first look for a controller
decltype(&HidD_GetHidGuid) p_GetHidGuid;
decltype(&HidD_GetAttributes) p_GetAttributes;
decltype(&HidD_GetFeature) p_GetFeature;
decltype(&HidD_GetPreparsedData) p_GetPreparsedData;
decltype(&HidD_FreePreparsedData) p_FreePreparsedData;
decltype(&HidP_GetCaps) p_GetCaps;
decltype(&SetupDiGetClassDevsA) p_GetClassDevs;
decltype(&SetupDiEnumDeviceInterfaces) p_EnumInterfaces;
decltype(&SetupDiGetDeviceInterfaceDetailA) p_GetInterfaceDetail;
decltype(&SetupDiDestroyDeviceInfoList) p_DestroyList;

bool load_dlls()
{
	HMODULE hid = LoadLibrary("hid.dll"), setup = LoadLibrary("setupapi.dll");
	if (!hid || !setup)
		return false;
#define GET(var, module, name) var = (decltype(var))GetProcAddress(module, name)
	GET(p_GetHidGuid, hid, "HidD_GetHidGuid");
	GET(p_GetAttributes, hid, "HidD_GetAttributes");
	GET(p_GetFeature, hid, "HidD_GetFeature");
	GET(p_GetPreparsedData, hid, "HidD_GetPreparsedData");
	GET(p_FreePreparsedData, hid, "HidD_FreePreparsedData");
	GET(p_GetCaps, hid, "HidP_GetCaps");
	GET(p_GetClassDevs, setup, "SetupDiGetClassDevsA");
	GET(p_EnumInterfaces, setup, "SetupDiEnumDeviceInterfaces");
	GET(p_GetInterfaceDetail, setup, "SetupDiGetDeviceInterfaceDetailA");
	GET(p_DestroyList, setup, "SetupDiDestroyDeviceInfoList");
#undef GET
	return p_GetHidGuid && p_GetAttributes && p_GetFeature && p_GetPreparsedData && p_FreePreparsedData && p_GetCaps &&
		p_GetClassDevs && p_EnumInterfaces && p_GetInterfaceDetail && p_DestroyList;
}

u32 crc32(u32 crc, const u8* data, size_t size)
{
	crc = ~crc;
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (int b = 0; b < 8; ++b)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

s16 s16_at(const u8* p) { return s16(u16(p[0]) | (u16(p[1]) << 8)); }

struct device
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	bool dualsense = false;
	bool bluetooth = false;
	u16 pid = 0;
	u32 input_size = 0;
};

// Shared with the reading thread
std::mutex s_lock;
gamepad_state s_state; // the gyro's turn added up until read() takes it
float s_rumble[2] = {0.f, 0.f};
int s_trigger = 0;
bool s_output_dirty = false;
std::atomic<bool> s_stop{false};
std::thread s_thread;
bool s_started = false;
HANDLE s_reading = nullptr; // the device being read: shutdown() cancels its read

// The gyro: its bias (the rate it reports at rest) from the controller's calibration, then measured whenever it lies
// still for a second; the direction up from the accelerometer for turns about the world's vertical
struct gyro_filter
{
	float bias[3] = {0.f, 0.f, 0.f};
	float up[3] = {0.f, 1.f, 0.f};
	bool up_valid = false;
	// The still window
	float sum[3], lo[6], hi[6];
	float time = 0.f;
	int count = 0;

	void reset_window()
	{
		for (int i = 0; i < 3; ++i)
			sum[i] = 0.f;
		for (int i = 0; i < 6; ++i)
			lo[i] = flt_max, hi[i] = -flt_max;
		time = 0.f;
		count = 0;
	}

	// raw: the gyro's three axes then the accelerometer's (counts); dt seconds. Adds the turn to yaw (right) and
	// pitch (up), radians
	void sample(const float raw[6], float dt, float& yaw, float& pitch)
	{
		// Still: every axis within a narrow range for a second (a slow steady turn tilts the accelerometer's reading)
		for (int i = 0; i < 6; ++i)
		{
			lo[i] = _min(lo[i], raw[i]);
			hi[i] = _max(hi[i], raw[i]);
		}
		bool still = true;
		for (int i = 0; i < 3; ++i)
			still = still && hi[i] - lo[i] < 40.f && hi[i + 3] - lo[i + 3] < 150.f;
		if (!still)
			reset_window();
		else
		{
			for (int i = 0; i < 3; ++i)
				sum[i] += raw[i];
			++count;
			time += dt;
			if (time >= 1.f)
			{
				for (int i = 0; i < 3; ++i)
					bias[i] = sum[i] / float(count);
				reset_window();
			}
		}

		// The gravity's direction, smoothed
		const float a_len = _sqrt(raw[3] * raw[3] + raw[4] * raw[4] + raw[5] * raw[5]);
		if (a_len > 1.f)
		{
			const float k = up_valid ? clampr(dt * 4.f, 0.f, 1.f) : 1.f;
			for (int i = 0; i < 3; ++i)
				up[i] += (raw[i + 3] / a_len - up[i]) * k;
			up_valid = true;
		}

		// Degrees per second: 16 counts each (+-2000 over 16 bits); x pitch, y yaw, z roll, counterclockwise positive
		float w[3];
		for (int i = 0; i < 3; ++i)
			w[i] = (raw[i] - bias[i]) / 16.f;
		// A rate under 1.5 degrees/s is scaled down towards zero: the hand's tremor
		const float rate = _sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		const float tight = 1.5f;
		if (rate < tight)
			for (int i = 0; i < 3; ++i)
				w[i] *= rate / tight;
		// Yaw about the world's vertical ("player space"): the controller held tilted turns about its y and z, its
		// size from both, the direction from gravity
		const float local = _sqrt(w[1] * w[1] + w[2] * w[2]);
		const float world = w[1] * up[1] + w[2] * up[2];
		const float yaw_rate = (world < 0.f ? -1.f : 1.f) * _min(_abs(world) * 1.41f, local);
		yaw -= deg2rad(yaw_rate) * dt;
		pitch += deg2rad(w[0]) * dt;
	}
};

void set_button(u32& buttons, int code, bool on)
{
	if (on)
		buttons |= 1u << (code - GP_A);
}

// The state from a full input report: data at the state's first byte (left stick x)
void parse(const device& d, const u8* p, gamepad_state& st, float raw[6], u32& stamp, bool& has_imu)
{
	float stick_axes[4];
	for (int i = 0; i < 4; ++i)
		stick_axes[i] = clampr((float(p[i]) - 127.5f) / 127.5f, -1.f, 1.f);
	st.lx = stick_axes[0];
	st.ly = -stick_axes[1];
	st.rx = stick_axes[2];
	st.ry = -stick_axes[3];
	// DualSense: triggers at 4, 5, buttons at 7..9; DualShock 4: buttons at 4..6, triggers at 7, 8
	const u8* b = p + (d.dualsense ? 7 : 4);
	st.lt = float(p[d.dualsense ? 4 : 7]) / 255.f;
	st.rt = float(p[d.dualsense ? 5 : 8]) / 255.f;
	u32 buttons = 0;
	const int hat = b[0] & 0x0F; // 0 up, clockwise to 7 up-left, 8 none
	set_button(buttons, GP_DPAD_UP, hat == 7 || hat == 0 || hat == 1);
	set_button(buttons, GP_DPAD_RIGHT, hat >= 1 && hat <= 3);
	set_button(buttons, GP_DPAD_DOWN, hat >= 3 && hat <= 5);
	set_button(buttons, GP_DPAD_LEFT, hat >= 5 && hat <= 7);
	set_button(buttons, GP_X, b[0] & 0x10); // square
	set_button(buttons, GP_A, b[0] & 0x20); // cross
	set_button(buttons, GP_B, b[0] & 0x40); // circle
	set_button(buttons, GP_Y, b[0] & 0x80); // triangle
	set_button(buttons, GP_LB, b[1] & 0x01);
	set_button(buttons, GP_RB, b[1] & 0x02);
	set_button(buttons, GP_BACK, (b[1] & 0x10) || (b[2] & 0x02)); // Create (Share) or the touchpad's click
	set_button(buttons, GP_START, b[1] & 0x20); // Options
	set_button(buttons, GP_LS, b[1] & 0x40);
	set_button(buttons, GP_RS, b[1] & 0x80);
	st.buttons = buttons;

	// The motion sensors: gyro then accelerometer, 3 x 16 bits each
	const u8* imu = p + (d.dualsense ? 15 : 12);
	for (int i = 0; i < 6; ++i)
		raw[i] = float(s16_at(imu + i * 2));
	// The sensors' clock (ticks): the DualSense's 32 bits, the DualShock 4's 16
	stamp = d.dualsense ? u32(p[27] | (p[28] << 8) | (p[29] << 16) | (u32(p[30]) << 24)) : u32(p[9] | (p[10] << 8));
	has_imu = true;
}

// A Sony controller among the HID devices, opened
bool find(device& d)
{
	GUID guid;
	p_GetHidGuid(&guid);
	HDEVINFO list = p_GetClassDevs(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (list == INVALID_HANDLE_VALUE)
		return false;
	static bool logged = false;
	bool found = false;
	SP_DEVICE_INTERFACE_DATA iface;
	iface.cbSize = sizeof(iface);
	for (DWORD i = 0; !found && p_EnumInterfaces(list, nullptr, &guid, i, &iface); ++i)
	{
		DWORD size = 0;
		p_GetInterfaceDetail(list, &iface, nullptr, 0, &size, nullptr);
		if (!size)
			continue;
		xr_vector<u8> buffer(size);
		auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buffer.data();
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
		if (!p_GetInterfaceDetail(list, &iface, detail, size, nullptr, nullptr))
			continue;
		HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		// Read only (no rumble) when another program has it for writing
		if (h == INVALID_HANDLE_VALUE)
			h = CreateFileA(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		HIDD_ATTRIBUTES attr;
		attr.Size = sizeof(attr);
		HIDP_CAPS caps = {};
		PHIDP_PREPARSED_DATA pp = nullptr;
		if (p_GetAttributes(h, &attr) && p_GetPreparsedData(h, &pp))
		{
			p_GetCaps(pp, &caps);
			p_FreePreparsedData(pp);
		}
		if (!logged && caps.UsagePage == 0x01 && (caps.Usage == 0x04 || caps.Usage == 0x05))
			Msg("* Gamepad HID: %04X:%04X, usage %02X:%02X, input report %u bytes", attr.VendorID, attr.ProductID,
			    caps.UsagePage, caps.Usage, caps.InputReportByteLength);
		bool ds5 = false, ds4 = false;
		if (attr.VendorID == SONY)
		{
			for (u16 pid : DUALSENSE)
				ds5 = ds5 || attr.ProductID == pid;
			for (u16 pid : DUALSHOCK4)
				ds4 = ds4 || attr.ProductID == pid;
		}
		// Its gamepad collection (the DualShock 4's adaptor has an audio one too)
		if ((ds5 || ds4) && caps.UsagePage == 0x01 && caps.Usage == 0x05 && caps.InputReportByteLength >= 64)
		{
			d.handle = h;
			d.dualsense = ds5;
			d.pid = attr.ProductID;
			d.input_size = caps.InputReportByteLength;
			// USB reports are 64 bytes, Bluetooth ones 78 (the DualShock 4 lists up to 547)
			d.bluetooth = caps.InputReportByteLength != 64;
			found = true;
		}
		else
			CloseHandle(h);
	}
	logged = true;
	p_DestroyList(list);
	return found;
}

// Reading the calibration (feature report 5, the DualShock 4's 2 over USB) also turns on the full reports, with the
// motion sensors, over Bluetooth. Its first three values are the gyro's bias
void calibrate(const device& d, gyro_filter& gyro)
{
	u8 buf[64] = {};
	buf[0] = (!d.dualsense && !d.bluetooth) ? 0x02 : 0x05;
	if (!p_GetFeature(d.handle, buf, d.dualsense || d.bluetooth ? 41 : 37))
	{
		Msg("! Gamepad: the controller's calibration not read (%u)", GetLastError());
		return;
	}
	for (int i = 0; i < 3; ++i)
		gyro.bias[i] = float(s16_at(buf + 1 + i * 2));
}

// Rumble and the trigger effect as an output report
u32 output_report(const device& d, u8* data, float low, float high, int trigger)
{
	const u8 rumble_low = u8(iFloor(clampr(low, 0.f, 1.f) * 255.f)), rumble_high = u8(iFloor(clampr(high, 0.f, 1.f) * 255.f));
	u32 size, offset;
	if (d.dualsense)
	{
		size = d.bluetooth ? 78 : 48;
		offset = d.bluetooth ? 2 : 1;
		memset(data, 0, size);
		data[0] = d.bluetooth ? 0x31 : 0x02;
		if (d.bluetooth)
			data[1] = 0x02;
		u8* e = data + offset;
		e[0] = 0x01 | 0x02 | 0x04; // rumble (emulated with the haptics), no audio haptics, the right trigger's effect
		e[2] = rumble_high;
		e[3] = rumble_low;
		// The right trigger: free, or resistance from a fifth of the pull to over a half, then it gives (the legacy
		// "section" effect)
		u8* t = e + 10;
		if (trigger == 1)
		{
			t[0] = 0x02;
			t[1] = 0x30;
			t[2] = 0x90;
			t[3] = 0xB0;
		}
		else
			t[0] = 0x05;
		e[38] = 0x04; // the firmware's better rumble emulation (2.24 and newer)
	}
	else
	{
		size = d.bluetooth ? 78 : 32;
		offset = d.bluetooth ? 6 : 4;
		memset(data, 0, size);
		data[0] = d.bluetooth ? 0x11 : 0x05;
		if (d.bluetooth)
		{
			data[1] = 0xC0 | 0x04;
			data[3] = 0x01; // rumble only, the light bar as it is
		}
		else
			data[1] = 0x01;
		data[offset] = rumble_high;
		data[offset + 1] = rumble_low;
	}
	// Bluetooth: a CRC over the HID header byte and the report
	if (d.bluetooth)
	{
		const u8 header = 0xA2;
		u32 crc = crc32(0, &header, 1);
		crc = crc32(crc, data, size - 4);
		memcpy(data + size - 4, &crc, 4);
	}
	return size;
}

bool write(const device& d, const u8* data, u32 size)
{
	OVERLAPPED ov = {};
	ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	BOOL ok = WriteFile(d.handle, data, size, nullptr, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0)
			ok = GetOverlappedResult(d.handle, &ov, &written, FALSE);
		else
		{
			CancelIoEx(d.handle, &ov);
			GetOverlappedResult(d.handle, &ov, &written, TRUE);
		}
	}
	CloseHandle(ov.hEvent);
	return !!ok;
}

void run()
{
	if (!load_dlls())
	{
		Msg("! Gamepad: hid.dll or setupapi.dll not loaded, no Sony controllers");
		return;
	}
	OVERLAPPED ov = {};
	ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	while (!s_stop)
	{
		device d;
		if (!find(d))
		{
			for (int i = 0; i < 20 && !s_stop; ++i)
				Sleep(100);
			continue;
		}
		Msg("* Gamepad connected: %s (%04X, %s)", d.dualsense ? "DualSense" : "DualShock 4", d.pid,
		    d.bluetooth ? "Bluetooth" : "USB");
		gyro_filter gyro;
		gyro.reset_window();
		calibrate(d, gyro);
		{
			std::lock_guard<std::mutex> lock(s_lock);
			s_reading = d.handle;
			s_output_dirty = true; // the trigger as the game has it
		}
		xr_vector<u8> buf(_max(d.input_size, 128u));
		u32 last_stamp = 0;
		bool has_stamp = false;
		LARGE_INTEGER freq, last_qpc;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&last_qpc);
		u32 reports = 0;
		while (!s_stop)
		{
			ResetEvent(ov.hEvent);
			DWORD got = 0;
			if (!ReadFile(d.handle, buf.data(), d.input_size, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
				break;
			DWORD wait;
			while ((wait = WaitForSingleObject(ov.hEvent, 200)) == WAIT_TIMEOUT && !s_stop)
				;
			if (wait != WAIT_OBJECT_0)
			{
				CancelIoEx(d.handle, &ov);
				GetOverlappedResult(d.handle, &ov, &got, TRUE);
				if (s_stop)
					break;
				continue;
			}
			if (!GetOverlappedResult(d.handle, &ov, &got, FALSE))
				break;

			// The full report: the DualSense's 0x01 over USB (state at 1) or 0x31 over Bluetooth (at 2); the
			// DualShock 4's 0x01 over USB (at 1) or 0x11 over Bluetooth (at 3). Bluetooth's short 0x01 reports (no
			// motion sensors) until the calibration is read: the buttons and sticks of those are at 1 too, but in
			// another order, so they're dropped
			const u8* p = nullptr;
			if (!d.bluetooth && buf[0] == 0x01 && got >= 64)
				p = buf.data() + 1;
			else if (d.bluetooth && d.dualsense && buf[0] == 0x31 && got >= 64)
				p = buf.data() + 2;
			else if (d.bluetooth && !d.dualsense && buf[0] == 0x11 && got >= 64)
				p = buf.data() + 3;
			if (!p)
				continue;
			gamepad_state st;
			float raw[6];
			u32 stamp = 0;
			bool imu = false;
			parse(d, p, st, raw, stamp, imu);
			LARGE_INTEGER qpc;
			QueryPerformanceCounter(&qpc);
			float dt = float(double(qpc.QuadPart - last_qpc.QuadPart) / double(freq.QuadPart));
			last_qpc = qpc;
			// The sensors' own clock is steadier than the reports' arrival: the DualSense's in 1/3 us, the DualShock
			// 4's in 16/3 us over 16 bits
			if (has_stamp)
			{
				const float us = d.dualsense ? float(stamp - last_stamp) / 3.f : float(u16(stamp - last_stamp)) * 16.f / 3.f;
				if (us > 0.f && us < 50000.f)
					dt = us * 1e-6f;
			}
			last_stamp = stamp;
			has_stamp = true;
			dt = clampr(dt, 0.f, 0.05f);
			float yaw = 0.f, pitch = 0.f;
			if (imu)
				gyro.sample(raw, dt, yaw, pitch);
			++reports;

			bool output = false;
			float rumble[2];
			int trigger;
			{
				std::lock_guard<std::mutex> lock(s_lock);
				const float yaw_sum = s_state.gyro_yaw + yaw, pitch_sum = s_state.gyro_pitch + pitch;
				s_state = st;
				s_state.connected = true;
				s_state.gyro = imu;
				s_state.playstation = true;
				s_state.gyro_yaw = yaw_sum;
				s_state.gyro_pitch = pitch_sum;
				output = s_output_dirty;
				s_output_dirty = false;
				rumble[0] = s_rumble[0];
				rumble[1] = s_rumble[1];
				trigger = s_trigger;
			}
			if (output)
			{
				u8 out[80];
				const u32 size = output_report(d, out, rumble[0], rumble[1], trigger);
				if (!write(d, out, size))
					Msg("! Gamepad: output report not sent (%u)", GetLastError());
			}
		}
		{
			std::lock_guard<std::mutex> lock(s_lock);
			s_state = gamepad_state();
			s_reading = nullptr;
		}
		if (!s_stop)
			Msg("* Gamepad disconnected (%s, %u reports)", d.dualsense ? "DualSense" : "DualShock 4", reports);
		CloseHandle(d.handle);
	}
	CloseHandle(ov.hEvent);
}

void start()
{
	if (s_started)
		return;
	s_started = true;
	s_stop = false;
	s_thread = std::thread(run);
}
}

bool read(gamepad_state& state)
{
	start();
	std::lock_guard<std::mutex> lock(s_lock);
	if (!s_state.connected)
		return false;
	state = s_state;
	s_state.gyro_yaw = s_state.gyro_pitch = 0.f;
	return true;
}

void vibrate(float low, float high)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_rumble[0] == low && s_rumble[1] == high)
		return;
	s_rumble[0] = low;
	s_rumble[1] = high;
	s_output_dirty = true;
}

void trigger_effect(int effect)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_trigger == effect)
		return;
	s_trigger = effect;
	s_output_dirty = true;
}

void shutdown()
{
	if (!s_started)
		return;
	s_stop = true;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_reading)
			CancelIoEx(s_reading, nullptr);
	}
	if (s_thread.joinable())
		s_thread.join();
	s_started = false;
}
}
