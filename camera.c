/*
 * THE CASTLE — Sistema de cámara y scroll (sub_623C + sub_5B96)
 * =============================================================
 *
 * ARQUITECTURA
 * ------------
 * El juego tiene dos sistemas de "scroll" distintos:
 *
 *   1. SCROLL DE SALA (sub_5B96) — llamado una vez por frame desde el
 *      game_loop. Detecta si el jugador está sobre una celda especial
 *      del mapa (bits 4-5 del tile) y ejecuta la transición correspondiente:
 *        tile & 0x04 != 0 → el jugador está sobre un trigger de sala
 *        IX+1 == 0x20 → trigger de subida (sub_51D9: cargar sala arriba)
 *        IX+1 == 0x21 → trigger de sala lateral (sub_518E: buscar en tabla)
 *        IX+1 == 0x22 → trigger especial (SET 3 de E321, dibujar HUD)
 *        IX+1 == 0x23 → trigger de llave (cargar música de llave)
 *        IX+1 == 0x24 → trigger de segunda llave
 *        IX+1 == 0x25 → trigger de mapa (setear g_map_flag)
 *        IX+1 == 0x26 → trigger de sub_5E4B (actualizar habitación)
 *        IX+1 >= 0x27 → trigger de coleccionable (C = IX+1 - 0x27)
 *
 *   2. ACTUALIZACIÓN DE CÁMARA (sub_623C) — llamado al final del game_loop.
 *      Gestiona tres cosas:
 *        a. Timer de fade del sprite de muerte (0xEAF9)
 *        b. Timers de chispa de animación (0xE343, 0xE344) — sub_6265
 *        c. Render del mapa visible (sub_63BB) — actualiza la name table
 *           con los 10×10 tiles de la habitación actual
 *
 * RENDER DEL MAPA (sub_63BB)
 * --------------------------
 * Itera 10 columnas × 10 filas de tiles de la habitación.
 * Para cada celda (col, row):
 *   1. Calcula offset en g_map: row*10 + col (sub_5D24 × 10 + col)
 *   2. Lee el tile de g_map (sub_60EB)
 *   3. Si tile != 0 → color = 0x07 (gris), else color = 0x04 (cyan)
 *   4. Llama sub_640F → escribe el tile y su color en la VRAM
 *      (2 bytes en la color table: hi nibble = ink, lo nibble = paper)
 *
 * sub_640F calcula la dirección VRAM del tile:
 *   row_index = (L * 3 + 2) * 8    → offset en color table (3 bytes/entrada × 8 filas)
 *   VRAM addr = color_base + row_index + sub_pixel_col
 *   Luego escribe el color en 2 bytes: (alto nibble, bajo nibble) vía BIOS_RDSLT
 *
 * MAPA VISIBLE
 * ------------
 * El juego renderiza una ventana de 10×10 tiles de la sala actual.
 * La sala completa en g_tilemap[] tiene stride=30, pero solo se muestran
 * 10 columnas × 10 filas (el "viewport" del castillo).
 * La posición de la ventana la determina el scroll (g_scroll_x, g_scroll_y).
 *
 * RAM RELEVANTE
 * -------------
 *   0xEAD3  g_trigger_flags — flags de colisión con triggers de sala
 *   0xEAD4  g_trigger_flags2 — flags de segundo trigger
 *   0xEACA  g_player_speed  — velocidad del jugador (también usada como tempo)
 *   0xEAF9  g_death_timer   — timer de fade de muerte (decrementado aquí)
 *   0xE343  g_spark_timer_a — timer de chispa A (decrementado aquí)
 *   0xE344  g_spark_timer_b — timer de chispa B
 *   0xEAE0  g_game_over     — flag de game over
 *   0xE324  g_lives         — vidas restantes (decrementado por timer)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"
#include "game.h"
#include "screen.h"
#include "tiledata.h"

/* Forward declarations of globals defined later in this file */
uint8_t g_music_transpose_fine   = 0;
uint8_t g_music_transpose_coarse = 0;
uint8_t g_music_tempo_counter    = 0;

/* Room exit direction codes (from room.c) */
#define EXIT_UP    0x01u
#define EXIT_RIGHT 0x03u
#define EXIT_DOWN  0x05u
#define EXIT_LEFT  0x07u


/* ==========================================================================
 * CONSTANTES
 * ========================================================================== */

