#include "http-server-internal.hpp"

#ifndef _WIN32

#include "constants.hpp"
#include "utils.hpp"

#include <obs-module.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace obs_linkr {
namespace {

class LinkrHttpServer {
public:
	~LinkrHttpServer() { stop(); }

	bool start()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (running_)
			return true;

		for (uint16_t port = DEFAULT_INTERNAL_PORT; port <= MAX_INTERNAL_PORT; ++port) {
			int candidate = create_listener(port);
			if (candidate >= 0) {
				listener_ = candidate;
				port_ = port;
				break;
			}
		}

		if (listener_ < 0) {
			blog(LOG_ERROR, "[obs-linkr] Failed to bind internal HTTP server on localhost.");
			return false;
		}

		running_ = true;
		thread_ = std::thread([this]() { run(); });
		blog(LOG_INFO, "[obs-linkr] Internal HTTP server listening on http://127.0.0.1:%u", port_);
		return true;
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!running_)
				return;
			running_ = false;
			if (listener_ >= 0) {
				::shutdown(listener_, SHUT_RDWR);
				::close(listener_);
				listener_ = -1;
			}
		}

		if (thread_.joinable())
			thread_.join();
	}

	std::string base_url() const
	{
		if (!running_ || port_ == 0)
			return {};
		return "http://127.0.0.1:" + std::to_string(port_) + "/";
	}

