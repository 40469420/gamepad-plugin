#include "gamesir-hid.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kGameSirVid = 0x3537;

struct Device {
	HANDLE handle = INVALID_HANDLE_VALUE;
	HANDLE event = nullptr;
	OVERLAPPED ov{};
	bool read_pending = false;
	DWORD in_len = 64;
	DWORD out_len = 64;
	uint8_t buf[128]{};
	uint16_t vid = 0;
	uint16_t pid = 0;
	ULONGLONG next_beat = 0;
	ULONGLONG next_mode = 0;
	ULONGLONG next_reg = 0;
	int reg_index = 0;
	bool waiting_reg = false;
	ULONGLONG reg_sent_at = 0;
	float tune[8] = {0, 100, 0, 100, 0, 100, 0, 100};
	bool have[8] = {};
	bool logged_tune = false;
	int poll_code = -1;
	int poll_set_hz = 0;
	ULONGLONG report_window_ms = 0;
	int report_count = 0;
	int poll_report_hz = 0;
};

Device g_dev;
std::mutex g_tune_mutex;
StickTune g_ls_tune;
StickTune g_rs_tune;
int g_poll_set_hz = 0;
int g_poll_report_hz = 0;
bool g_want_reports = true;
bool g_want_tune = true;
bool g_want_poll = true;
std::atomic<bool> g_reader_run{false};
std::thread g_reader;
GyroSample g_latest{};
bool g_latest_valid = false;
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_window{};

int poll_code_to_hz(int code)
{
	static const int kHz[] = {250, 500, 1000, 2000, 4000, 8000};
	if (code < 0 || code > 5)
		return 0;
	return kHz[code];
}

bool send_output(Device &d, const uint8_t *src, DWORD n);

/* G7 Pro 8K PC (0x10C5 Aimlabs / 0x10C7 Nioh): 16-bit BE percent*10 */
const uint16_t kTuneAddr[8] = {0x0397, 0x0399, 0x039B, 0x039D, 0x03BB, 0x03BD, 0x03BF, 0x03C1};

void publish_tune()
{
	StickTune ls{};
	StickTune rs{};
	ls.valid = g_dev.have[0] && g_dev.have[1];
	rs.valid = g_dev.have[4] && g_dev.have[5];
	ls.dz_min = g_dev.tune[0];
	ls.dz_max = g_dev.tune[1];
	ls.adz_min = g_dev.tune[2];
	ls.adz_max = g_dev.tune[3];
	rs.dz_min = g_dev.tune[4];
	rs.dz_max = g_dev.tune[5];
	rs.adz_min = g_dev.tune[6];
	rs.adz_max = g_dev.tune[7];
	ls.valid = ls.valid && g_dev.have[2] && g_dev.have[3];
	rs.valid = rs.valid && g_dev.have[6] && g_dev.have[7];
	std::lock_guard<std::mutex> lock(g_tune_mutex);
	if (ls.valid)
		g_ls_tune = ls;
	if (rs.valid)
		g_rs_tune = rs;
}

void request_next_reg(Device &d)
{
	if (d.waiting_reg)
		return;
	if (d.reg_index < 0 || d.reg_index > 8)
		d.reg_index = 0;
	if (d.reg_index < 8 && !g_want_tune)
		d.reg_index = 8;
	if (d.reg_index == 8 && !g_want_poll) {
		d.reg_index = 0;
		return;
	}
	uint16_t addr;
	uint8_t len;
	if (d.reg_index == 8) {
		addr = 0x002E;
		len = 1;
	} else {
		addr = kTuneAddr[d.reg_index];
		len = 2;
	}
	const uint8_t cmd[] = {0x0F, 0x04, 0x01, (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF), len};
	send_output(d, cmd, sizeof(cmd));
	d.waiting_reg = true;
	d.reg_sent_at = GetTickCount64();
}

