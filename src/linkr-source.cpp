#include "linkr-source.hpp"

#include "config-store.hpp"
#include "constants.hpp"
#include "http-server.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

namespace obs_linkr {
namespace {

struct LinkrSource {
	obs_source_t *source = nullptr;
	obs_source_t *browser = nullptr;
	std::string instance_id;
	std::string url;
	uint32_t width = DEFAULT_WIDTH;
	uint32_t height = DEFAULT_HEIGHT;
	uint32_t reload_counter = 0;
	bool browser_available = false;
	bool child_attached = false;
	bool showing_child = false;
	bool active_child = false;
};

std::string build_page_url(const LinkrSource *ctx)
{
	std::string base = http_server_base_url();
	if (base.empty()) {
		blog(LOG_ERROR, "[obs-linkr] Internal HTTP server is not running; cannot open embedded Linkr page.");
		return "about:blank";
	}

	const char separator = base.find('?') == std::string::npos ? '?' : '&';
	std::ostringstream url;
	url << base << separator << "source=" << url_encode(ctx ? ctx->instance_id : "")
	    << "&reload=" << (ctx ? ctx->reload_counter : 0);
	return url.str();
}

void store_source_config(LinkrSource *ctx, obs_data_t *settings)
{
	if (!ctx)
		return;

	LinkrConfig config;
	config.device_url = normalize_device(obs_string(settings, "device_url", "linkr-device.local"));
	config.username = obs_string(settings, "username", "admin");
	config.password = obs_string(settings, "password", "");
	config.receive_audio = obs_data_get_bool(settings, "receive_audio");
	config.reload_counter = ctx->reload_counter;
	set_config(ctx->instance_id, config);
}

obs_data_t *create_browser_settings(LinkrSource *ctx, obs_data_t *settings)
{
	obs_data_t *browser_settings = obs_data_create();
	obs_data_set_bool(browser_settings, "is_local_file", false);
	obs_data_set_string(browser_settings, "url", ctx->url.c_str());
	obs_data_set_int(browser_settings, "width", ctx->width);
	obs_data_set_int(browser_settings, "height", ctx->height);
	obs_data_set_bool(browser_settings, "fps_custom", false);
	obs_data_set_int(browser_settings, "fps", DEFAULT_FPS);
	obs_data_set_bool(browser_settings, "shutdown", obs_data_get_bool(settings, "shutdown_when_inactive"));
	obs_data_set_bool(browser_settings, "restart_when_active", obs_data_get_bool(settings, "restart_when_active"));
	obs_data_set_bool(browser_settings, "reroute_audio", obs_data_get_bool(settings, "receive_audio"));
	obs_data_set_int(browser_settings, "webpage_control_level", 0);
	obs_data_set_string(browser_settings, "css", "body { margin: 0; overflow: hidden; background: #000; }");
	return browser_settings;
}

void release_child_state(LinkrSource *ctx)
{
	if (!ctx || !ctx->browser)
		return;

	if (ctx->active_child) {
		obs_source_dec_active(ctx->browser);
		ctx->active_child = false;
	}
	if (ctx->showing_child) {
		obs_source_dec_showing(ctx->browser);
		ctx->showing_child = false;
	}
	if (ctx->child_attached) {
		obs_source_remove_active_child(ctx->source, ctx->browser);
		ctx->child_attached = false;
	}

	obs_source_release(ctx->browser);
	ctx->browser = nullptr;
}

void update_browser(LinkrSource *ctx, obs_data_t *settings)
{
	if (!ctx)
		return;

	ctx->width = DEFAULT_WIDTH;
	ctx->height = DEFAULT_HEIGHT;
	store_source_config(ctx, settings);
	ctx->url = build_page_url(ctx);
	ctx->browser_available = (obs_get_source_output_flags(BROWSER_SOURCE_ID) != 0);

	if (!ctx->browser_available) {
		blog(LOG_ERROR, "[obs-linkr] OBS browser source is not available. Install or enable obs-browser.");
		release_child_state(ctx);
		return;
	}

	obs_data_t *browser_settings = create_browser_settings(ctx, settings);

	if (!ctx->browser) {
		ctx->browser = obs_source_create_private(BROWSER_SOURCE_ID, "Linkr Browser Renderer", browser_settings);
		if (ctx->browser) {
			ctx->child_attached = obs_source_add_active_child(ctx->source, ctx->browser);
			if (!ctx->child_attached)
				blog(LOG_WARNING, "[obs-linkr] Failed to attach browser_source as an active child.");
		} else {
			blog(LOG_ERROR, "[obs-linkr] Failed to create private browser_source child.");
		}
	} else {
		obs_source_update(ctx->browser, browser_settings);
	}

	obs_data_release(browser_settings);
}

void *linkr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new LinkrSource();
	ctx->source = source;
	ctx->instance_id = make_instance_id(ctx);
	update_browser(ctx, settings);
	return ctx;
}

void linkr_destroy(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	release_child_state(ctx);
	if (ctx)
		erase_config(ctx->instance_id);
	delete ctx;
}

void linkr_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx)
		ctx->reload_counter++;
	update_browser(ctx, settings);
}

