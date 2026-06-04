/*
 * THE CASTLE — Pantalla de título (sub_4A4A)
 * ==========================================
 *
 * ESTRUCTURA DE LA INTRO
 * ----------------------
 * La pantalla de título de The Castle consta de 3 fases que se repiten
 * en ciclo (B=3 en el bucle externo). Cada ciclo completo dura ~7 segundos.
 *
 *   FASE 1 — sub_4B4C: Animación del logo "THE CASTLE"
 *     El logo se desliza desde la izquierda usando la tabla de coordenadas
 *     en ROM 0x56D4 (secuencia 1) y 0x5738 (secuencia 2).
 *     Los datos son pares (col, row) con signo que forman el borde exterior
 *     del texto animado — es una espiral de entrada (los tiles se colocan
 *     desde fuera hacia dentro hasta llegar al centro 0x09,0x0C).
 *     Cada entrada de 2 bytes → sub_4BAF dibuja un sprite de 7×5 tiles
 *     con los datos de texto en ROM (sub_4BDC / sub_4BDF).
 *
 *   FASE 2 — sub_4C0B: Strips de texto de créditos/demo
 *     Cinco tiras horizontales de texto se deslizan desde arriba
 *     hasta su posición final en la pantalla. Cada tira:
 *       BC=(start_row, end_row), DE=puntero a string en ROM
 *       sub_4C3D: decrementa H desde 0x1D hasta B, llamando sub_4C5C
 *       sub_4C5C: dibuja la string en la fila H de la name table
 *     Las strings son texto ASCII con codificación sub_62B0 (igual que scripts).
 *
 *   FASE 3 — sub_4AD7: Espera input del jugador (0x80 = 128 frames)
 *     Loop de 128 frames llamando sub_5128 (vsync + música).
 *     Si el jugador pulsa fire (Z/space/ctrl) → salir del ciclo → empezar juego.
 *
 *   CURTAIN — sub_4B13: Efecto de "cortina" entre ciclos
 *     Limpia las filas de pantalla de par en par (fila 4 y 23, 6 y 21, etc.)
 *     hasta borrar toda la pantalla. Cada par de filas por frame.
 *
 * DATOS EN ROM
 * ------------
 *   0x56D4  — Coordenadas del borde exterior del logo (secuencia espiral)
 *             Cada entrada: 2 bytes (col, row), sentinel=0x80
 *   0x5738  — Coordenadas del núcleo del logo (entrada final)
 *             7 entradas de (col=0x09, row=0x0C..0x06)
 *   0x567F  — String "[ 1985  ISAO YOSHIDA" (créditos)
 *   0x5694  — String siguiente de créditos
 *   0x56AB  — String de demo
 *   0x56B5  — String adicional
 *   0x56B8  — String final de créditos
 *
 * RAM USADA
 * ---------
 *   0xEAE4  g_intro_active  — 1 = intro activa, 0 = terminar intro
 *   0xEACD  g_keyframe_queue[9] — cola de keyframes (0xFF = vacío)
 *   0xEACA  g_player_speed  — velocidad (2 durante logo, 0x20 durante créditos)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"
#include "game.h"

/* ==========================================================================
 * CONSTANTES
 * ========================================================================== */
#define ROM_ORG         0x4000u
#define VRAM_NAME_BASE  0x1800u

/* Dirección de las tablas de datos del intro en ROM */
#define ROM_LOGO_SEQ1   0x56D4u   /* espiral exterior del logo          */
#define ROM_LOGO_SEQ2   0x5738u   /* núcleo interior del logo           */
#define ROM_CREDIT_1    0x567Fu   /* "[ 1985  ISAO YOSHIDA"  */
#define ROM_CREDIT_2    0x5694u   /* "[ 1986 KEISUKE IWAKURA" */
#define ROM_CREDIT_3    0x56ABu   /* "PRESENTED"              */
#define ROM_CREDIT_4    0x56B5u   /* "BY"                     */
#define ROM_CREDIT_5    0x56B8u   /* "ASCII CORPORATION"      */
#define ROM_GAME_MUSIC  0x7ABEu   /* demo AI keyframes + demo music ptr  */

