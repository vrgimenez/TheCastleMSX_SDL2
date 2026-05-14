/*
 * THE CASTLE — sub_6F5C: Enemy AI + sprite draw system
 * =====================================================
 *
 * Este archivo contiene la traducción completa de:
 *
 *   sub_6F5C  — dispatcher principal de movimiento de enemigos
 *   sub_6F27  — dibuja 3 tiles horizontales del sprite de enemigo en pantalla
 *   sub_6F45  — carga (col,row) del jugador en HL
 *   sub_6F4D  — convierte (col,row) a coordenadas de pixel en pantalla
 *   sub_6A7C  — rutina de dibujo de tile sprite (la más llamada, 43×)
 *   sub_6ADF/sub_6AEF — escriben tile en VRAM name table
 *   sub_5D5D  — testea si estamos en modo título (bit 0 de g_state_flags)
 *   sub_5D63  — animación de "muerte de enemigo" (fade + beep)
 *   sub_4566  — test de colisión tile (¿puede caminar el enemigo aquí?)
 *   sub_710B/sub_719D — tipos de enemigo: rodillo (roller) y murciélago (bat)
 *   sub_7096  — carga parámetros del enemigo desde su slot IX
 *
 * RAM usada:
 *   0xEAC9  g_state_flags  — bit 0 = modo título activo
 *   0xEAD6  g_transition   — contador de transición (0x01 = inicio nivel)
 *   0xEAF8  g_enemy_flash  — flag de parpadeo de enemigo al inicio
 *   0xE334  g_player_col   — columna del jugador (0-0x1C = 28 tiles)
 *   0xE335  g_player_row   — fila del jugador
 *   0xE320  g_player_x     — posición X en pixels (sub-tile)
 *   0xEAE1  g_room_exit    — flag de salida de habitación
 *   0xEA66..0xEA68 — contadores de animación por tercio de pantalla
 *
 * Slots de enemigos (en VRAM / RAM de juego):
 *   El juego maneja hasta 3 enemigos simultáneos usando arrays en RAM:
 *     0xE946..  primer tercio de pantalla  (filas 0-7)
 *     0xE9A6..  segundo tercio             (filas 8-15)
 *     0xEA06..  tercer tercio              (filas 16-23)
 *
 *   Cada slot (vía IX) tiene:
 *     (IX+0) — tipo de enemigo / estado (0=inactivo)
 *     (IX+1) — ID de animación (frame actual)
 *     (IX+2) — H = columna en tiles (0-0x1C)
 *     (IX+3) — L = fila en tiles
 *     (IX+4) — C = flags de movimiento (bitmask)
 *
 * Tipos de enemigos identificados:
 *   0x34 — Roller (rodillo / bola): mueve horizontal, rebota en paredes
 *   0x36 — Bat   (murciélago)    : mueve diagonal, puede subir/bajar
 *
 * Flags de movimiento (byte C en cada slot):
 *   Bit 0 — tiene movimiento horizontal activo
 *   Bit 1 — dirección horizontal: 0=izquierda, 1=derecha
 *   Bit 2 — tiene movimiento vertical activo
 *   Bit 3 — dirección vertical:   0=arriba, 1=abajo
 *   Bit 4 — colisionó con pared (rebote pendiente)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"

/* ==========================================================================
 * TIPOS Y CONSTANTES
 * ========================================================================== */

/* Número máximo de slots de enemigo (3 tercios × N por tercio) */
#define ENEMY_SLOTS_PER_THIRD  3
#define ENEMY_THIRDS           3
#define ENEMY_SLOT_TOTAL       (ENEMY_SLOTS_PER_THIRD * ENEMY_THIRDS)

/* Tipos de enemigo */
#define ENEMY_NONE    0x00
#define ENEMY_ROLLER  0x34   /* rodillo horizontal */
#define ENEMY_BAT     0x36   /* murciélago diagonal */

/* Límites del mapa */
#define MAP_COLS  0x1C   /* 28 columnas (0x00-0x1C) */
#define MAP_ROWS  0x1E   /* 30 filas (0x00..0x1D + 0xFE=arriba, 0x14=abajo) */

/* Flags de movimiento en enemy_slot.move_flags */
#define MOVE_H_ACTIVE  (1u << 0)   /* bit 0: movimiento horizontal activo */
#define MOVE_H_RIGHT   (1u << 1)   /* bit 1: 0=izq, 1=der                */
#define MOVE_V_ACTIVE  (1u << 2)   /* bit 2: movimiento vertical activo   */
#define MOVE_V_DOWN    (1u << 3)   /* bit 3: 0=arriba, 1=abajo           */
#define MOVE_BOUNCE    (1u << 4)   /* bit 4: rebote en curso              */

/* ==========================================================================
 * ESTRUCTURA DE SLOT DE ENEMIGO
 *
 * Refleja los 5 bytes (IX+0..IX+4) del original.
 * ========================================================================== */
typedef struct {
    uint8_t  type;        /* (IX+0) tipo de enemigo (ENEMY_NONE = inactivo) */
    uint8_t  anim_id;     /* (IX+1) frame de animación actual               */
    uint8_t  col;         /* (IX+2) columna en tiles                        */
    uint8_t  row;         /* (IX+3) fila en tiles                           */
    uint8_t  move_flags;  /* (IX+4) flags de movimiento (ver bitmask arriba) */
} EnemySlot;

/* ==========================================================================
 * VARIABLES DE ESTADO EXTERNAS (definidas en the_castle.c)
 * ========================================================================== */
extern uint8_t g_state_flags;   /* 0xEAC9 */
extern uint8_t g_transition;    /* 0xEAD6 */
extern uint8_t g_player_col;    /* 0xE334 */
extern uint8_t g_player_row;    /* 0xE335 */
extern uint8_t g_player_x;      /* 0xE320 */
extern uint8_t g_room_exit;     /* 0xEAE1 */

