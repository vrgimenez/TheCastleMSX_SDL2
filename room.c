/*
 * THE CASTLE — Cargador de salas (sub_5053) + intérprete de scripts (sub_55F6)
 * =============================================================================
 *
 * Este archivo implementa el sistema de transición y carga de salas del juego.
 *
 * ARQUITECTURA
 * ------------
 * El juego tiene un mapa de salas en rejilla BCD:
 *
 *   g_room_x (0xE320) — columna BCD de la sala actual (dígito unidades)
 *   g_room_y (0xE321) — fila BCD + flags (bit 3 = flag especial)
 *
 *   Navegar arriba  → g_room_x -= 0x10 (BCD)
 *   Navegar derecha → g_room_x += 0x01 (BCD)
 *   Navegar abajo   → g_room_x += 0x10 (BCD)
 *   Navegar izquierda → g_room_x -= 0x01 (BCD)
 *
 * Cuando g_room_exit tiene un valor, sub_5053 ejecuta la transición:
 *   g_room_exit == 0x01 → salida por arriba    (sub_5084)
 *   g_room_exit == 0x03 → salida por la derecha (sub_5094)
 *   g_room_exit == 0x05 → salida por abajo      (sub_50A7)
 *   g_room_exit == 0x07 → salida por la izquierda (sub_50BA)
 *
 * SCRIPTS DE SALA
 * ---------------
 * Cada sala tiene un script en ROM que describe qué tiles se dibujan
 * en la name table del VDP. El formato es un bytecode propio:
 *
 *   0x3D col row   → mover cursor a (col, row) en la name table
 *   0x28           → activar modo "caps" (escala de caracteres alternativa)
 *   0x29           → desactivar caps
 *   0x20           → imprimir espacio (tile 0)
 *   0x21           → NOP (sin efecto)
 *   0x40           → FIN del script → setea g_restart_flag = 1
 *   byte >= 0x5D y <= 0xBF → imprimir tile = byte - 0x40
 *   byte >= 0xA6 y <= 0xBF → imprimir tile = byte - 0xA6 + 0x27
 *   byte >= 0xC0 y <= 0xDD → imprimir tile = byte - 0xC0 + 0x81
 *   byte >= 0xDE           → imprimir tile = byte - 0xA1 + 0x42
 *   0x3A..0x5C             → imprimir tile = byte - 0x40 + 0x5D (símbolo/dígito)
 *
 * Los scripts de sala conocidos en ROM:
 *   0x592D — pantalla de título (intro)
 *   0x598D — sala de inicio del juego ("THE END" pantalla de victoria)
 *   0x588D — sala de juego, variante 1
 *   0x58D1 — sala de juego, variante 2
 *   0x57DE — sala con flag especial (bit 3 de g_room_y activo)
 *   0x58D1 — idem variante 2
 *
 * CARGA DE SALA (sub_5382)
 * ------------------------
 * La carga completa de una sala hace en orden:
 *   1. sub_659B — limpiar tabla de sprites de enemigos (0xE946..0xEA65)
 *                 y tabla de objetos (0xE346..0xE495)
 *   2. sub_65C4 — limpiar tilemap (0xE496..0xE945, stride 30) y color table VDP
 *   3. sub_4E8E — recargar WALLS (28 tiles) + ANIM_BG (10 tiles) desde ROM→VRAM
 *   4. sub_549D — copiar datos gráficos de sala desde ROM a VRAM via BIOS_RDSLT
 *                 (los datos en 0xF91F/0xF920 = slot de cartucho extra)
 *   5. sub_4E91 — recargar tileset extra (BG1_MAIN en 0x7BC2, tercios 1 y 2)
 *   6. sub_64AB — cargar sprites de puerta (0x7BD8), tiles de jugador (0x7BD4)
 *   7. Inicializar contadores de animación (0xEA66=0xAF, 0xEA67=0x1A, 0xEA68=0)
 *   8. Resetear g_state_flags, g_transition, g_restart_flag, timers de chispa
 *
 * POSICIÓN DEL JUGADOR TRAS TRANSICIÓN
 * -------------------------------------
 * Según la dirección de salida, el jugador aparece en el borde opuesto:
 *   Salida arriba    → col=g_player_col, row=0x11 (fila 17 = fila inferior)
 *   Salida derecha   → col=0x00,         row=g_player_row
 *   Salida abajo     → col=g_player_col, row=0x00 (fila 0 = fila superior)
 *   Salida izquierda → col=0x1C,         row=g_player_row
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"
#include "game.h"

/* ==========================================================================
 * CONSTANTES
 * ========================================================================== */

#define ROM_ORG             0x4000u
#define VRAM_NAME_BASE      0x1800u
#define VRAM_PATTERN_BASE   0x0000u
#define VRAM_COLOR_BASE     0x2000u
#define VRAM_THIRD_SIZE     0x0800u

/* Descriptores de tilesets (para recarga entre salas) */
#define ROM_TS_BG1_MAIN     0x7BC2u
#define ROM_TS_WALLS        0x7BD0u
#define ROM_TS_ANIM_BG      0x7BCEu
#define ROM_TS_DOOR_EXTRA   0x7BD8u
#define ROM_TS_DOOR         0x7BD4u

/* Códigos de salida de sala */
#define EXIT_UP     0x01u
#define EXIT_RIGHT  0x03u
#define EXIT_DOWN   0x05u
#define EXIT_LEFT   0x07u

/* Opcodes del script de sala */
#define SCRIPT_SETPOS   0x3Du
#define SCRIPT_CAPS_ON  0x28u
#define SCRIPT_CAPS_OFF 0x29u
#define SCRIPT_SPACE    0x20u
#define SCRIPT_NOP      0x21u
#define SCRIPT_END      0x40u

/* Tamaño del mapa de tiles (0xE496): 30 filas × 30 columnas */
#define TILEMAP_COLS    30u
#define TILEMAP_ROWS    30u

/* Sprite-enemy table size */
#define SPRITE_TABLE_SIZE  0x0120u   /* 0xE946..0xEA65 */
#define OBJECT_TABLE_SIZE  0x0150u   /* 0xE346..0xE495 */
#define TILEMAP_SIZE       0x04B0u   /* 0xE496..0xE945 */

/* ==========================================================================
 * VARIABLES INTERNAS DE ESTADO DE SALA
 * ========================================================================== */

/* g_room_x / g_room_y — coordenadas BCD de la sala actual
 * Estos usan las mismas celdas de RAM que g_player_x / g_player_y
 * durante las transiciones (el juego los comparte, sección crítica). */
uint8_t g_room_x = 0x70u;   /* 0xE320 — BCD row=7, col=0 (inicio) */
uint8_t g_room_y = 0x01u;   /* 0xE321 — fila BCD + flags             */

/* Posición del jugador tras la transición */
/* (se copia desde g_player_col/g_player_row en sub_50CC: LDIR 0x12 bytes) */

/* Flag de sala especial: bit 3 de g_room_y */
static bool g_room_special;         /* 0xEAE2-like flag */

/* Script pointer para el intérprete (0xEAFE) */
static const uint8_t *g_script_ptr = NULL;

/* Script pointer auxiliar (0xEAFF, 0xEB03, 0xEB05) */
static const uint8_t *g_script_bg_ptr  = NULL;  /* 0xEB05 */
static const uint8_t *g_script_obj_ptr = NULL;  /* 0xEB08 */
static uint8_t        g_script_step    = 0;     /* 0xEB07 */
static uint8_t        g_script_caps    = 0;     /* 0xEB02 */