#define ROM_ORG         0x4000u
#define VRAM_NAME_BASE  0x1800u
#define VRAM_PAT_BASE   0x0000u
#define VRAM_COL_BASE   0x2000u
#define VRAM_THIRD_SIZE 0x0800u

/* Ancho y alto del viewport de tiles visible */
#define VIEWPORT_COLS   10u
#define VIEWPORT_ROWS   10u

/* Stride del tilemap en g_tilemap[] (30 columnas, sub_49B6: row*30+col) */
#define TILEMAP_STRIDE  30u

/* Tile IDs de trigger en la tabla de puertas de salida (IX+1 values) */
#define TRIGGER_UP      0x20u
#define TRIGGER_SIDE    0x21u
#define TRIGGER_SPECIAL 0x22u
#define TRIGGER_KEY_A   0x23u
#define TRIGGER_KEY_B   0x24u
#define TRIGGER_MAP     0x25u
#define TRIGGER_ROOM    0x26u
/* 0x27+ = coleccionables (C = type - 0x27) */

/* ==========================================================================
 * VARIABLES INTERNAS
 * ========================================================================== */

/* Trigger flags (0xEAD3, 0xEAD4) — seteadas por sub_5D1E / sub_5B96 */
static uint8_t g_trigger_flags  = 0;   /* 0xEAD3 */
static uint8_t g_trigger_flags2 = 0;   /* 0xEAD4 */

/* Flag de limpieza de mapa pendiente (0xEAE2) — sub_5C52 */
static uint8_t g_map_clear_flag = 0;   /* 0xEAE2 */

/* ==========================================================================
 * EXTERN — variables de otros módulos
 * ========================================================================== */
extern uint8_t g_spark_timer_a;   /* 0xE343 — particles.c */
extern uint8_t g_spark_timer_b;   /* 0xE344 — particles.c */
extern uint8_t g_death_fade_timer;/* 0xEAF9 — particles.c */
extern uint8_t g_tilemap[];       /* 0xE496 — room.c      */

/* ==========================================================================
 * FORWARD DECLARATIONS
 * ========================================================================== */
extern void room_load_initial(void);    /* room.c */
extern void room_transition(void);      /* room.c */
extern void music_sfx_trigger(uint8_t sfx_id, uint8_t vol);  /* music.c */
extern void music_load(uint16_t a, uint16_t b);               /* music.c */

/* ==========================================================================
 * sub_5D1E — Comparar HL con DE (HL == DE?)
 *
 * Original Z80:
 *   LD A,H; CP D; RET NZ
 *   LD A,L; CP E; RET
 * Retorna: Z=1 si HL==DE, Z=0 si no.
 *
 * En C: simplemente comparar dos pares col/row.
 * ========================================================================== */
static bool coords_equal(uint8_t col_a, uint8_t row_a,
                          uint8_t col_b, uint8_t row_b)
{
    return (col_a == col_b) && (row_a == row_b);
}

/* ==========================================================================
 * sub_63FD — Escribir color de sala en la name table (color VDP del HUD)
 *
 * Entrada: A = room_x (BCD), C = color (0x0F = blanco, 0x09 = azul)
 *
 * Original:
 *   L = A >> 4   → decena BCD (fila)
 *   H = A & 0x0F → unidad BCD (columna)
 *   CALL sub_640F → escribir color en la VRAM color table
 *
 * sub_640F escribe 2 bytes de color (ink|paper) en la color table del VDP
 * para el tile indicado por (H=col, L=row) dentro del área del HUD.
 * ========================================================================== */
static void write_room_hud_color(uint8_t room_x, uint8_t color)
{
    uint8_t col = (uint8_t)(room_x & 0x0Fu);   /* unidad BCD = columna */
    uint8_t row = (uint8_t)(room_x >> 4);       /* decena BCD = fila */

    /* sub_640F: calcular dirección VRAM de la color table para (col, row) */
    /* row_offset = (L*3 + 2) * 8 donde L = row en la tabla */
    uint16_t row_off  = (uint16_t)((row * 3u + 2u) * 8u);
    uint16_t col_off  = (uint16_t)((col >> 1));   /* cada 2 columnas = 1 byte */
    uint16_t vram_col = (uint16_t)(VRAM_COL_BASE + row_off + col_off);

    uint8_t existing = hal_vdp_read_vram(vram_col);
    uint8_t new_val;

    if (col & 1u) {
        /* columna impar: bits bajos */
        new_val = (uint8_t)((existing & 0xF0u) | (color & 0x0Fu));
    } else {
        /* columna par: bits altos */
        new_val = (uint8_t)((existing & 0x0Fu) | ((color & 0x0Fu) << 4));
    }

    hal_vdp_write_vram(vram_col, new_val);
}

