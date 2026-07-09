#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "hal.h"
#include "screen.h"
#include "game.h"

#define VRAM_PAT_BASE   0x0000u
#define VRAM_COL_BASE   0x2000u
#define VRAM_NAME_BASE  0x1800u

static uint8_t g_tiles[256][16];

#define ROM_ADDR(a)  ((uint16_t)(a))

static const struct {
    uint16_t vram_idx;
    uint16_t rom_addr;
    uint8_t  count;
} TILE_MAP[] = {
    { 0x00, ROM_ADDR(0x8956), 1 },    /* BLANK */
    { 0x01, ROM_ADDR(0x9A56), 2 },    /* KEY (dark blue)*/
    { 0x0D, ROM_ADDR(0x9A76), 1 },    /* HEART */
    { 0x0E, ROM_ADDR(0x84B6), 28 },   /* MAP Area 7x4, rows 0-3, BG3 (tiles 0x0E-0x29) (file 0x44B6-0x4675) */
    { 0x2A, ROM_ADDR(0x7E96), 28 },   /* LOGO Area 7x4, rows 0-3 (tiles 0x2A-0x45) (file 0x3E96-0x4055) */
    { 0x46, ROM_ADDR(0x9A86),  1 },   /* Vertical Separator */
    { 0x47, ROM_ADDR(0x86F6), 10 },   /* Digits 0-9 - ANIM_BG (0x47-0x50) */
    { 0x51, ROM_ADDR(0x8676), 4 },    /* "Hi""SCORE" - BG4_A (0x51-0x54) */
    { 0x55, ROM_ADDR(0x86B6), 2 },    /* "Key" - BG4_B (0x55-0x56) */
    { 0x57, ROM_ADDR(0x86D6), 2 },    /* "Life" - BG4_C (0x57-0x58) */
    { 0x59, ROM_ADDR(0x8796), 26 },   /* Font A-Z */
    { 0x73, ROM_ADDR(0x8056), 70 },   /* LOGO Body 14x5, rows 0-4 (tiles 0x73-0xB8) (file 0x4056-0x44B5) */
//  { 0x73, ROM_ADDR(0x89C6), 2 },    /* wall tiles 0x73-0x74 */
//  { 0x75, ROM_ADDR(0x8966), 2 },    /* wall tiles 0x75-0x76 */
};
#define N_MAPS (sizeof(TILE_MAP)/sizeof(TILE_MAP[0]))

static void write_tile_to_vdp(uint8_t src_idx, uint16_t dst_idx)
{
    uint16_t pat = (uint16_t)(VRAM_PAT_BASE + dst_idx * 8u);
    uint16_t col = (uint16_t)(VRAM_COL_BASE + dst_idx * 8u);
    for (uint8_t r = 0u; r < 8u; r++) {
        hal_vdp_write_vram((uint16_t)(pat + r), g_tiles[src_idx][r * 2u]);
        hal_vdp_write_vram((uint16_t)(col + r), g_tiles[src_idx][r * 2u + 1u]);
    }
}

/* ==========================================================================
 * GENERAR TILES DE LLAVES (0x01-0x0C) desde 2 patrones base en ROM
 * La ROM solo contiene 2 tiles de llave en 0x9A56/0x9A66 (dark blue).
 * La rutina Z80 original genera 5 copias adicionales cambiando el ink.
 * ========================================================================== */
static const uint8_t KEY_INKS[6] = { 0x4, 0x8, 0xD, 0x2, 0x7, 0xA };

static void tiles_load_keys(void)
{
    uint32_t base_off = 0x5A56u;
    if (base_off + 32u > g_rom_size) return;

    for (int k = 0; k < 6; k++) {
        uint8_t vram_idx = (uint8_t)(0x01u + (uint8_t)k * 2u);
        uint8_t ink = KEY_INKS[k];

        for (int t = 0; t < 2; t++) {
            uint8_t idx = (uint8_t)(vram_idx + t);
            uint32_t src = base_off + (uint32_t)t * 16u;

            for (int r = 0; r < 8; r++) {
                g_tiles[idx][r * 2u]     = g_rom[src + (uint32_t)r * 2u];
                g_tiles[idx][r * 2u + 1u] = (uint8_t)((ink << 4u) | (g_rom[src + (uint32_t)r * 2u + 1u] & 0x0Fu));
            }
        }
    }
}

