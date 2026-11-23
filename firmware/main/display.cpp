#include "display.h"

#include <atomic>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace dm::esp32 {

namespace {

const char *TAG = "display";

// ── Board wiring (Guition ESP32-S3-4848S040, same as ESPHome's "GUITION-4848S040" model) ──
constexpr int WIDTH = 480;
constexpr int HEIGHT = 480;
constexpr gpio_num_t PIN_LCD_CS = GPIO_NUM_39;
constexpr gpio_num_t PIN_LCD_SCK = GPIO_NUM_48;
constexpr gpio_num_t PIN_LCD_SDA = GPIO_NUM_47;
constexpr gpio_num_t PIN_DE = GPIO_NUM_18;
constexpr gpio_num_t PIN_HSYNC = GPIO_NUM_16;
constexpr gpio_num_t PIN_VSYNC = GPIO_NUM_17;
constexpr gpio_num_t PIN_PCLK = GPIO_NUM_21;
// RGB565 data lines, in esp_lcd order: B0..B4, G0..G5, R0..R4
constexpr int PIN_DATA[16] = {4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0};
constexpr gpio_num_t PIN_BACKLIGHT = GPIO_NUM_38;
constexpr gpio_num_t PIN_TOUCH_SDA = GPIO_NUM_19;
constexpr gpio_num_t PIN_TOUCH_SCL = GPIO_NUM_45;

// Pixel clock. One frame is (480 + 40) x (480 + 30) clocks: 6 MHz (ESPHome's value) refreshes the panel at
// ~22 Hz. 16 MHz made the picture shake and shift on the real board: the bounce buffers, refilled by the
// CPU from the PSRAM frame buffer, could not keep up.
constexpr uint32_t PCLK_HZ = 6 * 1000 * 1000;

// Two PSRAM frame buffers: LVGL draws into the hidden one (direct mode) and the panel switches at the end
// of a frame, so a half-drawn frame is never shown (no tearing during swipes). It also removes the copy of
// LVGL's partial buffers into the frame buffer. Set to false to go back to one frame buffer.
constexpr bool DOUBLE_BUFFER = true;

// ── Orientation ──────────────────────────────────────────────────────────────
// The legacy ESPHome config mirrored the panel on X and Y (ST7701 SDIR/MADCTL) and rotated LVGL by 180°:
// the two cancel out. Neither is done here. A software mirror would also be costly: with mirror X, the RGB
// driver copies LVGL's buffers pixel by pixel instead of with memcpy.
// If the picture or the touch is flipped on a new board, prefer the SDIR/MADCTL bits of the init sequence.
constexpr bool FLIP_X = false;
constexpr bool FLIP_Y = false;
constexpr bool TOUCH_MIRROR_X = false;
constexpr bool TOUCH_MIRROR_Y = false;

/**
 * @description One ST7701S init command; the sequence is ESPHome's ST7701S + GUITION-4848S040 model.
 */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[16];
  uint16_t delay_ms;
};

const InitCmd INIT_SEQUENCE[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}, 0},  // command2 bank 0
    {0xC0, 2, {0x3B, 0x00}, 0},
    {0xC1, 2, {0x0D, 0x02}, 0},
    {0xC2, 2, {0x31, 0x05}, 0},
    {0xB0, 16, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 0},
    {0xB1, 16, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}, 0},  // command2 bank 1
    {0xB0, 1, {0x60}, 0},
    {0xB1, 1, {0x32}, 0},
    {0xB2, 1, {0x07}, 0},
    {0xB3, 1, {0x80}, 0},
    {0xB5, 1, {0x49}, 0},
    {0xB7, 1, {0x85}, 0},
    {0xB8, 1, {0x21}, 0},
    {0xC1, 1, {0x78}, 0},
    {0xC2, 1, {0x78}, 0},
    {0xE0, 3, {0x00, 0x1B, 0x02}, 0},
    {0xE1, 11, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 0},
    {0xE2, 12, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 0},
    {0xE3, 4, {0x00, 0x00, 0x11, 0x11}, 0},
    {0xE4, 2, {0x44, 0x44}, 0},
    {0xE5, 16, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 0},
    {0xE6, 4, {0x00, 0x00, 0x11, 0x11}, 0},
    {0xE7, 2, {0x44, 0x44}, 0},
    {0xE8, 16, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 0},
    {0xEB, 7, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 0},
    {0xEC, 2, {0x3C, 0x00}, 0},
    {0xED, 16, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}, 0},  // command2 bank 3
    {0xE5, 1, {0xE4}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}, 0},  // command2 bank 0
    {0xCD, 1, {0x08}, 0},
    {0xCD, 1, {0x00}, 0},  // Guition: 16-bit bus
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}, 0},  // command2 off
    {0x3A, 1, {0x66}, 0},  // pixel format: 18 bit
    // Scan direction, sent like ESPHome does: SDIR only exists in command2 bank 0 (ignored otherwise)
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}, 0},  // command2 bank 0
    {0xC7, 1, {0x00}, 0},  // SDIR: normal (0x04 mirrors X)
    {0x36, 1, {0x00}, 0},  // MADCTL: normal (0x10 mirrors Y)
    {0x20, 0, {}, 0},      // inversion off
    {0x11, 0, {}, 120},    // sleep out
    {0x29, 0, {}, 10},     // display on
};