void handle_reg_reply(const uint8_t *data, DWORD n)
{
	if (n < 7 || data[0] != 0x10 || data[1] != 0x05)
		return;
	const uint16_t addr = (uint16_t)((data[3] << 8) | data[4]);
	const int len = data[5];
	g_dev.waiting_reg = false;
	if (addr == 0x002E && len >= 1 && n >= 7) {
		g_dev.poll_code = data[6];
		g_dev.poll_set_hz = poll_code_to_hz(g_dev.poll_code);
		g_dev.reg_index = 0;
		std::lock_guard<std::mutex> lock(g_tune_mutex);
		g_poll_set_hz = g_dev.poll_set_hz;
		return;
	}
	if (len < 2 || n < 8)
		return;
	const float pct = std::clamp(((data[6] << 8) | data[7]) / 10.0f, 0.0f, 100.0f);
	for (int i = 0; i < 8; i++) {
		if (kTuneAddr[i] != addr)
			continue;
		g_dev.tune[i] = pct;
		g_dev.have[i] = true;
		g_dev.reg_index = (i + 1);
		publish_tune();
		if (!g_dev.logged_tune) {
			bool all = true;
			for (int j = 0; j < 8; j++)
				all = all && g_dev.have[j];
			if (all) {
				g_dev.logged_tune = true;
				blog(LOG_INFO,
				     "[gamepad-plugin] GameSir tune LS DZ %.1f-%.1f ADZ %.1f-%.1f | RS DZ %.1f-%.1f ADZ %.1f-%.1f",
				     g_dev.tune[0], g_dev.tune[1], g_dev.tune[2], g_dev.tune[3], g_dev.tune[4],
				     g_dev.tune[5], g_dev.tune[6], g_dev.tune[7]);
			}
		}
		return;
	}
}

bool path_is_xinput(const std::wstring &path)
{
	for (size_t i = 0; i + 2 < path.size(); i++) {
		wchar_t a = path[i] | 32, b = path[i + 1] | 32, c = path[i + 2];
		if (a == L'i' && b == L'g' && c == L'_')
			return true;
	}
	return false;
}

bool send_output(Device &d, const uint8_t *src, DWORD n)
{
	if (d.handle == INVALID_HANDLE_VALUE)
		return false;
	uint8_t pkt[128]{};
	DWORD len = d.out_len;
	if (len < n)
		len = n;
	if (len > sizeof(pkt))
		len = sizeof(pkt);
	std::memcpy(pkt, src, n);

	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	BOOL ok = WriteFile(d.handle, pkt, len, &written, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		WaitForSingleObject(ov.hEvent, 50);
		ok = GetOverlappedResult(d.handle, &ov, &written, FALSE);
	}
	if (ov.hEvent)
		CloseHandle(ov.hEvent);
	if (ok)
		return true;
	return HidD_SetOutputReport(d.handle, pkt, len) == TRUE;
}

void heartbeat(Device &d, ULONGLONG now)
{
	if (d.waiting_reg && now - d.reg_sent_at > 250)
		d.waiting_reg = false;
	if (g_want_tune || g_want_poll) {
		if (now >= d.next_reg) {
			request_next_reg(d);
			d.next_reg = now + (d.reg_index == 0 && d.logged_tune ? 2000 : 70);
		}
	}
	if (g_want_reports || g_want_tune || g_want_poll) {
		if (now >= d.next_beat) {
			const uint8_t beat[] = {0x0F, 0xF2};
			send_output(d, beat, sizeof(beat));
			d.next_beat = now + 400;
		}
	}
	if (g_want_reports && now >= d.next_mode) {
		const uint8_t mode[] = {0xA2, 0x01, 0x00};
		send_output(d, mode, sizeof(mode));
		d.next_mode = now + 2000;
	}
}

void close_device()
{
	if (g_dev.read_pending && g_dev.handle != INVALID_HANDLE_VALUE)
		CancelIo(g_dev.handle);
	if (g_dev.handle != INVALID_HANDLE_VALUE) {
		CloseHandle(g_dev.handle);
		g_dev.handle = INVALID_HANDLE_VALUE;
	}
	if (g_dev.event) {
		CloseHandle(g_dev.event);
		g_dev.event = nullptr;
	}
	g_dev = {};
	g_dev.handle = INVALID_HANDLE_VALUE;
}

struct Candidate {
	std::wstring path;
	USHORT vid = 0;
	USHORT pid = 0;
	USHORT usage_page = 0;
	USHORT in_len = 0;
	USHORT out_len = 0;
	int score = 0;
};

