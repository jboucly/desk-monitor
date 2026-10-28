# Desktop simulator of the Desk Monitor (480x480 SDL window, mouse = touch).
# Builds the same portable core as the device, with mock HA/PC data, real Open-Meteo weather,
# and the REST API + web editor served on http://localhost:8080.
#
#   cmake -S firmware/sim -B firmware/sim/build && cmake --build firmware/sim/build -j
#   firmware/sim/build/desk_monitor_sim

cmake_minimum_required(VERSION 3.20)
project(desk_monitor_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ── LVGL (same version as firmware/core/idf_component.yml) ─────────────────
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.5.0 GIT_SHALLOW TRUE)

# ── cJSON (the device uses the copy bundled with ESP-IDF) ──────────────────
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cjson GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git GIT_TAG v1.7.18 GIT_SHALLOW TRUE)

# ── cpp-httplib (header-only HTTP server for the API and the web editor) ───
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.18.3 GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(lvgl cjson)
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
  FetchContent_Populate(httplib)
endif()

# cJSON's CMake target has no usage requirements: expose its headers
target_include_directories(cjson PUBLIC $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>)

# ── SDL2 and libcurl from the system (brew install sdl2 curl) ──────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(CURL REQUIRED)
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

# ── Portable core ──────────────────────────────────────────────────────────
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/core)

# ── Simulator executable ───────────────────────────────────────────────────
add_executable(desk_monitor_sim main.cpp mock.cpp png.cpp ws_server.cpp)
target_include_directories(desk_monitor_sim PRIVATE ${httplib_SOURCE_DIR})
target_link_libraries(desk_monitor_sim PRIVATE dm_core lvgl PkgConfig::SDL2 CURL::libcurl)
target_compile_definitions(desk_monitor_sim PRIVATE
  DM_DEFAULT_LAYOUT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../data/layout.json"
  DM_WEB_DIST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../web/dist"
  DM_SIM_DATA_PATH="${CMAKE_BINARY_DIR}/data")
