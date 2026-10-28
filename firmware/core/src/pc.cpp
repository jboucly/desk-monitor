#include "dm/pc.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "dm/json.h"

namespace dm {

namespace {

// A connected PC sends metrics every second; after this much silence it counts as gone
constexpr auto SILENCE_TIMEOUT = std::chrono::seconds(6);
constexpr auto PAIRING_TIME = std::chrono::minutes(2);
constexpr int PAIRING_ATTEMPTS = 5;

/**
 * @description Builds {"type": type}.
 */
cJSON *message(const char *type) {
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", type);
  return msg;
}

cJSON *welcome_message() {
  cJSON *msg = message("welcome");
  cJSON_AddStringToObject(msg, "name", "Desk Monitor");
  return msg;
}

cJSON *error_message(const char *code, const char *text) {
  cJSON *msg = message("error");
  cJSON_AddStringToObject(msg, "code", code);
  cJSON_AddStringToObject(msg, "message", text);
  return msg;
}

/**
 * @description Constant-time comparison, so that the token cannot be guessed byte by byte.
 */
bool same_secret(const std::string &a, const std::string &b) {
  if (a.size() != b.size() || a.empty()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace

uint32_t PcSession::on_open(Sender send, Closer close) {
  Closer previous;
  uint32_t id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    previous = std::move(close_);
    end_pairing_locked();
    id = connection_ = next_connection_++;
    send_ = std::move(send);
    close_ = std::move(close);
    pc_name_.clear();
    last_message_ = Clock::now();
    set_state_locked(State::Unpaired);
  }
  if (previous) previous();
  return id;
}

void PcSession::on_close(uint32_t connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection != connection_) return;
  connection_ = 0;
  send_ = nullptr;
  close_ = nullptr;
  end_pairing_locked();
  set_state_locked(State::Closed);
}

void PcSession::on_message(uint32_t connection, const std::string &text) {
  auto root = json::parse(text);
  if (!cJSON_IsObject(root.get())) return;
  const std::string type = json::str(root.get(), "type");

  std::string paired_token, paired_name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection != connection_) return;
    last_message_ = Clock::now();

    if (type == "hello") {
      pc_name_ = json::str(root.get(), "name", "PC");
      if (same_secret(json::str(root.get(), "token"), settings_.get().pc_token)) {
        send_locked(welcome_message());
        set_state_locked(State::Ready);
      } else {
        send_locked(message("pair_required"));
      }
    } else if (type == "pair_start" && state_ == State::Unpaired) {
      pair_code_ = random_digits(6);
      pair_deadline_ = Clock::now() + PAIRING_TIME;
      pair_attempts_ = 0;
      cJSON *show = cJSON_CreateObject();
      cJSON_AddStringToObject(show, "code", pair_code_.c_str());
      cJSON_AddStringToObject(show, "name", pc_name_.c_str());
      bus_.post("sys/pc_pair", show);
      send_locked(message("pair_pending"));
    } else if (type == "pair" && state_ == State::Unpaired) {
      if (pair_code_.empty() || Clock::now() > pair_deadline_) {
        end_pairing_locked();
        send_locked(error_message("pair_expired", "Code expiré, recommence l'appairage"));
      } else if (!same_secret(json::str(root.get(), "code"), pair_code_)) {
        if (++pair_attempts_ >= PAIRING_ATTEMPTS) {
          end_pairing_locked();
          send_locked(error_message("pair_expired", "Trop d'essais, recommence l'appairage"));
        } else {
          send_locked(error_message("bad_code", "Code incorrect"));
        }
      } else {
        end_pairing_locked();
        paired_token = random_digits(32);
        paired_name = pc_name_;
        cJSON *paired = message("paired");
        cJSON_AddStringToObject(paired, "token", paired_token.c_str());
        send_locked(paired);
        send_locked(welcome_message());
        set_state_locked(State::Ready);
      }
    } else if (type == "metrics" && state_ == State::Ready) {
      cJSON *metrics = cJSON_Duplicate(root.get(), true);
      cJSON_DeleteItemFromObject(metrics, "type");
      bus_.post("pc", metrics);
    } else if (type == "action_result" && state_ == State::Ready) {
      if (!json::boolean(root.get(), "ok", true)) {
        cJSON *toast = cJSON_CreateObject();
        cJSON_AddStringToObject(toast, "text", json::str(root.get(), "error", "Action refusée par le PC"));
        bus_.post("sys/toast", toast);
      }
    } else if (type == "ping") {
      send_locked(message("pong"));
    }
  }
  // Saved outside the lock: Settings::on_change may call back into this session
  if (!paired_token.empty()) settings_.set_pc(paired_name, paired_token);
  flush();
}

void PcSession::poll() {
  Closer close;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pair_code_.empty() && Clock::now() > pair_deadline_) end_pairing_locked();
    if (state_ != State::Closed && Clock::now() - last_message_ > SILENCE_TIMEOUT) {
      close = close_;
      connection_ = 0;
      send_ = nullptr;
      close_ = nullptr;
      set_state_locked(State::Closed);
    }
  }
  if (close) close();
}

void PcSession::forget() {
  Closer close;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Ready || !settings_.get().pc_token.empty()) return;
    close = close_;
    connection_ = 0;
    send_ = nullptr;
    close_ = nullptr;
    set_state_locked(State::Closed);
  }
  if (close) close();
}

bool PcSession::send_action(const cJSON *action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Ready || !action) return false;
    cJSON *msg = message("action");
    cJSON_AddNumberToObject(msg, "id", next_action_id_++);
    cJSON_AddItemToObject(msg, "action", cJSON_Duplicate(action, true));
    send_locked(msg);
  }
  flush();
  return true;
}

void PcSession::send_locked(cJSON *msg) {
  outgoing_.push_back(json::print(msg));
  cJSON_Delete(msg);
}

void PcSession::flush() {
  std::vector<std::string> frames;
  Sender send;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames.swap(outgoing_);
    send = send_;
  }
  if (send)
    for (const std::string &frame : frames) send(frame);
}

void PcSession::set_state_locked(State state) {
  state_ = state;
  if (state == State::Closed) outgoing_.clear();
  cJSON *status = cJSON_CreateObject();
  cJSON_AddStringToObject(status, "state", state == State::Ready ? "connected" : state == State::Unpaired ? "pairing" : "off");
  cJSON_AddStringToObject(status, "name", pc_name_.c_str());
  bus_.post("sys/pc", status);
  // Widgets clear their values when the PC goes away
  if (state != State::Ready) bus_.post("pc", cJSON_CreateObject());
}

void PcSession::end_pairing_locked() {
  if (pair_code_.empty()) return;
  pair_code_.clear();
  bus_.post("sys/pc_pair", cJSON_CreateObject());
}

std::string PcSession::random_digits(int count) {
  std::string out;
  while (static_cast<int>(out.size()) < count) {
    char chunk[16];
    snprintf(chunk, sizeof chunk, "%08lu", static_cast<unsigned long>(random_() % 100000000UL));
    out += chunk;
  }
  out.resize(count);
  return out;
}

}  // namespace dm
