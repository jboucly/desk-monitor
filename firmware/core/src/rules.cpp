#include "dm/rules.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dm/json.h"

namespace dm {

namespace {

// Bounds the work done on every state change of a tile
constexpr int MAX_RULES = 12;

/**
 * @description Reads a string or a list of strings ("is", "not"). Numbers are accepted and kept as text.
 */
bool read_values(const cJSON *item, std::vector<std::string> &out) {
  if (!item) return true;
  auto add = [&out](const cJSON *v) {
    if (cJSON_IsString(v)) out.emplace_back(v->valuestring);
    else if (cJSON_IsNumber(v)) {
      char buf[32];
      snprintf(buf, sizeof buf, "%g", v->valuedouble);
      out.emplace_back(buf);
    } else if (cJSON_IsBool(v)) out.emplace_back(cJSON_IsTrue(v) ? "true" : "false");
    else return false;
    return true;
  };
  if (!cJSON_IsArray(item)) return add(item);
  const cJSON *v = nullptr;
  cJSON_ArrayForEach(v, item) if (!add(v)) return false;
  return true;
}

/**
 * @description Text of a state or attribute value, as compared by "is" and "not".
 */
std::string value_text(const cJSON *value) {
  if (cJSON_IsString(value)) return value->valuestring;
  if (cJSON_IsNumber(value)) {
    char buf[32];
    snprintf(buf, sizeof buf, "%g", value->valuedouble);
    return buf;
  }
  if (cJSON_IsBool(value)) return cJSON_IsTrue(value) ? "true" : "false";
  return {};
}

/**
 * @description Number of a state or attribute value, or false when it is not numeric.
 */
bool value_number(const cJSON *value, double &out) {
  if (cJSON_IsNumber(value)) {
    out = value->valuedouble;
    return true;
  }
  if (!cJSON_IsString(value) || !*value->valuestring) return false;
  char *end = nullptr;
  out = strtod(value->valuestring, &end);
  return *end == '\0';
}

bool contains(const std::vector<std::string> &list, const std::string &value) {
  for (const auto &item : list)
    if (item == value) return true;
  return false;
}

}  // namespace

bool Rules::parse_condition(const cJSON *json, RuleCondition &out, std::string &error) {
  if (!json) return true;
  if (!cJSON_IsObject(json)) {
    error = "condition invalide (objet attendu)";
    return false;
  }
  out.entity = json::str(json, "entity");
  out.attr = json::str(json, "attr");
  if (!out.entity.empty() && !strchr(out.entity.c_str(), '.')) {
    error = "condition : entité invalide \"" + out.entity + "\"";
    return false;
  }
  if (!read_values(json::get(json, "is"), out.is) || !read_values(json::get(json, "not"), out.not_)) {
    error = "condition : « is » et « not » attendent un texte ou une liste";
    return false;
  }
  for (const char *key : {"above", "below"}) {
    const cJSON *item = json::get(json, key);
    if (!item) continue;
    if (!cJSON_IsNumber(item)) {
      error = std::string("condition : « ") + key + " » attend un nombre";
      return false;
    }
    if (!strcmp(key, "above")) out.above = item->valuedouble, out.has_above = true;
    else out.below = item->valuedouble, out.has_below = true;
  }
  return true;
}

bool Rules::parse(const cJSON *json, std::string &error) {
  rules_.clear();
  if (!json) return true;
  if (!cJSON_IsArray(json)) {
    error = "règles : liste attendue";
    return false;
  }
  if (cJSON_GetArraySize(json) > MAX_RULES) {
    error = "règles : " + std::to_string(MAX_RULES) + " au maximum";
    return false;
  }
  int index = 0;
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, json) {
    index++;
    const std::string where = "règle " + std::to_string(index) + " : ";
    if (!cJSON_IsObject(item)) {
      error = where + "objet attendu";
      return false;
    }
    Rule rule;
    if (!parse_condition(json::get(item, "when"), rule.when, error)) {
      error = where + error;
      return false;
    }
    rule.text = json::str(item, "text");
    rule.icon = json::str(item, "icon");
    if (const cJSON *color = json::get(item, "color")) {
      const char *hex = cJSON_IsString(color) ? color->valuestring : "";
      char *end = nullptr;
      const unsigned long value = hex[0] == '#' && strlen(hex) == 7 ? strtoul(hex + 1, &end, 16) : 0;
      if (!end || *end != '\0') {
        error = where + "couleur invalide (#RRGGBB attendu)";
        return false;
      }
      rule.color = static_cast<uint32_t>(value);
      rule.has_color = true;
    }
    if (const cJSON *active = json::get(item, "active"); cJSON_IsBool(active)) rule.active = cJSON_IsTrue(active) ? 1 : 0;
    rules_.push_back(std::move(rule));
  }
  return true;
}

bool Rules::test(const RuleCondition &when, const cJSON *state, const Lookup &lookup) {
  if (!when.entity.empty()) state = lookup ? lookup(when.entity) : nullptr;
  if (!state) return false;
  const cJSON *value = when.attr.empty() ? json::get(state, "s") : json::get(json::get(state, "a"), when.attr.c_str());

  const std::string text = value_text(value);
  if (!when.is.empty() && !contains(when.is, text)) return false;
  if (!when.not_.empty() && contains(when.not_, text)) return false;
  if (when.has_above || when.has_below) {
    double number = 0;
    if (!value_number(value, number)) return false;
    if (when.has_above && !(number > when.above)) return false;
    if (when.has_below && !(number < when.below)) return false;
  }
  return true;
}

const Rule *Rules::match(const cJSON *state, const Lookup &lookup) const {
  for (const Rule &rule : rules_)
    if (test(rule.when, state, lookup)) return &rule;
  return nullptr;
}

std::set<std::string> Rules::entities() const {
  std::set<std::string> out;
  for (const Rule &rule : rules_)
    if (!rule.when.entity.empty()) out.insert(rule.when.entity);
  return out;
}

/**
 * @description Text of a value inserted in a rule text: numbers get a decimal comma ("21,5"), like the
 * rest of the French interface.
 */
static std::string display_text(const cJSON *value) {
  std::string text = value_text(value);
  double number = 0;
  if (value_number(value, number))
    for (char &c : text)
      if (c == '.') c = ',';
  return text;
}

std::string Rules::format(const std::string &text, const cJSON *state, const std::string &name) {
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t open = text.find('{', pos);
    const size_t close = open == std::string::npos ? open : text.find('}', open);
    if (close == std::string::npos) {
      out += text.substr(pos);
      break;
    }
    out += text.substr(pos, open - pos);
    const std::string key = text.substr(open + 1, close - open - 1);
    if (key == "state") out += display_text(json::get(state, "s"));
    else if (key == "name") out += name;
    else if (key.rfind("attr.", 0) == 0) out += display_text(json::get(json::get(state, "a"), key.c_str() + 5));
    else out += text.substr(open, close - open + 1);  // unknown placeholder: kept as typed
    pos = close + 1;
  }
  return out;
}

}  // namespace dm
