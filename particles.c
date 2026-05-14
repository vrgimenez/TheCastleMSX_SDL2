/*
 * THE CASTLE — sub_61F5 (efecto de partículas) + sub_7279 (trampa murciélago)
 * ===========================================================================
 *
 * Este archivo contiene:
 *
 *   sub_61F5  — Efecto de "chispa" al aplastarse un roller sobre el jugador.
 *               Busca slots de partículas libres en 0xE43E y dispara sub_735F.
 *
 *   sub_735F  — Spawn de partícula individual: determina dirección según
 *               colisión de mapa (sub_474E) y dibuja el sprite de chispa.
 *
 *   sub_7279  — Animación del murciélago "atrapado": el murciélago se quedó
 *               sin espacio vertical y anima un ciclo de 4 fases basado en
 *               (g_state_flags & 0x3F). Cuando llega a 0x38 borra su sprite.
 *
 *   sub_6C9B  — Actualiza el estado de animación del tile de fondo (copia
 *               datos de ROM a VRAM para el frame actual de animación).
 *
 *   sub_5D24  — Multiplicación de 16 bits (HL = BC × DE), usada por sub_49B6
 *               para calcular offset en el mapa: offset = row*30 + col.
 *
 *   sub_49B6  — Convierte (B=row, C=col) a offset en g_map[].
 *   sub_49C7  — Lee tile del mapa en (B=row, C=col).
 *   sub_49D4  — Testea si el tile en (B,C) es sólido (bits 4-5 del tile).
 *
 * RAM relevante:
 *   0xE43E  — Array de 16 slots de partículas, stride=5, sentinel=0x1B
 *   0xEAF9  — Timer de fade del sprite de muerte (decrementado cada frame)
 *   0xE343  — Timer de animación de chispa canal A
 *   0xE344  — Timer de animación de chispa canal B
 *   0xEAF2  — Velocidad de partícula (sub-pixel X)
 *   0xEAF4  — Dirección de partícula (0xFF = izquierda)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"

/* ==========================================================================
 * EXTERNOS (definidos en the_castle.c / enemies.c)
 * ========================================================================== */
extern uint8_t g_state_flags;   /* 0xEAC9 */
extern uint8_t g_player_col;    /* 0xE334 */
extern uint8_t g_player_row;    /* 0xE335 */
extern uint8_t g_map[0x400];    /* mapa de tiles 20×30 */

/* ==========================================================================
 * PARTÍCULAS — layout de la tabla en RAM
 *
 * La tabla en 0xE43E tiene 16 slots, cada uno de 5 bytes, stride=5:
 *   [0]  type   — 0x1B = libre, otro = activo (tipo de partícula)
 *   [1]  col    — columna en tiles
 *   [2]  row    — fila en tiles
 *   [3]  vel_x  — velocidad X (signed, sub-pixel)
 *   [4]  vel_y  — velocidad Y (signed, sub-pixel)
 *
 * sub_61F5 itera los 16 slots buscando type==0x1B (libre) y llama sub_735F.
 * sub_735F rellena el slot y dibuja el sprite de chispa.
 * ========================================================================== */
#define PARTICLE_SLOTS   16
#define PARTICLE_STRIDE   5
#define PARTICLE_FREE   0x1Bu

typedef struct {
    uint8_t type;    /* 0x1B = libre */
    uint8_t col;
    uint8_t row;
    int8_t  vel_x;
    int8_t  vel_y;
} Particle;

static Particle g_particles[PARTICLE_SLOTS];

/* Timers de animación de chispa (0xE343, 0xE344) */
uint8_t g_spark_timer_a;  /* 0xE343 */
uint8_t g_spark_timer_b;  /* 0xE344 */

/* Timer de fade del sprite de muerte (0xEAF9) */
uint8_t g_death_fade_timer;  /* 0xEAF9 */

/* Variables de sub-pixel de partícula (0xEAF2, 0xEAF4) */
static uint8_t g_particle_spd;  /* 0xEAF2 — velocidad sub-pixel */
static uint8_t g_particle_dir;  /* 0xEAF4 — dirección (0xFF=izq, 0=der) */

