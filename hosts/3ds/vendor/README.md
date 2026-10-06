# Vendored single-header libraries

Used by the `media.local` module (`hosts/3ds/src/localmedia_*.c`). Each file is
an unmodified copy at the commit listed; `shasum -a 256` must match.

| File | Source | Commit | SHA-256 | Licence |
|---|---|---|---|---|
| `minimp3.h` | https://github.com/lieff/minimp3 | `ea99364f61c14656440e8d77e9c233ccf3124633` | `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad` | CC0 1.0 (`minimp3.LICENSE`) |
| `minimp3.LICENSE` | same | same | `6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01` | — |
| `stb_image.h` | https://github.com/nothings/stb | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` | MIT or public domain (text at the end of the file) |

`localmedia_player.c` defines `MINIMP3_IMPLEMENTATION`, `MINIMP3_ONLY_MP3` and
`MINIMP3_NO_SIMD`; `localmedia_art.c` defines `STB_IMAGE_IMPLEMENTATION` with
`STBI_ONLY_JPEG`, `STBI_ONLY_PNG` and `STBI_NO_STDIO`.
