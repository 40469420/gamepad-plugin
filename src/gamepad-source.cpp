#include "gamepad-source.hpp"
#include "gamepad-runtime.hpp"
#include "gamepad-draw.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct GamepadSource {
	obs_source_t *source = nullptr;
	int slot = 0;
	uint32_t width = 1280;
	uint32_t height = 720;
	GamepadDrawSettings draw{};
	int rumble_left = 80;
	int rumble_right = 80;
	int rumble_ms = 400;
	std::vector<uint8_t> pixels;
	std::vector<uint8_t> upload;
	gs_texture_t *tex = nullptr;
	uint32_t tex_w = 0;
	uint32_t tex_h = 0;
	bool dirty = false;
	PadSnapshot last_pad{};
	GamepadDrawSettings last_draw{};
	uint32_t last_w = 0;
	uint32_t last_h = 0;
	bool have_last = false;
	StickTrail ls_trail;
	StickTrail rs_trail;
	std::mutex mutex;
};

int clamp_i(int v, int lo, int hi)
{
	return std::clamp(v, lo, hi);
}

int get_pos(obs_data_t *settings, const char *key)
{
	return clamp_i((int)obs_data_get_int(settings, key), -50, 50);
}

uint64_t now_ms()
{
	using clock = std::chrono::steady_clock;
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
}

void push_needs(const GamepadDrawSettings &d)
{
	GamepadNeeds n;
	n.gyro = d.show_gyro || d.show_gyro_numbers;
	n.hid_tune = d.show_deadzone_bars || d.show_anti_deadzone_bars;
	n.hid_poll = d.show_status;
	n.jitter = d.show_jitter_bars;
	gamepad_runtime_set_needs(n);
}

uint32_t clamp_u32(int v, int lo, int hi)
{
	return (uint32_t)std::clamp(v, lo, hi);
}

void destroy_texture(GamepadSource *ctx)
{
	if (ctx->tex) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->tex);
		obs_leave_graphics();
		ctx->tex = nullptr;
		ctx->tex_w = 0;
		ctx->tex_h = 0;
	}
}

const char *source_name(void *)
{
	return obs_module_text("GamepadSource");
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new GamepadSource();
	ctx->source = source;
	gamepad_runtime_acquire();
	obs_source_update(source, settings);
	return ctx;
}

void source_destroy(void *data)
{
	auto *ctx = static_cast<GamepadSource *>(data);
	destroy_texture(ctx);
	gamepad_runtime_release();
	delete ctx;
}