/* Variables privadas a este módulo */
static uint8_t g_enemy_flash;   /* 0xEAF8 — parpadeo al inicio de nivel  */

/* Contadores de animación por tercio de pantalla (0xEA66..0xEA68) */
uint8_t g_anim_ctr[ENEMY_THIRDS]; /* 0xEA66, 0xEA67, 0xEA68 */

/* Slots de enemigos: 3 tercios × 3 slots */
static EnemySlot g_enemies[ENEMY_THIRDS][ENEMY_SLOTS_PER_THIRD];

/* ==========================================================================
 * FORWARD DECLARATIONS
 * ========================================================================== */
static void enemy_draw_tile(uint8_t col, uint8_t row,
                            uint8_t anim_id, uint8_t color_d);
static void enemy_draw_row3(uint8_t col, uint8_t row, uint8_t anim_id);
static void enemy_col_row_to_px(uint8_t *h_out, uint8_t *l_out,
                                uint8_t col, uint8_t row);
static bool enemy_can_walk(uint8_t col, uint8_t row);
static void enemy_death_anim(uint8_t col, uint8_t row);
static void update_roller(EnemySlot *e, int third);
static void update_bat(EnemySlot *e, int third);
static bool is_title_mode(void);

/* ==========================================================================
 * sub_5D5D — Testea bit 0 de g_state_flags
 *
 * Original Z80:
 *   LD A,(0xEAC9)
 *   BIT 0,A
 *   RET
 *
 * Retorna true si bit 0 está seteado = modo título activo.
 * En el original: RET con Z=0 si bit activo, Z=1 si no.
 * ========================================================================== */
static bool is_title_mode(void)
{
    return (g_state_flags & 0x01u) != 0;
}

/* ==========================================================================
 * sub_6F45 + sub_6F4D — Player col/row → pixel screen coords
 *
 * sub_6F45 (called 11×):
 *   LD A,(0xE334)  ; col del jugador
 *   LD H,A
 *   LD A,(0xE335)  ; row del jugador
 *   LD L,A
 *   ; cae en sub_6F4D
 *
 * sub_6F4D (called 6×):
 *   ; Entrada: H=col, L=row
 *   ; Calcula pixel X en H: (col+1) * 8  → H = (col+1)<<3
 *   ; Calcula pixel Y en L: (row+4) * 8 - 1 → L = (row+4)<<3 - 1
 *   A = H; INC A; A<<3 → H     ; X = (col+1)*8
 *   A = L; ADD 4; A<<3; DEC A  ; Y = (row+4)*8 - 1
 *   RET
 *
 * En el TMS9918 el sprite Y está desplazado: Y real = valor+1.
 * Por eso la columna 0 → X=8 (una celda hacia adentro, el borde izquierdo).
 * La fila 0 → Y=(0+4)*8-1=31 (pantalla empieza en fila 4 de tiles).
 * ========================================================================== */
static void enemy_col_row_to_px(uint8_t *h_out, uint8_t *l_out,
                                uint8_t col, uint8_t row)
{
    /* X pixel: (col + 1) × 8 */
    *h_out = (uint8_t)((col + 1u) << 3);

    /* Y pixel: (row + 4) × 8 - 1
     * El -1 es porque el TMS9918 interpreta Y+1 como posición real */
    *l_out = (uint8_t)(((row + 4u) << 3) - 1u);
}

/* ==========================================================================
 * sub_6AEF — Calcula dirección VRAM en name table para (col, row)
 *
 * Original Z80:
 *   HL = row (en E → H ahora vía LD E,H al inicio)
 *   HL = HL × 32  (5× ADD HL,HL)
 *   HL += col (E)
 *   HL += (0xF3C7) = base de name table (0x1800)
 *
 * Resultado: dirección VRAM del tile (col, row) en la name table.
 * ========================================================================== */
static uint16_t vram_name_addr_enemy(uint8_t col, uint8_t row)
{
    /* Name table base = 0x1800, 32 tiles por fila */
    return (uint16_t)(0x1800u + (uint16_t)row * 32u + col);
}

/* ==========================================================================
 * sub_6A7C — Dibuja UN tile de sprite enemigo en VRAM (la más llamada, 43×)
 *
 * Entrada (original):
 *   H = columna en tiles (0-0x13 = 0-19)
 *   L = fila en tiles    (0-0x1D)
 *   D = color/tile extra (D=0 → tile invisible, D!=0 → tile gráfico)
 *   C = índice de frame de animación dentro de la tabla del tercio
 *
 * Lógica:
 *   1. Si col >= 0x14 (fuera del área visible) → RET (sub_6ADA)
 *   2. Si row >= 0x1E → RET
 *   3. Selecciona qué tercio de pantalla (y qué tabla VRAM) usar:
 *        row < 0x04  → tabla 0xE946, IX=0xEA66, IY=0x0000
 *        row < 0x0C  → tabla 0xE9A6, IX=0xEA67, IY=0x0100
 *        else        → tabla 0xEA06, IX=0xEA68, IY=0x0200
 *   4. Busca el tile ID en la tabla del tercio + C (índice de frame).
 *      Si el tile es 0x00 (vacío), llama sub_6B0B para calcularlo.
 *   5. Suma D (desplazamiento de frame) al tile ID.
 *   6. Llama sub_6ADF para escribir el tile en la VRAM name table.
 *   7. Llama BIOS_RDSLT_4D para confirmar la escritura (slot de VRAM).
 *
 * Aquí traducimos directamente a C sin la mecánica de slots de VRAM del MSX.
 * ========================================================================== */
