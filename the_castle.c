/*
 * THE CASTLE (ASCII, 1986) - MSX ROM → C Port
 * ============================================
 * Disassembled from: The_Castle_-_ASCII__1986___GoodMSX___356_.rom
 * ROM: 32KB, ORG 0x4000, entry point 0x4010
 *
 * Portability target: C99, platform-agnostic core logic.
 * All MSX BIOS calls are wrapped behind a HAL (Hardware Abstraction Layer)
 * so the game logic compiles and runs without MSX hardware.
 *
 * Register mapping convention used throughout:
 *   Z80 A  → uint8_t  a  (accumulator)
 *   Z80 BC → uint16_t bc, with b = hi byte, c = lo byte
 *   Z80 DE → uint16_t de, with d = hi byte, e = lo byte
 *   Z80 HL → uint16_t hl, used as address or 16-bit value
 *   Z80 F  → flags reconstructed from C expressions (Z, C, NZ, NC)
 *
 * RAM map (MSX Work RAM used by the game, base 0xE000-0xEAFF):
 *   The game uses two RAM regions:
 *     0xE000-0xE3FF : map/level data area
 *     0xEA00-0xEAFF : game state variables
 *
 * Build: gcc -std=c99 -Wall -o the_castle the_castle.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "hal.h"
#include "game.h"

/* Internal state (not shared with other modules) */
static const uint8_t *g_music_ptr  = NULL; /* 0xEAE5 */
static uint8_t        g_music_ticks = 0;   /* 0xEAE7 */
static uint8_t        g_sprites[32 * 4];   /* sprite attr shadow */
uint8_t               g_keyframe_queue[9]; /* 0xEACD */
static uint8_t        g_subpixel_x  = 0;   /* 0xE322 */
static uint8_t        g_dir_timer   = 0;   /* 0xE323 */
static uint8_t        g_scroll_x    = 0;   /* 0xE331 */
static uint8_t        g_scroll_y    = 0;   /* 0xE332 */
static uint8_t        g_enemy_slots[9];    /* 0xE325..0xE32D */
static const uint8_t *g_tile_data_ptr = NULL; /* 0xEAD7 */
static uint16_t       g_vram_name_base  = 0x1800u;
static uint16_t       g_vram_color_base = 0x2000u;
static uint16_t       g_vram_pat_base   = 0x0000u;
static uint16_t       g_vram_spr_attr   = 0x1B00u;
static uint16_t       g_vram_spr_pat    = 0x3800u;


/* ==========================================================================
 * VRAM LAYOUT (MSX Screen 2 / Graphic 2)
 *   Pattern table  : 0x0000  (256 patterns × 8 bytes = 2KB)
 *   Color table    : 0x2000  (256 patterns × 8 bytes = 2KB)
 *   Name table     : 0x1800  (32×24 = 768 bytes)
 *   Sprite patterns: 0x3800
 *   Sprite attribs : 0x1B00
 * ========================================================================== */
#define VRAM_PATTERN_BASE   0x0000u
#define VRAM_COLOR_BASE     0x2000u
#define VRAM_NAME_BASE      0x1800u
#define VRAM_SPRITE_PAT     0x3800u
#define VRAM_SPRITE_ATTR    0x1B00u

/* ==========================================================================
 * GAME RAM - mirrors the MSX work-RAM variables used by the original code.
 * Variable names are derived from the disassembly context.
 * ========================================================================== */

/* --- State flags (0xEAC9) ---
 * Bit 0 : title screen / demo mode flag  (tested in sub_5D5D / sub_6F17)
 * Bit 2 : fire / action button pressed   (tested in sub_6F21)
 */
// (defined in main.c) uint8_t g_state_flags; */

/* --- Player movement state (0xEACB / 0xEACC) ---
 * eacb : current animation frame / direction index (0-8)
 * eacc : facing direction flag (0=right, 0xFF=left)
 */
// (defined in main.c) uint8_t g_anim_frame; */
// (defined in main.c) uint8_t g_facing; */

/* --- Player world position (0xEACA) ---
 * Used as a speed/position counter (initialised to 0x70 = 112)
 */
// (defined in main.c) uint8_t g_player_speed; */