/* Cursor de escritura en name table (col=H, row=L en original) */
static uint8_t g_cursor_col = 0;
static uint8_t g_cursor_row = 0;

/* Tabla de tiles del mapa (0xE496): 30×30 */
uint8_t g_tilemap[TILEMAP_ROWS * TILEMAP_COLS];  /* 0xE496 */

/* Sprite/enemy y object tables (limpiadas en sub_659B) */
static uint8_t g_sprite_table[SPRITE_TABLE_SIZE];  /* 0xE946 */
static uint8_t g_object_table[OBJECT_TABLE_SIZE];  /* 0xE346 */

/* Contadores de animación por tercio (0xEA66, 0xEA67, 0xEA68) */
/* declarados en enemies.c como g_anim_ctr[] — aquí los referenciamos */
extern uint8_t g_anim_ctr[3];   /* enemies.c */

/* ==========================================================================
 * ROM ACCESS
 * ========================================================================== */

static inline uint8_t rom_rb(uint16_t addr)
{
    uint32_t off = (uint32_t)addr - ROM_ORG;
    if (!g_rom || off >= g_rom_size) return 0xFFu;
    return g_rom[off];
}

static inline uint16_t rom_rw(uint16_t addr)
{
    return (uint16_t)(rom_rb(addr) | ((uint16_t)rom_rb((uint16_t)(addr + 1u)) << 8));
}

/* ==========================================================================
 * sub_659B — Limpiar tablas de sprites y objetos
 *
 * Original:
 *   HL=0xE946, DE=0xE947, BC=0x011F → LDIR (limpia 0x120 bytes de sprites)
 *   HL=0xE346, DE=0xE347, BC=0x014F → LDIR (limpia 0x150 bytes de objetos)
 *   (0xEA66) = 0x72   ; anim counter tercio 0
 *   (0xEA67) = 0x1A   ; anim counter tercio 1
 *   (0xEA68) = 0x00   ; anim counter tercio 2
 * ========================================================================== */
static void clear_sprite_object_tables(void)
{
    memset(g_sprite_table, 0, sizeof(g_sprite_table));
    memset(g_object_table, 0, sizeof(g_object_table));

    /* Reiniciar contadores de animación por tercio */
    g_anim_ctr[0] = 0x72u;
    g_anim_ctr[1] = 0x1Au;
    g_anim_ctr[2] = 0x00u;
}

/* ==========================================================================
 * sub_65C4 — Limpiar VRAM color table + tilemap RAM
 *
 * Original:
 *   DE = (0xF3C7) = name_base (0x1800)
 *   HL = DE + 0x80 = 0x1880
 *   FILVRM(HL, 0x00, 0x0280)  → limpiar 640 bytes de color table en VRAM
 *   LDIR: HL=0xE496, DE=0xE497, BC=0x04AF → limpiar tilemap RAM
 * ========================================================================== */
static void clear_vram_and_tilemap(void)
{
    /* Limpiar los 3 tercios de la color table del VDP */
    for (int t = 0; t < 3; t++) {
        uint16_t base = (uint16_t)(VRAM_COLOR_BASE + (uint16_t)t * VRAM_THIRD_SIZE);
        hal_vdp_fill_vram(base, 0x00u, 0x0100u);
    }

    /* Limpiar tilemap en RAM */
    memset(g_tilemap, 0, sizeof(g_tilemap));
}

/* ==========================================================================
 * sub_659B + sub_65C4 combinadas = sub_5382 pasos 1 y 2
 * ========================================================================== */
static void room_clear_state(void)
{
    clear_sprite_object_tables();
    clear_vram_and_tilemap();
}

/* ==========================================================================
 * load_tileset_range — Carga N tiles de un tileset a VRAM (wrapper de sub_64AB)
 *
 * Delega a la función ya implementada en tiles.c.
 * Aquí solo declaramos el prototype para usarla.
 * ========================================================================== */
extern void tiles_reload_walls_and_anim(void);
extern void tiles_load_from_rom(const uint8_t *rom_data, uint32_t rom_size);

/* Carga N tiles de raw tile data desde ROM → VRAM
 * El descriptor almacena la dirección base de los datos raw (16 bytes interleaved
 * por tile). Los primeros 4 descriptores (0x7BC0-0x7BC6) tienen dirección completa.
 * Los descriptores 0x7BC8+ usan formato compacto (solo byte bajo) y requieren
 * que el byte alto se determine por contexto. */
static void load_tileset(uint16_t ts_desc_addr, uint16_t vram_idx,
                         uint8_t count)
{
    uint16_t base_addr;
    switch (ts_desc_addr) {
        case 0x7BC2: base_addr = 0x8056; break;  /* BG1_MAIN */
        case 0x7BCE: base_addr = 0x86F6; break;  /* ANIM_BG */
        case 0x7BD0: base_addr = 0x8796; break;  /* WALLS */
        case 0x7BD4: base_addr = 0x9A76; break;  /* DOOR */
        case 0x7BD8: base_addr = 0x9A96; break;  /* DOOR_EXTRA */
        default:     base_addr = rom_rw(ts_desc_addr); break;
    }
    tiles_rom_to_vram(base_addr, vram_idx, count);
}

/* ==========================================================================
 * ROOM SHAPE DECODER — port of Z80 sub_64DD / rl_load_room
 *
 * Decodes the geometric shape stream from ROM to build the name table
 * (VRAM tiles) and collision map (g_tilemap) for a room.
 *
 * Tile allocation per code (0..95) per third (0..2) is tracked in
 * g_tile_alloc[3][96]; counters in g_anim_ctr[3].
 * ========================================================================== */

/* Tile allocation table: tracks which VRAM tile index is assigned to each
 * (third, code) pair. 0 = unassigned (blank tile at VRAM index 0). */
static uint8_t g_tile_alloc[3][96];

/* Door slot index per cell (stored at colmap row 20+, matching Z80 0xE6EE) */
#define DOOR_SLOT_BASE_ROW  20u

/* Name-table VRAM address for play-area cell (h=col 0..29, l=row 0..19) */
static inline uint16_t nt_addr(uint8_t col, uint8_t row)
{
    return (uint16_t)(VRAM_NAME_BASE + (uint16_t)(row + 4u) * 32u + col);
}

/* ===== sub_6CB5 — Find or allocate tile slot for (code, third) ===== */
static uint8_t tile_alloc(uint8_t code, uint8_t third)
{
    uint8_t tile = g_tile_alloc[third][code];
    if (tile != 0u || code == 0u) return tile;
    tile = g_anim_ctr[third];
    if (tile >= 0x9Eu) return 0u;   /* out of VRAM tile space */
    g_anim_ctr[third] = (uint8_t)(tile + 1u);
    g_tile_alloc[third][code] = tile;
    return tile;
}

