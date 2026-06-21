#pragma once

#include <cstdint>

namespace obs_linkr {

inline constexpr const char *LINKR_SOURCE_ID = "linkr_webrtc_source";
inline constexpr const char *BROWSER_SOURCE_ID = "browser_source";
inline constexpr uint32_t DEFAULT_WIDTH = 1920;
inline constexpr uint32_t DEFAULT_HEIGHT = 1080;
inline constexpr uint32_t DEFAULT_FPS = 30;
inline constexpr uint16_t DEFAULT_INTERNAL_PORT = 19773;
inline constexpr uint16_t MAX_INTERNAL_PORT = 19793;

} // namespace obs_linkr
