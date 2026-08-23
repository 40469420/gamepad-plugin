#include "hid-gyro.hpp"

#include <cstring>

namespace {

int16_t le16(const uint8_t *p)
{
	return (int16_t)(p[0] | ((uint16_t)p[1] << 8));
}

int16_t be16(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t u8_stick(uint8_t v)
{
	int centered = (int)v - 128;
	if (centered == 0)
		return 0;
	if (centered > 0)
		return (int16_t)((centered * 32767) / 127);
	return (int16_t)((centered * 32768) / 128);
}

void set_axes(GyroSample *out, const int16_t *gyro, const int16_t *accel, float gyro_scale, float accel_scale)
{
	for (int i = 0; i < 3; i++) {
		out->gyro_raw[i] = gyro[i];
		out->accel_raw[i] = accel[i];
		out->gyro_dps[i] = (float)gyro[i] * gyro_scale;
		out->accel_g[i] = (float)accel[i] * accel_scale;
	}
	out->valid = true;
}

bool is_sony(uint16_t vid)
{
	return vid == 0x054C;
}

bool is_ds4(uint16_t pid)
{
	return pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;
}

bool is_dualsense(uint16_t pid)
{
	return pid == 0x0CE6 || pid == 0x0DF2;
}

bool is_gamesir(uint16_t vid)
{
	return vid == 0x3537;
}

bool is_switch_pro(uint16_t vid, uint16_t pid)
{
	return vid == 0x057E && (pid == 0x2009 || pid == 0x200E);
}

bool parse_ds4(const uint8_t *data, size_t size, GyroSample *out)
{
	size_t base = 0;
	if (size < 25)
		return false;
	if (data[0] == 0x11) {
		base = 2;
		if (size < base + 25)
			return false;
	} else if (data[0] != 0x01) {
		return false;
	}

	const uint8_t *r = data + base;
	out->has_sticks = true;
	out->stick_lx = u8_stick(r[1]);
	out->stick_ly = (int16_t)-u8_stick(r[2]);
	out->stick_rx = u8_stick(r[3]);
	out->stick_ry = (int16_t)-u8_stick(r[4]);

	int16_t gyro[3] = {le16(r + 13), le16(r + 15), le16(r + 17)};
	int16_t accel[3] = {le16(r + 19), le16(r + 21), le16(r + 23)};
	set_axes(out, gyro, accel, 1.0f / 1024.0f, 1.0f / 8192.0f);
	out->device_name = "DualShock 4";
	return true;
}

bool parse_dualsense(const uint8_t *data, size_t size, GyroSample *out)
{
	size_t base = 0;
	if (data[0] == 0x31) {
		base = 1;
	} else if (data[0] != 0x01) {
		return false;
	}
	if (size < base + 28)
		return false;

	const uint8_t *r = data + base;
	out->has_sticks = true;
	out->stick_lx = u8_stick(r[1]);
	out->stick_ly = (int16_t)-u8_stick(r[2]);
	out->stick_rx = u8_stick(r[3]);
	out->stick_ry = (int16_t)-u8_stick(r[4]);

	int16_t gyro[3] = {le16(r + 16), le16(r + 18), le16(r + 20)};
	int16_t accel[3] = {le16(r + 22), le16(r + 24), le16(r + 26)};
	set_axes(out, gyro, accel, 1.0f / 1024.0f, 1.0f / 8192.0f);
	out->device_name = "DualSense";
	return true;
}

bool parse_gamesir_sdl(const uint8_t *data, size_t size, GyroSample *out)
{
	const uint8_t *payload = nullptr;
	size_t payload_size = 0;

	if (size >= 3 && data[0] == 0x43 && data[1] == 0xA1 && data[2] == 0xC8) {
		payload = data + 3;
		payload_size = size - 3;
	} else if (size >= 2 && data[0] == 0xA1 && data[1] == 0xC8) {
		payload = data + 2;
		payload_size = size - 2;
	}
	if (!payload || payload_size < 26)
		return false;

	out->has_sticks = true;
	out->stick_lx = be16(payload + 4);
	out->stick_ly = (int16_t)-be16(payload + 6);
	out->stick_rx = be16(payload + 8);
	out->stick_ry = (int16_t)-be16(payload + 10);

	int16_t accel[3] = {be16(payload + 14), be16(payload + 16), be16(payload + 18)};
	int16_t gyro[3] = {be16(payload + 20), be16(payload + 22), be16(payload + 24)};
	set_axes(out, gyro, accel, 1.0f / 16.0f, 4.0f / 32768.0f);
	out->device_name = "GameSir";
	return true;
}

bool parse_gamesir_12(const uint8_t *data, size_t size, GyroSample *out)
{
	if (size < 26)
		return false;
	const uint8_t *r = data;
	if (data[0] == 0x12) {
		r = data;
	} else {
		return false;
	}

	out->has_sticks = true;
	out->stick_lx = u8_stick(r[1]);
	out->stick_ly = (int16_t)-u8_stick(r[2]);
	out->stick_rx = u8_stick(r[3]);
	out->stick_ry = (int16_t)-u8_stick(r[4]);

	int16_t gyro_le[3] = {le16(r + 14), le16(r + 16), le16(r + 18)};
	int16_t accel_le[3] = {le16(r + 20), le16(r + 22), le16(r + 24)};
	int16_t gyro_be[3] = {be16(r + 14), be16(r + 16), be16(r + 18)};
	int16_t accel_be[3] = {be16(r + 20), be16(r + 22), be16(r + 24)};

	auto mag = [](const int16_t *v) {
		auto a = [](int16_t x) { return x < 0 ? -(int)x : (int)x; };
		return a(v[0]) + a(v[1]) + a(v[2]);
	};
	const bool use_be = mag(gyro_be) <= mag(gyro_le);
	if (use_be)
		set_axes(out, gyro_be, accel_be, 1.0f / 16.0f, 4.0f / 32768.0f);
	else
		set_axes(out, gyro_le, accel_le, 1.0f / 16.0f, 4.0f / 32768.0f);
	out->device_name = "GameSir G7";
	return true;
}

bool parse_gamesir(const uint8_t *data, size_t size, GyroSample *out)
{
	if (parse_gamesir_12(data, size, out))
		return true;
	if (parse_gamesir_sdl(data, size, out))
		return true;
	return false;
}

bool parse_switch(const uint8_t *data, size_t size, GyroSample *out)
{
	if (size < 49 || data[0] != 0x30)
		return false;

	const uint8_t *sample = data + 13 + 24;
	int16_t accel[3] = {le16(sample + 0), le16(sample + 2), le16(sample + 4)};
	int16_t gyro[3] = {le16(sample + 6), le16(sample + 8), le16(sample + 10)};
	set_axes(out, gyro, accel, 2000.0f / 32768.0f, 8.0f / 32768.0f);
	out->device_name = "Switch Pro";
	return true;
}

} // namespace

const char *hid_device_name(uint16_t vid, uint16_t pid)
{
	if (is_sony(vid) && is_dualsense(pid))
		return "DualSense";
	if (is_sony(vid) && is_ds4(pid))
		return "DualShock 4";
	if (is_gamesir(vid))
		return "GameSir G7";
	if (is_switch_pro(vid, pid))
		return "Switch Pro";
	return "HID gamepad";
}

bool hid_parse_motion(uint16_t vid, uint16_t pid, const uint8_t *data, size_t size, GyroSample *out)
{
	if (!data || !out || size < 8)
		return false;

	GyroSample sample{};
	bool ok = false;

	if (is_sony(vid) && is_dualsense(pid))
		ok = parse_dualsense(data, size, &sample);
	else if (is_sony(vid) && is_ds4(pid))
		ok = parse_ds4(data, size, &sample);
	else if (is_gamesir(vid))
		ok = parse_gamesir(data, size, &sample);
	else if (is_switch_pro(vid, pid))
		ok = parse_switch(data, size, &sample);
	else if (parse_gamesir(data, size, &sample))
		ok = true;
	else if (data[0] == 0x01 && size >= 28 && parse_dualsense(data, size, &sample))
		ok = true;
	else if (data[0] == 0x01 && parse_ds4(data, size, &sample))
		ok = true;

	if (!ok)
		return false;

	sample.device_name = hid_device_name(vid, pid);
	*out = sample;
	return true;
}