/* ==========================================================================
 * sub_640F — Escribir tile + color en la VRAM (render de mapa de sala)
 *
 * Entrada:
 *   H = col (0-9), L = row (0-9) dentro del viewport
 *   C = color (0x04=libre, 0x07=muro)
 *   A = tile ID a escribir
 *
 * Calcula la dirección en la name table y color table para la posición
 * (col, row) del viewport y escribe el tile + color.
 *
 * Screen 2 color table: cada byte controla 8 pixels (un row de un tile).
 * offset = tile_idx * 8 + pixel_row
 * ========================================================================== */
static void vdp_write_map_tile(uint8_t col, uint8_t row,
                                uint8_t tile_id, uint8_t color)
{
    /* Name table: dirección = 0x1800 + row*32 + col */
    /* El mapa del castillo empieza en la fila 4 (primeras 4 filas = HUD) */
    uint8_t  screen_col = (uint8_t)(col + 2u);  /* offset horizontal del mapa */
    uint8_t  screen_row = (uint8_t)(row + 4u);  /* offset vertical (debajo HUD) */
    uint16_t name_addr  = (uint16_t)(VRAM_NAME_BASE
                           + (uint16_t)screen_row * 32u + screen_col);
    hal_vdp_write_vram(name_addr, tile_id);

    /* Color table: 8 bytes por tile, un byte por fila de pixels */
    /* En Screen 2 la color table está en 0x2000, mismo tamaño que pattern */
    uint16_t col_base = (uint16_t)(VRAM_COL_BASE
                         + (uint16_t)tile_id * 8u);
    /* Escribir el mismo color en las 8 filas del tile */
    for (uint8_t r = 0; r < 8u; r++) {
        hal_vdp_write_vram((uint16_t)(col_base + r), color);
    }
}

/* ==========================================================================
 * sub_63BB — Render del mapa visible (10×10 tiles)
 *
 * Itera las 10 columnas × 10 filas del viewport y escribe cada tile
 * en la name table del VDP.
 *
 * Para cada (col, row):
 *   offset = row * TILEMAP_STRIDE + col  (acceso a g_tilemap)
 *   tile   = g_tilemap[offset]
 *   color  = (tile != 0) ? 0x07 : 0x04
 *   vdp_write_map_tile(col, row, tile, color)
 *
 * Después: sub_63E7 → actualizar color del HUD según g_room_x
 *          sub_63F2 → si bit 1 de g_room_y activo → color especial
 * ========================================================================== */
static void render_map_viewport(void)
{
    extern uint8_t g_tilemap[];  /* room.c: 30×30 tiles */

    for (uint8_t row = 0; row < VIEWPORT_ROWS; row++) {
        for (uint8_t col = 0; col < VIEWPORT_COLS; col++) {
            uint16_t off    = (uint16_t)((uint16_t)row * TILEMAP_STRIDE + col);
            uint8_t  tile   = (off < 30u * 30u) ? g_tilemap[off] : 0u;
            uint8_t  color  = (tile != 0u) ? 0x07u : 0x04u;
            vdp_write_map_tile(col, row, tile, color);
        }
    }

    /* Actualizar color de sala en el HUD (sub_63E7) */
    extern uint8_t g_room_x;   /* room.c */
    extern uint8_t g_room_y;
    write_room_hud_color(g_room_x, 0x0Fu);

    /* Si bit 1 de g_room_y → color alternativo (sub_63F2) */
    if (g_room_y & 0x02u) {
        write_room_hud_color(0x09u, 0x09u);
    }
}

