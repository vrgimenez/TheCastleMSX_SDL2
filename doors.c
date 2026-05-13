/*
 * THE CASTLE — Puertas, switches, coleccionables y trampas
 * =========================================================
 *
 * Este archivo traduce:
 *
 *   sub_442D  — update_doors():  dispatcher de puertas y switches (1 llamada/frame)
 *   sub_4499  — check_door_exit(): colisión jugador↔puerta de salida
 *   sub_434A  — update_collectibles(): update de llaves, power-ups y rollers
 *   sub_438D  — check_key_pickup(): ¿el jugador recogió una llave?
 *   sub_4406  — update_traps():  pinchos/trampas (tile 0x1F)
 *
 * Objetos interactivos identificados por su tile ID:
 *
 *   Tile 0x0C  → DOOR_ANIMATED (puerta que cicla una animación de apertura)
 *   Tile 0x0D  → DOOR_SOLID    (puerta sólida que se abre cuando se pulsa switch)
 *   Tile 0x0F  → SWITCH        (palanca/switch: cambia estado de puerta)
 *   Tile 0x1C  → BLOCK_LEFT    (bloque empujable hacia la izquierda)
 *   Tile 0x1D  → BLOCK_RIGHT   (bloque empujable hacia la derecha)
 *   Tile 0x1F  → TRAP_SPIKE    (pincho retráctil)
 *   Tile 0x21  → KEY_ITEM      (llave recogible)
 *   Tile 0x23  → KEY_USED      (hueco donde estaba la llave)
 *   Tile 0x34  → ROLLER        (rodillo — actualizado por enemies.c)
 *
 * RAM de objetos:
 *   0xE43E  — Tabla de objetos interactivos: 16 slots × 5 bytes (stride=5)
 *             (compartida con la tabla de partículas de particles.c)
 *               [0] type   — tile ID del objeto (0=inactivo)
 *               [1] ?      — parámetro extra (depende del tipo)
 *               [2] col    — columna en tiles
 *               [3] row    — fila en tiles
 *               [4] state  — estado/flags del objeto
 *
 *   0xE3D6  — Tabla de puertas de salida: 16 slots × 4 bytes (stride=4)
 *               [0] active — 0=inactiva, otro=activa
 *               [1] type   — 0x21=puerta izq, 0x23=puerta der
 *               [2] col
 *               [3] row
 *
 *   0xE386  — Tabla de coleccionables: 16 slots × 5 bytes (stride=5)
 *               [0] type   — tile ID (0=inactivo)
 *               [1] ?
 *               [2] col
 *               [3] row
 *               [4] anim   — contador de animación / estado
 *
 *   0xE416  — Tabla de murciélagos/keys: 8 slots × 5 bytes (stride=5)
 *               [0] type
 *               [1] ?
 *               [2] col
 *               [3] row
 *               [4] move_flags
 *
 * Datos de animación de puertas en ROM:
 *   0x77F2  — Tabla de tiles para animación de puerta izquierda (tipo 0x0C)
 *             B filas × 4 bytes: 4 tile IDs por fila de animación
 *   0x7802  — Tabla de tiles para animación de switch (tipo 0x0F)
 *             Mismo formato.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"

/* ==========================================================================
 * EXTERNOS
 * ========================================================================== */
extern uint8_t  g_state_flags;  /* 0xEAC9 — bits bajos = frame counter       */
extern uint8_t  g_player_col;   /* 0xE334 */
extern uint8_t  g_player_row;   /* 0xE335 */
extern uint8_t  g_room_exit;    /* 0xEAE1 */
extern uint8_t  g_map[0x400];   /* mapa de tiles                              */
extern const uint8_t *g_rom;    /* puntero a la ROM completa (tiles.c)        */

/* ==========================================================================
 * CONSTANTES DE TILE
 * ========================================================================== */
#define TILE_DOOR_ANIM    0x0Cu  /* puerta animada (cicla frames)             */
#define TILE_DOOR_SOLID   0x0Du  /* puerta sólida (switch la abre)            */
#define TILE_SWITCH       0x0Fu  /* switch / palanca                          */
#define TILE_BLOCK_LEFT   0x1Cu  /* bloque empujable izquierda                */
#define TILE_BLOCK_RIGHT  0x1Du  /* bloque empujable derecha                  */
#define TILE_TRAP_SPIKE   0x1Fu  /* pincho retráctil                          */
#define TILE_KEY          0x21u  /* llave recogible                           */
#define TILE_KEY_HOLE     0x23u  /* hueco de llave (después de recogerla)     */
#define TILE_ROLLER       0x34u  /* roller (gestionado por enemies.c)         */

/* Número de slots y stride de cada tabla */
#define OBJ_SLOTS         16
#define OBJ_STRIDE         5
#define EXIT_SLOTS        16
#define EXIT_STRIDE        4
#define COLL_SLOTS        16
#define COLL_STRIDE        5
#define BAT_SLOTS          8
#define BAT_STRIDE         5

/* ROM: tablas de animación de puertas */
#define ROM_DOOR_ANIM_TABLE   0x77F2u  /* animación de puerta tipo 0x0C       */
#define ROM_SWITCH_ANIM_TABLE 0x7802u  /* animación de switch tipo 0x0F       */
#define ROM_ORG               0x4000u

