/*
Better Key - realtime chroma/luma keyer for OBS Studio
Copyright (C) 2026 Insanity Labs
*/

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info infinity_key_filter;

bool obs_module_load(void)
{
	obs_register_source(&infinity_key_filter);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