/* ===== sub_6B0B — Load tile graphics for a code in given third ===== */
static void tile_load_code(uint8_t code, uint8_t third)
{
    if (code == 0u) return;
    uint8_t tile = g_tile_alloc[third][code];
    if (tile != 0u) return;  /* already loaded */
    tile = tile_alloc(code, third);
    if (tile == 0u) return;

    /* Read descriptor: 3 bytes at 0x7BDA + code*3 */
    uint8_t pat_lo = rom_rb((uint16_t)(0x7BDAu + code * 3u));
    uint8_t pat_hi = rom_rb((uint16_t)(0x7BDAu + code * 3u + 1u));
    uint8_t color  = rom_rb((uint16_t)(0x7BDAu + code * 3u + 2u));

    uint8_t page = (pat_hi >> 5) & 7u;
    uint16_t pat = (uint16_t)(((uint16_t)(pat_hi & 0x1Fu) << 8) | pat_lo);
    uint16_t vram = (uint16_t)((uint16_t)third << 8) | tile;

    if (page < 4u) {
        static const uint32_t rom_pages[4] = {0x8796u, 0x86F6u, 0x8056u, 0x9A76u};
        if (page < 4u) {
            uint32_t rom_off = rom_pages[page] + (uint32_t)pat * 16u;
            tiles_load_interleaved_tile(rom_off, vram, false);
        }
    } else {
        /* Room-specific slot page: single-pattern expansion */
        uint16_t src_z80 = (uint16_t)(((uint16_t)(pat_hi & 0x03u) << 8) | pat_lo);
        src_z80 = (uint16_t)(src_z80 | 0x4000u);
        uint8_t pattern = rom_rb(src_z80);
        uint8_t pat_base = (uint16_t)third * 0x800u + (uint16_t)tile * 8u;
        uint8_t col_base = (uint16_t)third * 0x800u + (uint16_t)tile * 8u;
        for (uint8_t r = 0; r < 8u; r++) {
            hal_vdp_write_vram((uint16_t)(VRAM_PATTERN_BASE + pat_base + r), pattern);
            hal_vdp_write_vram((uint16_t)(VRAM_COLOR_BASE + col_base + r), color);
        }
    }
}

/* ===== sub_5E80 — Write colmap entry at (h,l) for code c ===== */
static void colmap_write(uint8_t col, uint8_t row, uint8_t code, uint8_t delta, uint8_t e)
{
    if (col >= 30u || row >= 20u) return;
    uint8_t cd = rom_rb((uint16_t)(0x7780u + (uint16_t)code * 2u));
    uint8_t ce = rom_rb((uint16_t)(0x7780u + (uint16_t)code * 2u + 1u));
    g_tilemap[(uint16_t)row * 30u + col] = (uint8_t)((delta & 0xC0u) | cd | (ce << 2));
    if (code == 0x0Du || code == 0x0Eu || code == 0x0Fu)
        g_tilemap[(DOOR_SLOT_BASE_ROW + (uint16_t)row) * 30u + col] = e;
}

/* ===== sub_6A7C — Cell writer: colmap + name table ===== */
static void cell_write(uint8_t col, uint8_t row, uint8_t code, uint8_t delta, uint8_t e)
{
    if (row >= 20u || col >= 30u) return;
    uint8_t third = (row < 4u) ? 0u : (row < 0x0Cu) ? 1u : 2u;
    uint8_t adj = (code == 0x0Du) ? 0x0Cu : code;
    if (g_tile_alloc[third][adj] == 0u) tile_load_code(adj, third);
    colmap_write(col, row, code, delta, e);
    hal_vdp_write_vram(nt_addr(col, row), (uint8_t)(g_tile_alloc[third][adj] + delta));
}

/* ===== sub_68B7 — Write 2-code pair from ROM table ===== */
static void pair_write(uint8_t col, uint8_t row, uint16_t rom_tbl, uint8_t e)
{
    cell_write(col, row, rom_rb(rom_tbl), rom_rb((uint16_t)(rom_tbl + 1u)), e);
    cell_write((uint8_t)(col + 1u), row, rom_rb((uint16_t)(rom_tbl + 2u)),
               rom_rb((uint16_t)(rom_tbl + 3u)), e);
}
/* sub_6888: pair from 0x6DEA + c*4 */
static void pair_simple(uint8_t col, uint8_t row, uint8_t c, uint8_t e)
{
    pair_write(col, row, (uint16_t)(0x6DEAu + (uint16_t)c * 4u), e);
}
/* sub_689A: pair from 0x6E1E + c*12 + phase*4 */
static void pair_phase(uint8_t col, uint8_t row, uint8_t c, uint8_t phase, uint8_t e)
{
    pair_write(col, row, (uint16_t)(0x6E1Eu + (uint16_t)c * 12u + (uint16_t)phase * 4u), e);
}
/* sub_686C: determine segment phase 0=start, 1=middle, 2=end */
static uint8_t seg_phase(uint16_t count, uint16_t orig_count)
{
    if (count == orig_count) return 0u;
    if (count == 1u) return 2u;
    return 1u;
}

/* ===== Cursor state for shape decoder ===== */
static uint8_t  sd_col, sd_row;      /* (H, L) cursor */
static uint16_t sd_count;            /* (DE) remaining repetitions */
static uint16_t sd_stream;           /* (0xEAD9) stream pointer (Z80 ROM addr) */
static uint8_t  sd_wall_var;         /* (0xEADF) last wall variant */

/* Forward declarations for mutually recursive shape functions */
static void adv_col(void);
static void adv_row(void);
static void s_66B0(uint8_t a);
static void s_6774(uint8_t a);
static void door_case(uint8_t *h, uint8_t *l, uint16_t de);
static void elevator_case(uint8_t *h, uint8_t *l, uint16_t de);

/* ===== sub_6A3A / sub_6A63 — Cursor advancement ===== */
static void adv_row(void)
{
    if (sd_col == 0x00u || sd_col == 0x1Cu) { adv_col(); return; }
    sd_col = (uint8_t)(sd_col + 2u);
    if (sd_col == 0x1Cu) { sd_col = 2u; sd_row++; }
}
static void adv_col(void)
{
    sd_row++;
    if (sd_row == 0x14u) { sd_row = 0u; sd_col = (uint8_t)(sd_col + 2u); }
}

/* ===== sub_6998 — Extended count (0xFF chain) ===== */
static uint16_t ext_count(uint16_t de)
{
    for (;;) {
        uint8_t b = rom_rb(++sd_stream);
        de = (uint16_t)(de + b);
        if (b != 0xFFu) return de;
    }
}

/* ===== sub_6616 — Read shape byte (3-bit count, 5-bit shape) ===== */
static uint8_t read_shape_6616(void)
{
    uint8_t cbyte = rom_rb(sd_stream);
    uint16_t de = (uint16_t)((cbyte & 7u) + 1u);
    if (de == 8u) de = ext_count(de);
    sd_count = de;
    sd_stream++;
    return (uint8_t)(cbyte >> 3);
}

/* ===== sub_6973 — Read shape byte (4-bit count, 4-bit shape) ===== */
static uint8_t read_shape_6973(void)
{
    uint8_t cbyte = rom_rb(sd_stream);
    uint16_t de = (uint16_t)((cbyte & 0x0Fu) + 1u);
    if (de == 0x10u) de = ext_count(de);
    sd_count = de;
    sd_stream++;
    return (uint8_t)(cbyte >> 4);
}

/* ===== sub_6055 — Elevator/ramp structural object registration ===== */
static void struct_register(uint8_t a, uint8_t col, uint8_t row, uint16_t de)
{
    uint8_t slot = g_object_table[0x14Fu];   /* 0xE495 = slot counter */
    g_object_table[0x14Fu] = (uint8_t)(slot + 1u);
    uint16_t ix = (uint16_t)(0xE43Eu - 0xE346u + slot * 5u);
    uint8_t t;
    g_object_table[ix] = a;
    g_object_table[ix + 1u] = col;
    g_object_table[ix + 2u] = row;
    g_object_table[ix + 3u] = (uint8_t)de;
    if (a == 0x0Cu)      t = 3u;
    else if (a == 0x0Du) t = 1u;
    else if (a < 0x1Cu)  t = 0u;
    else if (a < 0x1Fu)  t = 4u;
    else                 t = 1u;
    g_object_table[ix + 4u] = t;
}