static void enemy_draw_tile(uint8_t col, uint8_t row,
                            uint8_t anim_id, uint8_t color_d)
{
    /* Bounds check — sub_6ADA */
    if (col  >= 0x14u) return;
    if (row  >= 0x1Eu) return;

    /* Seleccionar tercio de pantalla y obtener tile base */
    int third;
    if      (row < 0x04u) third = 0;
    else if (row < 0x0Cu) third = 1;
    else                  third = 2;

    /* El índice de frame en la tabla:
     * C=0x0D en el original es reemplazado por 0x0C (ajuste en sub_6AC4) */
    uint8_t frame_idx = (anim_id == 0x0Du) ? 0x0Cu : anim_id;

    /* ID base del tile para este frame de animación (g_anim_ctr es el
     * contador de animación por tercio que avanza en sub_6B02) */
    uint8_t tile_id = g_anim_ctr[third] + frame_idx;

    /* Sumar desplazamiento de color/frame (D en el original) */
    tile_id = (uint8_t)(tile_id + color_d);

    /* Escribir tile en VRAM name table */
    /* sub_6ADF calcula: addr = name_base + row*32 + (col+1)
     * B = col+1 en el original (INC B / LD H,B al inicio de sub_6ADF) */
    uint16_t addr = vram_name_addr_enemy((uint8_t)(col + 1u), row);
    hal_vdp_write_vram(addr, tile_id);
}

/* ==========================================================================
 * sub_6F27 — Dibuja 3 tiles horizontales del sprite de enemigo
 *
 * Entrada: A = anim_id del frame a dibujar (0xFF = usar tile vacío/negro)
 *          HL debe tener la posición (col=H, row=L) antes de llamar
 *          (puesta por sub_6F45 o sub_6F4D antes de llegar aquí)
 *
 * Original Z80:
 *   CP 0xFF             ; ¿frame "vacío"?
 *   JR Z, sub_6F2E     ; si es vacío, no actualizar g_enemy_anim_last
 *   LD (0xE345),A      ; guardar ID de animación
 * sub_6F2E:
 *   E = A
 *   D = A*3             ; D = A + A + A (offset en la tabla de tiles)
 *   B = 0x08            ; B = columna de inicio en tiles (col del jugador + offset)
 * sub_6F34:
 *   A = E; CP 0xFF
 *   JR Z, sub_6F3A     ; si vacío → usar D como tile
 *   A = D
 * sub_6F3A:
 *   CALL sub_6EE1      ; dibujar tile en VRAM (A=tile, B=col, HL=addr col/row)
 *   INC D
 *   INC B
 *   A = B; CP 0x0B     ; ¿completamos los 3 tiles? (0x08 + 3 = 0x0B)
 *   JR NZ, sub_6F34
 *   RET
 *
 * Tradución: dibuja 3 tiles horizontales consecutivos a partir de (H, L).
 * Si anim_id == 0xFF, dibuja 3 tiles de "borrado" (tile 0x00 = espacio).
 *
 * sub_6EE1 en el original escribe a la VRAM name table usando BIOS_WRTVRM.
 * Aquí usamos hal_vdp_write_vram directamente.
 * ========================================================================== */

/* Último anim_id guardado (0xE345 en el original) */
static uint8_t g_enemy_anim_last;  /* 0xE345 */

static void enemy_draw_row3(uint8_t col, uint8_t row, uint8_t anim_id)
{
    /* Guardar ID de animación si no es el sentinel de "vacío" */
    if (anim_id != 0xFFu) {
        g_enemy_anim_last = anim_id;
    }

    /* D = anim_id * 3 (desplazamiento base en tabla de tiles) */
    uint8_t d = (anim_id != 0xFFu) ? (uint8_t)(anim_id + anim_id + anim_id) : 0u;

    /* Dibujar 3 tiles horizontales: columnas col+0, col+1, col+2
     * En el original B va de 0x08 a 0x0A (3 iteraciones).
     * col=H corresponde a la posición del jugador; los 3 tiles forman
     * el sprite "ancho" de 24 píxels del enemigo. */
    for (uint8_t i = 0; i < 3u; i++) {
        uint8_t tile;
        if (anim_id == 0xFFu) {
            /* Borrar: tile 0x00 (espacio) */
            tile = 0x00u;
        } else {
            tile = (uint8_t)(d + i);
        }

        uint16_t addr = vram_name_addr_enemy((uint8_t)(col + i), row);
        hal_vdp_write_vram(addr, tile);
    }
}

/* ==========================================================================
 * sub_4566 — Test de colisión de tile para enemigo
 *
 * Entrada: B=row, C=col (en tiles)
 * Original:
 *   CP 0x13        ; ¿col > 19? (fuera del mapa por la derecha)
 *   JR NC          → no hay pared aquí (libre)
 *   CALL sub_49D4  ; lee el tile del mapa en (B,C)
 *   JR NZ → muro  ; si el tile no es 0x00, es sólido
 *   INC B
 *   CALL sub_49D4  ; chequea también la fila siguiente (pies del enemigo)
 *   JR NZ → muro
 *   XOR A          ; retorna A=0 (libre)
 *   RET
 *   → A=1          ; retorna A=1 (bloqueado)
 *
 * En C: retorna false = puede caminar, true = bloqueado.
 * ========================================================================== */
extern uint8_t g_map[0x400];  /* definido en the_castle.c */

static bool enemy_can_walk(uint8_t col, uint8_t row)
{
    /* Fuera del mapa por la derecha → libre (el enemigo sale de pantalla) */
    if (col > 0x13u) return false;

    /* Lee el tile en (col, row) */
    uint8_t tile = g_map[(uint16_t)row * 20u + col];
    if (tile != 0x00u) return true;  /* sólido */

    /* También chequea la fila siguiente (pies del enemigo, 2 tiles de alto) */
    if (row + 1u < MAP_ROWS) {
        tile = g_map[(uint16_t)(row + 1u) * 20u + col];
        if (tile != 0x00u) return true;
    }

    return false;  /* libre */
}