/* ==========================================================================
 * sub_49B6 — Convierte (row=B, col=C) a offset de mapa
 *
 * Original Z80:
 *   HL = C (col)
 *   BC = (B=0, C=row)     → tras ajuste LD C,B / LD B,0
 *   DE = 0x001E (= 30)
 *   HL = sub_5D24(BC, DE) → HL = row * 30  (multiplicación 16-bit)
 *   HL += col
 *   RET  ; HL = row*30 + col
 *
 * Nota: el mapa en RAM tiene 30 filas × 20 columnas = 600 bytes,
 * pero el stride es 30 (no 20). Es probable que el juego use un mapa
 * de 30×30 del cual solo 20 columnas son visibles, o que el stride
 * incluya datos de objetos adicionales después de cada fila.
 * Usamos stride=30 como indica el código (DE=0x001E).
 * ========================================================================== */
static uint16_t map_offset(uint8_t row, uint8_t col)
{
    /* HL = row * 30 + col  (sub_5D24 es multiplicación 16-bit) */
    return (uint16_t)((uint16_t)row * 30u + col);
}

/* ==========================================================================
 * sub_49C7 — Lee el tile del mapa en (row=B, col=C)
 *
 * Original Z80:
 *   CALL sub_49B6         ; HL = row*30 + col
 *   HL += 0xE496          ; HL apunta a g_map[offset] en RAM (base 0xE496)
 *   A = (HL)
 *   RET
 *
 * En C usamos g_map[] con base 0xE000; 0xE496-0xE000 = 0x0496 = 1174.
 * Pero el mapa real empieza en 0xE000 con stride 20.
 * Tras analizar la dirección: 0xE496 = 0xE000 + 0x496 → offset 0x496 dentro
 * del bloque de RAM del juego. Con stride 30: row=0,col=0 → 0xE496+0 = 0xE496.
 * Esto sugiere que el array de tiles del mapa tiene su base en 0xE496, no 0xE000.
 * (0xE000..0xE495 = 0x496 = 1174 bytes usados para otros datos de nivel)
 * ========================================================================== */

/* El mapa de tiles con su base real: g_tilemap[row*30 + col] */
static uint8_t g_tilemap[30 * 30];   /* 0xE496 en el MSX original */

static uint8_t map_read_tile(uint8_t row, uint8_t col)
{
    uint16_t off = map_offset(row, col);
    if (off >= sizeof(g_tilemap)) return 0xFFu; /* fuera de bounds = sólido */
    return g_tilemap[off];
}

/* ==========================================================================
 * sub_49D4 — Testea si el tile en (B=row, C=col) tiene bits de solidez
 *
 * Original Z80:
 *   CALL sub_49C7     ; A = tile en (row, col)
 *   AND 0x30          ; aislar bits 4 y 5
 *   RET               ; Z=1 si libre (bits 4-5 = 00), Z=0 si sólido
 *
 * Bits 4-5 del tile codifican el tipo de colisión:
 *   00 = espacio libre (walkable)
 *   01 = suelo          (bit 4)
 *   10 = pared          (bit 5)
 *   11 = techo/sólido   (bits 4 y 5)
 *
 * Retorna: 0 = libre, != 0 = sólido.
 * ========================================================================== */
static uint8_t tile_is_solid(uint8_t row, uint8_t col)
{
    return map_read_tile(row, col) & 0x30u;
}

/* ==========================================================================
 * sub_474E — Detecta dirección libre para la partícula
 *
 * Contexto: llamado desde sub_735F con:
 *   A = move_flags del slot de partícula (bits de dirección)
 *   B = row del enemigo
 *   C = col del enemigo
 *
 * Lógica (simplificada del árbol de saltos):
 *   Bit 3 de A (MOVE_V_ACTIVE = movimiento vertical):
 *     Si 1 (tiene componente vertical):
 *       Probar col-1 (izquierda):
 *         sub_4515 (leer joystick) → AND resultado con tile
 *         Si tile libre: mantener bit 2 de H (V_ACTIVE)
 *         Si no: probar col+4 (4 tiles a la derecha)
 *           Si libre: mantener V_ACTIVE
 *           Si no: RES 2,H → quitar V_ACTIVE
 *     Si 0 (solo horizontal):
 *       Probar col+1:
 *         Si sólido: RES 2,H → sin V_ACTIVE
 *         Si libre:  sub_4744 × 2 (ajustar posición)
 *
 * En la práctica, sub_474E decide si la partícula puede seguir moviéndose
 * en la dirección actual o debe detenerse/rebotar, y ajusta move_flags.
 *
 * Retorna: move_flags ajustado (en C del original).
 * ========================================================================== */
