// Minimal PNG writer for the simulator's screenshots.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

/**
 * @description Writes an RGB565 frame as an 8-bit RGB PNG file.
 */
bool write_png_rgb565(const std::string &path, const uint16_t *pixels, int width, int height);

/**
 * @description Encodes an RGB565 frame as PNG bytes.
 */
std::vector<uint8_t> encode_png_rgb565(const uint16_t *pixels, int width, int height);

}  // namespace sim
