/*
 * THE CASTLE — Cargador de tiles: ROM → VRAM
 * ===========================================
 *
 * Traduce las rutinas sub_4D52 / sub_64AB / sub_6CD9 que cargan todos
 * los tiles gráficos del juego desde la ROM del MSX a la VRAM del TMS9918A.
 *
 * FORMATO DE DATOS EN ROM
 * -----------------------
 * Todo el gráfico del juego está organizado en tres niveles:
 *
 *   Nivel 1 — Tabla de descriptores de tileset (0x7BC0..0x7BDF):
 *     Array de entradas de 2 bytes. Cada entrada = dirección (little-endian)
 *     del bloque de datos del tileset correspondiente.
 *
 *       0x7BC0 → BG0    (un solo tile de fondo)
 *       0x7BC2 → BG1_MAIN (tileset principal de habitaciones)
 *       0x7BC4 → BG2
 *       0x7BC6 → BG3    (28 tiles de pared/suelo)
 *       0x7BC8 → BG4    (tiles de plataformas)
 *       0x7BCA → BG5
 *       0x7BCE → ANIM_BG (10 tiles animados)
 *       0x7BD0 → WALLS  (26 tiles de pared sólida)
 *       0x7BD2 → PLAYER (sprites del jugador)
 *       0x7BD4 → DOOR   (sprite de puerta)
 *       0x7BD6 → KEY    (sprite de llave)
 *       0x7BDA → ENEMY  (sprites de enemigos)
 *
 *   Nivel 2 — Bloque de datos del tileset (e.g. 0x8056 para BG1):
 *     Array de entradas de 2 bytes. Cada entrada = dirección del tile
 *     individual en la ROM. N entradas = N tiles en el tileset.
 *     (El número de tiles se pasa como B en sub_64AB.)
 *
 *   Nivel 3 — Datos del tile individual (e.g. 0xA100):
 *     16 bytes = 8 pares (patrón, color), uno por fila del tile de 8×8:
 *       byte[0]  = patrón fila 0  (bits = pixeles, 1=ink 0=paper)
 *       byte[1]  = color  fila 0  (bits[7:4]=ink, bits[3:0]=paper, paleta TMS9918)
 *       byte[2]  = patrón fila 1
 *       byte[3]  = color  fila 1
 *       ...
 *       byte[14] = patrón fila 7
 *       byte[15] = color  fila 7
 *
 * MAPA DE ÍNDICES VRAM (tile_index → tileset)
 * --------------------------------------------
 * sub_4D52 carga los tilesets en este orden y con este mapeo:
 *
 *   VRAM tile 0x00..0x0C  → blancos (tile 0x3F = espacio)
 *   VRAM tile 0x0D        → DOOR[0]       (1 tile)
 *   VRAM tile 0x0E..0x29  → BG3[0..27]    (28 tiles)
 *   VRAM tile 0x2A        → BG0[0]        (1 tile)
 *   VRAM tile 0x2B        → KEY[0]        (1 tile)
 *   VRAM tile 0x2C..0x35  → ANIM_BG[0..9] (10 tiles)
 *   VRAM tile 0x36..0x39  → BG4[0..3]     (4 tiles)
 *   VRAM tile 0x3A..0x3B  → BG4[4..5]     (2 tiles)
 *   VRAM tile 0x3C..0x3D  → BG4[6..7]     (2 tiles)
 *   VRAM tile 0x3E..0x57  → WALLS[0..25]  (26 tiles)
 *   VRAM tile 0x3F        → ESPACIO (tile transparente)
 *
 * VRAM layout (Screen 2 / Graphics II del TMS9918A):
 *   Pattern table : 0x0000 + tile_index * 8         (8 bytes de patrón)
 *   Color table   : 0x2000 + tile_index * 8         (8 bytes de color)
 *   Name table    : 0x1800 + row * 32 + col         (1 byte = tile_index)
 *   En Screen 2 cada tercio (0x800 bytes) es independiente, por lo que
 *   el mismo tile_index en los tres tercios puede tener patrones distintos.
 *   El juego solo usa el tercio 0 (filas 0-7) para los tiles del HUD y
 *   reutiliza los mismos datos en los tres tercios vía LDIRVM.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"

/* ==========================================================================
 * CONSTANTES DE LAYOUT
 * ========================================================================== */

/* Direcciones en la ROM (relativas al ORG 0x4000) */
#define ROM_ORG             0x4000u