/* VRAM name table base */
#define VRAM_NAME_BASE        0x1800u

/* ==========================================================================
 * ESTRUCTURAS DE SLOTS
 * ========================================================================== */

typedef struct {
    uint8_t type;    /* tile ID del objeto (0 = inactivo) */
    uint8_t param;   /* parámetro extra                   */
    uint8_t col;
    uint8_t row;
    uint8_t state;   /* flags / contador de estado        */
} ObjSlot;           /* 0xE43E: tabla de objetos interactivos */

typedef struct {
    uint8_t active;  /* 0 = inactivo                      */
    uint8_t type;    /* 0x21=izq, 0x23=der                */
    uint8_t col;
    uint8_t row;
} ExitDoor;          /* 0xE3D6: tabla de puertas de salida */

typedef struct {
    uint8_t type;
    uint8_t param;
    uint8_t col;
    uint8_t row;
    uint8_t anim;    /* contador de animación              */
} CollSlot;          /* 0xE386: tabla de coleccionables     */

typedef struct {
    uint8_t type;
    uint8_t param;
    uint8_t col;
    uint8_t row;
    uint8_t move_flags;
} BatSlot;           /* 0xE416: tabla de murciélagos/llaves */

/* ==========================================================================
 * ESTADO GLOBAL
 * ========================================================================== */
static ObjSlot  g_objects[OBJ_SLOTS];
static ExitDoor g_exit_doors[EXIT_SLOTS];
static CollSlot g_collectibles[COLL_SLOTS];
static BatSlot  g_bat_slots[BAT_SLOTS];

/* Keys recogidas (bitmask de 8 bits, una por slot) */
static uint8_t  g_keys_collected;

/* ==========================================================================
 * HELPERS INTERNOS
 * ========================================================================== */

/* Leer 1 byte de la ROM (mismo helper que tiles.c) */
static inline uint8_t rom_rb(uint16_t addr)
{
    extern uint32_t g_rom_size;
    uint32_t off = (uint32_t)addr - ROM_ORG;
    if (!g_rom || off >= g_rom_size) return 0xFFu;
    return g_rom[off];
}

/* Escribir tile en la name table del VDP */
static void vdp_write_tile(uint8_t col, uint8_t row, uint8_t tile)
{
    uint16_t addr = (uint16_t)(VRAM_NAME_BASE + (uint16_t)row * 32u + col);
    hal_vdp_write_vram(addr, tile);
}

/* Leer tile del mapa de colisión (mismo que particles.c pero local) */
static uint8_t map_tile(uint8_t col, uint8_t row)
{
    if (col >= 20u || row >= 30u) return 0xFFu;
    return g_map[(uint16_t)row * 20u + col];
}

/* sub_4488 — Leer (param, col, row, state) del slot a partir de HL+1..HL+4
 * Aquí simplificado: el llamador ya tiene el puntero al slot struct. */
static void slot_read_extra(const ObjSlot *s,
                            uint8_t *col_out, uint8_t *row_out,
                            uint8_t *state_out)
{
    *col_out   = s->col;
    *row_out   = s->row;
    *state_out = s->state;
}

/* ==========================================================================
 * sub_72BA — Cargar parámetros del slot en IX (equivalente)
 *
 * Original: PUSH HL / POP IX  → IX apunta al slot
 *           A=(IX+1) H=(IX+2) L=(IX+3) C=(IX+4)
 *
 * En C: simplemente leer los campos del struct.
 * Retorna: anim_id en A, col en H, row en L, move_flags en C.
 * ========================================================================== */
/* (No necesita función propia en C: accedemos directamente a los campos) */

/* ==========================================================================
 * sub_72F4 / sub_72F8 — Dibujar animación de puerta/switch en name table
 *
 * Entrada:
 *   A        = frame de animación (g_state_flags & 0x03 para puerta animada,
 *              invertido para puerta sólida)
 *   C        = tile ID base (0x0C para door_anim, 0x0D para door_solid,
 *              0x0F para switch)
 *   HL=(col,row) = posición del objeto
 *   IX       = tabla de tiles de animación en ROM (0x77F2 o 0x7802)
 *
 * Lógica (sub_72F8):
 *   A = A × 4           ; cada frame ocupa 4 bytes en la tabla
 *   offset = A + (B-1)*4  ; B = número de filas del objeto
 *   IX += offset
 *   Loop B veces:
 *     D = (IX+0..2)     ; 3 tiles horizontales de la fila
 *     CALL sub_6A7C     ; dibujar tile en (col,   row) con D
 *     INC H
 *     D = (IX+1)        ; tile siguiente
 *     CALL sub_6A7C     ; dibujar tile en (col+1, row) con D
 *     INC H
 *     D = (IX+2)
 *     CALL sub_6A7C     ; dibujar tile en (col+2, row) con D
 *     B es el contador de filas: loop hacia arriba (sub_7306: INC H)
 *     IX += 4           ; siguiente fila de la tabla
 *     DJNZ
 *   Último: (IX+3) en (col+3, row)
 *
 * En C: lee B×4 bytes de la tabla ROM y los escribe en la name table.
 * ========================================================================== */
