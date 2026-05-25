#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "hal.h"
#include "game.h"

#define TILES_PER_TERCIO 256u

#define VRAM_PAT_BASE   0x0000u
#define VRAM_COL_BASE   0x2000u
#define VRAM_NAME_BASE  0x1800u
#define VRAM_THIRD_SIZE 0x0800u

static uint8_t g_tiles[TILES_PER_TERCIO][16];

#define ROM_ADDR(a)  ((uint16_t)(a))

static const struct {
    uint16_t vram_idx;
    uint16_t rom_addr;
    uint8_t  count;
} TILE_MAP[] = {
    { 0x00, ROM_ADDR(0x8956), 1 },    /* BLANK */
    { 0x01, ROM_ADDR(0x8796), 26 },   /* font A-Z (also used as WALLS at 0x59-0x72) */
    { 0x0D, ROM_ADDR(0x9A76), 1 },    /* DOOR */
    { 0x0E, ROM_ADDR(0x84B6), 28 },   /* BG3 (0x0E-0x29) */
    { 0x2A, ROM_ADDR(0x7E96),  7 },   /* LOGO row 0 (file 0x3E96) */
    { 0x31, ROM_ADDR(0x7F06),  7 },   /* LOGO row 1 (file 0x3F06) */
    { 0x38, ROM_ADDR(0x7F76),  7 },   /* LOGO row 2 (file 0x3F76) */
    { 0x3F, ROM_ADDR(0x7FE6),  7 },   /* LOGO row 3 (file 0x3FE6) */
    { 0x46, ROM_ADDR(0x9A86), 1 },    /* KEY */
    { 0x47, ROM_ADDR(0x86F6), 10 },   /* ANIM_BG (0x47-0x50) */
    { 0x51, ROM_ADDR(0x8676), 4 },    /* BG4_A (0x51-0x54) */
    { 0x55, ROM_ADDR(0x86B6), 2 },    /* BG4_B (0x55-0x56) */
    { 0x57, ROM_ADDR(0x86D6), 2 },    /* BG4_C (0x57-0x58) */
    { 0x59, ROM_ADDR(0x8796), 26 },   /* WALLS (0x59-0x72) — same ROM data as font */
    { 0x73, ROM_ADDR(0x89C6), 2 },    /* wall tiles 0x73-0x74 */
    { 0x75, ROM_ADDR(0x8966), 2 },    /* wall tiles 0x75-0x76 */
    { 0x77, ROM_ADDR(0x8096), 66 },   /* LOGO body (0x77-0xB8) — tile #5+ from ROM 0x8056 */
};
#define N_MAPS (sizeof(TILE_MAP)/sizeof(TILE_MAP[0]))

/* Lee tile de g_tiles[tercio * 256 + vram_idx] y lo escribe a VRAM en dst_idx
 * third = -1 → los 3 tercios,  third = 0/1/2 → solo ese tercio */
static void write_tile_to_vdp(uint8_t src_idx, uint8_t dst_idx, int third)
{
    if (src_idx >= TILES_PER_TERCIO) return;

    int t0 = (third < 0) ? 0 : third;
    int t1 = (third < 0) ? 3 : third + 1;

    for (int t = t0; t < t1; t++) {
        uint16_t pat = (uint16_t)(VRAM_PAT_BASE + (uint16_t)t * VRAM_THIRD_SIZE + dst_idx * 8u);
        uint16_t col = (uint16_t)(VRAM_COL_BASE + (uint16_t)t * VRAM_THIRD_SIZE + dst_idx * 8u);
        for (uint8_t r = 0u; r < 8u; r++) {
            hal_vdp_write_vram((uint16_t)(pat + r), g_tiles[src_idx][r * 2u]);
            hal_vdp_write_vram((uint16_t)(col + r), g_tiles[src_idx][r * 2u + 1u]);
        }
    }
}

/* ==========================================================================
 * GENERAR TILES DE LLAVES (0x01-0x0C) desde 2 patrones base en ROM
 * La ROM solo contiene 2 tiles de llave en 0x9A56/0x9A66 (dark blue).
 * La rutina Z80 original genera 5 copias adicionales cambiando el ink.
 * ========================================================================== */