/* ==========================================================================
 * sub_638E — Dibujar borde de sala (2 filas de tiles decorativos)
 *
 * Original:
 *   A=0x02 → 2 iteraciones
 *   HL=(0xFF, 0x90) → posición inicial en la name table
 *   B=0x00, C=0x2F
 *   Loop (sub_6397):
 *     sub_6EE1(col=C, row=row, A=C)  → columna 0x2F
 *     HL += 0x1000 (row+1, misma col)
 *     sub_6EE1(col=C+1, row=row+1) → columna 0x2F
 *     HL += 0x1000
 *     C++
 *     sub_6EE1(col=C+1) → col 0x31
 *     HL = HL + 0xFF10 (col-1, row-2)
 *     B++; C++
 *   INC H × 2 veces más (2 filas decorativas)
 *
 * Dibuja los tiles de borde de la sala en la name table.
 * ========================================================================== */
static void draw_room_border(void)
{
    /* Dos filas de tiles decorativos en los bordes del viewport.
     * sub_638E dibuja con C comenzando en 0x2F y avanzando en diagonal. */
    uint8_t c = 0x2Fu;
    for (int iter = 0; iter < 2; iter++) {
        /* Fila decorativa superior */
        {
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)0x10u * 32u + c);
            hal_vdp_write_vram(addr, c);
        }
        {
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)0x11u * 32u + c);
            hal_vdp_write_vram(addr, c);
        }
        c++;
        {
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)0x11u * 32u + c);
            hal_vdp_write_vram(addr, c);
        }
        c++;
    }
}

/* ==========================================================================
 * sub_6265 — Decrementar timer de chispa y disparar música
 *
 * Original:
 *   A = (HL); OR A; RET Z       → si timer==0, no hacer nada
 *   DEC A; (HL)=A               → decrementar
 *   CP 0x06; RET NC             → si >= 6, salir
 *   OR A; JR Z → sub_627B       → si llegó a 0 → cargar música de chispa
 *   (0xEAF2)=5; (0xEAF4)=0xFF  → configurar transpose y tempo para SFX
 *
 * Cuando el timer llega a 0: cargar música del título (sub_627B → sub_7769)
 * ========================================================================== */
static void spark_timer_tick(uint8_t *timer)
{
    if (*timer == 0u) return;
    (*timer)--;
    if (*timer >= 6u) return;

    if (*timer == 0u) {
        /* Timer agotado → recargar música del título */
        music_load(0x78D2u, 0x7916u);
        g_music_transpose_fine   = 0u;
        g_music_transpose_coarse = 0u;
    } else {
        /* Efecto de SFX: modificar transpose y tempo */
        g_music_transpose_fine   = 5u;
        g_music_transpose_coarse = 0xFFu;
    }
}

/* ==========================================================================
 * sub_623C — Actualización de cámara (llamada al final del game_loop)
 *
 * Secuencia exacta:
 *   1. HL=0xEAF9 (death_timer): si > 0 → DEC; si llega a 0 → borrar sprite
 *   2. Si (g_state_flags & 0x0F) != 0 → RET (solo ejecutar cada 16 frames)
 *   3. Decrementar g_spark_timer_a (sub_6265)
 *   4. Decrementar g_spark_timer_b (sub_6265)
 *   5. sub_63BB → render del mapa visible
 * ========================================================================== */
void camera_update(void)
{
    /* Paso 1: timer de fade de muerte (0xEAF9) */
    if (g_death_fade_timer > 0u) {
        g_death_fade_timer--;
        if (g_death_fade_timer == 0u) {
            /* Borrar sprite de muerte: tile 0x3F en col 0x0D */
            uint16_t addr = (uint16_t)(VRAM_NAME_BASE
                            + (uint16_t)g_player_row * 32u + 0x0Du);
            hal_vdp_write_vram(addr, 0x3Fu);
        }
    }

    /* Paso 2: solo ejecutar el resto cada 16 frames */
    if ((g_state_flags & 0x0Fu) != 0u) return;

    /* Paso 3+4: decrementar timers de chispa */
    spark_timer_tick(&g_spark_timer_a);
    spark_timer_tick(&g_spark_timer_b);

    /* Paso 5: render del mapa visible */
    render_map_viewport();
}

/* ==========================================================================
 * sub_5B96 — Scroll de sala (llamada desde el game_loop)
 *
 * Detecta si el jugador está sobre un trigger de sala y ejecuta la
 * transición correspondiente.
 *
 * Original:
 *   CALL sub_5D5D → si modo título → RET NZ (no procesar en título)
 *   B = g_player_col, C = g_player_row
 *   CALL sub_49C7 → leer tile en (B,C) de g_tilemap
 *   AND 0x04 → ¿bit 2 activo? (trigger de sala)
 *   RET Z → si no hay trigger, no hacer nada
 *   IX = 0xE3D6 (tabla de puertas de salida)
 *   D = B (player_col), E = C (player_row)
 *   B = 0x10 (16 slots)
 *   Loop: comparar (IX+2, IX+3) con (D, E):
 *     Si coincide → ejecutar acción de (IX+1)
 *     IX += 4 (siguiente slot)
 * ========================================================================== */