static void door_draw_animation(uint8_t col, uint8_t row,
                                uint8_t anim_frame,
                                uint16_t table_addr,
                                uint8_t rows)
{
    /* Calcular offset en la tabla: frame × 4 + (rows-1) × 4 */
    uint16_t offset = (uint16_t)((uint16_t)anim_frame * 4u
                                + (uint16_t)(rows - 1u) * 4u);
    uint16_t ix = (uint16_t)(table_addr + offset);

    /* sub_7306: loop de B filas, 3 tiles horizontales + 1 extra */
    for (uint8_t r = 0; r < rows; r++) {
        vdp_write_tile(col,     row, rom_rb((uint16_t)(ix)));
        vdp_write_tile(col + 1, row, rom_rb((uint16_t)(ix + 1u)));
        vdp_write_tile(col + 2, row, rom_rb((uint16_t)(ix + 2u)));
        ix += 4u;
        row++;
    }
    /* Último tile (IX+3 del último frame) en la columna final */
    vdp_write_tile(col + 3, row - 1u, rom_rb((uint16_t)(ix - 1u)));
}

/* ==========================================================================
 * sub_7341 — Switch que modifica el mapa de colisión directamente
 *
 * Cuando BIT 5 de g_state_flags está activo (switch "activado"):
 *   Itera B veces (filas del switch):
 *     Calcula offset en g_map con sub_49B6 (row*30+col)
 *     Escribe 0x10 en la celda del mapa de colisión (= suelo)
 *     Escribe 0x00 en la celda 0x258 bytes más adelante (pared opuesta)
 *   Avanza col (INC H)
 *
 * Efecto: convierte una sección de pared en suelo transitable al activar
 * el switch. El offset 0x258 = 600 decimal = 30×20 bytes = el mapa completo,
 * lo que sugiere que el juego tiene una segunda capa del mapa para el
 * estado "switch activo".
 * ========================================================================== */
static void switch_toggle_map(uint8_t col, uint8_t row, uint8_t rows)
{
    for (uint8_t r = 0; r < rows; r++) {
        uint16_t off = (uint16_t)((uint16_t)row * 20u + col);
        if (off < sizeof(g_map)) {
            g_map[off] = 0x10u;                /* suelo activo */
            if (off + 0x258u < sizeof(g_map)) {
                g_map[off + 0x258u] = 0x00u;   /* limpiar capa opuesta */
            }
        }
        col++;
    }
}

/* ==========================================================================
 * sub_72CA — Animar puerta tipo 0x0C (puerta que cicla sola)
 *
 * Original:
 *   CALL sub_72BA        ; cargar params del slot en IX
 *   A = g_state_flags & 0x03   ; frame = 2 bits bajos del frame counter
 *   C = 0x0C            ; tile base de la puerta animada
 *   CALL sub_72F4        ; dibujar animación usando tabla 0x77F2
 *   JP sub_7199          ; restaurar registros y RET
 * ========================================================================== */
static void door_animated_update(ObjSlot *s)
{
    uint8_t anim_frame = g_state_flags & 0x03u;
    door_draw_animation(s->col, s->row, anim_frame,
                        ROM_DOOR_ANIM_TABLE, s->param ? s->param : 4u);
}

/* ==========================================================================
 * sub_72DD — Animar puerta sólida tipo 0x0D (se abre con switch)
 *
 * Original:
 *   CALL sub_72BA
 *   A = 0x03 - (g_state_flags & 0x03)  ; frame invertido (cierra→abre)
 *   C = 0x0D
 *   CALL sub_72F4  → tabla 0x77F2
 *   JP sub_7199
 *
 * La inversión del frame hace que la puerta se vea "abierta" cuando
 * el switch está activo y "cerrada" cuando no lo está.
 * ========================================================================== */
static void door_solid_update(ObjSlot *s)
{
    uint8_t raw_frame = g_state_flags & 0x03u;
    uint8_t anim_frame = (uint8_t)(0x03u - raw_frame);  /* invertir */
    door_draw_animation(s->col, s->row, anim_frame,
                        ROM_DOOR_ANIM_TABLE, s->param ? s->param : 4u);
}

/* ==========================================================================
 * sub_7326 — Switch / palanca tipo 0x0F
 *
 * Si BIT 5 de g_state_flags == 0 (switch no activado globalmente):
 *   A = g_state_flags & 0x03  ; frame normal
 *   C = 0x0F                  ; tile switch
 *   CALL sub_72F8             ; tabla 0x7802 (switch anim)
 *   JP sub_7199
 *
 * Si BIT 5 == 1 (switch activado):
 *   Rama sub_7341: modifica el mapa de colisión directamente.
 *   Itera B (=rows del switch) veces con INC H (col+0, col+1...).
 *   sub_7344: escribe 0x10 en g_tilemap[row*30+col] y 0x00 en [offset+0x258]
 *   JP sub_7199
 * ========================================================================== */
static void switch_update(ObjSlot *s)
{
    bool switch_global = (g_state_flags & 0x20u) != 0u;  /* BIT 5 */

    if (!switch_global) {
        /* Dibujar animación del switch */
        uint8_t anim_frame = g_state_flags & 0x03u;
        door_draw_animation(s->col, s->row, anim_frame,
                            ROM_SWITCH_ANIM_TABLE, s->param ? s->param : 2u);
    } else {
        /* Switch activado: modificar mapa de colisión */
        uint8_t rows = s->param ? s->param : 2u;
        switch_toggle_map(s->col, s->row, rows);
    }
}

