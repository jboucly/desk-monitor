// Minimal WebSocket client for the simulator's 3D printer link (the device uses esp_websocket_client).
// Plain ws:// only, text frames, reconnects every few seconds while started.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sim {

/**
 * @description One WebSocket connection kept open in a background thread.
 */
class WsClient {
 public:
  /**
   * @description Callbacks of the connection. They run on the client's thread.
   */
  struct Handlers {
    std::function<void()> on_open;
    std::function<void(const std::string &text)> on_message;
    std::function<void()> on_close;
  };

  ~WsClient() { stop(); }

  /**
   * @description Connects to a ws://host:port/path URL, and reconnects after every failure until stop().
   */
  void start(const std::string &url, Handlers handlers);

  /**
   * @description Closes the connection and stops reconnecting. Waits for the thread.
   */
  void stop();

  /**
   * @description Sends a text frame. Returns false when not connected.
   */
  bool send(const std::string &text);

 private:
  void run(std::string url, Handlers handlers);

  std::thread thread_;
  std::mutex mutex_;  // socket writes and fd_
  std::condition_variable wake_;
  std::atomic<bool> running_{false};
  int fd_ = -1;
};

}  // namespace sim
