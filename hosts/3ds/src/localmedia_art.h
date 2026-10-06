/*
 * media.local cover art: reads an embedded picture span (undoing ID3
 * unsynchronisation), decodes JPEG or PNG with stb_image, centre-crops to a
 * square and scales to LM_ART_EDGE x LM_ART_EDGE RGBA8 (alpha 255).
 *
 * Pure C: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_ART_H
#define POCKETJS_LOCALMEDIA_ART_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LM_ART_EDGE 128
#define LM_ART_PIXELS_BYTES (LM_ART_EDGE * LM_ART_EDGE * 4)
/* Pictures larger than this, compressed, are refused. */
#define LM_ART_MAX_BYTES (2L * 1024 * 1024)
/* Pictures wider or taller than this are refused before decoding. */
#define LM_ART_MAX_DIM 1500

/* Reads raw_bytes from offset (undoing unsynchronisation when unsync) into a malloc'd
 * buffer. Returns its length, 0 on failure or when larger than LM_ART_MAX_BYTES. */
size_t lm_art_read(FILE *file, long offset, long raw_bytes, int unsync, uint8_t **out);

/* Decodes a JPEG or PNG and fits it into out (LM_ART_PIXELS_BYTES). Returns 1 on success. */
int lm_art_decode(const uint8_t *data, size_t length, uint8_t *out);

/* Centre-crops an RGB image to a square and scales it to LM_ART_EDGE: box filter when
 * shrinking, nearest neighbour when growing. */
void lm_art_fit(const uint8_t *rgb, int width, int height, uint8_t *out);

#endif