/* Tabla de descriptores de tileset (punteros a bloques) */
#define ROM_TS_BG0          0x7BC0u
#define ROM_TS_BG1_MAIN     0x7BC2u
#define ROM_TS_BG2          0x7BC4u
#define ROM_TS_BG3          0x7BC6u
#define ROM_TS_BG4          0x7BC8u
#define ROM_TS_BG5          0x7BCAu
#define ROM_TS_ANIM_BG      0x7BCEu
#define ROM_TS_WALLS        0x7BD0u
#define ROM_TS_PLAYER       0x7BD2u
#define ROM_TS_DOOR         0x7BD4u
#define ROM_TS_KEY          0x7BD6u
#define ROM_TS_ENEMY        0x7BDAu

/* Tamaños de tileset (número de tiles = argumento B en sub_64AB) */
#define TS_COUNT_BG3        28u
#define TS_COUNT_ANIM_BG    10u
#define TS_COUNT_BG4_A       4u
#define TS_COUNT_BG4_B       2u
#define TS_COUNT_BG4_C       2u
#define TS_COUNT_WALLS_A    26u   /* sub_4E07: B=0x1A */
#define TS_COUNT_WALLS_B    28u   /* sub_4E91: B=0x1C */

/* Índices VRAM de inicio para cada grupo de tiles */
#define VRAM_TILE_BLANK     0x00u  /* 0x00..0x0C = blancos */
#define VRAM_TILE_DOOR      0x0Du
#define VRAM_TILE_BG3       0x0Eu
#define VRAM_TILE_BG0       0x2Au
#define VRAM_TILE_KEY       0x2Bu
#define VRAM_TILE_ANIM_BG   0x2Cu
#define VRAM_TILE_BG4       0x36u
#define VRAM_TILE_WALLS     0x3Eu
#define VRAM_TILE_SPACE     0x3Fu  /* tile "espacio" / transparente */

/* VRAM: patrón y color */
#define VRAM_PATTERN_BASE   0x0000u
#define VRAM_COLOR_BASE     0x2000u
#define VRAM_NAME_BASE      0x1800u

/* Bytes por tile en VRAM: 8 filas de patrón + 8 filas de color */
#define TILE_ROWS           8u
#define TILE_BYTES          (TILE_ROWS * 2u)   /* 16 bytes (8 patrón + 8 color) */
#define TILE_PAT_BYTES      TILE_ROWS           /* 8 bytes de patrón en VRAM */
#define TILE_COL_BYTES      TILE_ROWS           /* 8 bytes de color en VRAM */

/* En Screen 2 hay 3 "tercios" de 256 tiles cada uno (uno por 8 filas de pantalla).
 * Cada tercio tiene su propia copia del pattern table y color table.
 * El juego carga los mismos datos en los 3 tercios. */
#define VRAM_THIRD_SIZE     0x0800u   /* 2KB por tercio en pattern/color */

/* ==========================================================================
 * PUNTERO A LA ROM
 *
 * tiles_load_from_rom() recibe un puntero al buffer completo de la ROM
 * cargada en RAM del host. Todas las lecturas van a través de este puntero.
 * ========================================================================== */

static const uint8_t *g_rom = NULL;
static uint32_t       g_rom_size = 0;

/* Leer 1 byte de la ROM en la dirección MSX dada */
static inline uint8_t rom_rb(uint16_t addr)
{
    uint32_t off = (uint32_t)addr - ROM_ORG;
    if (off >= g_rom_size) return 0xFFu;
    return g_rom[off];
}

/* Leer 2 bytes (little-endian) de la ROM */
static inline uint16_t rom_rw(uint16_t addr)
{
    return (uint16_t)(rom_rb(addr) | ((uint16_t)rom_rb((uint16_t)(addr + 1u)) << 8));
}

/* ==========================================================================
 * sub_6CD9 — Copiar UN tile (16 bytes) a VRAM
 *
 * Original Z80:
 *   B = 8 (filas)
 * sub_6CDC:
 *   A = (DE)  → sub_6CED → SETWRT(pat_base + HL) / WRTVRM(A)  (patrón)
 *   INC DE
 *   A = (DE)  → sub_6CF5 → SETWRT(col_base + HL) / WRTVRM(A)  (color)
 *   INC HL; INC DE
 *   DJNZ sub_6CDC
 *
 * En C: lee los 16 bytes desde (rom + tile_src_offset) y los escribe en
 * las tablas de patrón y color de la VRAM para el tile_index dado.
 *
 * Screen 2: el mismo tile_index existe en los 3 tercios.
 * El juego escribe en los 3 tercios siempre (sub_64DD llama sub_6CD9 ×3
 * con HL=0x0000, 0x0800, 0x1000 para los tercios 0, 1, 2).
 * ========================================================================== */
