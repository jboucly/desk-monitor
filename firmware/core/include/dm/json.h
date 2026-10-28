// Small helpers around cJSON shared by the whole firmware.
#pragma once

#include <memory>
#include <string>

#include "cJSON.h"

namespace dm::json {

// Shared ownership of a parsed cJSON tree (deleted with cJSON_Delete).
using Ptr = std::shared_ptr<cJSON>;

/**
 * @description Wraps a cJSON tree in a shared pointer that frees it when the last owner goes away.
 * @param node Tree to own, may be null.
 */
inline Ptr own(cJSON *node) { return Ptr(node, [](cJSON *n) { cJSON_Delete(n); }); }

/**
 * @description Parses text into an owned tree. Returns null on a syntax error.
 */
inline Ptr parse(const std::string &text) { return own(cJSON_ParseWithLength(text.data(), text.size())); }

/**
 * @description Reads a string member, or returns the fallback when it is missing or not a string.
 */
inline const char *str(const cJSON *obj, const char *key, const char *fallback = "") {
  const cJSON *item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

/**
 * @description Reads a number member, or returns the fallback when it is missing or not a number.
 */
inline double num(const cJSON *obj, const char *key, double fallback = 0) {
  const cJSON *item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
  return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

/**
 * @description Reads a boolean member, or returns the fallback when it is missing or not a boolean.
 */
inline bool boolean(const cJSON *obj, const char *key, bool fallback = false) {
  const cJSON *item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

/**
 * @description Returns a member of any type, or null.
 */
inline const cJSON *get(const cJSON *obj, const char *key) {
  return obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
}

/**
 * @description Serializes a tree without formatting. Returns an empty string for null.
 */
inline std::string print(const cJSON *node) {
  if (!node) return {};
  char *text = cJSON_PrintUnformatted(node);
  std::string out = text ? text : "";
  cJSON_free(text);
  return out;
}

}  // namespace dm::json