int score_caps(const HIDP_CAPS &caps, const std::wstring &path)
{
	if (path_is_xinput(path))
		return -1000;
	if (caps.UsagePage == 0x01 && (caps.Usage == 0x02 || caps.Usage == 0x06 || caps.Usage == 0x07))
		return -1000;
	int score = 0;
	if (caps.InputReportByteLength >= 64)
		score += 50;
	if (caps.OutputReportByteLength >= 32)
		score += 40;
	if (caps.UsagePage >= 0xFF00)
		score += 30;
	if (caps.InputReportByteLength == 64 && caps.OutputReportByteLength == 64)
		score += 20;
	return score;
}

std::vector<Candidate> enumerate_gamesir()
{
	std::vector<Candidate> out;
	GUID hid_guid;
	HidD_GetHidGuid(&hid_guid);
	HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE)
		return out;

	SP_DEVICE_INTERFACE_DATA iface{};
	iface.cbSize = sizeof(iface);
	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &iface); i++) {
		DWORD needed = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
		if (needed == 0)
			continue;
		std::vector<uint8_t> store(needed);
		auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(store.data());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, nullptr))
			continue;

		std::wstring path = detail->DevicePath;
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
				       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;

		HIDD_ATTRIBUTES attr{};
		attr.Size = sizeof(attr);
		PHIDP_PREPARSED_DATA preparsed = nullptr;
		HIDP_CAPS caps{};
		bool ok = HidD_GetAttributes(h, &attr) && attr.VendorID == kGameSirVid;
		if (ok)
			ok = HidD_GetPreparsedData(h, &preparsed) && preparsed &&
			     HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
		if (preparsed)
			HidD_FreePreparsedData(preparsed);
		CloseHandle(h);
		if (!ok)
			continue;

		Candidate c;
		c.path = std::move(path);
		c.vid = attr.VendorID;
		c.pid = attr.ProductID;
		c.usage_page = caps.UsagePage;
		c.in_len = caps.InputReportByteLength;
		c.out_len = caps.OutputReportByteLength;
		c.score = score_caps(caps, c.path);
		if (c.score > 0)
			out.push_back(std::move(c));
	}
	SetupDiDestroyDeviceInfoList(set);
	return out;
}

