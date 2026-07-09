/*
 * THE CASTLE — Estado global compartido entre módulos
 * ====================================================
 * Todas las variables que antes eran "static" en the_castle.c
 * y que otros módulos necesitan via "extern" se declaran aquí.
 *
 * Incluir este header en cualquier .c que necesite acceder al
 * estado del juego.
 */

#pragma once
#ifndef CASTLE_GAME_H
#define CASTLE_GAME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * ESTADO DEL JUGADOR Y DEL JUEGO (RAM MSX 0xE320..0xEAFF)
 * ========================================================================== */

extern uint8_t g_state_flags;   /* 0xEAC9 — frame counter + flags de modo   */
extern uint8_t g_anim_frame;    /* 0xEACB — frame de animación del jugador   */
extern uint8_t g_facing;        /* 0xEACC — dirección (0=der, 0xFF=izq)      */
extern uint8_t g_player_speed;  /* 0xEACA — velocidad/contador sub-pixel     */
extern uint8_t g_transition;    /* 0xEAD6 — contador de transición 0x00..0x11*/
extern uint8_t g_game_over;     /* 0xEAE0 — flag de game over                */
extern uint8_t g_room_exit;     /* 0xEAE1 — flag/código de salida de sala    */
extern uint8_t g_restart_flag;  /* 0xEAE3 — solicitud de restart de sala     */
extern uint8_t g_intro_active;  /* 0xEAE4 — flag de intro/título activo      */
extern uint8_t g_enemies_active;/* 0xEAE8 — semáforo de actualización enemigos*/

extern uint8_t g_player_col;    /* 0xE334 — columna del jugador (0-19)       */
extern uint8_t g_player_row;    /* 0xE335 — fila del jugador (0-29)          */
extern uint8_t g_player_x;      /* 0xE320 — posición X en pixels             */
extern uint8_t g_player_y;      /* 0xE321 — posición Y en tiles              */

extern uint8_t g_lives;         /* 0xE324 — vidas restantes                  */
extern uint8_t g_room_number;   /* 0xE333 — número de sala actual            */

extern uint8_t g_score[3];      /* 0xE33D — puntuación BCD (3 bytes)         */
extern uint8_t g_hiscore[3];    /* 0xE340 — hi-score BCD                     */

/* Mapa de colisión/tiles: 20 columnas × 30 filas */
extern uint8_t g_map[0x400];    /* 0xE000 */

/* ROM data pointer (inicializado por tiles_load_from_rom) */
extern const uint8_t *g_rom;       /* puntero al buffer de la ROM            */
extern uint32_t       g_rom_size;  /* tamaño del buffer                      */

/* ==========================================================================
 * API DE CADA MÓDULO
 * ========================================================================== */

/* --- tiles.c --- */
void    tiles_load_from_rom(const uint8_t *rom_data, uint32_t rom_size);
void    tiles_rom_to_vram(uint32_t rom_file_off, uint16_t vram_idx,
                          uint8_t count);
void    tiles_vram_from_rom(uint32_t rom_file_off, uint16_t vram_idx,
                            uint8_t count);
void    tiles_load_interleaved_tile(uint32_t rom_file_off, uint16_t vram_idx,
                                    bool mirror);
void    tiles_reload_all(void);
void    tiles_load_walls_and_anim(uint16_t vram_idx);
void    tiles_load_patterns(uint32_t rom_off, uint16_t vram_idx,
                            uint8_t count, uint8_t color);
void    tiles_reload_walls_and_anim(void);
void    tiles_animate(uint8_t frame_counter);
void    tiles_dump_vram(const char *label);
uint8_t tiles_vram_idx_blank(void);
uint8_t tiles_vram_idx_door(void);
uint8_t tiles_vram_idx_bg3(uint8_t n);
uint8_t tiles_vram_idx_key(void);
uint8_t tiles_vram_idx_anim_bg(uint8_t n);
uint8_t tiles_vram_idx_bg4(uint8_t n);
uint8_t tiles_vram_idx_wall(uint8_t n);
uint8_t tiles_vram_idx_space(void);

