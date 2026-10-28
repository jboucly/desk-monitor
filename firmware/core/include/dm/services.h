// Interfaces the platform (ESP32 or simulator) provides to the portable core.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "cJSON.h"

namespace dm {

/**
 * @description Small persistent file store (LittleFS on the ESP32, a folder in the simulator).
 */
class Storage {
 public:
  virtual ~Storage() = default;

  /**
   * @description Reads a whole file. Returns nothing when it does not exist.
   * @param name File name without directory, e.g. "layout.json".
   */
  virtual std::optional<std::string> read(const std::string &name) = 0;

  /**
   * @description Replaces a file atomically (write to a temp file, then rename).
   */
  virtual bool write(const std::string &name, const std::string &data) = 0;
};

/**
 * @description Actions widgets can trigger. Every callback may be empty (feature not available).
 */
struct Services {
  /**
   * @description Calls a Home Assistant service.
   * The data tree (may be null) is owned by the callee.
   */
  std::function<void(const std::string &domain, const std::string &service, const std::string &entity_id, cJSON *data)>
      ha_call;

  /**
   * @description Sends a shortcut action to the PC companion. Returns false when no PC is connected.
   */
  std::function<bool(const cJSON *action)> pc_action;
};

}  // namespace dm