/* ===== sub_66B0 — Row-major shape dispatcher ===== */
static void s_66B0(uint8_t a)
{
    uint8_t h = sd_col, l = sd_row;
    uint16_t de = sd_count;
    if (a >= 0x0Cu && a != 0x0Eu) struct_register(a, h, l, de);
    if (a == 0u) {
        while (de) { adv_row(); de--; }
    } else if (a < 4u) {
        sd_wall_var = a;
        while (de) {
            uint8_t d0 = (uint8_t)(l & 1u);
            cell_write(h, l, a, d0, (uint8_t)de);
            cell_write((uint8_t)(h + 1u), l, a, (uint8_t)(d0 ^ 1u), (uint8_t)de);
            adv_row(); de--;
        }
    } else if (a < 7u) {
        uint8_t c = (uint8_t)(a - 3u);
        while (de) { pair_simple(h, l, c, (uint8_t)de); adv_row(); de--; }
    } else if (a < 9u) {
        uint8_t c = (uint8_t)(a - 7u);
        while (de) { pair_phase(h, l, c, seg_phase(de, sd_count), (uint8_t)de); adv_row(); de--; }
    } else if (a == 9u) {
        while (de) { pair_simple(h, l, 4u, (uint8_t)de); adv_row(); de--; }
    } else if (a < 0x0Cu) {
        while (de) {
            cell_write(h, l, a, 0u, (uint8_t)de);
            cell_write((uint8_t)(h + 1u), l, a, 0u, (uint8_t)de);
            cell_write(h, (uint8_t)(l + 1u), a, 1u, (uint8_t)de);
            cell_write((uint8_t)(h + 1u), (uint8_t)(l + 1u), a, 1u, (uint8_t)de);
            adv_row(); de--;
        }
    } else {
        uint8_t c = (uint8_t)(a - 0x0Au);
        while (de) { pair_phase(h, l, c, seg_phase(de, sd_count), (uint8_t)de); adv_row(); de--; }
    }
    sd_col = h; sd_row = l; sd_count = de;
}

/* ===== sub_68CF — Wall variant lookup for doors ===== */
static void door_wall_var(uint8_t h, uint8_t l)
{
    uint8_t a = 0u;
    if (l > 0u) {
        uint8_t tbl = (l < 4u) ? 0u : (l < 0x0Cu) ? 1u : 2u;
        uint8_t tile = hal_vdp_read_vram(nt_addr(h, (uint8_t)(l - 1u)));
        int b;
        for (b = 3; b >= 1; b--) {
            uint8_t at = g_tile_alloc[tbl][(uint8_t)(4 - b)];
            if (at != 0u && (at == tile || (uint8_t)(at + 1u) == tile)) break;
        }
        if (b >= 1) a = (uint8_t)(4 - b);
    }
    if (a < 4u) { sd_wall_var = a; return; }
    a = (uint8_t)(sd_wall_var & 3u);
    sd_wall_var = a ? a : 3u;
}

/* ===== Door sub-shapes (case 6 of s_6774) ===== */
static void door_frame(uint8_t h, uint8_t l, uint8_t e, uint8_t bit)
{
    uint8_t c = (uint8_t)(sd_wall_var + 0x3Fu);
    cell_write(h, l, c, 0u, e);
    cell_write((uint8_t)(h + 1u), l, c, 1u, e);
    if (bit == 0u) return;
    c = (uint8_t)(e + (uint8_t)((uint8_t)(sd_wall_var - 1u) * 6u) + 0x41u);
    cell_write(h, l, c, 0u, e);
    cell_write((uint8_t)(h + 1u), l, c, 1u, e);
}
static void door_panel(uint8_t h, uint8_t l, uint8_t e, uint8_t bit)
{
    if (bit == 0u) return;
    uint8_t c = (uint8_t)(e + 0x53u);
    uint8_t eslot = (uint8_t)(g_object_table[0x14Bu] - 1u); /* 0xE491 */
    cell_write(h, l, c, 0u, eslot);
    cell_write((uint8_t)(h + 1u), l, c, 1u, eslot);
}
static void door_passage(uint8_t h, uint8_t l, uint8_t e)
{
    cell_write(h, l, 0x16u, 0u, e);
    cell_write((uint8_t)(h + 1u), l, 0x16u, 1u, e);
}

/* ===== sub_5FDF — Door registration ===== */
static uint8_t door_register(uint8_t h, uint8_t l, uint8_t e_count)
{
    uint8_t slot = g_object_table[0x14Bu]; /* 0xE491 */
    g_object_table[0x14Bu] = (uint8_t)(slot + 1u);
    uint16_t ix = (uint16_t)(0xE346u - 0xE346u + slot * 4u);
    uint8_t cat;
    if (h == 0x00u)      { g_object_table[0x148u]++; cat = 0u; } /* 0xE48E */
    else if (h == 0x1Cu) { g_object_table[0x149u]++; cat = 2u; } /* 0xE48F */
    else                 { g_object_table[0x14Au]++; cat = 1u; } /* 0xE490 */
    /* Persistence bitfield: g_map at 0xE00D + room_row*0x15 + room_col*2 + cat */
    {
        uint8_t rm = g_room_x;
        uint16_t bf = (uint16_t)((rm >> 4) * 0x15u + (rm & 0x0Fu) * 2u + cat);
        uint16_t bf_base = (uint16_t)(bf / 8u);
        uint8_t bit = (uint8_t)((g_map[bf_base] >> (7u - (bf & 7u))) & 1u);
        g_object_table[ix]     = bit;
        g_object_table[ix + 1u] = (uint8_t)((sd_wall_var << 4) | (uint8_t)(e_count - 1u));
        g_object_table[ix + 2u] = h;
        g_object_table[ix + 3u] = l;
        return bit;
    }
}

/* ===== sub_6774 — Column-major shape dispatcher ===== */
static void s_6774(uint8_t a)
{
    uint8_t h = sd_col, l = sd_row;
    uint16_t de = sd_count;
    if (a >= 0x0Cu && a != 0x0Eu) struct_register((uint8_t)(a + 0x10u), h, l, de);
    if (a == 0u) {
        while (de) { adv_col(); de--; }
    } else if (a < 5u) {
        uint8_t c = (uint8_t)(a + 4u);
        while (de) { pair_simple(h, l, c, (uint8_t)de); adv_col(); de--; }
    } else if (a == 5u) {
        while (de) { pair_phase(h, l, 6u, seg_phase(de, sd_count), (uint8_t)de); adv_col(); de--; }
    } else if (a == 6u) {
        door_case(&h, &l, de);
    } else if (a < 0x0Bu) {
        uint8_t c = (uint8_t)(a + 2u);
        while (de) { pair_simple(h, l, c, (uint8_t)de); adv_col(); de--; }
    } else if (a == 0x0Bu) {
        elevator_case(&h, &l, de);
    } else if (a < 0x0Fu) {
        uint8_t c = (uint8_t)(a - 4u);
        while (de) { pair_phase(h, l, c, seg_phase(de, sd_count), (uint8_t)de); adv_col(); de--; }
    } else {
        uint8_t c = (uint8_t)(a - 3u);
        while (de) { pair_simple(h, l, c, (uint8_t)de); adv_col(); de--; }
    }
    sd_col = h; sd_row = l; sd_count = de;
}

