#pragma once
#ifndef CASTLE_TILEDATA_H
#define CASTLE_TILEDATA_H

#include <stdint.h>

#define TILE_BYTES 16

extern uint8_t g_font[28][TILE_BYTES];
extern uint8_t g_digits[10][TILE_BYTES];
extern uint8_t g_title_logo[70][TILE_BYTES];
extern uint8_t g_hud_logo[28][TILE_BYTES];
extern uint8_t g_hud_map[28][TILE_BYTES];
extern uint8_t g_wall_variants[4][TILE_BYTES];
extern uint8_t g_heart[1][TILE_BYTES];
extern uint8_t g_door_base[4][TILE_BYTES];
extern uint8_t g_door_open[2][TILE_BYTES];
extern uint8_t g_door_variants[6][4][TILE_BYTES];
extern uint8_t g_key_base[2][TILE_BYTES];
extern uint8_t g_keys[12][TILE_BYTES];

void tiledata_load_from_rom(const uint8_t *rom_data, uint32_t rom_size);
void tiledata_generate_keys(void);
void tiledata_generate_doors(void);

#endif
