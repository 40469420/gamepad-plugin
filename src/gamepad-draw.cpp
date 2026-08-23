#include "gamepad-draw.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

using namespace Gdiplus;

namespace {

ULONG_PTR gdi_token = 0;
std::once_flag gdi_once;

void ensure_gdiplus()
{
	std::call_once(gdi_once, []() {
		GdiplusStartupInput input;
		GdiplusStartup(&gdi_token, &input, nullptr);
	});
}

Color argb(BYTE a, BYTE r, BYTE g, BYTE b)
{
	return Color(a, r, g, b);
}

std::wstring utf16(const char *text)
{
	if (!text || !*text)
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	int count = n > 1 ? n : 1;
	std::wstring out((size_t)count, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
	if (!out.empty() && out.back() == L'\0')
		out.pop_back();
	return out;
}

bool down(const PadSnapshot &pad, uint16_t mask)
{
	return pad.connected && (pad.buttons & mask) != 0;
}

void draw_outlined(Graphics &g, const FontFamily &family, REAL size, const Color &fill, const RectF &layout,
		   const wchar_t *text)
{
	if (!text || !*text)
		return;
	StringFormat fmt;
	fmt.SetAlignment(layout.Width > 1 ? StringAlignmentCenter : StringAlignmentNear);
	fmt.SetLineAlignment(layout.Height > 1 ? StringAlignmentCenter : StringAlignmentNear);
	fmt.SetFormatFlags(StringFormatFlagsNoClip);

	GraphicsPath path;
	if (layout.Width > 1 && layout.Height > 1)
		path.AddString(text, -1, &family, FontStyleBold, size, layout, &fmt);
	else
		path.AddString(text, -1, &family, FontStyleBold, size, PointF(layout.X, layout.Y), &fmt);

	Pen outline(argb(255, 0, 0, 0), std::max(2.4f, size * 0.14f));
	outline.SetLineJoin(LineJoinRound);
	g.DrawPath(&outline, &path);
	SolidBrush brush(fill);
	g.FillPath(&brush, &path);
}

void draw_text(Graphics &g, const FontFamily &family, REAL size, const Color &fill, float x, float y, const char *text,
	       bool center = false)
{
	auto w = utf16(text);
	if (center) {
		draw_outlined(g, family, size, fill, RectF(x - 500, y, 1000, size * 1.45f), w.c_str());
	} else {
		draw_outlined(g, family, size, fill, RectF(x, y, 0, 0), w.c_str());
	}
}

void fill_round_rect(Graphics &g, const Brush &brush, float x, float y, float w, float h, float rad)
{
	GraphicsPath path;
	rad = std::min(rad, std::min(w, h) * 0.5f);
	path.AddArc(x, y, rad * 2, rad * 2, 180, 90);
	path.AddArc(x + w - rad * 2, y, rad * 2, rad * 2, 270, 90);
	path.AddArc(x + w - rad * 2, y + h - rad * 2, rad * 2, rad * 2, 0, 90);
	path.AddArc(x, y + h - rad * 2, rad * 2, rad * 2, 90, 90);
	path.CloseFigure();
	g.FillPath(&brush, &path);
}

void stroke_round_rect(Graphics &g, const Pen &pen, float x, float y, float w, float h, float rad)
{
	GraphicsPath path;
	rad = std::min(rad, std::min(w, h) * 0.5f);
	path.AddArc(x, y, rad * 2, rad * 2, 180, 90);
	path.AddArc(x + w - rad * 2, y, rad * 2, rad * 2, 270, 90);
	path.AddArc(x + w - rad * 2, y + h - rad * 2, rad * 2, rad * 2, 0, 90);
	path.AddArc(x, y + h - rad * 2, rad * 2, rad * 2, 90, 90);
	path.CloseFigure();
	g.DrawPath(&pen, &path);
}

void draw_stick(Graphics &g, float cx, float cy, float r, float nx, float ny, float deadzone, const Color &accent,
		bool click, bool connected)
{
	SolidBrush plate(argb(70, 12, 16, 24));
	g.FillEllipse(&plate, cx - r, cy - r, r * 2, r * 2);

	Pen ring(click ? accent : argb(220, 226, 232, 240), std::max(2.0f, r * 0.03f));
	g.DrawEllipse(&ring, cx - r, cy - r, r * 2, r * 2);

	Pen cross(argb(55, 255, 255, 255), 1.5f);
	g.DrawLine(&cross, cx - r * 0.78f, cy, cx + r * 0.78f, cy);
	g.DrawLine(&cross, cx, cy - r * 0.78f, cx, cy + r * 0.78f);

	if (deadzone > 0.01f) {
		float dz = r * deadzone;
		Pen dzp(argb(90, 255, 200, 80), 1.5f);
		g.DrawEllipse(&dzp, cx - dz, cy - dz, dz * 2, dz * 2);
	}

	if (!connected) {
		nx = 0;
		ny = 0;
	}
	nx = std::clamp(nx, -1.0f, 1.0f);
	ny = std::clamp(ny, -1.0f, 1.0f);
	const float sx = cx + nx * r * 0.86f;
	const float sy = cy - ny * r * 0.86f;

	Pen line(accent, std::max(3.0f, r * 0.045f));
	line.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
	g.DrawLine(&line, cx, cy, sx, sy);

	SolidBrush center(argb(220, 240, 244, 250));
	float csz = std::max(5.0f, r * 0.06f);
	g.FillEllipse(&center, cx - csz, cy - csz, csz * 2, csz * 2);

	SolidBrush nub(accent);
	float nsz = click ? std::max(12.0f, r * 0.14f) : std::max(10.0f, r * 0.11f);
	g.FillEllipse(&nub, sx - nsz, sy - nsz, nsz * 2, nsz * 2);
	Pen nub_ring(argb(255, 255, 255, 255), 1.6f);
	g.DrawEllipse(&nub_ring, sx - nsz, sy - nsz, nsz * 2, nsz * 2);
}

void draw_face_button(Graphics &g, const FontFamily &family, REAL size, float cx, float cy, float r,
		      const wchar_t *label, bool pressed, Color color)
{
	SolidBrush fill(pressed ? color : argb(55, 18, 20, 28));
	g.FillEllipse(&fill, cx - r, cy - r, r * 2, r * 2);
	Pen outline(pressed ? argb(255, 255, 255, 255) : color, pressed ? 3.0f : 2.0f);
	g.DrawEllipse(&outline, cx - r, cy - r, r * 2, r * 2);
	draw_outlined(g, family, size, pressed ? argb(255, 16, 18, 22) : argb(240, 244, 247, 251),
		      RectF(cx - r, cy - r, r * 2, r * 2), label);
}

void draw_small_button(Graphics &g, const FontFamily &family, REAL size, float cx, float cy, float w, float h,
		       const wchar_t *label, bool pressed)
{
	float x = cx - w * 0.5f;
	float y = cy - h * 0.5f;
	SolidBrush fill(pressed ? argb(255, 255, 209, 102) : argb(55, 18, 20, 28));
	fill_round_rect(g, fill, x, y, w, h, h * 0.35f);
	Pen outline(pressed ? argb(255, 255, 255, 255) : argb(160, 180, 188, 200), pressed ? 2.0f : 1.2f);
	stroke_round_rect(g, outline, x, y, w, h, h * 0.35f);
	draw_outlined(g, family, size, pressed ? argb(255, 20, 22, 28) : argb(240, 244, 247, 251), RectF(x, y, w, h),
		      label);
}

void draw_dpad(Graphics &g, float cx, float cy, float arm, bool u, bool d, bool l, bool r)
{
	const float t = arm * 0.42f;
	auto pad = [&](float x, float y, float w, float h, bool on) {
		SolidBrush fill(on ? argb(255, 255, 209, 102) : argb(55, 18, 20, 28));
		fill_round_rect(g, fill, x, y, w, h, 6);
		Pen outline(on ? argb(255, 255, 255, 255) : argb(160, 180, 188, 200), on ? 2.0f : 1.2f);
		stroke_round_rect(g, outline, x, y, w, h, 6);
	};
	pad(cx - t * 0.5f, cy - arm, t, arm + t * 0.5f, u);
	pad(cx - t * 0.5f, cy - t * 0.5f, t, arm + t * 0.5f, d);
	pad(cx - arm, cy - t * 0.5f, arm + t * 0.5f, t, l);
	pad(cx - t * 0.5f, cy - t * 0.5f, arm + t * 0.5f, t, r);
}

void draw_gyro(Graphics &g, float cx, float cy, float r, const PadSnapshot &pad)
{
	SolidBrush plate(argb(70, 12, 16, 24));
	g.FillEllipse(&plate, cx - r, cy - r, r * 2, r * 2);
	Pen ring(argb(230, 255, 193, 77), std::max(2.0f, r * 0.03f));
	g.DrawEllipse(&ring, cx - r, cy - r, r * 2, r * 2);
	Pen cross(argb(55, 255, 255, 255), 1.5f);
	g.DrawLine(&cross, cx - r * 0.78f, cy, cx + r * 0.78f, cy);
	g.DrawLine(&cross, cx, cy - r * 0.78f, cx, cy + r * 0.78f);
	if (!pad.gyro_valid)
		return;

	const float scale = r * 0.85f / 90.0f;
	float gx = std::clamp(pad.gyro_dps[2] * scale, -r * 0.86f, r * 0.86f);
	float gy = std::clamp(-pad.gyro_dps[0] * scale, -r * 0.86f, r * 0.86f);
	Pen line(argb(255, 255, 193, 77), std::max(3.0f, r * 0.045f));
	line.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
	g.DrawLine(&line, cx, cy, cx + gx, cy + gy);
	SolidBrush nub(argb(255, 255, 193, 77));
	float nsz = std::max(8.0f, r * 0.1f);
	g.FillEllipse(&nub, cx + gx - nsz, cy + gy - nsz, nsz * 2, nsz * 2);
}

void draw_vtrack(Graphics &g, float x, float y, float w, float h)
{
	SolidBrush back(argb(80, 12, 16, 24));
	fill_round_rect(g, back, x, y, w, h, 5);
	Pen outline(argb(200, 0, 0, 0), 2.0f);
	stroke_round_rect(g, outline, x, y, w, h, 5);
}

void draw_sideways(Graphics &g, const FontFamily &family, REAL size, float cx, float cy, const char *text)
{
	auto w = utf16(text);
	GraphicsState st = g.Save();
	g.TranslateTransform(cx, cy);
	g.RotateTransform(-90.0f);
	draw_outlined(g, family, size, argb(240, 244, 247, 251), RectF(-160.0f, -size * 0.7f, 320.0f, size * 1.4f),
		      w.c_str());
	g.Restore(st);
}

void draw_zone_bar(Graphics &g, const FontFamily &family, REAL size, const Color &fill_c, float x, float y, float w,
		   float h, float zmin, float zmax, const char *label)
{
	zmin = std::clamp(zmin, 0.0f, 100.0f);
	zmax = std::clamp(zmax, 0.0f, 100.0f);
	if (zmax < zmin)
		std::swap(zmin, zmax);
	draw_vtrack(g, x, y, w, h);
	const float y1 = y + h * (1.0f - zmax / 100.0f);
	const float y0 = y + h * (1.0f - zmin / 100.0f);
	const float fh = std::max(1.0f, y0 - y1);
	SolidBrush fill(fill_c);
	fill_round_rect(g, fill, x + 2, y1, w - 4, fh, 3);
	draw_text(g, family, size * 0.78f, argb(240, 244, 247, 251), x + w * 0.5f, y + h + 4, label, true);
	char range[24];
	std::snprintf(range, sizeof(range), "%.0f-%.0f", zmin, zmax);
	draw_sideways(g, family, size * 0.72f, x + w + size * 0.85f, y + h * 0.5f, range);
}

void draw_jitter_bar(Graphics &g, const FontFamily &family, REAL size, float x, float y, float w, float h, float pct,
		     const char *label)
{
	pct = std::clamp(pct, 0.0f, 100.0f);
	draw_vtrack(g, x, y, w, h);
	const float fh = std::max(1.0f, (h - 4) * pct / 100.0f);
	SolidBrush fill(argb(230, 255, 193, 77));
	fill_round_rect(g, fill, x + 2, y + h - 2 - fh, w - 4, fh, 3);
	draw_text(g, family, size * 0.78f, argb(240, 244, 247, 251), x + w * 0.5f, y + h + 4, label, true);
	char val[24];
	std::snprintf(val, sizeof(val), "%.1f", pct);
	draw_sideways(g, family, size * 0.72f, x + w + size * 0.85f, y + h * 0.5f, val);
}

void draw_trigger(Graphics &g, float x, float y, float w, float h, float t, bool digital)
{
	t = std::clamp(t, 0.0f, 1.0f);
	SolidBrush back(argb(70, 18, 20, 28));
	fill_round_rect(g, back, x, y, w, h, 6);
	Pen outline(digital ? argb(255, 255, 255, 255) : argb(160, 180, 188, 200), digital ? 2.0f : 1.2f);
	stroke_round_rect(g, outline, x, y, w, h, 6);
	if (t > 0.01f) {
		SolidBrush fill(argb(230, 124, 255, 177));
		fill_round_rect(g, fill, x + 3, y + 3, std::max(0.0f, (w - 6) * t), h - 6, 4);
	}
}

} // namespace

void gamepad_draw_frame(uint8_t *bgra, int width, int height, int stride, const PadSnapshot &pad,
			const GamepadDrawSettings &settings)
{
	ensure_gdiplus();
	if (!bgra || width < 64 || height < 64)
		return;

	std::memset(bgra, 0, (size_t)stride * (size_t)height);

	Bitmap bmp(width, height, stride, PixelFormat32bppARGB, bgra);
	Graphics g(&bmp);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

	const float W = (float)width;
	const float H = (float)height;
	const float S = std::min(W, H);

	if (settings.show_background) {
		SolidBrush bg(argb(140, 8, 10, 16));
		g.FillRectangle(&bg, 0.0f, 0.0f, W, H);
	}

	FontFamily family(L"Segoe UI");
	const float title_sz = std::max(19.0f, S * 0.036f);
	const float body_sz = std::max(16.0f, S * 0.029f);
	const float face_sz = std::max(17.0f, S * 0.033f);
	const float label_sz = std::max(14.0f, S * 0.025f);
	const Color text_c = argb(240, 244, 247, 251);
	const Color warn_c = argb(240, 255, 180, 90);

	/* Fixed slots. Disabled items leave a hole; text lives in reserved lanes. */
	const float y0 = settings.show_status ? std::max(44.0f, H * 0.06f) : 0.0f;
	const float lt_x = W * 0.07f, rt_x = W * 0.58f, trig_y = H * 0.055f + y0, trig_w = W * 0.32f, trig_h = S * 0.032f;
	const float lb_cx = W * 0.20f, rb_cx = W * 0.80f, bumper_y = H * 0.16f + y0;
	const float ls_cx = W * 0.20f, ls_cy = H * 0.39f + y0, ls_r = S * 0.13f;
	const float rs_cx = W * 0.80f, rs_cy = H * 0.72f + y0, rs_r = S * 0.12f;
	const float dpad_arm = S * 0.075f;
	const float dpad_cx = W * 0.20f;
	const float dpad_cy = rs_cy + rs_r - dpad_arm;
	const float face_cx = W * 0.80f, face_cy = H * 0.36f + y0, face_r = S * 0.04f, face_spread = S * 0.074f;
	const float view_cx = W * 0.42f, guide_cx = W * 0.50f, menu_cx = W * 0.58f, menu_cy = H * 0.24f + y0;
	const float gyro_cx = W * 0.50f, gyro_cy = ls_cy, gyro_r = S * 0.085f;

	if (settings.show_status) {
		char header[220];
		if (pad.connected && (pad.poll_set_hz > 0 || pad.poll_report_hz > 0)) {
			std::snprintf(header, sizeof(header), "Machi's controller overlay   %d Hz set / %d Hz",
				      pad.poll_set_hz, pad.poll_report_hz);
		} else {
			std::snprintf(header, sizeof(header), "Machi's controller overlay");
		}
		draw_text(g, family, title_sz, pad.connected ? text_c : warn_c, 18, 8, header);
	}

	if (settings.show_triggers) {
		draw_trigger(g, lt_x, trig_y, trig_w, trig_h, pad.lt / 255.0f, pad.lt > 30);
		draw_trigger(g, rt_x, trig_y, trig_w, trig_h, pad.rt / 255.0f, pad.rt > 30);
		draw_text(g, family, label_sz, text_c, lt_x, trig_y - S * 0.030f, "LT");
		draw_text(g, family, label_sz, text_c, rt_x, trig_y - S * 0.030f, "RT");
	}
	if (settings.show_trigger_numbers) {
		char buf[48];
		std::snprintf(buf, sizeof(buf), "%u  %.3f", pad.lt, pad.lt / 255.0f);
		draw_text(g, family, body_sz, text_c, lt_x + trig_w + 10, trig_y - 2, buf);
		std::snprintf(buf, sizeof(buf), "%u  %.3f", pad.rt, pad.rt / 255.0f);
		draw_text(g, family, body_sz, text_c, rt_x + trig_w + 10, trig_y - 2, buf);
	}

	if (settings.show_bumpers) {
		draw_small_button(g, family, label_sz, lb_cx, bumper_y, S * 0.16f, S * 0.048f, L"LB", down(pad, 0x0100));
		draw_small_button(g, family, label_sz, rb_cx, bumper_y, S * 0.16f, S * 0.048f, L"RB", down(pad, 0x0200));
	}

	if (settings.show_menu_buttons) {
		draw_small_button(g, family, label_sz, view_cx, menu_cy, S * 0.10f, S * 0.042f, L"Follow",
				  down(pad, 0x0020));
		draw_small_button(g, family, label_sz, guide_cx, menu_cy, S * 0.15f, S * 0.048f, L"Machi's",
				  down(pad, 0x0400));
		draw_small_button(g, family, label_sz, menu_cx, menu_cy, S * 0.09f, S * 0.042f, L"X",
				  down(pad, 0x0010));
	}

	if (settings.show_left_stick) {
		draw_stick(g, ls_cx, ls_cy, ls_r, pad.lx_n, pad.ly_n, settings.deadzone, argb(255, 61, 220, 255),
			   down(pad, 0x0040), pad.connected);
		if (!settings.show_left_stick_numbers)
			draw_text(g, family, label_sz, text_c, ls_cx, ls_cy + ls_r + 6, "L", true);
	}
	if (settings.show_left_stick_numbers) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "raw X %+6d  Y %+6d\nnorm X %+0.4f  Y %+0.4f", pad.lx, pad.ly, pad.lx_n,
			      pad.ly_n);
		draw_text(g, family, body_sz, text_c, ls_cx - ls_r * 0.15f, ls_cy + ls_r + 4, buf);
	}

	if (settings.show_right_stick) {
		draw_stick(g, rs_cx, rs_cy, rs_r, pad.rx_n, pad.ry_n, settings.deadzone, argb(255, 255, 106, 213),
			   down(pad, 0x0080), pad.connected);
		if (!settings.show_right_stick_numbers)
			draw_text(g, family, label_sz, text_c, rs_cx, rs_cy + rs_r + 6, "R", true);
	}
	if (settings.show_right_stick_numbers) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "raw X %+6d  Y %+6d\nnorm X %+0.4f  Y %+0.4f", pad.rx, pad.ry, pad.rx_n,
			      pad.ry_n);
		draw_text(g, family, body_sz, text_c, rs_cx - rs_r * 0.2f, rs_cy - rs_r - body_sz * 2.5f, buf);
	}

	if (settings.show_dpad) {
		draw_dpad(g, dpad_cx, dpad_cy, dpad_arm, down(pad, 0x0001), down(pad, 0x0002), down(pad, 0x0004),
			  down(pad, 0x0008));
	}

	if (settings.show_face_buttons) {
		draw_face_button(g, family, face_sz, face_cx, face_cy - face_spread, face_r, L"Y", down(pad, 0x8000),
				 argb(255, 232, 197, 71));
		draw_face_button(g, family, face_sz, face_cx - face_spread, face_cy, face_r, L"X", down(pad, 0x4000),
				 argb(255, 59, 140, 219));
		draw_face_button(g, family, face_sz, face_cx + face_spread, face_cy, face_r, L"B", down(pad, 0x2000),
				 argb(255, 224, 60, 63));
		draw_face_button(g, family, face_sz, face_cx, face_cy + face_spread, face_r, L"A", down(pad, 0x1000),
				 argb(255, 124, 187, 63));
	}

	if (settings.show_gyro)
		draw_gyro(g, gyro_cx, gyro_cy, gyro_r, pad);

	if (settings.show_gyro_numbers && pad.gyro_valid) {
		char gyro[160];
		std::snprintf(gyro, sizeof(gyro),
			      "g %+5d %+5d %+5d  %+6.1f %+6.1f %+6.1f\n"
			      "a %+5d %+5d %+5d  %+6.3f %+6.3f %+6.3f",
			      pad.gyro_raw[0], pad.gyro_raw[1], pad.gyro_raw[2], pad.gyro_dps[0], pad.gyro_dps[1],
			      pad.gyro_dps[2], pad.accel_raw[0], pad.accel_raw[1], pad.accel_raw[2], pad.accel_g[0],
			      pad.accel_g[1], pad.accel_g[2]);
		draw_text(g, family, body_sz * 0.92f, text_c, gyro_cx - gyro_r * 1.35f, gyro_cy + gyro_r + 6, gyro);
	}

	if (settings.show_deadzone_bars || settings.show_anti_deadzone_bars || settings.show_jitter_bars) {
		const float bar_w = S * 0.026f;
		const float bar_h = S * 0.16f;
		const float gap = S * 0.058f;
		const float y = gyro_cy + gyro_r + S * 0.095f;
		const Color dz_c = argb(210, 61, 220, 255);
		const Color adz_c = argb(210, 255, 106, 213);

		auto draw_group = [&](float start_x, const StickTune &tune, const char *ldz, const char *ladz,
				      const char *ljit) {
			float x = start_x;
			auto step = [&]() { x += bar_w + gap; };
			if (settings.show_deadzone_bars) {
				const float z0 = tune.valid ? tune.dz_min : 0;
				const float z1 = tune.valid ? tune.dz_max : 100;
				draw_zone_bar(g, family, label_sz, dz_c, x, y, bar_w, bar_h, z0, z1, ldz);
				step();
			}
			if (settings.show_anti_deadzone_bars) {
				const float z0 = tune.valid ? tune.adz_min : 0;
				const float z1 = tune.valid ? tune.adz_max : 100;
				draw_zone_bar(g, family, label_sz, adz_c, x, y, bar_w, bar_h, z0, z1, ladz);
				step();
			}
			if (settings.show_jitter_bars)
				draw_jitter_bar(g, family, label_sz, x, y, bar_w, bar_h, tune.jitter, ljit);
		};

		int n = 0;
		if (settings.show_deadzone_bars)
			n++;
		if (settings.show_anti_deadzone_bars)
			n++;
		if (settings.show_jitter_bars)
			n++;
		const float group_w = n * bar_w + (n - 1) * gap;
		draw_group(W * 0.38f - group_w * 0.5f, pad.ls_tune, "L DZ", "L ADZ", "L JIT");
		draw_group(W * 0.62f - group_w * 0.5f, pad.rs_tune, "R DZ", "R ADZ", "R JIT");
	}
}
