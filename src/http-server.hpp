#pragma once

#include <string>

namespace obs_linkr {

bool start_http_server();
void stop_http_server();
std::string http_server_base_url();

} // namespace obs_linkr
