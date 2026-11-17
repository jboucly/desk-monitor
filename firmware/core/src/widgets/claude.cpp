// Claude Code widgets, fed by the PC companion (bus key pc/claude, see protocol/README.md "Claude Code"):
// - claude_sessions: running sessions with their state; a tap shows the session's detail, or its
//   approval when it waits for one
// - claude_usage: tokens and estimated cost of today and of the week, with a 7-day histogram
// - claude_limits: the plan's 5-hour and weekly limits as two arcs
// A tap on the usage or limits card opens its detail view (claude_detail.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "claude_common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace claude {

/**
 * @description Token count in short French form: "850", "12 k", "1,2 M".
 */
std::string format_tokens(double tokens) {
  if (std::isnan(tokens) || tokens < 0) return "—";
  if (tokens < 1000) return util::format_number(tokens, 0);
  if (tokens < 1e6) return util::format_number(tokens / 1000, tokens < 1e4 ? 1 : 0) + " k";
  if (tokens < 1e9) return util::format_number(tokens / 1e6, tokens < 1e8 ? 1 : 0) + " M";
  return util::format_number(tokens / 1e9, 1) + " G";
}

/**
 * @description Estimated cost in US dollars, French style: "3,42 $", "128 $".
 */
std::string format_cost(double dollars) {
  if (std::isnan(dollars) || dollars < 0) return "";
  return util::format_number(dollars, dollars < 100 ? 2 : 0) + " $";
}

/**
 * @description Seconds elapsed since a Unix time, NaN when the time is missing or the clock is not set.
 */
double seconds_since(double unix_time) {
  const time_t now = time(nullptr);
  if (std::isnan(unix_time) || unix_time <= 0 || now < 1700000000) return NAN;
  return std::max(0.0, static_cast<double>(now) - unix_time);
}

/**
 * @description Clock time of a reset: "14:30" today, "lun. 09:00" another day.
 */
std::string format_reset(double unix_time) {
  if (std::isnan(unix_time) || unix_time <= 0) return "";
  const time_t when = static_cast<time_t>(unix_time), now = time(nullptr);
  struct tm at, today;
  localtime_r(&when, &at);
  localtime_r(&now, &today);
  char out[32];
  if (at.tm_yday == today.tm_yday && at.tm_year == today.tm_year)
    snprintf(out, sizeof out, "%02d:%02d", at.tm_hour, at.tm_min);
  else snprintf(out, sizeof out, "%s %02d:%02d", util::day_name(at.tm_wday, true), at.tm_hour, at.tm_min);
  return out;
}

void set_hidden(lv_obj_t *obj, bool hidden) {
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @description Transparent flex container, full width, sized to its content.
 */
lv_obj_t *make_row(lv_obj_t *parent, int gap) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_add_style(row, theme::bare(), 0);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, gap, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

/**
 * @description Card header: icon and title, the rest of the row left to the caller.
 */
lv_obj_t *make_header(lv_obj_t *parent, const char *icon, const char *title, lv_obj_t **icon_out) {
  lv_obj_t *header = make_row(parent, 8);
  lv_obj_t *glyph = theme::make_icon(header, &dm_icons_18, icon, theme::text3());
  lv_obj_t *label = theme::make_label(header, &dm_font_16, theme::text(), title);
  lv_obj_set_flex_grow(label, 1);
  theme::single_line(label);
  if (icon_out) *icon_out = glyph;
  return header;
}

lv_obj_t *make_column(lv_obj_t *parent, int gap) {
  lv_obj_t *col = make_row(parent, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, gap, 0);
  return col;
}

lv_color_t state_color(const char *state) {
  if (!strcmp(state, "waiting")) return theme::warn();
  if (!strcmp(state, "working")) return theme::accent();
  return theme::text3();
}

const char *state_text(const char *state) {
  if (!strcmp(state, "waiting")) return "Attend ton accord";
  if (!strcmp(state, "working")) return "Au travail";
  return "Inactive";
}

void pass_clicks(lv_obj_t *card) {
  lv_obj_tree_walk(
      card,
      [](lv_obj_t *obj, void *root) {
        if (obj != root) lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        return LV_OBJ_TREE_WALK_NEXT;
      },
      card);
}

}  // namespace claude