void source_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<GamepadSource *>(data);
	std::lock_guard<std::mutex> lock(ctx->mutex);
	ctx->slot = (int)obs_data_get_int(settings, "slot");
	ctx->width = clamp_u32((int)obs_data_get_int(settings, "width"), 320, 2560);
	ctx->height = clamp_u32((int)obs_data_get_int(settings, "height"), 180, 1440);
	ctx->draw.slot = ctx->slot;
	ctx->draw.show_background = obs_data_get_bool(settings, "show_background");
	ctx->draw.show_status = obs_data_get_bool(settings, "show_status");
	ctx->draw.show_left_stick = obs_data_get_bool(settings, "show_left_stick");
	ctx->draw.show_left_stick_numbers = obs_data_get_bool(settings, "show_left_stick_numbers");
	ctx->draw.show_right_stick = obs_data_get_bool(settings, "show_right_stick");
	ctx->draw.show_right_stick_numbers = obs_data_get_bool(settings, "show_right_stick_numbers");
	ctx->draw.show_dpad = obs_data_get_bool(settings, "show_dpad");
	ctx->draw.show_face_buttons = obs_data_get_bool(settings, "show_face_buttons");
	ctx->draw.show_bumpers = obs_data_get_bool(settings, "show_bumpers");
	ctx->draw.show_menu_buttons = obs_data_get_bool(settings, "show_menu_buttons");
	ctx->draw.show_triggers = obs_data_get_bool(settings, "show_triggers");
	ctx->draw.show_trigger_numbers = obs_data_get_bool(settings, "show_trigger_numbers");
	ctx->draw.show_gyro = obs_data_get_bool(settings, "show_gyro");
	ctx->draw.show_gyro_numbers = obs_data_get_bool(settings, "show_gyro_numbers");
	ctx->draw.show_deadzone_bars = obs_data_get_bool(settings, "show_deadzone_bars");
	ctx->draw.show_anti_deadzone_bars = obs_data_get_bool(settings, "show_anti_deadzone_bars");
	ctx->draw.show_jitter_bars = obs_data_get_bool(settings, "show_jitter_bars");
	ctx->draw.show_trails = obs_data_get_bool(settings, "show_trails");
	ctx->draw.trail_len = clamp_i((int)obs_data_get_int(settings, "trail_len"), 8, StickTrail::kMax);
	ctx->draw.trail_life = (float)std::clamp(obs_data_get_double(settings, "trail_life"), 0.4, 6.0);
	ctx->draw.trail_thick = (float)clamp_i((int)obs_data_get_int(settings, "trail_thick"), 50, 200) / 100.0f;
	ctx->draw.deadzone = (float)obs_data_get_double(settings, "deadzone");
	ctx->draw.col_ls = (uint32_t)obs_data_get_int(settings, "col_ls");
	ctx->draw.col_rs = (uint32_t)obs_data_get_int(settings, "col_rs");
	ctx->draw.col_a = (uint32_t)obs_data_get_int(settings, "col_a");
	ctx->draw.col_b = (uint32_t)obs_data_get_int(settings, "col_b");
	ctx->draw.col_x = (uint32_t)obs_data_get_int(settings, "col_x");
	ctx->draw.col_y = (uint32_t)obs_data_get_int(settings, "col_y");
	ctx->draw.col_dpad = (uint32_t)obs_data_get_int(settings, "col_dpad");
	ctx->draw.pos_ls_x = get_pos(settings, "pos_ls_x");
	ctx->draw.pos_ls_y = get_pos(settings, "pos_ls_y");
	ctx->draw.pos_rs_x = get_pos(settings, "pos_rs_x");
	ctx->draw.pos_rs_y = get_pos(settings, "pos_rs_y");
	ctx->draw.pos_face_x = get_pos(settings, "pos_face_x");
	ctx->draw.pos_face_y = get_pos(settings, "pos_face_y");
	ctx->draw.pos_dpad_x = get_pos(settings, "pos_dpad_x");
	ctx->draw.pos_dpad_y = get_pos(settings, "pos_dpad_y");
	ctx->draw.pos_gyro_x = get_pos(settings, "pos_gyro_x");
	ctx->draw.pos_gyro_y = get_pos(settings, "pos_gyro_y");
	ctx->draw.pos_trig_x = get_pos(settings, "pos_trig_x");
	ctx->draw.pos_trig_y = get_pos(settings, "pos_trig_y");
	ctx->draw.pos_menu_x = get_pos(settings, "pos_menu_x");
	ctx->draw.pos_menu_y = get_pos(settings, "pos_menu_y");
	ctx->draw.pos_bump_x = get_pos(settings, "pos_bump_x");
	ctx->draw.pos_bump_y = get_pos(settings, "pos_bump_y");
	ctx->draw.pos_bars_x = get_pos(settings, "pos_bars_x");
	ctx->draw.pos_bars_y = get_pos(settings, "pos_bars_y");
	if (!ctx->draw.show_trails || !ctx->draw.show_left_stick)
		ctx->ls_trail.clear();
	if (!ctx->draw.show_trails || !ctx->draw.show_right_stick)
		ctx->rs_trail.clear();
	ctx->rumble_left = (int)obs_data_get_int(settings, "rumble_left");
	ctx->rumble_right = (int)obs_data_get_int(settings, "rumble_right");
	ctx->rumble_ms = (int)obs_data_get_int(settings, "rumble_ms");
	push_needs(ctx->draw);
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "slot", 0);
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_bool(settings, "show_background", false);
	obs_data_set_default_bool(settings, "show_status", true);
	obs_data_set_default_bool(settings, "show_left_stick", true);
	obs_data_set_default_bool(settings, "show_left_stick_numbers", true);
	obs_data_set_default_bool(settings, "show_right_stick", true);
	obs_data_set_default_bool(settings, "show_right_stick_numbers", true);
	obs_data_set_default_bool(settings, "show_dpad", true);
	obs_data_set_default_bool(settings, "show_face_buttons", true);
	obs_data_set_default_bool(settings, "show_bumpers", true);
	obs_data_set_default_bool(settings, "show_menu_buttons", true);
	obs_data_set_default_bool(settings, "show_triggers", true);
	obs_data_set_default_bool(settings, "show_trigger_numbers", true);
	obs_data_set_default_bool(settings, "show_gyro", true);
	obs_data_set_default_bool(settings, "show_gyro_numbers", true);
	obs_data_set_default_bool(settings, "show_deadzone_bars", true);
	obs_data_set_default_bool(settings, "show_anti_deadzone_bars", true);
	obs_data_set_default_bool(settings, "show_jitter_bars", true);
	obs_data_set_default_bool(settings, "show_trails", true);
	obs_data_set_default_int(settings, "trail_len", 28);
	obs_data_set_default_double(settings, "trail_life", 2.2);
	obs_data_set_default_int(settings, "trail_thick", 100);
	obs_data_set_default_double(settings, "deadzone", 0.0);
	obs_data_set_default_int(settings, "col_ls", 0x00FFDC3D);
	obs_data_set_default_int(settings, "col_rs", 0x00D56AFF);
	obs_data_set_default_int(settings, "col_a", 0x003FBB7C);
	obs_data_set_default_int(settings, "col_b", 0x003F3CE0);
	obs_data_set_default_int(settings, "col_x", 0x00DB8C3B);
	obs_data_set_default_int(settings, "col_y", 0x0047C5E8);
	obs_data_set_default_int(settings, "col_dpad", 0x0066D1FF);
	obs_data_set_default_int(settings, "pos_ls_x", 0);
	obs_data_set_default_int(settings, "pos_ls_y", 0);
	obs_data_set_default_int(settings, "pos_rs_x", 0);
	obs_data_set_default_int(settings, "pos_rs_y", 0);
	obs_data_set_default_int(settings, "pos_face_x", 0);
	obs_data_set_default_int(settings, "pos_face_y", 0);
	obs_data_set_default_int(settings, "pos_dpad_x", 0);
	obs_data_set_default_int(settings, "pos_dpad_y", 0);
	obs_data_set_default_int(settings, "pos_gyro_x", 0);
	obs_data_set_default_int(settings, "pos_gyro_y", 0);
	obs_data_set_default_int(settings, "pos_trig_x", 0);
	obs_data_set_default_int(settings, "pos_trig_y", 0);
	obs_data_set_default_int(settings, "pos_menu_x", 0);
	obs_data_set_default_int(settings, "pos_menu_y", 0);
	obs_data_set_default_int(settings, "pos_bump_x", 0);
	obs_data_set_default_int(settings, "pos_bump_y", 0);
	obs_data_set_default_int(settings, "pos_bars_x", 0);
	obs_data_set_default_int(settings, "pos_bars_y", 0);
	obs_data_set_default_int(settings, "rumble_left", 80);
	obs_data_set_default_int(settings, "rumble_right", 80);
	obs_data_set_default_int(settings, "rumble_ms", 400);
}

