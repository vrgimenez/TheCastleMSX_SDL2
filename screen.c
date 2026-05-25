#include <stdint.h>
#include <string.h>
#include "screen.h"
#include "hal.h"

uint8_t g_screen_buf[SCREEN_ROWS][SCREEN_COLS];
uint8_t g_bg_tiles[256][16];
uint32_t g_palette[16];

static uint8_t s_border = 0;

void screen_init(void)
{
    memset(g_screen_buf, 0, sizeof(g_screen_buf));
    memset(g_bg_tiles, 0, sizeof(g_bg_tiles));
    for (int i = 0; i < 256; i++)
        for (int r = 0; r < 8; r++)
            g_bg_tiles[i][r * 2 + 1] = 0x11;
    s_border = 0;
}

void screen_clear(void)
{
    memset(g_screen_buf, 0, sizeof(g_screen_buf));
}

void screen_put(uint8_t col, uint8_t row, uint8_t tile)
{
    if (col >= SCREEN_COLS || row >= SCREEN_ROWS) return;
    g_screen_buf[row][col] = tile;
}

void screen_fill(uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t tile)
{
    for (uint8_t r = 0; r < h && (row + r) < SCREEN_ROWS; r++)
        for (uint8_t c = 0; c < w && (col + c) < SCREEN_COLS; c++)
            g_screen_buf[row + r][col + c] = (uint8_t)(tile + r * w + c);
}

void screen_set_border(uint8_t color)
{
    s_border = color & 0x0F;
}

void screen_put_tile(uint32_t *fb, int fb_w,
                     const uint8_t tile[16],
                     int px, int py)
{
    for (int row = 0; row < 8; row++) {
        uint8_t pat   = tile[row * 2];
        uint8_t col   = tile[row * 2 + 1];
        uint8_t ink   = (col >> 4) & 0x0F;
        uint8_t paper = col & 0x0F;
        uint32_t ink_rgba   = g_palette[ink   ? ink   : s_border];
        uint32_t paper_rgba = g_palette[paper ? paper : s_border];
        int py2 = py + row;
        if (py2 < 0 || py2 >= 192) continue;
        for (int bit = 7; bit >= 0; bit--) {
            int px2 = px + (7 - bit);
            if (px2 < 0 || px2 >= 256) continue;
            fb[py2 * fb_w + px2] = (pat & (1u << bit)) ? ink_rgba : paper_rgba;
        }
    }
}

void screen_put_tile_array(uint32_t *fb, int fb_w,
                           const uint8_t *tiles, int cols, int rows,
                           int px, int py)
{
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int src_idx = r * cols + c;
            screen_put_tile(fb, fb_w,
                           tiles + (uint32_t)src_idx * 16,
                           px + c * 8, py + r * 8);
        }
    }
}

void screen_render(uint32_t *fb, int fb_w, int fb_h)
{
    uint32_t border_rgba = g_palette[s_border];

    /* Fill with border */
    for (int i = 0; i < fb_w * fb_h; i++)
        fb[i] = border_rgba;

    /* Render background tiles */
    for (int row = 0; row < SCREEN_ROWS; row++) {
        for (int col = 0; col < SCREEN_COLS; col++) {
            uint8_t idx = g_screen_buf[row][col];
            screen_put_tile(fb, fb_w, g_bg_tiles[idx],
                           col * 8, row * 8);
        }
    }
}