namespace {

using namespace claude;

/**
 * @description True when the PC is connected and sent a Claude snapshot.
 */
bool has_snapshot(const cJSON *claude) { return cJSON_IsArray(json::get(claude, "sessions")); }

// ── claude_sessions ──────────────────────────────────────────────────────────

constexpr int ROW_H = 44;
constexpr int ROW_GAP = 0;

class ClaudeSessions : public Widget {
 public:
  ClaudeSessions(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx) {
    root = theme::make_card(parent);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, ROW_GAP, 0);

    lv_obj_t *header = make_header(root, "creation", json::str(spec.props, "name", "Claude Code"), &icon_);
    lv_obj_set_style_pad_bottom(header, 2, 0);
    desktop_ = theme::make_icon(header, &dm_icons_18, "monitor", theme::text3());
    count_ = theme::make_label(header, &dm_font_14, theme::text2());

    empty_ = theme::make_label(root, &dm_font_14, theme::text3(), "PC déconnecté");
    lv_obj_set_width(empty_, lv_pct(100));
    lv_label_set_long_mode(empty_, LV_LABEL_LONG_WRAP);

    // As many rows as the card height allows
    const int inner = theme::span_height(spec.h) - 2 * theme::CARD_PAD - 22;
    rows_count_ = std::clamp(inner / (ROW_H + ROW_GAP), 1, MAX_ROWS);
    for (int i = 0; i < rows_count_; i++) rows_[i] = make_session_row();

    ctx.bus.subscribe("pc/claude", [this](const cJSON *) { render(); });
  }

  void tick() override { render(); }

 private:
  static constexpr int MAX_ROWS = 6;

  struct Row {
    lv_obj_t *box, *dot, *name, *status, *context;
    std::string id;
  };