/* --- Screen-transition / intro counter (0xEAD6) ---
 * Counts 0x00..0x11 (17 steps) during wipe/scroll transitions.
 * Value 0x11 = transition complete.
 */
// (defined in main.c) uint8_t g_transition; */

/* --- Game-over / death flag (0xEAE0) ---
 * Non-zero when player has died or game should end.
 */
// (defined in main.c) uint8_t g_game_over; */

/* --- Room exit flag (0xEAE1) ---
 * Non-zero when player has reached the room exit.
 */
// (defined in main.c) uint8_t g_room_exit; */

/* --- "Quentin mode" / special event flag (0xEAE3) ---
 * Set by several subsystems to request a scene restart.
 */
// (defined in main.c) uint8_t g_restart_flag; */

/* --- Intro-sequence active flag (0xEAE4) ---
 * Non-zero while the title / intro animation is running.
 */
// (defined in main.c) uint8_t g_intro_active; */

/* --- Current music/SFX data pointer (0xEAE5 / 0xEAE7) ---
 * HL = pointer into the music table; A = remaining ticks for current note.
 */
static const uint8_t *g_music_ptr;  /* 0xEAE5 */
// (static in block above) uint8_t g_music_ticks;       /* 0xEAE7 */

/* --- Enemy-present flag (0xEAE8) ---
 * Non-zero when enemies are on-screen and active.
 */
// (defined in main.c) uint8_t g_enemies_active; */

/* --- Score (BCD, 3 bytes little-endian) (0xEA00) --- */
// (defined in main.c) uint8_t g_score[3]; */

/* --- Hi-score (BCD, 3 bytes) (0xE340) --- */
// (defined in main.c) uint8_t g_hiscore[3]; */

/* --- Player map position (0xE334 = col, 0xE335 = row) --- */
// (defined in main.c) uint8_t g_player_col; */
// (defined in main.c) uint8_t g_player_row; */

/* --- Level/room data (0xE000..0xE3FF) --- */
// (defined in main.c) uint8_t g_map[0x400]; */

/* --- Sprite attribute table mirror (32 sprites × 4 bytes) --- */
// (static in block above) uint8_t g_sprites[32 * 4];  /* shadow of VRAM 0x1B00 */

/* --- Key-frame queue for intro animation (0xEACD..0xEAD5 = 9 bytes) --- */
// (static in block above) uint8_t g_keyframe_queue[9]; /* 0xEACD */

/* --- VDP address registers (mirrors of MSX BIOS variables) --- */
// (static in block above) uint16_t g_vram_name_base;   /* 0xF3C7 : name table base   = 0x1800 */
// (static in block above) uint16_t g_vram_color_base;  /* 0xF3C9 : color table base  = 0x2000 */
// (static in block above) uint16_t g_vram_pat_base;    /* 0xF3CB : pattern table     = 0x0000 */
// (static in block above) uint16_t g_vram_spr_attr;    /* 0xF3CD : sprite attr table = 0x1B00 */
// (static in block above) uint16_t g_vram_spr_pat;     /* 0xF3CF : sprite pattern    = 0x3800 */

/* --- Pointer into ROM sprite/tile data --- */
static const uint8_t *g_tile_data_ptr; /* 0xEAD7, init = 0x7CF2 in ROM    */

/* ==========================================================================
 * FORWARD DECLARATIONS
 * ========================================================================== */
void game_init(void);
void title_screen(void);
void game_loop(void);
void game_reset_level(void);
/* void frame_update(void); */

static void render_map(void);
static void update_player(void);
static uint8_t collision_check(uint8_t col, uint8_t row);

static void music_tick(void);
/* void sfx_play(uint8_t id); */

static void draw_sprite(uint8_t sprite_id, uint8_t x, uint8_t y, uint8_t tile, uint8_t color);
static void put_tile(uint8_t col, uint8_t row, uint8_t tile_id, uint8_t color);
static uint16_t vram_name_addr(uint8_t col, uint8_t row);
static uint16_t vram_sprite_row(uint8_t col, uint8_t row);

static void score_add(uint8_t tens, uint8_t units);
static void hiscore_check(void);
static void score_display(void);

