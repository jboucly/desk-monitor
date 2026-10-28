#include "ws_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace sim {

namespace {

/**
 * @description SHA-1 of a short string (only used for the Sec-WebSocket-Accept handshake value).
 */
std::string sha1(const std::string &input) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string msg = input;
  const uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
  msg += static_cast<char>(0x80);
  while (msg.size() % 64 != 56) msg += '\0';
  for (int i = 7; i >= 0; i--) msg += static_cast<char>((bit_len >> (i * 8)) & 0xFF);

  const auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      const auto *p = reinterpret_cast<const unsigned char *>(&msg[chunk + i * 4]);
      w[i] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  std::string out;
  for (uint32_t v : h)
    for (int i = 3; i >= 0; i--) out += static_cast<char>((v >> (i * 8)) & 0xFF);
  return out;
}

std::string base64(const std::string &in) {
  static const char *TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t n = static_cast<unsigned char>(in[i]) << 16;
    if (i + 1 < in.size()) n |= static_cast<unsigned char>(in[i + 1]) << 8;
    if (i + 2 < in.size()) n |= static_cast<unsigned char>(in[i + 2]);
    out += TABLE[(n >> 18) & 63];
    out += TABLE[(n >> 12) & 63];
    out += i + 1 < in.size() ? TABLE[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? TABLE[n & 63] : '=';
  }
  return out;
}

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
 * @description One client socket. Writes are serialized because the LVGL loop and the reader both send.
 */
struct Connection {
  int fd;
  std::mutex write_mutex;
  std::atomic<bool> open{true};

  void write_frame(uint8_t opcode, const std::string &payload) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) {
      frame += static_cast<char>(payload.size());
    } else if (payload.size() < 65536) {
      frame += static_cast<char>(126);
      frame += static_cast<char>((payload.size() >> 8) & 0xFF);
      frame += static_cast<char>(payload.size() & 0xFF);
    } else {
      frame += static_cast<char>(127);
      for (int i = 7; i >= 0; i--) frame += static_cast<char>((static_cast<uint64_t>(payload.size()) >> (i * 8)) & 0xFF);
    }
    frame += payload;
    std::lock_guard<std::mutex> lock(write_mutex);
    if (open) send(fd, frame.data(), frame.size(), 0);
  }

  void shut() {
    if (open.exchange(false)) shutdown(fd, SHUT_RDWR);
  }
};

void serve(int fd, const std::string &path, const WsHandlers &handlers) {
  // HTTP upgrade request, read up to the blank line
  std::string request;
  char c;
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
    if (recv(fd, &c, 1, 0) != 1) return;
    request += c;
  }
  const std::string key_header = "sec-websocket-key:";
  std::string lower = request;
  for (char &ch : lower) ch = static_cast<char>(tolower(ch));
  const size_t key_pos = lower.find(key_header);
  if (request.rfind("GET " + path + " ", 0) != 0 || key_pos == std::string::npos) {
    const char *reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send(fd, reply, strlen(reply), 0);
    return;
  }
  size_t start = key_pos + key_header.size();
  while (request[start] == ' ') start++;
  const std::string key = request.substr(start, request.find("\r\n", start) - start);
  const std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  send(fd, reply.data(), reply.size(), 0);

  auto conn = std::make_shared<Connection>();
  conn->fd = fd;
  const uint32_t id = handlers.on_open([conn](const std::string &text) { conn->write_frame(0x1, text); },
                                       [conn] { conn->shut(); });

  std::string message;
  while (conn->open) {
    uint8_t head[2];
    if (!read_exact(fd, head, 2)) break;
    const uint8_t opcode = head[0] & 0x0F;
    const bool fin = head[0] & 0x80;
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
    if (len > 1024 * 1024) break;
    uint8_t mask[4] = {0, 0, 0, 0};
    if ((head[1] & 0x80) && !read_exact(fd, mask, 4)) break;
    std::string payload(len, '\0');
    if (len && !read_exact(fd, payload.data(), len)) break;
    for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);

    if (opcode == 0x8) break;                              // close
    if (opcode == 0x9) conn->write_frame(0xA, payload);    // ping → pong
    if (opcode == 0x1 || opcode == 0x0) {                  // text, continuation
      message += payload;
      if (fin) {
        handlers.on_message(id, message);
        message.clear();
      }
    }
  }
  conn->shut();
  handlers.on_close(id);
}

}  // namespace

bool ws_listen(int port, const std::string &path, WsHandlers handlers) {
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return false;
  const int yes = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(server, 4) != 0) {
    close(server);
    return false;
  }
  std::thread([server, path, handlers] {
    while (true) {
      const int fd = accept(server, nullptr, nullptr);
      if (fd < 0) continue;
      const int nodelay = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
      std::thread([fd, path, handlers] {
        serve(fd, path, handlers);
        close(fd);
      }).detach();
    }
  }).detach();
  return true;
}

}  // namespace sim