  Row make_session_row() {
    Row row;
    row.box = make_row(root, 10);
    lv_obj_set_height(row.box, ROW_H);
    lv_obj_set_style_radius(row.box, 14, 0);
    lv_obj_set_style_pad_hor(row.box, 8, 0);
    lv_obj_set_style_bg_color(row.box, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row.box, 30, LV_STATE_PRESSED);
    bind_event<ClaudeSessions, &ClaudeSessions::on_row_click>(row.box, LV_EVENT_CLICKED, this);

    row.dot = lv_obj_create(row.box);
    lv_obj_remove_style_all(row.dot);
    lv_obj_set_size(row.dot, 10, 10);
    lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(row.dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *col = lv_obj_create(row.box);
    lv_obj_add_style(col, theme::bare(), 0);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    row.name = theme::make_label(col, &dm_font_16, theme::text());
    lv_obj_set_width(row.name, lv_pct(100));
    theme::single_line(row.name);
    row.status = theme::make_label(col, &dm_font_14, theme::text2());
    lv_obj_set_width(row.status, lv_pct(100));
    theme::single_line(row.status);

    row.context = theme::make_label(row.box, &dm_font_14, theme::text3());
    return row;
  }

  void on_row_click(lv_event_t *e) {
    auto *box = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    // A waiting session shows its approval, the others their detail
    if (lv_obj_has_state(box, LV_STATE_USER_1)) {
      ctx_.app.show_claude_asks();
      return;
    }
    for (int i = 0; i < rows_count_; i++) {
      if (rows_[i].box == box && !rows_[i].id.empty()) ctx_.app.show_claude_detail("session", rows_[i].id);
    }
  }

  void render() {
    const cJSON *claude = ctx_.bus.get("pc/claude");
    const bool online = has_snapshot(claude);
    const cJSON *sessions = json::get(claude, "sessions");
    const int total = online ? cJSON_GetArraySize(sessions) : 0;

    lv_obj_set_style_text_color(icon_, online ? theme::accent() : theme::text3(), 0);
    set_hidden(desktop_, !json::boolean(claude, "desktop", false));
    lv_label_set_text(count_, total > 0 ? std::to_string(total).c_str() : "");
    set_hidden(empty_, total > 0);
    lv_label_set_text(empty_, online ? "Aucune session en cours" : "PC déconnecté");

    for (int i = 0; i < rows_count_; i++) {
      Row &row = rows_[i];
      const cJSON *session = i < total ? cJSON_GetArrayItem(sessions, i) : nullptr;
      set_hidden(row.box, !session);
      if (!session) continue;
      fill_row(row, session);
    }
  }

  void fill_row(Row &row, const cJSON *session) {
    row.id = json::str(session, "id");
    const std::string state = json::str(session, "state", "idle");
    const bool waiting = state == "waiting", working = state == "working";
    lv_obj_set_style_bg_color(row.dot, waiting ? theme::warn() : working ? theme::accent() : theme::text3(), 0);
    if (waiting) lv_obj_add_state(row.box, LV_STATE_USER_1);
    else lv_obj_remove_state(row.box, LV_STATE_USER_1);

    const char *name = json::str(session, "name");
    lv_label_set_text(row.name, *name ? name : json::str(session, "project", "Session"));

    // Second line: what the session does, and for how long
    std::string status;
    const double elapsed = seconds_since(json::num(session, "since", NAN));
    const std::string age = std::isnan(elapsed) || elapsed < 60 ? "" : " · " + util::format_elapsed(elapsed);
    if (waiting) {
      status = "Attend ton accord" + age;
    } else if (working) {
      const char *tool = json::str(session, "tool");
      status = (*tool ? std::string(tool) : std::string("Réfléchit")) + age;
    } else {
      status = std::string("Inactive") + age;
    }
    const int agents = static_cast<int>(json::num(session, "subagents", 0));
    if (agents > 0) status += " · " + std::to_string(agents) + (agents > 1 ? " agents" : " agent");
    const char *project = json::str(session, "project");
    if (*project && strcmp(project, name) != 0) status = std::string(project) + " · " + status;
    lv_label_set_text(row.status, status.c_str());
    lv_obj_set_style_text_color(row.status, waiting ? theme::warn() : theme::text2(), 0);

    const double context = json::num(session, "context", NAN);
    lv_label_set_text(row.context, context > 0 ? format_tokens(context).c_str() : "");
  }

  WidgetContext &ctx_;
  lv_obj_t *icon_ = nullptr, *desktop_ = nullptr, *count_ = nullptr, *empty_ = nullptr;
  Row rows_[MAX_ROWS] = {};
  int rows_count_ = 0;
};

// ── claude_usage ─────────────────────────────────────────────────────────────

constexpr int DAYS = 7;

class ClaudeUsage : public Widget {
 public:
  ClaudeUsage(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx) {
    root = theme::make_card(parent);
    bind_event<ClaudeUsage, &ClaudeUsage::on_click>(root, LV_EVENT_CLICKED, this);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, 2, 0);

    make_header(root, "chart-bar", json::str(spec.props, "name", "Tokens"), &icon_);