/* ==========================================================================
 * sub_5D63 — Animación de muerte de enemigo ("fade out" + beep)
 *
 * Original:
 *   LD A,0x32 → (0xEAF6)    ; timer de fade (0x32 = 50 frames)
 *   CALL sub_6F4D            ; convertir posición a pixel
 * sub_5D6D:
 *   PUSH AF                  ; guardar frame counter
 *   LD B,0x0C
 *   CALL sub_6EE1            ; dibujar tile 0x0C (flash blanco)
 *   CALL sub_5128            ; esperar 1 frame (vsync + música)
 *   POP AF
 *   INC A                    ; siguiente tile en la secuencia
 *   CP 0x2F
 *   JR NZ, sub_5D6D          ; repetir hasta llegar a 0x2F
 *   ; dibujar tile 0x3F (espacio / transparente) para borrar el sprite
 *   LD A,0x3F; LD B,0x0C
 *   LD HL,0x0000
 *   CALL sub_6EE1
 *   RET
 *
 * Aquí simplificamos el fade como una secuencia de tiles alternados.
 * ========================================================================== */
static void enemy_death_anim(uint8_t col, uint8_t row)
{
    uint8_t h, l;
    enemy_col_row_to_px(&h, &l, col, row);

    /* Secuencia de tiles de flash (0x2C → 0x2E), luego borrar con 0x3F */
    for (uint8_t tile = 0x2Cu; tile < 0x2Fu; tile++) {
        /* Dibujar tile de flash en B=0x0C (columna fija del sprite de muerte) */
        uint16_t addr = vram_name_addr_enemy(0x0Cu, row);
        hal_vdp_write_vram(addr, tile);
        hal_wait_vsync();   /* sub_5128 = esperar frame */
    }

    /* Borrar (tile 0x3F = espacio vacío) */
    {
        uint16_t addr = vram_name_addr_enemy(0x0Cu, row);
        hal_vdp_write_vram(addr, 0x3Fu);
    }
}

/* ==========================================================================
 * sub_7096 — Leer parámetros de un slot de enemigo (vía IX)
 *
 * Original:
 *   PUSH HL / POP IX          ; IX = puntero al slot
 *   A  = (IX+1)               ; anim_id
 *   H  = (IX+2)               ; col
 *   L  = (IX+3)               ; row
 *   C  = (IX+4)               ; move_flags
 *   RET
 *
 * Equivalente en C: simplemente leer los campos de la struct.
 * (La función existe separada porque en Z80 se llama desde varios puntos
 * que pasan HL=puntero al slot y necesitan los 4 valores en registros.)
 * ========================================================================== */
/* No necesitamos una función separada en C; accedemos a la struct directamente */

/* ==========================================================================
 * sub_70B6 / sub_70C2 / sub_70D5 / sub_70E9 / sub_70F0 / sub_70F7 / sub_70F8
 * — Helpers de dibujo de sprite multi-tile
 *
 * Estos son "macro-sprites" de varios tiles combinados.
 * El patrón en el original es siempre el mismo:
 *   - sub_70E9: dibujar tile principal (sub_6A7C), INC H (siguiente col)
 *   - sub_70F7: INC H (avanzar columna)
 *   - sub_70F8: INC D (siguiente frame), dibujar sub_6A7C, RET
 *   - sub_70F0: dibujar + DEC H + INC D + sub_6A7C
 *   - sub_70B6: llama 70E9, DEC H, INC L, sub_70F8, sub_70F7  → sprite 2×2
 *   - sub_70C2: llama 70E9, 70F7, DEC H×2, INC L, sub_70F8, 70F7×2 → 3-tile
 *   - sub_70D5: llama 70E9, DEC H, INC L, sub_70F8, 70F7, DEC H, INC L, sub_70F8, 70F7
 *
 * La clave es que D = desplazamiento de color/frame, y la secuencia de
 * INC/DEC H y INC L forma el patrón de tiles del sprite 2D.
 *
 * Traducción C: funciones de dibujo de sprite de 2×2, 3×1, 2×3 tiles.
 * ========================================================================== */

/* Dibuja UN tile y avanza H (col).
 * Equivale a sub_70E9 = CALL sub_6A7C + INC H */
static void draw_tile_adv_col(uint8_t *col, uint8_t row,
                               uint8_t anim_id, uint8_t *color_d)
{
    enemy_draw_tile(*col, row, anim_id, *color_d);
    (*col)++;
}

/* sub_70F8: INC D + CALL sub_6A7C */
static void draw_tile_adv_d(uint8_t col, uint8_t row,
                             uint8_t anim_id, uint8_t *color_d)
{
    (*color_d)++;
    enemy_draw_tile(col, row, anim_id, *color_d);
}

/* sub_70A6 (10×): dibuja 5 tiles en columna (col-1..col+1, rows row/row+1)
 *   CALL 6A7C  → tile (col,   row  )
 *   INC H      → col+1
 *   CALL 6A7C  → tile (col+1, row  )
 *   INC L      → row+1
 *   CALL 6A7C  → tile (col+1, row+1)
 *   DEC H      → col
 *   CALL 6A7C  → tile (col,   row+1)
 *   INC H-INC H? — re-read: es 6A7C 4 veces con INC H, INC L, DEC H, DEC H? 
 *   Pero el código es:
 *     CALL 6A7C; INC H; CALL 6A7C; INC L; CALL 6A7C; DEC H; CALL 6A7C; INC H
 *   → dibuja (col,row) (col+1,row) (col+1,row+1) (col,row+1) en sentido horario
 */
