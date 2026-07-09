/*
 * THE CASTLE — Cell-based pickup system
 * =======================================
 * Port of DF0 pickup.c (Z80 sub_5B96 + sub_5BB0 + sub_4499):
 *
 *   Every even frame, check colmap at player's top-left cell for bit 2.
 *   If set, scan g_object_table[0x90..0xCF] (ITEM table at 0xE3D6)
 *   for a collectible at (g_player_col, g_player_row).
 *   Dispatch effect by type code:
 *
 *     0x20  final item      0x24  power-up green    0x28+ score + key
 *     0x21  special item    0x25  door reset
 *     0x22  map             0x26  extra life
 *     0x23  power-up red    0x27  score only
 *
 * Also provides HUD helpers (score, lives, keys, minimap).
 */

#include <stdint.h>
#include <string.h>
#include "hal.h"
#include "game.h"

/* ==========================================================================
 * EXTERNAL DATA (from room.c)
 * ========================================================================== */
extern uint8_t g_object_table[];  /* 0xE346, size 0x150 */

/* ==========================================================================
 * CONSTANTS
 * ========================================================================== */
#define VRAM_NAME_BASE   0x1800u
#define ROM_ORG          0x4000u
#define ITEM_TABLE_OFF   0x90u     /* offset into g_object_table for items */

/* ==========================================================================
 * MUSIC/SFX STATE (static, system-specific)
 * ========================================================================== */
static uint8_t g_music_ctrl;    /* 0xEAF2 */
static uint8_t g_music_tempo;   /* 0xEAF3 */
static uint8_t g_music_data;    /* 0xEAF4 */
static uint8_t g_sfx_hold;      /* 0xEAF6 */
static uint8_t g_sfx_timer;     /* 0xEAF7 */
static uint8_t g_sprite_timer;  /* 0xEAF9 */

/* Persistence buffer (0xE322-0xE333) */
static uint8_t g_persist_buf[12];

/* ==========================================================================
 * HELPERS
 * ========================================================================== */
static inline uint8_t rom_rb(uint16_t addr)
{
    uint32_t off = (uint32_t)addr - ROM_ORG;
    return (g_rom && off < g_rom_size) ? g_rom[off] : 0xFFu;
}

/* Colmap read at player position (g_tilemap = 0xE496, 30×30) */
static inline uint8_t cmv(uint8_t col, uint8_t row)
{
    return g_tilemap[(uint16_t)row * 30u + col];
}

/* Name table write at absolute offset from 0x1800 */
static void nt_put(uint16_t off, uint8_t tile)
{
    hal_vdp_write_vram((uint16_t)(VRAM_NAME_BASE + off), tile);
}

/* Write tile at play-area cell (row 0-19, col 0-29) */
static void cell_put(uint8_t col, uint8_t row, uint8_t tile)
{
    hal_vdp_write_vram((uint16_t)(VRAM_NAME_BASE
                      + (uint16_t)(row + 4u) * 32u + col), tile);
}

/* Wait one game frame */
static void pk_wait_frame(void)
{
    hal_wait_vsync();
    hal_poll_events();
}

/* ==========================================================================
 * sub_5E4B: Extra life
 * ========================================================================== */
static void s_5E4B(void)
{
    if (g_lives == 0xFFu) return;
    g_lives++;
}

/* ==========================================================================
 * sub_5E5C: HUD life hearts (row 3 col 3+, max 14, tile 0x0D)
 * ========================================================================== */
static void hud_lives(void)
{
    uint16_t a = (uint16_t)(VRAM_NAME_BASE + 0x63u);
    int b = 0x0E;
    uint8_t n;
    if (g_lives == 0u) return;
    n = (uint8_t)(g_lives - 1u);
    if (n > 0x0Eu) n = 0x0Eu;
    for (uint8_t i = 0u; i < n && b > 0; i++, b--)
        hal_vdp_write_vram(a++, 0x0Du);
    while (b-- > 0) hal_vdp_write_vram(a++, 0x00u);
}

/* ==========================================================================
 * sub_5D87: BCD 3-byte score add + hi-score update + HUD redraw
 * ========================================================================== */