/* ===== sub_6664 — Dispatch border shape ===== */
static void s_6664(uint8_t shape)
{
    if (shape != 0u && shape < 0x10u) { s_6774(shape); return; }
    s_66B0((uint8_t)(shape & 0x0Fu));
}

/* ===== sub_67C5 — Door case handler (modifies cursor via pointers) ===== */
static void door_case(uint8_t *h, uint8_t *l, uint16_t de)
{
    uint8_t e = (uint8_t)de;
    if (e != 1u) {
        uint8_t bit;
        door_wall_var(*h, *l);
        bit = door_register(*h, *l, e);
        door_frame(*h, *l, e, bit);
        sd_col = *h; sd_row = *l; adv_col(); *h = sd_col; *l = sd_row;
        door_panel(*h, *l, e, bit);
        sd_col = *h; sd_row = *l; adv_col(); *h = sd_col; *l = sd_row;
        door_panel(*h, *l, e, bit);
        sd_col = *h; sd_row = *l; adv_col(); *h = sd_col; *l = sd_row;
        if (*h != 0x00u && *h != 0x1Cu) return;
    }
    sd_col = *h; sd_row = *l;
    door_passage(*h, *l, e);
    sd_col = *h; sd_row = *l; adv_col(); *h = sd_col; *l = sd_row;
}

/* ===== sub_6814 — Elevator case handler ===== */
static void elevator_case(uint8_t *h, uint8_t *l, uint16_t de)
{
    uint8_t lbot = (uint8_t)(*l + (uint8_t)((uint8_t)de - 1u));
    uint8_t tile = hal_vdp_read_vram(nt_addr(*h, lbot));
    uint8_t c;
    if (tile == 0u) struct_register(0x1Bu, *h, lbot, de);
    c = tile ? 0x0Bu : 0x07u;
    while (de) {
        sd_col = *h; sd_row = *l;
        pair_phase(*h, *l, c, seg_phase(de, sd_count), (uint8_t)de);
        sd_col = *h; sd_row = *l; adv_col(); *h = sd_col; *l = sd_row;
        de--;
    }
    if (tile == 0u)
        pair_simple(*h, (uint8_t)(*l + 3u), 0x0Bu, 0u);
}

/* ===== sub_6616_loop — Border band stream loop ===== */
static void s_6616_loop(void)
{
    for (;;) {
        uint8_t shape = read_shape_6616();
        if (shape == 7u && sd_col == 0x1Cu)      shape = 8u;
        else if (shape == 8u && sd_col == 0x00u) shape = 7u;
        s_6664(shape);
        if (sd_col == 0x02u || sd_col == 0x1Eu) return;
    }
}

/* ===== sub_6671 — Body + objects stream ===== */
static void s_6671(uint16_t ptr)
{
    sd_col = 2u; sd_row = 0u; sd_count = 0u;
    sd_stream = ptr;
    /* Pass 1: row-major shapes (sub_66A2) */
    while (sd_row != 0x14u) s_66B0(read_shape_6973());
    /* Pass 2: column-major shapes (sub_6766) */
    sd_col = 2u; sd_row = 0u; sd_count = 0u;
    sd_stream = ptr;
    while (sd_col != 0x1Cu) s_6774(read_shape_6973());
    /* Pass 3: object placement (sub_69AA) */
    sd_stream = ptr;
    for (;;) {
        uint8_t b0 = rom_rb(sd_stream);
        if (b0 == 0u) return;
        sd_col = (uint8_t)((b0 & 0x0Fu) << 1);
        sd_stream++;
        uint8_t b1 = rom_rb(sd_stream);
        sd_row = (uint8_t)(b1 & 0x1Fu);
        uint8_t cflags = (uint8_t)((b0 >> 4) | (b1 & 0xE0u));
        sd_stream++;
        uint8_t code = (uint8_t)((cflags & 0x0Fu) + ((cflags & 0x40u) ? 0x20u : 0x30u));
        uint8_t slot;
        /* Persistence bit lookup */
        uint8_t bit;
        if (code < 0x30u) {
            slot = g_object_table[0x14Du]; /* 0xE493 */
            g_object_table[0x14Du] = (uint8_t)(slot + 1u);
            uint16_t ix = (uint16_t)(0xE3D6u - 0xE346u + slot * 4u);
            uint8_t room_idx = (uint8_t)(((g_room_x >> 4) & 0x0Fu) * 10u + (g_room_x & 0x0Fu));
            uint16_t bf_base = (uint16_t)(0xE1A7u - 0xE000u + (uint16_t)room_idx * 2u);
            bit = (uint8_t)((g_map[bf_base] >> (7u - (slot & 7u))) & 1u);
            g_object_table[ix] = bit;
            g_object_table[ix + 1u] = code;
            g_object_table[ix + 2u] = sd_col;
            g_object_table[ix + 3u] = sd_row;
        } else if (code < 0x36u) {
            slot = g_object_table[0x14Cu]; /* 0xE492 */
            g_object_table[0x14Cu] = (uint8_t)(slot + 1u);
            uint16_t ix = (uint16_t)(0xE386u - 0xE346u + slot * 5u);
            uint8_t room_idx = (uint8_t)(((g_room_x >> 4) & 0x0Fu) * 10u + (g_room_x & 0x0Fu));
            uint16_t bf_base = (uint16_t)(0xE0DFu - 0xE000u + (uint16_t)room_idx * 2u);
            bit = (uint8_t)((g_map[bf_base] >> (7u - (slot & 7u))) & 1u);
            g_object_table[ix] = bit;
            g_object_table[ix + 1u] = code;
            g_object_table[ix + 2u] = sd_col;
            g_object_table[ix + 3u] = sd_row;
            g_object_table[ix + 4u] = 0u;
        } else {
            slot = g_object_table[0x14Eu]; /* 0xE494 */
            g_object_table[0x14Eu] = (uint8_t)(slot + 1u);
            uint16_t ix = (uint16_t)(0xE416u - 0xE346u + slot * 5u);
            uint8_t room_idx = (uint8_t)(((g_room_x >> 4) & 0x0Fu) * 10u + (g_room_x & 0x0Fu));
            uint16_t bf_base = (uint16_t)(0xE26Fu - 0xE000u + room_idx);
            bit = (uint8_t)((g_map[bf_base] >> (7u - (slot & 7u))) & 1u);
            g_object_table[ix] = bit;
            g_object_table[ix + 1u] = code;
            g_object_table[ix + 2u] = sd_col;
            g_object_table[ix + 3u] = sd_row;
            g_object_table[ix + 4u] = (uint8_t)((cflags & 0x20u) ? 3u : 1u);
        }
        if (bit == 0u) continue;
        if (code == 0x36u) {
            cell_write(sd_col, (uint8_t)(sd_row + 1u), code, 0u, slot);
            cell_write((uint8_t)(sd_col + 1u), (uint8_t)(sd_row + 1u), code, 1u, slot);
            continue;
        }
        if (code == 0x23u) {
            cell_write(sd_col, sd_row, code, 0u, slot);
            cell_write((uint8_t)(sd_col + 1u), sd_row, code, 0u, slot);
            cell_write((uint8_t)(sd_col + 1u), (uint8_t)(sd_row + 1u), code, 0u, slot);
            cell_write(sd_col, (uint8_t)(sd_row + 1u), code, 0u, slot);
            continue;
        }
        /* code < 0x23 or 0x24-0x29: 4-tile block with optional delta 4 */
        {
            uint8_t d = (uint8_t)((cflags & 0x20u) ? 4u : 0u);
            cell_write(sd_col, sd_row, code, d, slot);
            cell_write((uint8_t)(sd_col + 1u), sd_row, code, (uint8_t)(d + 1u), slot);
            cell_write(sd_col, (uint8_t)(sd_row + 1u), code, (uint8_t)(d + 2u), slot);
            cell_write((uint8_t)(sd_col + 1u), (uint8_t)(sd_row + 1u), code, (uint8_t)(d + 3u), slot);
        }
    }
}