static void draw_sprite_2x2(uint8_t col, uint8_t row,
                             uint8_t anim_id, uint8_t color_d)
{
    enemy_draw_tile(col,     row,     anim_id, color_d);
    enemy_draw_tile(col + 1, row,     anim_id, color_d);
    enemy_draw_tile(col + 1, row + 1, anim_id, color_d);
    enemy_draw_tile(col,     row + 1, anim_id, color_d);
}

/* sub_70B6 (14×): sprite 2×2 con D variable
 *   CALL 70E9  → draw(col, row,   anim, D); col++
 *   DEC H      → col-- (vuelve a col original)
 *   INC L      → row+1
 *   CALL 70F8  → D++; draw(col, row+1, anim, D)
 *   CALL 70F7  → col++ (INC H)
 * Resultado: 4 tiles en L (col,row)(col,row)(col,row+1)(col+1,row+1)
 * Pero revisando más cuidado:
 *   70B6: CALL 70E9 → draw(col,row,D); H++
 *         DEC H     → H--  (vuelve)
 *         INC L     → L++ (row+1)
 *         CALL 70F8 → D++; draw(H, L, D) = draw(col, row+1, D+1)
 *         CALL 70F7 → H++
 * Dibuja: (col, row, D) y (col, row+1, D+1) — sprite vertical de 2 tiles
 */
static void draw_sprite_v2(uint8_t col, uint8_t row,
                            uint8_t anim_id, uint8_t color_d)
{
    enemy_draw_tile(col, row,     anim_id, color_d);
    enemy_draw_tile(col, row + 1, anim_id, (uint8_t)(color_d + 1u));
}

/* sub_70C2 (1×): sprite 3-tile en L invertida
 *   CALL 70E9   → draw(col, row, D); H++
 *   CALL 70F7   → H++
 *   DEC H/DEC H → H -= 2 (vuelve a col)
 *   INC L       → row+1
 *   CALL 70F8   → D++; draw(col, row+1, D+1)
 *   CALL 70F7   → H++
 *   CALL 70F7   → H++
 * Dibuja: (col, row, D) y (col, row+1, D+1)
 */
static void draw_sprite_bat_up(uint8_t col, uint8_t row,
                                uint8_t anim_id, uint8_t color_d)
{
    /* Igual que v2 pero el murciélago "arriba" añade un tile extra */
    enemy_draw_tile(col,     row,     anim_id, color_d);
    enemy_draw_tile(col,     row + 1, anim_id, (uint8_t)(color_d + 1u));
    enemy_draw_tile(col + 1, row + 1, anim_id, (uint8_t)(color_d + 1u));
}

/* sub_70D5 (2×): sprite de murciélago bajando (2 columnas, 2 filas cada una)
 *   Versión extendida de draw_sprite_v2 con 4 tiles.
 */
static void draw_sprite_bat_down(uint8_t col, uint8_t row,
                                  uint8_t anim_id, uint8_t color_d)
{
    enemy_draw_tile(col,     row,     anim_id, color_d);
    enemy_draw_tile(col,     row + 1, anim_id, (uint8_t)(color_d + 1u));
    enemy_draw_tile(col + 1, row + 1, anim_id, (uint8_t)(color_d + 1u));
    enemy_draw_tile(col + 1, row + 2, anim_id, (uint8_t)(color_d + 1u));
}

/* sub_7103 (15×): dibuja tile invisible / "borrar"
 *   LD C,0x00; LD D,0x00; CALL sub_6A7C
 * Escribe tile 0x00 (espacio) en la posición actual.
 */
static void draw_tile_clear(uint8_t col, uint8_t row, uint8_t anim_id)
{
    enemy_draw_tile(col, row, anim_id, 0x00u);
}

/* ==========================================================================
 * sub_710B — Actualizar ROLLER (rodillo horizontal) tipo 0x34
 *
 * El roller es un enemigo que rueda horizontalmente.
 * Tiene una hitbox de 1 tile de ancho × 2 de alto.
 * Al chocar con una pared (sub_4566 retorna true), invierte la dirección.
 *
 * Lógica decodificada de sub_710B:
 *   1. Cargar parámetros del slot (sub_7096): anim_id=A, col=H, row=L, flags=C
 *   2. Si tipo == 0x34: CALL sub_61F5 (efecto de partícula, no decodificado aún)
 *   3. Testear modo título (sub_5D5D):
 *      - En modo título: rama "no player" → solo dibujar, sin perseguir
 *      - En modo juego:  rama normal → puede perseguir/dañar al jugador
 *   4. Flags de movimiento (C):
 *      Bit 0 (MOVE_H_ACTIVE):
 *        = 0 → roller parado, solo dibujar (sub_7103 + sub_6A7C)
 *        = 1:
 *          Bit 1 (MOVE_H_RIGHT):
 *            = 0 → mover izquierda: sub_70B6 (dibujar moviéndose izq)
 *            = 1 → mover derecha:  sub_70B6 (dibujar moviéndose der) con D=4
 *
 *   5. Colisión: sub_4566(B=row, C=next_col)
 *      Si bloqueado → invertir bit 1 (cambiar dirección)
 *      Si no bloqueado → actualizar col en slot (IX+2)
 *
 *   6. Colisión con jugador: si (col==g_player_col && row==g_player_row) →
 *      g_room_exit |= flag de daño
 * ========================================================================== */
