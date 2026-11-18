// Display rules of the Home Assistant tiles: "when this state matches, show this text, icon or color".
// Plain C++ (no LVGL), so that the layout validation can parse them too. See protocol/README.md, "Rules".
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "cJSON.h"

namespace dm {

/**
 * @description Condition of a rule. Every field set must hold; an empty condition always matches.
 * The tested value is the state of `entity` (default: the tile's entity), or its attribute `attr`.
 */
struct RuleCondition {
  std::string entity;
  std::string attr;
  std::vector<std::string> is;   // value equals one of these
  std::vector<std::string> not_;  // value equals none of these
  double above = 0, below = 0;
  bool has_above = false, has_below = false;
};

/**
 * @description One rule: a condition and what it changes. Empty strings and has_* = false keep the default.
 */
struct Rule {
  RuleCondition when;
  std::string text;  // placeholders: {state}, {name}, {attr.<name>}
  std::string icon;
  uint32_t color = 0;
  bool has_color = false;
  int active = -1;  // -1 = default, 0 = force inactive, 1 = force the colored "active" look
};

/**
 * @description Ordered list of rules; the first one that matches wins.
 */
class Rules {
 public:
  /**
   * @description Returns the state of another entity (`{"s", "a"}`), or null when unknown.
   */
  using Lookup = std::function<const cJSON *(const std::string &entity_id)>;

  /**
   * @description Parses the `rules` prop of a widget (missing = no rule).
   * @param json Array of rules, or null.
   * @param error Receives a French, user-facing message when a rule is malformed.
   * @returns False when the rules are invalid.
   */
  bool parse(const cJSON *json, std::string &error);

  /**
   * @description First rule matching the tile's state, or null.
   * @param state State of the tile's entity (`{"s", "a"}`), may be null.
   * @param lookup Gives the state of the other entities a condition names.
   */
  const Rule *match(const cJSON *state, const Lookup &lookup) const;

  /**
   * @description Entity ids named by the conditions (besides the tile's own entity).
   */
  std::set<std::string> entities() const;

  bool empty() const { return rules_.empty(); }

  /**
   * @description Replaces {state}, {name} and {attr.<name>} in a rule text.
   */
  static std::string format(const std::string &text, const cJSON *state, const std::string &name);

  /**
   * @description True when a single condition holds for a state (used by `visible_if` too).
   */
  static bool test(const RuleCondition &when, const cJSON *state, const Lookup &lookup);

  /**
   * @description Parses one condition object. Returns false with a French message when malformed.
   */
  static bool parse_condition(const cJSON *json, RuleCondition &out, std::string &error);

 private:
  std::vector<Rule> rules_;
};

}  // namespace dm