/* ===== sub_65E1 — Stream pointer from room table ===== */
static uint16_t stream_ptr(uint16_t off)
{
    uint16_t p = (uint16_t)(0x7CF2u + off);
    uint16_t v = rom_rw(p);
    return v;
}

/* ===== room_decode_shapes — Main entry: decode room from shape streams =====
 *
 * Equivalent to Z80 sub_64DD / DF0 rl_load_room.
 * Fills name table rows 4-23 and colmap g_tilemap[0..19][0..29].
 * WALLS/ANIM_BG must already be loaded to all 3 thirds.
 * ========================================================================== */
static void room_decode_shapes(void)
{
    uint8_t room = g_room_x;

    /* Clear sprite + object tables */
    memset(g_tile_alloc, 0, sizeof(g_tile_alloc));
    memset(g_sprite_table, 0, sizeof(g_sprite_table));
    memset(g_object_table, 0, sizeof(g_object_table));
    g_anim_ctr[0] = 0x72u; g_anim_ctr[1] = 0x1Au; g_anim_ctr[2] = 0x00u;

    /* Fill blank tile (tile 0) in all 3 thirds with zeros */
    for (int t = 0; t < 3; t++) {
        uint16_t base = (uint16_t)((uint16_t)t * 0x800u);
        for (uint8_t r = 0; r < 8u; r++) {
            hal_vdp_write_vram((uint16_t)(VRAM_PATTERN_BASE + base + r), 0x00u);
            hal_vdp_write_vram((uint16_t)(VRAM_COLOR_BASE + base + r), 0x00u);
        }
    }

    /* Clear name table rows 4-23 + colmap */
    hal_vdp_fill_vram((uint16_t)(VRAM_NAME_BASE + 0x80u), 0x00u, 0x0280u);
    memset(g_tilemap, 0, sizeof(g_tilemap));

    /* Stream offset = 42 * row + 4 * col (BCD room coordinates) */
    uint16_t off = (uint16_t)(42u * (room >> 4) + 4u * (room & 0x0Fu));

    /* Top border band: starts at col=0, row=0 */
    sd_col = 0x00u; sd_row = 0x00u; sd_count = 0u;
    sd_stream = stream_ptr(off);
    s_6616_loop();

    /* Bottom border band: starts at col=0x1C, row=0 */
    sd_col = 0x1Cu; sd_row = 0x00u; sd_count = 0u;
    sd_stream = stream_ptr((uint16_t)(off + 4u));
    s_6616_loop();

    /* Body + objects */
    s_6671(stream_ptr((uint16_t)(off + 2u)));

    /* Load tile patterns for all allocated tiles */
    for (int t = 0; t < 3; t++) {
        for (int c = 0; c < 96; c++) {
            if (g_tile_alloc[t][c] != 0u) {
                uint8_t code = (uint8_t)c;
                uint8_t pat_lo = rom_rb((uint16_t)(0x7BDAu + code * 3u));
                uint8_t pat_hi = rom_rb((uint16_t)(0x7BDAu + code * 3u + 1u));
                uint8_t page = (pat_hi >> 5) & 7u;
                uint16_t pat = (uint16_t)(((uint16_t)(pat_hi & 0x1Fu) << 8) | pat_lo);
                uint16_t vram = (uint16_t)((uint16_t)t << 8) | g_tile_alloc[t][code];
                if (page < 4u) {
                    static const uint32_t rom_pages[4] = {0x8796u, 0x86F6u, 0x8056u, 0x9A76u};
                    uint32_t rom_off = rom_pages[page] + (uint32_t)pat * 16u;
                    tiles_load_interleaved_tile(rom_off, vram, false);
                }
            }
        }
    }
}

/* ==========================================================================
 * sub_5382 — Carga completa de una sala
 *
 * Llamado desde los loaders de sala (sub_51D9, sub_53D4, etc.)
 * Ejecuta la secuencia completa de inicialización de sala.
 * ========================================================================== */
static void room_full_load(void)
{
    /* Paso 1+2: limpiar tablas */
    room_clear_state();

    /* Paso 3: tercios 0+1+2 reciben WALLS(26)+ANIM_BG(10) (sub_4E8E)
     * Z80: WALLS @ 0x59-0x72, ANIM_BG @ 0x47-0x50 en CADA tercio. */
    tiles_reload_walls_and_anim();                  /* tercio 0 (TILE_MAP) */
    tiles_rom_to_vram(0x8796u, 0x0159u, 26u);      /* tercio 1: WALLS */
    tiles_rom_to_vram(0x86F6u, 0x0147u, 10u);      /* tercio 1: ANIM_BG */
    tiles_rom_to_vram(0x8796u, 0x0259u, 26u);      /* tercio 2: WALLS */
    tiles_rom_to_vram(0x86F6u, 0x0247u, 10u);      /* tercio 2: ANIM_BG */

    /* Decode shape streams from ROM -> name table + colmap */
    room_decode_shapes();

    /* Reset contadores globales */
    g_state_flags  = 0;
    g_transition   = 0;
    g_restart_flag = 0;
}

/* ==========================================================================
 * sub_55F6 — Intérprete de script de sala (1 byte por frame)
 *
 * Se llama una vez por frame desde el loop de transición.
 * Lee el siguiente byte del script (g_script_ptr) y lo ejecuta.
 *
 * Retorna: true si el script terminó (g_restart_flag seteado).
 *
 * El script usa un cursor (g_cursor_col, g_cursor_row) que avanza
 * automáticamente después de cada tile impreso (INC H en original).
 * ========================================================================== */
static bool script_step(void)
{
    if (!g_script_ptr) return true;

    uint8_t b = *g_script_ptr++;

    switch (b) {
        case SCRIPT_END:
            /* 0x40 → fin de script → g_restart_flag = 1 */
            g_restart_flag = 1;
            return true;

        case SCRIPT_SETPOS: {
            /* 0x3D col row → mover cursor */
            g_cursor_col = *g_script_ptr++;
            g_cursor_row = *g_script_ptr++;
            return false;
        }

        case SCRIPT_CAPS_ON:
            g_script_caps = 1u;
            return false;

        case SCRIPT_CAPS_OFF:
            g_script_caps = 0u;
            return false;

        case SCRIPT_NOP:
            /* 0x21 = NOP */
            return false;

        case SCRIPT_SPACE: {
            /* 0x20 = espacio → tile 0 */
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE
                            + (uint16_t)g_cursor_row * 32u
                            + g_cursor_col);
            hal_vdp_write_vram(addr, 0x00u);
            g_cursor_col++;
            return false;
        }

        default:
            break;
    }

    /* Traducción byte→tile según sub_55F6 (Z80) */
    uint8_t tile;

    if (b < 0x3Au) {
        /* b < 0x3A (no 0x20/21/28/29): tile = b - 0x13 */
        tile = (uint8_t)(b - 0x13u);
    } else if (b < 0x5Du) {
        /* 0x3A <= b <= 0x5C: tile = b - 0x40 */
        tile = (uint8_t)(b - 0x40u);
    } else if (g_script_caps == 0u) {
        /* caps == 0, b >= 0x5D: tile = b - 0x5F */
        tile = (uint8_t)(b - 0x5Fu);
    } else if (b >= 0xC0u) {
        /* caps == 1, b >= 0xC0: tile = b - 0x3F */
        tile = (uint8_t)(b - 0xC0u + 0x81u);
    } else if (b >= 0xA6u) {
        /* caps == 1, 0xA6 <= b < 0xC0: tile = b - 0x7F */
        tile = (uint8_t)(b - 0xA6u + 0x27u);
    } else {
        /* caps == 1, 0x5D <= b < 0xA6: tile = b - 0x5F */
        tile = (uint8_t)(b - 0x5Fu);
    }

    /* Escribir tile en la name table del VDP */
    if (g_cursor_col < 32u && g_cursor_row < 24u) {
        uint16_t addr = (uint16_t)(VRAM_NAME_BASE
                        + (uint16_t)g_cursor_row * 32u
                        + g_cursor_col);
        hal_vdp_write_vram(addr, tile);
    }
    g_cursor_col++;

    return false;
}