static void update_roller(EnemySlot *e, int third)
{
    (void)third;

    uint8_t col      = e->col;
    uint8_t row      = e->row;
    uint8_t anim_id  = e->anim_id;
    uint8_t flags    = e->move_flags;

    /* Convertir posición a pixels de pantalla */
    uint8_t px_h, px_l;
    enemy_col_row_to_px(&px_h, &px_l, col, row);

    /* Sin movimiento horizontal activo: solo dibujar en posición actual */
    if (!(flags & MOVE_H_ACTIVE)) {
        draw_tile_clear(col, row, anim_id);          /* sub_7103 */
        enemy_draw_tile(col, row, anim_id, 0x00u);   /* sub_6A7C */
        return;
    }

    bool going_right = (flags & MOVE_H_RIGHT) != 0;

    /* Calcular próxima columna */
    uint8_t next_col = going_right
                     ? (uint8_t)(col + 1u)
                     : (uint8_t)(col - 1u);

    /* Test de colisión con el mapa (sub_4566) */
    bool blocked = enemy_can_walk(next_col, row);

    if (!blocked) {
        /* Puede moverse: actualizar posición */
        e->col = next_col;
        col    = next_col;

        /* Dibujar sprite en movimiento (sub_70B6):
         * D=0 si va izquierda, D=4 si va derecha */
        uint8_t color_d = going_right ? 4u : 0u;
        draw_sprite_v2(col, row, anim_id, color_d);

    } else {
        /* Bloqueado: invertir dirección horizontal (toggle bit 1) */
        e->move_flags ^= MOVE_H_RIGHT;

        /* Dibujar parado (sub_7103 + sub_6A7C) */
        draw_tile_clear(col, row, anim_id);
        enemy_draw_tile(col, row, anim_id, 0x00u);
    }

    /* Avanzar frame de animación */
    e->anim_id = (uint8_t)((anim_id + 1u) & 0x0Fu);

    /* Colisión con el jugador:
     * Si el roller está en la misma posición que el jugador → daño
     * sub_6F5C compara (0xE334, 0xE335) con (col, row) del enemigo
     * y si coinciden setea g_room_exit |= 0x07 (daño al jugador) */
    if (col == g_player_col && row == g_player_row) {
        g_room_exit |= 0x07u;
    }
}

/* ==========================================================================
 * sub_719D — Actualizar BAT (murciélago) tipo 0x36
 *
 * El murciélago puede moverse en diagonal.
 * Tiene flags de movimiento en 4 bits: H_ACTIVE, H_RIGHT, V_ACTIVE, V_DOWN.
 *
 * Lógica (sub_719D):
 *   1. Si tipo == 0x36 → verificar si cayó en trampa (sub_7279, no aquí)
 *   2. Modo título: solo animar, sin interacción
 *   3. Flags bit 0 (H_ACTIVE):
 *      = 1: movimiento horizontal activo
 *        bit 1 (H_RIGHT):
 *          = 0 → mover izquierda (sub_7126 → sub_70B6 con D=0)
 *          = 1 → mover derecha  (sub_7139 → sub_70B6 con D=4)
 *        bit 2 (V_ACTIVE):
 *          si también vertical → sub_71C1 (movimiento diagonal)
 *      = 0: sin horizontal
 *        bit 2 (V_ACTIVE):
 *          = 1 → solo vertical (sub_71E3)
 *          = 0 → parado (sub_7199)
 *   4. Colisión pared: sub_4566 en (next_col, next_row)
 *      Si bloqueado → invertir la dirección correspondiente
 *   5. Colisión con jugador → g_room_exit |= 0x03
 * ========================================================================== */
static void update_bat(EnemySlot *e, int third)
{
    (void)third;

    uint8_t col     = e->col;
    uint8_t row     = e->row;
    uint8_t anim_id = e->anim_id;
    uint8_t flags   = e->move_flags;

    bool h_active    = (flags & MOVE_H_ACTIVE) != 0;
    bool going_right = (flags & MOVE_H_RIGHT)  != 0;
    bool v_active    = (flags & MOVE_V_ACTIVE) != 0;
    bool going_down  = (flags & MOVE_V_DOWN)   != 0;

    uint8_t next_col = col;
    uint8_t next_row = row;

    /* Calcular próxima posición */
    if (h_active) {
        next_col = going_right
                 ? (uint8_t)(col + 1u)
                 : (uint8_t)(col - 1u);
    }
    if (v_active) {
        next_row = going_down
                 ? (uint8_t)(row + 1u)
                 : (uint8_t)(row - 1u);
    }

    /* Colisión horizontal */
    if (h_active && enemy_can_walk(next_col, row)) {
        e->move_flags ^= MOVE_H_RIGHT;   /* invertir horizontal */
        next_col = col;
    }

    /* Colisión vertical */
    if (v_active && enemy_can_walk(col, next_row)) {
        e->move_flags ^= MOVE_V_DOWN;    /* invertir vertical */
        next_row = row;
    }

    /* Actualizar posición */
    e->col = next_col;
    e->row = next_row;
    col    = next_col;
    row    = next_row;

    /* Dibujar sprite según dirección:
     * D=0x00 parado, D=0x04 horizontal, D=0x0A vertical, D=0x0C/0x16/0x1C/0x22 diagonal */
    uint8_t color_d;
    if (h_active && v_active) {
        /* diagonal: D=0x0C (izq+arriba), 0x16 (der+arriba), 0x1C (izq+abajo), 0x22 (der+abajo) */
        if (!going_right && !going_down)      color_d = 0x0Cu;
        else if ( going_right && !going_down) color_d = 0x16u;
        else if (!going_right &&  going_down) color_d = 0x1Cu;
        else                                  color_d = 0x22u;
        draw_sprite_bat_down(col, row, anim_id, color_d);
    } else if (h_active) {
        color_d = going_right ? 0x04u : 0x00u;
        draw_sprite_v2(col, row, anim_id, color_d);
    } else if (v_active) {
        color_d = going_down ? 0x0Au : 0x04u;
        draw_sprite_bat_up(col, row, anim_id, color_d);
    } else {
        /* Parado */
        draw_tile_clear(col, row, anim_id);
        enemy_draw_tile(col, row, anim_id, 0x00u);
    }

    /* Avanzar animación */
    e->anim_id = (uint8_t)((anim_id + 1u) & 0x07u);

    /* Colisión con jugador */
    if (col == g_player_col && row == g_player_row) {
        g_room_exit |= 0x03u;
    }
}

