// Screen of the Guition ESP32-S3-4848S040: ST7701S RGB panel, GT911 touch, PWM backlight, sleep on inactivity.
#pragma once

#include "lvgl.h"

namespace dm::esp32 {

/**
 * @description Initializes the panel, the touch controller, the backlight and the LVGL task (esp_lvgl_port).
 * After this, LVGL calls from other tasks must be wrapped in lvgl_port_lock() / lvgl_port_unlock().
 */
void display_init();

/**
 * @description Sets the backlight level (5-100 %) and the inactivity delay before it turns off (0 = never).
 */
void display_configure(int brightness_pct, int sleep_minutes);

/**
 * @description Draws the first frame, then turns the backlight on. Call once the UI is built:
 * until then the backlight stays off (also in the bootloader, see bootloader_components/).
 */
void display_show();

}  // namespace dm::esp32