// ── Backlight and sleep ──────────────────────────────────────────────────────
std::atomic<int> g_brightness{100};
std::atomic<int> g_sleep_minutes{5};
std::atomic<bool> g_asleep{false};
std::atomic<bool> g_wake_request{false};
lv_indev_t *g_touch = nullptr;
esp_lcd_touch_handle_t g_touch_handle = nullptr;
std::atomic<bool> g_shown{false};  // the backlight stays off until the first frame is drawn

// Wake-up trace, kept in NVS so that it survives a power loss (a power-on reset clears the RTC memory)
constexpr const char *TRACE_NAMESPACE = "dm_display";
constexpr const char *TRACE_KEY = "state";
constexpr uint8_t TRACE_AWAKE = 1, TRACE_ASLEEP = 2, TRACE_WAKING = 3;
constexpr int AWAKE_AFTER_TICKS = 20;  // sleep task ticks (500 ms) after a wake-up before it counts as done
constexpr uint32_t WAKE_FADE_MS = 800;
uint8_t g_trace = 0;                  // last value written, to write only on changes
const char *g_previous_state = "unknown";

/**
 * @description Name of a trace value, as GET /api/status shows it.
 */
const char *trace_name(uint8_t value) {
  switch (value) {
    case TRACE_AWAKE: return "awake";
    case TRACE_ASLEEP: return "asleep";
    case TRACE_WAKING: return "waking";
    default: return "unknown";
  }
}

/**
 * @description Stores the display state in NVS when it changes. Only called on sleep and wake-up
 * transitions, never periodically, so that it does not wear the flash. Runs in the sleep task (or at init).
 */
void trace_display(uint8_t value) {
  if (value == g_trace) return;
  nvs_handle_t handle;
  if (nvs_open(TRACE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  if (nvs_set_u8(handle, TRACE_KEY, value) == ESP_OK && nvs_commit(handle) == ESP_OK) g_trace = value;
  nvs_close(handle);
}

/**
 * @description Reads the state the display was in when the chip last stopped, then marks it awake.
 */
void trace_init() {
  nvs_handle_t handle;
  uint8_t value = 0;
  if (nvs_open(TRACE_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u8(handle, TRACE_KEY, &value);
    nvs_close(handle);
  }
  g_trace = value;
  g_previous_state = trace_name(value);
  ESP_LOGI(TAG, "display state before this boot: %s", g_previous_state);
  trace_display(TRACE_AWAKE);
}

/**
 * @description Asks the LVGL task to redraw the whole screen and waits for it, so that the backlight
 * never reveals an empty or stale frame. The drawing itself must stay in the LVGL task: rendering
 * needs far more stack than the callers (sleep task, main task) have.
 */
void redraw_and_wait() {
  if (lvgl_port_lock(100)) {
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
  }
  lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
  // A full redraw takes ~60-100 ms; the double buffer then switches at the next frame
  vTaskDelay(pdMS_TO_TICKS(150));
}

/**
 * @description Converts a backlight percentage into a 13-bit LEDC duty.
 */
uint32_t backlight_duty(int pct) { return (8191u * static_cast<uint32_t>(pct)) / 100u; }

/**
 * @description Sets the backlight level at once (the fade API is used so that it never races a fade).
 */
void backlight_set(int pct) {
  ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, backlight_duty(pct), 0);
}

/**
 * @description Ramps the backlight up instead of switching it on at once. A step from 0 to full brightness
 * is a current spike on the 5 V rail, and on wake-up it adds to the Wi-Fi peaks and the full redraw: a
 * voltage dip there resets the chip (brownout).
 */
void backlight_fade_in(int pct, uint32_t ms = 250) {
  ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, backlight_duty(pct), ms, LEDC_FADE_NO_WAIT);
}

void backlight_init() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_13_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 100;  // the frequency the ESPHome config used for this board
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ledc_channel_config_t channel = {};
  channel.gpio_num = PIN_BACKLIGHT;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
  ESP_ERROR_CHECK(ledc_fade_func_install(0));
}