static uint8_t particle_probe_direction(uint8_t move_flags,
                                        uint8_t row, uint8_t col)
{
    bool v_active = (move_flags & 0x08u) != 0; /* bit 3 = MOVE_V_ACTIVE */

    if (v_active) {
        /* Probar columna izquierda (col-1) */
        if (col > 0 && tile_is_solid(row, (uint8_t)(col - 1u)) == 0) {
            /* Libre a la izquierda: mantener flags */
            return move_flags;
        }
        /* Probar columna derecha +4 */
        if (col + 4u < 20u && tile_is_solid(row, (uint8_t)(col + 4u)) == 0) {
            return move_flags;
        }
        /* Bloqueado: quitar bit V_ACTIVE */
        return (uint8_t)(move_flags & ~0x04u);
    } else {
        /* Solo horizontal: probar col+1 */
        if (col + 1u < 20u && tile_is_solid(row, (uint8_t)(col + 1u)) != 0) {
            /* Sólido a la derecha: quitar V_ACTIVE */
            return (uint8_t)(move_flags & ~0x04u);
        }
        /* Libre: ajustar posición (sub_4744 × 2 en el original
         * — aquí simplificado como no-op ya que mueve el sprite) */
        return move_flags;
    }
}

/* ==========================================================================
 * sub_735F — Spawn / actualización de UNA partícula de chispa
 *
 * Llamado desde sub_61F5 cuando encuentra un slot libre (type == 0x1B).
 * Recibe en C el índice de la chispa (0x04 = derecha, 0x0C = izquierda).
 *
 * Original (resumen):
 *   CALL sub_72BA       ; cargar parámetros del slot de enemigo en IX
 *   A = sub_72BA retorna A = anim_id del enemigo (0x34 para roller)
 *   C = A (guardar)
 *   CALL sub_5D5D       ; ¿modo título?
 *   JR NZ, sub_739C     ; en modo título → rama de solo dibujar
 *
 *   ; --- Rama modo juego ---
 *   A = C (anim_id anterior)
 *   B = H (col del enemigo), C = L (row)
 *   CALL sub_474E       ; detectar dirección libre → C = move_flags ajustado
 *   (IX+4) = C         ; guardar move_flags en slot
 *   BIT 2,C / JP Z, sub_73F6  ; si sin V_ACTIVE → solo animar sin mover
 *   BIT 3,C (V_DOWN):
 *     = 0 → INC L / (IX+2)=L  ; mover row+1 (chispa cae)
 *     = 1 → DEC L / (IX+2)=L  ; mover row-1 (chispa sube)
 *   C = 0x1B (tipo de tile de chispa)
 *   D = 0x07 (frame de animación de chispa, izquierda)
 *   CALL sub_70B6       ; dibujar sprite vertical 2-tile en (H,L) con D=7
 *   D = 0x08 / DEC H / INC L×3
 *   CALL sub_70B6       ; dibujar segundo segmento de chispa
 *   JR sub_73F6
 *
 *   ; --- Rama modo título ---
 *   IX = slot del enemigo (PUSH IX)
 *   C = (IX+4) (move_flags)
 *   BIT 2,C / JR Z, sub_73F0  ; si sin V_ACTIVE → borrar y salir
 *   BIT 3,C (V_DOWN):
 *     = 1 → dibujar chispa "bajando" (D=0x03, D=0x05)
 *     = 0 → dibujar chispa "subiendo" (D=0x02/0x04)
 *   sub_73F0: POP IX / (IX+4)=0 / RET
 *
 * El sprite de chispa ocupa 2 segmentos verticales de 2 tiles cada uno
 * (total 4 tiles en forma de "S" o zigzag).
 * ========================================================================== */
