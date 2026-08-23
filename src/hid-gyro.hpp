#pragma once

#include <cstdint>
#include <cstddef>

struct GyroSample {
	bool valid = false;
	bool has_sticks = false;
	int16_t stick_lx = 0;
	int16_t stick_ly = 0;
	int16_t stick_rx = 0;
	int16_t stick_ry = 0;
	int16_t gyro_raw[3] = {0, 0, 0};
	int16_t accel_raw[3] = {0, 0, 0};
	float gyro_dps[3] = {0, 0, 0};
	float accel_g[3] = {0, 0, 0};
	const char *device_name = "Motion";
};

bool hid_parse_motion(uint16_t vid, uint16_t pid, const uint8_t *data, size_t size, GyroSample *out);
const char *hid_device_name(uint16_t vid, uint16_t pid);
