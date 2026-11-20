#include "dm/cover.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "dm/util.h"
#include "png_decode.h"
#include "src/libs/tjpgd/tjpgd.h"

namespace dm {

namespace {

/**
 * @description Decoder state: input cursor, crop square, and per-pixel sums of the scaled cover.
 */
struct Job {
  const std::string *jpeg;
  size_t offset = 0;
  int x0 = 0, y0 = 0, side = 0, size = 0;
  std::vector<uint32_t> sums;  // r, g, b, count per destination pixel
};

size_t input(JDEC *jd, uint8_t *buf, size_t len) {
  auto *job = static_cast<Job *>(jd->device);
  len = std::min(len, job->jpeg->size() - job->offset);
  if (buf) memcpy(buf, job->jpeg->data() + job->offset, len);
  job->offset += len;
  return len;
}

/**
 * @description Receives one decoded block and adds its pixels to the destination sums. LVGL's copy of
 * TJpgDec writes 24-bit pixels in LVGL's memory order: blue, green, red.
 */
int output(JDEC *jd, void *bitmap, JRECT *rect) {
  auto *job = static_cast<Job *>(jd->device);
  const auto *px = static_cast<const uint8_t *>(bitmap);
  for (int y = rect->top; y <= rect->bottom; y++) {
    for (int x = rect->left; x <= rect->right; x++, px += 3) {
      const int cx = x - job->x0, cy = y - job->y0;
      if (cx < 0 || cy < 0 || cx >= job->side || cy >= job->side) continue;
      const int dx = cx * job->size / job->side, dy = cy * job->size / job->side;
      uint32_t *s = &job->sums[static_cast<size_t>(dy * job->size + dx) * 4];
      s[0] += px[2];
      s[1] += px[1];
      s[2] += px[0];
      s[3]++;
    }
  }
  return 1;
}

}  // namespace

bool decode_jpeg_cover(const std::string &jpeg, int size, std::vector<uint8_t> &rgb565) {
  Job job;
  job.jpeg = &jpeg;
  // Work area of TJpgDec (about 3.5 KB without and 10 KB with JD_FASTDECODE)
  constexpr size_t POOL = 12 * 1024;
  void *pool = malloc(POOL);
  if (!pool) return false;
  JDEC jd;
  bool ok = jd_prepare(&jd, input, pool, POOL, &job) == JDR_OK && jd.width >= size && jd.height >= size;
  if (ok) {
    job.side = std::min<int>(jd.width, jd.height);
    job.x0 = (jd.width - job.side) / 2;
    job.y0 = (jd.height - job.side) / 2;
    job.size = size;
    job.sums.assign(static_cast<size_t>(size * size) * 4, 0);
    ok = jd_decomp(&jd, output, 0) == JDR_OK;
  }
  free(pool);
  if (!ok) return false;

  rgb565.resize(static_cast<size_t>(size * size) * 2);
  for (int i = 0; i < size * size; i++) {
    const uint32_t *s = &job.sums[static_cast<size_t>(i) * 4];
    const uint32_t n = s[3] ? s[3] : 1;
    const uint16_t c = static_cast<uint16_t>(((s[0] / n >> 3) << 11) | ((s[1] / n >> 2) << 5) | (s[2] / n >> 3));
    rgb565[static_cast<size_t>(i) * 2] = c & 0xFF;
    rgb565[static_cast<size_t>(i) * 2 + 1] = c >> 8;
  }
  return true;
}

bool decode_png_thumbnail(const std::string &png, int size, uint32_t background, std::vector<uint8_t> &rgb565) {
  // LVGL's copy of lodepng allocates with lv_malloc (PSRAM on the device)
  DmPngImage image = {};
  if (!dm_png_decode_rgba(reinterpret_cast<const unsigned char *>(png.data()), png.size(), &image)) return false;
  const unsigned w = image.width, h = image.height;
  const int side = static_cast<int>(std::min(w, h));
  const int x0 = (static_cast<int>(w) - side) / 2, y0 = (static_cast<int>(h) - side) / 2;
  const uint32_t bg_r = (background >> 16) & 0xFF, bg_g = (background >> 8) & 0xFF, bg_b = background & 0xFF;
  rgb565.resize(static_cast<size_t>(size * size) * 2);
  for (int dy = 0; dy < size; dy++) {
    // Source rows and columns covered by this destination pixel (at least one)
    const int sy0 = dy * side / size, sy1 = std::max(sy0 + 1, (dy + 1) * side / size);
    for (int dx = 0; dx < size; dx++) {
      const int sx0 = dx * side / size, sx1 = std::max(sx0 + 1, (dx + 1) * side / size);
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++, n++) {
          const uint8_t *px = image.rgba + static_cast<size_t>(y0 + sy) * image.stride + static_cast<size_t>(x0 + sx) * 4;
          const uint32_t a = px[3];
          r += (px[0] * a + bg_r * (255 - a)) / 255;
          g += (px[1] * a + bg_g * (255 - a)) / 255;
          b += (px[2] * a + bg_b * (255 - a)) / 255;
        }
      }
      const uint16_t c = static_cast<uint16_t>(((r / n >> 3) << 11) | ((g / n >> 2) << 5) | (b / n >> 3));
      rgb565[static_cast<size_t>(dy * size + dx) * 2] = c & 0xFF;
      rgb565[static_cast<size_t>(dy * size + dx) * 2 + 1] = c >> 8;
    }
  }
  dm_png_free(&image);
  return true;
}

cJSON *cover_json(const std::string &id, int size, const std::vector<uint8_t> &rgb565) {
  cJSON *art = cJSON_CreateObject();
  cJSON_AddStringToObject(art, "id", id.c_str());
  cJSON_AddNumberToObject(art, "w", size);
  cJSON_AddNumberToObject(art, "h", size);
  cJSON_AddStringToObject(art, "data", util::base64_encode(rgb565.data(), rgb565.size()).c_str());
  return art;
}

}  // namespace dm
