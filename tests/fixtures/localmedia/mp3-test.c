#include "../../../hosts/3ds/src/localmedia_mp3.h"
#include "check.h"

static long file_size(FILE *f) { fseek(f, 0, SEEK_END); return ftell(f); }

static LmStream probe(const char *path) {
  LmStream s;
  FILE *f = fopen(path, "rb");
  long size = file_size(f);
  CHECK(lm_stream_probe(f, 0, size, &s));
  fclose(f);
  return s;
}

/* Writes n CBR MPEG-1 128 kbps 44.1 kHz frames (417 bytes, no padding) after `lead` junk
 * bytes; with false_sync, the junk holds a valid-looking header at byte 10. */
static void write_cbr(const char *path, int lead, int n, int false_sync) {
  FILE *f = fopen(path, "wb");
  uint8_t junk[1024];
  memset(junk, 0x11, sizeof junk);
  if (false_sync) { junk[10] = 0xff; junk[11] = 0xfb; junk[12] = 0x90; junk[13] = 0x64; }
  fwrite(junk, 1, (size_t)lead, f);
  uint8_t frame[417] = {0xff, 0xfb, 0x90, 0x64};
  for (int i = 4; i < 417; i++) frame[i] = 0x22;
  for (int i = 0; i < n; i++) fwrite(frame, 1, sizeof frame, f);
  fclose(f);
}