/* ==========================================================================
 * Tabla de scripts de sala en ROM
 *
 * sub_518E itera la tabla en 0x5748 buscando la sala que coincida con
 * (g_room_x, g_player_col, g_player_row) y llama el script correspondiente.
 *
 * Formato de cada entrada (5 bytes):
 *   [0] room_x   — valor BCD de g_room_x (0xFF = fin de tabla)
 *   [1] col      — columna del jugador para este evento
 *   [2] row      — fila del jugador para este evento
 *   [3] lo       — LSB del puntero al script/sub-rutina
 *   [4] hi       — MSB del puntero al script/sub-rutina
 *
 * Los punteros apuntan a subrutinas de la ROM:
 *   0x53D4 — loader de sala estándar (sin flags especiales)
 *   0x5431 — loader de sala con flag especial (bit 3 de g_room_y)
 * ========================================================================== */
#define ROOM_TABLE_ADDR  0x5748u
#define ROOM_ENTRY_SIZE  5u

/* Estructura de una entrada de la tabla de salas */
typedef struct {
    uint8_t  room_x;      /* valor BCD de g_room_x */
    uint8_t  col;         /* columna del jugador */
    uint8_t  row;         /* fila del jugador */
    uint16_t script_ptr;  /* puntero al loader */
} RoomEntry;

/* Scripts conocidos indexados por sala */
static const struct {
    uint16_t title_script;
    uint16_t game_start_script;
    uint16_t intro_bg_script_a;
    uint16_t intro_bg_script_b;
    uint16_t special_script_a;
    uint16_t special_script_b;
} g_known_scripts = {
    .title_script       = 0x592Du,
    .game_start_script  = 0x58D1u,   /* script sala inicial (0x70=especial) */
    .intro_bg_script_a  = 0x5887u,   /* sub_53D4: tabla bg ptrs (estándar) */
    .intro_bg_script_b  = 0x588Au,   /* sub_5431: tabla bg ptrs (especial) */
    .special_script_a   = 0x588Du,   /* sub_53D4: script sala (estándar)   */
    .special_script_b   = 0x58D1u,   /* sub_5431: script sala (especial)   */
};

/* ==========================================================================
 * sub_518E — Buscar en la tabla de salas la entrada correspondiente
 *            y ejecutar el loader de sala.
 *
 * Original:
 *   IX = 0x5748      ; inicio de tabla
 * loop:
 *   C = (IX+0)       ; room_x
 *   CP 0xFF → RET    ; fin de tabla
 *   A = (0xe320)     ; g_room_x actual
 *   CP C → JR NZ    ; no coincide → siguiente
 *   A = (0xe334); CP (IX+1) → JR NZ  ; comprobar col
 *   A = (0xe335); CP (IX+2) → JR NZ  ; comprobar row
 *   L = (IX+3); H = (IX+4) → HL = script_ptr
 *   CALL sub_4262    ; ejecutar loader (HL = ptr función)
 *   CALL 0xEAFA      ; hook de post-carga
 *   RET
 *   IX += 5; JR loop
 * ========================================================================== */
static bool room_table_lookup(void)
{
    uint16_t ix = ROOM_TABLE_ADDR;

    while (true) {
        uint8_t entry_room = rom_rb(ix);
        if (entry_room == 0xFFu) break;   /* fin de tabla */

        uint8_t entry_col = rom_rb((uint16_t)(ix + 1u));
        uint8_t entry_row = rom_rb((uint16_t)(ix + 2u));

        if (entry_room == g_room_x &&
            entry_col  == g_player_col &&
            entry_row  == g_player_row)
        {
            uint16_t loader_ptr = rom_rw((uint16_t)(ix + 3u));
            /* En el original: CALL sub_4262 que hace JP (HL) = llamar al loader */
            /* Los loaders conocidos son sub_53D4 (0x53D4) y sub_5431 (0x5431) */
            (void)loader_ptr;  /* se usa en room_load_by_exit() abajo */
            return true;
        }

        ix = (uint16_t)(ix + ROOM_ENTRY_SIZE);
    }
    return false;
}

/* ==========================================================================
 * room_advance_coord — Avanzar coordenadas BCD de sala según la dirección
 *
 * Equivale a sub_5084/5094/50A7/50BA.
 * BCD addition/subtraction con DAA.
 * ========================================================================== */
static void room_advance_coord(uint8_t exit_dir)
{
    uint8_t rx = g_room_x;

    switch (exit_dir) {
        case EXIT_UP: {
            /* SUB 0x10 / DAA — decrementar decena (fila BCD) */
            uint8_t hi = (rx >> 4) & 0x0Fu;
            uint8_t lo = rx & 0x0Fu;
            if (hi == 0) hi = 9; else hi--;
            g_room_x = (uint8_t)((hi << 4) | lo);
            /* Jugador aparece en la fila inferior */
            g_player_row = 0x11u;
            break;
        }
        case EXIT_RIGHT: {
            /* ADD 0x01 / DAA — incrementar unidad (columna BCD) */
            uint8_t hi = (rx >> 4) & 0x0Fu;
            uint8_t lo = rx & 0x0Fu;
            if (lo == 9) { lo = 0; hi++; } else lo++;
            g_room_x = (uint8_t)((hi << 4) | lo);
            /* Jugador aparece en la columna izquierda */
            g_player_col = 0x00u;
            break;
        }
        case EXIT_DOWN: {
            /* ADD 0x10 / DAA — incrementar decena */
            uint8_t hi = (rx >> 4) & 0x0Fu;
            uint8_t lo = rx & 0x0Fu;
            if (hi == 9) hi = 0; else hi++;
            g_room_x = (uint8_t)((hi << 4) | lo);
            /* Jugador aparece en la fila superior */
            g_player_row = 0x00u;
            break;
        }
        case EXIT_LEFT: {
            /* SUB 0x01 / DAA — decrementar unidad */
            uint8_t hi = (rx >> 4) & 0x0Fu;
            uint8_t lo = rx & 0x0Fu;
            if (lo == 0) { lo = 9; if (hi > 0) hi--; } else lo--;
            g_room_x = (uint8_t)((hi << 4) | lo);
            /* Jugador aparece en la columna derecha */
            g_player_col = 0x1Cu;
            break;
        }
        default:
            break;
    }
}

