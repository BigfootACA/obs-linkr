#pragma once

#include <cstdint>
#include <string>

namespace obs_linkr {

struct LinkrConfig {
	std::string device_url = "";
	std::string username = "";
	std::string password = "";
	bool receive_audio = false;
	uint32_t reload_counter = 0;
};

void set_config(const std::string &id, const LinkrConfig &config);
void erase_config(const std::string &id);
std::string config_json(const std::string &id);

} // namespace obs_linkr
