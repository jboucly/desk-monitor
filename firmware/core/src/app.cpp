#include "dm/app.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "dm/theme.h"

namespace dm {

namespace {

constexpr const char *LAYOUT_FILE = "layout.json";

// Grid templates of a page (4 equal columns and rows); LVGL keeps pointers to them
const int32_t GRID_COL_DSC[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
const int32_t GRID_ROW_DSC[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

/**
 * @description Card shown for a widget type this firmware does not know (layout from a newer editor).
 */
class Placeholder : public Widget {
 public:
  Placeholder(lv_obj_t *parent, const WidgetSpec &) {
    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    theme::make_icon(root, &dm_icons_24, "alert-circle-outline", theme::text3());
    theme::make_label(root, &dm_font_14, theme::text3(), "Non pris en charge");
  }
};

/**
 * @description Wi-Fi icon for a signal strength.
 */
const char *wifi_icon(double rssi) {
  if (rssi >= -55) return "wifi-strength-4";
  if (rssi >= -65) return "wifi-strength-3";
  if (rssi >= -75) return "wifi-strength-2";
  return "wifi-strength-1";
}

}  // namespace

App::App(Bus &bus, Services &services, Storage &storage, std::string default_layout)
    : bus_(bus),
      services_(services),
      storage_(storage),
      default_layout_(std::move(default_layout)),
      widget_ctx_{bus, services, *this} {}

void App::start() {
  // Stored layout first, the built-in default when it is missing or invalid
  std::string error;
  if (auto stored = storage_.read(LAYOUT_FILE)) layout_ = Layout::parse(*stored, error);
  if (!layout_) {
    if (!error.empty()) LV_LOG_WARN("layout.json rejected: %s", error.c_str());
    layout_ = Layout::parse(default_layout_, error);
  }
  if (!layout_) {
    LV_LOG_ERROR("default layout rejected: %s", error.c_str());
    layout_ = Layout::parse(R"({"version":1,"pages":[{"id":"home","title":"Accueil","widgets":[]}]})", error);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_json_ = layout_->source();
    current_entities_ = layout_->ha_entities();
  }

  theme::apply(layout_->theme);
  build_shell();
  build_pages();
  if (on_entities_changed) on_entities_changed(current_entities_);

  // Bus dispatch and layout swaps run in the LVGL thread through this timer
  lv_timer_create(
      [](lv_timer_t *t) {
        auto *app = static_cast<App *>(lv_timer_get_user_data(t));
        app->apply_pending_layout();
        app->bus_.dispatch();
      },
      30, this);
  lv_timer_create([](lv_timer_t *t) { static_cast<App *>(lv_timer_get_user_data(t))->tick(); }, 1000, this);
  tick();
}

bool App::set_layout(const std::string &text, std::string &error) {
  auto layout = Layout::parse(text, error);
  if (!layout) return false;
  if (!storage_.write(LAYOUT_FILE, layout->source())) {
    error = "impossible d'enregistrer le layout";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  current_json_ = layout->source();
  pending_ = std::move(layout);
  return true;
}

std::string App::layout_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_json_;
}

std::set<std::string> App::ha_entities() {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_entities_;
}

void App::apply_pending_layout() {
  std::unique_ptr<Layout> next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next.swap(pending_);
  }
  if (!next) return;

  // Keep the current page across rebuilds when it still exists
  lv_obj_t *active = tileview_ ? lv_tileview_get_tile_active(tileview_) : nullptr;
  const uint32_t page = active ? lv_obj_get_index(active) : 0;
  layout_ = std::move(next);
  theme::apply(layout_->theme);
  apply_background();
  lv_obj_report_style_change(nullptr);
  build_pages();
  show_page(static_cast<int>(page) < page_count() ? static_cast<int>(page) : 0, false);

  std::set<std::string> entities = layout_->ha_entities();
  bool changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    changed = entities != current_entities_;
    current_entities_ = entities;
  }
  if (changed && on_entities_changed) on_entities_changed(entities);
}

void App::apply_background() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, theme::background2(), 0);
  lv_obj_set_style_bg_grad_color(screen, theme::background(), 0);
  // The flat style has no wallpaper: the screen's own gradient shows instead
  if (theme::style() == theme::Style::Flat) {
    lv_obj_add_flag(wallpaper_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(wallpaper_, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(wallpaper_, theme::wallpaper());
  }
  lv_obj_invalidate(screen);
}

void App::build_shell() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  wallpaper_ = lv_image_create(screen);
  lv_obj_set_pos(wallpaper_, 0, 0);
  apply_background();

  // Header: page title on the left, status icons and time on the right
  lv_obj_t *header = lv_obj_create(screen);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, theme::SCREEN_W, theme::HEADER_H);
  lv_obj_set_style_pad_hor(header, 18, 0);
  lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  title_ = theme::make_label(header, &dm_font_20, theme::text());
  lv_obj_align(title_, LV_ALIGN_LEFT_MID, 0, 2);

  lv_obj_t *status = lv_obj_create(header);
  lv_obj_remove_style_all(status);
  lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 10, 0);
  lv_obj_align(status, LV_ALIGN_RIGHT_MID, 0, 2);
  status_pc_ = theme::make_icon(status, &dm_icons_18, "laptop", theme::text3());
  status_ha_ = theme::make_icon(status, &dm_icons_18, "home-assistant", theme::text3());
  status_wifi_ = theme::make_icon(status, &dm_icons_18, "wifi-off", theme::text3());
  clock_ = theme::make_label(status, &dm_font_16, theme::text(), "--:--");
  lv_obj_set_style_pad_left(clock_, 4, 0);

  // Pages: one horizontal tile per page
  tileview_ = lv_tileview_create(screen);
  lv_obj_remove_style_all(tileview_);
  lv_obj_set_pos(tileview_, 0, theme::HEADER_H);
  lv_obj_set_size(tileview_, theme::SCREEN_W, theme::SCREEN_H - theme::HEADER_H - theme::FOOTER_H);
  lv_obj_set_scrollbar_mode(tileview_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(
      tileview_,
      [](lv_event_t *e) {
        auto *app = static_cast<App *>(lv_event_get_user_data(e));
        app->update_page_indicator();
      },
      LV_EVENT_VALUE_CHANGED, this);
  // The pages loop: a swipe past the last page goes to the first one, and back. LVGL gestures cannot
  // be used (the tileview claims every horizontal drag, even at its ends), so the pointer itself is
  // watched: a press on a page and a release far to the side, on the same page, wraps around.
  lv_obj_remove_flag(tileview_, LV_OBJ_FLAG_SCROLL_ELASTIC);
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
    lv_indev_add_event_cb(indev, [](lv_event_t *e) { static_cast<App *>(lv_event_get_user_data(e))->on_pointer(e); },
                          LV_EVENT_ALL, this);
  }

  // Page indicator: dots, the active one stretched into a pill
  dots_ = lv_obj_create(screen);
  lv_obj_remove_style_all(dots_);
  lv_obj_set_size(dots_, theme::SCREEN_W, theme::FOOTER_H);
  lv_obj_align(dots_, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_flex_flow(dots_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dots_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(dots_, 6, 0);
  lv_obj_remove_flag(dots_, LV_OBJ_FLAG_CLICKABLE);
}

void App::build_pages() {
  // Popups and listeners point into the widgets: drop them first
  close_sheet();
  bus_.clear_subscriptions();
  widgets_.clear();
  lv_obj_clean(tileview_);
  lv_obj_clean(dots_);

  const int count = static_cast<int>(layout_->pages.size());
  for (int i = 0; i < count; i++) {
    const PageSpec &page = layout_->pages[i];
    lv_dir_t dir = LV_DIR_NONE;
    if (i > 0) dir = static_cast<lv_dir_t>(dir | LV_DIR_LEFT);
    if (i < count - 1) dir = static_cast<lv_dir_t>(dir | LV_DIR_RIGHT);
    lv_obj_t *tile = lv_tileview_add_tile(tileview_, i, 0, dir);
    lv_obj_set_style_pad_all(tile, theme::GRID_PAD, 0);
    lv_obj_set_style_pad_row(tile, theme::GRID_GAP, 0);
    lv_obj_set_style_pad_column(tile, theme::GRID_GAP, 0);
    lv_obj_set_layout(tile, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(tile, GRID_COL_DSC, GRID_ROW_DSC);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);

    for (const WidgetSpec &spec : page.widgets) {
      std::unique_ptr<Widget> widget;
      if (WidgetFactory factory = find_widget_factory(spec.type)) widget = factory(tile, spec, widget_ctx_);
      if (!widget) widget = std::make_unique<Placeholder>(tile, spec);
      lv_obj_set_grid_cell(widget->root, LV_GRID_ALIGN_STRETCH, spec.x, spec.w, LV_GRID_ALIGN_STRETCH, spec.y, spec.h);
      widgets_.push_back(std::move(widget));
    }

    lv_obj_t *dot = lv_obj_create(dots_);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_40, 0);
  }
  if (count < 2) lv_obj_add_flag(dots_, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(dots_, LV_OBJ_FLAG_HIDDEN);

  // The tileview has no active tile until one is set explicitly
  if (count > 0) lv_tileview_set_tile_by_index(tileview_, 0, 0, LV_ANIM_OFF);
  theme::finish_cards(tileview_);
  build_header_status();
  update_page_indicator();
}

void App::build_header_status() {
  bus_.subscribe("sys/wifi", [this](const cJSON *v) {
    const cJSON *rssi = json::get(v, "rssi");
    const bool online = cJSON_IsNumber(rssi);
    lv_label_set_text(status_wifi_, theme::icon(online ? wifi_icon(rssi->valuedouble) : "wifi-off"));
    lv_obj_set_style_text_color(status_wifi_, online ? theme::text2() : theme::crit(), 0);
    update_setup_banner(v);
  });
  bus_.subscribe("sys/ha", [this](const cJSON *v) {
    const std::string state = json::str(v, "state");
    lv_obj_set_style_text_color(status_ha_,
                                state == "ok"           ? theme::text2()
                                : state == "auth_error" ? theme::crit()
                                                        : theme::text3(),
                                0);
    if (state == "off") lv_obj_add_flag(status_ha_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(status_ha_, LV_OBJ_FLAG_HIDDEN);
  });
  bus_.subscribe("sys/pc", [this](const cJSON *v) {
    const bool connected = !strcmp(json::str(v, "state"), "connected");
    lv_obj_set_style_text_color(status_pc_, connected ? theme::accent() : theme::text3(), 0);
  });
  // Pairing code of a new PC companion; an empty object closes the sheet
  bus_.subscribe("sys/pc_pair", [this](const cJSON *v) {
    const char *code = json::str(v, "code");
    if (*code) show_pairing_code(code, json::str(v, "name"));
    else if (pairing_sheet_) close_sheet();
  });
  bus_.subscribe("sys/toast", [this](const cJSON *v) { toast(json::str(v, "text")); });
}

void App::show_pairing_code(const char *code, const char *pc_name) {
  lv_obj_t *panel = open_sheet(380, 250, [this] { pairing_sheet_ = false; });
  pairing_sheet_ = true;
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(panel, 6, 0);

  theme::make_icon(panel, &dm_icons_32, "laptop", theme::accent());
  theme::make_label(panel, &dm_font_20, theme::text(), "Appairage du PC");
  lv_obj_t *who = theme::make_label(panel, &dm_font_14, theme::text2(), *pc_name ? pc_name : "PC");
  lv_obj_set_width(who, lv_pct(100));
  lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
  theme::single_line(who);
  lv_obj_t *digits = theme::make_label(panel, &dm_font_72, theme::text(), code);
  lv_obj_set_style_text_letter_space(digits, 4, 0);
  theme::make_label(panel, &dm_font_14, theme::text2(), "Saisis ce code dans l'app Desk Monitor");
}

void App::update_setup_banner(const cJSON *wifi) {
  const char *ap = json::str(wifi, "ap");
  const bool online = cJSON_IsNumber(json::get(wifi, "rssi"));
  if (!*ap || online) {
    if (setup_) lv_obj_add_flag(setup_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // Setup mode: explain how to reach the editor through the screen's own access point
  if (!setup_) {
    setup_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(setup_);
    lv_obj_set_size(setup_, 400, LV_SIZE_CONTENT);
    lv_obj_center(setup_);
    lv_obj_set_style_radius(setup_, 28, 0);
    lv_obj_set_style_bg_color(setup_, theme::panel(), 0);
    lv_obj_set_style_bg_opa(setup_, theme::panel_opa(), 0);
    lv_obj_set_style_border_color(setup_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(setup_, 40, 0);
    lv_obj_set_style_border_width(setup_, 1, 0);
    lv_obj_set_style_pad_all(setup_, 24, 0);
    lv_obj_set_style_pad_row(setup_, 10, 0);
    lv_obj_set_flex_flow(setup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(setup_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    theme::make_icon(setup_, &dm_icons_48, "wifi", theme::accent());
    theme::make_label(setup_, &dm_font_28, theme::text(), "Configuration");
  } else {
    lv_obj_remove_flag(setup_, LV_OBJ_FLAG_HIDDEN);
    while (lv_obj_get_child_count(setup_) > 2) lv_obj_delete(lv_obj_get_child(setup_, 2));
  }
  const std::string step1 = std::string("1. Connecte-toi au Wi-Fi « ") + ap + " »";
  const std::string step2 = std::string("2. Ouvre http://") + json::str(wifi, "ap_ip", "192.168.4.1");
  for (const std::string &line : {step1, step2}) {
    lv_obj_t *label = theme::make_label(setup_, &dm_font_16, theme::text2(), line.c_str());
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }
}

void App::update_page_indicator() {
  lv_obj_t *active = lv_tileview_get_tile_active(tileview_);
  const int index = active ? static_cast<int>(lv_obj_get_index(active)) : 0;
  if (index < page_count()) lv_label_set_text(title_, layout_->pages[index].title.c_str());
  for (uint32_t i = 0; i < lv_obj_get_child_count(dots_); i++) {
    lv_obj_t *dot = lv_obj_get_child(dots_, i);
    const bool on = static_cast<int>(i) == index;
    lv_obj_set_width(dot, on ? 18 : 6);
    lv_obj_set_style_bg_color(dot, on ? theme::accent() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

void App::on_pointer(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;
  auto *indev = static_cast<lv_indev_t *>(lv_event_get_current_target(e));
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_obj_t *active = tileview_ ? lv_tileview_get_tile_active(tileview_) : nullptr;
  const int index = active ? static_cast<int>(lv_obj_get_index(active)) : -1;

  if (code == LV_EVENT_PRESSED) {
    // Only presses on a page count (not on a popup, which lives on the top layer)
    auto *target = static_cast<lv_obj_t *>(lv_event_get_param(e));
    swipe_page_ = -1;
    for (lv_obj_t *obj = target; obj; obj = lv_obj_get_parent(obj)) {
      if (obj == tileview_) {
        swipe_page_ = index;
        swipe_start_ = point;
        break;
      }
    }
    return;
  }

  const int count = page_count();
  if (swipe_page_ < 0 || index != swipe_page_ || count < 2) return;
  const int dx = point.x - swipe_start_.x, dy = point.y - swipe_start_.y;
  swipe_page_ = -1;
  if (std::abs(dx) < 80 || std::abs(dy) > std::abs(dx)) return;
  if (dx < 0 && index == count - 1) show_page(0, true);
  else if (dx > 0 && index == 0) show_page(count - 1, true);
}

void App::show_page(int index, bool animate) {
  if (index < 0 || index >= page_count()) return;
  lv_tileview_set_tile_by_index(tileview_, index, 0, animate ? LV_ANIM_ON : LV_ANIM_OFF);
  update_page_indicator();
}

int App::page_count() const { return layout_ ? static_cast<int>(layout_->pages.size()) : 0; }

lv_obj_t *App::open_sheet(int width, int height, std::function<void()> on_close) {
  close_sheet();
  sheet_on_close_ = std::move(on_close);

  // Dimmed backdrop: a tap outside the panel closes the sheet
  sheet_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sheet_);
  lv_obj_set_size(sheet_, theme::SCREEN_W, theme::SCREEN_H);
  lv_obj_set_style_bg_color(sheet_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(sheet_, theme::backdrop_opa(), 0);
  lv_obj_add_flag(sheet_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      sheet_,
      [](lv_event_t *e) {
        auto *app = static_cast<App *>(lv_event_get_user_data(e));
        if (lv_event_get_target(e) == lv_event_get_current_target(e)) app->close_sheet();
      },
      LV_EVENT_CLICKED, this);

  lv_obj_t *panel = lv_obj_create(sheet_);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, width, height);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 28, 0);
  lv_obj_set_style_bg_color(panel, theme::panel(), 0);
  lv_obj_set_style_bg_opa(panel, theme::panel_opa(), 0);
  lv_obj_set_style_border_color(panel, lv_color_white(), 0);
  // Without a dimmed backdrop, a brighter border sets the panel apart
  lv_obj_set_style_border_opa(panel, theme::style() == theme::Style::Glass ? 40 : 90, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_pad_all(panel, 18, 0);
  lv_obj_set_style_text_color(panel, theme::text(), 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
  return panel;
}

void App::close_sheet() {
  if (!sheet_) return;
  // Deleted asynchronously: this may run from an event of an object inside the sheet
  lv_obj_delete_async(sheet_);
  sheet_ = nullptr;
  auto on_close = std::move(sheet_on_close_);
  sheet_on_close_ = nullptr;
  if (on_close) on_close();
}

void App::toast(const std::string &text) {
  static lv_timer_t *hide_timer = nullptr;
  if (!toast_) {
    toast_ = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(toast_, &dm_font_16, 0);
    lv_obj_set_style_text_color(toast_, theme::text(), 0);
    lv_obj_set_style_bg_color(toast_, theme::panel(), 0);
    lv_obj_set_style_bg_opa(toast_, theme::panel_opa(), 0);
    lv_obj_set_style_border_color(toast_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(toast_, 50, 0);
    lv_obj_set_style_border_width(toast_, 1, 0);
    lv_obj_set_style_radius(toast_, 20, 0);
    // Long messages (e.g. an error from the PC) wrap on two lines instead of overflowing
    lv_obj_set_width(toast_, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(toast_, 420, 0);
    lv_label_set_long_mode(toast_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(toast_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(toast_, 18, 0);
    lv_obj_set_style_pad_ver(toast_, 10, 0);
    lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -30);
  }
  lv_label_set_text(toast_, text.c_str());
  lv_obj_remove_flag(toast_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toast_);
  if (!hide_timer) {
    hide_timer = lv_timer_create(
        [](lv_timer_t *t) {
          lv_obj_add_flag(static_cast<lv_obj_t *>(lv_timer_get_user_data(t)), LV_OBJ_FLAG_HIDDEN);
          lv_timer_pause(t);
        },
        2500, toast_);
  }
  lv_timer_set_period(hide_timer, text.size() > 40 ? 4500 : 2500);
  lv_timer_reset(hide_timer);
  lv_timer_resume(hide_timer);
}

void App::tick() {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  // Before SNTP sync the clock reads 1970: show dashes instead
  if (local.tm_year + 1900 >= 2024) lv_label_set_text_fmt(clock_, "%02d:%02d", local.tm_hour, local.tm_min);
  else lv_label_set_text(clock_, "--:--");
  for (auto &widget : widgets_) widget->tick();
}

std::string App::benchmark(int frames) {
  frames = std::max(1, frames);
  lv_obj_t *page = lv_tileview_get_tile_active(tileview_);
  const auto time_frames = [&] {
    // Wall clock: the simulator's LVGL tick is simulated and does not move here
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; i++) {
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(nullptr);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
  };
  char out[160];
  const double full = time_frames();
  if (page) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  const double no_cards = time_frames();
  const bool wallpaper_shown = !lv_obj_has_flag(wallpaper_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wallpaper_, LV_OBJ_FLAG_HIDDEN);
  const double bare = time_frames();
  if (wallpaper_shown) lv_obj_remove_flag(wallpaper_, LV_OBJ_FLAG_HIDDEN);
  if (page) lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(lv_screen_active());
  snprintf(out, sizeof out, "full %.1f ms, without cards %.1f ms, without cards and wallpaper %.1f ms", full, no_cards,
           bare);
  return out;
}

}  // namespace dm
