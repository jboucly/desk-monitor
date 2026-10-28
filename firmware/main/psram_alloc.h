// Heap routing: LVGL objects and cJSON trees in PSRAM, internal RAM kept for Wi-Fi, stacks and draw buffers.
#pragma once

namespace dm::esp32 {

/**
 * @description Routes cJSON allocations to PSRAM. Call it first in app_main, before any JSON is parsed.
 * LVGL allocations are routed by psram_alloc.cpp through CONFIG_LV_USE_CUSTOM_MALLOC.
 */
void psram_alloc_init();

}  // namespace dm::esp32