/* Sentinel y terminadores */
#define SEQ_END   0x80u   /* fin de secuencia de coordenadas */
#define STR_END   0x40u   /* fin de string de créditos       */

/* Número de ciclos del demo antes de empezar el juego automáticamente */
#define DEMO_CYCLES   3u

/* Filas de pantalla del logo (viewport de 10×10 empieza en fila 4) */
#define LOGO_ROW_TOP     4u
#define LOGO_ROW_BOTTOM  23u

/* ==========================================================================
 * ROM ACCESS
 * ========================================================================== */
static inline uint8_t rom_rb(uint16_t addr)
{
    uint32_t off = (uint32_t)addr - ROM_ORG;
    if (!g_rom || off >= g_rom_size) return 0xFFu;
    return g_rom[off];
}

/* ==========================================================================
 * HELPERS DE VRAM
 * ========================================================================== */

/* Escribir un tile en la name table en (col, row) */
static void vdp_put(uint8_t col, uint8_t row, uint8_t tile)
{
    if (col >= 32u || row >= 24u) return;
    uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)row * 32u + col);
    hal_vdp_write_vram(addr, tile);
}

/* Limpiar una fila completa de la name table */
static void vdp_clear_row(uint8_t row)
{
    for (uint8_t col = 0; col < 32u; col++) {
        vdp_put(col, row, 0x00u);
    }
}

/* ==========================================================================
 * sub_62B0 — Codificación de carácter a tile (Z80 match)
 *
 * 0x20 = espacio → tile 0
 * 0x30+ = cualquier carácter (dígito o letra) → tile = chr - 0x30 + 0x5D
 *                    (Z80: SUB 0x30; ADD 0x5D; RET NC)
 * otro → tile = chr - 0x41 + tile_base (fallthrough para chr < 0x30)
 *
 * Credit character → tile encoding:
 *   '0'..'9' → tiles 0x5D..0x66
 *   'A'..'Z' → tiles 0x6E..0x87
 *   '['      → tile 0x88
 * ========================================================================== */
static uint8_t char_to_tile(uint8_t chr, uint8_t tile_base)
{
    (void)tile_base;
    if (chr == 0x20u) return 0x00u;
    if (chr >= 0x30u) return (uint8_t)(chr - 0x30u + 0x5Du);
    return (uint8_t)(chr - 0x41u + tile_base);
}

/* ==========================================================================
 * sub_4AE2 — Preparar VRAM para el intro
 *
 * Original:
 *   CALL sub_4B13   → primera cortina (limpiar pantalla)
 *   FILVRM(pat_base + 0x400, 0x00, 0x1400) → limpiar pattern table
 *   FILVRM(col_base + 0x400, 0x11, 0x1400) → rellenar color table con 0x11
 *   Loop B=0x08..0x0D (cols 8 a 13): escribir tile 0x3F (espacio) en col, row=0
 * ========================================================================== */
static void intro_prepare_vram(void);  /* forward decl */

/* ==========================================================================
 * sub_4B3F — Calcular dirección VRAM de name table para row L
 *
 * L×32 (5× ADD HL,HL) + (0xF3C7) = name_base → HL = addr
 * ========================================================================== */
static uint16_t vram_row_addr(uint8_t row)
{
    return (uint16_t)(VRAM_NAME_BASE + (uint16_t)row * 32u);
}

/* ==========================================================================
 * sub_4B2C — Limpiar 32 tiles de una fila (escribe 0x00 en 32 celdas)
 *
 * Original:
 *   CALL sub_4B3F   → HL = addr de la fila
 *   B = 0x20 (32)
 *   Loop: WRTVRM(0x00); INC HL; DJNZ
 * ========================================================================== */
static void clear_row_vram(uint8_t row)
{
    uint16_t addr = vram_row_addr(row);
    for (uint8_t i = 0; i < 32u; i++) {
        hal_vdp_write_vram((uint16_t)(addr + i), 0x00u);
    }
}

