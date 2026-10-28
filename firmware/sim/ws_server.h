// Minimal WebSocket server for the simulator's PC link (the device uses esp_http_server instead).
// Text frames only, one thread per connection; enough to run the real companion against the simulator.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sim {

/**
 * @description Callbacks of one WebSocket endpoint. They run on the connection's thread.
 */
struct WsHandlers {
  // A client completed the handshake. `send` writes a text frame, `close` shuts the socket.
  // Returns an id passed back to on_message() and on_close().
  std::function<uint32_t(std::function<void(const std::string &)> send, std::function<void()> close)> on_open;
  std::function<void(uint32_t id, const std::string &text)> on_message;
  std::function<void(uint32_t id)> on_close;
};

/**
 * @description Listens on a TCP port in a background thread and accepts WebSocket clients on `path`.
 * @returns False when the port cannot be opened.
 */
bool ws_listen(int port, const std::string &path, WsHandlers handlers);

}  // namespace sim
