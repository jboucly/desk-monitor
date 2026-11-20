// C wrapper of LVGL's lodepng (its header does not compile as C++). Private to core.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @description A decoded PNG: 8-bit RGBA rows of `stride` bytes.
 */
typedef struct {
  void *handle;
  const uint8_t *rgba;
  unsigned width;
  unsigned height;
  unsigned stride;
} DmPngImage;

/**
 * @description Decodes a PNG of any color type to 8-bit RGBA. Release the image with dm_png_free().
 * @returns 1 on success, 0 when the data is not a valid PNG.
 */
int dm_png_decode_rgba(const unsigned char *png, size_t size, DmPngImage *image);

/**
 * @description Frees a decoded image.
 */
void dm_png_free(DmPngImage *image);

#ifdef __cplusplus
}
#endif
