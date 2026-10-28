// Heap routing: LVGL objects and cJSON trees go to PSRAM.
// With CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, plain malloc() keeps every small block in internal RAM, and the
// hundreds of small LVGL objects of a layout exhausted it before Wi-Fi could start. Internal RAM stays for
// Wi-Fi, task stacks and the LVGL draw buffers (allocated by esp_lvgl_port with heap_caps directly).
#include "psram_alloc.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

namespace {

void *psram_malloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

}  // namespace

// LVGL allocator, selected by CONFIG_LV_USE_CUSTOM_MALLOC (see lvgl/src/stdlib/lv_mem.h)
extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *, size_t) { return nullptr; }

void lv_mem_remove_pool(lv_mem_pool_t) {}

void *lv_malloc_core(size_t size) { return psram_malloc(size); }

void *lv_realloc_core(void *p, size_t new_size) {
  return heap_caps_realloc_prefer(p, new_size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *) {}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

namespace dm::esp32 {

void psram_alloc_init() {
  cJSON_Hooks hooks = {};
  hooks.malloc_fn = psram_malloc;
  hooks.free_fn = heap_caps_free;
  cJSON_InitHooks(&hooks);
}

}  // namespace dm::esp32