int main(void) {
  LmFrame f;
  /* Every MPEG version x sample rate x bitrate entry. */
  static const int rates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
  static const int kbps[2][14] = {{32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
                                  {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
  static const int version_bits[3] = {3, 2, 0};
  for (int v = 0; v < 3; v++)
    for (int r = 0; r < 3; r++)
      for (int b = 1; b <= 14; b++)
        for (int pad = 0; pad <= 1; pad++) {
          uint8_t h[4] = {0xff, (uint8_t)(0xe0 | version_bits[v] << 3 | 1 << 1 | 1), (uint8_t)(b << 4 | r << 2 | pad << 1), 0x44};
          CHECK(lm_frame_parse(h, &f));
          int rate = rates[v][r], bitrate = kbps[v ? 1 : 0][b - 1];
          CHECK_INT(f.sample_rate, rate);
          CHECK_INT(f.bitrate_kbps, bitrate);
          CHECK_INT(f.samples, v == 0 ? 1152 : 576);
          CHECK_INT(f.bytes, (v == 0 ? 144 : 72) * bitrate * 1000 / rate + pad);
          CHECK_INT(f.mpeg, v == 0 ? 1 : v == 1 ? 2 : 25);
          CHECK_INT(f.channels, 2);
        }
  uint8_t mono[4] = {0xff, 0xfb, 0x90, 0xc4};
  CHECK(lm_frame_parse(mono, &f) && f.channels == 1 && f.bytes == 417);
  uint8_t bad[][4] = {
    {0xff, 0xfb, 0x00, 0x00}, /* free format */
    {0xff, 0xfb, 0xf0, 0x00}, /* bitrate 15 */
    {0xff, 0xfb, 0x9c, 0x00}, /* reserved sample rate */
    {0xff, 0xeb, 0x90, 0x00}, /* reserved version */
    {0xff, 0xfd, 0x90, 0x00}, /* Layer II */
    {0xff, 0xff, 0x90, 0x00}, /* Layer I */
    {0xfe, 0xfb, 0x90, 0x00}, /* no sync */
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!lm_frame_parse(bad[i], &f));

  /* Encoder fixtures: Info/Xing counts are exact; headerless files are estimated. */
  LmStream s = probe("cbr-info.mp3");
  CHECK(s.exact); CHECK_INT(s.duration_ms, 1044); CHECK_INT(s.first.bitrate_kbps, 128);
  CHECK_INT(s.data_start, 417); /* the Info frame is not audio */
  s = probe("cbr-plain.mp3");
  CHECK(!s.exact); CHECK(s.duration_ms >= 1030 && s.duration_ms <= 1050); CHECK_INT(s.data_start, 0);
  s = probe("vbr-xing.mp3");
  CHECK(s.exact && s.has_toc); CHECK_INT(s.duration_ms, 1044);
  s = probe("vbr-plain.mp3");
  CHECK(!s.exact && s.duration_ms > 0);
  s = probe("mono22.mp3");
  CHECK(s.exact); CHECK_INT(s.first.mpeg, 2); CHECK_INT(s.first.channels, 1); CHECK_INT(s.first.sample_rate, 22050);
  CHECK_INT(s.duration_ms, 1071);
  { /* Probing starts after the ID3v2 tag. */
    FILE *t = fopen("tagged-v23.mp3", "rb");
    long size = file_size(t);
    CHECK(lm_stream_probe(t, 2786, size, &s)); /* 10-byte header + 2776-byte tag */
    CHECK(s.exact); CHECK_INT(s.duration_ms, 1044);
    fclose(t);
  }

  /* A false sync in junk before the audio is rejected by the two-frame check. */
  const char *tmp = "mp3-test.tmp";
  write_cbr(tmp, 300, 20, 1);
  s = probe(tmp);
  CHECK_INT(s.data_start, 300);
  CHECK(!s.exact);
  CHECK_INT(s.duration_ms, 20 * 417 * 8 / 128); /* bytes * 8 / kbps */
  /* Linear seek: halfway lands halfway through the audio bytes; resync finds the next frame. */
  long offset = lm_stream_seek_offset(&s, s.duration_ms / 2);
  CHECK(offset >= 300 + 417 * 10 - 417 && offset <= 300 + 417 * 10 + 417);
  FILE *t = fopen(tmp, "rb");
  long size = file_size(t);
  long at = lm_stream_resync(t, offset, size, &s.first);
  CHECK(at >= offset && (at - 300) % 417 == 0);
  CHECK_INT(lm_stream_resync(t, 300 + 417 * 19 + 1, size, &s.first), -1); /* inside the last frame: nothing follows */
  CHECK_INT(lm_stream_resync(t, 300 + 417 * 19, size, &s.first), 300 + 417 * 19); /* the last frame ends at EOF */
  fclose(t);
  CHECK_INT(lm_stream_seek_offset(&s, 0), 300);
  CHECK_INT(lm_stream_seek_offset(&s, s.duration_ms + 5000), size);
  /* No frames at all. */
  t = fopen(tmp, "wb");
  for (int i = 0; i < 70000; i++) fputc(0x11, t);
  fclose(t);
  t = fopen(tmp, "rb");
  CHECK(!lm_stream_probe(t, 0, 70000, &s));
  CHECK_INT(s.duration_ms, 0);
  fclose(t);
  remove(tmp);

  /* Xing TOC: a linear TOC maps halfway to halfway; a skewed one moves the offset. */
  LmStream toc = {0};
  toc.toc_base = 1000; toc.data_start = 1417; toc.data_end = 101000; toc.duration_ms = 100000; toc.has_toc = 1; toc.toc_bytes = 100000;
  for (int i = 0; i < 100; i++) toc.toc[i] = (uint8_t)(i * 256 / 100);
  CHECK_INT(lm_stream_seek_offset(&toc, 50000), 1000 + 50000);
  toc.toc[50] = 200;
  CHECK_INT(lm_stream_seek_offset(&toc, 50000), 1000 + (long)(200.0 / 256.0 * 100000));
  CHECK(lm_stream_seek_offset(&toc, 99999) <= toc.data_end);
  CHECK_INT(lm_stream_seek_offset(&toc, 1), toc.data_start); /* never inside the Xing frame */

  /* VBRI: frame count at offset 36 + 14. */
  {
    FILE *v = fopen(tmp, "wb");
    uint8_t frame[417] = {0xff, 0xfb, 0x90, 0x64};
    memcpy(frame + 36, "VBRI", 4);
    frame[36 + 14] = 0; frame[36 + 15] = 0; frame[36 + 16] = 0; frame[36 + 17] = 100;
    fwrite(frame, 1, sizeof frame, v);
    memset(frame + 4, 0x22, sizeof frame - 4);
    for (int i = 0; i < 5; i++) fwrite(frame, 1, sizeof frame, v);
    fclose(v);
    s = probe(tmp);
    CHECK(s.exact); CHECK_INT(s.duration_ms, 100 * 1152 * 1000 / 44100); CHECK_INT(s.data_start, 417);
    remove(tmp);
  }
  CHECK_DONE("localmedia mp3");
}
