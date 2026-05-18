#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "hal.h"
#include "game.h"

#define TILE_COUNT      185
#define VRAM_PAT_BASE   0x0000u
#define VRAM_COL_BASE   0x2000u
#define VRAM_NAME_BASE  0x1800u
#define VRAM_THIRD_SIZE 0x0800u

static uint8_t g_tiles[TILE_COUNT][16];

#define ROM_ADDR(a)  ((uint16_t)(a))

static const struct {
    uint16_t vram_idx;
    uint16_t rom_addr;
    uint8_t  count;
} TILE_MAP[] = {
    { 0x00, ROM_ADDR(0x8956), 1 },    /* BLANK */
    { 0x01, ROM_ADDR(0x9A56), 2 },    /* charset A-B */
    { 0x0D, ROM_ADDR(0x9A76), 1 },    /* DOOR */
    { 0x0E, ROM_ADDR(0x84B6), 28 },   /* BG3 (0x0E-0x29) */
    { 0x27, ROM_ADDR(0x8056), 28 },   /* BG1_MAIN (0x27-0x42) */
    { 0x46, ROM_ADDR(0x9A86), 1 },    /* KEY */
    { 0x47, ROM_ADDR(0x86F6), 10 },   /* ANIM_BG (0x47-0x50) */
    { 0x51, ROM_ADDR(0x8676), 4 },    /* BG4_A (0x51-0x54) */
    { 0x55, ROM_ADDR(0x86B6), 2 },    /* BG4_B (0x55-0x56) */
    { 0x57, ROM_ADDR(0x86D6), 2 },    /* BG4_C (0x57-0x58) */
    { 0x59, ROM_ADDR(0x8796), 26 },   /* WALLS (0x59-0x72) */
    { 0x73, ROM_ADDR(0x89C6), 2 },    /* wall tiles 0x73-0x74 */
    { 0x75, ROM_ADDR(0x8966), 2 },    /* wall tiles 0x75-0x76 */
    { 0x37, ROM_ADDR(0x7F66), 15 },   /* HUD/score (0x37-0x45, overwrites BG0) */
    { 0x77, ROM_ADDR(0x8096), 66 },   /* LOGO body (0x77-0xB8) — tile #5+ from ROM 0x8056 */
};
#define N_MAPS (sizeof(TILE_MAP)/sizeof(TILE_MAP[0]))

static void write_tile_to_vdp(uint8_t src_idx, uint8_t dst_idx, int third)
{
    if (src_idx >= (uint8_t)TILE_COUNT) return;

    int t0 = (third < 0) ? 0 : third;
    int t1 = (third < 0) ? 3 : third + 1;

    for (int t = t0; t < t1; t++) {
        uint16_t pat = (uint16_t)(VRAM_PAT_BASE
                       + (uint16_t)t * VRAM_THIRD_SIZE
                       + (uint16_t)dst_idx * 8u);
        uint16_t col = (uint16_t)(VRAM_COL_BASE
                       + (uint16_t)t * VRAM_THIRD_SIZE
                       + (uint16_t)dst_idx * 8u);
        for (uint8_t r = 0u; r < 8u; r++) {
            hal_vdp_write_vram((uint16_t)(pat + r), g_tiles[src_idx][r * 2u]);
            hal_vdp_write_vram((uint16_t)(col + r), g_tiles[src_idx][r * 2u + 1u]);
        }
    }
}

void tiles_load_from_rom(const uint8_t *rom_data, uint32_t rom_size)
{
    g_rom      = rom_data;
    g_rom_size = rom_size;

    memset(g_tiles, 0, sizeof(g_tiles));
    for (int i = 0; i < TILE_COUNT; i++)
        for (int r = 0; r < 8; r++)
            g_tiles[i][r * 2u + 1u] = 0x11u;

    for (uint8_t m = 0u; m < (uint8_t)N_MAPS; m++) {
        uint16_t idx  = TILE_MAP[m].vram_idx;
        uint32_t foff = (uint32_t)TILE_MAP[m].rom_addr - 0x4000u;
        for (uint8_t t = 0u; t < TILE_MAP[m].count; t++) {
            if ((uint16_t)(idx + t) >= (uint16_t)TILE_COUNT) break;
            if (foff + (uint32_t)t * 16u + 16u > rom_size) break;
            memcpy(g_tiles[idx + t], rom_data + foff + (uint32_t)t * 16u, 16);
        }
    }

    hal_vdp_fill_vram(VRAM_PAT_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_COL_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_NAME_BASE, 0x00u, 768u);

    for (uint8_t i = 0u; i < (uint8_t)TILE_COUNT; i++)
        write_tile_to_vdp(i, i, -1);
}

