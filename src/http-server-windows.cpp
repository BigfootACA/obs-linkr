#include "http-server-internal.hpp"

#ifdef _WIN32

#include "constants.hpp"
#include "utils.hpp"

#include <obs-module.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

		WSADATA wsa_data = {};
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
			blog(LOG_ERROR, "[obs-linkr] WSAStartup failed.");
			return false;
		}
		wsa_started_ = true;

		for (uint16_t port = DEFAULT_INTERNAL_PORT; port <= MAX_INTERNAL_PORT; ++port) {
			SOCKET candidate = create_listener(port);
			if (candidate != INVALID_SOCKET) {
				listener_ = candidate;
				port_ = port;
				break;
			}
		}

		if (listener_ == INVALID_SOCKET) {
			blog(LOG_ERROR, "[obs-linkr] Failed to bind internal HTTP server on localhost.");
			WSACleanup();
			wsa_started_ = false;
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
			if (listener_ != INVALID_SOCKET) {
				closesocket(listener_);
				listener_ = INVALID_SOCKET;
			}
		}

		if (thread_.joinable())
			thread_.join();

		if (wsa_started_) {
			WSACleanup();
			wsa_started_ = false;
		}
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
	bool wsa_started_ = false;
	SOCKET listener_ = INVALID_SOCKET;
	uint16_t port_ = 0;

	static SOCKET create_listener(uint16_t port)
	{
		SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			return INVALID_SOCKET;

		BOOL yes = TRUE;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);

		if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
		    listen(sock, SOMAXCONN) == SOCKET_ERROR) {
			closesocket(sock);
			return INVALID_SOCKET;
		}

		return sock;
	}

	void run()
	{
		while (running_) {
			SOCKET client = accept(listener_, nullptr, nullptr);
			if (client == INVALID_SOCKET)
				break;
			std::thread([this, client]() { handle_client(client); }).detach();
		}
	}

	static bool send_all(SOCKET sock, const char *data, size_t size)
	{
		size_t sent = 0;
		while (sent < size) {
			const int chunk = send(sock, data + sent, static_cast<int>((std::min<size_t>)(size - sent, 16384)), 0);
			if (chunk <= 0)
				return false;
			sent += static_cast<size_t>(chunk);
		}
		return true;
	}

	static bool recv_request(SOCKET sock, HttpRequest &request)
	{
		std::string raw;
		char buffer[8192];
		size_t header_end = std::string::npos;

		while (raw.size() < 1024 * 1024) {
			const int received = recv(sock, buffer, sizeof(buffer), 0);
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
			const int received = recv(sock, buffer, sizeof(buffer), 0);
			if (received <= 0)
				return false;
			request.body.append(buffer, static_cast<size_t>(received));
		}

		if (request.body.size() > expected)
			request.body.resize(expected);

		return true;
	}

	static void send_response(SOCKET sock, int status, const char *reason, const std::string &content_type,
				  std::string_view body)
	{
		const std::string header = response_header(status, reason, content_type, body.size());
		send_all(sock, header.data(), header.size());
		send_all(sock, body.data(), body.size());
	}

	static std::wstring widen(const std::string &value)
	{
		if (value.empty())
			return {};
		const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		std::wstring wide(static_cast<size_t>((std::max)(0, size - 1)), L'\0');
		if (size > 1)
			MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
		return wide;
	}

	static bool parse_url(const std::string &base, URL_COMPONENTS &parts, std::wstring &wide)
	{
		wide = widen(base);
		ZeroMemory(&parts, sizeof(parts));
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		return WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == TRUE;
	}

	void handle_proxy(SOCKET sock, const HttpRequest &request)
	{
		std::string target_base = header_value(request, "x-linkr-base");
		if (target_base.empty()) {
			send_response(sock, 400, "Bad Request", "application/json; charset=utf-8",
				      "{\"code\":\"missing_target\",\"message\":\"Missing X-Linkr-Base header\"}");
			return;
		}
		if (!has_scheme(target_base))
			target_base = "http://" + target_base;

		URL_COMPONENTS parts = {};
		std::wstring wide_base;
		if (!parse_url(target_base, parts, wide_base)) {
			send_response(sock, 400, "Bad Request", "application/json; charset=utf-8",
				      "{\"code\":\"bad_target\",\"message\":\"Invalid X-Linkr-Base header\"}");
			return;
		}

		std::string path = request.path.substr(std::string("/proxy").size());
		if (path.empty())
			path = "/";
		const std::wstring wide_path = widen(path);
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);

		HINTERNET session = WinHttpOpen(L"obs-linkr/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
						WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) {
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"WinHttpOpen failed\"}");
			return;
		}

		HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
		if (!connect) {
			WinHttpCloseHandle(session);
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"WinHttpConnect failed\"}");
			return;
		}

		const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
		const std::wstring method = widen(request.method);
		HINTERNET upstream = WinHttpOpenRequest(connect, method.c_str(), wide_path.c_str(), nullptr,
							WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!upstream) {
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"WinHttpOpenRequest failed\"}");
			return;
		}

		std::wstring headers;
		for (const char *name : {"authorization", "content-type", "accept"}) {
			const std::string value = header_value(request, name);
			if (!value.empty())
				headers += widen(std::string(name) + ": " + value + "\r\n");
		}

		const BOOL sent = WinHttpSendRequest(upstream, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
						    headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
						    request.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)request.body.data(),
						    static_cast<DWORD>(request.body.size()),
						    static_cast<DWORD>(request.body.size()), 0);
		if (!sent || !WinHttpReceiveResponse(upstream, nullptr)) {
			WinHttpCloseHandle(upstream);
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			send_response(sock, 502, "Bad Gateway", "application/json; charset=utf-8",
				      "{\"code\":\"proxy_error\",\"message\":\"Upstream request failed\"}");
			return;
		}

		DWORD status = 502;
		DWORD status_size = sizeof(status);
		WinHttpQueryHeaders(upstream, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

		std::string body;
		for (;;) {
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(upstream, &available) || available == 0)
				break;
			std::string chunk(available, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(upstream, chunk.data(), available, &read) || read == 0)
				break;
			chunk.resize(read);
			body += chunk;
		}

		WinHttpCloseHandle(upstream);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);

		send_response(sock, static_cast<int>(status), status >= 200 && status < 300 ? "OK" : "Upstream",
			      "application/json; charset=utf-8", body);
	}

	void handle_client(SOCKET client)
	{
		HttpRequest request;
		if (!recv_request(client, request)) {
			closesocket(client);
			return;
		}

		HttpResponse response;
		if (build_common_response(request, response)) {
			send_response(client, response.status, response.reason, response.content_type, response.body);
		} else {
			handle_proxy(client, request);
		}

		shutdown(client, SD_BOTH);
		closesocket(client);
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