static uint8_t bcd_add8(uint8_t a, uint8_t b, int *carry)
{
    int lo = (a & 0x0Fu) + (b & 0x0Fu) + *carry;
    int hi = (a >> 4) + (b >> 4) + (lo > 9 ? 1 : 0);
    if (lo > 9) lo -= 10;
    *carry = 0;
    if (hi > 9) { hi -= 10; *carry = 1; }
    return (uint8_t)((hi << 4) | lo);
}

static void s_5D87(uint16_t pts)
{
    uint8_t d = (uint8_t)(pts >> 8), e = (uint8_t)(pts & 0xFFu);
    int carry = 0;
    g_score[2] = bcd_add8(g_score[2], e, &carry);
    g_score[1] = bcd_add8(g_score[1], d, &carry);
    if (carry) s_5E4B();
    g_score[0] = bcd_add8(g_score[0], 0u, &carry);
    /* Hi-score: copy score if greater */
    {
        int i, update = 0;
        for (i = 0; i < 3; i++) {
            if (g_hiscore[i] < g_score[i]) { update = 1; break; }
            if (g_hiscore[i] > g_score[i]) break;
        }
        if (update)
            memcpy(g_hiscore, g_score, 3);
    }
}

/* ==========================================================================
 * sub_5DC0 / sub_5DD3: Display score + hi-score on HUD
 * ========================================================================== */
static void hud_scores(void)
{
    /* Score (0xE33D-0xE33F) at name table offset 0x22, 0x24, 0x26 */
    for (int i = 0; i < 3; i++) {
        uint8_t v = g_score[i];
        nt_put((uint16_t)(0x22u + (uint16_t)i * 2u),     (uint8_t)(0x47u + (v >> 4)));
        nt_put((uint16_t)(0x22u + (uint16_t)i * 2u + 1u),(uint8_t)(0x47u + (v & 0x0Fu)));
    }
    /* Hi-score (0xE340-0xE342) at name table offset 0x2A, 0x2C, 0x2E */
    for (int i = 0; i < 3; i++) {
        uint8_t v = g_hiscore[i];
        nt_put((uint16_t)(0x2Au + (uint16_t)i * 2u),     (uint8_t)(0x47u + (v >> 4)));
        nt_put((uint16_t)(0x2Au + (uint16_t)i * 2u + 1u),(uint8_t)(0x47u + (v & 0x0Fu)));
    }
}

/* ==========================================================================
 * sub_5E01: Key icons on HUD (row 2 col 3, tiles 0x01-0x0C)
 * ========================================================================== */
static void keys_hud_redraw(void)
{
    uint16_t a = (uint16_t)(VRAM_NAME_BASE + 0x43u);
    int b = 0x0E;
    uint8_t c = 1u;
    uint8_t *kp = g_keys;
    while (c != 0x0Du) {
        uint8_t cnt = *kp;
        uint8_t d = (uint8_t)(cnt / 5u + 1u), e = (uint8_t)(cnt % 5u + 1u);
        for (;;) {
            if (--d == 0u) break;
            hal_vdp_write_vram(a++, c);
            if (--b == 0) return;
        }
        c++;
        for (;;) {
            if (--e == 0u) break;
            hal_vdp_write_vram(a++, c);
            if (--b == 0) return;
        }
        c++; kp++;
    }
    while (b-- > 0) hal_vdp_write_vram(a++, 0x00u);
}

/* ==========================================================================
 * sub_5CD4: Delete collectible (blank 2×2 + SFX + wait)
 * ========================================================================== */
static void s_5CD4(uint16_t ix, uint8_t type)
{
    uint8_t h = g_object_table[(uint16_t)(ix + 2u)];
    uint8_t l = g_object_table[(uint16_t)(ix + 3u)];
    g_object_table[ix] = 0u;                    /* mark slot inactive */
    cell_put(h,     l,     0u);                 /* blank 2×2 area */
    cell_put((uint8_t)(h + 1u), l,              0u);
    cell_put((uint8_t)(h + 1u), (uint8_t)(l + 1u), 0u);
    cell_put(h,     (uint8_t)(l + 1u), 0u);
    g_sfx_timer = 0x10u;                        /* SFX pickup */
    pk_wait_frame();
}

/* ==========================================================================
 * sub_5CB5: Special-item jingle + state commit
 * ========================================================================== */