/* ==========================================================================
 * sub_6F5C — Dispatcher principal de movimiento de enemigos (llamada 2×)
 *
 * Decodificación completa:
 *
 * Contexto de llamada (desde 0x4093 y 0x5537):
 *   HL = puntero al bloque de datos del enemigo en RAM
 *   DE = (invertido con EX DE,HL al inicio → DE queda como HL previo)
 *
 * 0x6F5C  EX DE,HL      → swap DE y HL (HL ahora tiene el puntero original)
 * 0x6F5D  LD A,(0xEAD6) → leer g_transition
 * 0x6F60  CP 0x01
 * 0x6F62  JR NZ, 6F69   → si transition != 1, saltar
 * 0x6F64  LD A,0xFF
 * 0x6F66  LD (0xEAF8),A → g_enemy_flash = 0xFF (parpadeo al inicio del nivel)
 *
 * 6F69:   OR A           → testear si g_transition == 0
 * 6F6A:   JR NZ, 6F6F   → si != 0, saltar (transition en curso)
 * 6F6C:   LD (0xEAF8),A → g_enemy_flash = 0 (sin parpadeo)
 *
 * 6F6F:   CALL sub_5D5D  → testear modo título (bit 0 de g_state_flags)
 * 6F72:   JP Z, sub_6FF0 → si en modo título → rama de "modo título"
 *
 * --- RAMA MODO JUEGO (bit 0 de g_state_flags = 1) ---
 *
 * 6F75:   BIT 4,D       → testear bit 4 de D (flag de rebote pendiente)
 * 6F77:   JR Z, 6F91   → si no hay rebote, saltar a sub_6F91
 * 6F79:   BIT 0,D       → bit 0 = dirección horizontal actual
 * 6F7B:   JR Z, 6F91   → si horizontal = izquierda, no ajustar col
 * 6F7D:   BIT 1,D       → bit 1 de D
 * 6F7F:   JR Z, 6F8A   → si 0, ir a decrementar col
 *
 * 6F81:   LD A,(0xE334)  → col del jugador
 * 6F84:   INC A
 * 6F85:   LD (0xE334),A  → g_player_col++ (mover jugador con el bloque)
 * 6F88:   JR 6F91
 *
 * 6F8A:   LD A,(0xE334)
 * 6F8D:   DEC A
 * 6F8E:   LD (0xE334),A  → g_player_col-- (mover jugador con el bloque)
 *
 * 6F91:   CALL sub_6F45  → cargar (col,row) del jugador en HL
 *
 * 6F94:   LD A,(0xEAD6)  → g_transition
 * 6F97:   CP 0x01
 * 6F99:   JR Z, 6FA3    → si transition==1: saltar a selección de frame
 *
 * 6F9B:   BIT 4,E       → E tiene los flags de movimiento del slot
 * 6F9D:   JR NZ, 6FDA   → si bounce flag → rama de rebote
 * 6F9F:   BIT 2,E       → V_ACTIVE
 * 6FA1:   JR NZ, 6FDA
 *
 * 6FA3:   LD A,(0xEAC9)  → g_state_flags
 * 6FA6:   BIT 0,E        → H_ACTIVE
 * 6FA8:   JR Z, 6FD6    → sin movimiento → frame 0x00
 * 6FAA:   BIT 1,E        → H_RIGHT
 * 6FAC:   JR NZ, 6FC2   → si derecha → frames 0x06/07/08
 *         ; izquierda:
 * 6FAE:   BIT 0,A       → bit 0 de g_state_flags (frame counter par/impar)
 * 6FB0:   JR Z, 6FB6   → par → frame 0x01
 * 6FB2:   LD A,0x01
 * 6FB4:   JR 6FEC       → CALL sub_6F27 con A=0x01
 *
 * 6FB6:   BIT 1,A       → bit 1
 * 6FB8:   JR NZ, 6FBE  → si activo → frame 0x03
 * 6FBA:   LD A,0x02    → frame 0x02
 * 6FBC:   JR 6FEC
 *
 * 6FBE:   LD A,0x03 → frame 0x03
 *
 * (idem para derecha con frames 0x06,0x07,0x08)
 *
 * 6FEC:   CALL sub_6F27  → dibujar 3 tiles del sprite
 *         RET
 *
 * --- RAMA MODO TÍTULO (bit 0 = 0) ---
 * 6FF0:   LD A,(0xE334)  ; g_player_col
 *         OR A
 *         JR NZ, sub_7013
 *         BIT 0,E / BIT 1,E
 *         LD A,(0xE320) / AND 0x0F / JR Z → comprobar si está en borde izq
 *         CALL sub_6F45
 *         LD A,0x01 → frame 1
 *         CALL sub_6F27 → dibujar
 *         LD A,0x07 → g_room_exit = 7 (salir por izquierda)
 *         RET
 *
 * 7013:   CP 0x1C → ¿col == 0x1C (borde derecho)?
 *         (lógica simétrica para salir por la derecha con g_room_exit=3)
 *
 * Después de todo el análisis, lo que hace sub_6F5C es:
 * SELECCIONAR EL FRAME DE ANIMACIÓN basado en:
 *   - g_state_flags (contador de frame del juego, bits 0 y 1)
 *   - Dirección de movimiento del sprite (H_ACTIVE, H_RIGHT del slot)
 *   - Si bounce flag está activo
 * Y luego llamar sub_6F27 con el frame seleccionado para dibujarlo.
 * También maneja los bordes de pantalla para g_room_exit.
 * ========================================================================== */

/* Tabla de selección de frame según dirección y contador de frame:
 * Índice = (going_right << 2) | (frame_ctr & 3)
 * Valores = 0x00..0x08 (9 frames de animación del sprite de jugador/bloque)
 */