bool open_best()
{
	auto list = enumerate_gamesir();
	if (list.empty())
		return false;
	const Candidate *best = &list[0];
	for (auto &c : list) {
		if (c.score > best->score)
			best = &c;
	}

	HANDLE h = CreateFileW(best->path.c_str(), GENERIC_READ | GENERIC_WRITE,
			       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			       FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		h = CreateFileW(best->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	}
	if (h == INVALID_HANDLE_VALUE)
		return false;

	g_dev.handle = h;
	g_dev.vid = best->vid;
	g_dev.pid = best->pid;
	g_dev.in_len = best->in_len ? best->in_len : 64;
	g_dev.out_len = best->out_len ? best->out_len : 64;
	if (g_dev.in_len > sizeof(g_dev.buf))
		g_dev.in_len = sizeof(g_dev.buf);
	g_dev.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_dev.ov.hEvent = g_dev.event;
	g_dev.next_beat = 0;
	g_dev.next_mode = 0;
	HidD_SetNumInputBuffers(h, 256);
	blog(LOG_INFO, "[gamepad-plugin] GameSir HID opened vid=%04X pid=%04X in=%u out=%u score=%d",
	     g_dev.vid, g_dev.pid, (unsigned)g_dev.in_len, (unsigned)g_dev.out_len, best->score);
	return true;
}

bool start_read()
{
	if (g_dev.handle == INVALID_HANDLE_VALUE || g_dev.read_pending)
		return false;
	ResetEvent(g_dev.event);
	std::memset(g_dev.buf, 0, sizeof(g_dev.buf));
	g_dev.ov = {};
	g_dev.ov.hEvent = g_dev.event;
	DWORD got = 0;
	BOOL ok = ReadFile(g_dev.handle, g_dev.buf, g_dev.in_len, &got, &g_dev.ov);
	if (ok || GetLastError() == ERROR_IO_PENDING) {
		g_dev.read_pending = true;
		return true;
	}
	return false;
}

void note_input_report()
{
	if (!g_want_poll)
		return;
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	if (g_qpc_window.QuadPart == 0)
		g_qpc_window = now;
	g_dev.report_count++;
	const LONGLONG elapsed = now.QuadPart - g_qpc_window.QuadPart;
	if (g_qpc_freq.QuadPart > 0 && elapsed >= g_qpc_freq.QuadPart) {
		g_dev.poll_report_hz = (int)((g_dev.report_count * g_qpc_freq.QuadPart) / elapsed);
		g_dev.report_count = 0;
		g_qpc_window = now;
		std::lock_guard<std::mutex> lock(g_tune_mutex);
		g_poll_report_hz = g_dev.poll_report_hz;
	}
}

bool consume_report()
{
	DWORD got = 0;
	if (!GetOverlappedResult(g_dev.handle, &g_dev.ov, &got, FALSE)) {
		g_dev.read_pending = false;
		return false;
	}
	g_dev.read_pending = false;
	if (g_want_tune || g_want_poll)
		handle_reg_reply(g_dev.buf, got);
	if (got > 0 && g_dev.buf[0] == 0x12)
		note_input_report();
	if (g_want_reports) {
		GyroSample sample{};
		if (hid_parse_motion(g_dev.vid, g_dev.pid, g_dev.buf, got, &sample) && sample.valid) {
			std::lock_guard<std::mutex> lock(g_tune_mutex);
			g_latest = sample;
			g_latest_valid = true;
		}
	}
	return true;
}

void reader_main()
{
	start_read();
	while (g_reader_run.load()) {
		if (!g_dev.read_pending)
			start_read();
		const DWORD wr = g_dev.event ? WaitForSingleObject(g_dev.event, 25) : WAIT_TIMEOUT;
		if (wr == WAIT_OBJECT_0 && g_dev.read_pending) {
			if (!consume_report()) {
				const DWORD err = GetLastError();
				if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE) {
					blog(LOG_INFO, "[gamepad-plugin] GameSir HID disconnected");
					break;
				}
			}
			start_read();
			while (g_reader_run.load() && g_dev.read_pending &&
			       WaitForSingleObject(g_dev.event, 0) == WAIT_OBJECT_0) {
				if (!consume_report())
					break;
				start_read();
			}
		}
		heartbeat(g_dev, GetTickCount64());
	}
}

void stop_reader()
{
	g_reader_run = false;
	if (g_dev.handle != INVALID_HANDLE_VALUE)
		CancelIo(g_dev.handle);
	if (g_dev.event)
		SetEvent(g_dev.event);
	if (g_reader.joinable())
		g_reader.join();
}

} // namespace

bool gamesir_hid_start()
{
	stop_reader();
	close_device();
	if (!open_best()) {
		blog(LOG_INFO, "[gamepad-plugin] GameSir vendor HID not opened (gyro will stay unavailable until it is)");
		return false;
	}
	QueryPerformanceFrequency(&g_qpc_freq);
	g_qpc_window = {};
	g_dev.report_count = 0;
	g_latest_valid = false;
	g_reader_run = true;
	g_reader = std::thread(reader_main);
	return true;
}

void gamesir_hid_stop()
{
	stop_reader();
	close_device();
}

void gamesir_hid_set_capture(bool reports, bool tune, bool poll)
{
	g_want_reports = reports;
	g_want_tune = tune;
	g_want_poll = poll;
}

bool gamesir_hid_poll(GyroSample *out)
{
	if (!out)
		return false;
	std::lock_guard<std::mutex> lock(g_tune_mutex);
	if (!g_latest_valid)
		return false;
	*out = g_latest;
	return true;
}

void gamesir_hid_copy_tune(StickTune *ls, StickTune *rs)
{
	std::lock_guard<std::mutex> lock(g_tune_mutex);
	if (ls)
		*ls = g_ls_tune;
	if (rs)
		*rs = g_rs_tune;
}

void gamesir_hid_copy_poll(int *set_hz, int *report_hz)
{
	std::lock_guard<std::mutex> lock(g_tune_mutex);
	if (set_hz)
		*set_hz = g_poll_set_hz;
	if (report_hz)
		*report_hz = g_poll_report_hz;
}