static void particle_spawn(Particle *p, uint8_t enemy_col,
                           uint8_t enemy_row, uint8_t spark_col_flag)
{
    /* spark_col_flag: 0x04 = chispa por la derecha, 0x0C = por la izquierda */
    p->type  = spark_col_flag;
    p->col   = enemy_col;
    p->row   = enemy_row;
    p->vel_x = (spark_col_flag == 0x04u) ?  1 : -1;
    p->vel_y = 0;
}

static void particle_update(Particle *p)
{
    if (p->type == PARTICLE_FREE) return;

    bool title = (g_state_flags & 0x01u) != 0;

    /* Calcular move_flags a partir del estado de la partícula */
    uint8_t move_flags = 0;
    if (p->vel_x != 0) move_flags |= 0x01u; /* H_ACTIVE */
    if (p->vel_x > 0)  move_flags |= 0x02u; /* H_RIGHT  */
    if (p->vel_y != 0) move_flags |= 0x04u; /* V_ACTIVE */
    if (p->vel_y > 0)  move_flags |= 0x08u; /* V_DOWN   */

    if (!title) {
        /* Probar dirección y ajustar move_flags */
        move_flags = particle_probe_direction(move_flags, p->row, p->col);

        bool v_active = (move_flags & 0x04u) != 0;
        bool v_down   = (move_flags & 0x08u) != 0;

        if (v_active) {
            /* Mover verticamente */
            if (v_down) {
                p->row++;
            } else {
                if (p->row > 0) p->row--;
            }
        }

        /* Dibujar sprite de chispa en (col, row).
         * El sprite usa tile 0x1B (chispa) con D=0x07/0x08.
         * sub_70B6 dibuja 2 tiles verticales:
         *   (col,   row)   → tile con D=7
         *   (col,   row+1) → tile con D=8
         * y luego un segundo segmento 3 filas más abajo:
         *   (col-1, row+4) → tile con D=8  */
        uint16_t addr;
        addr = (uint16_t)(0x1800u + (uint16_t)p->row * 32u + p->col + 1u);
        hal_vdp_write_vram(addr, 0x1Bu + 7u);   /* tile chispa frame 7 */

        if (p->row + 1u < 24u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(p->row + 1u) * 32u + p->col + 1u);
            hal_vdp_write_vram(addr, 0x1Bu + 8u);
        }
        if (p->row + 4u < 24u && p->col > 0u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(p->row + 4u) * 32u + p->col);
            hal_vdp_write_vram(addr, 0x1Bu + 8u);
        }

        /* Actualizar vel_y desde move_flags */
        p->vel_y = v_active ? (v_down ? 1 : -1) : 0;

        /* Si ya no hay movimiento vertical → liberar slot */
        if (!v_active) {
            p->type = PARTICLE_FREE;
        }

    } else {
        /* Modo título: solo dibujar, no mover.
         * sub_739C dibuja 2 segmentos usando sub_70E9 y sub_7103:
         *   Segmento 1 (D=0x03): (col, row) y (col-1, row+1)
         *   Segmento 2 (D=0x05): (col, row+4) y (col-1, row+5)
         * Cuando no queda V_ACTIVE, borra el sprite y libera el slot. */
        bool v_active = (move_flags & 0x04u) != 0;

        if (!v_active) {
            /* Borrar sprite y liberar */
            uint16_t addr;
            addr = (uint16_t)(0x1800u + (uint16_t)p->row * 32u + p->col + 1u);
            hal_vdp_write_vram(addr, 0x00u);
            p->type = PARTICLE_FREE;
            return;
        }

        bool v_down = (move_flags & 0x08u) != 0;
        uint8_t d1 = v_down ? 0x03u : 0x02u;
        uint8_t d2 = v_down ? 0x05u : 0x04u;

        uint16_t addr;
        addr = (uint16_t)(0x1800u + (uint16_t)p->row * 32u + p->col + 1u);
        hal_vdp_write_vram(addr, 0x1Bu + d1);

        if (p->row + 1u < 24u && p->col > 0u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(p->row + 1u) * 32u + p->col);
            hal_vdp_write_vram(addr, 0x00u);  /* sub_7103 = clear */
        }
        if (p->row + 4u < 24u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(p->row + 4u) * 32u + p->col + 1u);
            hal_vdp_write_vram(addr, 0x1Bu + d2);
        }
        if (p->row + 5u < 24u && p->col > 0u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(p->row + 5u) * 32u + p->col);
            hal_vdp_write_vram(addr, 0x00u);
        }

        /* Quitar V_ACTIVE para el siguiente frame */
        p->vel_y = 0;
    }
}