/* ==========================================================================
 * sub_4601 / sub_4609 — Blocks empujables (tipo 0x1C izq, 0x1D der)
 *
 * sub_4488 lee (col=B, row=C, state=A) del slot.
 * sub_4601: D=0x01 → CALL sub_4611 (mover bloque izquierda)
 * sub_4609: D=0x02 → CALL sub_4611 (mover bloque derecha)
 *
 * sub_4611 (el núcleo del bloque):
 *   BIT 3,A (bit 3 de state):
 *     = 0 → rama "caer" (sub_4618):
 *         INC C (col+1), loop D veces:
 *           CALL sub_452D → testea si el jugador está encima del bloque
 *           Si está → marca para empuje (bit en H)
 *           INC B (fila+2)  DEC D
 *         Luego sub_462E: CALL sub_468A × D veces → aplica movimiento del bloque
 *         Si el bloque llega al suelo → animar aterrizaje
 *     = 1 → rama "empujar" (sub_4648):
 *         Verifica si el jugador empuja en la dirección D
 *         Si sí → mover bloque (actualizar col en slot)
 *         sub_44C2 → comprobar colisión con pared
 *         sub_5D47 → actualizar sprite del bloque
 *
 * Traducción simplificada: el bloque tiene dos fases:
 *   - FALLING (bit 3 = 0): cae por gravedad hasta encontrar suelo
 *   - SLIDING (bit 3 = 1): el jugador lo puede empujar horizontalmente
 * ========================================================================== */
static void block_update(ObjSlot *s, bool push_right)
{
    bool is_falling = (s->state & 0x08u) == 0u;  /* BIT 3 de state */

    if (is_falling) {
        /* Fase de caída: comprobar si hay suelo debajo */
        uint8_t below_col = s->col;
        uint8_t below_row = (uint8_t)(s->row + 1u);

        if (below_row >= 30u || map_tile(below_col, below_row) != 0x00u) {
            /* Hay suelo: pasar a fase sliding */
            s->state |= 0x08u;
        } else {
            /* Seguir cayendo: mover row+1 */
            vdp_write_tile(s->col, s->row, 0x00u);         /* borrar posición actual */
            s->row++;
            vdp_write_tile(s->col, s->row,
                push_right ? TILE_BLOCK_RIGHT : TILE_BLOCK_LEFT);
        }
    } else {
        /* Fase sliding: el jugador puede empujar */
        uint8_t next_col = push_right
                         ? (uint8_t)(s->col + 1u)
                         : (uint8_t)(s->col - 1u);

        /* Comprobar colisión con pared (sub_44C2 / sub_49D4) */
        bool blocked = (next_col >= 20u)
                    || (map_tile(next_col, s->row) != 0x00u)
                    || (map_tile(next_col, (uint8_t)(s->row + 1u)) != 0x00u);

        if (!blocked) {
            /* Mover: borrar posición actual, actualizar col, redibujar */
            vdp_write_tile(s->col, s->row, 0x00u);
            s->col = next_col;
            vdp_write_tile(s->col, s->row,
                push_right ? TILE_BLOCK_RIGHT : TILE_BLOCK_LEFT);

            /* Si el jugador está encima del bloque, moverlo con él
             * (sub_4F75..4F85 en el original: g_player_col ±= 1) */
            if (g_player_row == s->row && g_player_col == s->col) {
                if (push_right && g_player_col < 19u) g_player_col++;
                else if (!push_right && g_player_col > 0u) g_player_col--;
            }
        }
    }
}

/* ==========================================================================
 * sub_442D — update_doors() — dispatcher principal de puertas y switches
 *
 * sub_4490: HL=0xE43E, DE=0x0005, B=0x10 → 16 slots × stride 5
 *
 * Loop B veces (sub_4430..sub_4481):
 *   A = (HL)  ; tipo del slot
 *   CP 0x0C → CALL sub_72CA (puerta animada)
 *   CP 0x0D → CALL sub_72DD (puerta sólida)
 *   CP 0x0F → CALL sub_7326 (switch)
 *   CP 0x1C → si (g_state_flags & 0x07) == 0:
 *               CALL sub_4488 (leer col,row,state)
 *               CALL sub_4601 (block left)
 *               (HL)=A        (guardar nuevo state)
 *               CALL sub_73FB (dibujar animación del bloque)
 *   CP 0x1D → similar con sub_4609 (block right) + sub_743D
 *   HL += DE ; siguiente slot
 *   DJNZ
 * ========================================================================== */
void update_doors(void)
{
    /* Solo actualizar bloques cada 8 frames (g_state_flags & 0x07 == 0) */
    bool block_frame = (g_state_flags & 0x07u) == 0u;

    for (int i = 0; i < OBJ_SLOTS; i++) {
        ObjSlot *s = &g_objects[i];

        switch (s->type) {
            case TILE_DOOR_ANIM:
                door_animated_update(s);
                break;

            case TILE_DOOR_SOLID:
                door_solid_update(s);
                break;

            case TILE_SWITCH:
                switch_update(s);
                break;

            case TILE_BLOCK_LEFT:
                if (block_frame) {
                    block_update(s, false);
                    /* sub_73FB: dibujar animación del bloque izquierda */
                    vdp_write_tile(s->col, s->row, TILE_BLOCK_LEFT);
                }
                break;

            case TILE_BLOCK_RIGHT:
                if (block_frame) {
                    block_update(s, true);
                    /* sub_743D: dibujar animación del bloque derecha */
                    vdp_write_tile(s->col, s->row, TILE_BLOCK_RIGHT);
                }
                break;

            default:
                break;
        }
    }
}