static void s_5CB5(void)
{
    uint8_t had = g_music_tempo;
    g_music_tempo = 0u; g_music_data = 0u; g_music_ctrl = 0u;
    g_sfx_hold = 0u; g_sfx_timer = 0u;
    for (int i = 0; i < 3; i++) pk_wait_frame();
    music_load(0x7A03u, 0x7A3Cu);
    if (had) g_music_tempo = 6u;
    /* Commit state: save 0xE334-0xE33F → 0xE322-0xE333 */
    /* Persist to g_map bitfields */
    {
        g_persist_buf[0] = g_subpixel_x;
        g_persist_buf[1] = g_dir_timer;
        g_persist_buf[2] = g_lives;
        memcpy(&g_persist_buf[3], g_enemy_slots, 9);
    }
}

/* ==========================================================================
 * sub_5B96 + sub_5BB0: The cell-based pickup (called every game frame)
 * ========================================================================== */
void pickup_frame(void)
{
    uint8_t b, c, s;
    uint16_t ix;

    if (g_state_flags & 0x01u) return;         /* even frames only */
    b = g_player_col;
    c = g_player_row;
    if (!(cmv(b, c) & 0x04u)) return;           /* no collectible here */

    for (s = 0u, ix = ITEM_TABLE_OFF; s < 16u; s++, ix += 4u) {
        uint8_t type;
        if (g_object_table[(uint16_t)(ix + 2u)] != b ||
            g_object_table[(uint16_t)(ix + 3u)] != c)
            continue;

        type = g_object_table[(uint16_t)(ix + 1u)];
        if (type != 0x23u) s_5CD4(ix, type);

        switch (type) {
        case 0x20u:                             /* final item (victory) */
            s_5CB5();
            g_room_y = (uint8_t)(g_room_y | 0x04u);  /* SET 2 */
            g_restart_flag = 1u;
            break;

        case 0x21u:                             /* special item: reload room */
            s_5CB5();
            room_full_load();
            g_restart_flag = 1u;
            break;

        case 0x22u:                             /* map */
            g_room_y = (uint8_t)(g_room_y | 0x08u);  /* SET 3 */
            minimap_draw_full();
            break;

        case 0x23u:                             /* power-up red (not deleted) */
            if (g_power_red == 0x0Au) break;
            g_power_red = 0x0Au;
            g_power_green = 0u;
            g_music_ctrl = 0u; g_music_data = 0u;
            music_load(0x79B7u, 0x79DEu);
            break;

        case 0x24u:                             /* power-up green */
            g_power_green = 0x10u;
            g_power_red = 0u;
            g_music_ctrl = 0u; g_music_data = 0u;
            music_load(0x7964u, 0x7993u);
            break;

        case 0x25u:                             /* door reset */
            g_door_reset = 1u;
            break;

        case 0x26u:                             /* extra life */
            s_5E4B();
            break;

        default:                                /* 0x27+: score / keys */
        {
            uint8_t idx = (uint8_t)(type - 0x27u);
            /* Score BCD from ROM table 0x6490 */
            {
                uint16_t pts = (uint16_t)(rom_rb((uint16_t)(0x6490u + (uint16_t)idx * 2u)) |
                              ((uint16_t)rom_rb((uint16_t)(0x6491u + (uint16_t)idx * 2u)) << 8));
                s_5D87(pts);
            }
            if (type >= 0x2Au) {                /* color key */
                uint16_t k = (uint16_t)(type - 0x2Au);
                if (k < 6u && g_keys[k] != 0xFFu) {
                    g_keys[k]++;
                    keys_hud_redraw();
                }
            }
            hud_scores();
            hud_lives();
            break;
        }
        }
    }
}

/* ==========================================================================
 * sub_4499: Item animation (called every frame)
 * Type 0x21: sprite plane 11 alternates 0x2A/0x2B, tiles delta 0-3/4-7
 * Type 0x23: color cycling (cosmetic, not yet implemented)
 * ========================================================================== */
