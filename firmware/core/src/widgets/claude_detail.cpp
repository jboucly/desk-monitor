// Full-screen Claude detail views, opened from the Claude cards (see protocol/README.md, "Claude Code"):
// - session: one session's state, context, tokens, subagents and last tools;
// - usage: 30 days of tokens by model, today by hour, the busiest projects;
// - limits: the plan's 5-hour and weekly windows, their reset, the pace and a 7-day curve.
// The view tells the companion what it shows (claude_view), and the companion sends its data on
// pc/claude_detail while it stays open.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "claude_common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

using namespace claude;

constexpr int BODY_W = theme::SCREEN_W - 2 * 16;

/**
 * @description Fixed color of a model family, so that a model keeps its color from day to day.
 */
lv_color_t model_color(const char *name) {
  if (!strncmp(name, "Opus", 4)) return lv_color_hex(0xA78BFA);
  if (!strncmp(name, "Sonnet", 6)) return lv_color_hex(0x60A5FA);
  if (!strncmp(name, "Haiku", 5)) return lv_color_hex(0x34D399);
  if (!strncmp(name, "Fable", 5) || !strncmp(name, "Mythos", 6)) return lv_color_hex(0xF59E0B);
  return theme::text3();
}

/**
 * @description Name of the place a session runs in, from the snapshot's surface.
 */
const char *surface_name(const char *surface) {
  if (!strcmp(surface, "vscode")) return "VS Code";
  if (!strcmp(surface, "jetbrains")) return "JetBrains";
  if (!strcmp(surface, "desktop")) return "App Claude";
  if (!strcmp(surface, "cli")) return "Terminal";
  return "";
}

/**
 * @description Plain rectangle, used for bars, dots and chart lines.
 */
lv_obj_t *make_box(lv_obj_t *parent, int w, int h, lv_color_t color, int radius) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, h);
  lv_obj_set_style_bg_color(box, color, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, radius, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  return box;
}

/**
 * @description Label that keeps one line and takes the width left in its row.
 */
lv_obj_t *grow_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *label = theme::make_label(parent, font, color, text);
  lv_obj_set_flex_grow(label, 1);
  theme::single_line(label);
  return label;
}

/**
 * @description Section title: small icon and text, with an optional count on the right.
 */
void make_section(lv_obj_t *parent, const char *icon, const char *title, const std::string &right = "") {
  lv_obj_t *row = make_row(parent, 8);
  lv_obj_set_style_pad_top(row, 6, 0);
  theme::make_icon(row, &dm_icons_18, icon, theme::text3());
  grow_label(row, &dm_font_14, theme::text3(), title);
  if (!right.empty()) theme::make_label(row, &dm_font_14, theme::text3(), right.c_str());
}

/**
 * @description Value with its caption under it, for rows of figures.
 */