static void vram_write_tile(uint8_t tile_index,
                            uint16_t tile_src_addr,
                            bool all_thirds)
{
    int thirds = all_thirds ? 3 : 1;

    for (int t = 0; t < thirds; t++) {
        uint16_t pat_base = (uint16_t)(VRAM_PATTERN_BASE
                            + (uint16_t)t * VRAM_THIRD_SIZE
                            + (uint16_t)tile_index * TILE_PAT_BYTES);
        uint16_t col_base = (uint16_t)(VRAM_COLOR_BASE
                            + (uint16_t)t * VRAM_THIRD_SIZE
                            + (uint16_t)tile_index * TILE_COL_BYTES);

        for (uint8_t row = 0; row < TILE_ROWS; row++) {
            /* Los datos están intercalados: (patrón, color, patrón, color...) */
            uint8_t pat_byte = rom_rb((uint16_t)(tile_src_addr + row * 2u));
            uint8_t col_byte = rom_rb((uint16_t)(tile_src_addr + row * 2u + 1u));

            hal_vdp_write_vram((uint16_t)(pat_base + row), pat_byte);
            hal_vdp_write_vram((uint16_t)(col_base + row), col_byte);
        }
    }
}

/* ==========================================================================
 * sub_64AB — Cargar N tiles de un tileset a VRAM
 *
 * Original Z80:
 *   HL  = dirección del descriptor del tileset en ROM (e.g. 0x7BC6)
 *   B   = número de tiles a cargar
 *   DE  = índice VRAM del primer tile destino (pasado como VRAM tile offset)
 *
 *   Lee (HL) → 2 bytes = dirección del bloque de datos del tileset
 *   INC HL; INC HL   (avanza al siguiente descriptor)
 *
 *   Loop B veces:
 *     Multiplica el índice VRAM × 8 (ADD HL,HL×3) = offset en pat/col table
 *     CALL sub_6CD9(DE_data, HL_vram)  → escribe el tile
 *     INC tile_index
 *
 * En C: lee el bloque de datos del tileset, itera N entradas de 2 bytes
 * (cada una = dirección ROM del tile), y copia cada tile a VRAM.
 *
 * Retorna: la dirección ROM actualizada de HL (para encadenar llamadas).
 * ========================================================================== */
static void load_tileset(uint16_t ts_desc_addr,
                         uint8_t  vram_start_idx,
                         uint8_t  count,
                         bool     all_thirds)
{
    /* Nivel 1: leer dirección del bloque del tileset */
    uint16_t block_addr = rom_rw(ts_desc_addr);

    /* Nivel 2: iterar las N entradas del bloque */
    for (uint8_t i = 0; i < count; i++) {
        /* Cada entrada = 2-byte ptr al tile individual */
        uint16_t tile_src = rom_rw((uint16_t)(block_addr + (uint16_t)i * 2u));
        uint8_t  tile_idx = (uint8_t)(vram_start_idx + i);

        /* Nivel 3: copiar los 16 bytes del tile a VRAM */
        vram_write_tile(tile_idx, tile_src, all_thirds);
    }
}

/* ==========================================================================
 * Llenar un rango de tiles VRAM con un valor constante
 * (equivale a FILVRM del BIOS, usado para blancos y el tile "espacio")
 * ========================================================================== */
static void vram_fill_tile(uint8_t tile_index, uint8_t pat_val, uint8_t col_val,
                           bool all_thirds)
{
    int thirds = all_thirds ? 3 : 1;
    for (int t = 0; t < thirds; t++) {
        uint16_t pat_base = (uint16_t)(VRAM_PATTERN_BASE
                            + (uint16_t)t * VRAM_THIRD_SIZE
                            + (uint16_t)tile_index * TILE_PAT_BYTES);
        uint16_t col_base = (uint16_t)(VRAM_COLOR_BASE
                            + (uint16_t)t * VRAM_THIRD_SIZE
                            + (uint16_t)tile_index * TILE_COL_BYTES);
        hal_vdp_fill_vram(pat_base, pat_val, TILE_PAT_BYTES);
        hal_vdp_fill_vram(col_base, col_val, TILE_COL_BYTES);
    }
}

