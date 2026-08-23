#include "gamepad-runtime.hpp"
#include "hid-gyro.hpp"
#include "gamesir-hid.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <obs-module.h>
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr int kSlots = 4;
constexpr DWORD kGuideButton = 0x0400;
constexpr wchar_t kWndClass[] = L"GamepadPluginRawInputWnd";

using XInputGetStateExFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

struct RumbleState {
	uint16_t left = 0;
	uint16_t right = 0;
	ULONGLONG until_ms = 0;
	bool active = false;
};

struct Runtime {
	std::atomic<int> refs{0};
	std::atomic<bool> running{false};
	std::thread thread;
	HWND hwnd = nullptr;
	HMODULE xinput_mod = nullptr;
	XInputGetStateExFn get_state_ex = nullptr;

	std::mutex mutex;
	PadSnapshot pads[kSlots];
	RumbleState rumble[kSlots];
	bool slot_seen[kSlots] = {};
	ULONGLONG next_empty_poll_ms[kSlots] = {};

	GyroSample gyro{};
	ULONGLONG gyro_ms = 0;

	struct Jitter {
		float peak = 0;
		float value = 0;
	} ls_jit, rs_jit;

	uint32_t last_packet = 0;
	int packet_changes = 0;
	ULONGLONG packet_window_ms = 0;
	int xinput_hz = 0;
	GamepadNeeds needs{};
};

Runtime g;

float axis_norm(int16_t v)
{
	if (v >= 0)
		return (float)v / 32767.0f;
	return (float)v / 32768.0f;
}

float update_jitter(Runtime::Jitter &j, float x, float y)
{
	const float mag = sqrtf(x * x + y * y);
	if (mag <= 0.14f) {
		if (mag > j.peak)
			j.peak = mag;
		else
			j.peak += (mag - j.peak) * 0.035f;
	}
	const float pct = std::clamp(j.peak / 0.035f * 100.0f, 0.0f, 100.0f);
	j.value += (pct - j.value) * 0.18f;
	return j.value;
}

void copy_name(char *dst, size_t dst_sz, const char *src)
{
	if (!src)
		src = "";
	std::snprintf(dst, dst_sz, "%s", src);
}

void apply_gyro(PadSnapshot *pad, const GyroSample &gyro)
{
	pad->gyro_valid = gyro.valid;
	if (!gyro.valid)
		return;
	for (int i = 0; i < 3; i++) {
		pad->gyro_raw[i] = gyro.gyro_raw[i];
		pad->accel_raw[i] = gyro.accel_raw[i];
		pad->gyro_dps[i] = gyro.gyro_dps[i];
		pad->accel_g[i] = gyro.accel_g[i];
	}
	copy_name(pad->gyro_name, sizeof(pad->gyro_name), gyro.device_name);
}

void poll_slot(int i, ULONGLONG now, bool force)
{
	if (!g.slot_seen[i] && !force && now < g.next_empty_poll_ms[i])
		return;

	XINPUT_STATE state{};
	DWORD err;
	if (g.get_state_ex)
		err = g.get_state_ex((DWORD)i, &state);
	else
		err = XInputGetState((DWORD)i, &state);

	PadSnapshot snap{};
	if (err == ERROR_SUCCESS) {
		g.slot_seen[i] = true;
		snap.connected = true;
		snap.packet = state.dwPacketNumber;
		snap.buttons = state.Gamepad.wButtons;
		snap.lt = state.Gamepad.bLeftTrigger;
		snap.rt = state.Gamepad.bRightTrigger;
		snap.lx = state.Gamepad.sThumbLX;
		snap.ly = state.Gamepad.sThumbLY;
		snap.rx = state.Gamepad.sThumbRX;
		snap.ry = state.Gamepad.sThumbRY;
		snap.lx_n = axis_norm(snap.lx);
		snap.ly_n = axis_norm(snap.ly);
		snap.rx_n = axis_norm(snap.rx);
		snap.ry_n = axis_norm(snap.ry);
		if (g.needs.jitter) {
			snap.ls_tune.jitter = update_jitter(g.ls_jit, snap.lx_n, snap.ly_n);
			snap.rs_tune.jitter = update_jitter(g.rs_jit, snap.rx_n, snap.ry_n);
		}
		if (g.packet_window_ms == 0)
			g.packet_window_ms = now;
		if (g.needs.hid_poll) {
			if (state.dwPacketNumber != g.last_packet) {
				g.packet_changes++;
				g.last_packet = state.dwPacketNumber;
			}
			if (now - g.packet_window_ms >= 1000) {
				g.xinput_hz =
					(int)((g.packet_changes * 1000) / std::max<ULONGLONG>(now - g.packet_window_ms, 1));
				g.packet_changes = 0;
				g.packet_window_ms = now;
			}
		}
		std::snprintf(snap.name, sizeof(snap.name), "Machi's controller overlay");
	} else {
		g.slot_seen[i] = false;
		g.next_empty_poll_ms[i] = now + 2000;
		snap.connected = false;
		std::snprintf(snap.name, sizeof(snap.name), "No controller (slot %d)", i + 1);
		copy_name(snap.gyro_name, sizeof(snap.gyro_name), "none");
	}

	{
		std::lock_guard<std::mutex> lock(g.mutex);
		copy_name(snap.gyro_name, sizeof(snap.gyro_name), snap.connected ? "unavailable" : "none");
		g.pads[i] = snap;

		if (g.rumble[i].active) {
			if (now >= g.rumble[i].until_ms) {
				XINPUT_VIBRATION stop{};
				XInputSetState((DWORD)i, &stop);
				g.rumble[i] = {};
			}
		}
	}
}