/* ==========================================================================
 * INIT (sub_4CA2 + sub_4C88 + sub_4D3F)
 *
 * Original Z80 summary:
 *   4010  LD SP, 0xF000        ; set stack
 *   4013  CALL sub_4CA2        ; hardware init
 *   sub_4CA2:
 *     CALL sub_4C88            ; VDP register setup (not shown, sets modes)
 *     LD H, 0x80
 *     CALL BIOS_DCOMPR         ; detect MSX2 (compare H with MSXVER)
 *     EI                       ; enable interrupts
 *     CALL BIOS_KEYINT         ; init keyboard
 *     LD A, 0x0F → (0xF3E9)   ; function key display off
 *     LD A, 0x01 → (0xF3EA/EB); line length = 40
 *     CALL 0x0062              ; BIOS init (undocumented slot)
 *     ; Set up VRAM base addresses in BIOS variables:
 *     HL=0x1800 → (0xF3C7)    ; name table
 *     HL=0x2000 → (0xF3C9)    ; color table
 *     HL=0x0000 → (0xF3CB)    ; pattern table
 *     HL=0x1B00 → (0xF3CD)    ; sprite attr
 *     HL=0x3800 → (0xF3CF)    ; sprite patterns
 *     CALL BIOS_RDVRM          ; dummy VDP read (sync)
 *     CALL BIOS_DISSCR         ; blank screen
 *     CALL BIOS_CLRSPR         ; clear all sprites
 *     ; Enable slot for ROM data at 0x4000-0x7FFF...
 *     ; Copy sprite/tile ROM data to VRAM (0x7CF2 → VRAM)
 *     ; Set VDP registers for graphics mode 2
 *     ; Hook interrupt vector at 0xFD9F (custom ISR at 0x75D4)
 *     EI / RET
 * ========================================================================== */
void game_init(void)
{
    /* Set VRAM layout addresses (mirrors MSX BIOS variables) */
    g_vram_name_base  = VRAM_NAME_BASE;    /* 0x1800 */
    g_vram_color_base = VRAM_COLOR_BASE;   /* 0x2000 */
    g_vram_pat_base   = VRAM_PATTERN_BASE; /* 0x0000 */
    g_vram_spr_attr   = VRAM_SPRITE_ATTR;  /* 0x1B00 */
    g_vram_spr_pat    = VRAM_SPRITE_PAT;   /* 0x3800 */

    hal_vdp_disable_screen();
    hal_vdp_clear_sprites();
    hal_vdp_init_screen2();

    /* Copy tile/sprite ROM data to VRAM.
     * In the original, the ROM data at 0x7CF2 is block-copied into VRAM.
     * Here we delegate to the HAL which should load the binary data. */
    /* hal_load_tile_data(); */

    /* Set up VDP color registers (from sub_4CA2 / INITXT calls in sub_4D27):
     *   VDP R7 = 0x0F  (white on black, border)
     *   VDP R1 bit 6 = 1 (enable screen)  -- done after init
     */
    hal_vdp_write_reg(7, 0x0F);

    /* Initialise game state */
    memset(g_score,   0, sizeof(g_score));
    memset(g_hiscore, 0, sizeof(g_hiscore));
    memset(g_map,   0, sizeof(g_map));
    memset(g_keyframe_queue, 0xFF, sizeof(g_keyframe_queue));

    g_state_flags    = 0;
    g_anim_frame     = 0;
    g_facing         = 0;
    g_player_speed   = 0x70;
    g_transition     = 0;
    g_game_over      = 0;
    g_room_exit      = 0;
    g_restart_flag   = 0;
    g_intro_active   = 0;
    g_enemies_active = 0;
    g_music_ptr      = NULL;
    g_music_ticks    = 0;
    g_player_col     = 0;
    g_player_row     = 0;
}

/* title_screen() is in title.c */


/* ==========================================================================
 * RESET LEVEL STATE (sub_4D52)
 *
 * Original Z80 summary:
 *   LD A,0x05 → (0xE324)   ; lives remaining = 5
 *   LD A,0x70 → (0xE320)   ; player X pixel position = 0x70 (112)
 *   LD A,0x06 → (0xE333)   ; room number = 6 (starting room)
 *   LD A,0x01 → (0xE321)   ; player Y = 1 (top of map)
 *   LD A,0x70 → (0xEACA)   ; speed counter = 0x70
 *   LD A,0x00 → (0xE322)   ; sub-pixel X = 0
 *   LD A,0x11 → (0xE323)   ; direction timer = 0x11
 *   Zero-fill 0xE325..0xE32D (9 bytes) ; enemy slot data
 *   Zero-fill 0xE331..0xE332          ; scroll offset
 *   LD (0xEAE8),A = 0      ; enemies inactive
 *   Fill 0xE000..0xE00C with 0x00     ; first 13 map bytes (walkable)
 *   Fill 0xE00D..0xE2D2 with 0xFF     ; rest of map (solid walls)
 * ========================================================================== */