/* Tabla de puertas de salida (extern de doors.c — acceso via game.h) */
/* La iteramos aquí para detectar triggers */

/* Subtipo de acción según IX+1 */
static void execute_trigger(uint8_t trigger_type)
{
    switch (trigger_type) {
        case TRIGGER_UP:
            /* Cargar sala de arriba (sub_51D9) */
            room_load_initial();
            /* SET 2,(HL) donde HL=0xE321 → bit 2 de g_room_y */
            g_room_exit = EXIT_UP;
            g_restart_flag = 1u;
            break;

        case TRIGGER_SIDE:
            /* Buscar en tabla de salas (sub_518E) */
            /* room_table_search(): TODO */
            g_restart_flag = 1u;
            break;

        case TRIGGER_SPECIAL:
            /* SET 3 de E321 → flag especial de sala */
            /* Dibujar HUD (sub_64C3) y borde de sala (sub_638E) */
            draw_room_border();
            break;

        case TRIGGER_KEY_A:
            /* Timer de chispa A = 0x0A, cargar música de llave A */
            if (g_spark_timer_a != 0x0Au) {
                g_spark_timer_a = 0x0Au;
                g_spark_timer_b = 0u;
                music_load(0x79B7u, 0x79DEu);
            }
            break;

        case TRIGGER_KEY_B:
            /* Timer de chispa B = 0x10, cargar música de llave B */
            g_spark_timer_b = 0x10u;
            g_spark_timer_a = 0u;
            music_load(0x7964u, 0x7993u);
            break;

        case TRIGGER_MAP:
            /* Setear flag de mapa (0xEAE2) */
            g_map_clear_flag = 1u;
            break;

        case TRIGGER_ROOM:
            /* sub_5E4B: actualizar datos de habitación */
            /* TODO: sub_5E4B */
            break;

        default:
            if (trigger_type >= 0x27u) {
                /* Coleccionable: C = trigger_type - 0x27 */
                uint8_t coll_idx = (uint8_t)(trigger_type - 0x27u);
                /* sub_5C66: dibujar tile de coleccionable, actualizar score */
                /* Timer de muerte: 0x10 */
                g_death_fade_timer = 0x10u;
                /* SFX de coleccionable */
                music_sfx_trigger(1u, 0x10u);
                (void)coll_idx;
            }
            break;
    }
}

void scroll_update(void)
{
    /* En modo título no procesar triggers */
    if (g_state_flags & 0x01u) return;

    /* Leer tile en la posición del jugador */
    uint8_t tile = 0u;
    {
        extern uint8_t g_tilemap[];
        uint16_t off = (uint16_t)((uint16_t)g_player_row * TILEMAP_STRIDE
                       + g_player_col);
        if (off < 30u * 30u) tile = g_tilemap[off];
    }

    /* Solo procesar si bit 2 del tile está activo (trigger de sala) */
    if (!(tile & 0x04u)) return;

    /* Iterar la tabla de puertas de salida (0xE3D6, 16 slots × 4 bytes)
     * para encontrar la entrada que coincide con (player_col, player_row) */
    /* g_exit_doors from doors.c — accessed via scroll_check_exit_doors() */

    /* Delegate trigger detection to doors.c which owns the exit door table */
    uint8_t trigger_type = doors_find_trigger(g_player_col, g_player_row);
    if (trigger_type != 0u) {
        execute_trigger(trigger_type);
    }
}

