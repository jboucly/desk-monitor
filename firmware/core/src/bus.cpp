#include "dm/bus.h"

namespace dm {

void Bus::post(const std::string &key, cJSON *value) {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.emplace_back(key, json::own(value));
}

void Bus::dispatch() {
  std::vector<std::pair<std::string, json::Ptr>> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(pending_);
  }
  for (auto &[key, value] : batch) {
    values_[key] = value;
    auto range = listeners_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
      if (it->second.second) it->second.second(value.get());
  }
  // Erase the listeners emptied by unsubscribe()
  for (auto it = listeners_.begin(); it != listeners_.end();)
    it = it->second.second ? std::next(it) : listeners_.erase(it);
}

const cJSON *Bus::get(const std::string &key) const {
  auto it = values_.find(key);
  return it == values_.end() ? nullptr : it->second.get();
}

int Bus::subscribe(const std::string &key, Listener listener) {
  const int token = next_token_++;
  auto it = listeners_.emplace(key, std::make_pair(token, std::move(listener)));
  if (const cJSON *value = get(key)) it->second.second(value);
  return token;
}

void Bus::unsubscribe(int token) {
  for (auto &[key, entry] : listeners_)
    if (entry.first == token) entry.second = nullptr;
}

void Bus::clear_subscriptions() { listeners_.clear(); }

}  // namespace dm
