// LVGL configuration of the desktop simulator. Options not set here keep LVGL's defaults
// (lv_conf_internal.h). The device gets the equivalent settings from firmware/sdkconfig.defaults.
#ifndef LV_CONF_H
#define LV_CONF_H

// Same pixel format as the panel (RGB565)
#define LV_COLOR_DEPTH 16

// System malloc, no fixed LVGL heap
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

// Single-threaded: other threads only talk to the bus, which has its own lock
#define LV_USE_OS LV_OS_NONE

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

// Gradients with alpha stops are used by the glass cards
#define LV_GRADIENT_MAX_STOPS 2
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0

// Image cache: the wallpaper and the color wheel are drawn from RAM buffers
#define LV_CACHE_DEF_SIZE 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_DIRECT_EXIT 1

#define LV_USE_SYSMON 0

// JPEG decoder for Home Assistant covers (used directly by core/src/cover.cpp)
#define LV_USE_TJPGD 1

#endif  // LV_CONF_H