void tiles_reload_all(void)
{
    for (uint8_t i = 0u; i < (uint8_t)TILE_COUNT; i++)
        write_tile_to_vdp(i, i, -1);
}

void tiles_reload_walls_and_anim(void)
{
    /* WALLS 0x59-0x72: 26 tiles */
    for (uint8_t i = 0u; i < 26u; i++) {
        uint8_t idx = (uint8_t)(0x59u + i);
        if (idx < (uint8_t)TILE_COUNT)
            write_tile_to_vdp(idx, idx, -1);
    }
    /* ANIM_BG 0x47-0x50: 10 tiles */
    for (uint8_t i = 0u; i < 10u; i++) {
        uint8_t idx = (uint8_t)(0x47u + i);
        if (idx < (uint8_t)TILE_COUNT)
            write_tile_to_vdp(idx, idx, -1);
    }
}

void tiles_animate(uint8_t frame_counter)
{
    if ((frame_counter & 0x03u) != 0u) return;
    uint8_t slot     = (uint8_t)((frame_counter >> 2u) % 10u);
    uint8_t next_src = (uint8_t)(0x47u + ((slot + 1u) % 10u));
    uint8_t dst      = (uint8_t)(0x47u + slot);
    if (next_src < (uint8_t)TILE_COUNT)
        write_tile_to_vdp(next_src, dst, -1);
}

/* Escribir un tile de font de 8 bytes (no interleaved) a VRAM y g_tiles */
static void deploy_font_tile(uint8_t vdp_idx, const uint8_t *pat8)
{
    if (vdp_idx >= (uint8_t)TILE_COUNT) return;
    for (int r = 0; r < 8; r++) {
        g_tiles[vdp_idx][r * 2]     = pat8[r];
        g_tiles[vdp_idx][r * 2 + 1] = 0xF1u;
    }
    write_tile_to_vdp(vdp_idx, vdp_idx, -1);
}

void tiles_load_bios_rom(const char *path)
{
    if (!path) return;

    FILE *f = fopen(path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x0800) { fclose(f); return; }

    /* Read MSX1 BIOS charset (256 chars × 8 bytes raw pattern) */
    unsigned char font[2048];
    if (fread(font, 1, sizeof(font), f) != sizeof(font)) { fclose(f); return; }
    fclose(f);

    /* Letters A–Z → tiles 0x01–0x1A (credit tile_base=0x01) */
    for (uint8_t c = 0x41u; c <= 0x5Au; c++)
        deploy_font_tile((uint8_t)(c - 0x41u + 0x01u), &font[c * 8u]);

    /* '[' → tile 0x1B */
    deploy_font_tile(0x1Bu, &font[0x5Bu * 8u]);

    /* Digits 0–9 → tiles 0x1D–0x26 */
    for (uint8_t c = 0x30u; c <= 0x39u; c++)
        deploy_font_tile((uint8_t)(c - 0x30u + 0x1Cu + 0x01u), &font[c * 8u]);
}

uint8_t tiles_vram_idx_blank(void)        { return 0x00u; }
uint8_t tiles_vram_idx_door(void)         { return 0x0Du; }
uint8_t tiles_vram_idx_bg3(uint8_t n)     { return (uint8_t)(0x0Eu + n); }
uint8_t tiles_vram_idx_key(void)          { return 0x46u; }
uint8_t tiles_vram_idx_anim_bg(uint8_t n) { return (uint8_t)(0x47u + n); }
uint8_t tiles_vram_idx_bg4(uint8_t n)     { return (uint8_t)(0x51u + n); }
uint8_t tiles_vram_idx_wall(uint8_t n)    { return (uint8_t)(0x59u + n); }
uint8_t tiles_vram_idx_space(void)        { return 0x3Fu; }