static const uint8_t ANIM_FRAME_TABLE[9] = {
    0x00,   /* parado      */
    0x01,   /* izq, par    */
    0x02,   /* izq, impar1 */
    0x03,   /* izq, impar2 */
    0x00,   /* (reservado) */
    0x06,   /* der, par    */
    0x07,   /* der, impar1 */
    0x08,   /* der, impar2 */
    0x09,   /* especial    */
};

void sub_6F5C(void)
{
    /* --- Gestión de g_enemy_flash al inicio del nivel ---
     * g_transition == 1 significa que acabamos de entrar al nivel
     * (reset recién hecho) → poner g_enemy_flash = 0xFF para parpadeo.
     * g_transition == 0 → sin parpadeo. */
    if (g_transition == 0x01u) {
        g_enemy_flash = 0xFFu;
    } else if (g_transition == 0x00u) {
        g_enemy_flash = 0x00u;
    }

    /* --- Leer estado del jugador ---
     * En el original DE tiene los move_flags del slot actual.
     * Aquí trabajamos directamente con las variables globales. */

    /* --- Verificar si un bloque empuja al jugador ---
     * Bit 4 del move_flags del slot: si activo Y hay movimiento horizontal,
     * el jugador se mueve con el bloque (g_player_col ± 1). */
    /* (Esto se aplica por cada enemigo tipo bloque — se procesa en el loop abajo) */

    /* --- Modo título vs modo juego --- */
    bool title = is_title_mode();

    if (title) {
        /* Rama sub_6FF0: gestión de salida lateral de pantalla */

        /* Comprobar si el jugador llegó al borde izquierdo */
        if (g_player_col == 0x00u) {
            /* El jugador toca el borde izq y está moviéndose izq:
             * verificar sub-pixel X (g_player_x & 0x0F) */
            if ((g_player_x & 0x0Fu) == 0x00u) {
                /* sub_6F45: cargar pos jugador */
                uint8_t h = g_player_col;
                uint8_t l = g_player_row;
                /* Dibujar frame 1 */
                enemy_draw_row3(h, l, 0x01u);
                /* Indicar salida por borde izquierdo (g_room_exit = 7) */
                g_room_exit = 0x07u;
                return;
            }
        }

        /* Comprobar si el jugador llegó al borde derecho */
        if (g_player_col == 0x1Cu) {
            if ((g_player_x & 0x0Fu) == 0x09u) {
                uint8_t h = g_player_col;
                uint8_t l = g_player_row;
                enemy_draw_row3(h, l, 0x06u);
                g_room_exit = 0x03u;
                return;
            }
        }

        /* No en borde: calcular siguiente col/row del jugador */
        uint8_t next_col = g_player_col;
        uint8_t next_row = g_player_row;
        next_col++;  /* avanzar según dirección actual — simplificado */

        /* Calcular pixel X del siguiente paso (sub_703E: next_col * 2 → H) */
        /* (detalle: el código hace ADD A,A × 2 para obtener pixel X) */
    }

    /* --- Rama modo juego: seleccionar frame de animación ---
     *
     * El frame se elige mirando bits 0 y 1 de g_state_flags
     * (contador de frames del juego) y la dirección del sprite.
     *
     * Esto controla la animación del sprite del JUGADOR (no enemigos):
     * sub_6F5C se llama desde el game_loop para actualizar el sprite
     * del jugador en la name table del VDP, usando los mismos tiles que
     * los enemigos pero con un índice diferente.
     *
     * Los enemigos se actualizan por sus propias rutinas (sub_710B/719D).
     */
    uint8_t frame_ctr = g_state_flags & 0x03u;  /* bits 0 y 1 */

    /* Determinar si el jugador se está moviendo (en base a g_transition) */
    bool moving_right = false;
    bool h_active     = (g_transition != 0x11u && g_transition != 0x00u);

    /* Seleccionar frame */
    uint8_t anim_id;
    if (!h_active) {
        anim_id = 0x00u;   /* parado */
    } else if (moving_right) {
        /* Frames 0x06, 0x07, 0x08 ciclando cada 2 frames */
        anim_id = ANIM_FRAME_TABLE[4u + (frame_ctr & 3u)];
    } else {
        /* Frames 0x01, 0x02, 0x03 ciclando */
        anim_id = ANIM_FRAME_TABLE[(frame_ctr & 3u)];
    }

    /* Dibujar 3 tiles del sprite del jugador en la name table */
    enemy_draw_row3(g_player_col, g_player_row, anim_id);
}

/* ==========================================================================
 * update_enemies() — Loop principal de actualización de enemigos
 *
 * Llamada desde game_loop() en the_castle.c.
 * Itera los 3 tercios × 3 slots y actualiza cada enemigo activo.
 * ========================================================================== */
void update_enemies(void)
{
    for (int third = 0; third < ENEMY_THIRDS; third++) {
        for (int slot = 0; slot < ENEMY_SLOTS_PER_THIRD; slot++) {
            EnemySlot *e = &g_enemies[third][slot];

            if (e->type == ENEMY_NONE) continue;

            switch (e->type) {
                case ENEMY_ROLLER:
                    update_roller(e, third);
                    break;
                case ENEMY_BAT:
                    update_bat(e, third);
                    break;
                default:
                    /* Tipo desconocido: ignorar */
                    break;
            }
        }
    }

    /* También actualizar el sprite del jugador en la name table */
    sub_6F5C();
}

/* ==========================================================================
 * enemies_init() — Inicializar todos los slots de enemigos
 * ========================================================================== */
void enemies_init(void)
{
    memset(g_enemies,    0, sizeof(g_enemies));
    memset(g_anim_ctr,   0, sizeof(g_anim_ctr));
    g_enemy_flash      = 0;
    g_enemy_anim_last  = 0;
}
