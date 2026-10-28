# ESP32 platform layer: display, storage, Wi-Fi, Home Assistant transport, weather, HTTP server.

idf_component_register(
  SRCS main.cpp psram_alloc.cpp display.cpp storage.cpp network.cpp ha_client.cpp weather_task.cpp http_server.cpp
  INCLUDE_DIRS .
  REQUIRES core json nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_http_server esp_timer
           app_update esp_app_format esp_lcd esp_driver_i2c esp_driver_ledc esp_psram mbedtls
  EMBED_FILES ${CMAKE_CURRENT_LIST_DIR}/../data/layout.json
  # psram_alloc.cpp defines LVGL's allocator: only lvgl references it, so keep every object file
  WHOLE_ARCHIVE)

# Web editor: the gzipped single-file build of web/, or a fallback page (served as is)
set(WEB_OUT ${CMAKE_CURRENT_BINARY_DIR}/web.bin)
add_custom_target(dm_web_bundle
  COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_LIST_DIR}/../../web/dist/index.html.gz
    -DFALLBACK=${CMAKE_CURRENT_LIST_DIR}/www_fallback.html
    -DOUT=${WEB_OUT}
    -P ${CMAKE_CURRENT_LIST_DIR}/select_web.cmake
  BYPRODUCTS ${WEB_OUT}
  VERBATIM)
target_add_binary_data(${COMPONENT_LIB} ${WEB_OUT} BINARY DEPENDS dm_web_bundle)
