#pragma once
#ifndef CASTLE_SCREEN_H
#define CASTLE_SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#define SCREEN_COLS 32
#define SCREEN_ROWS 24

extern uint8_t  g_screen_buf[SCREEN_ROWS][SCREEN_COLS];
extern uint8_t  g_bg_tiles[256][16];
extern uint32_t g_palette[16];  /* packed in texture pixel format */

void screen_init(void);
void screen_clear(void);
void screen_put(uint8_t col, uint8_t row, uint8_t tile);
void screen_fill(uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t tile);

/* Render g_screen_buf → g_bg_tiles → pixels to a 32-bit RGBA framebuffer.
   fb_w must be ≥ 256, fb_h must be ≥ 192. Only the MSX area is written. */
void screen_render(uint32_t *fb, int fb_w, int fb_h);

/* Set color for border/pixels when color nibble is 0 */
void screen_set_border(uint8_t color);

/* Render a tile at pixel position (px, py) from raw tile data */
void screen_put_tile(uint32_t *fb, int fb_w,
                     const uint8_t tile[16],
                     int px, int py);

/* Render a tile array (cols × rows) at pixel position (px, py).
   Tiles stored row-major: [row*cols + col][16] */
void screen_put_tile_array(uint32_t *fb, int fb_w,
                           const uint8_t *tiles, int cols, int rows,
                           int px, int py);

#endif