/* ==========================================================================
 * sub_4499 — check_door_exit() — colisión jugador↔puerta de salida
 *
 * sub_4499: HL=0xE3D6, DE=0x0004, B=0x10 → 16 slots × stride 4
 *
 * Loop B veces (sub_44A1..sub_44BF):
 *   A = (HL)   ; active
 *   OR A; JR Z → slot inactivo, siguiente
 *   INC HL; A=(HL); DEC HL  ; leer type sin avanzar
 *   CP 0x21 → CALL sub_74E9 (animación de salida izquierda + g_room_exit=7)
 *   CP 0x23 → CALL sub_7510 (animación de salida derecha + lógica de color)
 *   HL += DE; DJNZ
 *
 * sub_74E9 (puerta de salida izquierda, 0x21):
 *   CALL sub_74DF  → IX = slot, H=(IX+2)=col, L=(IX+3)=row
 *   CALL sub_6F4D  → convertir (col,row) a pixel (H=px, L=py)
 *   A = (g_state_flags & 0x01) + 0x2A  ; tile 0x2A o 0x2B (frames de salida)
 *   B = 0x0B; CALL sub_6EE1            ; dibujar tile en columna 0x0B
 *   A = (g_state_flags & 0x01) × 4     ; D = 0 o 4
 *   C = 0x21; CALL sub_70B6            ; dibujar sprite de puerta 2 tiles
 *   JP sub_7199
 *
 * sub_7510 (puerta de salida derecha, 0x23):
 *   A = g_state_flags & 0x7F
 *   CP 0x10 → JR Z: disparar animación de "entrar por la puerta"
 *   JP NC sub_7199  (si A > 0x10, ya terminó → no hacer nada)
 *   Si A < 0x10: animar la transición de color de los tiles de la puerta
 *     (loop de 3 slots en 0xE969, escribe en color table de VRAM)
 *     HL += 0x60 (avanzar 3 slots, stride=0x60)
 *     DJNZ
 *
 * Simplificación: detectar si el jugador está en la posición de la puerta
 * y setear g_room_exit con el código de dirección correspondiente.
 * ========================================================================== */
void check_door_exit(void)
{
    for (int i = 0; i < EXIT_SLOTS; i++) {
        ExitDoor *e = &g_exit_doors[i];
        if (!e->active) continue;

        /* Comprobar colisión jugador con la puerta */
        if (g_player_col != e->col && g_player_col != (uint8_t)(e->col + 1u))
            continue;
        if (g_player_row != e->row && g_player_row != (uint8_t)(e->row + 1u))
            continue;

        /* Animación de la puerta y setear g_room_exit */
        uint8_t anim_tile = (uint8_t)(0x2Au + (g_state_flags & 0x01u));

        if (e->type == 0x21u) {
            /* Puerta izquierda: sub_74E9
             * Dibujar tile en columna 0x0B de la fila de la puerta */
            vdp_write_tile(0x0Bu, e->row, anim_tile);

            /* Sprite de la puerta: 2 tiles verticales (sub_70B6) */
            uint8_t d = (uint8_t)((g_state_flags & 0x01u) << 2);  /* 0 o 4 */
            vdp_write_tile(e->col,     e->row, (uint8_t)(0x21u + d));
            vdp_write_tile(e->col,     (uint8_t)(e->row + 1u), (uint8_t)(0x21u + d + 1u));

            g_room_exit = 0x07u;  /* salida por la izquierda */

        } else if (e->type == 0x23u) {
            /* Puerta derecha: sub_7510 — lógica de phase */
            uint8_t phase = g_state_flags & 0x7Fu;

            if (phase == 0x10u) {
                /* Fase final: disparar transición de habitación */
                g_room_exit = 0x03u;  /* salida por la derecha */
            } else if (phase < 0x10u) {
                /* Animación de color: el juego rota los colores de los
                 * tiles de la puerta en la color table de VRAM.
                 * sub_7546: loop de 7 bytes decrementando HL en la color table.
                 * Aquí simplificamos: escribir tile de animación. */
                vdp_write_tile(e->col, e->row,
                               (uint8_t)(0x23u + (phase & 0x03u)));
            }
            /* Si phase > 0x10: no hacer nada (ya salió) */
        }
    }
}