/* ==========================================================================
 * sub_4B13 — Efecto de "cortina" (wipe de pantalla)
 *
 * Limpia las filas de la pantalla de par en par, desde fuera hacia dentro:
 *   Frame 1: fila 4  y fila 23 → limpiar
 *   Frame 2: fila 6  y fila 21 → limpiar
 *   ...
 *   Frame n: fila 24 y fila 3  → limpiar (se pasan, fine)
 * hasta L=0x18 (24).
 *
 * Original:
 *   L=0x04, E=0x17
 *   Loop:
 *     sub_4B2C(L)  → limpiar fila L
 *     EX DE,HL
 *     sub_4B2C(L)  → limpiar fila E
 *     EX DE,HL
 *     sub_5128     → esperar 1 frame
 *     L += 2; E -= 2
 *     si L < 0x18 → repetir
 * ========================================================================== */
static void curtain_wipe(void)
{
    uint8_t top = 0x04u;
    uint8_t bot = 0x17u;

    while (top < 0x18u) {
        clear_row_vram(top);
        clear_row_vram(bot);
        hal_wait_vsync();
        top += 2u;
        if (bot >= 2u) bot -= 2u;
    }
}

static void load_title_tiles(void)
{
    tiles_vram_from_rom(0x8056u, 0x73u, 70u);
    tiles_vram_from_rom(0x86F6u, 0x5Du, 10u);
    tiles_vram_from_rom(0x8796u, 0x6Eu, 28u);
}

/* ==========================================================================
 * sub_4BDC / sub_4BDF — Dibujar sprite de logo (7 cols × C filas)
 *
 * sub_4BDC: LD BC,0x0705 → B=7 (ancho), C=5 (alto)
 * sub_4BDF: Loop C filas:
 *   sub_4BE1: Loop B cols:
 *     Si row < 4 o row >= 24 o col >= 32 → skip
 *     Escribir D en (H=col, L=row) de la name table
 *     Si D != 0 → INC D (siguiente tile del sprite)
 *   INC L (siguiente fila)
 *   DEC C
 *
 * El sprite se dibuja en la posición (H=col, L=row) con el tile base D.
 * ========================================================================== */
static void draw_logo_sprite(uint8_t col, uint8_t row, uint8_t tile_base)
{
    uint8_t tile = tile_base;
    for (uint8_t r = 0; r < 5u; r++) {
        for (uint8_t c = 0; c < 7u; c++) {
            uint8_t draw_row = (uint8_t)(row + r);
            uint8_t draw_col = (uint8_t)(col + c);
            if (draw_row < LOGO_ROW_TOP || draw_row >= LOGO_ROW_BOTTOM) continue;
            if (draw_col >= 32u) continue;
            vdp_put(draw_col, draw_row, tile);
            if (tile != 0u) tile++;
        }
    }
}

/* ==========================================================================
 * sub_4BAF — Dibujar un par de posiciones del logo
 *
 * Entrada: DE = (col, row) del sprite
 * Original:
 *   PUSH BC; PUSH DE
 *   D = 0x73 (tile base)
 *   sub_4BB3: PUSH HL; PUSH HL
 *     sub_4BDC(H=col, L=row, D=0x73, C=5) → dibujar sprite
 *     POP HL; H += 7; sub_4BDC → dibujar segunda mitad del sprite
 *     POP HL; POP DE; POP BC
 *
 * Los sprites del logo son 14 tiles de ancho (2× sub_4BDC con H+=7).
 * ========================================================================== */
static void draw_logo_at(uint8_t col, uint8_t row)
{
    draw_logo_sprite(col,        row, 0x73u);
    draw_logo_sprite(col + 7u,   row, 0x73u + 7u * 5u);
}

/* ==========================================================================
 * sub_4BC4 — Scroll del logo: limpiar fila anterior y dibujar en nueva
 *
 * Si C == 0 (secuencia 1 — borrar):
 *   D=0x00 → draw_logo_sprite con tile 0 (borrar)
 * Si C == 1 (secuencia 2 — dibujar):
 *   L += 4; sub_628C × 0x0E (limpiar 14 tiles en L)
 *   Luego draw_logo_at con nuevo (col, row)
 * ========================================================================== */
