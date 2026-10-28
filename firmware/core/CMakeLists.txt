# Portable core of the Desk Monitor: data bus, layout, UI, widgets, HA/weather protocol logic, REST API.
# Built as an ESP-IDF component for the device, and as a static library for the desktop simulator.

file(GLOB_RECURSE DM_CORE_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

if(ESP_PLATFORM)
  idf_component_register(
    SRCS ${DM_CORE_SOURCES}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src
    REQUIRES json)
  target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
else()
  add_library(dm_core STATIC ${DM_CORE_SOURCES})
  target_include_directories(dm_core PUBLIC include PRIVATE src)
  target_compile_features(dm_core PUBLIC cxx_std_17)
  target_link_libraries(dm_core PUBLIC lvgl cjson)
endif()
