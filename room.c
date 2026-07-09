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

    /* Pasos 4+6: patrones de sala a tercios 1+2 (sub_549D).
     * Z80: 120 tiles desde (slotpage)+0x0430, color 0xF0.
     * Tercio 0 no toca — queda con TILE_MAP. */
    load_tileset(ROM_TS_BG1_MAIN, 0x0127u, 28u);   /* tercio 1: 0x27-0x42 */
    load_tileset(ROM_TS_BG1_MAIN, 0x0227u, 28u);   /* tercio 2: 0x27-0x42 */
    /* FIXME: tiles 0x43-0x9E no se cargan — falta (0xF920)
     * En Z80 vienen de sub_549D con fuente = *(uint16*)0xF920+0x0430.
     * Hasta entonces los scripts de sala referencian tiles fuera de
     * BG1_MAIN (paredes en 0x59-0x72 etc.) y se ven basura. */

    /* Pasos 4+6b: variantes de pared 0x73-0x76 a tercio 0.
     * TILE_MAP las carga como logo (0x8056) al inicio; aquí las
     * sobrescribimos con los patrones correctos desde ROM.
     * 0x73-0x74 @ 0x89C6, 0x75-0x76 @ 0x8966 */
    tiles_rom_to_vram(0x89C6u, 0x0073u, 2u);
    tiles_rom_to_vram(0x8966u, 0x0075u, 2u);
    /* También a tercios 1+2 (las necesita sub_4E8E+sub_549D) */
    tiles_rom_to_vram(0x89C6u, 0x0173u, 2u);
    tiles_rom_to_vram(0x8966u, 0x0175u, 2u);
    tiles_rom_to_vram(0x89C6u, 0x0273u, 2u);
    tiles_rom_to_vram(0x8966u, 0x0275u, 2u);

    /* Pasos 5+7: tiles de enemigos (sub_549D + sub_6CD9 + sub_6D5A).
     * 14 tiles interleaved @ ROM 0x5916; se espejan para llenar
     * 28 posiciones por tercio. El mapeo normal/espejado coincide
     * con el dump VRAM de OpenMSX para sala 0x70. */
    {
        /* Mapeo: {src_idx (0-13), mirror, vram_offset} */
        static const uint8_t enemy_map[28][2] = {
            { 0,0},{ 1,0},{ 2,0},{ 3,0},   /* 0x87-0x8A: src 0-3 normal */
            { 1,1},{ 0,1},{ 3,1},{ 2,1},   /* 0x8B-0x8E: src 1,0,3,2 mirror */
            { 4,0},{ 5,0},{ 6,0},{ 7,0},   /* 0x8F-0x92: src 4-7 normal */
            { 8,0},{ 9,0},{10,0},{11,0},   /* 0x93-0x96: src 8-11 normal */
            {12,0},{13,0},                  /* 0x97-0x98: src 12-13 normal */
            { 5,1},{ 4,1},{ 7,1},{ 6,1},   /* 0x99-0x9C: mirror src 5,4,7,6 */
            {10,1},{ 9,1},{ 8,1},           /* 0x9D-0x9F: mirror src 10,9,8 */
            {13,1},{12,1},{11,1},           /* 0xA0-0xA2: mirror src 13,12,11 */
        };
        /* Tercio 0: base 0x87, tercio 1: base 0x40, tercio 2: base 0x1F */
        static const uint16_t tbase[3] = {0x0087u, 0x0140u, 0x021Fu};
        for (uint8_t t = 0u; t < 3u; t++) {
            for (uint8_t i = 0u; i < 28u; i++) {
                uint8_t src_idx  = enemy_map[i][0];
                bool    mirror   = enemy_map[i][1] != 0u;
                uint16_t vram    = (uint16_t)(tbase[t] + i);
                uint32_t rom_off = 0x5916u + (uint32_t)src_idx * 16u;
                tiles_load_interleaved_tile(rom_off, vram, mirror);
            }
        }
    }

    /* Puertas en tercio 0 (legacy: tiles_vram_idx_door()=0x0D).
     * Z80 las carga a tercios 1+2 (0x019F/0x01AF/0x029F), pero
     * el port aún no traduce nombre-tabla por tercio. */
    load_tileset(ROM_TS_DOOR_EXTRA, 0x9Fu, 16u);
    load_tileset(ROM_TS_DOOR, 0x0Du, 1u);

    /* Contadores de animación */
    g_anim_ctr[0] = 0x72u;
    g_anim_ctr[1] = 0xAFu;
    g_anim_ctr[2] = 0x00u;

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
}
