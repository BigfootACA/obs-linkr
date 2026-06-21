cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES CXX)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html" OBS_LINKR_INDEX_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/web/app.js" OBS_LINKR_APP_JS)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/web/styles.css" OBS_LINKR_STYLES_CSS)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedded-assets.generated.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded-assets.generated.hpp"
  @ONLY
)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/config-store.cpp
    src/http-server.cpp
    src/http-server-unix.cpp
    src/http-server-windows.cpp
    src/linkr-source.cpp
    src/plugin-main.cpp
    src/utils.cpp
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OBS_LINKR_VERSION="${PROJECT_VERSION}")

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 winhttp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