static const uint8_t KEY_INKS[6] = { 0x4, 0x6, 0xD, 0x2, 0x7, 0xA };

static void tiles_load_keys(void)
{
    uint32_t base_off = 0x5A56u;
    if (base_off + 32u > g_rom_size) return;

    for (int k = 0; k < 6; k++) {
        uint8_t vram_idx = (uint8_t)(0x01u + (uint8_t)k * 2u);
        uint8_t ink = KEY_INKS[k];

        for (int t = 0; t < 2; t++) {
            uint8_t idx = (uint8_t)(vram_idx + t);
            if (idx >= TILES_PER_TERCIO) break;
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
    for (uint16_t i = 0; i < TILES_PER_TERCIO; i++)
        for (int r = 0; r < 8; r++)
            g_tiles[i][r * 2u + 1u] = 0x11u;

    /* Cargar TILE_MAP solo al tercio 0 (default) */
    for (uint8_t m = 0u; m < (uint8_t)N_MAPS; m++) {
        uint16_t idx  = TILE_MAP[m].vram_idx;
        uint32_t foff = (uint32_t)TILE_MAP[m].rom_addr - 0x4000u;
        for (uint8_t t = 0u; t < TILE_MAP[m].count; t++) {
            if ((uint16_t)(idx + t) >= TILES_PER_TERCIO) break;
            if (foff + (uint32_t)t * 16u + 16u > rom_size) break;
            uint16_t gi = (uint16_t)(idx + t);
            memcpy(g_tiles[gi], rom_data + foff + (uint32_t)t * 16u, 16);
        }
    }

    /* Generar 12 tiles de llaves (0x01-0x0C) */
    tiles_load_keys();

    /* Limpiar VRAM y escribir solo tercio 0 desde g_tiles */
    hal_vdp_fill_vram(VRAM_PAT_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_COL_BASE,  0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_NAME_BASE, 0x00u, 768u);

    for (uint16_t i = 0u; i < TILES_PER_TERCIO; i++)
        write_tile_to_vdp((uint8_t)i, (uint8_t)i, 0);
}

void tiles_reload_all(void)
{
    for (uint16_t i = 0u; i < TILES_PER_TERCIO; i++)
        write_tile_to_vdp((uint8_t)i, (uint8_t)i, 0);
}

void tiles_reload_walls_and_anim(void)
{
    for (uint8_t i = 0u; i < 26u; i++) {
        uint8_t idx = (uint8_t)(0x59u + i);
        if (idx < TILES_PER_TERCIO)
            write_tile_to_vdp(idx, idx, -1);
    }
    for (uint8_t i = 0u; i < 10u; i++) {
        uint8_t idx = (uint8_t)(0x47u + i);
        if (idx < TILES_PER_TERCIO)
            write_tile_to_vdp(idx, idx, -1);
    }
}

void tiles_write_range_to_thirds(uint8_t start_idx, uint8_t count, int third)
{
    for (uint8_t i = 0u; i < count; i++) {
        uint8_t idx = (uint8_t)(start_idx + i);
        if (idx < TILES_PER_TERCIO)
            write_tile_to_vdp(idx, idx, third);
    }
}

void tiles_animate(uint8_t frame_counter)
{
    if ((frame_counter & 0x03u) != 0u) return;
    uint8_t slot     = (uint8_t)((frame_counter >> 2u) % 10u);
    uint8_t next_src = (uint8_t)(0x47u + ((slot + 1u) % 10u));
    uint8_t dst      = (uint8_t)(0x47u + slot);
    if (next_src < TILES_PER_TERCIO)
        write_tile_to_vdp(next_src, dst, -1);
}

/* Escribe datos raw desde ROM a g_tiles y VRAM para un rango de tiles.
   rom_file_off = offset directo en el buffer g_rom (0-based).
   Si es >= 0x4000, se resta 0x4000 (convierte ROM address a file offset). */
void tiles_rom_to_vram(uint32_t rom_file_off, uint8_t vram_start,
                       uint8_t count, int first_tercio)
{
    if (rom_file_off >= 0x4000u) rom_file_off -= 0x4000u;
    if (rom_file_off + (uint32_t)count * 16u > g_rom_size) return;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t  tile_idx = (uint8_t)(vram_start + i);
        uint32_t off      = rom_file_off + (uint32_t)i * 16u;
        /* Actualizar g_tiles[0..255] */
        for (uint8_t row = 0; row < 8u; row++) {
            g_tiles[tile_idx][row * 2u]     = g_rom[off + (uint32_t)row * 2u];
            g_tiles[tile_idx][row * 2u + 1u] = g_rom[off + (uint32_t)row * 2u + 1u];
        }
        /* Escribir a VRAM en los tercios indicados */
        for (int t = first_tercio; t < 3; t++) {
            uint16_t pat = (uint16_t)(VRAM_PAT_BASE + (uint16_t)t * VRAM_THIRD_SIZE + tile_idx * 8u);
            uint16_t col = (uint16_t)(VRAM_COL_BASE + (uint16_t)t * VRAM_THIRD_SIZE + tile_idx * 8u);
            for (uint8_t row = 0; row < 8u; row++) {
                hal_vdp_write_vram((uint16_t)(pat + row), g_rom[off + (uint32_t)row * 2u]);
                hal_vdp_write_vram((uint16_t)(col + row), g_rom[off + (uint32_t)row * 2u + 1u]);
            }
        }
    }
}

/* Escribe datos raw desde ROM a VRAM sin modificar g_tiles.
   rom_file_off = offset directo en g_rom (0-based, o ≥ 0x4000 = ROM address).
   first_tercio = 0 (todos), 1 (tercios 1-2), 2 (solo tercio 2). */
void tiles_vram_from_rom(uint32_t rom_file_off, uint8_t vram_start,
                         uint8_t count, int first_tercio)
{
    if (rom_file_off >= 0x4000u) rom_file_off -= 0x4000u;
    if (rom_file_off + (uint32_t)count * 16u > g_rom_size) return;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t  tile_idx = (uint8_t)(vram_start + i);
        uint32_t off      = rom_file_off + (uint32_t)i * 16u;
        for (int t = first_tercio; t < 3; t++) {
            uint16_t pat = (uint16_t)(VRAM_PAT_BASE + (uint16_t)t * VRAM_THIRD_SIZE + tile_idx * 8u);
            uint16_t col = (uint16_t)(VRAM_COL_BASE + (uint16_t)t * VRAM_THIRD_SIZE + tile_idx * 8u);
            for (uint8_t row = 0; row < 8u; row++) {
                hal_vdp_write_vram((uint16_t)(pat + row), g_rom[off + (uint32_t)row * 2u]);
                hal_vdp_write_vram((uint16_t)(col + row), g_rom[off + (uint32_t)row * 2u + 1u]);
            }
        }
    }
}

