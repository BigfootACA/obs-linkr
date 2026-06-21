#pragma once

#include <obs-module.h>

#include <string>

namespace obs_linkr {

std::string obs_string(obs_data_t *settings, const char *name, const char *fallback = "");
std::string to_lower(std::string value);
std::string trim(std::string value);
bool has_scheme(const std::string &value);
std::string normalize_device(std::string device);
std::string url_encode(const std::string &value);
std::string json_escape(const std::string &value);
std::string make_instance_id(const void *ptr);

} // namespace obs_linkr
