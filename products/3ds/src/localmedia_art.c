/* Cover art decode and fit; see localmedia_art.h. */
#include "localmedia_art.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

size_t lm_art_read(FILE *file, long offset, long raw_bytes, int unsync, uint8_t **out) {
  *out = NULL;
  if (raw_bytes <= 0 || offset < 0 || fseek(file, offset, SEEK_SET) != 0) return 0;
  /* An unsynchronised span may run to the tag end; only refuse what cannot shrink below the cap. */
  if (!unsync && raw_bytes > LM_ART_MAX_BYTES) return 0;
  long take = raw_bytes > LM_ART_MAX_BYTES * 2 ? LM_ART_MAX_BYTES * 2 : raw_bytes;
  uint8_t *bytes = malloc((size_t)take);
  if (!bytes) return 0;
  size_t got = fread(bytes, 1, (size_t)take, file);
  size_t length = got;
  if (unsync) {
    length = 0;
    for (size_t i = 0; i < got; i++) {
      bytes[length++] = bytes[i];
      if (bytes[i] == 0xff && i + 1 < got && bytes[i + 1] == 0) i++;
    }
  }
  if (length == 0 || (long)length > LM_ART_MAX_BYTES || (!unsync && got < (size_t)take)) { free(bytes); return 0; }
  *out = bytes;
  return length;
}

void lm_art_fit(const uint8_t *rgb, int width, int height, uint8_t *out) {
  int side = width < height ? width : height;
  int x0 = (width - side) / 2, y0 = (height - side) / 2;
  for (int y = 0; y < LM_ART_EDGE; y++) {
    for (int x = 0; x < LM_ART_EDGE; x++) {
      uint8_t *pixel = out + (y * LM_ART_EDGE + x) * 4;
      if (side < LM_ART_EDGE) {
        const uint8_t *src = rgb + ((y0 + y * side / LM_ART_EDGE) * width + x0 + x * side / LM_ART_EDGE) * 3;
        pixel[0] = src[0]; pixel[1] = src[1]; pixel[2] = src[2];
      } else {
        int sx0 = x * side / LM_ART_EDGE, sx1 = (x + 1) * side / LM_ART_EDGE;
        int sy0 = y * side / LM_ART_EDGE, sy1 = (y + 1) * side / LM_ART_EDGE;
        uint32_t sum[3] = {0, 0, 0}, count = (uint32_t)((sx1 - sx0) * (sy1 - sy0));
        for (int sy = sy0; sy < sy1; sy++) {
          const uint8_t *row = rgb + ((y0 + sy) * width + x0) * 3;
          for (int sx = sx0; sx < sx1; sx++) { sum[0] += row[sx * 3]; sum[1] += row[sx * 3 + 1]; sum[2] += row[sx * 3 + 2]; }
        }
        for (int c = 0; c < 3; c++) pixel[c] = (uint8_t)((sum[c] + count / 2) / count);
      }
      pixel[3] = 255;
    }
  }
}

int lm_art_decode(const uint8_t *data, size_t length, uint8_t *out) {
  int width, height, channels;
  if (length == 0 || length > (size_t)LM_ART_MAX_BYTES) return 0;
  if (!stbi_info_from_memory(data, (int)length, &width, &height, &channels)) return 0;
  if (width <= 0 || height <= 0 || width > LM_ART_MAX_DIM || height > LM_ART_MAX_DIM) return 0;
  uint8_t *rgb = stbi_load_from_memory(data, (int)length, &width, &height, &channels, 3);
  if (!rgb) return 0;
  lm_art_fit(rgb, width, height, out);
  stbi_image_free(rgb);
  return 1;
}
