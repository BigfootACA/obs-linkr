#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace obs_linkr {

std::string obs_string(obs_data_t *settings, const char *name, const char *fallback)
{
	const char *value = obs_data_get_string(settings, name);
	if (!value || !*value)
		return fallback;
	return value;
}

std::string to_lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

std::string trim(std::string value)
{
	auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
			    return !is_space(static_cast<unsigned char>(ch));
		    }));
	value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
			    return !is_space(static_cast<unsigned char>(ch));
		    }).base(),
		    value.end());
	return value;
}

bool has_scheme(const std::string &value)
{
	return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string normalize_device(std::string device)
{
	device = trim(device);
	if (device.empty())
		device = "linkr-device.local";

	if (!has_scheme(device))
		device = "http://" + device;

	while (!device.empty() && device.back() == '/')
		device.pop_back();

	return device;
}

std::string url_encode(const std::string &value)
{
	std::ostringstream out;
	out.fill('0');
	out << std::hex;

	for (const unsigned char ch : value) {
		if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			out << ch;
		} else {
			out << '%' << std::uppercase;
			out.width(2);
			out << static_cast<int>(ch);
			out << std::nouppercase;
		}
	}

	return out.str();
}

std::string json_escape(const std::string &value)
{
	std::ostringstream out;
	for (const unsigned char ch : value) {
		switch (ch) {
		case '"':
			out << "\\\"";
			break;
		case '\\':
			out << "\\\\";
			break;
		case '\b':
			out << "\\b";
			break;
		case '\f':
			out << "\\f";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (ch < 0x20) {
				out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
				    << std::dec;
			} else {
				out << static_cast<char>(ch);
			}
		}
	}
	return out.str();
}

std::string make_instance_id(const void *ptr)
{
	std::ostringstream out;
	out << std::hex << reinterpret_cast<uintptr_t>(ptr);
	return out.str();
}

} // namespace obs_linkr