/* --- enemies.c --- */
void enemies_init(void);
void update_enemies(void);
void sub_6F5C(void);

/* Stubs requeridos por doors.c */
void update_roller_by_pos(uint8_t col, uint8_t row, uint8_t move_flags);
void update_bat_by_slot(uint8_t col, uint8_t row, uint8_t move_flags);

/* --- particles.c --- */
void particles_init(void);
void update_particles(void);
void sub_61F5(uint8_t move_flags, uint8_t enemy_col, uint8_t enemy_row);
void sub_7279(uint8_t *col_inout, uint8_t *row_inout, uint8_t anim_id);

/* --- doors.c --- */
void    doors_init(void);
void    update_doors(void);
void    check_door_exit(void);
void    update_collectibles(void);
void    check_key_pickup(void);
void    update_traps(void);
uint8_t doors_keys_collected(void);
uint8_t doors_find_trigger(uint8_t col, uint8_t row);

/* --- the_castle.c (game logic) --- */
void game_init(void);
void game_reset_level(void);
void game_loop(void);

/* --- main.c (per-frame game loop) --- */
void game_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* CASTLE_GAME_H */

extern uint8_t g_anim_ctr[3];   /* animation counter per screen third (enemies.c) */

/* --- room.c --- */
void room_init(void);
void room_load_initial(void);
void room_load_title(void);
void room_transition(void);
void room_full_load(void);
bool room_script_tick(void);
extern uint8_t g_object_table[]; /* 0xE346, size 0x150 */

/* --- pickup.c --- */
void pickup_init(void);
void pickup_frame(void);
void pickup_anim_frame(void);
void minimap_draw_full(void);
void minimap_room_exit_mark(void);
uint8_t pickup_any_key(void);
extern uint8_t g_keys[6];        /* 0xE337-0xE33C */
extern uint8_t g_power_red;      /* 0xE343 */
extern uint8_t g_power_green;    /* 0xE344 */
extern uint8_t g_door_reset;     /* 0xEAE2 */

/* --- the_castle.c (exposed for persistence) --- */
extern uint8_t g_subpixel_x;     /* 0xE322 */
extern uint8_t g_dir_timer;      /* 0xE323 */
extern uint8_t g_enemy_slots[9]; /* 0xE325..0xE32D */

/* --- music.c --- */
void music_init(void);
void music_isr_tick(void);
void music_load(uint16_t music_a_addr, uint16_t music_b_addr);
void music_set_tempo(uint8_t counter, uint8_t value);
void music_set_transpose(uint8_t fine, uint8_t coarse);
void music_sfx_trigger(uint8_t sfx_id, uint8_t volume);
void music_play_title(void);
void music_play_game(void);
void music_play_underwater(void);
void music_play_immortality(void);
void music_play_song2(void);
void music_stop(void);

/* --- camera.c --- */
void camera_init(void);
void camera_update(void);
void scroll_update(void);
void render_background(void);
void draw_hud(void);
void camera_draw_string(uint8_t col, uint8_t row, uint16_t rom_str_addr,
                        uint8_t tile_base, uint8_t delay_frames);
extern uint8_t g_music_transpose_fine;
extern uint8_t g_music_transpose_coarse;
extern uint8_t g_music_tempo_counter;

/* --- particles.c (shared state) --- */
extern uint8_t g_spark_timer_a;
extern uint8_t g_spark_timer_b;
extern uint8_t g_death_fade_timer;

/* --- room.c (shared state) --- */
extern uint8_t g_tilemap[];
extern uint8_t g_room_x;
extern uint8_t g_room_y;

extern uint8_t g_keyframe_queue[9];

/* --- title.c --- */
void title_screen(void);
