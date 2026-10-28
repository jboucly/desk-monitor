// Widget framework: every widget type is a factory registered in widgets/registry.cpp.
#pragma once

#include <memory>
#include <string>

#include "dm/bus.h"
#include "dm/layout.h"
#include "dm/services.h"
#include "lvgl.h"

namespace dm {

class App;

/**
 * @description What a widget can reach while it is created and while it runs.
 */
struct WidgetContext {
  Bus &bus;
  Services &services;
  App &app;
};

/**
 * @description Base class of a widget instance. LVGL objects live under `root`, which the app deletes;
 * a destructor must only release what lives elsewhere (e.g. a popup on the top layer).
 */
class Widget {
 public:
  virtual ~Widget() = default;

  /**
   * @description Called every second (clocks, relative times).
   */
  virtual void tick() {}

  lv_obj_t *root = nullptr;
};

using WidgetFactory = std::unique_ptr<Widget> (*)(lv_obj_t *parent, const WidgetSpec &spec, WidgetContext &ctx);

/**
 * @description Returns the factory of a widget type, or null for types without an implementation yet.
 */
WidgetFactory find_widget_factory(const std::string &type);

}  // namespace dm
