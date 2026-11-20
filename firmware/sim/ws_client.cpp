#include "ws_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace sim {

namespace {

constexpr int RECONNECT_S = 5;
constexpr int CONNECT_TIMEOUT_S = 5;

bool read_exact(int fd, void *buf, size_t len) {
  auto *p = static_cast<char *>(buf);
  while (len > 0) {
    const ssize_t n = recv(fd, p, len, 0);
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

/**
 * @description Opens a TCP connection with a timeout. Returns -1 on failure.
 */
int connect_to(const std::string &host, const std::string &port) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *list = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 || !list) return -1;
  const int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(list);
    return -1;
  }
  // Non-blocking connect, so that an absent printer does not hang the thread for a minute
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = connect(fd, list->ai_addr, list->ai_addrlen);
  freeaddrinfo(list);
  if (rc != 0 && errno == EINPROGRESS) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval timeout = {CONNECT_TIMEOUT_S, 0};
    int error = 0;
    socklen_t len = sizeof error;
    rc = select(fd + 1, nullptr, &set, nullptr, &timeout) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0 ? 0 : -1;
  }
  if (rc != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, flags);
  const int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
  return fd;
}

/**
 * @description Builds a client frame (always masked, as RFC 6455 requires).
 */
std::string make_frame(uint8_t opcode, const std::string &payload) {
  static std::mt19937 rng{std::random_device{}()};
  std::string frame;
  frame += static_cast<char>(0x80 | opcode);
  if (payload.size() < 126) {
    frame += static_cast<char>(0x80 | payload.size());
  } else if (payload.size() < 65536) {
    frame += static_cast<char>(0x80 | 126);
    frame += static_cast<char>((payload.size() >> 8) & 0xFF);
    frame += static_cast<char>(payload.size() & 0xFF);
  } else {
    frame += static_cast<char>(0x80 | 127);
    for (int i = 7; i >= 0; i--) frame += static_cast<char>((static_cast<uint64_t>(payload.size()) >> (i * 8)) & 0xFF);
  }
  uint8_t mask[4];
  for (uint8_t &b : mask) b = static_cast<uint8_t>(rng());
  frame.append(reinterpret_cast<const char *>(mask), 4);
  for (size_t i = 0; i < payload.size(); i++) frame += static_cast<char>(payload[i] ^ mask[i % 4]);
  return frame;
}

}  // namespace

void WsClient::start(const std::string &url, Handlers handlers) {
  stop();
  running_ = true;
  thread_ = std::thread(&WsClient::run, this, url, std::move(handlers));
}

void WsClient::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
  }
  wake_.notify_all();
  thread_.join();
}

bool WsClient::send(const std::string &text) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) return false;
  const std::string frame = make_frame(0x1, text);
  return ::send(fd_, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
}

void WsClient::run(std::string url, Handlers handlers) {
  // ws://host:port/path
  std::string rest = url.rfind("ws://", 0) == 0 ? url.substr(5) : url;
  const size_t slash = rest.find('/');
  const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::string authority = rest.substr(0, slash);
  const size_t colon = authority.rfind(':');
  const std::string host = authority.substr(0, colon);
  const std::string port = colon == std::string::npos ? "80" : authority.substr(colon + 1);

  while (running_) {
    const int fd = connect_to(host, port);
    bool opened = false;
    if (fd >= 0) {
      const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + authority +
                                  "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: ZGVzay1tb25pdG9yLXNpbQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
      ::send(fd, request.data(), request.size(), 0);
      std::string reply;
      char c;
      while (reply.find("\r\n\r\n") == std::string::npos && reply.size() < 4096 && recv(fd, &c, 1, 0) == 1) reply += c;
      opened = reply.find(" 101 ") != std::string::npos;
    }
    if (opened) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        fd_ = fd;
      }
      printf("[printer] connected to %s\n", url.c_str());
      if (handlers.on_open) handlers.on_open();
      std::string message;
      while (running_) {
        uint8_t head[2];
        if (!read_exact(fd, head, 2)) break;
        const uint8_t opcode = head[0] & 0x0F;
        uint64_t len = head[1] & 0x7F;
        if (len == 126) {
          uint8_t ext[2];
          if (!read_exact(fd, ext, 2)) break;
          len = (ext[0] << 8) | ext[1];
        } else if (len == 127) {
          uint8_t ext[8];
          if (!read_exact(fd, ext, 8)) break;
          len = 0;
          for (uint8_t b : ext) len = (len << 8) | b;
        }
        if (len > 4 * 1024 * 1024) break;
        uint8_t mask[4] = {0, 0, 0, 0};
        if ((head[1] & 0x80) && !read_exact(fd, mask, 4)) break;
        std::string payload(len, '\0');
        if (len && !read_exact(fd, payload.data(), len)) break;
        for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        if (opcode == 0x8) break;
        if (opcode == 0x9) {
          std::lock_guard<std::mutex> lock(mutex_);
          const std::string pong = make_frame(0xA, payload);
          ::send(fd, pong.data(), pong.size(), 0);
        }
        if (opcode == 0x1 || opcode == 0x0) {
          message += payload;
          if (head[0] & 0x80) {
            if (handlers.on_message) handlers.on_message(message);
            message.clear();
          }
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        fd_ = -1;
      }
      printf("[printer] disconnected\n");
    }
    if (fd >= 0) close(fd);
    if (handlers.on_close) handlers.on_close();
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::seconds(RECONNECT_S), [this] { return !running_; });
  }
}

}  // namespace sim
