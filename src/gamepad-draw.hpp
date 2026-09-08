#pragma once

#include "gamepad-runtime.hpp"

#include <cstdint>

struct StickTrail {
	static constexpr int kMax = 40;
	float x[kMax]{};
	float y[kMax]{};
	uint64_t t[kMax]{};
	int n = 0;
	int head = 0;
	uint64_t now_ms = 0;

	void push(float nx, float ny, uint64_t now)
	{
		now_ms = now;
		if (n > 0) {
			const int last = (head + kMax - 1) % kMax;
			const float dx = nx - x[last];
			const float dy = ny - y[last];
			if (dx * dx + dy * dy < 0.0002f)
				return;
		}
		x[head] = nx;
		y[head] = ny;
		t[head] = now;
		head = (head + 1) % kMax;
		if (n < kMax)
			n++;
	}

	void expire(uint64_t now, float life_sec)
	{
		now_ms = now;
		if (life_sec < 0.2f)
			life_sec = 0.2f;
		if (life_sec > 8.0f)
			life_sec = 8.0f;
		const uint64_t life = (uint64_t)(life_sec * 1000.0f);
		while (n > 0) {
			const int oldest = (head + kMax - n) % kMax;
			if (now >= t[oldest] && now - t[oldest] >= life)
				n--;
			else
				break;
		}
		if (n <= 0) {
			n = 0;
			head = 0;
		}
	}

	void clear()
	{
		n = 0;
		head = 0;
		now_ms = 0;
	}
};

struct GamepadDrawSettings {
	bool show_background = false;
	bool show_status = true;
	bool show_left_stick = true;
	bool show_left_stick_numbers = true;
	bool show_right_stick = true;
	bool show_right_stick_numbers = true;
	bool show_dpad = true;
	bool show_face_buttons = true;
	bool show_bumpers = true;
	bool show_menu_buttons = true;
	bool show_triggers = true;
	bool show_trigger_numbers = true;
	bool show_gyro = true;
	bool show_gyro_numbers = true;
	bool show_deadzone_bars = true;
	bool show_anti_deadzone_bars = true;
	bool show_jitter_bars = true;
	bool show_trails = true;
	float deadzone = 0.0f;
	int trail_len = 28;
	float trail_life = 2.2f;
	float trail_thick = 1.0f;
	int slot = 0;

	uint32_t col_ls = 0x00FFDC3D;
	uint32_t col_rs = 0x00D56AFF;
	uint32_t col_a = 0x003FBB7C;
	uint32_t col_b = 0x003F3CE0;
	uint32_t col_x = 0x00DB8C3B;
	uint32_t col_y = 0x0047C5E8;
	uint32_t col_dpad = 0x0066D1FF;

	int pos_ls_x = 0, pos_ls_y = 0;
	int pos_rs_x = 0, pos_rs_y = 0;
	int pos_face_x = 0, pos_face_y = 0;
	int pos_dpad_x = 0, pos_dpad_y = 0;
	int pos_gyro_x = 0, pos_gyro_y = 0;
	int pos_trig_x = 0, pos_trig_y = 0;
	int pos_menu_x = 0, pos_menu_y = 0;
	int pos_bump_x = 0, pos_bump_y = 0;
	int pos_bars_x = 0, pos_bars_y = 0;
};

void gamepad_draw_frame(uint8_t *bgra, int width, int height, int stride, const PadSnapshot &pad,
			const GamepadDrawSettings &settings, const StickTrail *ls_trail, const StickTrail *rs_trail);