void tiles_load_from_rom(const uint8_t *rom_data, uint32_t rom_size)
{
    g_rom      = rom_data;
    g_rom_size = rom_size;

    memset(g_tiles, 0, sizeof(g_tiles));
    for (uint16_t i = 0; i < 256; i++)
        for (int r = 0; r < 8; r++)
            g_tiles[i][r * 2u + 1u] = 0x11u;

    for (uint8_t m = 0u; m < (uint8_t)N_MAPS; m++) {
        uint16_t idx  = TILE_MAP[m].vram_idx;
        uint32_t foff = (uint32_t)TILE_MAP[m].rom_addr - 0x4000u;
        for (uint8_t t = 0u; t < TILE_MAP[m].count; t++) {
            if (foff + (uint32_t)t * 16u + 16u > rom_size) break;
            uint16_t gi = (uint16_t)(idx + t);
            memcpy(g_tiles[gi], rom_data + foff + (uint32_t)t * 16u, 16);
        }
    }

    tiles_load_keys();

    hal_vdp_fill_vram(VRAM_PAT_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_COL_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_NAME_BASE, 0x00u, 768u);

    for (uint16_t third = 0u; third < 3u; third++)
        for (uint16_t i = 0u; i < 256u; i++)
            write_tile_to_vdp((uint8_t)i, third * 256u + i);
}

void tiles_reload_all(void)
{
    for (uint16_t third = 0u; third < 3u; third++)
        for (uint16_t i = 0u; i < 256u; i++)
            write_tile_to_vdp((uint8_t)i, third * 256u + i);
}

void tiles_reload_walls_and_anim(void)
{
    for (uint8_t i = 0u; i < 26u; i++)
        write_tile_to_vdp((uint8_t)(0x59u + i), (uint16_t)(0x59u + i));
    for (uint8_t i = 0u; i < 10u; i++)
        write_tile_to_vdp((uint8_t)(0x47u + i), (uint16_t)(0x47u + i));
}

void tiles_load_walls_and_anim(uint16_t vram_idx)
{
    /* WALLS (28 tiles) @ ROM 0x8796 → vram_idx, igual que sub_4E91 */
    tiles_rom_to_vram(0x8796u, vram_idx, 28u);
    /* ANIM_BG (10 tiles) @ ROM 0x86F6 → vram_idx + 28 */
    tiles_rom_to_vram(0x86F6u, (uint16_t)(vram_idx + 28u), 10u);
}

/* sub_549D: copia `count` tiles de solo patrones (8 bytes/tile) desde
 * ROM a VRAM, color fijo `color`. Los datos en ROM son raw pattern bytes,
 * NO interleaved (a diferencia de tiles_rom_to_vram que lee 16 bytes/tile).
 * Las escrituras van a g_bg_tiles via hal_vdp_write_vram. */
void tiles_load_patterns(uint32_t rom_off, uint16_t vram_idx,
                         uint8_t count, uint8_t color)
{
    for (uint8_t i = 0; i < count; i++) {
        uint16_t vi = (uint16_t)(vram_idx + i);
        uint32_t off = rom_off + (uint32_t)i * 8u;
        if (off + 8u > g_rom_size) break;
        uint16_t pat = (uint16_t)(VRAM_PAT_BASE + vi * 8u);
        uint16_t col = (uint16_t)(VRAM_COL_BASE + vi * 8u);
        for (uint8_t row = 0; row < 8u; row++) {
            hal_vdp_write_vram((uint16_t)(pat + row), g_rom[off + row]);
            hal_vdp_write_vram((uint16_t)(col + row), color);
        }
    }
}

void tiles_animate(uint8_t frame_counter)
{
    if ((frame_counter & 0x03u) != 0u) return;
    uint8_t slot     = (uint8_t)((frame_counter >> 2u) % 10u);
    uint8_t next_src = (uint8_t)(0x47u + ((slot + 1u) % 10u));
    uint8_t dst      = (uint8_t)(0x47u + slot);
    write_tile_to_vdp(next_src, dst);
}