/* ==========================================================================
 * sub_62D8 — Render del fondo con intro activa
 *
 * Llamado desde el game_loop cuando g_intro_active != 0.
 * Configura g_music_tempo y procesa la cola de keyframes (0xEACD).
 *
 * Original:
 *   A = (0xeae4); OR A; JR Z → sub_62FA (saltar si intro no activa)
 *   A = 0x06 → (0xeaf3) = tempo counter
 *   HL = 0xEACD; B = 0x09 (9 bytes de keyframe queue)
 *   Loop: si (HL) != 0xFF → sub_62F1 (hay keyframe pendiente → g_restart_flag=1)
 *         INC HL; DJNZ
 *   sub_62FA: gestión de g_trigger_flags → sub_630C → sub_6336
 *             Según bits de C (trigger_flags):
 *               bit 5: g_game_over = 1
 *               bit 6: g_lives-- y g_game_over = 1
 *               bit 1: bloque moviéndose lento (speed=0x01, transpose=0x0C)
 *               bit 2: bloque moviéndose rápido (speed=0x30, transpose=0x07)
 *               bit 3: sin sonido de bloque (E = 0x00)
 * ========================================================================== */
void render_background(void)
{
    if (g_intro_active) {
        /* Configurar tempo de música durante intro */
        music_set_tempo(0x06u, 0x01u);

        /* Comprobar keyframe queue (0xEACD): si alguno != 0xFF → restart */
        extern uint8_t g_keyframe_queue[];  /* the_castle.c */
        for (int i = 0; i < 9; i++) {
            if (g_keyframe_queue[i] != 0xFFu) {
                g_restart_flag = 1u;
                return;
            }
        }
        return;
    }

    /* sub_62FA: gestión de trigger flags */
    uint8_t c = g_trigger_flags;

    /* Modo juego (sub_5D5D retorna NZ = modo juego activo) */
    if (!(g_state_flags & 0x01u)) {
        /* bit 5: game over por caída */
        if (c & 0x20u) {
            g_game_over = 1u;
        }
        /* bit 6: game over por daño (perder una vida) */
        if (c & 0x40u) {
            if (g_lives > 0u) g_lives--;
            g_game_over = 1u;
        }
    }

    /* Configurar velocidad del jugador y música según tipo de bloque */
    uint8_t d = g_trigger_flags;
    uint8_t e = g_trigger_flags2;

    /* bit 1: bloque lento */
    if (!(c & 0x02u)) {
        g_player_speed = 0x01u;
        music_set_transpose(0x0Cu, 0u);
        e = 0x02u;
    }
    /* bit 2: bloque rápido */
    else if (c & 0x04u) {
        g_player_speed = 0x30u;
        music_set_transpose(0x07u, 0u);
        e = 0x04u;
    }
    /* bit 0 (parado) */
    else {
        g_player_speed = 0x70u;   /* velocidad normal */
        music_set_transpose(0x00u, 0u);
        e = 0x06u;
    }

    /* bit 3: sin sonido de bloque */
    if (!(c & 0x08u)) e = 0u;

    /* Aplicar configuración */
    g_music_tempo_counter = e;

    /* sub_634B: CALL sub_4F93 si bit 0 de g_trigger_flags2 == 0 */
    /* sub_4F93: gestión de movimiento de cámara — TODO */
    /* sub_6358: si bit 1 de g_trigger_flags2 == 0:
     *   esperar frame, resetear anim, limpiar keyframe queue,
     *   loop hasta que g_anim_frame o g_facing != 0 */
    (void)d;
}

/* ==========================================================================
 * hud_fill_rect — Escribe rectángulo de tiles con incremento
 *
 * Replica sub_64C3: Escribe tile start_tile + col en cada columna,
 * reiniciando por fila. start_tile=0 → escribe 0s (blanco).
 *
 *   col/row = posición inicial en name table
 *   width/height = dimensiones del rectángulo
 *   start_tile = tile base
 *
 * Mapeo de tiles (pattern table 0x00-0x72, tercio 0, no cambia):
 *   0x00 blank  0x01-0C llaves  0x0D corazón
 *   0x0E-29 mapa  0x2A-45 logo  0x46 línea vertical
 *   0x47-50 dígitos 0-9  0x51 "Hi"  0x52-54 "SCORE"
 *   0x55-56 "Key"  0x57-58 "Life"  0x59-72 letras A-Z
 * ========================================================================== */
static void hud_fill_rect(uint8_t col, uint8_t row,
                          uint8_t width, uint8_t height,
                          uint8_t start_tile)
{
    uint8_t tile = start_tile;
    for (uint8_t r = 0u; r < height; r++) {
        uint16_t base = (uint16_t)(VRAM_NAME_BASE
                      + (uint16_t)(row + r) * 32u + col);
        for (uint8_t c = 0u; c < width; c++) {
            hal_vdp_write_vram((uint16_t)(base + c),
                               (start_tile == 0u) ? 0u : tile);
            if (start_tile != 0u) tile++;
        }
    }
}