/* ==========================================================================
 * sub_61F5 — Efecto de partículas al aplastarse roller sobre jugador
 *
 * Llamado desde sub_710B (update_roller) cuando type == 0x34.
 *
 * Entrada (original):
 *   C = move_flags del slot de enemigo (del roller)
 *   H = col del roller
 *   L = row del roller
 *
 * Lógica:
 *   BIT 0,C           ; ¿tiene movimiento horizontal?
 *   JR Z, sub_6237   ; si no → salir sin efecto
 *   E = C (guardar move_flags)
 *   B = H, C = L     ; posición del roller
 *
 *   CALL sub_5D5D     ; ¿modo título?
 *   JR NZ, sub_621B  ; en modo juego → asignar columna de chispa directamente
 *
 *   ; --- Modo título: probar tiles adyacentes ---
 *   DEC C (col-1)
 *   CALL sub_4A38    ; leer tile en (B=row, C=col-1)
 *   JR Z, sub_6211  ; si tile es 0 (vacío) → probar col+1
 *   AND 0x10         ; bit 4 del tile
 *   JR Z, sub_6211  ; si bit 4 = 0 → probar col+1
 *   JR sub_621B     ; bit 4 activo → ir a asignar chispa
 *
 *   sub_6211: INC B
 *   CALL sub_4A38    ; probar (row+1, col-1)
 *   JR Z, sub_6237  ; vacío → sin chispa
 *   AND 0x10
 *   JR Z, sub_6237  ; bit 4 = 0 → sin chispa
 *
 *   sub_621B:
 *   C = 0x04 (chispa por la derecha)
 *   BIT 1,E          ; ¿H_RIGHT activo en el roller?
 *   JR NZ, sub_6223 ; si sí → usar C=0x04 (derecha)
 *   C = 0x0C         ; no → chispa por la izquierda
 *
 *   sub_6223:
 *   HL = 0xE43E      ; inicio de la tabla de partículas
 *   DE = 0x0005      ; stride = 5 bytes por slot
 *   B = 0x10         ; 16 slots a iterar
 *
 *   sub_622B:
 *   A = (HL)          ; leer type del slot
 *   CP 0x1B           ; ¿libre?
 *   JR NZ, sub_6234  ; no → siguiente slot
 *   A = C             ; spark_col_flag
 *   CALL sub_735F    ; spawn de partícula en este slot
 *   sub_6234: HL += DE (siguiente slot)
 *   DJNZ sub_622B
 *   RET
 *
 * sub_4A38 (sub_49C7 con AND 0x01 adicional):
 *   Testea el bit 0 del tile en (B,C). Si activo, lee el tile en
 *   (B, C + 0xE6EE - algo) — es una tabla de colisión extendida.
 *   En la práctica funciona como "¿hay suelo en este tile?".
 *
 * En C: sub_61F5 se vuelve:
 *   si el roller tiene movimiento horizontal → buscar slot libre y spawnear.
 * ========================================================================== */
void sub_61F5(uint8_t move_flags, uint8_t enemy_col, uint8_t enemy_row)
{
    /* BIT 0,C: ¿tiene movimiento horizontal? */
    if (!(move_flags & 0x01u)) return;

    bool going_right = (move_flags & 0x02u) != 0;
    bool title       = (g_state_flags & 0x01u) != 0;

    /* En modo título hacer comprobación adicional de tile adyacente */
    if (title) {
        /* Probar tile en (row, col-1): bit 4 debe estar activo para spawnear */
        uint8_t tile_left = 0;
        if (enemy_col > 0)
            tile_left = map_read_tile(enemy_row, (uint8_t)(enemy_col - 1u));

        bool ok = (tile_left != 0) && (tile_left & 0x10u);

        if (!ok) {
            /* Probar también (row+1, col-1) */
            uint8_t tile_below = 0;
            if (enemy_col > 0 && enemy_row + 1u < 30u)
                tile_below = map_read_tile((uint8_t)(enemy_row + 1u),
                                           (uint8_t)(enemy_col - 1u));
            ok = (tile_below != 0) && (tile_below & 0x10u);
        }

        if (!ok) return; /* ningún tile adyacente tiene bit 4 → sin chispa */
    }

    /* Elegir columna de chispa según dirección del roller */
    uint8_t spark_col_flag = going_right ? 0x04u : 0x0Cu;

    /* Buscar slot libre y spawnear */
    for (int i = 0; i < PARTICLE_SLOTS; i++) {
        if (g_particles[i].type == PARTICLE_FREE) {
            particle_spawn(&g_particles[i], enemy_col, enemy_row, spark_col_flag);
            break; /* solo spawnear en el primer slot libre */
        }
    }
}

