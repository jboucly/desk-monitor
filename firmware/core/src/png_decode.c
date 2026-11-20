/*
  PNG decoding through LVGL's copy of lodepng (LV_USE_LODEPNG).
  Its header only compiles as C: C++ code (cover.cpp) calls this wrapper instead.
  LVGL patched lodepng_decode32() to return an lv_draw_buf_t (pixels in R, G, B, A byte order) instead of
  a plain pixel array.
*/
#include "png_decode.h"

#include "lvgl.h"
#include "src/libs/lodepng/lodepng.h"

int dm_png_decode_rgba(const unsigned char *png, size_t size, DmPngImage *image) {
  lv_draw_buf_t *decoded = NULL;
  unsigned width = 0, height = 0;
  const unsigned error = lodepng_decode32((unsigned char **)&decoded, &width, &height, png, size);
  if (error != 0 || !decoded || width == 0 || height == 0) {
    if (decoded) lv_draw_buf_destroy(decoded);
    return 0;
  }
  image->handle = decoded;
  image->rgba = decoded->data;
  image->width = width;
  image->height = height;
  image->stride = decoded->header.stride;
  return 1;
}

void dm_png_free(DmPngImage *image) {
  if (image->handle) lv_draw_buf_destroy((lv_draw_buf_t *)image->handle);
  image->handle = NULL;
  image->rgba = NULL;
}