/* Game state variables for level (mapped from disassembly) */
// (defined in main.c) uint8_t g_lives; */
// (defined in main.c) uint8_t g_player_x; */
// (defined in main.c) uint8_t g_room_number; */
// (defined in main.c) uint8_t g_player_y; */
// (static in block above) uint8_t g_subpixel_x;     /* 0xE322 - sub-pixel offset  */
// (static in block above) uint8_t g_dir_timer;      /* 0xE323 - direction timer   */
// (static in block above) uint8_t g_scroll_x;       /* 0xE331 - scroll X          */
// (static in block above) uint8_t g_scroll_y;       /* 0xE332 - scroll Y          */
// (static in block above) uint8_t g_enemy_slots[9]; /* 0xE325..0xE32D             */

void game_reset_level(void)
{
    g_lives        = 5;
    g_player_x     = 0x70;    /* 112 pixels */
    g_room_number  = 6;
    g_player_y     = 1;
    g_player_speed = 0x70;
    g_subpixel_x   = 0;
    g_dir_timer    = 0x11;
    g_scroll_x     = 0;
    g_scroll_y     = 0;
    g_enemies_active = 0;

    memset(g_enemy_slots, 0, sizeof(g_enemy_slots));

    /* Map: first 13 bytes walkable (0x00), rest walls (0xFF) */
    memset(g_map,        0x00, 13);
    memset(g_map + 13,   0xFF, sizeof(g_map) - 13);
}

/* ==========================================================================
 * PLAYER UPDATE (sub_40BB) — Una iteración de movimiento del jugador
 *
 * Original Z80:
 *   40BB  LD HL,0x0000   ; H=dy, L=dx
 *   40BE  LD DE,0x00FF   ; D=vertical, E=horizontal delta
 *   40C1  LD A,(0xE334)  ; player_col
 *   ...
 *   [lee joystick, computa movimiento, chequea colisión, actualiza animación]
 * ========================================================================== */
void game_loop(void)
{
    /* sub_6383: reset keyframe queue sentinel values */
    memset(g_keyframe_queue, 0xFF, sizeof(g_keyframe_queue));

    /* Update player movement (sub_40BB) */
    update_player();
}

/* ==========================================================================
 * PLAYER MOVEMENT (sub_40BB)
 *
 * Original Z80 summary:
 *   HL = 0x0000  (result movement vector: H=dy, L=dx)
 *   DE = 0x00FF  (D=vertical delta, E=horizontal delta; init to sentinel)
 *   BC = (0xE334)(0xE335+2)  ; player col, row+2 (feet position)
 *   CALL sub_4515  ; read joystick → A = direction bits
 *   OR A / LD A,(0xEAD6)     ; check transition counter
 *   [complex branching to compute dx/dy from joystick direction]
 *   CALL sub_41F6  ; apply movement (clamp to map, update position)
 *   [bit-test E for horizontal/vertical flags → set bits in H for animation]
 *   CALL sub_4515  ; re-read joystick
 *   [more animation state updates]
 *
 * Joystick direction encoding (MSX standard, from BIOS GTSTCK):
 *   0 = none, 1 = up, 2 = up-right, 3 = right, 4 = down-right,
 *   5 = down, 6 = down-left, 7 = left, 8 = up-left
 * ========================================================================== */

/* Direction → (dx, dy) table derived from the branching logic */
static const int8_t DIR_DX[9] = { 0,  0,  1,  1,  1,  0, -1, -1, -1 };
static const int8_t DIR_DY[9] = { 0, -1, -1,  0,  1,  1,  1,  0, -1 };

