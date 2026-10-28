// PC companion session (transport-free), see protocol/README.md "PC link".
//
// The screen is the WebSocket server. The platform forwards open/close/message
// events of each connection here. A PC proves it was paired with a token; an
// unknown PC asks for a pairing code, which the screen shows and the user types
// in the companion. Only the paired PC receives shortcut actions.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "dm/bus.h"
#include "dm/settings.h"

namespace dm {

/**
 * @description Protocol state machine of the PC link. Thread-safe; one active connection at a time.
 */
class PcSession {
 public:
  using Sender = std::function<void(const std::string &text)>;
  using Closer = std::function<void()>;

  /**
   * @param random Source of random numbers for pairing codes and tokens (esp_random on the device).
   */
  PcSession(Bus &bus, Settings &settings, std::function<uint32_t()> random)
      : bus_(bus), settings_(settings), random_(std::move(random)) {}

  /**
   * @description A PC connected. It replaces the previous connection, which is closed.
   * @param send Writes one text frame on this connection.
   * @param close Closes this connection.
   * @returns The id to pass to on_message() and on_close().
   */
  uint32_t on_open(Sender send, Closer close);

  /**
   * @description Handles one complete text message of a connection.
   */
  void on_message(uint32_t connection, const std::string &text);

  /**
   * @description A connection closed. Ignored when it was already replaced.
   */
  void on_close(uint32_t connection);

  /**
   * @description Call about once a second: drops a silent PC and expires the pairing code.
   */
  void poll();

  /**
   * @description The paired PC was forgotten from the editor: disconnects it.
   */
  void forget();

  /**
   * @description Sends a shortcut action to the paired PC.
   * @returns False when no paired PC is connected.
   */
  bool send_action(const cJSON *action);

  /**
   * @description Sends a media command ("toggle", "next", "prev") to the paired PC.
   * @returns False when no paired PC is connected.
   */
  bool send_media(const std::string &command);

 private:
  using Clock = std::chrono::steady_clock;
  enum class State { Closed, Unpaired, Ready };

  void send_locked(cJSON *message);
  void flush();
  void set_state_locked(State state);
  void end_pairing_locked();
  std::string random_digits(int count);

  Bus &bus_;
  Settings &settings_;
  std::function<uint32_t()> random_;

  std::mutex mutex_;
  uint32_t connection_ = 0;
  uint32_t next_connection_ = 1;
  Sender send_;
  Closer close_;
  std::vector<std::string> outgoing_;  // frames queued under the lock, written by flush()
  State state_ = State::Closed;
  std::string pc_name_;
  Clock::time_point last_message_;
  uint32_t next_action_id_ = 1;

  // Pairing in progress
  std::string pair_code_;
  Clock::time_point pair_deadline_;
  int pair_attempts_ = 0;
};

}  // namespace dm