void draw_hud(void)
{
    uint16_t base1, base2;

    /* --- Static HUD (replicando Z80 sub_4E0C + sub_64C3 ×5) --- */

    /* 1) MAP area: 7Ã—4 rectángulo en col 17, row 0, tiles 0x0E-0x29 */
    hud_fill_rect(17u, 0u, 7u, 4u, 0x0Eu);

    /* 2) LOGO area: 7x4 rectángulo en col 24, row 0, tiles 0x2A-0x45 */
    hud_fill_rect(24u, 0u, 7u, 4u, 0x2Au);

    /* 3) Separador vertical: col 31, rows 0-3, tile 0x46 (sub_4ECA) */
    uint8_t row = 0, col = 31, tile = 0x46;
    for (uint8_t r = 0u; r < 4u; r++) {
        uint16_t addr = (uint16_t)(VRAM_NAME_BASE
                      + (uint16_t)(row + r) * 32u + col);
            hal_vdp_write_vram((uint16_t)(addr), tile);
    }

    /* 4) "SCORE" label: col 1, row 0, tiles 0x52-0x54 (3×1) */
    hud_fill_rect(1u, 0u, 3u, 1u, 0x52u);

    /* 5) "Hi SCORE" label: col 9, row 0, tiles 0x51-0x54 (4×1) */
    hud_fill_rect(9u, 0u, 4u, 1u, 0x51u);

    /* 6) "Key" label: col 1, row 2, tiles 0x55-0x56 (2×1) */
    hud_fill_rect(1u, 2u, 2u, 1u, 0x55u);

    /* 7) "Life" label: col 1, row 3, tiles 0x57-0x58 (2×1) */
    hud_fill_rect(1u, 3u, 2u, 1u, 0x57u);

    /* --- Blank rows 1-2 left area (no longer using static arrays) --- */
    /* Rows 1-2, cols 0-16 are already 0 from VRAM init. We only
     * overwrite the dynamic positions below. */

    /* --- Dynamic overlays --- */

    base1 = (uint16_t)(VRAM_NAME_BASE + 1u * 32u);
    base2 = (uint16_t)(VRAM_NAME_BASE + 2u * 32u);

    /* Score digits: 6 digits en (col 2, row 1) */
    {
        uint8_t s2 = g_score[2];
        hal_vdp_write_vram((uint16_t)(base1 + 2u),
                           (uint8_t)(0x47u + ((s2 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 3u),
                           (uint8_t)(0x47u + (s2 & 0x0Fu)));
        uint8_t s1 = g_score[1];
        hal_vdp_write_vram((uint16_t)(base1 + 4u),
                           (uint8_t)(0x47u + ((s1 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 5u),
                           (uint8_t)(0x47u + (s1 & 0x0Fu)));
        uint8_t s0 = g_score[0];
        hal_vdp_write_vram((uint16_t)(base1 + 6u),
                           (uint8_t)(0x47u + ((s0 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 7u),
                           (uint8_t)(0x47u + (s0 & 0x0Fu)));
    }

    /* Hi-score digits: 6 dígitos en (col 10, row 1) */
    {
        uint8_t h2 = g_hiscore[2];
        hal_vdp_write_vram((uint16_t)(base1 + 10u),
                           (uint8_t)(0x47u + ((h2 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 11u),
                           (uint8_t)(0x47u + (h2 & 0x0Fu)));
        uint8_t h1 = g_hiscore[1];
        hal_vdp_write_vram((uint16_t)(base1 + 12u),
                           (uint8_t)(0x47u + ((h1 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 13u),
                           (uint8_t)(0x47u + (h1 & 0x0Fu)));
        uint8_t h0 = g_hiscore[0];
        hal_vdp_write_vram((uint16_t)(base1 + 14u),
                           (uint8_t)(0x47u + ((h0 >> 4) & 0x0Fu)));
        hal_vdp_write_vram((uint16_t)(base1 + 15u),
                           (uint8_t)(0x47u + (h0 & 0x0Fu)));
    }

    /* Text overlays en MAP area (replicando Z80 sub_4E0C + sub_4E60/4E76) */
    {
        if (g_intro_active) {
            /* Demo mode: "DEMO" en (18,1), "GAME" en (19,2) */
            camera_draw_string(18u, 1u, 0x56CAu, 0x59u, 0u);
            camera_draw_string(19u, 2u, 0x56CFu, 0x59u, 0u);
        } else if (!(g_player_y & 0x08u)) {
            /* Sin mapa: "NO" en (19,1), "MAP" en (19,2) */
            camera_draw_string(19u, 1u, 0x6472u, 0x59u, 0u);
            camera_draw_string(19u, 2u, 0x6476u, 0x59u, 0u);
        }
        /* else: gameplay normal — map tiles de hud_fill_rect se mantienen */
    }

    /* Key icons: row 2 col 3+ (0x01-0x0C) */
    {
        extern uint8_t doors_keys_collected(void);
        uint8_t keys = doors_keys_collected();
        uint8_t kc = 0u;
        for (uint8_t k = keys; k != 0u; k >>= 1u) { if (k & 1u) kc++; }
        for (uint8_t i = 0u; i < kc && i < 12u; i++) {
            hal_vdp_write_vram((uint16_t)(base2 + 3u + i),
                               (uint8_t)(0x01u + i));
        }
    }

    /* Life hearts: row 3 col 3+ (tile 0x0D)
     * Muestra vidas EXTRA (g_lives-1) — la vida actual no cuenta */
    {
        uint16_t base3 = (uint16_t)(VRAM_NAME_BASE + 3u * 32u);
        uint8_t lives = (g_lives > 0u) ? (uint8_t)(g_lives - 1u) : 0u;
        if (lives > 6u) lives = 6u;
        for (uint8_t i = 0u; i < lives; i++) {
            hal_vdp_write_vram((uint16_t)(base3 + 3u + i), 0x0Du);
        }
    }

    /* Map/logo tiles at cols 17-30 for rows 1-2 already written by
     * hud_fill_rect above — no need to re-write. */
}

/* ==========================================================================
 * sub_629D — Render de cadena de texto (llamada 8×)
 *
 * Entrada: H=col inicial, L=row, DE=puntero a cadena ASCII en ROM
 * La cadena termina en 0x40.
 * Cada carácter:
 *   0x20 = espacio → tile 0
 *   0x30-0x39 = dígito → tile = (byte - 0x30) + 0x5D
 *   otro → tile = byte - 0x41 + C (donde C = tile base)
 *
 * sub_62C2: escribe el tile en VRAM y espera B frames (sub_5128 × B)
 * ========================================================================== */
void camera_draw_string(uint8_t col, uint8_t row,
                        uint16_t rom_str_addr, uint8_t tile_base,
                        uint8_t delay_frames)
{
    if (!g_rom) return;

    uint16_t addr = rom_str_addr;
    uint8_t  c    = col;

    while (true) {
        uint32_t off = (uint32_t)addr - ROM_ORG;
        if (off >= g_rom_size) break;
        uint8_t byte = g_rom[off];
        addr++;

        if (byte == 0x40u) break;  /* fin de cadena */

        uint8_t tile;
        if (byte == 0x20u) {
            tile = 0u;  /* espacio */
        } else if (byte >= 0x30u) {
            tile = (uint8_t)(byte - 0x30u + 0x5Du);  /* Z80: chr - 0x30 + 0x5D */
        } else {
            tile = (uint8_t)(byte - 0x41u + tile_base);  /* fallthrough chr < 0x30 */
        }

        /* Escribir tile en la name table */
        uint16_t vram_addr = (uint16_t)(VRAM_NAME_BASE
                             + (uint16_t)row * 32u + c);
        hal_vdp_write_vram(vram_addr, tile);
        c++;

        /* Esperar delay_frames frames (sub_62CF) */
        for (uint8_t f = 0; f < delay_frames; f++) {
            hal_wait_vsync();
        }
    }
}

/* ==========================================================================
 * Variables de música accedidas por camera_update
 * (expuestas por music.c via game.h)
 * ========================================================================== */
/* (globals defined above) */

/* ==========================================================================
 * INICIALIZACIÓN
 * ========================================================================== */
void camera_init(void)
{
    g_trigger_flags          = 0;
    g_trigger_flags2         = 0;
    g_map_clear_flag         = 0;
    g_music_transpose_fine   = 0;
    g_music_transpose_coarse = 0;
    g_music_tempo_counter    = 0;
}