/* ==========================================================================
 * update_particles() — Actualizar todas las partículas activas
 *
 * También actualiza los timers de animación de chispa (0xE343, 0xE344)
 * y el fade del sprite de muerte (0xEAF9).
 *
 * El código en 0x623C..0x626F (después del RET de sub_61F5) hace:
 *   HL = 0xEAF9 / A=(HL) / DEC A / (HL)=A   → decrementar death_fade_timer
 *   Cuando llega a 0 → escribir tile 0x3F (espacio) en col 0x0D
 *   Si A < 0x06:
 *     Si A != 0: escribir 0xEAF2=5, 0xEAF4=0xFF
 *     Si A == 0: cargar nueva música (sub_7769 con HL=0x78D2, DE=0x7916)
 *
 *   sub_6265 (×2, para 0xE343 y 0xE344):
 *     DEC timer / Si < 6:
 *       Si != 0: g_particle_spd=5, g_particle_dir=0xFF
 *       Si == 0: cargar música (sub_7769)
 * ========================================================================== */
void update_particles(void)
{
    /* Actualizar partículas individuales */
    for (int i = 0; i < PARTICLE_SLOTS; i++) {
        particle_update(&g_particles[i]);
    }

    /* --- Decremento del timer de fade de muerte (0xEAF9) --- */
    if (g_death_fade_timer > 0) {
        g_death_fade_timer--;

        if (g_death_fade_timer == 0) {
            /* Borrar el sprite de muerte: tile 0x3F en col 0x0D */
            uint16_t addr = (uint16_t)(0x1800u + (uint16_t)g_player_row * 32u + 0x0Du);
            hal_vdp_write_vram(addr, 0x3Fu);
        } else if (g_death_fade_timer < 6u) {
            /* Activar efecto de partícula final */
            if (g_death_fade_timer != 0u) {
                g_particle_spd = 5u;
                g_particle_dir = 0xFFu;
            }
            /* Cuando llega a 0: sub_7769 carga nueva música
             * → delegar a music_load() cuando esté implementado */
        }
    }

    /* --- sub_6265: timer de chispa canal A (0xE343) --- */
    if (g_spark_timer_a > 0) {
        g_spark_timer_a--;
        if (g_spark_timer_a < 6u) {
            if (g_spark_timer_a != 0u) {
                g_particle_spd = 5u;
                g_particle_dir = 0xFFu;
            }
            /* else: cargar música */
        }
    }

    /* --- sub_6265: timer de chispa canal B (0xE344) --- */
    if (g_spark_timer_b > 0) {
        g_spark_timer_b--;
        if (g_spark_timer_b < 6u) {
            if (g_spark_timer_b != 0u) {
                g_particle_spd = 5u;
                g_particle_dir = 0xFFu;
            }
        }
    }
}