//R La mas parecida a sub_64AB
void tiles_rom_to_vram(uint32_t rom_file_off, uint16_t vram_idx,
                       uint8_t count)
{
    if (rom_file_off >= 0x4000u) rom_file_off -= 0x4000u;
    if (rom_file_off + (uint32_t)count * 16u > g_rom_size) return;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t vi = (uint16_t)(vram_idx + i);
        uint32_t off = rom_file_off + (uint32_t)i * 16u;
        /* Caché g_tiles solo para tercio 0 */
        if (vi < 256u) {
            uint8_t ti = (uint8_t)vi;
            for (uint8_t row = 0; row < 8u; row++) {
                g_tiles[ti][row * 2u]      = g_rom[off + (uint32_t)row * 2u];
                g_tiles[ti][row * 2u + 1u] = g_rom[off + (uint32_t)row * 2u + 1u];
            }
        }
        uint16_t pat = (uint16_t)(VRAM_PAT_BASE + vi * 8u);
        uint16_t col = (uint16_t)(VRAM_COL_BASE + vi * 8u);
        for (uint8_t row = 0; row < 8u; row++) {
            hal_vdp_write_vram((uint16_t)(pat + row), g_rom[off + (uint32_t)row * 2u]);
            hal_vdp_write_vram((uint16_t)(col + row), g_rom[off + (uint32_t)row * 2u + 1u]);
        }
    }
}

/* sub_6D5A: mirror pattern byte horizontally (reverse bits) */
static uint8_t mirror_byte(uint8_t b)
{
    uint8_t r = 0u;
    for (uint8_t i = 0u; i < 8u; i++) {
        r = (uint8_t)((r << 1u) | (b & 1u));
        b >>= 1u;
    }
    return r;
}

/* Carga un tile interleaved desde ROM a VRAM, con espejado opcional.
 * mirror=true → reversa bits de la fila de patrón (sub_6D5A).
 * El color se copia sin cambios. */
void tiles_load_interleaved_tile(uint32_t rom_file_off, uint16_t vram_idx,
                                 bool mirror)
{
    if (rom_file_off >= 0x4000u) rom_file_off -= 0x4000u;
    if (rom_file_off + 16u > g_rom_size) return;
    uint32_t off = rom_file_off;
    uint16_t pat = (uint16_t)(VRAM_PAT_BASE + vram_idx * 8u);
    uint16_t col = (uint16_t)(VRAM_COL_BASE + vram_idx * 8u);
    for (uint8_t row = 0u; row < 8u; row++) {
        uint8_t p = g_rom[off + (uint32_t)row * 2u];
        uint8_t c = g_rom[off + (uint32_t)row * 2u + 1u];
        if (mirror) p = mirror_byte(p);
        hal_vdp_write_vram((uint16_t)(pat + row), p);
        hal_vdp_write_vram((uint16_t)(col + row), c);
    }
}

void tiles_vram_from_rom(uint32_t rom_file_off, uint16_t vram_idx,
                          uint8_t count)
{
    if (rom_file_off >= 0x4000u) rom_file_off -= 0x4000u;
    if (rom_file_off + (uint32_t)count * 16u > g_rom_size) return;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t vi = (uint16_t)(vram_idx + i);
        uint32_t off = rom_file_off + (uint32_t)i * 16u;
        uint16_t pat = (uint16_t)(VRAM_PAT_BASE + vi * 8u);
        uint16_t col = (uint16_t)(VRAM_COL_BASE + vi * 8u);
        for (uint8_t row = 0; row < 8u; row++) {
            hal_vdp_write_vram((uint16_t)(pat + row), g_rom[off + (uint32_t)row * 2u]);
            hal_vdp_write_vram((uint16_t)(col + row), g_rom[off + (uint32_t)row * 2u + 1u]);
        }
    }
}

void tiles_dump_vram(const char *label)
{
    char fname[64];
    FILE *fp;
    snprintf(fname, sizeof(fname), "tiles_%s.bin", label);
    fp = fopen(fname, "wb");
    if (fp) {
        fwrite(g_bg_tiles, 1, sizeof(g_bg_tiles), fp);
        fclose(fp);
    }
}

uint8_t tiles_vram_idx_blank(void)        { return 0x00u; }
uint8_t tiles_vram_idx_door(void)         { return 0x0Du; }
uint8_t tiles_vram_idx_bg3(uint8_t n)     { return (uint8_t)(0x0Eu + n); }
uint8_t tiles_vram_idx_key(void)          { return 0x46u; }
uint8_t tiles_vram_idx_anim_bg(uint8_t n) { return (uint8_t)(0x47u + n); }
uint8_t tiles_vram_idx_bg4(uint8_t n)     { return (uint8_t)(0x51u + n); }
uint8_t tiles_vram_idx_wall(uint8_t n)    { return (uint8_t)(0x59u + n); }
uint8_t tiles_vram_idx_space(void)        { return 0x3Fu; }