void tiles_dump_vram(const char *label)
{
    char fname[64];
    uint8_t vram[0x4000];
    uint8_t tiles[3 * 256 * 16];
    FILE *fp;

    /* Dump 1: raw VRAM 0x0000-0x3FFF */
    snprintf(fname, sizeof(fname), "vram_%s.bin", label);
    hal_vdp_copy_from_vram(0x0000u, vram, 0x4000);
    fp = fopen(fname, "wb");
    if (fp) {
        fwrite(vram, 1, 0x4000, fp);
        fclose(fp);
    }

    /* Dump 2: tile data interleaved (pat/col), 16 bytes per tile, 3 thirds */
    for (int t = 0; t < 3; t++) {
        uint16_t pat_base = (uint16_t)(t * 0x0800u);
        uint16_t col_base = (uint16_t)(0x2000u + t * 0x0800u);
        for (uint16_t i = 0; i < 256; i++) {
            uint32_t dst = (uint32_t)t * 256 * 16 + i * 16;
            for (int r = 0; r < 8; r++) {
                tiles[dst + r * 2u]     = vram[pat_base + i * 8u + r];
                tiles[dst + r * 2u + 1u] = vram[col_base + i * 8u + r];
            }
        }
    }
    snprintf(fname, sizeof(fname), "tiles_%s.bin", label);
    fp = fopen(fname, "wb");
    if (fp) {
        fwrite(tiles, 1, sizeof(tiles), fp);
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