bool rumble_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<GamepadSource *>(data);
	int slot, left_pct, right_pct, duration;
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		slot = ctx->slot;
		left_pct = ctx->rumble_left;
		right_pct = ctx->rumble_right;
		duration = ctx->rumble_ms;
	}
	const uint16_t left = (uint16_t)std::clamp(left_pct * 65535 / 100, 0, 65535);
	const uint16_t right = (uint16_t)std::clamp(right_pct * 65535 / 100, 0, 65535);
	gamepad_runtime_rumble(slot, left, right, (uint32_t)std::max(duration, 50));
	return false;
}

bool reset_layout_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<GamepadSource *>(data);
	obs_data_t *s = obs_source_get_settings(ctx->source);
	static const char *keys[] = {"pos_ls_x",   "pos_ls_y",   "pos_rs_x",   "pos_rs_y",   "pos_face_x", "pos_face_y",
				     "pos_dpad_x", "pos_dpad_y", "pos_gyro_x", "pos_gyro_y", "pos_trig_x", "pos_trig_y",
				     "pos_menu_x", "pos_menu_y", "pos_bump_x", "pos_bump_y", "pos_bars_x", "pos_bars_y"};
	for (const char *key : keys)
		obs_data_set_int(s, key, 0);
	obs_source_update(ctx->source, s);
	obs_data_release(s);
	return true;
}

