#include "http-server.hpp"

#include "config-store.hpp"
#include "embedded-assets.generated.hpp"
#include "http-server-internal.hpp"
#include "utils.hpp"

#include <sstream>

namespace obs_linkr {

std::string header_value(const HttpRequest &request, const std::string &name)
{
	auto found = request.headers.find(to_lower(name));
	return found == request.headers.end() ? std::string() : found->second;
}

bool parse_request(const std::string &raw, HttpRequest &request)
{
	const size_t header_end = raw.find("\r\n\r\n");
	if (header_end == std::string::npos)
		return false;

	std::istringstream stream(raw.substr(0, header_end));
	std::string line;
	if (!std::getline(stream, line))
		return false;
	if (!line.empty() && line.back() == '\r')
		line.pop_back();

	std::istringstream request_line(line);
	request_line >> request.method >> request.path;
	if (request.method.empty() || request.path.empty())
		return false;

	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		request.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
	}

	request.body = raw.substr(header_end + 4);
	return true;
}

std::string response_header(int status, const char *reason, const std::string &content_type, size_t length)
{
	std::ostringstream out;
	out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
	    << "Content-Length: " << length << "\r\n"
	    << "Content-Type: " << content_type << "\r\n"
	    << "Cache-Control: no-store\r\n"
	    << "Connection: close\r\n\r\n";
	return out.str();
}

bool build_common_response(const HttpRequest &request, HttpResponse &response)
{
	if (request.path.rfind("/proxy/", 0) == 0)
		return false;

	if (request.path.rfind("/config/", 0) == 0) {
		std::string id = request.path.substr(std::string("/config/").size());
		const size_t query = id.find('?');
		if (query != std::string::npos)
			id.resize(query);

		response.content_type = "application/json; charset=utf-8";
		response.body = config_json(id);
		if (response.body.empty()) {
			response.status = 404;
			response.reason = "Not Found";
			response.body = "{\"message\":\"source config not found\"}";
		}
		return true;
	}

	std::string path = request.path;
	const size_t query = path.find('?');
	if (query != std::string::npos)
		path.resize(query);

	response.status = 200;
	response.reason = "OK";
	if (path == "/" || path == "/index.html") {
		response.content_type = "text/html; charset=utf-8";
		response.body.assign(assets::index_html);
	} else if (path == "/app.js") {
		response.content_type = "text/javascript; charset=utf-8";
		response.body.assign(assets::app_js);
	} else if (path == "/styles.css") {
		response.content_type = "text/css; charset=utf-8";
		response.body.assign(assets::styles_css);
	} else {
		response.status = 404;
		response.reason = "Not Found";
		response.content_type = "text/plain; charset=utf-8";
		response.body = "Not found";
	}
	return true;
}

bool start_http_server()
{
	return start_http_server_platform();
}

void stop_http_server()
{
	stop_http_server_platform();
}

std::string http_server_base_url()
{
	return http_server_base_url_platform();
}

} // namespace obs_linkr