static void logo_erase_at(uint8_t col, uint8_t row)
{
    /* Borrar los 14×5 tiles del sprite */
    for (uint8_t r = 0; r < 5u; r++) {
        for (uint8_t c = 0; c < 14u; c++) {
            uint8_t dr = (uint8_t)(row + r);
            uint8_t dc = (uint8_t)(col + c);
            if (dr < LOGO_ROW_TOP || dr >= LOGO_ROW_BOTTOM) continue;
            if (dc >= 32u) continue;
            vdp_put(dc, dr, 0x00u);
        }
    }
}

/* ==========================================================================
 * sub_4B7F — Animar logo con una secuencia de coordenadas
 *
 * Itera la secuencia ROM en HL:
 *   sub_4BA6: leer (col, row) = (D, E)
 *     Si D == 0x80 → JR sub_4B93 (fin: retroceder 2 entradas, redibujar)
 *   sub_4BAF: dibujar logo en (D, E)
 *   sub_5128: esperar 1 frame (+ comprobar fire)
 *     Si fire pulsado → NZ → g_intro_active=0, RET
 *   sub_4BC4: borrar posición anterior, dibujar en nueva
 *   Repetir
 *
 * Retorna:
 *   Z=1 si el logo llegó al final de la secuencia (completó el movimiento)
 *   NZ  si el jugador pulsó fire (salir del intro)
 * ========================================================================== */
static bool animate_logo_sequence(uint16_t seq_addr, bool erase_prev, uint8_t *last_col, uint8_t *last_row)
{
    uint16_t ptr = seq_addr;
    uint8_t prev_col = *last_col;
    uint8_t prev_row = *last_row;

    while (true) {
        /* sub_4BA6: leer siguiente entrada */
        uint8_t d = rom_rb(ptr);
        if (d == SEQ_END) {
            /* Fin de secuencia: redibujar las 2 últimas posiciones */
            ptr -= 2u;
            d = rom_rb(ptr);
            uint8_t e = rom_rb((uint16_t)(ptr + 1u));
            draw_logo_at(d, e);
            *last_col = d;
            *last_row = e;
            return true;  /* secuencia completada */
        }
        uint8_t e = rom_rb((uint16_t)(ptr + 1u));
        ptr += 2u;

        /* Borrar posición anterior */
        if (erase_prev && (prev_col != d || prev_row != e)) {
            logo_erase_at(prev_col, prev_row);
        }

        /* Dibujar en nueva posición */
        draw_logo_at(d, e);

        prev_col = d;
        prev_row = e;

        /* Esperar 1 frame y comprobar input */
        hal_wait_vsync();
        hal_poll_events();

        if (!hal_is_running()) {
            g_intro_active = 0;
            *last_col = prev_col;
            *last_row = prev_row;
            return false;
        }

        if (hal_key_pressed()) {
            g_intro_active = 0;
            *last_col = prev_col;
            *last_row = prev_row;
            return false;  /* jugador interrumpió */
        }
    }
}

/* ==========================================================================
 * sub_4B4C — Animación completa del logo (fase 1)
 *
 * Original:
 *   (0xEACA) = 0x02   → g_player_speed = 2 (lento)
 *   HL = 0x56D4       → secuencia 1 (espiral exterior)
 *   sub_4B54: animar con HL
 *     Si completado: HL = 0x56D4, C=0
 *       sub_4B7F(HL, C=0) → segunda pasada sin borrar
 *       Si Z: RET Z (logo completado)
 *     Si NZ (fire): g_intro_active=0; RET NZ
 *   Resultado: logo en posición final (col=0x09, row=0x0C)
 * ========================================================================== */