void add_xy(obs_properties_t *p, const char *xk, const char *yk, const char *xl, const char *yl)
{
	obs_properties_add_int_slider(p, xk, obs_module_text(xl), -50, 50, 1);
	obs_properties_add_int_slider(p, yk, obs_module_text(yl), -50, 50, 1);
}

obs_properties_t *source_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *slot = obs_properties_add_list(props, "slot", obs_module_text("Controller"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(slot, "XInput 1", 0);
	obs_property_list_add_int(slot, "XInput 2", 1);
	obs_property_list_add_int(slot, "XInput 3", 2);
	obs_property_list_add_int(slot, "XInput 4", 3);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 320, 2560, 2);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 180, 1440, 2);
	obs_properties_add_bool(props, "show_background", obs_module_text("ShowBackground"));
	obs_properties_add_bool(props, "show_status", obs_module_text("ShowStatus"));
	obs_properties_add_float_slider(props, "deadzone", obs_module_text("Deadzone"), 0.0, 0.40, 0.01);

	obs_properties_t *sticks = obs_properties_create();
	obs_properties_add_bool(sticks, "show_left_stick", obs_module_text("ShowLeftStick"));
	obs_properties_add_bool(sticks, "show_left_stick_numbers", obs_module_text("ShowLeftStickNumbers"));
	obs_properties_add_bool(sticks, "show_right_stick", obs_module_text("ShowRightStick"));
	obs_properties_add_bool(sticks, "show_right_stick_numbers", obs_module_text("ShowRightStickNumbers"));
	obs_properties_add_bool(sticks, "show_trails", obs_module_text("ShowTrails"));
	obs_properties_add_int_slider(sticks, "trail_len", obs_module_text("TrailLen"), 8, StickTrail::kMax, 1);
	obs_properties_add_float_slider(sticks, "trail_life", obs_module_text("TrailLife"), 0.4, 6.0, 0.1);
	obs_properties_add_int_slider(sticks, "trail_thick", obs_module_text("TrailThick"), 50, 200, 5);
	obs_properties_add_group(props, "sticks_group", obs_module_text("SticksGroup"), OBS_GROUP_NORMAL, sticks);

	obs_properties_t *buttons = obs_properties_create();
	obs_properties_add_bool(buttons, "show_face_buttons", obs_module_text("ShowFaceButtons"));
	obs_properties_add_bool(buttons, "show_dpad", obs_module_text("ShowDpad"));
	obs_properties_add_bool(buttons, "show_bumpers", obs_module_text("ShowBumpers"));
	obs_properties_add_bool(buttons, "show_menu_buttons", obs_module_text("ShowMenuButtons"));
	obs_properties_add_group(props, "buttons_group", obs_module_text("ButtonsGroup"), OBS_GROUP_NORMAL, buttons);

	obs_properties_t *triggers = obs_properties_create();
	obs_properties_add_bool(triggers, "show_triggers", obs_module_text("ShowTriggers"));
	obs_properties_add_bool(triggers, "show_trigger_numbers", obs_module_text("ShowTriggerNumbers"));
	obs_properties_add_group(props, "triggers_group", obs_module_text("TriggersGroup"), OBS_GROUP_NORMAL, triggers);

	obs_properties_t *gyro = obs_properties_create();
	obs_properties_add_bool(gyro, "show_gyro", obs_module_text("ShowGyro"));
	obs_properties_add_bool(gyro, "show_gyro_numbers", obs_module_text("ShowGyroNumbers"));
	obs_properties_add_group(props, "gyro_group", obs_module_text("GyroGroup"), OBS_GROUP_NORMAL, gyro);

	obs_properties_t *tune = obs_properties_create();
	obs_properties_add_bool(tune, "show_deadzone_bars", obs_module_text("ShowDeadzoneBars"));
	obs_properties_add_bool(tune, "show_anti_deadzone_bars", obs_module_text("ShowAntiDeadzoneBars"));
	obs_properties_add_bool(tune, "show_jitter_bars", obs_module_text("ShowJitterBars"));
	obs_properties_add_group(props, "tune_group", obs_module_text("TuneGroup"), OBS_GROUP_NORMAL, tune);

	obs_properties_t *colors = obs_properties_create();
	obs_properties_add_color(colors, "col_ls", obs_module_text("ColLs"));
	obs_properties_add_color(colors, "col_rs", obs_module_text("ColRs"));
	obs_properties_add_color(colors, "col_a", obs_module_text("ColA"));
	obs_properties_add_color(colors, "col_b", obs_module_text("ColB"));
	obs_properties_add_color(colors, "col_x", obs_module_text("ColX"));
	obs_properties_add_color(colors, "col_y", obs_module_text("ColY"));
	obs_properties_add_color(colors, "col_dpad", obs_module_text("ColDpad"));
	obs_properties_add_group(props, "colors_group", obs_module_text("ColorsGroup"), OBS_GROUP_NORMAL, colors);

	obs_properties_t *layout = obs_properties_create();
	add_xy(layout, "pos_ls_x", "pos_ls_y", "PosLsX", "PosLsY");
	add_xy(layout, "pos_rs_x", "pos_rs_y", "PosRsX", "PosRsY");
	add_xy(layout, "pos_face_x", "pos_face_y", "PosFaceX", "PosFaceY");
	add_xy(layout, "pos_dpad_x", "pos_dpad_y", "PosDpadX", "PosDpadY");
	add_xy(layout, "pos_gyro_x", "pos_gyro_y", "PosGyroX", "PosGyroY");
	add_xy(layout, "pos_trig_x", "pos_trig_y", "PosTrigX", "PosTrigY");
	add_xy(layout, "pos_menu_x", "pos_menu_y", "PosMenuX", "PosMenuY");
	add_xy(layout, "pos_bump_x", "pos_bump_y", "PosBumpX", "PosBumpY");
	add_xy(layout, "pos_bars_x", "pos_bars_y", "PosBarsX", "PosBarsY");
	obs_properties_add_button(layout, "reset_layout", obs_module_text("ResetLayout"), reset_layout_clicked);
	obs_properties_add_group(props, "layout_group", obs_module_text("LayoutGroup"), OBS_GROUP_NORMAL, layout);

	obs_properties_add_int_slider(props, "rumble_left", obs_module_text("RumbleLeft"), 0, 100, 1);
	obs_properties_add_int_slider(props, "rumble_right", obs_module_text("RumbleRight"), 0, 100, 1);
	obs_properties_add_int(props, "rumble_ms", obs_module_text("RumbleDuration"), 50, 3000, 50);
	obs_properties_add_button(props, "rumble_test", obs_module_text("TestRumble"), rumble_clicked);

	return props;
}