void attach_gyro(ULONGLONG now)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const bool gyro_live = g.needs.gyro && g.gyro.valid && (now - g.gyro_ms) < 1500;
	StickTune ls{};
	StickTune rs{};
	int poll_set = 0;
	int poll_rep = 0;
	if (g.needs.hid_tune)
		gamesir_hid_copy_tune(&ls, &rs);
	if (g.needs.hid_poll) {
		gamesir_hid_copy_poll(&poll_set, &poll_rep);
		if (poll_rep <= 0)
			poll_rep = g.xinput_hz;
	}
	for (int i = 0; i < kSlots; i++) {
		if (!g.pads[i].connected) {
			g.pads[i].gyro_valid = false;
			copy_name(g.pads[i].gyro_name, sizeof(g.pads[i].gyro_name), "none");
			continue;
		}
		if (gyro_live)
			apply_gyro(&g.pads[i], g.gyro);
		const float ls_j = g.pads[i].ls_tune.jitter;
		const float rs_j = g.pads[i].rs_tune.jitter;
		if (ls.valid) {
			g.pads[i].ls_tune = ls;
			g.pads[i].ls_tune.jitter = ls_j;
		}
		if (rs.valid) {
			g.pads[i].rs_tune = rs;
			g.pads[i].rs_tune.jitter = rs_j;
		}
		g.pads[i].poll_set_hz = poll_set;
		g.pads[i].poll_report_hz = poll_rep;
	}
}