/* ==========================================================================
 * sub_4820 — Actualizar coleccionable individual (llaves, power-ups)
 *
 * Entrada: A = anim/state del coleccionable, B=col, C=row
 *
 * BIT 0,A: ¿tiene movimiento horizontal?
 * BIT 2,A: ¿recogido?  (SET 2,H en sub_483F = marcar como recogido)
 * BIT 3,A: ¿cayendo?   (RES 3,H en sub_4843)
 *
 * Lógica:
 *   CALL sub_5D5D → ¿modo título?
 *   Si modo juego:
 *     INC C; INC C   (col+2)
 *     CALL sub_49DC  → ¿tile en (B,C+2) es el jugador? (sub_49DC = "¿hay jugador aquí?")
 *     JR NZ → no: rama de movimiento (sub_4847)
 *     Si sí (jugador en (B, C+2)):
 *       INC B / CALL sub_49DC → también (B+1, C+2)?
 *       Si ambas celdas = jugador:
 *         CALL sub_42F5 × 2  → efecto de recogida (sonido/score)
 *         RES 0,H (quitar H_ACTIVE)
 *         SET 2,H (marcar recogido)
 *         RES 3,H (quitar falling)
 *         JR sub_486D → retornar A=H
 *
 * sub_4847 (movimiento del coleccionable):
 *   CALL sub_47F5  → calcular nueva posición (H = delta_col ajustado)
 *   CALL sub_41F6  → aplicar movimiento (misma que para el jugador)
 *   sub_44C2 × 2   → colisión con paredes derecha e izquierda
 *   sub_4210       → dibujar coleccionable en nueva posición
 * ========================================================================== */
static uint8_t collectible_update_state(CollSlot *s)
{
    uint8_t state = s->anim;
    bool title    = (g_state_flags & 0x01u) != 0u;

    if (title) {
        /* Modo título: solo dibujar en posición actual */
        vdp_write_tile(s->col, s->row, s->type);
        return state;
    }

    /* Comprobar si el jugador está en (col+2, row) y (col+2, row+1) */
    bool player_at = (g_player_col == (uint8_t)(s->col + 2u))
                  && (g_player_row == s->row);
    bool player_below = (g_player_col == (uint8_t)(s->col + 2u))
                     && (g_player_row == (uint8_t)(s->row + 1u));

    if (player_at && player_below) {
        /* ¡Recogida! — sub_42F5 × 2 (efecto: sonido + score) */
        /* score_add(0, 10); */  /* TODO: ajustar valor real */
        state &= ~0x01u;        /* RES 0: quitar H_ACTIVE */
        state |=  0x04u;        /* SET 2: marcar recogido */
        state &= ~0x08u;        /* RES 3: quitar falling */
        g_keys_collected |= (uint8_t)(1u << (s - g_collectibles));

        /* Borrar el coleccionable de la pantalla */
        vdp_write_tile(s->col, s->row, 0x00u);
        return state;
    }

    /* No recogido: mover el coleccionable (sub_4847) */

    /* sub_47F5: calcular delta_col según tiles adyacentes */
    bool move_right = (state & 0x01u) != 0u;  /* BIT 0 = dirección */
    uint8_t next_col = move_right
                     ? (uint8_t)(s->col + 1u)
                     : (uint8_t)(s->col - 1u);

    /* sub_44C2: colisión con pared */
    bool blocked_r = (next_col >= 20u) || (map_tile(next_col, s->row) != 0x00u);
    bool blocked_l = (s->col == 0u) || (map_tile((uint8_t)(s->col - 1u), s->row) != 0x00u);

    if (blocked_r) state &= ~0x01u;  /* RES 0 */
    if (blocked_l) state |=  0x01u;  /* SET 0 */

    /* sub_4210: dibujar en nueva posición */
    if (!blocked_r && move_right) {
        vdp_write_tile(s->col, s->row, 0x00u);
        s->col = next_col;
    } else if (!blocked_l && !move_right) {
        vdp_write_tile(s->col, s->row, 0x00u);
        s->col = next_col;
    }
    vdp_write_tile(s->col, s->row, s->type);

    return state;
}

/* ==========================================================================
 * sub_434A — update_collectibles()
 *
 * Dos pasadas sobre la tabla 0xE386 (16 slots × 5):
 *
 * PASADA 1 (sub_4354..4371):
 *   Para cada slot activo (type != 0):
 *     Lee col=B, row=C, anim=A
 *     CALL sub_4820   → actualizar estado, A = nuevo state
 *     (HL)=A          → guardar en slot
 *     Lee (HL+1) = type; si type != 0x34 → CALL sub_710B (update_roller)
 *
 * PASADA 2 (sub_437C..438C):
 *   Para cada slot activo:
 *     Lee (HL+1) = type; si type == 0x34 → CALL sub_710B (update_roller)
 *
 * En C: iterar g_collectibles[], actualizar estado, luego actualizar rollers.
 * Los rollers que sean coleccionables (type == 0x34) se actualizan en la
 * segunda pasada para respetar el orden de dibujado del juego original.
 * ========================================================================== */
void update_collectibles(void)
{
    /* Pasada 1: todo menos rollers */
    for (int i = 0; i < COLL_SLOTS; i++) {
        CollSlot *s = &g_collectibles[i];
        if (s->type == 0u) continue;

        if (s->type != TILE_ROLLER) {
            s->anim = collectible_update_state(s);
        }
    }

    /* Pasada 2: solo rollers */
    for (int i = 0; i < COLL_SLOTS; i++) {
        CollSlot *s = &g_collectibles[i];
        if (s->type != TILE_ROLLER) continue;

        /* Los rollers en la tabla de coleccionables se actualizan igual
         * que los de la tabla de enemigos (sub_710B). Aquí delegamos. */
        extern void update_roller_by_pos(uint8_t col, uint8_t row,
                                         uint8_t move_flags);
        update_roller_by_pos(s->col, s->row, s->anim);
    }
}