void make_figure(lv_obj_t *parent, const std::string &value, const char *caption, const std::string &extra = "") {
  lv_obj_t *col = make_column(parent, 0);
  lv_obj_set_width(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(col, 1);
  theme::make_label(col, &dm_font_20, theme::text(), value.c_str());
  if (!extra.empty()) theme::make_label(col, &dm_font_14, theme::text2(), extra.c_str());
  theme::make_label(col, &dm_font_14, theme::text3(), caption);
}

/**
 * @description "dans 2 h 14 min" until a Unix time, empty when it is unknown or past.
 */
std::string countdown(double unix_time) {
  const double left = -seconds_since(unix_time);
  if (std::isnan(left) || unix_time <= 0) return "";
  const time_t now = time(nullptr);
  if (unix_time <= static_cast<double>(now)) return "";
  return "dans " + util::format_elapsed(unix_time - static_cast<double>(now));
}

/**
 * @description "dd/mm" of the day `back` days before today.
 */
std::string day_label(int back) {
  const time_t when = time(nullptr) - static_cast<time_t>(back) * 86400;
  struct tm local;
  localtime_r(&when, &local);
  char out[24];
  snprintf(out, sizeof out, "%02d/%02d", local.tm_mday, local.tm_mon + 1);
  return out;
}

/**
 * @description One open detail view. Deleted when its sheet closes.
 */
class ClaudeDetail {
 public:
  ClaudeDetail(WidgetContext &ctx, std::string view, std::string id) : ctx_(ctx), view_(std::move(view)), id_(std::move(id)) {
    panel_ = ctx_.app.open_sheet(theme::SCREEN_W, theme::SCREEN_H, [this] { delete this; });
    // Full screen: no rounded panel, no border
    lv_obj_set_style_radius(panel_, 0, 0);
    lv_obj_set_style_border_width(panel_, 0, 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel_, 16, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel_, 10, 0);
    // A swipe down closes the view, like the close button
    lv_obj_add_event_cb(
        panel_,
        [](lv_event_t *e) {
          if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM) return;
          static_cast<ClaudeDetail *>(lv_event_get_user_data(e))->ctx_.app.close_sheet();
        },
        LV_EVENT_GESTURE, this);

    build_header();
    body_ = make_column(panel_, 10);
    lv_obj_set_height(body_, 0);
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_ACTIVE);

    if (!ctx_.services.pc_claude_view || !ctx_.services.pc_claude_view(view_, id_)) show_message("PC déconnecté");
    else show_message("Chargement…");
    token_ = ctx_.bus.subscribe("pc/claude_detail", [this](const cJSON *detail) { render(detail); });
    // Countdowns and ages move without new data
    timer_ = lv_timer_create(
        [](lv_timer_t *t) {
          auto *self = static_cast<ClaudeDetail *>(lv_timer_get_user_data(t));
          self->render(self->ctx_.bus.get("pc/claude_detail"));
        },
        15000, this);
  }

  ~ClaudeDetail() {
    lv_timer_delete(timer_);
    ctx_.bus.unsubscribe(token_);
    if (ctx_.services.pc_claude_view) ctx_.services.pc_claude_view("", "");
  }

 private:
  void build_header() {
    lv_obj_t *head = make_row(panel_, 10);
    icon_ = theme::make_icon(head, &dm_icons_24, view_ == "usage" ? "chart-bar" : view_ == "limits" ? "timer-sand" : "creation",
                             theme::accent());
    const char *title = view_ == "usage" ? "Tokens" : view_ == "limits" ? "Limites du forfait" : "Session";
    title_ = grow_label(head, &dm_font_20, theme::text(), title);

    lv_obj_t *close = lv_obj_create(head);
    lv_obj_remove_style_all(close);
    lv_obj_add_style(close, theme::bubble(), 0);
    lv_obj_set_size(close, 44, 44);
    lv_obj_set_style_bg_color(close, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, 60, LV_STATE_PRESSED);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close, 8);
    lv_obj_center(theme::make_icon(close, &dm_icons_24, "close", theme::text()));
    lv_obj_add_event_cb(
        close, [](lv_event_t *e) { static_cast<ClaudeDetail *>(lv_event_get_user_data(e))->ctx_.app.close_sheet(); },
        LV_EVENT_CLICKED, this);
  }

  void show_message(const char *text) {
    lv_obj_clean(body_);
    lv_obj_t *label = theme::make_label(body_, &dm_font_16, theme::text3(), text);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(label, 120, 0);
  }

  void render(const cJSON *detail) {
    if (strcmp(json::str(detail, "view"), view_.c_str()) != 0 || (view_ == "session" && id_ != json::str(detail, "id"))) {
      // The link dropped (empty value) or the data is for another view
      if (!cJSON_IsObject(detail) || !json::get(detail, "view")) show_message("PC déconnecté");
      return;
    }
    // Rebuilt at each update; the scroll position is kept
    const int32_t scroll = lv_obj_get_scroll_y(body_);
    lv_obj_clean(body_);
    lines_.clear();
    if (view_ == "session") render_session(detail);
    else if (view_ == "usage") render_usage(detail);
    else render_limits(detail);
    lv_obj_update_layout(body_);
    lv_obj_scroll_to_y(body_, scroll, LV_ANIM_OFF);
  }

  // ── session ────────────────────────────────────────────────────────────────

  void render_session(const cJSON *d) {
    const char *state = json::str(d, "state", "idle");
    const char *name = json::str(d, "name");
    lv_label_set_text(title_, *name ? name : json::str(d, "project", "Session"));
    lv_obj_set_style_text_color(icon_, state_color(state), 0);

    // Where it runs and with which model
    std::string where = json::str(d, "project");
    for (const char *part : {surface_name(json::str(d, "surface")), json::str(d, "model")}) {
      if (!*part) continue;
      if (!where.empty()) where += " · ";
      where += part;
    }
    lv_obj_t *where_label = theme::make_label(body_, &dm_font_14, theme::text2(), where.c_str());
    lv_obj_set_width(where_label, lv_pct(100));
    theme::single_line(where_label);

    // State, tool in use and time in this state
    lv_obj_t *state_row = make_row(body_, 8);
    make_box(state_row, 12, 12, state_color(state), LV_RADIUS_CIRCLE);
    std::string state_line = state_text(state);
    const char *tool = json::str(d, "tool");
    if (!strcmp(state, "working") && *tool) state_line += std::string(" · ") + tool;
    const double elapsed = seconds_since(json::num(d, "since", NAN));
    if (!std::isnan(elapsed) && elapsed >= 60) state_line += " · " + util::format_elapsed(elapsed);
    grow_label(state_row, &dm_font_16, state_color(state), state_line.c_str());

    // Context window
    const double context = json::num(d, "context", 0), context_max = json::num(d, "context_max", 200000);
    if (context > 0) {
      const double pct = context_max > 0 ? context / context_max * 100 : 0;
      lv_obj_t *head = make_row(body_, 8);
      lv_obj_set_style_pad_top(head, 4, 0);
      grow_label(head, &dm_font_14, theme::text2(), "Contexte");
      const std::string usage = format_tokens(context) + " / " + format_tokens(context_max) + " · " + util::format_number(pct, 0) + " %";
      theme::make_label(head, &dm_font_14, theme::text(), usage.c_str());
      lv_obj_t *track = make_box(body_, lv_pct(100), 8, theme::track(), 4);
      make_box(track, lv_pct(static_cast<int32_t>(std::clamp(pct, 2.0, 100.0))), 8, theme::level(pct, 70, 90, theme::accent()), 4);
    }

    // Tokens of the session
    const cJSON *tokens = json::get(d, "tokens");
    lv_obj_t *figures = make_row(body_, 8);
    lv_obj_set_style_pad_top(figures, 4, 0);
    make_figure(figures, format_tokens(json::num(tokens, "in", 0)), "Entrée");
    make_figure(figures, format_tokens(json::num(tokens, "out", 0)), "Sortie");
    make_figure(figures, format_tokens(json::num(tokens, "cache_read", 0)), "Cache lu");
    make_figure(figures, format_tokens(json::num(tokens, "cache_write", 0)), "Cache écrit");

    std::string total = "Total " + format_tokens(json::num(d, "total", 0));
    const std::string cost = format_cost(json::num(d, "cost", NAN));
    if (!cost.empty()) total += " · ~ " + cost;
    const double started = json::num(d, "started", NAN);
    if (started > 0) total += " · depuis " + format_reset(started);
    lv_obj_t *total_label = theme::make_label(body_, &dm_font_14, theme::text2(), total.c_str());
    lv_obj_set_width(total_label, lv_pct(100));
    theme::single_line(total_label);

    // Subagents, running ones first
    const cJSON *agents = json::get(d, "subagents");
    const int agent_count = cJSON_IsArray(agents) ? cJSON_GetArraySize(agents) : 0;
    if (agent_count > 0) {
      int running = 0;
      for (int i = 0; i < agent_count; i++) running += json::boolean(cJSON_GetArrayItem(agents, i), "running") ? 1 : 0;
      make_section(body_, "robot-outline", "Sous-agents", running > 0 ? std::to_string(running) + " en cours" : "terminés");
      for (int i = 0; i < agent_count; i++) {
        const cJSON *agent = cJSON_GetArrayItem(agents, i);
        const bool active = json::boolean(agent, "running");
        lv_obj_t *row = make_row(body_, 8);
        make_box(row, 8, 8, active ? theme::accent() : theme::text3(), LV_RADIUS_CIRCLE);
        lv_obj_t *col = make_column(row, 0);
        lv_obj_set_width(col, 0);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_t *type = theme::make_label(col, &dm_font_16, active ? theme::text() : theme::text2(), json::str(agent, "type", "Agent"));
        lv_obj_set_width(type, lv_pct(100));
        theme::single_line(type);
        const char *description = json::str(agent, "description");
        if (*description) {
          lv_obj_t *text = theme::make_label(col, &dm_font_14, theme::text3(), description);
          lv_obj_set_width(text, lv_pct(100));
          theme::single_line(text);
        }
        theme::make_label(row, &dm_font_14, theme::text3(), format_tokens(json::num(agent, "tokens", 0)).c_str());
      }
    }

    // Last tool calls, newest first
    const cJSON *tools = json::get(d, "tools");
    const int tool_count = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    make_section(body_, "history", "Derniers outils");
    if (tool_count == 0) {
      theme::make_label(body_, &dm_font_14, theme::text3(), "Aucun outil depuis l'ouverture du compagnon");
    }
    for (int i = 0; i < tool_count; i++) {
      const cJSON *call = cJSON_GetArrayItem(tools, i);
      const char *status = json::str(call, "status");
      const lv_color_t color = !strcmp(status, "ok")       ? theme::ok()
                               : !strcmp(status, "fail")   ? theme::crit()
                               : !strcmp(status, "denied") ? theme::warn()
                               : !strcmp(status, "stopped") ? theme::text3()
                                                            : theme::accent();
      lv_obj_t *row = make_row(body_, 8);
      make_box(row, 8, 8, color, LV_RADIUS_CIRCLE);
      theme::make_label(row, &dm_font_16, theme::text(), json::str(call, "name", "Outil"));
      grow_label(row, &dm_font_14, theme::text2(), json::str(call, "detail"));
      const double age = seconds_since(json::num(call, "at", NAN));
      const std::string when = std::isnan(age) ? "" : age < 60 ? "à l'instant" : util::format_elapsed(age);
      theme::make_label(row, &dm_font_14, theme::text3(), when.c_str());
    }
  }

  // ── usage ──────────────────────────────────────────────────────────────────

  void render_usage(const cJSON *d) {
    // Totals of today, 7 days and 30 days
    lv_obj_t *figures = make_row(body_, 8);
    const auto figure = [&](const char *key, const char *caption) {
      const cJSON *total = json::get(d, key);
      const std::string cost = format_cost(json::num(total, "cost", NAN));
      make_figure(figures, format_tokens(json::num(total, "tokens", 0)), caption, cost.empty() ? "" : "~ " + cost);
    };
    figure("today", "Aujourd'hui");
    figure("week", "7 jours");
    figure("month", "30 jours");

    // 30 days, one stacked bar per day, a color per model family
    const cJSON *models = json::get(d, "models"), *days = json::get(d, "days");
    const int model_count = cJSON_IsArray(models) ? cJSON_GetArraySize(models) : 0;
    const int day_count = cJSON_IsArray(days) ? std::min(cJSON_GetArraySize(days), 31) : 0;
    make_section(body_, "chart-bar", "30 derniers jours");
    double peak = 0;
    for (int i = 0; i < day_count; i++) peak = std::max(peak, json::num(cJSON_GetArrayItem(days, i), "tokens", 0));
    constexpr int CHART_H = 110;
    lv_obj_t *chart = make_row(body_, 2);
    lv_obj_set_height(chart, CHART_H);
    lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < day_count; i++) {
      const cJSON *day = cJSON_GetArrayItem(days, i);
      lv_obj_t *col = make_column(chart, 0);
      lv_obj_set_size(col, 0, CHART_H);
      lv_obj_set_flex_grow(col, 1);
      lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      const cJSON *split = json::get(day, "m");
      const int parts = cJSON_IsArray(split) ? cJSON_GetArraySize(split) : 0;
      // The first model is drawn at the bottom: children go top to bottom
      for (int m = parts - 1; m >= 0 && peak > 0; m--) {
        const double value = cJSON_GetArrayItem(split, m)->valuedouble;
        const int height = static_cast<int>(value / peak * CHART_H);
        if (height < 1) continue;
        const char *model = m < model_count ? cJSON_GetArrayItem(models, m)->valuestring : "";
        make_box(col, lv_pct(100), height, model_color(model ? model : ""), 1);
      }
      if (lv_obj_get_child_count(col) == 0) make_box(col, lv_pct(100), 2, theme::track(), 1);
    }
    lv_obj_t *axis = make_row(body_, 0);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    theme::make_label(axis, &dm_font_14, theme::text3(), day_label(std::max(0, day_count - 1)).c_str());
    theme::make_label(axis, &dm_font_14, theme::text3(), day_label(day_count / 2).c_str());
    theme::make_label(axis, &dm_font_14, theme::text3(), "Aujourd'hui");

    // Legend
    lv_obj_t *legend = make_row(body_, 14);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    for (int m = 0; m <= model_count; m++) {
      const char *model = m < model_count ? cJSON_GetArrayItem(models, m)->valuestring : "Autres";
      if (m == model_count && !has_other(days, model_count)) break;
      lv_obj_t *item = make_row(legend, 6);
      lv_obj_set_width(item, LV_SIZE_CONTENT);
      make_box(item, 10, 10, model_color(model ? model : ""), 2);
      theme::make_label(item, &dm_font_14, theme::text2(), model ? model : "");
    }

    // Today, hour by hour
    const cJSON *hours = json::get(d, "hours");
    if (cJSON_IsArray(hours) && cJSON_GetArraySize(hours) == 24) {
      make_section(body_, "clock-outline", "Aujourd'hui, par heure");
      double hour_peak = 0;
      for (int h = 0; h < 24; h++) hour_peak = std::max(hour_peak, cJSON_GetArrayItem(hours, h)->valuedouble);
      const time_t now = time(nullptr);
      struct tm local;
      localtime_r(&now, &local);
      constexpr int HOURS_H = 56;
      lv_obj_t *bars = make_row(body_, 3);
      lv_obj_set_height(bars, HOURS_H);
      lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
      for (int h = 0; h < 24; h++) {
        const double value = cJSON_GetArrayItem(hours, h)->valuedouble;
        const int height = hour_peak > 0 ? std::max(2, static_cast<int>(value / hour_peak * HOURS_H)) : 2;
        lv_obj_t *bar = make_box(bars, 0, height, h == local.tm_hour ? theme::accent() : value > 0 ? theme::text3() : theme::track(), 2);
        lv_obj_set_flex_grow(bar, 1);
      }
      lv_obj_t *hour_axis = make_row(body_, 0);
      lv_obj_set_flex_align(hour_axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      for (const char *label : {"0 h", "6 h", "12 h", "18 h", "23 h"}) theme::make_label(hour_axis, &dm_font_14, theme::text3(), label);
    }

    // Busiest projects
    const cJSON *projects = json::get(d, "projects");
    const int project_count = cJSON_IsArray(projects) ? cJSON_GetArraySize(projects) : 0;
    if (project_count > 0) {
      make_section(body_, "folder", "Projets, 30 jours");
      const double top = json::num(cJSON_GetArrayItem(projects, 0), "tokens", 0);
      for (int i = 0; i < project_count; i++) {
        const cJSON *project = cJSON_GetArrayItem(projects, i);
        const double tokens = json::num(project, "tokens", 0);
        lv_obj_t *row = make_row(body_, 8);
        grow_label(row, &dm_font_16, theme::text(), json::str(project, "name", "?"));
        std::string value = format_tokens(tokens);
        const std::string cost = format_cost(json::num(project, "cost", NAN));
        if (!cost.empty()) value += " · ~ " + cost;
        theme::make_label(row, &dm_font_14, theme::text2(), value.c_str());
        lv_obj_t *track = make_box(body_, lv_pct(100), 4, theme::track(), 2);
        make_box(track, lv_pct(top > 0 ? std::max(1, static_cast<int>(tokens / top * 100)) : 1), 4, theme::accent(), 2);
      }
    }
  }

  /**
   * @description True when a day has tokens of a model outside the named families.
   */
  static bool has_other(const cJSON *days, int model_count) {
    for (int i = 0; cJSON_IsArray(days) && i < cJSON_GetArraySize(days); i++) {
      const cJSON *split = json::get(cJSON_GetArrayItem(days, i), "m");
      const cJSON *other = cJSON_IsArray(split) ? cJSON_GetArrayItem(split, model_count) : nullptr;
      if (cJSON_IsNumber(other) && other->valuedouble > 0) return true;
    }
    return false;
  }

  // ── limits ─────────────────────────────────────────────────────────────────

  void render_limits(const cJSON *d) {
    lv_obj_t *meters = make_row(body_, 8);
    lv_obj_set_flex_align(meters, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    make_meter(meters, json::get(d, "five_hour"), "5 heures");
    make_meter(meters, json::get(d, "week"), "7 jours");

    // Weekly and 5-hour use over the last 7 days
    const cJSON *history = json::get(d, "history");
    const int count = cJSON_IsArray(history) ? cJSON_GetArraySize(history) : 0;
    make_section(body_, "chart-bar", "7 derniers jours");
    constexpr int CHART_H = 100;
    lv_obj_t *chart = lv_obj_create(body_);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, BODY_W, CHART_H);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    // Grid: 0, 50 and 100 %
    for (int pct : {0, 50, 100}) {
      lv_obj_t *line = make_box(chart, BODY_W, 1, theme::track(), 0);
      lv_obj_set_pos(line, 0, (CHART_H - 1) - pct * (CHART_H - 1) / 100);
    }
    const double end = static_cast<double>(time(nullptr)), start = end - 7 * 86400.0;
    std::vector<lv_point_precise_t> week, five;
    for (int i = 0; i < count; i++) {
      const cJSON *sample = cJSON_GetArrayItem(history, i);
      if (!cJSON_IsArray(sample) || cJSON_GetArraySize(sample) < 3) continue;
      const double t = cJSON_GetArrayItem(sample, 0)->valuedouble;
      if (t < start) continue;
      const lv_value_precise_t x = static_cast<lv_value_precise_t>((t - start) / (end - start) * (BODY_W - 1));
      const auto y = [&](int index) {
        const double pct = std::clamp(cJSON_GetArrayItem(sample, index)->valuedouble, 0.0, 100.0);
        return static_cast<lv_value_precise_t>((CHART_H - 2) - pct / 100 * (CHART_H - 3));
      };
      five.push_back({x, y(1)});
      week.push_back({x, y(2)});
    }
    const bool empty = week.size() < 2;
    add_line(chart, std::move(five), theme::text3(), 2);
    add_line(chart, std::move(week), theme::accent(), 3);
    if (empty) {
      lv_obj_t *none = theme::make_label(chart, &dm_font_14, theme::text3(), "Pas encore de relevés");
      lv_obj_center(none);
    }
    lv_obj_t *axis = make_row(body_, 0);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    theme::make_label(axis, &dm_font_14, theme::text3(), day_label(7).c_str());
    theme::make_label(axis, &dm_font_14, theme::text3(), day_label(4).c_str());
    theme::make_label(axis, &dm_font_14, theme::text3(), "Maintenant");
    lv_obj_t *legend = make_row(body_, 14);
    for (const auto &[name, color] : {std::pair<const char *, lv_color_t>{"7 jours", theme::accent()}, {"5 heures", theme::text3()}}) {
      lv_obj_t *item = make_row(legend, 6);
      lv_obj_set_width(item, LV_SIZE_CONTENT);
      make_box(item, 14, 3, color, 1);
      theme::make_label(item, &dm_font_14, theme::text2(), name);
    }

    // Where the reading comes from, and its age
    std::string source = !strcmp(json::str(d, "source"), "desktop") ? "Source : app Claude" : "Source : status line de Claude Code";
    const double age = seconds_since(json::num(d, "at", NAN));
    if (!std::isnan(age)) source += age < 60 ? " · à l'instant" : " · il y a " + util::format_elapsed(age);
    lv_obj_t *note = theme::make_label(body_, &dm_font_14, theme::text3(), source.c_str());
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  }

  /**
   * @description Line of the limits chart. The points must outlive the line: they stay in lines_.
   */
  void add_line(lv_obj_t *chart, std::vector<lv_point_precise_t> points, lv_color_t color, int width) {
    if (points.size() < 2) return;
    lines_.push_back(std::move(points));
    const auto &kept = lines_.back();
    lv_obj_t *line = lv_line_create(chart);
    lv_line_set_points(line, kept.data(), static_cast<uint32_t>(kept.size()));
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  }

  /**
   * @description Big arc of one window, with its reset and the pace.
   */
  void make_meter(lv_obj_t *parent, const cJSON *limit, const char *name) {
    constexpr int SIZE = 140;
    lv_obj_t *col = make_column(parent, 2);
    lv_obj_set_width(col, BODY_W / 2 - 8);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const double pct = json::num(limit, "pct", NAN);
    const bool known = !std::isnan(pct);
    lv_obj_t *arc = lv_arc_create(col);
    lv_obj_set_size(arc, SIZE, SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, known ? static_cast<int32_t>(std::clamp(pct, 0.0, 100.0) * 10) : 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::level(known ? pct : NAN, 75, 90, theme::accent()), LV_PART_INDICATOR);
    lv_obj_t *value = theme::make_label(arc, &dm_font_28, theme::text(), known ? (util::format_number(pct, 0) + " %").c_str() : "—");
    lv_obj_align(value, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *label = theme::make_label(arc, &dm_font_14, theme::text2(), name);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -4);

    // Reset: countdown and clock time ("vers" when the companion estimated it)
    const double reset = json::num(limit, "resets_at", NAN);
    const std::string left = countdown(reset);
    std::string when = left.empty() ? "Remise à zéro inconnue" : "Remise à zéro " + left;
    lv_obj_t *reset_label = theme::make_label(col, &dm_font_14, theme::text(), when.c_str());
    lv_obj_set_style_text_align(reset_label, LV_TEXT_ALIGN_CENTER, 0);
    if (!left.empty()) {
      const std::string at = (json::boolean(limit, "estimated") ? "vers " : "") + format_reset(reset);
      theme::make_label(col, &dm_font_14, theme::text3(), at.c_str());
    }

    // Pace: use compared with the share of the window already gone
    const double pace = json::num(limit, "pace", NAN);
    if (!std::isnan(pace)) {
      const int points = static_cast<int>(std::lround(std::fabs(pace)));
      std::string text = "Dans le rythme";
      lv_color_t color = theme::text2();
      if (pace > 5) {
        text = "Au-dessus du rythme (+" + std::to_string(points) + ")";
        color = theme::warn();
      } else if (pace < -5) {
        text = "Sous le rythme (-" + std::to_string(points) + ")";
        color = theme::ok();
      }
      lv_obj_t *pace_label = theme::make_label(col, &dm_font_14, color, text.c_str());
      lv_obj_set_style_text_align(pace_label, LV_TEXT_ALIGN_CENTER, 0);
    }
  }

  WidgetContext &ctx_;
  std::string view_, id_;
  lv_obj_t *panel_ = nullptr, *body_ = nullptr, *icon_ = nullptr, *title_ = nullptr;
  lv_timer_t *timer_ = nullptr;
  int token_ = 0;
  std::vector<std::vector<lv_point_precise_t>> lines_;
};

}  // namespace

void open_claude_detail(WidgetContext &ctx, const std::string &view, const std::string &id) {
  new ClaudeDetail(ctx, view, id);
}

}  // namespace dm::widgets
