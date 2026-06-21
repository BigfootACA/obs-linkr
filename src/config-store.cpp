#include "config-store.hpp"

#include "utils.hpp"

#include <obs-module.h>

#include <map>
#include <mutex>
#include <sstream>

namespace obs_linkr {
namespace {

std::mutex config_mutex;
std::map<std::string, LinkrConfig> configs;

struct PageText {
	const char *json_key;
	const char *locale_key;
};

constexpr PageText page_texts[] = {
	{"readingSettings", "Page.ReadingSettings"},
	{"missingUsername", "Page.MissingUsername"},
	{"missingPassword", "Page.MissingPassword"},
	{"missingSourceId", "Page.MissingSourceId"},
	{"configFailed", "Page.ConfigFailed"},
	{"loginFailed", "Page.LoginFailed"},
	{"invalidWhep", "Page.InvalidWhep"},
	{"authFailed", "Page.AuthFailed"},
	{"apiMissing", "Page.ApiMissing"},
	{"deviceUnreachable", "Page.DeviceUnreachable"},
	{"connectionFailed", "Page.ConnectionFailed"},
	{"unknownFailed", "Page.UnknownFailed"},
	{"loggingIn", "Page.LoggingIn"},
	{"connectingWebrtc", "Page.ConnectingWebrtc"},
	{"reconnecting", "Page.Reconnecting"},
	{"retriesExhausted", "Page.RetriesExhausted"},
	{"reconnectAttempt", "Page.ReconnectAttempt"},
};

void append_page_messages(std::ostringstream &out)
{
	out << "\"messages\":{";
	for (size_t i = 0; i < std::size(page_texts); ++i) {
		if (i > 0)
			out << ",";
		out << "\"" << page_texts[i].json_key << "\":\"" << json_escape(obs_module_text(page_texts[i].locale_key))
		    << "\"";
	}
	out << "}";
}

} // namespace

void set_config(const std::string &id, const LinkrConfig &config)
{
	std::lock_guard<std::mutex> lock(config_mutex);
	configs[id] = config;
}

void erase_config(const std::string &id)
{
	std::lock_guard<std::mutex> lock(config_mutex);
	configs.erase(id);
}

std::string config_json(const std::string &id)
{
	std::lock_guard<std::mutex> lock(config_mutex);
	const auto found = configs.find(id);
	if (found == configs.end())
		return {};

	const LinkrConfig &config = found->second;
	const char *locale = obs_get_locale();
	std::ostringstream out;
	out << "{"
	    << "\"deviceUrl\":\"" << json_escape(config.device_url) << "\","
	    << "\"username\":\"" << json_escape(config.username) << "\","
	    << "\"password\":\"" << json_escape(config.password) << "\","
	    << "\"receiveAudio\":" << (config.receive_audio ? "true" : "false") << ","
	    << "\"locale\":\"" << json_escape(locale ? locale : "en-US") << "\",";
	append_page_messages(out);
	out << ","
	    << "\"maxRetries\":5,"
	    << "\"retryMs\":3000,"
	    << "\"reload\":" << config.reload_counter << "}";
	return out.str();
}

} // namespace obs_linkr