static bool title_animate_logo(void)
{
    g_player_speed = 0x02u;

    uint8_t last_col = 0xFDu;  /* posición inicial (fuera de pantalla) */
    uint8_t last_row = 0x00u;

    /* Secuencia 1: espiral exterior */
    if (!animate_logo_sequence(ROM_LOGO_SEQ1, false, &last_col, &last_row))
        return false;  /* jugador interrumpió */

    if (g_intro_active == 0) return false;

    /* Segunda pasada de secuencia 1 (sin borrar — fija el logo) */
    last_col = 0xFDu;
    last_row = 0x00u;
    if (!animate_logo_sequence(ROM_LOGO_SEQ1, false, &last_col, &last_row))
        return false;

    if (g_intro_active == 0) return false;

    /* Secuencia 2: núcleo (col=0x09, filas 0x0C..0x06) */
    if (!animate_logo_sequence(ROM_LOGO_SEQ2, false, &last_col, &last_row))
        return false;

    return g_intro_active != 0;
}

/* ==========================================================================
 * sub_4C5C / sub_4C81 — Dibujar/borrar texto de créditos en (row, start_col)
 *
 * sub_4C5C (C=1, "dibujar"): escribe los tiles de la string desde start_col
 * sub_4C81 (C=0, "borrar"):  escribe tile 0x00 desde start_col
 *
 * Z80: H = start_col (loop var), L = C = row (fijo). INC H por cada char.
 * Si H >= 0x20, saltea el caracter (offscreen).
 * HL/DE restaurados al final (PUSH/POP).
 * ========================================================================== */
static void draw_credit_row(uint8_t row, uint8_t start_col,
                            uint16_t str_addr, bool draw)
{
    uint16_t base = vram_row_addr(row);
    uint16_t ptr  = str_addr;
    uint8_t  col  = start_col;

    while (col < 32u) {
        uint8_t chr = rom_rb(ptr++);
        if (chr == STR_END) break;

        uint8_t tile;
        if (!draw) {
            tile = 0x00u;  /* borrar */
        } else {
            tile = char_to_tile(chr, 0x01u);
        }
        hal_vdp_write_vram((uint16_t)(base + col), tile);
        col++;
    }
}

/* ==========================================================================
 * sub_4C3D — Scroll horizontal de créditos (derecha → izquierda)
 *
 * Z80:
 *   H=0x1D (start_col), L=C=row (fijo)
 *   Loop: sub_4C5C, sub_5128, sub_4C81, DEC H, CP B → loop
 *   sub_4C5C final
 * ========================================================================== */
static bool scroll_credit_strip(uint8_t row, uint8_t target_col,
                                uint16_t str_addr)
{
    /* Scroll horizontal: columna inicial = 0x1D, desciende hasta target_col */
    for (uint8_t h = 0x1Du; h > target_col; h--) {
        draw_credit_row(row, h, str_addr, true);
        hal_wait_vsync();
        hal_poll_events();

        if (!hal_is_running()) {
            g_intro_active = 0;
            return false;
        }

        if (hal_key_pressed()) {
            g_intro_active = 0;
            return false;
        }

        draw_credit_row(row, h, str_addr, false);
    }

    draw_credit_row(row, target_col, str_addr, true);
    return g_intro_active != 0;
}

/* ==========================================================================
 * sub_4C0B — Animar los 5 strips de créditos (fase 2)
 *
 * Z80: BC=(target_col, row), DE=string_addr
 *   Strip 1: BC=0x060E, DE=0x567F  → row=14, col=6  → "[ 1985  ISAO YOSHIDA"
 *   Strip 2: BC=0x0510, DE=0x5694  → row=16, col=5  → "[ 1986 KEISUKE IWAKURA"
 *   Strip 3: BC=0x0C12, DE=0x56AB  → row=18, col=12 → "PRESENTED"
 *   Strip 4: BC=0x0F14, DE=0x56B5  → row=20, col=15 → "BY"
 *   Strip 5: BC=0x0816, DE=0x56B8  → row=22, col=8  → "ASCII CORPORATION"
 * ========================================================================== */
static const struct { uint8_t row; uint8_t target_col; uint16_t addr; } CREDIT_STRIPS[5] = {
    { 0x0E, 0x06, ROM_CREDIT_1 },   /* "[ 1985  ISAO YOSHIDA"  */
    { 0x10, 0x05, ROM_CREDIT_2 },   /* "[ 1986 KEISUKE IWAKURA" */
    { 0x12, 0x0C, ROM_CREDIT_3 },   /* "PRESENTED"              */
    { 0x14, 0x0F, ROM_CREDIT_4 },   /* "BY"                     */
    { 0x16, 0x08, ROM_CREDIT_5 },   /* "ASCII CORPORATION"      */
};

