// pc_metrics: dashboard of the PC companion (CPU, RAM, GPU usage bars and network throughput).
#include <cmath>
#include <cstdio>

#include "common.h"
#include "dm/util.h"

namespace dm::widgets {

namespace {

/**
 * @description One usage line: name, bar, percentage and an optional detail ("8,1 / 16 Go", "61 °C").
 */
struct Row {
  lv_obj_t *box, *bar, *value;
};

/**
 * @description Throughput in French units (o/s, Ko/s, Mo/s).
 */
constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

std::string format_rate(double bytes_per_s) {
  if (std::isnan(bytes_per_s)) return "—";
  if (bytes_per_s < 1000) return util::format_number(bytes_per_s, 0) + " o/s";
  if (bytes_per_s < 1000 * 1000) return util::format_number(bytes_per_s / 1000, 0) + " Ko/s";
  return util::format_number(bytes_per_s / 1e6, bytes_per_s < 1e7 ? 1 : 0) + " Mo/s";
}

class PcMetrics : public Widget {
 public:
  PcMetrics(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
    root = theme::make_card(parent);
    compact_ = spec.h < 2 || spec.w < 3;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    // Rows spread over the card height
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, 4, 0);

    // Header: PC name and connection state
    lv_obj_t *header = lv_obj_create(root);
    lv_obj_add_style(header, theme::bare(), 0);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 8, 0);
    icon_ = theme::make_icon(header, &dm_icons_18, "desktop-tower", theme::text3());
    title_ = theme::make_label(header, &dm_font_16, theme::text(), json::str(spec.props, "name", "PC"));
    lv_obj_set_flex_grow(title_, 1);
    theme::single_line(title_);

    offline_ = theme::make_label(root, &dm_font_14, theme::text3(), "PC déconnecté");
    lv_obj_set_flex_grow(offline_, 1);

    rows_[0] = make_row("CPU");
    rows_[1] = make_row("RAM");
    rows_[2] = make_row("GPU");

    // Network line (hidden on one-row cards)
    net_ = lv_obj_create(root);
    lv_obj_add_style(net_, theme::bare(), 0);
    lv_obj_set_size(net_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(net_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(net_, 6, 0);
    theme::make_icon(net_, &dm_icons_18, "download", theme::text2());
    rx_ = theme::make_label(net_, &dm_font_14, theme::text2());
    lv_obj_set_width(rx_, lv_pct(40));
    theme::make_icon(net_, &dm_icons_18, "upload", theme::text2());
    tx_ = theme::make_label(net_, &dm_font_14, theme::text2());

    ctx.bus.subscribe("sys/pc", [this, spec](const cJSON *v) {
      const char *name = json::str(v, "name");
      if (!*json::str(spec.props, "name") && *name) lv_label_set_text(title_, name);
    });
    ctx.bus.subscribe("pc", [this](const cJSON *metrics) { render(metrics); });
    render(nullptr);
  }

 private:
  Row make_row(const char *name) {
    Row row;
    row.box = lv_obj_create(root);
    lv_obj_add_style(row.box, theme::bare(), 0);
    lv_obj_set_size(row.box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row.box, 10, 0);

    lv_obj_t *label = theme::make_label(row.box, &dm_font_14, theme::text2(), name);
    lv_obj_set_width(label, 36);

    row.bar = lv_bar_create(row.box);
    lv_obj_set_flex_grow(row.bar, 1);
    lv_obj_set_height(row.bar, 10);
    lv_bar_set_range(row.bar, 0, 1000);
    lv_obj_set_style_radius(row.bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(row.bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(row.bar, theme::track(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row.bar, LV_OPA_COVER, LV_PART_INDICATOR);

    row.value = theme::make_label(row.box, &dm_font_14, theme::text(), "—");
    lv_obj_set_width(row.value, compact_ ? 44 : 150);
    lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
    theme::single_line(row.value);
    return row;
  }

  /**
   * @description Fills one row: percentage on the bar, value text with an optional detail.
   */
  void set_row(Row &row, double percent, const std::string &detail) {
    const bool valid = !std::isnan(percent);
    lv_bar_set_value(row.bar, valid ? static_cast<int32_t>(std::fmin(std::fmax(percent, 0), 100) * 10) : 0,
                     LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row.bar, theme::level(valid ? percent : NAN, 80, 92, theme::accent()),
                              LV_PART_INDICATOR);
    std::string text = valid ? util::format_number(percent, 0) + " %" : "—";
    if (valid && !compact_ && !detail.empty()) text = detail + "   " + text;
    lv_label_set_text(row.value, text.c_str());
  }

  void render(const cJSON *m) {
    const bool online = cJSON_IsNumber(json::get(m, "cpu"));
    lv_obj_set_style_text_color(icon_, online ? theme::accent() : theme::text3(), 0);
    set_hidden(offline_, online);
    for (Row &row : rows_) set_hidden(row.box, !online);
    set_hidden(net_, !online || compact_);
    if (!online) return;

    char buf[48];
    const double cpu_temp = json::num(m, "cpu_temp", NAN);
    if (!std::isnan(cpu_temp)) snprintf(buf, sizeof buf, "%.0f °C", cpu_temp);
    set_row(rows_[0], json::num(m, "cpu", NAN), std::isnan(cpu_temp) ? "" : buf);

    const double used = json::num(m, "ram_used", NAN), total = json::num(m, "ram_total", NAN);
    std::string ram_detail;
    if (!std::isnan(used) && !std::isnan(total))
      // Binary gigabytes, as the operating systems show them
      ram_detail = util::format_number(used / GIB, 1) + " / " + util::format_number(total / GIB, 0) + " Go";
    set_row(rows_[1], json::num(m, "ram", NAN), ram_detail);

    // GPU: hidden when the companion cannot read it
    const double gpu = json::num(m, "gpu", NAN);
    set_hidden(rows_[2].box, std::isnan(gpu));
    const double gpu_temp = json::num(m, "gpu_temp", NAN);
    if (!std::isnan(gpu_temp)) snprintf(buf, sizeof buf, "%.0f °C", gpu_temp);
    set_row(rows_[2], gpu, std::isnan(gpu_temp) ? "" : buf);

    lv_label_set_text(rx_, format_rate(json::num(m, "net_rx", NAN)).c_str());
    lv_label_set_text(tx_, format_rate(json::num(m, "net_tx", NAN)).c_str());
  }

  static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }

  bool compact_ = false;
  lv_obj_t *icon_ = nullptr, *title_ = nullptr, *offline_ = nullptr, *net_ = nullptr, *rx_ = nullptr, *tx_ = nullptr;
  Row rows_[3] = {};
};

}  // namespace

std::unique_ptr<Widget> create_pc_metrics(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx) {
  return std::make_unique<PcMetrics>(parent, spec, ctx);
}

}  // namespace dm::widgets
