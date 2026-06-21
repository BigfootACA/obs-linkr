#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace obs_linkr {

struct HttpRequest {
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;
};

struct HttpResponse {
	int status = 200;
	const char *reason = "OK";
	std::string content_type;
	std::string body;
};

std::string header_value(const HttpRequest &request, const std::string &name);
bool parse_request(const std::string &raw, HttpRequest &request);
std::string response_header(int status, const char *reason, const std::string &content_type, size_t length);
bool build_common_response(const HttpRequest &request, HttpResponse &response);

bool start_http_server_platform();
void stop_http_server_platform();
std::string http_server_base_url_platform();

} // namespace obs_linkr