/* ==========================================================================
 * sub_7279 — Animación del murciélago "atrapado"
 *
 * Llamado al inicio de sub_719D cuando (IX+1) == 0x36
 * (el slot tiene type=0x36 en el byte de anim_id — indica estado "trampa").
 *
 * El murciélago se queda atascado entre dos paredes o en un callejón sin
 * salida. Anima un ciclo de 4 fases usando (g_state_flags & 0x3F):
 *
 *   g_state_flags & 0x3F:
 *     0x00..0x0F (fase 1): D=0x00, INC L → dibujar en (col, row+1) con C=0x36
 *     0x10..0x17 (fase 2): D=0x02, igual que fase 1
 *     0x18..0x37 (fase 3): según bit 0 de g_state_flags:
 *       bit 0=0 → D=0x04, CALL sub_70B6  (sprite de 2 tiles vertical)
 *       bit 0=1 → D=0x08, CALL sub_70B6
 *     0x38..0x3F (fase 4): borrar sprite (CALL sub_7103 × 2) + reset a fase 2
 *
 * La fase 4 limpia el sprite y rebobina al contador a la fase 2 (0x10)
 * invirtiendo la dirección vertical del murciélago para el próximo ciclo.
 *
 * Después de sub_7279 siempre se salta a sub_7199 (POP BC/DE/HL + RET).
 * ========================================================================== */
void sub_7279(uint8_t *col_inout, uint8_t *row_inout, uint8_t anim_id)
{
    uint8_t col = *col_inout;
    uint8_t row = *row_inout;

    /* Fase basada en los 6 bits bajos del contador de frames */
    uint8_t phase = g_state_flags & 0x3Fu;

    uint16_t addr;

    if (phase < 0x10u) {
        /* Fase 1: D=0x00, dibujar en (col, row+1) con tile 0x36 */
        uint8_t draw_row = (uint8_t)(row + 1u);
        addr = (uint16_t)(0x1800u + (uint16_t)draw_row * 32u + col + 1u);
        hal_vdp_write_vram(addr, 0x36u + 0x00u);

    } else if (phase < 0x18u) {
        /* Fase 2: D=0x02 */
        uint8_t draw_row = (uint8_t)(row + 1u);
        addr = (uint16_t)(0x1800u + (uint16_t)draw_row * 32u + col + 1u);
        hal_vdp_write_vram(addr, 0x36u + 0x02u);

    } else if (phase < 0x38u) {
        /* Fase 3: sprite de 2 tiles vertical (sub_70B6) según bit 0 */
        uint8_t d = (g_state_flags & 0x01u) ? 0x08u : 0x04u;

        /* sub_70B6: draw (col, row, D) + (col, row+1, D+1) */
        addr = (uint16_t)(0x1800u + (uint16_t)row * 32u + col + 1u);
        hal_vdp_write_vram(addr, 0x36u + d);

        if (row + 1u < 24u) {
            addr = (uint16_t)(0x1800u + (uint16_t)(row + 1u) * 32u + col + 1u);
            hal_vdp_write_vram(addr, 0x36u + d + 1u);
        }

    } else {
        /* Fase 4 (0x38..0x3F): borrar sprite y resetear ciclo
         *
         * sub_7103 × 2: escribir tile 0x00 en (col, row) y (col-1, row)
         * Luego volver a fase 2: la lógica en el original hace INC H
         * dos veces y JR a la fase 2 (0x10..0x17).
         * En la práctica esto es un "nop frame" — el sprite desaparece
         * un frame y reaparece en fase 2 el siguiente.
         */
        addr = (uint16_t)(0x1800u + (uint16_t)row * 32u + col + 1u);
        hal_vdp_write_vram(addr, 0x00u);

        if (col > 0u) {
            addr = (uint16_t)(0x1800u + (uint16_t)row * 32u + col);
            hal_vdp_write_vram(addr, 0x00u);
        }
        /* col+1 para el siguiente ciclo (INC H en el original) */
        *col_inout = (uint8_t)(col + 1u);
    }

    (void)anim_id; /* no usado directamente aquí; lo usa sub_70B6 internamente */
}

/* ==========================================================================
 * particles_init() — Inicializar tabla de partículas
 * ========================================================================== */
void particles_init(void)
{
    for (int i = 0; i < PARTICLE_SLOTS; i++) {
        g_particles[i].type  = PARTICLE_FREE;
        g_particles[i].col   = 0;
        g_particles[i].row   = 0;
        g_particles[i].vel_x = 0;
        g_particles[i].vel_y = 0;
    }
    g_spark_timer_a     = 0;
    g_spark_timer_b     = 0;
    g_death_fade_timer  = 0;
    g_particle_spd      = 0;
    g_particle_dir      = 0;
    memset(g_tilemap, 0, sizeof(g_tilemap));
}
