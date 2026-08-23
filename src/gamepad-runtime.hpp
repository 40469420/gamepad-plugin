#pragma once

#include <cstdint>

struct StickTune {
	bool valid = false;
	float dz_min = 0;
	float dz_max = 100;
	float adz_min = 0;
	float adz_max = 100;
	float jitter = 0;
};

struct PadSnapshot {
	bool connected = false;
	uint32_t packet = 0;
	uint16_t buttons = 0;
	uint8_t lt = 0;
	uint8_t rt = 0;
	int16_t lx = 0;
	int16_t ly = 0;
	int16_t rx = 0;
	int16_t ry = 0;
	float lx_n = 0;
	float ly_n = 0;
	float rx_n = 0;
	float ry_n = 0;
	bool gyro_valid = false;
	int16_t gyro_raw[3] = {0, 0, 0};
	int16_t accel_raw[3] = {0, 0, 0};
	float gyro_dps[3] = {0, 0, 0};
	float accel_g[3] = {0, 0, 0};
	StickTune ls_tune;
	StickTune rs_tune;
	int poll_set_hz = 0;
	int poll_report_hz = 0;
	char name[64] = "No controller";
	char gyro_name[32] = "none";
};

struct GamepadNeeds {
	bool gyro = false;
	bool hid_tune = false;
	bool hid_poll = false;
	bool jitter = false;
};

void gamepad_runtime_acquire();
void gamepad_runtime_release();
void gamepad_runtime_set_needs(const GamepadNeeds &needs);
PadSnapshot gamepad_runtime_snapshot(int slot);
void gamepad_runtime_rumble(int slot, uint16_t left, uint16_t right, uint32_t duration_ms);
void gamepad_runtime_shutdown();
