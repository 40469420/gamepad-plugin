#pragma once

#include "gamepad-runtime.hpp"

#include <cstdint>

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
	float deadzone = 0.0f;
	int slot = 0;
};

void gamepad_draw_frame(uint8_t *bgra, int width, int height, int stride, const PadSnapshot &pad,
			const GamepadDrawSettings &settings);