/**
 * @description Touch read callback, replacing esp_lvgl_port's one: that one wraps the I2C reads in
 * ESP_ERROR_CHECK, so a single failed transfer with the GT911 aborted and rebooted the screen (seen at
 * wake-up). Here an error only drops the sample: the touch reads as released and the next poll retries.
 * Runs in the LVGL task.
 */
void touch_read(lv_indev_t *, lv_indev_data_t *data) {
  static uint32_t errors = 0;
  data->state = LV_INDEV_STATE_RELEASED;
  esp_err_t err = esp_lcd_touch_read_data(g_touch_handle);
  esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {};
  uint8_t count = 0;
  if (err == ESP_OK) err = esp_lcd_touch_get_data(g_touch_handle, points, &count, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
  if (err != ESP_OK) {
    // Rate-limited: a dead bus would otherwise flood the log at the polling rate
    if (errors++ % 100 == 0) ESP_LOGW(TAG, "touch read failed (%s), %u errors", esp_err_to_name(err), (unsigned)errors);
    return;
  }
  if (count > 0) {
    data->point.x = points[0].x;
    data->point.y = points[0].y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
}

/**
 * @description Touch filter: while the screen sleeps, the first touch only wakes it up. LVGL then
 * ignores that touch until the finger is lifted, so it never presses a widget. The point count must
 * not be changed here: the GT911 driver rejects a count of 0 with an error that esp_lvgl_port turns
 * into an abort (the screen used to reboot, with a white flash, on every wake-up).
 * Runs in the LVGL task, inside the touch read callback.
 */
void filter_touch(esp_lcd_touch_handle_t, uint16_t *, uint16_t *, uint16_t *, uint8_t *point_num, uint8_t) {
  if (*point_num > 0 && g_asleep && !g_wake_request) {
    g_wake_request = true;
    if (g_touch) lv_indev_wait_release(g_touch);
  }
}

/**
 * @description Turns the backlight off after the inactivity delay and back on when touched.
 */
void sleep_task(void *) {
  int awake_countdown = 0;  // ticks left before a wake-up is traced as done
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (g_wake_request.exchange(false) && g_asleep) {
      trace_display(TRACE_WAKING);
      g_asleep = false;
      if (lvgl_port_lock(100)) {
        lv_display_trigger_activity(nullptr);
        lvgl_port_unlock();
      }
      redraw_and_wait();
      // Let the redraw and its flash reads settle before the backlight draws its current, then ramp it
      // up slowly: the load step at wake-up is what cuts the power on a weak supply
      vTaskDelay(pdMS_TO_TICKS(100));
      backlight_fade_in(g_brightness, WAKE_FADE_MS);
      awake_countdown = AWAKE_AFTER_TICKS;
      continue;
    }
    if (awake_countdown > 0 && --awake_countdown == 0 && !g_asleep) trace_display(TRACE_AWAKE);
    const int minutes = g_sleep_minutes;
    if (minutes <= 0 || g_asleep) continue;
    uint32_t inactive = 0;
    if (lvgl_port_lock(100)) {
      inactive = lv_display_get_inactive_time(nullptr);
      lvgl_port_unlock();
    }
    if (inactive > static_cast<uint32_t>(minutes) * 60000u) {
      g_asleep = true;
      awake_countdown = 0;
      backlight_set(0);
      trace_display(TRACE_ASLEEP);
    }
  }
}

}  // namespace