    lv_obj_t *today = make_row(root, 8);
    lv_obj_set_flex_align(today, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    today_ = theme::make_label(today, &dm_font_28, theme::text(), "—");
    today_cost_ = theme::make_label(today, &dm_font_14, theme::text2());
    lv_obj_set_style_pad_bottom(today_cost_, 4, 0);

    // 7-day histogram, hidden on one-row cards
    bars_box_ = make_row(root, 6);
    lv_obj_set_flex_align(bars_box_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_flex_grow(bars_box_, 1);
    for (int i = 0; i < DAYS; i++) {
      lv_obj_t *col = lv_obj_create(bars_box_);
      lv_obj_add_style(col, theme::bare(), 0);
      lv_obj_set_height(col, lv_pct(100));
      lv_obj_set_flex_grow(col, 1);
      lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(col, 2, 0);
      lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
      bars_[i] = lv_obj_create(col);
      lv_obj_remove_style_all(bars_[i]);
      lv_obj_set_size(bars_[i], lv_pct(70), 2);
      lv_obj_set_style_radius(bars_[i], 3, 0);
      lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
      day_labels_[i] = theme::make_label(col, &dm_font_14, theme::text3());
    }
    set_hidden(bars_box_, spec.h < 2);

    week_ = theme::make_label(root, &dm_font_14, theme::text2());
    lv_obj_set_width(week_, lv_pct(100));
    theme::single_line(week_);
    pass_clicks(root);

    ctx.bus.subscribe("pc/claude", [this](const cJSON *claude) { render(claude); });
  }

 private:
  void on_click(lv_event_t *) {
    if (has_snapshot(ctx_.bus.get("pc/claude"))) ctx_.app.show_claude_detail("usage");
  }

  void render(const cJSON *claude) {
    const bool online = has_snapshot(claude);
    lv_obj_set_style_text_color(icon_, online ? theme::accent() : theme::text3(), 0);
    const cJSON *today = json::get(claude, "today"), *week = json::get(claude, "week");
    lv_label_set_text(today_, online ? format_tokens(json::num(today, "tokens", NAN)).c_str() : "—");
    const std::string cost = format_cost(json::num(today, "cost", NAN));
    lv_label_set_text(today_cost_, online ? (cost.empty() ? "aujourd'hui" : "~ " + cost).c_str() : "PC déconnecté");

    std::string week_text = "Semaine : " + format_tokens(json::num(week, "tokens", NAN));
    const std::string week_cost = format_cost(json::num(week, "cost", NAN));
    if (!week_cost.empty()) week_text += " · ~ " + week_cost;
    lv_label_set_text(week_, online ? week_text.c_str() : "");

    // Bars scaled to the busiest day; the last one is today
    const cJSON *days = json::get(claude, "days");
    double values[DAYS] = {}, peak = 0;
    for (int i = 0; i < DAYS && cJSON_IsArray(days); i++) {
      const cJSON *item = cJSON_GetArrayItem(days, cJSON_GetArraySize(days) - DAYS + i);
      values[i] = cJSON_IsNumber(item) ? std::max(0.0, item->valuedouble) : 0;
      peak = std::max(peak, values[i]);
    }
    const time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    lv_obj_update_layout(bars_box_);
    const int max_h = std::max(8, static_cast<int>(lv_obj_get_content_height(bars_box_)) - 20);
    for (int i = 0; i < DAYS; i++) {
      const int height = peak > 0 ? std::max(3, static_cast<int>(values[i] / peak * max_h)) : 3;
      lv_obj_set_height(bars_[i], height);
      lv_obj_set_style_bg_color(bars_[i], i == DAYS - 1 ? theme::accent() : theme::track(), 0);
      // First letter of the day name, upper case
      const char *day = util::day_name((local.tm_wday + i + 1) % 7, true);
      char letter[2] = {static_cast<char>(day[0] - 'a' + 'A'), 0};
      lv_label_set_text(day_labels_[i], letter);
    }
  }

  WidgetContext &ctx_;
  lv_obj_t *icon_ = nullptr, *today_ = nullptr, *today_cost_ = nullptr, *week_ = nullptr, *bars_box_ = nullptr;
  lv_obj_t *bars_[DAYS] = {}, *day_labels_[DAYS] = {};
};

// ── claude_limits ────────────────────────────────────────────────────────────

class ClaudeLimits : public Widget {
 public:
  ClaudeLimits(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) : ctx_(ctx), bus_(ctx.bus) {
    root = theme::make_card(parent);
    bind_event<ClaudeLimits, &ClaudeLimits::on_click>(root, LV_EVENT_CLICKED, this);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 2, 0);

    make_header(root, "timer-sand", json::str(spec.props, "name", "Limites"), &icon_);

    // Two arcs side by side, sized to the card
    lv_obj_t *arcs = make_row(root, 6);
    lv_obj_set_flex_align(arcs, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int inner_w = (theme::span_width(spec.w) - 2 * theme::CARD_PAD - 6) / 2;
    const int inner_h = theme::span_height(spec.h) - 2 * theme::CARD_PAD - 22 - 36;
    const int size = std::max(56, std::min(inner_w, inner_h));
    meters_[0] = make_meter(arcs, size, "5 h");
    meters_[1] = make_meter(arcs, size, "7 j");

    note_ = theme::make_label(root, &dm_font_14, theme::text3());
    lv_obj_set_width(note_, lv_pct(100));
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    theme::single_line(note_);
    pass_clicks(root);

    ctx.bus.subscribe("pc/claude", [this](const cJSON *claude) { render(claude); });
  }