static void update_player(void)
{
    /* Read joystick (sub_4515 wraps BIOS GTSTCK port 1) */
    uint8_t dir = hal_joystick_read(1); /* 0-8 */
    if (dir > 8) dir = 0;

    int8_t dx = DIR_DX[dir];
    int8_t dy = DIR_DY[dir];

    /* g_transition counts a wipe animation 0x00→0x11.
     * While wipe is in progress the player cannot move. */
    if (g_transition > 0 && g_transition < 0x11) {
        /* Advance transition counter */
        if (g_transition < 0x11) g_transition++;
        /* sub_41F6: apply zero movement (player frozen) */
        dx = dy = 0;
    }

    /* sub_41F6: apply movement with collision (simplified) */
    uint8_t new_col = (uint8_t)((int)g_player_col + dx);
    uint8_t new_row = (uint8_t)((int)g_player_row + dy);

    /* Map bounds: 20 cols (0x14), 30 rows (0x1E) — from sub_6A7C CP 0x14 / CP 0x1E */
    if (new_col >= 0x14) new_col = g_player_col;
    if (new_row >= 0x1E) new_row = g_player_row;

    /* Tile collision check */
    if (collision_check(new_col, new_row) == 0) {
        g_player_col = new_col;
        g_player_row = new_row;
    }

    /* Update animation frame (from bit manipulation in sub_40BB / sub_412B) */
    if (dx != 0 || dy != 0) {
        g_anim_frame = (g_anim_frame + 1) & 0x07;
        g_facing = (dx < 0) ? 0xFF : 0x00;
    }

    /* Update player pixel X (sub-pixel counter at 0xEAD6, range 0..0x11 = 17 steps) */
    if (dx != 0) {
        if (dx > 0) {
            if (g_transition < 0x11) g_transition++;
            else g_transition = 0x11;
        } else {
            if (g_transition > 0x00) g_transition--;
            else g_transition = 0x00;
        }
    }

    /* Update sprite position in shadow table */
    uint8_t px = g_player_x;
    uint8_t py = (uint8_t)(g_player_row * 8);
    draw_sprite(0, px, py, g_anim_frame, (g_facing == 0) ? 0x0F : 0x0E);
}

/* ==========================================================================
 * COLLISION CHECK
 *
 * The map stores tile IDs in g_map[row*20 + col].
 * 0x00 = walkable space, 0xFF = solid wall.
 * Other values encode doors, spikes, keys, etc.
 *
 * Returns: 0 = passable, non-zero = blocked
 * ========================================================================== */
static uint8_t collision_check(uint8_t col, uint8_t row)
{
    if (col >= 20 || row >= 30) return 0xFF; /* out of bounds = solid */
    return g_map[(uint16_t)row * 20u + col];
}

/* update_enemies() is implemented in enemies.c */


/* ==========================================================================
 * RENDER MAP (sub_62D8 / sub_6383 area — stub)
 *
 * The render loop at sub_63BB iterates 10×10 tiles and calls sub_640F
 * which writes tile ID and colour to the VDP name table.
 * sub_6EE1 is the low-level "write one tile to VRAM" function.
 * ========================================================================== */
static void render_map(void)
{
    for (uint8_t row = 0; row < 10; row++) {
        for (uint8_t col = 0; col < 10; col++) {
            uint8_t tile = g_map[(uint16_t)row * 20u + col];
            uint8_t color = (tile == 0x00) ? 0x04 : 0x07;
            put_tile(col, row, tile, color);
        }
    }
}

/* ==========================================================================
 * MUSIC TICK (sub_4B4C / sub_5128 area — stub)
 *
 * sub_5128 is the main "wait + music" routine called 30×.
 * It loops g_player_speed (0xEACA = 0x70 = 112) times calling sub_50E8
 * (the PSG update) then handles the music data stream.
 *
 * Music data format (from sub_5128 / sub_516A area):
 *   Each byte: bits[3:0] = note duration ticks
 *              bit[4]    = 1 → set facing to 0xFF (left), else 0
 *   The pointer g_music_ptr advances through a table; 0xFF = loop/end.
 * ========================================================================== */