/* ==========================================================================
 * sub_43BF — Actualizar murciélago/key en tabla 0xE416
 *
 * Llamado desde sub_438D para slots activos.
 * CALL sub_5D5D → solo en modo juego.
 * Lee: param=A, col=B, row=C, type=C(+1), move_flags=D(+4)
 * CP 0x36..0x3A → distintos tipos de llave/power-up:
 *   0x36 → sub_4901 (actualizar tipo 0x36)
 *   0x37 → sub_4901 (mismo)
 *   0x38 → sub_48AD
 *   0x39 → sub_4882
 *   0x3A → sub_487A
 *   otro → sub_4872
 * Guarda nuevo state en (HL+4).
 * ========================================================================== */

/* Tipos de llave extendida (en tabla 0xE416) */
#define KEY_TYPE_A  0x36u
#define KEY_TYPE_B  0x37u
#define KEY_TYPE_C  0x38u
#define KEY_TYPE_D  0x39u
#define KEY_TYPE_E  0x3Au

static uint8_t key_update_state(BatSlot *s)
{
    bool title = (g_state_flags & 0x01u) != 0u;
    if (title) return s->move_flags;

    /* Seleccionar rutina según tipo (sub_43BF dispatch) */
    switch (s->type) {
        case KEY_TYPE_A:
        case KEY_TYPE_B:
            /* sub_4901: llave que rebota verticalmente */
            {
                bool going_down = (s->move_flags & 0x08u) != 0u;
                uint8_t next_row = going_down
                                 ? (uint8_t)(s->row + 1u)
                                 : (uint8_t)(s->row - 1u);
                if (next_row >= 30u || map_tile(s->col, next_row) != 0x00u) {
                    s->move_flags ^= 0x08u;  /* invertir dirección */
                } else {
                    vdp_write_tile(s->col, s->row, 0x00u);
                    s->row = next_row;
                    vdp_write_tile(s->col, s->row, s->type);
                }
            }
            break;

        case KEY_TYPE_C:
            /* sub_48AD: llave que oscila horizontalmente a mayor velocidad */
        case KEY_TYPE_D:
            /* sub_4882: similar a C */
        case KEY_TYPE_E:
            /* sub_487A: llave con movimiento combinado */
            {
                bool going_right = (s->move_flags & 0x02u) != 0u;
                uint8_t next_col = going_right
                                 ? (uint8_t)(s->col + 1u)
                                 : (uint8_t)(s->col - 1u);
                if (next_col >= 20u || map_tile(next_col, s->row) != 0x00u) {
                    s->move_flags ^= 0x02u;
                } else {
                    vdp_write_tile(s->col, s->row, 0x00u);
                    s->col = next_col;
                    vdp_write_tile(s->col, s->row, s->type);
                }
            }
            break;

        default:
            /* sub_4872: tipo genérico — solo animar tile */
            {
                uint8_t anim = (uint8_t)(s->move_flags + 1u);
                if (anim >= 8u) anim = 0u;
                s->move_flags = anim;
                vdp_write_tile(s->col, s->row,
                               (uint8_t)(s->type + (anim & 0x03u)));
            }
            break;
    }

    /* Comprobar colisión con jugador */
    if (g_player_col == s->col && g_player_row == s->row) {
        g_keys_collected |= (uint8_t)(1u << 7);  /* key especial recogida */
        vdp_write_tile(s->col, s->row, 0x00u);
        s->type = 0u;
    }

    return s->move_flags;
}

/* ==========================================================================
 * sub_438D — check_key_pickup()
 *
 * 0xE416: 8 slots × 5 bytes.
 *
 * Pasada 1 (sub_4395..439F):
 *   Para cada slot activo: CALL sub_43BF (actualizar estado)
 *
 * Pasada 2 (sub_43A6..43BE):
 *   Para cada slot activo:
 *     (IX+4) & 0x05 != 0 → CALL sub_43BF (actualizar si tiene move flags)
 *     CALL sub_719D        (update_bat — si es murciélago)
 * ========================================================================== */
void check_key_pickup(void)
{
    /* Pasada 1: actualizar todos los slots activos */
    for (int i = 0; i < BAT_SLOTS; i++) {
        BatSlot *s = &g_bat_slots[i];
        if (s->type == 0u) continue;
        s->move_flags = key_update_state(s);
    }

    /* Pasada 2: si tiene H_ACTIVE o V_ACTIVE, actualizar de nuevo;
     * y si es murciélago, llamar update_bat (sub_719D desde enemies.c) */
    for (int i = 0; i < BAT_SLOTS; i++) {
        BatSlot *s = &g_bat_slots[i];
        if (s->type == 0u) continue;

        if (s->move_flags & 0x05u) {
            s->move_flags = key_update_state(s);
        }

        /* Si el tipo corresponde a un murciélago → update_bat */
        if (s->type == 0x36u || s->type == 0x37u) {
            extern void update_bat_by_slot(uint8_t col, uint8_t row,
                                           uint8_t move_flags);
            update_bat_by_slot(s->col, s->row, s->move_flags);
        }
    }
}

