#include <obs-module.h>
#include <plugin-support.h>
#include "gamepad-source.hpp"
#include "gamepad-runtime.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Gamepad overlay source with raw XInput sticks/buttons, gyro, and rumble test.";
}

bool obs_module_load(void)
{
	register_gamepad_source();
	obs_log(LOG_INFO, "loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	gamepad_runtime_shutdown();
	obs_log(LOG_INFO, "unloaded");
}
