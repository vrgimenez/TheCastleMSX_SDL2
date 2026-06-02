#include <stdint.h>
#include <string.h>
#include "tiledata.h"

uint8_t g_font[28][TILE_BYTES];
uint8_t g_digits[10][TILE_BYTES];
uint8_t g_title_logo[70][TILE_BYTES];
uint8_t g_hud_logo[28][TILE_BYTES];
uint8_t g_hud_map[28][TILE_BYTES];
uint8_t g_wall_variants[4][TILE_BYTES];
uint8_t g_heart[1][TILE_BYTES];
uint8_t g_door_base[4][TILE_BYTES];
uint8_t g_door_open[2][TILE_BYTES];
uint8_t g_door_variants[6][4][TILE_BYTES];
uint8_t g_key_base[2][TILE_BYTES];
uint8_t g_keys[12][TILE_BYTES];

static void read_tile(uint8_t dst[16], uint32_t file_off,
                      const uint8_t *rom, uint32_t rom_size)
{
    if (file_off + 16 > rom_size) return;
    memcpy(dst, rom + file_off, 16);
}

void tiledata_load_from_rom(const uint8_t *rom_data, uint32_t rom_size)
{
    memset(g_font, 0, sizeof(g_font));
    memset(g_digits, 0, sizeof(g_digits));
    memset(g_title_logo, 0, sizeof(g_title_logo));
    memset(g_hud_logo, 0, sizeof(g_hud_logo));
    memset(g_hud_map, 0, sizeof(g_hud_map));
    memset(g_wall_variants, 0, sizeof(g_wall_variants));
    memset(g_door_base, 0, sizeof(g_door_base));
    memset(g_door_open, 0, sizeof(g_door_open));
    memset(g_heart, 0, sizeof(g_heart));
    memset(g_key_base, 0, sizeof(g_key_base));

    /* g_font[28] @ ROM 0x8796 (A-Z + [ + \ ) */
    for (int i = 0; i < 28; i++)
        read_tile(g_font[i], 0x4796u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_digits[10] @ ROM 0x86F6 */
    for (int i = 0; i < 10; i++)
        read_tile(g_digits[i], 0x46F6u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_title_logo[70] @ ROM 0x8056 (4 border + 66 body) */
    for (int i = 0; i < 70; i++)
        read_tile(g_title_logo[i], 0x4056u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_hud_logo[28] = 7×4, loaded from 4 row banks */
    for (int i = 0; i < 7; i++)
        read_tile(g_hud_logo[0 * 7 + i], 0x3E96u + (uint32_t)i * 16u, rom_data, rom_size);
    for (int i = 0; i < 7; i++)
        read_tile(g_hud_logo[1 * 7 + i], 0x3F06u + (uint32_t)i * 16u, rom_data, rom_size);
    for (int i = 0; i < 7; i++)
        read_tile(g_hud_logo[2 * 7 + i], 0x3F76u + (uint32_t)i * 16u, rom_data, rom_size);
    for (int i = 0; i < 7; i++)
        read_tile(g_hud_logo[3 * 7 + i], 0x3FE6u + (uint32_t)i * 16u, rom_data, rom_size);

    /* Fixup: tiles 0x42 (idx 24) rows 3-6 y 0x44 (idx 26) rows 2-6
     * tienen col=0x04 (ink=0 → border). Cambiar ink=0 → ink=1 (negro)
     * para que no hereden el color del borde. */
    for (int r = 3; r <= 6; r++) {
        uint8_t *col = &g_hud_logo[24][r * 2 + 1];
        if (*col == 0x04u) *col = 0x14u;
    }
    for (int r = 2; r <= 6; r++) {
        uint8_t *col = &g_hud_logo[26][r * 2 + 1];
        if (*col == 0x04u) *col = 0x14u;
    }

    /* g_hud_map[28] @ ROM 0x84B6 */
    for (int i = 0; i < 28; i++)
        read_tile(g_hud_map[i], 0x44B6u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_wall_variants[4] @ file 0x49C6 (0-1) + 0x4966 (2-3) */
    for (int i = 0; i < 2; i++)
        read_tile(g_wall_variants[i], 0x49C6u + (uint32_t)i * 16u, rom_data, rom_size);
    for (int i = 0; i < 2; i++)
        read_tile(g_wall_variants[2 + i], 0x4966u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_heart[1] @ file 0x5A76 (life icon) */
    read_tile(g_heart[0], 0x5A76u, rom_data, rom_size);

    /* g_door_base[4] = 2x2 door tiles @ file 0x59F6 */
    for (int i = 0; i < 4; i++)
        read_tile(g_door_base[i], 0x59F6u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_door_open[2] (open door frame) @ file 0x5A36 */
    for (int i = 0; i < 2; i++)
        read_tile(g_door_open[i], 0x5A36u + (uint32_t)i * 16u, rom_data, rom_size);

    /* g_key_base[2] @ file 0x5A56 (0) and 0x5A66 (1) */
    read_tile(g_key_base[0], 0x5A56u, rom_data, rom_size);
    read_tile(g_key_base[1], 0x5A66u, rom_data, rom_size);

    tiledata_generate_keys();
    tiledata_generate_doors();
}

void tiledata_generate_keys(void)
{
    static const uint8_t KEY_BASE_INKS[6] = { 0x4, 0x6, 0xD, 0x2, 0x7, 0xA };
    for (int k = 0; k < 6; k++) {
        uint8_t ink = KEY_BASE_INKS[k];
        for (int t = 0; t < 2; t++) {
            uint8_t *dst = g_keys[k * 2 + t];
            for (int r = 0; r < 8; r++) {
                dst[r * 2]     = g_key_base[t][r * 2];
                dst[r * 2 + 1] = (uint8_t)((ink << 4) | (g_key_base[t][r * 2 + 1] & 0x0Fu));
            }
        }
    }
}

void tiledata_generate_doors(void)
{
    static const uint8_t DOOR_INKS[6] = { 0x4, 0x6, 0xD, 0x2, 0x7, 0xA };
    for (int k = 0; k < 6; k++) {
        uint8_t new_col = DOOR_INKS[k];
        for (int t = 0; t < 4; t++) {
            uint8_t *dst = g_door_variants[k][t];
            for (int r = 0; r < 8; r++) {
                dst[r * 2] = g_door_base[t][r * 2];
                uint8_t col = g_door_base[t][r * 2 + 1];
                uint8_t ink   = (col >> 4u) & 0x0Fu;
                uint8_t paper = col & 0x0Fu;
                if (ink   == 0x4u) ink   = new_col;
                if (paper == 0x4u) paper = new_col;
                dst[r * 2 + 1] = (uint8_t)((ink << 4u) | paper);
            }
        }
    }
}
