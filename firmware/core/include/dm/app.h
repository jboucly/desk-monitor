// UI shell: wallpaper, header with status icons, swipeable pages built from the layout, popups and toasts.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dm/bus.h"
#include "dm/layout.h"
#include "dm/services.h"
#include "dm/widget.h"
#include "lvgl.h"

namespace dm {

/**
 * @description Owns the LVGL screen. Everything that touches LVGL runs in the LVGL thread;
 * other threads only use set_layout() and the bus.
 */
class App {
 public:
  /**
   * @param default_layout JSON used when storage holds no valid layout.json.
   */
  App(Bus &bus, Services &services, Storage &storage, std::string default_layout);

  /**
   * @description Builds the screen and starts the internal timers. LVGL thread only.
   */
  void start();

  /**
   * @description Validates a layout, saves it and schedules a rebuild. Thread-safe.
   * @param error Receives a French message when the layout is rejected.
   */
  bool set_layout(const std::string &text, std::string &error);

  /**
   * @description Current layout as JSON. Thread-safe.
   */
  std::string layout_json();

  /**
   * @description Home Assistant entities referenced by the current layout. Thread-safe.
   */
  std::set<std::string> ha_entities();

  /**
   * @description Called (from the LVGL thread) when the set of referenced HA entities changes.
   */
  std::function<void(const std::set<std::string> &)> on_entities_changed;

  /**
   * @description Opens a modal sheet on the top layer and returns its content panel. LVGL thread only.
   * @param on_close Called once when the sheet closes (tap outside, close button or close_sheet()).
   */
  lv_obj_t *open_sheet(int width, int height, std::function<void()> on_close = nullptr);

  /**
   * @description Closes the open sheet, if any. LVGL thread only.
   */
  void close_sheet();

  /**
   * @description Shows a short message at the bottom of the screen. LVGL thread only.
   */
  void toast(const std::string &text);

  /**
   * @description Shows the page at an index (used by the simulator's screenshot mode). LVGL thread only.
   */
  void show_page(int index, bool animate);

  /**
   * @description Times full-screen redraws of the current page (LVGL thread only): as is, without the
   * cards, then without the wallpaper either. Used to find where rendering time goes on the device.
   * @returns One line per case, in milliseconds per frame.
   */
  std::string benchmark(int frames);

  /**
   * @description Number of pages of the current layout. LVGL thread only.
   */
  int page_count() const;

 private:
  void build_shell();
  void apply_background();
  void build_pages();
  void build_header_status();
  void apply_pending_layout();
  void update_page_indicator();
  void update_setup_banner(const cJSON *wifi);
  void show_pairing_code(const char *code, const char *pc_name);
  void on_pointer(lv_event_t *e);
  void tick();

  Bus &bus_;
  Services &services_;
  Storage &storage_;
  std::string default_layout_;
  WidgetContext widget_ctx_;  // widgets keep a reference to it

  // Layout handover between threads
  std::mutex mutex_;
  std::unique_ptr<Layout> pending_;
  std::string current_json_;
  std::set<std::string> current_entities_;

  // LVGL thread state
  std::unique_ptr<Layout> layout_;
  std::vector<std::unique_ptr<Widget>> widgets_;
  lv_obj_t *wallpaper_ = nullptr;
  lv_obj_t *tileview_ = nullptr;
  lv_obj_t *title_ = nullptr;
  lv_obj_t *clock_ = nullptr;
  lv_obj_t *status_wifi_ = nullptr;
  lv_obj_t *status_ha_ = nullptr;
  lv_obj_t *status_pc_ = nullptr;
  lv_obj_t *dots_ = nullptr;
  lv_obj_t *sheet_ = nullptr;
  std::function<void()> sheet_on_close_;
  bool pairing_sheet_ = false;
  // Swipe being watched for the page loop (see on_pointer)
  int swipe_page_ = -1;
  lv_point_t swipe_start_ = {0, 0};
  lv_obj_t *toast_ = nullptr;
  lv_obj_t *setup_ = nullptr;
};

}  // namespace dm