private:
	mutable std::mutex mutex_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	int listener_ = -1;
	uint16_t port_ = 0;

	struct ParsedUrl {
		std::string scheme;
		std::string host;
		std::string port;
	};

	static int create_listener(uint16_t port)
	{
		int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock < 0)
			return -1;

		int yes = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);

		if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(sock, SOMAXCONN) != 0) {
			::close(sock);
			return -1;
		}

		return sock;
	}

	void run()
	{
		while (running_) {
			int client = accept(listener_, nullptr, nullptr);
			if (client < 0) {
				if (running_)
					blog(LOG_WARNING, "[obs-linkr] accept failed: %d", errno);
				break;
			}
			std::thread([this, client]() { handle_client(client); }).detach();
		}
	}

	static bool send_all(int sock, const char *data, size_t size)
	{
		size_t sent = 0;
		while (sent < size) {
			const ssize_t chunk = send(sock, data + sent, (std::min<size_t>)(size - sent, 16384), 0);
			if (chunk <= 0)
				return false;
			sent += static_cast<size_t>(chunk);
		}
		return true;
	}

	static bool recv_request(int sock, HttpRequest &request)
	{
		std::string raw;
		char buffer[8192];
		size_t header_end = std::string::npos;

		while (raw.size() < 1024 * 1024) {
			const ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
			if (received <= 0)
				return false;
			raw.append(buffer, static_cast<size_t>(received));
			header_end = raw.find("\r\n\r\n");
			if (header_end != std::string::npos)
				break;
		}

		if (header_end == std::string::npos || !parse_request(raw, request))
			return false;

		const std::string content_length = header_value(request, "content-length");
		const size_t expected =
			content_length.empty() ? 0 : static_cast<size_t>(std::strtoull(content_length.c_str(), nullptr, 10));
		while (request.body.size() < expected && raw.size() < (1024 * 1024 * 8)) {
			const ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
			if (received <= 0)
				return false;
			request.body.append(buffer, static_cast<size_t>(received));
		}

		if (request.body.size() > expected)
			request.body.resize(expected);

		return true;
	}

	static void send_response(int sock, int status, const char *reason, const std::string &content_type,
				  std::string_view body)
	{
		const std::string header = response_header(status, reason, content_type, body.size());
		send_all(sock, header.data(), header.size());
		send_all(sock, body.data(), body.size());
	}

	static bool parse_base_url(std::string url, ParsedUrl &parsed)
	{
		if (!has_scheme(url))
			url = "http://" + url;

		const size_t scheme_end = url.find("://");
		if (scheme_end == std::string::npos)
			return false;

		parsed.scheme = to_lower(url.substr(0, scheme_end));
		if (parsed.scheme != "http")
			return false;

		size_t authority_start = scheme_end + 3;
		size_t authority_end = url.find_first_of("/?#", authority_start);
		std::string authority = url.substr(authority_start, authority_end - authority_start);

		const size_t at = authority.rfind('@');
		if (at != std::string::npos)
			authority.erase(0, at + 1);

		if (authority.empty())
			return false;

		if (authority.front() == '[') {
			const size_t bracket = authority.find(']');
			if (bracket == std::string::npos)
				return false;
			parsed.host = authority.substr(1, bracket - 1);
			if (bracket + 1 < authority.size() && authority[bracket + 1] == ':')
				parsed.port = authority.substr(bracket + 2);
		} else {
			const size_t colon = authority.rfind(':');
			if (colon != std::string::npos) {
				parsed.host = authority.substr(0, colon);
				parsed.port = authority.substr(colon + 1);
			} else {
				parsed.host = authority;
			}
		}

		if (parsed.host.empty())
			return false;
		if (parsed.port.empty())
			parsed.port = "80";
		return true;
	}

	static int connect_upstream(const ParsedUrl &target)
	{
		addrinfo hints = {};
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_family = AF_UNSPEC;

		addrinfo *results = nullptr;
		if (getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &results) != 0)
			return -1;

		int sock = -1;
		for (addrinfo *item = results; item; item = item->ai_next) {
			sock = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
			if (sock < 0)
				continue;
			if (connect(sock, item->ai_addr, item->ai_addrlen) == 0)
				break;
			::close(sock);
			sock = -1;
		}

		freeaddrinfo(results);
		return sock;
	}

	static std::string host_header(const ParsedUrl &target)
	{
		if (target.port == "80")
			return target.host;
		if (target.host.find(':') != std::string::npos)
			return "[" + target.host + "]:" + target.port;
		return target.host + ":" + target.port;
	}

	static bool read_all(int sock, std::string &out)
	{
		char buffer[8192];
		for (;;) {
			const ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
			if (received < 0)
				return false;
			if (received == 0)
				return true;
			out.append(buffer, static_cast<size_t>(received));
			if (out.size() > 1024 * 1024 * 16)
				return false;
		}
	}

	static bool decode_chunked(const std::string &chunked, std::string &decoded)
	{
		size_t pos = 0;
		while (pos < chunked.size()) {
			const size_t line_end = chunked.find("\r\n", pos);
			if (line_end == std::string::npos)
				return false;

			std::string size_text = chunked.substr(pos, line_end - pos);
			const size_t semicolon = size_text.find(';');
			if (semicolon != std::string::npos)
				size_text.resize(semicolon);

			char *end = nullptr;
			const unsigned long size = std::strtoul(size_text.c_str(), &end, 16);
			if (!end || *end != '\0')
				return false;

			pos = line_end + 2;
			if (size == 0)
				return true;
			if (pos + size + 2 > chunked.size())
				return false;

			decoded.append(chunked.data() + pos, static_cast<size_t>(size));
			pos += static_cast<size_t>(size);
			if (chunked.compare(pos, 2, "\r\n") != 0)
				return false;
			pos += 2;
		}
		return false;
	}

	static int parse_upstream_response(const std::string &raw, std::string &body)
	{
		const size_t header_end = raw.find("\r\n\r\n");
		if (header_end == std::string::npos)
			return 502;

		std::istringstream stream(raw.substr(0, header_end));
		std::string status_line;
		if (!std::getline(stream, status_line))
			return 502;
		if (!status_line.empty() && status_line.back() == '\r')
			status_line.pop_back();

		std::istringstream status_stream(status_line);
		std::string http_version;
		int status = 502;
		status_stream >> http_version >> status;
		if (status < 100 || status > 599)
			status = 502;

		std::map<std::string, std::string> headers;
		std::string line;
		while (std::getline(stream, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		}

		body = raw.substr(header_end + 4);
		const auto transfer_encoding = headers.find("transfer-encoding");
		if (transfer_encoding != headers.end() && to_lower(transfer_encoding->second).find("chunked") != std::string::npos) {
			std::string decoded;
			if (decode_chunked(body, decoded))
				body = std::move(decoded);
		}

		return status;
	}

	void handle_proxy(int sock, const HttpRequest &request)
	{
		std::string target_base = header_value(request, "x-linkr-base");
		if (target_base.empty()) {
			send_response(sock, 400, "Bad Request", "application/json; charset=utf-8",
				      "{\"code\":\"missing_target\",\"message\":\"Missing X-Linkr-Base header\"}");
			return;
		}

		ParsedUrl target;
		if (!parse_base_url(target_base, target)) {
			send_response(sock, 400, "Bad Request", "application/json; charset=utf-8",
				      "{\"code\":\"bad_target\",\"message\":\"Invalid or unsupported X-Linkr-Base header\"}");
			return;
		}

		std::string path = request.path.substr(std::string("/proxy").size());
		if (path.empty())
			path = "/";

		int upstream = connect_upstream(target);
		if (upstream < 0) {
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"Upstream connection failed\"}");
			return;
		}

		std::ostringstream upstream_request;
		upstream_request << request.method << ' ' << path << " HTTP/1.1\r\n"
				 << "Host: " << host_header(target) << "\r\n"
				 << "Connection: close\r\n"
				 << "User-Agent: obs-linkr/0.1\r\n";

		for (const char *name : {"authorization", "content-type", "accept"}) {
			const std::string value = header_value(request, name);
			if (!value.empty())
				upstream_request << name << ": " << value << "\r\n";
		}

		if (!request.body.empty())
			upstream_request << "Content-Length: " << request.body.size() << "\r\n";
		upstream_request << "\r\n" << request.body;

		const std::string wire_request = upstream_request.str();
		if (!send_all(upstream, wire_request.data(), wire_request.size())) {
			::close(upstream);
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"Upstream request failed\"}");
			return;
		}

		std::string raw_response;
		if (!read_all(upstream, raw_response)) {
			::close(upstream);
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"Upstream request failed\"}");
			return;
		}
		::close(upstream);

		std::string body;
		const int status = parse_upstream_response(raw_response, body);
		send_response(sock, static_cast<int>(status), status >= 200 && status < 300 ? "OK" : "Upstream",
			      "application/json; charset=utf-8", body);
	}

	void handle_client(int client)
	{
		HttpRequest request;
		if (!recv_request(client, request)) {
			::close(client);
			return;
		}

		HttpResponse response;
		if (build_common_response(request, response)) {
			send_response(client, response.status, response.reason, response.content_type, response.body);
		} else {
			handle_proxy(client, request);
		}

		::shutdown(client, SHUT_RDWR);
		::close(client);
	}
};

LinkrHttpServer server;

} // namespace

bool start_http_server_platform()
{
	return server.start();
}

void stop_http_server_platform()
{
	server.stop();
}

std::string http_server_base_url_platform()
{
	return server.base_url();
}

} // namespace obs_linkr

#endif
