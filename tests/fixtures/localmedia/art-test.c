#include "../../../hosts/3ds/src/localmedia_art.h"
#include "../../../hosts/3ds/src/localmedia_tags.h"
#include "check.h"

#include <stdlib.h>

static uint8_t out[LM_ART_PIXELS_BYTES];

static const uint8_t *px(int x, int y) { return out + (y * LM_ART_EDGE + x) * 4; }

/* Reads a fixture's embedded picture through the tag reader and decodes it. */
static int decode_embedded(const char *path) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  LmTags tags;
  lm_tags_read(f, size, &tags);
  uint8_t *data;
  size_t length = lm_art_read(f, tags.art_offset, tags.art_raw_bytes, tags.art_unsync, &data);
  fclose(f);
  int ok = length > 0 && lm_art_decode(data, length, out);
  free(data);
  return ok;
}

static size_t slurp(const char *path, uint8_t **data) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  *data = malloc((size_t)size);
  size_t got = fread(*data, 1, (size_t)size, f);
  fclose(f);
  return got;
}

int main(void) {
  /* Box filter: a 256x256 checkerboard of 1-pixel black/white averages to mid-grey. */
  uint8_t *rgb = malloc(256 * 256 * 3);
  for (int i = 0; i < 256 * 256; i++) { uint8_t v = ((i % 256) + (i / 256)) % 2 ? 255 : 0; rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v; }
  lm_art_fit(rgb, 256, 256, out);
  CHECK_INT(px(0, 0)[0], 128); CHECK_INT(px(127, 127)[1], 128); CHECK_INT(px(5, 9)[3], 255);

  /* Wide: 384x128 with red | green | blue thirds keeps only the green centre. */
  rgb = realloc(rgb, 384 * 128 * 3);
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 384; x++) { uint8_t *p = rgb + (y * 384 + x) * 3; p[0] = x < 128 ? 255 : 0; p[1] = x >= 128 && x < 256 ? 255 : 0; p[2] = x >= 256 ? 255 : 0; }
  lm_art_fit(rgb, 384, 128, out);
  CHECK(px(0, 0)[1] == 255 && px(0, 0)[0] == 0); CHECK(px(127, 64)[1] == 255 && px(127, 64)[2] == 0);

  /* Tall: 128x384 with top/middle/bottom thirds keeps the middle. */
  for (int y = 0; y < 384; y++)
    for (int x = 0; x < 128; x++) { uint8_t *p = rgb + (y * 128 + x) * 3; p[0] = y < 128 ? 255 : 0; p[1] = y >= 128 && y < 256 ? 255 : 0; p[2] = y >= 256 ? 255 : 0; }
  lm_art_fit(rgb, 128, 384, out);
  CHECK(px(64, 0)[1] == 255 && px(64, 127)[1] == 255 && px(64, 127)[2] == 0);

  /* Tiny: 2x2 grows by nearest neighbour into four 64x64 quadrants. */
  uint8_t tiny[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  lm_art_fit(tiny, 2, 2, out);
  CHECK(px(0, 0)[0] == 255 && px(63, 63)[0] == 255 && px(64, 0)[1] == 255 && px(0, 64)[2] == 255 && px(127, 127)[0] == 255 && px(127, 127)[2] == 255);

  /* A 200x150 non-square downscale: crop to 150, box sizes 1 or 2 source pixels. */
  rgb = realloc(rgb, 200 * 150 * 3);
  for (int i = 0; i < 200 * 150 * 3; i++) rgb[i] = 77;
  lm_art_fit(rgb, 200, 150, out);
  CHECK_INT(px(100, 100)[0], 77);
  free(rgb);

  /* Embedded JPEG (96x64, lame v2.3) and PNG (40x40, ffmpeg v2.4) decode through the tag offsets. */
  CHECK(decode_embedded("tagged-v23.mp3"));
  CHECK_INT(px(0, 0)[3], 255);
  CHECK(decode_embedded("tagged-v24.mp3"));
  uint8_t *png;
  size_t png_length = slurp("cover-small.png", &png);
  uint8_t direct[LM_ART_PIXELS_BYTES];
  CHECK(lm_art_decode(png, png_length, direct));
  CHECK(memcmp(direct, out, sizeof direct) == 0);

  /* Over-limit dimensions are refused before decoding: a PNG IHDR claiming 2000x10. */
  uint8_t big[33];
  memcpy(big, png, 33);
  big[16] = 0; big[17] = 0; big[18] = 0x07; big[19] = 0xd0;
  CHECK(!lm_art_decode(big, 33, out));
  /* Corrupt and truncated data. */
  CHECK(!lm_art_decode(png, 20, out));
  uint8_t junk[64];
  memset(junk, 0x5a, sizeof junk);
  CHECK(!lm_art_decode(junk, sizeof junk, out));
  CHECK(!lm_art_decode(png, 0, out));
  free(png);

  /* lm_art_read: unsynchronised bytes come back clean; oversize and short spans fail. */
  const char *tmp = "art-test.tmp";
  FILE *f = fopen(tmp, "wb");
  uint8_t raw[] = {9, 9, 0xff, 0x00, 0xd8, 0xff, 0x00, 0xe0, 1};
  fwrite(raw, 1, sizeof raw, f);
  fclose(f);
  f = fopen(tmp, "rb");
  uint8_t *data;
  size_t length = lm_art_read(f, 2, 7, 1, &data);
  CHECK_INT(length, 5);
  CHECK(data && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff && data[3] == 0xe0 && data[4] == 1);
  free(data);
  CHECK_INT(lm_art_read(f, 2, 50, 0, &data), 0); /* past EOF */
  CHECK(data == NULL);
  CHECK_INT(lm_art_read(f, 0, LM_ART_MAX_BYTES + 1, 0, &data), 0);
  CHECK_INT(lm_art_read(f, 0, 0, 0, &data), 0);
  fclose(f);
  remove(tmp);
  CHECK_DONE("localmedia art");
}