uint32_t source_width(void *data)
{
	return static_cast<GamepadSource *>(data)->width;
}

uint32_t source_height(void *data)
{
	return static_cast<GamepadSource *>(data)->height;
}

void source_tick(void *data, float)
{
	auto *ctx = static_cast<GamepadSource *>(data);
	PadSnapshot pad;
	GamepadDrawSettings settings;
	StickTrail ls_copy;
	StickTrail rs_copy;
	uint32_t w;
	uint32_t h;
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		w = ctx->width;
		h = ctx->height;
		settings = ctx->draw;
		pad = gamepad_runtime_snapshot(ctx->slot);
		const uint64_t now = now_ms();
		const bool ls_trail_on = settings.show_trails && settings.show_left_stick;
		const bool rs_trail_on = settings.show_trails && settings.show_right_stick;
		if (ls_trail_on) {
			if (pad.connected)
				ctx->ls_trail.push(pad.lx_n, pad.ly_n, now);
			ctx->ls_trail.expire(now, settings.trail_life);
		} else {
			ctx->ls_trail.clear();
		}
		if (rs_trail_on) {
			if (pad.connected)
				ctx->rs_trail.push(pad.rx_n, pad.ry_n, now);
			ctx->rs_trail.expire(now, settings.trail_life);
		} else {
			ctx->rs_trail.clear();
		}
		const bool trail_fading = (ls_trail_on && ctx->ls_trail.n >= 2) || (rs_trail_on && ctx->rs_trail.n >= 2);
		if (ctx->have_last && !trail_fading && w == ctx->last_w && h == ctx->last_h &&
		    std::memcmp(&pad, &ctx->last_pad, sizeof(pad)) == 0 &&
		    std::memcmp(&settings, &ctx->last_draw, sizeof(settings)) == 0)
			return;
		ls_copy = ctx->ls_trail;
		rs_copy = ctx->rs_trail;
		ctx->last_pad = pad;
		ctx->last_draw = settings;
		ctx->last_w = w;
		ctx->last_h = h;
		ctx->have_last = true;
		ctx->pixels.resize((size_t)w * (size_t)h * 4);
	}
	const StickTrail *ls_tr = (settings.show_trails && settings.show_left_stick) ? &ls_copy : nullptr;
	const StickTrail *rs_tr = (settings.show_trails && settings.show_right_stick) ? &rs_copy : nullptr;
	gamepad_draw_frame(ctx->pixels.data(), (int)w, (int)h, (int)w * 4, pad, settings, ls_tr, rs_tr);
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->upload.swap(ctx->pixels);
		if (ctx->pixels.size() != ctx->upload.size())
			ctx->pixels.assign(ctx->upload.size(), 0);
		ctx->dirty = true;
	}
}

void source_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<GamepadSource *>(data);

	std::lock_guard<std::mutex> lock(ctx->mutex);
	if (ctx->upload.empty())
		return;

	if (!ctx->tex || ctx->tex_w != ctx->width || ctx->tex_h != ctx->height) {
		if (ctx->tex)
			gs_texture_destroy(ctx->tex);
		ctx->tex = gs_texture_create(ctx->width, ctx->height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
		ctx->tex_w = ctx->width;
		ctx->tex_h = ctx->height;
	}
	if (!ctx->tex)
		return;

	if (ctx->dirty) {
		gs_texture_set_image(ctx->tex, ctx->upload.data(), ctx->width * 4, false);
		ctx->dirty = false;
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, ctx->tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(ctx->tex, 0, ctx->width, ctx->height);
	gs_blend_state_pop();
}

} // namespace

void register_gamepad_source()
{
	obs_source_info info{};
	info.id = "gamepad_plugin_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = source_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_defaults;
	info.get_properties = source_properties;
	info.get_width = source_width;
	info.get_height = source_height;
	info.video_tick = source_tick;
	info.video_render = source_render;
	info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
	obs_register_source(&info);
}
