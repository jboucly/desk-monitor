#include "storage.h"

#include <cstdio>

#include "esp_littlefs.h"
#include "esp_log.h"

namespace dm::esp32 {

namespace {
const char *TAG = "storage";
const char *BASE = "/storage";
}  // namespace

LittleFsStorage::LittleFsStorage() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = BASE;
  conf.partition_label = "storage";
  conf.format_if_mount_failed = true;
  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
}

std::optional<std::string> LittleFsStorage::read(const std::string &name) {
  FILE *f = fopen((std::string(BASE) + "/" + name).c_str(), "rb");
  if (!f) return std::nullopt;
  std::string data;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  fclose(f);
  return data;
}

bool LittleFsStorage::write(const std::string &name, const std::string &data) {
  const std::string path = std::string(BASE) + "/" + name, tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  if (!ok) return false;
  // LittleFS rename replaces the target atomically
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "rename %s failed", path.c_str());
    return false;
  }
  return true;
}

}  // namespace dm::esp32