static bool title_animate_credits(void)
{
    g_player_speed = 0x20u;

    for (int i = 0; i < 5; i++) {
        if (!scroll_credit_strip(CREDIT_STRIPS[i].row,
                                 CREDIT_STRIPS[i].target_col,
                                 CREDIT_STRIPS[i].addr))
            return false;
        if (!g_intro_active) return false;
    }
    return true;
}

/* ==========================================================================
 * sub_4AD7 — Esperar input del jugador (0x80 = 128 frames)
 *
 * Original:
 *   B = 0x80
 *   Loop: sub_5128 → si fire → RET NZ; DJNZ
 *   XOR A; RET (Z=1 si timeout sin input)
 * ========================================================================== */
static bool title_wait_for_input(void)
{
    for (uint16_t i = 0; i < 0x80u; i++) {
        hal_wait_vsync();
        hal_poll_events();
        if (!hal_is_running()) return false;
        if (hal_key_pressed()) {
            return true;   /* fire pulsado */
        }
    }
    return false;  /* timeout */
}

/* ==========================================================================
 * sub_4AE2 — Preparar VRAM para el intro
 *
 * 1. curtain_wipe() → limpiar pantalla
 * 2. Limpiar patrón y color table del VDP desde offset 0x400
 * 3. Escribir tile 0x3F (espacio) en cols 8..13 de fila 0
 * ========================================================================== */
static void intro_prepare_vram(void)
{
    /* Z80 fiel: NO tocar rows 0-3 (el HUD dibujado por sub_4D52 persiste) */

    curtain_wipe();

    /* Z80 sub_4B07: sprites 8-13 en pixel (0,0) con patrón blank.
     * NO escribe al name table — no tiene equivalente en SDL. */
    // for (uint8_t col = 8u; col < 14u; col++) {
    //     vdp_put(col, 0u, 0x00u);
    // }
}

/* ==========================================================================
 * sub_5327 — Cleanup al salir del intro
 *
 * Original:
 *   Z80 sub_4B07: sprites 8-13 en pixel (0,0) con patrón blank.
 *   No escribe al name table — omitido en SDL.
 *   curtain_wipe() = CALL sub_5128 (wait frames) + cleanup
 * ========================================================================== */
static void intro_cleanup(void)
{
    // for (uint8_t col = 8u; col < 14u; col++) {
    //     vdp_put(col, 0u, 0x00u);
    // }

    /* Limpiar la pantalla completa */
    curtain_wipe();
}

/* ==========================================================================
 * sub_4A29 — Limpiar estado auxiliar (sub_4029)
 *
 * Limpia bytes en 0xEAF1/F2/F4/F5 (transpose, tempo):
 *   (0xEAF1)=0, (0xEAF2)=0, (0xEAF4)=0, (0xEAF5)=0
 * ========================================================================== */
static void reset_aux_state(void)
{
    extern uint8_t g_music_transpose_fine;
    extern uint8_t g_music_transpose_coarse;
    g_music_transpose_fine   = 0u;
    g_music_transpose_coarse = 0u;
    music_set_tempo(0u, 0u);   /* silencio hasta cargar música del juego */
}