void display_init() {
  trace_init();
  backlight_init();

  // 3-wire SPI used only to send the ST7701S init sequence
  esp_lcd_panel_io_3wire_spi_config_t io_config = {};
  io_config.line_config.cs_io_type = IO_TYPE_GPIO;
  io_config.line_config.cs_gpio_num = PIN_LCD_CS;
  io_config.line_config.scl_io_type = IO_TYPE_GPIO;
  io_config.line_config.scl_gpio_num = PIN_LCD_SCK;
  io_config.line_config.sda_io_type = IO_TYPE_GPIO;
  io_config.line_config.sda_gpio_num = PIN_LCD_SDA;
  io_config.line_config.io_expander = nullptr;
  io_config.expect_clk_speed = PANEL_IO_3WIRE_SPI_CLK_MAX;
  io_config.spi_mode = 0;
  io_config.lcd_cmd_bytes = 1;
  io_config.lcd_param_bytes = 1;
  io_config.flags.use_dc_bit = 1;
  io_config.flags.del_keep_cs_inactive = 1;
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &io));

  // RGB timing of the ST7701S model in ESPHome, frame buffer(s) in PSRAM, 20-line bounce buffers
  esp_lcd_rgb_panel_config_t rgb = {};
  rgb.clk_src = LCD_CLK_SRC_DEFAULT;
  rgb.timings.pclk_hz = PCLK_HZ;
  rgb.timings.h_res = WIDTH;
  rgb.timings.v_res = HEIGHT;
  rgb.timings.hsync_pulse_width = 10;
  rgb.timings.hsync_back_porch = 10;
  rgb.timings.hsync_front_porch = 20;
  rgb.timings.vsync_pulse_width = 10;
  rgb.timings.vsync_back_porch = 10;
  rgb.timings.vsync_front_porch = 10;
  rgb.timings.flags.pclk_active_neg = false;
  rgb.data_width = 16;
  rgb.bits_per_pixel = 16;
  rgb.num_fbs = DOUBLE_BUFFER ? 2 : 1;
  rgb.bounce_buffer_size_px = WIDTH * 20;
  rgb.hsync_gpio_num = PIN_HSYNC;
  rgb.vsync_gpio_num = PIN_VSYNC;
  rgb.de_gpio_num = PIN_DE;
  rgb.pclk_gpio_num = PIN_PCLK;
  rgb.disp_gpio_num = GPIO_NUM_NC;
  for (int i = 0; i < 16; i++) rgb.data_gpio_nums[i] = PIN_DATA[i];
  rgb.flags.fb_in_psram = 1;

  static st7701_lcd_init_cmd_t cmds[sizeof(INIT_SEQUENCE) / sizeof(INIT_SEQUENCE[0])];
  for (size_t i = 0; i < sizeof(INIT_SEQUENCE) / sizeof(INIT_SEQUENCE[0]); i++) {
    cmds[i] = {INIT_SEQUENCE[i].cmd, INIT_SEQUENCE[i].data, INIT_SEQUENCE[i].len, INIT_SEQUENCE[i].delay_ms};
  }
  st7701_vendor_config_t vendor = {};
  vendor.init_cmds = cmds;
  vendor.init_cmds_size = sizeof(cmds) / sizeof(cmds[0]);
  vendor.rgb_config = &rgb;
  vendor.flags.mirror_by_cmd = 0;  // esp_lcd_panel_mirror() acts on the RGB copy, see FLIP_X / FLIP_Y
  vendor.flags.auto_del_panel_io = 0;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = GPIO_NUM_NC;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 18;
  panel_config.vendor_config = &vendor;
  esp_lcd_panel_handle_t panel = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &panel_config, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, FLIP_X, FLIP_Y));

  // LVGL task + display. Partial buffers in internal RAM (fast rendering), copied by the CPU into the
  // PSRAM frame buffer: no DMA capability needed. 2 x 20 lines = 2 x 19 KB; 2 x 40 lines did not fit.
  lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  port.task_priority = 4;
  port.task_stack = 12 * 1024;
  port.task_affinity = 1;
  port.task_max_sleep_ms = 500;
  ESP_ERROR_CHECK(lvgl_port_init(&port));

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = panel;
  disp_cfg.buffer_size = DOUBLE_BUFFER ? WIDTH * HEIGHT : WIDTH * 20;
  disp_cfg.double_buffer = true;
  disp_cfg.flags.direct_mode = DOUBLE_BUFFER;
  disp_cfg.hres = WIDTH;
  disp_cfg.vres = HEIGHT;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  disp_cfg.rotation.mirror_x = FLIP_X;
  disp_cfg.rotation.mirror_y = FLIP_Y;
  lvgl_port_display_rgb_cfg_t rgb_cfg = {};
  rgb_cfg.flags.bb_mode = 1;
  rgb_cfg.flags.avoid_tearing = DOUBLE_BUFFER;
  lv_display_t *display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
  ESP_LOGI(TAG, "internal RAM after display: %u free, largest block %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (!display) {
    ESP_LOGE(TAG, "LVGL display creation failed (see the error above)");
    abort();
  }

  // GT911 on its own I2C bus. Its registers advertise 1024x600, but raw coordinates span 0..480.
  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = I2C_NUM_0;
  bus_config.sda_io_num = PIN_TOUCH_SDA;
  bus_config.scl_io_num = PIN_TOUCH_SCL;
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = 1;
  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

  // Same values as ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(), whose designated initializers do not compile in C++
  esp_lcd_panel_io_i2c_config_t touch_io_config = {};
  touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
  touch_io_config.scl_speed_hz = 400000;
  touch_io_config.control_phase_bytes = 1;
  touch_io_config.dc_bit_offset = 0;
  touch_io_config.lcd_cmd_bits = 16;
  touch_io_config.flags.disable_control_phase = 1;
  // Finite timeout: the default waits forever, and a stuck transfer would freeze the LVGL task
  touch_io_config.transaction_timeout_ms = 50;
  esp_lcd_panel_io_handle_t touch_io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &touch_io_config, &touch_io));

  esp_lcd_touch_config_t touch_config = {};
  touch_config.x_max = WIDTH;
  touch_config.y_max = HEIGHT;
  touch_config.rst_gpio_num = GPIO_NUM_NC;
  touch_config.int_gpio_num = GPIO_NUM_NC;
  touch_config.flags.mirror_x = TOUCH_MIRROR_X;
  touch_config.flags.mirror_y = TOUCH_MIRROR_Y;
  touch_config.process_coordinates = filter_touch;
  esp_lcd_touch_handle_t touch = nullptr;
  if (esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch) == ESP_OK) {
    lvgl_port_touch_cfg_t touch_cfg = {};
    touch_cfg.disp = display;
    touch_cfg.handle = touch;
    g_touch_handle = touch;
    g_touch = lvgl_port_add_touch(&touch_cfg);
    if (g_touch && lvgl_port_lock(0)) {
      lv_indev_set_read_cb(g_touch, touch_read);
      lvgl_port_unlock();
    }
  } else {
    ESP_LOGE(TAG, "GT911 not found, the screen will not react to touch");
  }

  // NVS writes of the wake-up trace need more stack than the sleep loop alone
  xTaskCreate(sleep_task, "dm_sleep", 4096, nullptr, 2, nullptr);
  ESP_LOGI(TAG, "display ready");
}

void display_configure(int brightness_pct, int sleep_minutes) {
  g_brightness = brightness_pct < 5 ? 5 : brightness_pct > 100 ? 100 : brightness_pct;
  g_sleep_minutes = sleep_minutes;
  if (g_shown && !g_asleep) backlight_set(g_brightness);
}

void display_show() {
  redraw_and_wait();
  g_shown = true;
  if (!g_asleep) backlight_fade_in(g_brightness);
}

const char *display_previous_state() { return g_previous_state; }

void display_wake() {
  // The sleep task redraws, then turns the backlight on (within half a second)
  if (g_asleep) g_wake_request = true;
}

}  // namespace dm::esp32