uint32_t linkr_get_width(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	return ctx ? ctx->width : DEFAULT_WIDTH;
}

uint32_t linkr_get_height(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	return ctx ? ctx->height : DEFAULT_HEIGHT;
}

void linkr_video_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser)
		obs_source_video_render(ctx->browser);
}

void linkr_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser)
		enum_callback(ctx->source, ctx->browser, param);
}

void linkr_show(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser && !ctx->showing_child) {
		obs_source_inc_showing(ctx->browser);
		ctx->showing_child = true;
	}
}

void linkr_hide(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser && ctx->showing_child) {
		obs_source_dec_showing(ctx->browser);
		ctx->showing_child = false;
	}
}

void linkr_activate(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser && !ctx->active_child) {
		obs_source_inc_active(ctx->browser);
		ctx->active_child = true;
	}
}

void linkr_deactivate(void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (ctx && ctx->browser && ctx->active_child) {
		obs_source_dec_active(ctx->browser);
		ctx->active_child = false;
	}
}

void linkr_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device_url", "");
	obs_data_set_default_string(settings, "username", "");
	obs_data_set_default_string(settings, "password", "");
	obs_data_set_default_bool(settings, "receive_audio", false);
	obs_data_set_default_bool(settings, "shutdown_when_inactive", false);
	obs_data_set_default_bool(settings, "restart_when_active", true);
}

bool linkr_refresh_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<LinkrSource *>(data);
	if (!ctx || !ctx->browser)
		return false;

	obs_data_t *settings = obs_source_get_settings(ctx->source);
	ctx->reload_counter++;
	update_browser(ctx, settings);
	obs_data_release(settings);
	return false;
}

obs_properties_t *linkr_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "device_url", obs_module_text("DeviceURL"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "username", obs_module_text("Username"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "password", obs_module_text("Password"), OBS_TEXT_PASSWORD);
	obs_properties_add_bool(props, "receive_audio", obs_module_text("ReceiveAudio"));
	obs_properties_add_bool(props, "restart_when_active", obs_module_text("RestartWhenActive"));
	obs_properties_add_bool(props, "shutdown_when_inactive", obs_module_text("ShutdownWhenInactive"));
	obs_properties_add_button2(props, "refresh", obs_module_text("Refresh"), linkr_refresh_clicked, data);

	return props;
}

const char *linkr_get_name(void *)
{
	return obs_module_text("LinkrSource");
}

obs_source_info linkr_source_info = {};

} // namespace

void register_linkr_source()
{
	linkr_source_info.id = LINKR_SOURCE_ID;
	linkr_source_info.type = OBS_SOURCE_TYPE_INPUT;
	linkr_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	linkr_source_info.get_name = linkr_get_name;
	linkr_source_info.create = linkr_create;
	linkr_source_info.destroy = linkr_destroy;
	linkr_source_info.update = linkr_update;
	linkr_source_info.get_defaults = linkr_get_defaults;
	linkr_source_info.get_properties = linkr_get_properties;
	linkr_source_info.get_width = linkr_get_width;
	linkr_source_info.get_height = linkr_get_height;
	linkr_source_info.video_render = linkr_video_render;
	linkr_source_info.enum_active_sources = linkr_enum_active_sources;
	linkr_source_info.show = linkr_show;
	linkr_source_info.hide = linkr_hide;
	linkr_source_info.activate = linkr_activate;
	linkr_source_info.deactivate = linkr_deactivate;
	linkr_source_info.icon_type = OBS_ICON_TYPE_BROWSER;

	obs_register_source(&linkr_source_info);
}

} // namespace obs_linkr