/* ==========================================================================
 * sub_64C3 — Escribir un tile_index en un rectángulo de la name table
 *
 * Original Z80:
 *   HL  = (col, row) en la name table (H=col, L=row)
 *   BC  = (height=B, width=C) del rectángulo
 *   A   = tile_index a escribir
 *
 *   Loop C filas de alto:
 *     Loop B tiles de ancho:
 *       CALL sub_6AEF  → calcular addr VRAM de name table en (H,L)
 *       WRTVRM(A)      → escribir tile_index
 *       INC H          → siguiente columna
 *       A = A+1 o A (si A==0 → no incrementar) ← sub_64CE
 *     INC L            → siguiente fila
 *     DEC C
 *
 * Se usa para llenar rectángulos de la name table con tiles consecutivos
 * o con el mismo tile. Si A=0, escribe siempre el mismo tile.
 * Si A≠0, incrementa A en cada celda (tilestrip horizontal).
 * ========================================================================== */
static void name_table_fill_rect(uint8_t col, uint8_t row,
                                  uint8_t width, uint8_t height,
                                  uint8_t tile_start)
{
    for (uint8_t r = 0; r < height; r++) {
        uint8_t tile = tile_start;
        for (uint8_t c = 0; c < width; c++) {
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE
                            + (uint16_t)(row + r) * 32u
                            + (col + c));
            hal_vdp_write_vram(addr, tile);
            /* sub_64CE: si tile != 0, incrementar (tilestrip); si 0, mantener */
            if (tile != 0u) tile++;
        }
    }
}

/* ==========================================================================
 * sub_6EE1 — Escribir tile A en columna B, fila dada por HL
 *
 * Versión simplificada del "write one tile to name table" usado en el init.
 * Original: A=tile, B=col, HL=(col,row) calculado por sub_6AEF.
 *
 * Aquí lo usamos para los tiles de relleno iniciales (tile 0x3F en cols
 * 0x00..0x02 y 0x0F..0x0D que sub_4DAF/4DBD escribe durante el init).
 * ========================================================================== */
static void name_table_write(uint8_t col, uint8_t row, uint8_t tile)
{
    uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)row * 32u + col);
    hal_vdp_write_vram(addr, tile);
}

/* ==========================================================================
 * sub_4E8E / sub_4E91 — Recargar WALLS + ANIM_BG (llamado en transiciones)
 *
 * Original:
 *   sub_4E8E: LD DE,0x0101
 *   sub_4E91: LD HL,0x7BD0 / B=0x1C / CALL sub_64AB
 *             LD HL,0x7BCE / B=0x0A / CALL sub_64AB
 *
 * Se llama al cambiar de habitación para recargar los tiles de pared
 * y los tiles animados con los valores del nuevo nivel.
 * ========================================================================== */
void tiles_reload_walls_and_anim(void)
{
    /* WALLS: 28 tiles a partir de VRAM 0x3E */
    load_tileset(ROM_TS_WALLS,   VRAM_TILE_WALLS,   TS_COUNT_WALLS_B, true);
    /* ANIM_BG: 10 tiles a partir de VRAM 0x2C */
    load_tileset(ROM_TS_ANIM_BG, VRAM_TILE_ANIM_BG, TS_COUNT_ANIM_BG, true);
}

/* ==========================================================================
 * tiles_load_from_rom() — Carga inicial completa de todos los tiles
 *
 * Equivale a la secuencia de sub_4D52 → sub_4DAF..sub_4E0F → sub_4E8E.
 *
 * Debe llamarse una vez después de hal_vdp_init_screen2() y antes de
 * mostrar cualquier contenido en pantalla.
 *
 * @param rom_data  Puntero al buffer de la ROM completa (32768 bytes)
 * @param rom_size  Tamaño del buffer (debe ser >= 32768)
 * ========================================================================== */