void pickup_anim_frame(void)
{
    uint16_t ix = ITEM_TABLE_OFF;
    uint8_t s;
    for (s = 0u; s < 16u; s++, ix += 4u) {
        if (!g_object_table[ix]) continue;
        if (g_object_table[(uint16_t)(ix + 1u)] == 0x21u) {
            uint8_t h = g_object_table[(uint16_t)(ix + 2u)];
            uint8_t l = g_object_table[(uint16_t)(ix + 3u)];
            uint8_t ph = g_state_flags & 0x01u;
            uint8_t d = (uint8_t)(ph * 4u);
            cell_put(h,     l,     (uint8_t)(0x21u + d));
            cell_put((uint8_t)(h + 1u), l,     (uint8_t)(0x21u + d + 1u));
            cell_put(h,     (uint8_t)(l + 1u), (uint8_t)(0x21u + d + 2u));
            cell_put((uint8_t)(h + 1u), (uint8_t)(l + 1u), (uint8_t)(0x21u + d + 3u));
        }
    }
}

/* ==========================================================================
 * MINIMAP (cols 17-23, rows 0-3; rooms 10×10 grid)
 * ========================================================================== */
static void minimap_paint(uint8_t col, uint8_t fila, uint8_t color)
{
    uint8_t py = (uint8_t)(fila * 3u + 2u);
    int i;
    for (i = 0; i < 2; i++, py++) {
        uint8_t ch = (uint8_t)(0x0Fu + (col >> 1) + (py >> 3) * 7u);
        uint16_t addr = (uint16_t)(0x2000u + (uint16_t)ch * 8u + (py & 7u));
        uint8_t v = hal_vdp_read_vram(addr);
        if (col & 0x01u) v = (uint8_t)((v & 0xF0u) | color);
        else             v = (uint8_t)((v & 0x0Fu) | (uint8_t)(color << 4));
        hal_vdp_write_vram(addr, v);
    }
}

static void minimap_paint_bcd(uint8_t roomBcd, uint8_t color)
{
    minimap_paint((uint8_t)(roomBcd & 0x0Fu), (uint8_t)(roomBcd >> 4), color);
}

void minimap_room_exit_mark(void)
{
    uint8_t r, idx;
    uint16_t a;
    if (!(g_room_y & 0x08u)) return;           /* no map */
    r = g_room_x;
    idx = (uint8_t)((r >> 4) * 10u + (r & 0x0Fu));
    a = (uint16_t)(idx >> 3);
    if (a < 0x400u)
        g_map[a] = (uint8_t)(g_map[a] | (uint8_t)(1u << (7u - (idx & 7u))));
    minimap_paint_bcd(r, 7u);
}

void minimap_draw_full(void)
{
    uint8_t fila, col, t;
    /* Fill minimap chars (7×4 at cols 17-23, rows 0-3) */
    t = 0x0Eu;
    for (fila = 0u; fila < 4u; fila++)
        for (col = 0u; col < 7u; col++)
            nt_put((uint16_t)(fila * 32u + 17u + col), t++);
    /* Paint all 100 rooms from g_map bitfield */
    for (fila = 0u; fila < 10u; fila++)
        for (col = 0u; col < 10u; col++) {
            uint8_t idx = (uint8_t)(fila * 10u + col);
            uint16_t bit_a = (uint16_t)(idx >> 3);
            uint8_t bit = (bit_a < 0x400u)
                        ? (uint8_t)((g_map[bit_a] >> (7u - (idx & 7u))) & 1u)
                        : 0u;
            minimap_paint(col, fila, bit ? 7u : 4u);
        }
    /* Current room in white; room 09 special if g_room_y bit 1 */
    minimap_paint_bcd(g_room_x, 0x0Fu);
    if (g_room_y & 0x02u) minimap_paint_bcd(0x09u, 9u);
}

/* ==========================================================================
 * INIT
 * ========================================================================== */
void pickup_init(void)
{
    memset(g_keys, 0, sizeof(g_keys));
    g_power_red      = 0u;
    g_power_green    = 0u;
    g_music_ctrl     = 0u;
    g_music_tempo    = 0u;
    g_music_data     = 0u;
    g_sfx_hold       = 0u;
    g_sfx_timer      = 0u;
    g_sprite_timer   = 0u;
    g_door_reset     = 0u;
    memset(g_persist_buf, 0, sizeof(g_persist_buf));
}

/* ==========================================================================
 * Accessor for old doors.c (keys mask)
 * ========================================================================== */
uint8_t pickup_any_key(void)
{
    for (int i = 0; i < 6; i++)
        if (g_keys[i] > 0u) return 1u;
    return 0u;
}
