// Minimal PNG writer (uncompressed deflate blocks) for the simulator's screenshots.
#include "png.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace sim {

namespace {

uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<uint8_t> &out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put32(out, crc32(body.data(), body.size()));
}

}  // namespace

std::vector<uint8_t> encode_png_rgb565(const uint16_t *pixels, int width, int height) {
  // Raw scanlines: filter byte 0 + RGB triplets
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
  for (int y = 0; y < height; y++) {
    raw.push_back(0);
    for (int x = 0; x < width; x++) {
      const uint16_t p = pixels[y * width + x];
      const uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
      raw.push_back(static_cast<uint8_t>((r << 3) | (r >> 2)));
      raw.push_back(static_cast<uint8_t>((g << 2) | (g >> 4)));
      raw.push_back(static_cast<uint8_t>((b << 3) | (b >> 2)));
    }
  }

  // zlib stream made of stored (uncompressed) blocks of at most 65535 bytes
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size();) {
    const size_t len = std::min<size_t>(65535, raw.size() - pos);
    z.push_back(pos + len == raw.size() ? 1 : 0);
    z.push_back(len & 0xFF);
    z.push_back(len >> 8);
    z.push_back(~len & 0xFF);
    z.push_back((~len >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
    pos += len;
  }
  put32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(ihdr, width);
  put32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  return png;
}

bool write_png_rgb565(const std::string &path, const uint16_t *pixels, int width, int height) {
  const std::vector<uint8_t> png = encode_png_rgb565(pixels, width, height);
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
  fclose(f);
  return ok;
}

}  // namespace sim
