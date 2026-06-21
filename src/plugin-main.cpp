#include "http-server.hpp"
#include "linkr-source.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-linkr", "en-US")
OBS_MODULE_AUTHOR("BigfootACA")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS LinkR";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Radxa LinkR WebRTC source.";
}

bool obs_module_load(void)
{
	obs_linkr::start_http_server();
	obs_linkr::register_linkr_source();
	blog(LOG_INFO, "[obs-linkr] Loaded OBS LinkR %s", OBS_LINKR_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_linkr::stop_http_server();
}
