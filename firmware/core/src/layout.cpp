#include "dm/layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dm/rules.h"

namespace dm {

namespace {

const char *const KNOWN_TYPES[] = {"clock",           "weather",      "ha_toggle",    "ha_light",
                                   "ha_sensor",       "ha_gauge",     "shortcut",     "pc_metrics",
                                   "pc_gauge",        "media",        "claude_sessions", "claude_usage",
                                   "claude_limits",   "ha_climate",   "ha_cover",     "ha_slider",
                                   "ha_select"};

const char *const ACTION_KINDS[] = {"ha", "keys", "launch", "url"};

// Templates are stored in the layout and sent to HA at every connection
constexpr size_t MAX_TEMPLATE = 1000;
constexpr size_t MAX_TEMPLATES = 16;

/**
 * @description Parses "#RRGGBB" into 0xRRGGBB. Returns false on a malformed color.
 */
bool parse_color(const char *text, uint32_t &out) {
  if (!text || text[0] != '#' || strlen(text) != 7) return false;
  char *end = nullptr;
  unsigned long value = strtoul(text + 1, &end, 16);
  if (*end != '\0') return false;
  out = static_cast<uint32_t>(value);
  return true;
}

/**
 * @description Reads an integer member, rejecting missing or non-integer values.
 */
bool read_int(const cJSON *obj, const char *key, int &out) {
  const cJSON *item = json::get(obj, key);
  if (!cJSON_IsNumber(item) || item->valuedouble != static_cast<int>(item->valuedouble)) return false;
  out = item->valueint;
  return true;
}

/**
 * @description Checks an action. Tile gestures also accept "toggle", "more_info" and "none".
 * Returns an error message, or empty when valid.
 */
std::string check_action(const cJSON *action, bool tile) {
  const char *kind = json::str(action, "kind");
  bool known = false;
  for (const char *k : ACTION_KINDS) known |= strcmp(kind, k) == 0;
  if (tile) for (const char *k : {"toggle", "more_info", "none"}) known |= strcmp(kind, k) == 0;
  if (!known) return std::string("action inconnue \"") + kind + "\"";
  if (!strcmp(kind, "ha") && (!*json::str(action, "domain") || !*json::str(action, "service")))
    return "action HA incomplète (domain/service)";
  if (!strcmp(kind, "keys") && !*json::str(action, "keys")) return "combinaison de touches vide";
  if (!strcmp(kind, "launch") && !*json::str(action, "target")) return "application à lancer manquante";
  if (!strcmp(kind, "url") && !*json::str(action, "url")) return "URL manquante";
  const cJSON *confirm = json::get(action, "confirm");
  if (confirm && !cJSON_IsBool(confirm) && !cJSON_IsString(confirm)) return "« confirm » attend true/false ou une question";
  return {};
}

/**
 * @description Checks the props each widget type needs. Returns an error message, or empty when valid.
 */
std::string check_props(const WidgetSpec &w) {
  // A sensor may show a template instead of an entity
  const char *tpl = json::str(w.props, "template");
  if (strlen(tpl) > MAX_TEMPLATE) return "template trop long (" + std::to_string(MAX_TEMPLATE) + " caractères au plus)";
  if (w.type == "ha_sensor" && *tpl && !*json::str(w.props, "entity")) return {};
  const bool needs_entity = w.type == "ha_toggle" || w.type == "ha_light" || w.type == "ha_sensor" || w.type == "ha_gauge" ||
                            w.type == "ha_climate" || w.type == "ha_cover" || w.type == "ha_slider" ||
                            w.type == "ha_select";
  if (needs_entity) {
    const char *entity = json::str(w.props, "entity");
    if (!strchr(entity, '.')) return "entité Home Assistant manquante";
  }
  if (const cJSON *rules = json::get(w.props, "rules")) {
    Rules parsed;
    std::string error;
    if (!parsed.parse(rules, error)) return error;
  }
  if (w.type == "shortcut") {
    if (std::string problem = check_action(json::get(w.props, "action"), false); !problem.empty())
      return "raccourci : " + problem;
  }
  for (const char *gesture : {"tap_action", "hold_action"}) {
    const cJSON *action = json::get(w.props, gesture);
    if (!action) continue;
    if (std::string problem = check_action(action, true); !problem.empty())
      return std::string(!strcmp(gesture, "tap_action") ? "appui" : "appui long") + " : " + problem;
  }
  return {};
}

}  // namespace

bool is_known_widget_type(const std::string &type) {
  for (const char *t : KNOWN_TYPES)
    if (type == t) return true;
  return false;
}

std::unique_ptr<Layout> Layout::parse(const std::string &text, std::string &error) {
  auto layout = std::make_unique<Layout>();
  layout->root_ = json::parse(text);
  const cJSON *root = layout->root_.get();
  if (!cJSON_IsObject(root)) {
    error = "JSON invalide";
    return nullptr;
  }
  if (json::num(root, "version") != 1) {
    error = "version de layout non supportée (attendu : 1)";
    return nullptr;
  }

  // Theme (every field is optional)
  if (const cJSON *theme = json::get(root, "theme")) {
    if (const cJSON *accent = json::get(theme, "accent"); accent && !parse_color(accent->valuestring, layout->theme.accent)) {
      error = "theme.accent : couleur invalide";
      return nullptr;
    }
    if (const cJSON *accent2 = json::get(theme, "accent2"); accent2 && !parse_color(accent2->valuestring, layout->theme.accent2)) {
      error = "theme.accent2 : couleur invalide";
      return nullptr;
    }
    layout->theme.wallpaper = json::str(theme, "wallpaper", "aurora");
    layout->theme.style = json::str(theme, "style", "flat");
    if (layout->theme.style != "glass" && layout->theme.style != "frozen" && layout->theme.style != "flat") {
      error = "theme.style : flat, frozen ou glass attendu";
      return nullptr;
    }
  }

  const cJSON *pages = json::get(root, "pages");
  const int page_count = cJSON_GetArraySize(pages);
  if (!cJSON_IsArray(pages) || page_count < 1 || page_count > 12) {
    error = "il faut entre 1 et 12 pages";
    return nullptr;
  }

  std::set<std::string> page_ids;
  const cJSON *page_json = nullptr;
  cJSON_ArrayForEach(page_json, pages) {
    PageSpec page;
    page.id = json::str(page_json, "id");
    page.title = json::str(page_json, "title");
    if (page.id.empty() || !page_ids.insert(page.id).second) {
      error = "page sans id ou id en double : \"" + page.id + "\"";
      return nullptr;
    }

    // Occupancy map used to reject overlapping widgets
    bool used[GRID_ROWS][GRID_COLS] = {};
    const cJSON *widget_json = nullptr;
    cJSON_ArrayForEach(widget_json, json::get(page_json, "widgets")) {
      WidgetSpec w;
      w.id = json::str(widget_json, "id");
      w.type = json::str(widget_json, "type");
      w.props = json::get(widget_json, "props");
      const std::string where = "page \"" + page.title + "\", widget " + w.type;
      if (!is_known_widget_type(w.type)) {
        error = "type de widget inconnu : \"" + w.type + "\"";
        return nullptr;
      }
      if (!read_int(widget_json, "x", w.x) || !read_int(widget_json, "y", w.y) || !read_int(widget_json, "w", w.w) ||
          !read_int(widget_json, "h", w.h) || w.x < 0 || w.y < 0 || w.w < 1 || w.h < 1 || w.x + w.w > GRID_COLS ||
          w.y + w.h > GRID_ROWS) {
        error = where + " : position hors de la grille 4×4";
        return nullptr;
      }
      for (int row = w.y; row < w.y + w.h; row++) {
        for (int col = w.x; col < w.x + w.w; col++) {
          if (used[row][col]) {
            error = where + " : chevauche un autre widget";
            return nullptr;
          }
          used[row][col] = true;
        }
      }
      if (std::string problem = check_props(w); !problem.empty()) {
        error = where + " : " + problem;
        return nullptr;
      }
      page.widgets.push_back(std::move(w));
    }
    layout->pages.push_back(std::move(page));
  }

  // Each template is a live subscription on the HA side
  if (layout->ha_templates().size() > MAX_TEMPLATES) {
    error = "trop de templates Home Assistant (" + std::to_string(MAX_TEMPLATES) + " au plus)";
    return nullptr;
  }

  layout->source_ = json::print(root);
  return layout;
}

std::set<std::string> Layout::ha_templates() const {
  std::set<std::string> out;
  for (const auto &page : pages)
    for (const auto &w : page.widgets)
      if (const char *tpl = json::str(w.props, "template"); *tpl) out.insert(tpl);
  return out;
}

std::set<std::string> Layout::ha_entities() const {
  std::set<std::string> out;
  for (const auto &page : pages) {
    for (const auto &w : page.widgets) {
      if (const char *entity = json::str(w.props, "entity"); *entity) out.insert(entity);
      // Rules may test other entities: they need their states too (validated by parse())
      Rules rules;
      std::string ignored;
      if (rules.parse(json::get(w.props, "rules"), ignored)) out.merge(rules.entities());
      // Entities named by actions: an "ha" call (states not needed, but kept as before) or a toggle of another entity
      for (const char *prop : {"action", "tap_action", "hold_action"}) {
        if (const char *entity = json::str(json::get(w.props, prop), "entity_id"); strchr(entity, '.')) out.insert(entity);
      }
    }
  }
  return out;
}

}  // namespace dm