void tiles_load_from_rom(const uint8_t *rom_data, uint32_t rom_size)
{
    g_rom      = rom_data;
    g_rom_size = rom_size;

    /* -----------------------------------------------------------------------
     * Paso 1: Limpiar toda la VRAM de patrón y color
     * (sub_4DAF/4DBD: escribe tile 0x3F en primeras celdas de name table)
     * --------------------------------------------------------------------- */
    /* Limpiar pattern table y color table completas (3 tercios × 2KB) */
    hal_vdp_fill_vram(VRAM_PATTERN_BASE, 0x00u, 0x1800u);
    hal_vdp_fill_vram(VRAM_COLOR_BASE,   0x00u, 0x1800u);
    /* Limpiar name table */
    hal_vdp_fill_vram(VRAM_NAME_BASE, 0x3Fu, 768u);   /* 32×24 = 768 bytes */

    /* -----------------------------------------------------------------------
     * Paso 2: Tiles "en blanco" (0x00..0x0C) → patrón 0x00, color 0x00
     * Tile 0x3F (espacio) → patrón 0x00, color 0x00
     * --------------------------------------------------------------------- */
    for (uint8_t i = 0x00u; i <= 0x0Cu; i++) {
        vram_fill_tile(i, 0x00u, 0x00u, true);
    }
    vram_fill_tile(0x3Fu, 0x00u, 0x00u, true);

    /* -----------------------------------------------------------------------
     * Paso 3: Cargar tilesets en VRAM según el mapa de índices
     *
     * Secuencia exacta de sub_4D52 (sub_4DD6..sub_4E0F):
     *   4DD6: HL=7BD4, DE=000D, B=01  → DOOR en VRAM[0x0D]
     *   4DDC: HL=7BC6, B=1C           → BG3  en VRAM[0x0E..0x29]
     *   4DE1: HL=7BC0                 → BG0  en VRAM[0x2A]
     *   4DE7: HL=7BD6, B=01           → KEY  en VRAM[0x2B]
     *   4DEF: HL=7BCE, B=0A           → ANIM_BG en VRAM[0x2C..0x35]
     *   4DF7: HL=7BC8, B=04           → BG4  en VRAM[0x36..0x39]
     *   4DFF: B=02 (mismo HL=7BC8)    → BG4  en VRAM[0x3A..0x3B]
     *   4E01: B=02 (mismo HL=7BC8)    → BG4  en VRAM[0x3C..0x3D]
     *   4E07: HL=7BD0, B=1A           → WALLS en VRAM[0x3E..0x57]
     *   4E0F: sub_4E8E → WALLS B=1C + ANIM_BG B=0A
     * --------------------------------------------------------------------- */

    load_tileset(ROM_TS_DOOR,    VRAM_TILE_DOOR,    1u,                true);
    load_tileset(ROM_TS_BG3,     VRAM_TILE_BG3,     TS_COUNT_BG3,      true);
    load_tileset(ROM_TS_BG0,     VRAM_TILE_BG0,     1u,                true);
    load_tileset(ROM_TS_KEY,     VRAM_TILE_KEY,     1u,                true);
    load_tileset(ROM_TS_ANIM_BG, VRAM_TILE_ANIM_BG, TS_COUNT_ANIM_BG,  true);
    load_tileset(ROM_TS_BG4,     VRAM_TILE_BG4,     TS_COUNT_BG4_A,    true);
    load_tileset(ROM_TS_BG4,     (uint8_t)(VRAM_TILE_BG4 + TS_COUNT_BG4_A),
                                              TS_COUNT_BG4_B,           true);
    load_tileset(ROM_TS_BG4,     (uint8_t)(VRAM_TILE_BG4 + TS_COUNT_BG4_A + TS_COUNT_BG4_B),
                                              TS_COUNT_BG4_C,           true);
    load_tileset(ROM_TS_WALLS,   VRAM_TILE_WALLS,   TS_COUNT_WALLS_A,  true);

    /* sub_4E8E: recarga walls (28) + anim_bg (10) */
    tiles_reload_walls_and_anim();

    /* -----------------------------------------------------------------------
     * Paso 4: sub_64C3 — Escribir tiles especiales en name table
     * (marcos del HUD, bordes de pantalla, área de puntuación)
     *
     * De sub_4E12..sub_4E4B:
     *   HL=0x1100, BC=0x0704, A=0x0E → rect 7×4 desde (col=0,row=17) con tiles 0x0E+
     *   HL=0x0100, BC=0x0301, A=0x52 → rect 3×1 desde (col=0,row=1)  con tiles 0x52+
     *   HL=0x0900, BC=0x0401, A=0x51 → rect 4×1 desde (col=9,row=0)  con tile  0x51+
     *   HL=0x0102, BC=0x0102, A=0x55 → rect 1×2 desde (col=0,row=2)  con tiles 0x55+
     *   HL=0x0103, BC=0x0102, A=0x57 → rect 1×2 desde (col=0,row=3)  con tiles 0x57+
     *
     * HL encoding: H=col, L=row  (en el MSX, sub_64C3 usa H=col, L=row)
     * BC encoding: B=width (cols), C=height (rows)
     * --------------------------------------------------------------------- */

    /* Marco del área de juego (fila 17, columna 0, 7 tiles de ancho × 4 alto) */
    name_table_fill_rect(0x00u, 0x11u, 7u, 4u, 0x0Eu);

    /* Borde superior izquierdo */
    name_table_fill_rect(0x00u, 0x01u, 3u, 1u, 0x52u);
    name_table_fill_rect(0x09u, 0x00u, 4u, 1u, 0x51u);
    name_table_fill_rect(0x00u, 0x02u, 1u, 2u, 0x55u);
    name_table_fill_rect(0x00u, 0x03u, 1u, 2u, 0x57u);

    /* -----------------------------------------------------------------------
     * Paso 5: sub_4ECA (0x3E → nombre del juego / título en name table)
     * y sub_4E60/4E76 (posición inicial del jugador en la name table)
     * Estos se omiten aquí y se manejan desde game_loop / title_screen.
     * --------------------------------------------------------------------- */
}