/* ==========================================================================
 * sub_47B8 — Actualizar trampa/pincho individual (tile 0x1F)
 *
 * Entrada: A = state del pincho (BIT 0 = activo)
 *
 * BIT 0,A: si 0 → no activo, RET
 * BIT 0 activo:
 *   CALL sub_5D5D → ¿modo juego?
 *   INC B; INC B  (row+2)
 *   CALL sub_47EA → ¿col > 0x1D o tile sólido en (row+2)? → Z=1 si libre
 *   JR NZ → hay suelo en row+2: sub_47CB → DEC B×2 (row), CALL sub_47EA(row)
 *     Si Z=1 (también libre arriba): SET 1,H (BIT 1 = sube)
 *   JR Z   → libre: sub_47D7 → DEC B×3 (row-1), CALL sub_47EA(row-1)
 *     Si Z=1: RES 1,H (baja)
 *     Si NZ: RES 0,H (desactivar)
 *
 * Efecto: el pincho sube y baja en función de si hay suelo arriba/abajo.
 * - BIT 0 del state = activo
 * - BIT 1 del state = dirección (0=baja, 1=sube)
 * ========================================================================== */
static uint8_t trap_update_state(uint8_t col, uint8_t row, uint8_t state)
{
    if (!(state & 0x01u)) return state;  /* BIT 0 = inactivo */

    bool title = (g_state_flags & 0x01u) != 0u;

    /* sub_47EA: testear si col > 0x1D o tile sólido en (row+2) */
    bool solid_below = (row + 2u >= 30u)
                    || (map_tile(col, (uint8_t)(row + 2u)) != 0x00u);

    if (solid_below) {
        /* row+2 es sólido: testear (row) */
        bool solid_at = (map_tile(col, row) != 0x00u);
        if (solid_at) {
            state |=  0x02u;   /* SET 1: sube */
        }
    } else {
        /* row+2 libre: testear (row-1) */
        if (row == 0u || map_tile(col, (uint8_t)(row - 1u)) != 0x00u) {
            state &= ~0x02u;   /* RES 1: baja */
        } else {
            state &= ~0x01u;   /* RES 0: desactivar */
        }
    }

    /* Mover el pincho según la dirección */
    bool going_up = (state & 0x02u) != 0u;
    uint8_t new_row = going_up
                    ? (row > 0u ? (uint8_t)(row - 1u) : row)
                    : (uint8_t)(row + 1u);

    if (new_row != row && new_row < 30u) {
        vdp_write_tile(col, row, 0x00u);
        vdp_write_tile(col, new_row, TILE_TRAP_SPIKE);
    }

    /* Colisión con el jugador */
    if (g_player_col == col && g_player_row == new_row) {
        g_room_exit |= 0x07u;  /* daño */
    }

    (void)title;
    return state;
}

/* ==========================================================================
 * sub_4406 — update_traps()
 *
 * sub_4490: HL=0xE43E, DE=0x0005, B=0x10 → misma tabla que update_doors
 *
 * Pasada 1 (sub_4409..441E):
 *   Para cada slot con type == 0x1F:
 *     CALL sub_4488 (leer col,row,state)
 *     CALL sub_47B8 (actualizar pincho)
 *     (HL)=A → guardar nuevo state
 *
 * Pasada 2 (sub_4421..442C):
 *   Para cada slot con type == 0x1F:
 *     CALL sub_7494 (dibujar animación del pincho)
 *
 * sub_7494 (animación del pincho):
 *   CALL sub_72BA → col=H, row=L, move_flags=C
 *   BIT 0,C → si 0 → JP sub_7199 (inactivo)
 *   BIT 1,C → 0x1F si izq, C=0x1F, D=0x02 (sub_70E9×2)
 *            → 1 (derecha): INC H (col+1), D=0x00 (sub_70E9×2)
 * ========================================================================== */
void update_traps(void)
{
    /* Pasada 1: actualizar estado */
    for (int i = 0; i < OBJ_SLOTS; i++) {
        ObjSlot *s = &g_objects[i];
        if (s->type != TILE_TRAP_SPIKE) continue;

        s->state = trap_update_state(s->col, s->row, s->state);
        /* Guardar nueva row si el pincho se movió — simplificado */
    }

    /* Pasada 2: dibujar animación (sub_7494)
     * El pincho tiene 2 frames: D=0x00 (bajado) y D=0x02 (subido)
     * según BIT 1 de move_flags (state). */
    for (int i = 0; i < OBJ_SLOTS; i++) {
        ObjSlot *s = &g_objects[i];
        if (s->type != TILE_TRAP_SPIKE) continue;
        if (!(s->state & 0x01u)) continue;  /* inactivo */

        bool going_up = (s->state & 0x02u) != 0u;
        uint8_t d = going_up ? 0x02u : 0x00u;

        /* sub_70E9 × 2 en posiciones (col, row) y (col+1, row) */
        vdp_write_tile(s->col,            s->row, (uint8_t)(TILE_TRAP_SPIKE + d));
        vdp_write_tile((uint8_t)(s->col + 1u), s->row, (uint8_t)(TILE_TRAP_SPIKE + d + 1u));
    }
}

/* ==========================================================================
 * INICIALIZACIÓN
 * ========================================================================== */
void doors_init(void)
{
    memset(g_objects,      0, sizeof(g_objects));
    memset(g_exit_doors,   0, sizeof(g_exit_doors));
    memset(g_collectibles, 0, sizeof(g_collectibles));
    memset(g_bat_slots,    0, sizeof(g_bat_slots));
    g_keys_collected = 0u;
}

uint8_t doors_keys_collected(void) { return g_keys_collected; }