void handle_raw_input(LPARAM lParam)
{
	UINT size = 0;
	GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
	if (size == 0 || size > 4096)
		return;

	alignas(RAWINPUT) uint8_t stack[4096];
	if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, stack, &size, sizeof(RAWINPUTHEADER)) != size)
		return;

	auto *raw = reinterpret_cast<RAWINPUT *>(stack);
	if (raw->header.dwType != RIM_TYPEHID)
		return;

	RID_DEVICE_INFO info{};
	info.cbSize = sizeof(info);
	UINT info_size = sizeof(info);
	if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICEINFO, &info, &info_size) == (UINT)-1)
		return;
	if (info.dwType != RIM_TYPEHID)
		return;

	const uint8_t *hid = raw->data.hid.bRawData;
	const size_t hid_size = raw->data.hid.dwSizeHid;
	if (!hid || hid_size == 0)
		return;

	GyroSample sample{};
	if (!hid_parse_motion((uint16_t)info.hid.dwVendorId, (uint16_t)info.hid.dwProductId, hid, hid_size, &sample))
		return;

	std::lock_guard<std::mutex> lock(g.mutex);
	g.gyro = sample;
	g.gyro_ms = GetTickCount64();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT) {
		handle_raw_input(lParam);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool register_raw_input(HWND hwnd)
{
	RAWINPUTDEVICE rid[4]{};
	rid[0].usUsagePage = 0x01;
	rid[0].usUsage = 0x05;
	rid[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	rid[0].hwndTarget = hwnd;

	rid[1].usUsagePage = 0x01;
	rid[1].usUsage = 0x04;
	rid[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	rid[1].hwndTarget = hwnd;

	rid[2].usUsagePage = 0xFF00;
	rid[2].usUsage = 0x00;
	rid[2].dwFlags = RIDEV_INPUTSINK | RIDEV_PAGEONLY | RIDEV_DEVNOTIFY;
	rid[2].hwndTarget = hwnd;

	rid[3].usUsagePage = 0x20;
	rid[3].usUsage = 0x00;
	rid[3].dwFlags = RIDEV_INPUTSINK | RIDEV_PAGEONLY | RIDEV_DEVNOTIFY;
	rid[3].hwndTarget = hwnd;

	if (RegisterRawInputDevices(rid, 4, sizeof(RAWINPUTDEVICE)))
		return true;

	return RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
}

void load_xinput_ex()
{
	const wchar_t *dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll"};
	for (auto *dll : dlls) {
		g.xinput_mod = LoadLibraryW(dll);
		if (!g.xinput_mod)
			continue;
		g.get_state_ex = (XInputGetStateExFn)GetProcAddress(g.xinput_mod, (LPCSTR)100);
		if (g.get_state_ex)
			return;
		FreeLibrary(g.xinput_mod);
		g.xinput_mod = nullptr;
	}
}

void thread_main()
{
	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.lpszClassName = kWndClass;
	RegisterClassExW(&wc);

	g.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, kWndClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
	if (g.hwnd)
		register_raw_input(g.hwnd);

	load_xinput_ex();

	blog(LOG_INFO, "[gamepad-plugin] runtime started (raw input %s, guide %s)", g.hwnd ? "on" : "off",
	     g.get_state_ex ? "on" : "off");

	bool hid_open = false;
	ULONGLONG next_hid_try = 0;
	while (g.running.load()) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		GamepadNeeds needs;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			needs = g.needs;
		}
		const bool want_hid = needs.gyro || needs.hid_tune || needs.hid_poll;
		gamesir_hid_set_capture(needs.gyro, needs.hid_tune, needs.hid_poll);
		const ULONGLONG now = GetTickCount64();
		if (want_hid && !hid_open) {
			if (now >= next_hid_try) {
				hid_open = gamesir_hid_start();
				next_hid_try = now + 1500;
			}
		} else if (!want_hid && hid_open) {
			gamesir_hid_stop();
			hid_open = false;
		}
		if (hid_open) {
			GyroSample hid_gyro{};
			if (gamesir_hid_poll(&hid_gyro) && hid_gyro.valid) {
				std::lock_guard<std::mutex> lock(g.mutex);
				g.gyro = hid_gyro;
				g.gyro_ms = now;
			}
		}
		for (int i = 0; i < kSlots; i++)
			poll_slot(i, now, false);
		attach_gyro(now);

		Sleep(needs.gyro || needs.jitter ? 4 : 12);
	}

	gamesir_hid_stop();
	for (int i = 0; i < kSlots; i++) {
		XINPUT_VIBRATION stop{};
		XInputSetState((DWORD)i, &stop);
	}

	if (g.hwnd) {
		RAWINPUTDEVICE rid[4]{};
		rid[0].usUsagePage = 0x01;
		rid[0].usUsage = 0x05;
		rid[0].dwFlags = RIDEV_REMOVE;
		rid[1].usUsagePage = 0x01;
		rid[1].usUsage = 0x04;
		rid[1].dwFlags = RIDEV_REMOVE;
		rid[2].usUsagePage = 0xFF00;
		rid[2].usUsage = 0x00;
		rid[2].dwFlags = RIDEV_REMOVE | RIDEV_PAGEONLY;
		rid[3].usUsagePage = 0x20;
		rid[3].usUsage = 0x00;
		rid[3].dwFlags = RIDEV_REMOVE | RIDEV_PAGEONLY;
		RegisterRawInputDevices(rid, 4, sizeof(RAWINPUTDEVICE));
		DestroyWindow(g.hwnd);
		g.hwnd = nullptr;
	}
	UnregisterClassW(kWndClass, inst);
	if (g.xinput_mod) {
		FreeLibrary(g.xinput_mod);
		g.xinput_mod = nullptr;
		g.get_state_ex = nullptr;
	}
}

} // namespace

void gamepad_runtime_set_needs(const GamepadNeeds &needs)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	g.needs = needs;
}

void gamepad_runtime_acquire()
{
	if (g.refs.fetch_add(1) == 0) {
		g.running = true;
		g.thread = std::thread(thread_main);
	}
}

void gamepad_runtime_release()
{
	if (g.refs.fetch_sub(1) == 1) {
		g.running = false;
		if (g.thread.joinable())
			g.thread.join();
	}
}

PadSnapshot gamepad_runtime_snapshot(int slot)
{
	slot = std::clamp(slot, 0, kSlots - 1);
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.pads[slot];
}

void gamepad_runtime_rumble(int slot, uint16_t left, uint16_t right, uint32_t duration_ms)
{
	slot = std::clamp(slot, 0, kSlots - 1);
	XINPUT_VIBRATION vib{};
	vib.wLeftMotorSpeed = left;
	vib.wRightMotorSpeed = right;
	XInputSetState((DWORD)slot, &vib);

	std::lock_guard<std::mutex> lock(g.mutex);
	g.rumble[slot].left = left;
	g.rumble[slot].right = right;
	g.rumble[slot].until_ms = GetTickCount64() + std::max<uint32_t>(duration_ms, 50);
	g.rumble[slot].active = true;
}

void gamepad_runtime_shutdown()
{
	g.running = false;
	if (g.thread.joinable())
		g.thread.join();
	g.refs = 0;
}