/* ==========================================================================
 * sub_5053 — Transición de sala (llamada cuando g_room_exit != 0)
 *
 * Original:
 *   OR A; RET Z         ; si g_room_exit == 0 → no hacer nada
 *   PUSH AF             ; guardar exit_dir
 *   CALL sub_6134       ; limpiar/restaurar estado previo de la sala
 *   A = (0xe321)        ; g_room_y flags
 *   BIT 3,A             ; flag especial?
 *   JR Z, sub_506B      ; no: saltar limpieza de map
 *   CALL sub_61E8       ; sí: restaurar mapa original
 *   C=0x07; A=(0xe320); CALL sub_63FD  ; actualizar color del borde
 * sub_506B:
 *   A = (0xeae2)        ; ¿limpieza de mapa pendiente?
 *   OR A; JR Z          ; no → sub_507E
 *   LDIR 0xD1 bytes (limpia mapa 0xE00D..0xE0DE)
 * sub_507E:
 *   POP AF              ; recuperar exit_dir
 *   CP 0x01 → sub_5084  ; arriba
 *   CP 0x03 → sub_5094  ; derecha
 *   CP 0x05 → sub_50A7  ; abajo
 *   CP 0x07 → sub_50BA  ; izquierda
 * sub_50CC:
 *   LDIR 0x12 bytes (copia posición nueva del jugador a 0xE322)
 *   BIT 3,(0xe321); si activo → CALL sub_63FD (actualizar color)
 *   POP AF; RET
 * ========================================================================== */
void room_transition(void)
{
    uint8_t exit_dir = g_room_exit;
    if (exit_dir == 0u) return;

    /* Limpiar estado anterior */
    room_clear_state();

    /* Avanzar coordenadas BCD de sala */
    room_advance_coord(exit_dir);

    /* Cargar nueva sala */
    room_full_load();

    /* Seleccionar script de la nueva sala */
    g_room_special = (g_room_y & 0x08u) != 0u;

    if (g_room_special) {
        /* Sala con flag especial: sub_5431 (script=0x58D1, bg=0x588A) */
        g_script_ptr    = g_rom + (g_known_scripts.special_script_b - ROM_ORG);
        g_script_bg_ptr = g_rom + (g_known_scripts.intro_bg_script_b - ROM_ORG);
    } else {
        /* Sala estándar: sub_53D4 (script=0x588D, bg=0x5887) */
        g_script_ptr    = g_rom + (g_known_scripts.special_script_a - ROM_ORG);
        g_script_bg_ptr = g_rom + (g_known_scripts.intro_bg_script_a - ROM_ORG);
    }

    g_cursor_col  = 0;
    g_cursor_row  = 0;
    g_script_caps = 0;
    g_script_step = 0;

    /* Animar transición: ejecutar el script un tick por frame */
    g_restart_flag = 0;
    while (!g_restart_flag) {
        script_step();
        g_state_flags++;
        hal_wait_vsync();
        if (!hal_poll_events()) {
            g_room_exit = 0;
            return;
        }
    }

    /* Limpiar flag de salida */
    g_room_exit    = 0;
    g_restart_flag = 0;

    /* Dibujar HUD */
    extern void draw_hud(void);
    draw_hud();
}

/* ==========================================================================
 * room_load_initial — Carga la sala inicial del juego (sub_51D9)
 *
 * Llamado desde game_init() para preparar la primera sala visible.
 *
 * sub_51D9:
 *   (0xE336)++          ; incrementar contador de salas visitadas
 *   CALL sub_5382       ; carga completa
 *   HL=0x0000, C=0x02   ; dibujar fila de tiles decorativos
 *   CALL sub_52DC       ; loop de dibujado horizontal
 *   ... (varios sub_52F2 para dibujar bordes y marcos)
 *   LD (0xe334)=0x02    ; col del jugador = 2
 *   LD (0xe335)=0x11    ; row del jugador = 17
 *   CALL sub_6F45       ; calcular pixel del jugador
 *   CALL sub_6F27(A=6)  ; dibujar sprite del jugador (frame 6 = parado)
 *   LD (0xeafe) = script de la sala  ; cargar puntero de script
 *   LD (0xeb08) = obj_script_ptr
 *   (loop: CALL sub_5128 / sub_5503 / sub_55F6 hasta g_restart_flag)
 * ========================================================================== */
void room_load_initial(void)
{
    /* Coordenadas iniciales del castillo */
    g_room_x = 0x70u;   /* BCD row=7, col=0 */
    g_room_y = 0x01u;   /* fila BCD 1 */

    /* Carga completa de sala */
    room_full_load();

    /* Posición inicial del jugador (sub_51D9: col=2, row=17) */
    g_player_col = 6u;      /* sub_4D52: LD (0xE333),6 */
    g_player_row = 0x11u;   /* 17 */

    /* Seleccionar script de la sala inicial */
    g_script_ptr    = g_rom + (g_known_scripts.game_start_script - ROM_ORG);
    g_script_bg_ptr = g_rom + (g_known_scripts.intro_bg_script_b - ROM_ORG);
    g_cursor_col    = 0;
    g_cursor_row    = 0;
    g_script_caps   = 0;
    g_restart_flag  = 0;

    /* Dibujar HUD */
    extern void draw_hud(void);
    draw_hud();
}

/* ==========================================================================
 * room_script_tick — Ejecutar un tick del script de sala (llamar 1×/frame)
 *
 * Equivale al CALL sub_55F6 dentro del loop de sala.
 * Retorna true cuando el script terminó (g_restart_flag seteado).
 * ========================================================================== */
bool room_script_tick(void)
{
    if (g_restart_flag) return true;
    return script_step();
}

/* ==========================================================================
 * room_load_title — Cargar la pantalla de título (sub_522A)
 *
 * sub_522A:
 *   (0xeafe) = 0x592D   ; script del título
 *   (0xeb05) = 0x5887   ; script de background de título
 *   (0xeb07) = 0        ; step counter
 *   (0xeae3) = 0        ; restart_flag = 0
 *   (0xeb08) = 0x5A02   ; script de objetos del título
 *   CALL sub_5128       ; 1 frame
 *   CALL sub_5503       ; tick de script de objetos
 *   CALL sub_55F6       ; tick de script de sala
 *   (0xeac9)++          ; incrementar frame counter
 *   JR Z, loop          ; mientras restart_flag == 0
 * ========================================================================== */
void room_load_title(void)
{
    g_script_ptr    = g_rom + (g_known_scripts.title_script - ROM_ORG);
    g_script_bg_ptr = g_rom + (g_known_scripts.intro_bg_script_a - ROM_ORG);
    g_script_step   = 0;
    g_restart_flag  = 0;
    g_cursor_col    = 0;
    g_cursor_row    = 0;
    g_script_caps   = 0;
}

/* ==========================================================================
 * room_init — Inicializar el sistema de salas
 * ========================================================================== */
void room_init(void)
{
    g_room_x        = 0x70u;
    g_room_y        = 0x01u;
    g_room_special  = false;
    g_script_ptr    = NULL;
    g_script_bg_ptr = NULL;
    g_script_step   = 0;
    g_script_caps   = 0;
    g_cursor_col    = 0;
    g_cursor_row    = 0;

    memset(g_sprite_table, 0, sizeof(g_sprite_table));
    memset(g_object_table, 0, sizeof(g_object_table));
    memset(g_tilemap,      0, sizeof(g_tilemap));
    memset(g_map,          0, sizeof(g_map));
    /* Persistence bitfields: all 1 = everything present (uncollected) */
    memset(g_map + 0x00D, 0xFF, 0x2C6);
}