/* ==========================================================================
 * sub_4A4A — Pantalla de título + demo + juego
 *
 * Estructura:
 *   1. Inicialización:
 *      g_intro_active = 1
 *      sub_6383 → limpiar keyframe queue
 *      sub_4AE2 → preparar VRAM
 *      Cargar tileset BG1_MAIN
 *      Música del título
 *
 *   2. Bucle principal B=3 ciclos (sub_4A86):
 *      a. sub_4B4C → logo animado        [si fire → goto game_start]
 *      b. sub_4C0B → créditos en scroll  [si fire → goto game_start]
 *      c. sub_4AD7 → esperar 128 frames  [si fire → goto game_start]
 *      d. sub_4B13 → curtain wipe
 *
 *   3. Si 3 ciclos sin input → DEMO MODE:
 *      game_reset_level()
 *      Cargar música/keyframes de juego (0x7ABE)
 *      sub_4029 → limpiar estado
 *      Loop: game_frame() hasta que el jugador pulse fire
 *           o el demo termine (6 salas)
 *
 *   4. JUEGO REAL (al pulsar fire):
 *      game_reset_level()
 *      music_play_game()
 *      enemies_init(), particles_init(), doors_init()
 *      Loop: game_frame() hasta game_over
 *
 *   5. Salida (sub_4AC8):
 *      g_intro_active = 0
 *      music_stop()
 *      intro_cleanup()
 *      RET → a main_loop (sub_401C)
 * ========================================================================== */
void title_screen(void)
{
    /* sub_4D52 a las 0x4019: reset nivel + HUD con "NO"/"MAP" antes del title */
    game_reset_level();
    draw_hud();

    /* Inicialización */
    g_intro_active = 1u;

    /* sub_6383: limpiar keyframe queue */
    memset(g_keyframe_queue, 0xFFu, 9u);

    /* sub_4AE2: preparar VRAM (NO toca rows 0-3, el HUD persiste) */
    intro_prepare_vram();

    tiles_reload_all();

    /* Cargar logo, font y dígitos de crédito desde ROM */
    load_title_tiles();

    tiles_dump_vram("title_init");

    /* Silencio durante la pantalla de título */
    music_stop();

    /* Bucle de 3 ciclos */
    for (uint8_t cycle = 0u; cycle < DEMO_CYCLES; cycle++) {

        /* Fase 1: logo animado */
        if (!title_animate_logo()) goto game_start;
        if (!g_intro_active) goto game_start;

        /* Fase 2: créditos en scroll (font/dígitos cargados por load_title_tiles) */
        if (!title_animate_credits()) goto game_start;
        if (!g_intro_active) goto game_start;

        /* Fase 3: esperar input (créditos visibles) */
        if (title_wait_for_input()) goto game_start;
        if (!g_intro_active) goto game_start;

        /* Curtain entre ciclos (solo name table, pattern table persiste) */
        curtain_wipe();
    }

    /* ======================================================================
     * 3 ciclos sin input → DEMO MODE
     * ====================================================================== */
    game_reset_level();
    {
        uint16_t music_ptr = (uint16_t)(rom_rb(ROM_GAME_MUSIC)
                             | ((uint16_t)rom_rb((uint16_t)(ROM_GAME_MUSIC + 1u)) << 8));
        music_load(music_ptr, 0u);
    }
    g_player_speed = 0x70u;
    reset_aux_state();

    /* Demo loop: corre game_frame() (con keyframes de AI desde 0x7ABE) */
    {
        static int demo_dumped = 0;
        while (g_intro_active) {
            game_frame();
            tiles_animate(g_state_flags);
            if (!demo_dumped) { tiles_dump_vram("demo"); demo_dumped = 1; }
            hal_wait_vsync();

            if (hal_key_pressed()) {
                goto game_start;
            }

            if (!hal_poll_events()) goto exit;
        }
    }

    /* ======================================================================
     * JUEGO REAL (fire presionado durante título o demo)
     * ====================================================================== */
game_start:
    music_stop();
    intro_cleanup();
    g_intro_active = 0u;

    game_reset_level();
    music_play_game();
    enemies_init();
    particles_init();
    doors_init();

    g_game_over  = 0;
    g_room_exit  = 0;
    g_state_flags = 0;

    /* Game loop hasta game over (sub_4064 ejecutado por frame) */
    while (!g_game_over) {
        game_frame();
        tiles_animate(g_state_flags);
        hal_wait_vsync();

        if (!hal_poll_events()) goto exit;
    }

    /* Game over: pausa breve */
    hal_delay(120);

exit:
    /* sub_4AC8: salida limpia */
    g_intro_active = 0u;
    music_stop();
    intro_cleanup();
}
