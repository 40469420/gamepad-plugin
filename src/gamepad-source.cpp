#include "gamepad-source.hpp"
#include "gamepad-runtime.hpp"
#include "gamepad-draw.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <algorithm>
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
	std::mutex mutex;
};

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
	ctx->draw.deadzone = (float)obs_data_get_double(settings, "deadzone");
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
	obs_data_set_default_double(settings, "deadzone", 0.0);
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
	uint32_t w;
	uint32_t h;
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		w = ctx->width;
		h = ctx->height;
		settings = ctx->draw;
		pad = gamepad_runtime_snapshot(ctx->slot);
		if (ctx->have_last && w == ctx->last_w && h == ctx->last_h &&
		    std::memcmp(&pad, &ctx->last_pad, sizeof(pad)) == 0 &&
		    std::memcmp(&settings, &ctx->last_draw, sizeof(settings)) == 0)
			return;
		ctx->last_pad = pad;
		ctx->last_draw = settings;
		ctx->last_w = w;
		ctx->last_h = h;
		ctx->have_last = true;
		ctx->pixels.resize((size_t)w * (size_t)h * 4);
	}
	gamepad_draw_frame(ctx->pixels.data(), (int)w, (int)h, (int)w * 4, pad, settings);
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