static void music_tick(void)
{
    if (g_music_ptr == NULL) return;

    /* Decrement tick counter for current note */
    if (g_music_ticks > 0) {
        g_music_ticks--;
        return;
    }

    /* Advance to next note */
    g_music_ptr += 2; /* each entry is 2 bytes: [note_data, channel_data] */
    uint8_t note_byte = g_music_ptr[0];

    if (note_byte == 0xFF) {
        /* End of stream — silence */
        hal_psg_write(8, 0); /* volume channel A = 0 */
        hal_psg_write(9, 0); /* volume channel B = 0 */
        return;
    }

    g_music_ticks = note_byte & 0x0F;
    g_anim_frame  = (note_byte & 0x10) ? 0xFF : 0x00; /* bit4 → facing */

    /* Write tone to PSG (simplified) */
    hal_psg_write(0, g_music_ptr[1]);
}

/* ==========================================================================
 * SCORE (sub_5D87 / sub_5DC0 area)
 *
 * Score is stored as 3-byte packed BCD (6 digits), little-endian.
 * sub_5D87 adds DE (tens/units BCD pair) to the score using DAA.
 * sub_5DC0 then checks against hi-score and updates the display.
 * ========================================================================== */
static void score_add(uint8_t tens, uint8_t units)
{
    /* BCD addition: units digit */
    uint8_t carry = 0;
    uint8_t sum = (g_score[0] & 0x0F) + (units & 0x0F);
    if (sum >= 10) { sum -= 10; carry = 1; }
    sum |= ((g_score[0] >> 4) + (units >> 4) + carry) * 16;
    /* simplification: real code uses DAA instruction */
    g_score[0] = sum;
    /* propagate carry into g_score[1], g_score[2] similarly */

    hiscore_check();
}

static void hiscore_check(void)
{
    /* Compare g_score vs g_hiscore (3-byte BCD, big-endian compare) */
    for (int i = 2; i >= 0; i--) {
        if (g_score[i] > g_hiscore[i]) {
            memcpy(g_hiscore, g_score, 3);
            return;
        }
        if (g_score[i] < g_hiscore[i]) return;
    }
}

static void score_display(void)
{
    /* Write BCD score digits to VRAM name table.
     * sub_5DC0 calls sub_5DD3 which iterates 3 bytes and calls sub_5DDE
     * to render each digit pair at a fixed screen position. */
    /* Score at name-table column 0x22, hi-score at 0x2A */
    for (int i = 0; i < 3; i++) {
        uint8_t byte   = g_score[i];
        uint8_t hi_dig = byte >> 4;
        uint8_t lo_dig = byte & 0x0F;
        put_tile((uint8_t)(0x22 + i * 2),     23, hi_dig + '0', 0x0F);
        put_tile((uint8_t)(0x22 + i * 2 + 1), 23, lo_dig + '0', 0x0F);
    }
}

/* ==========================================================================
 * VDP HELPERS
 * ========================================================================== */

/* Name-table VRAM address for a given (col, row) tile position */
static uint16_t vram_name_addr(uint8_t col, uint8_t row)
{
    return (uint16_t)(g_vram_name_base + (uint16_t)row * 32u + col);
}

/* Write a tile (character + colour) to the VDP name table.
 * sub_6EE1 / sub_6EAE in the original. */
static void put_tile(uint8_t col, uint8_t row, uint8_t tile_id, uint8_t color)
{
    uint16_t addr = vram_name_addr(col, row);
    hal_vdp_write_vram(addr, tile_id);
    /* Colour table: same offset but in colour base */
    hal_vdp_write_vram((uint16_t)(g_vram_color_base + (uint16_t)tile_id * 8u), color);
}

/* Write sprite attributes to shadow and queue DMA.
 * sub_6A7C / sub_6ADF in the original. */
static void draw_sprite(uint8_t sprite_id, uint8_t x, uint8_t y,
                        uint8_t tile, uint8_t color)
{
    uint8_t *s = &g_sprites[sprite_id * 4];
    s[0] = y;    /* Y pixel position (VDP: Y is stored first) */
    s[1] = x;    /* X pixel position */
    s[2] = tile; /* pattern number  */
    s[3] = color;/* colour + early-clock */

    /* Write to VRAM sprite attribute table */
    hal_vdp_copy_to_vram((uint16_t)(g_vram_spr_attr + sprite_id * 4u), s, 4);
}

/* ==========================================================================
 * ENTRY POINT
 * ========================================================================== */