/* ==========================================================================
 * tiles_animate() — Actualizar tiles animados (sub_6265 / sub_6CD9 area)
 *
 * El juego tiene 10 tiles animados (ANIM_BG, VRAM 0x2C..0x35).
 * Cada frame se actualiza uno de ellos rotando los 16 bytes entre
 * los 10 slots (efecto de "shimmer" en los fondos decorativos).
 *
 * Original: sub_6265 decrementa g_spark_timer_a/b y cuando llegan a 0
 * llama sub_7769 que recarga el puntero de música. La animación de tiles
 * corre en paralelo.
 *
 * Aquí simplificamos: cada 4 frames rotamos el tile animado activo.
 *
 * @param frame_counter  Contador de frames del juego (g_state_flags)
 * ========================================================================== */
void tiles_animate(uint8_t frame_counter)
{
    /* Actualizar solo cada 4 frames (bits[1:0] == 0) */
    if ((frame_counter & 0x03u) != 0u) return;

    /* Seleccionar qué tile animado actualizar (ciclo de 10) */
    uint8_t anim_slot = (uint8_t)((frame_counter >> 2) % TS_COUNT_ANIM_BG);
    uint8_t tile_idx  = (uint8_t)(VRAM_TILE_ANIM_BG + anim_slot);

    /* Recargar el tile desde la ROM con el siguiente frame de animación.
     * El bloque ANIM_BG tiene 10 entradas; el siguiente frame se obtiene
     * leyendo la entrada (anim_slot + 1) % 10 del bloque. */
    uint16_t block_addr  = rom_rw(ROM_TS_ANIM_BG);
    uint8_t  next_slot   = (uint8_t)((anim_slot + 1u) % TS_COUNT_ANIM_BG);
    uint16_t tile_src    = rom_rw((uint16_t)(block_addr + (uint16_t)next_slot * 2u));

    vram_write_tile(tile_idx, tile_src, true);
}

/* ==========================================================================
 * tiles_get_vram_index() — Traducir nombre de tile a índice VRAM
 *
 * Utilidad para el resto del código: devuelve el índice VRAM que
 * corresponde a un tile con nombre simbólico.
 * ========================================================================== */
uint8_t tiles_vram_idx_blank(void)        { return 0x00u; }
uint8_t tiles_vram_idx_door(void)         { return VRAM_TILE_DOOR; }
uint8_t tiles_vram_idx_bg3(uint8_t n)     { return (uint8_t)(VRAM_TILE_BG3    + n); }
uint8_t tiles_vram_idx_key(void)          { return VRAM_TILE_KEY; }
uint8_t tiles_vram_idx_anim_bg(uint8_t n) { return (uint8_t)(VRAM_TILE_ANIM_BG + n); }
uint8_t tiles_vram_idx_bg4(uint8_t n)     { return (uint8_t)(VRAM_TILE_BG4    + n); }
uint8_t tiles_vram_idx_wall(uint8_t n)    { return (uint8_t)(VRAM_TILE_WALLS  + n); }
uint8_t tiles_vram_idx_space(void)        { return VRAM_TILE_SPACE; }
