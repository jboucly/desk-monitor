// Layout model: the pages and widgets described by layout.json (protocol/layout.schema.json).
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dm/json.h"

namespace dm {

// Size of the widget grid on every page.
constexpr int GRID_COLS = 4;
constexpr int GRID_ROWS = 4;

/**
 * @description One widget placed on the grid. `props` points into the layout's JSON tree.
 */
struct WidgetSpec {
  std::string id;
  std::string type;
  int x = 0, y = 0, w = 1, h = 1;
  const cJSON *props = nullptr;
};

/**
 * @description One swipeable page.
 */
struct PageSpec {
  std::string id;
  std::string title;
  std::vector<WidgetSpec> widgets;
};

/**
 * @description Theme options of the layout.
 */
struct ThemeSpec {
  uint32_t accent = 0x7C9CFF;
  uint32_t accent2 = 0xC084FC;
  std::string wallpaper = "aurora";
  std::string style = "flat";  // flat, frozen or glass (see theme.h)
};

/**
 * @description A validated layout. It keeps the parsed JSON alive so that widget props stay valid.
 */
class Layout {
 public:
  /**
   * @description Parses and validates a layout document.
   * @param text JSON text.
   * @param error Receives a French, user-facing message when validation fails.
   * @returns The layout, or null when the document is invalid.
   */
  static std::unique_ptr<Layout> parse(const std::string &text, std::string &error);

  /**
   * @description Every Home Assistant entity id referenced by a widget or a shortcut.
   */
  std::set<std::string> ha_entities() const;

  /**
   * @description Compact JSON of the layout as it was accepted.
   */
  const std::string &source() const { return source_; }

  ThemeSpec theme;
  std::vector<PageSpec> pages;

 private:
  json::Ptr root_;
  std::string source_;
};

/**
 * @description Widget types known by the firmware.
 */
bool is_known_widget_type(const std::string &type);

}  // namespace dm