  void tick() override {
    // The age of the reading moves even when nothing new comes
    if (++ticks_ % 30 == 0) render(bus_.get("pc/claude"));
  }

 private:
  void on_click(lv_event_t *) {
    if (has_snapshot(bus_.get("pc/claude"))) ctx_.app.show_claude_detail("limits");
  }

  struct Meter {
    lv_obj_t *arc, *value, *reset;
  };

  Meter make_meter(lv_obj_t *parent, int size, const char *name) {
    Meter m;
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_add_style(col, theme::bare(), 0);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    m.arc = lv_arc_create(col);
    lv_obj_set_size(m.arc, size, size);
    lv_arc_set_rotation(m.arc, 135);
    lv_arc_set_bg_angles(m.arc, 0, 270);
    lv_arc_set_range(m.arc, 0, 1000);
    lv_arc_set_value(m.arc, 0);
    lv_obj_remove_style(m.arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(m.arc, LV_OBJ_FLAG_CLICKABLE);
    const int width = size < 80 ? 6 : 8;
    lv_obj_set_style_arc_width(m.arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(m.arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m.arc, 40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(m.arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(m.arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(m.arc, true, LV_PART_MAIN);

    m.value = theme::make_label(m.arc, size < 80 ? &dm_font_16 : &dm_font_20, theme::text(), "—");
    lv_obj_align(m.value, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *label = theme::make_label(m.arc, &dm_font_14, theme::text2(), name);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);

    m.reset = theme::make_label(col, &dm_font_14, theme::text3());
    return m;
  }

  void show(Meter &m, const cJSON *limit) {
    const double pct = json::num(limit, "pct", NAN);
    const bool valid = !std::isnan(pct);
    lv_label_set_text(m.value, valid ? (util::format_number(pct, 0) + " %").c_str() : "—");
    lv_arc_set_value(m.arc, valid ? static_cast<int32_t>(std::clamp(pct, 0.0, 100.0) * 10) : 0);
    lv_obj_set_style_arc_color(m.arc, theme::level(valid ? pct : NAN, 75, 90, theme::accent()), LV_PART_INDICATOR);
    const std::string reset = format_reset(json::num(limit, "resets_at", NAN));
    lv_label_set_text(m.reset, reset.c_str());
  }

  void render(const cJSON *claude) {
    const cJSON *limits = json::get(claude, "limits");
    const bool known = cJSON_IsObject(limits);
    lv_obj_set_style_text_color(icon_, known ? theme::accent() : theme::text3(), 0);
    show(meters_[0], json::get(limits, "five_hour"));
    show(meters_[1], json::get(limits, "week"));

    // Where the reading comes from, and its age when it is old
    std::string note;
    if (!has_snapshot(claude)) note = "PC déconnecté";
    else if (!known) note = "Indisponible";
    else {
      const double age = seconds_since(json::num(limits, "at", NAN));
      if (!std::isnan(age) && age > 15 * 60) note = "il y a " + util::format_elapsed(age);
      if (!strcmp(json::str(limits, "source"), "desktop")) note = note.empty() ? "App Claude" : "App Claude · " + note;
    }
    lv_label_set_text(note_, note.c_str());
  }

  lv_obj_t *icon_ = nullptr, *note_ = nullptr;
  Meter meters_[2] = {};
  WidgetContext &ctx_;
  Bus &bus_;
  int ticks_ = 0;
};

}  // namespace

std::unique_ptr<Widget> create_claude_sessions(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<ClaudeSessions>(parent, spec, ctx);
}

std::unique_ptr<Widget> create_claude_usage(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<ClaudeUsage>(parent, spec, ctx);
}

std::unique_ptr<Widget> create_claude_limits(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<ClaudeLimits>(parent, spec, ctx);
}

}  // namespace dm::widgets
