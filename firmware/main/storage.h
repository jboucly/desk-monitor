// LittleFS-backed storage of layout.json and settings.json (partition "storage", mounted on /storage).
#pragma once

#include "dm/services.h"

namespace dm::esp32 {

/**
 * @description Files of the "storage" partition. Writes go through a temporary file and a rename,
 * so a power cut never leaves a half-written layout.
 */
class LittleFsStorage : public Storage {
 public:
  /**
   * @description Mounts the partition, formatting it on first boot.
   */
  LittleFsStorage();

  std::optional<std::string> read(const std::string &name) override;
  bool write(const std::string &name, const std::string &data) override;
};

}  // namespace dm::esp32
