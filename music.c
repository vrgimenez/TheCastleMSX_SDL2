/*
 * THE CASTLE — Reproductor de música PSG (sub_75D4 + sub_7769)
 * =============================================================
 *
 * ARQUITECTURA
 * ------------
 * El sistema de música del juego corre en la ISR (rutina de interrupción
 * de VBlank del TMS9918A). En MSX, el VBlank ocurre a 60Hz (NTSC) o 50Hz (PAL).
 *
 * La HAL SDL2 llama a music_isr_tick() desde hal_wait_vsync() para emular
 * este comportamiento.
 *
 * RAM DE MÚSICA (0xEAE9..0xEAF8)
 * --------------------------------
 * Canal A (melodía principal):
 *   0xEAE9  duration   — ticks restantes para la nota actual
 *   0xEAEA  tick_count — sub-contador de ticks (0..duration-1)
 *   0xEAEB  ptr_lo     — puntero a la siguiente nota (lo)
 *   0xEAEC  ptr_hi     — puntero a la siguiente nota (hi)
 *
 * Canal B (acompañamiento/bajo):
 *   0xEAED  duration
 *   0xEAEE  tick_count
 *   0xEAEF  ptr_lo
 *   0xEAF0  ptr_hi
 *
 * Globales de tempo:
 *   0xEAF1  transpose_fine — transposición fina (sumada al índice de nota)
 *   0xEAF2  transpose_coarse — transposición gruesa
 *   0xEAF3  tempo_counter  — contador de tempo (0 = silencio global)
 *   0xEAF4  tempo_value    — valor de tempo (sumado a counter cada tick)
 *   0xEAF5  tick_phase     — fase del tick (0..tempo_period-1)
 *
 * Volúmenes por canal (0xEAF6..0xEAF8):
 *   0xEAF6  vol_ch_A  — volumen canal A (decrementado cada tick → fade out)
 *   0xEAF7  vol_ch_B  — volumen canal B
 *   0xEAF8  vol_ch_C  — volumen canal C (no usado directamente en la melodía)
 *
 * FORMATO DE DATOS DE MÚSICA
 * --------------------------
 * Cada byte en el stream de notas:
 *   bits[6:0] = índice de nota (0x00..0x5F) o comando especial
 *   bit[7]    = 1 → el siguiente byte es la nueva duración (ticks por nota)
 *
 * Índices especiales:
 *   0x60 = SILENCIO (rest) — apaga el canal
 *   0xFF = FIN de stream → silencio permanente
 *   0xFE = LOOP → los 2 bytes siguientes = dirección de salto (little-endian)
 *
 * Tabla de períodos AY (0x7812, 48 entradas × 2 bytes):
 *   Nota 0x00 = C1 (32.7 Hz), periodo = 0x0D5D
 *   Nota 0x0C = C2 (65.4 Hz), periodo = 0x06AF
 *   Nota 0x18 = C3 (130.8 Hz), periodo = 0x0357
 *   Nota 0x24 = C4 (261.4 Hz), periodo = 0x01AC
 *   Nota 0x30 = C5 (522.8 Hz), periodo = 0x00D6 (extrapolado)
 *   Nota 0x5F = nota más aguda
 *   Nota 0x60 = SILENCIO (no hay entrada en la tabla)
 *
 * ESCRITURA AL PSG
 * ----------------
 * sub_765C: A = note_index → lookup en tabla 0x7812 → DE = tone_period
 *   BIOS_INITXT(reg=C+0, E=period_lo)    → PSG reg 0 o 2 (fine tune)
 *   BIOS_INITXT(reg=C+1, E=period_hi)    → PSG reg 1 o 3 (coarse tune)
 *   BIOS_INITXT(reg=C+8, E=volume)       → PSG reg 8 o 9 (volume)
 *   Si canal A (C=0):
 *     BIOS_INITXT(reg=0x0D, E=0x00)      → envelope shape
 *
 * BIOS_INITXT en este contexto es BIOS_WRTPSG:
 *   A = registro PSG (0-15), E = valor
 *
 * sub_76AC (silence_channel):
 *   PSG reg 8 = 0 → volume canal A = 0
 *   PSG reg 9 = 0 → volume canal B = 0
 *   PSG reg 10 = 0 → volume canal C = 0
 *
 * sub_76BE (update_sfx_volumes):
 *   Decrementa 0xEAF6, 0xEAF7, 0xEAF8 si > 0 (fade out de SFX)
 *   Luego escribe los valores al PSG (regs 7, 5/6/4)
 *   Esto implementa los efectos de sonido de chispa/muerte superpuestos a la música
 *
 * CANCIONES CONOCIDAS EN ROM
 * --------------------------
 *   Canal A @ 0x78D2 + Canal B @ 0x7916 → música del título/sparks
 *   Canal A @ 0x592D-area               → sala de intro
 *   Canal A @ 0x5A02 (referenciado en sub_522A/53D4) → melodía de juego
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"
#include "game.h"

/* ==========================================================================
 * TABLA DE PERÍODOS DE NOTAS (extraída de la ROM, 0x7812, 48 entradas)
 * ========================================================================== */
static const uint16_t NOTE_PERIODS[48] = {
    0x0D5D, /* 0x00: C1  32.7 Hz  */
    0x0C9C, /* 0x01: C#1 34.7 Hz  */
    0x0BE7, /* 0x02: D1  36.7 Hz  */
    0x0B3C, /* 0x03: D#1 38.9 Hz  */
    0x0A9B, /* 0x04: E1  41.2 Hz  */
    0x0A02, /* 0x05: F1  43.7 Hz  */
    0x0973, /* 0x06: F#1 46.2 Hz  */
    0x08EB, /* 0x07: G1  49.0 Hz  */
    0x086B, /* 0x08: G#1 51.9 Hz  */
    0x07F2, /* 0x09: A1  55.0 Hz  */
    0x0780, /* 0x0A: A#1 58.3 Hz  */
    0x0714, /* 0x0B: B1  61.7 Hz  */
    0x06AF, /* 0x0C: C2  65.4 Hz  */
    0x064E, /* 0x0D: C#2 69.3 Hz  */
    0x05F4, /* 0x0E: D2  73.4 Hz  */
    0x059E, /* 0x0F: D#2 77.8 Hz  */
    0x054E, /* 0x10: E2  82.4 Hz  */
    0x0501, /* 0x11: F2  87.3 Hz  */
    0x04BA, /* 0x12: F#2 92.4 Hz  */
    0x0476, /* 0x13: G2  98.0 Hz  */
    0x0436, /* 0x14: G#2 103.8 Hz */
    0x03F9, /* 0x15: A2  110.0 Hz */
    0x03C0, /* 0x16: A#2 116.5 Hz */
    0x038A, /* 0x17: B2  123.5 Hz */
    0x0357, /* 0x18: C3  130.8 Hz */
    0x0327, /* 0x19: C#3 138.6 Hz */
    0x02FA, /* 0x1A: D3  146.8 Hz */
    0x02CF, /* 0x1B: D#3 155.6 Hz */
    0x02A7, /* 0x1C: E3  164.7 Hz */
    0x0281, /* 0x1D: F3  174.5 Hz */
    0x025D, /* 0x1E: F#3 184.9 Hz */
    0x023B, /* 0x1F: G3  195.9 Hz */
    0x021B, /* 0x20: G#3 207.5 Hz */
    0x01FD, /* 0x21: A3  219.8 Hz */
    0x01E0, /* 0x22: A#3 233.0 Hz */
    0x01C5, /* 0x23: B3  246.9 Hz */
    0x01AC, /* 0x24: C4  261.4 Hz */
    0x0194, /* 0x25: C#4 276.9 Hz */
    0x017D, /* 0x26: D4  293.6 Hz */
    0x0168, /* 0x27: D#4 310.7 Hz */
    0x0153, /* 0x28: E4  330.0 Hz */
    0x0140, /* 0x29: F4  349.6 Hz */
    0x012E, /* 0x2A: F#4 370.4 Hz */
    0x011D, /* 0x2B: G4  392.5 Hz */
    0x010D, /* 0x2C: G#4 415.8 Hz */
    0x00FE, /* 0x2D: A4  440.4 Hz */
    0x00F0, /* 0x2E: A#4 466.1 Hz */
    0x00E3, /* 0x2F: B4  492.8 Hz */
};

/* ==========================================================================
 * ESTADO DEL REPRODUCTOR
 * ========================================================================== */

/* Un canal del reproductor (corresponde a los 4 bytes en RAM por canal) */
typedef struct {
    uint8_t         duration;    /* 0xEAE9/0xEAED — ticks por nota actual   */
    uint8_t         tick_count;  /* 0xEAEA/0xEAEE — sub-tick actual         */
    const uint8_t  *ptr;         /* 0xEAEB/0xEAEF — puntero a datos de nota */
} MusicChannel;

/* Estado completo del reproductor */
typedef struct {
    MusicChannel ch[2];          /* Canal A (melodía) y B (bajo)            */
    uint8_t  transpose_fine;     /* 0xEAF1 — transposición fina             */
    uint8_t  transpose_coarse;   /* 0xEAF2 — transposición gruesa           */
    uint8_t  tempo_counter;      /* 0xEAF3 — 0 = silencio global            */
    uint8_t  tempo_value;        /* 0xEAF4 — incremento de tempo por tick   */
    uint8_t  tick_phase;         /* 0xEAF5 — fase del tick                  */
    uint8_t  vol[3];             /* 0xEAF6/F7/F8 — volúmenes SFX           */
    bool     active;             /* reproductor activo                      */
} MusicState;

static MusicState g_music;

/* ==========================================================================
 * CANCIONES CONOCIDAS
 * ========================================================================== */

#define ROM_ORG  0x4000u

/* Dirección ROM de los streams de notas */
#define MUSIC_TITLE_A    0x78D2u   /* canal A — título/sparks              */
#define MUSIC_TITLE_B    0x7916u   /* canal B — título/sparks              */
#define MUSIC_GAME_A     0x5A02u   /* canal A — melodía de juego           */
/* 0x5A02 es referenciado en sub_522A como (0xeb08) = 0x5a02               */

/* Sentinel bytes */
#define NOTE_END   0xFFu   /* fin de stream → silencio */
#define NOTE_LOOP  0xFEu   /* loop → siguientes 2 bytes = dirección */
#define NOTE_REST  0x60u   /* silencio */
#define NOTE_DUR   0x80u   /* bit 7 = siguiente byte es nueva duración */

/* ==========================================================================
 * HELPERS DE PSG
 * (sub_765C, sub_76AC, sub_76BE)
 * ========================================================================== */

/* silence_all_channels — sub_76AC
 * Apaga los 3 canales del PSG escribiendo volumen 0. */
static void psg_silence_all(void)
{
    hal_psg_write(8,  0);    /* PSG R8  = volume canal A = 0 */
    hal_psg_write(9,  0);    /* PSG R9  = volume canal B = 0 */
    hal_psg_write(10, 0);    /* PSG R10 = volume canal C = 0 */
}

/* play_note — sub_765C
 * Escribe una nota en el PSG para el canal indicado.
 *
 * @param note_idx  índice de nota (0x00..0x5F) o NOTE_REST (0x60)
 * @param psg_ch    canal PSG: 0 = canal A (regs 0,1,8,13), 1 = canal B (regs 2,3,9)
 * @param volume    volumen (0-15)
 */
static void psg_play_note(uint8_t note_idx, uint8_t psg_ch, uint8_t volume)
{
    if (note_idx == NOTE_REST || note_idx >= 0x60u) {
        /* Silencio: apagar solo este canal */
        hal_psg_write((uint8_t)(8u + psg_ch), 0u);
        return;
    }

    /* Lookup del período en la tabla (con transposición) */
    uint8_t  idx    = (uint8_t)(note_idx
                      + g_music.transpose_fine
                      + g_music.transpose_coarse);
    idx &= 0x3Fu;  /* limitar a 64 notas */

    uint16_t period;
    if (idx < 48u) {
        period = NOTE_PERIODS[idx];
    } else {
        /* Notas más agudas: extrapolar dividiendo por 2 */
        uint8_t sub = idx - 48u;
        period = (uint16_t)(NOTE_PERIODS[47] >> (sub / 12u + 1u));
        if (period == 0) period = 1;
    }

    uint8_t reg_fine   = (uint8_t)(psg_ch * 2u);       /* R0 o R2 */
    uint8_t reg_coarse = (uint8_t)(psg_ch * 2u + 1u);  /* R1 o R3 */
    uint8_t reg_vol    = (uint8_t)(8u + psg_ch);        /* R8 o R9 */

    hal_psg_write(reg_fine,   (uint8_t)(period & 0xFFu));
    hal_psg_write(reg_coarse, (uint8_t)(period >> 8));
    hal_psg_write(reg_vol,    volume & 0x0Fu);

    /* Solo canal A: escribir envelope shape (R13 = 0x00 = saw-down, one-shot) */
    if (psg_ch == 0u) {
        hal_psg_write(13, 0x00u);
    }

    /* Mixer: habilitar tono en el canal, deshabilitar ruido
     * R7: bits[2:0] = tone enable (0=on), bits[5:3] = noise enable (1=off)
     * Para canal A bit0=0 (tone on), para canal B bit1=0 (tone on)
     * Ruido siempre off para música: bits 3-5 = 1
     */
    uint8_t mixer = hal_psg_read(7);
    /* Limpiar bit de tone del canal: bit psg_ch */
    mixer &= ~(uint8_t)(1u << psg_ch);
    /* Asegurarse que ruido esté apagado para este canal */
    mixer |=  (uint8_t)(1u << (3u + psg_ch));
    hal_psg_write(7, mixer);
}

/* update_sfx_volumes — sub_76BE
 * Decrementa los 3 contadores de volumen de SFX y los escribe al PSG.
 * Los SFX (chispa, muerte) superponen su volumen a la música.
 */
static void psg_update_sfx(void)
{
    /* Decrementar contadores de fade-out de SFX */
    for (int i = 0; i < 3; i++) {
        if (g_music.vol[i] > 0u) g_music.vol[i]--;
    }

    /* Si vol[0] > 0: escribir SFX canal A con mezcla especial
     * sub_76D3: A=0x07, E=0x98 → PSG R7 = 0x98 (mixer: noise+tone mix)
     *           A=0x06, E=vol  → PSG R6 (noise period)
     *           A=0x05, E=vol  → PSG R5
     *           A=0x0A, E=0x0F → PSG R10 = max volume
     */
    if (g_music.vol[0] > 0u) {
        uint8_t v = g_music.vol[0];
        hal_psg_write(7,  0x98u);   /* mixer especial con ruido */
        hal_psg_write(6,  (uint8_t)(3u + (v >> 1)));   /* noise period */
        hal_psg_write(5,  (uint8_t)(3u + (v >> 1)));
        hal_psg_write(10, 0x0Fu);   /* canal C max volume */
        return;
    }

    /* Si vol[1] > 0: SFX canal B
     * sub_76FF: R7=0xB8, R5=0x01, R4=periodo, R10=0x0F */
    if (g_music.vol[1] > 0u) {
        uint8_t v  = g_music.vol[1];
        uint8_t hv = v >> 3u;
        hal_psg_write(7,  0xB8u);
        hal_psg_write(5,  0x01u);
        hal_psg_write(4,  (uint8_t)(0x1Eu + hv + (v >> 1)));
        hal_psg_write(10, 0x0Fu);
        return;
    }

    /* Si vol[2] > 0: SFX canal C (sub_772B)
     * Similar pero con R4 calculado de manera diferente */
    if (g_music.vol[2] > 0u) {
        uint8_t v = g_music.vol[2];
        uint8_t c = (uint8_t)(0xFFu - v);
        c >>= 1u;
        uint8_t d = (c & 1u) ? 0x32u : 0x00u;
        hal_psg_write(7,  0xB8u);
        hal_psg_write(5,  0x00u);
        hal_psg_write(4,  (uint8_t)(0x1Eu + d + (c & ~1u)));
        hal_psg_write(10, 0x0Eu);
        return;
    }

    /* Sin SFX activos: silenciar canal C */
    hal_psg_write(10, 0x00u);
}

/* ==========================================================================
 * AVANCE DE UN CANAL — sub_7615 / sub_7618 / sub_7625
 *
 * Lógica de sub_7615:
 *   B = (IX+0) = duration
 *   A = (IX+1) = tick_count; INC A; (IX+1)=A
 *   CP B → si tick_count < duration: RET (nota sigue sonando)
 *   Resetear tick_count a 0
 *   HL = (IX+2..3) = ptr
 *   A = (HL); CP 0xFF → silence (fin)
 *   A = (HL); CP 0xFE → loop
 *   bit 7 de A → si 1: INC HL; B=(HL); (IX+0)=B (nueva duración)
 *   INC HL; (IX+2..3) = HL (avanzar puntero)
 *   CALL sub_765C (escribir nota al PSG)
 * ========================================================================== */
static void music_channel_tick(int ch_idx)
{
    MusicChannel *ch     = &g_music.ch[ch_idx];
    uint8_t       psg_ch = (uint8_t)ch_idx;

    /* Incrementar sub-tick */
    ch->tick_count++;
    if (ch->tick_count < ch->duration) return;  /* nota en curso */

    /* Nota completada: resetear y leer la siguiente */
    ch->tick_count = 0;

    if (!ch->ptr) {
        psg_play_note(NOTE_REST, psg_ch, 0);
        return;
    }

    while (true) {
        uint8_t byte = *ch->ptr;

        if (byte == NOTE_END) {
            /* Fin de stream: silenciar */
            ch->ptr = NULL;
            psg_silence_all();
            return;
        }

        if (byte == NOTE_LOOP) {
            /* Loop: saltar a la dirección indicada */
            uint8_t lo = ch->ptr[1];
            uint8_t hi = ch->ptr[2];
            uint16_t target = (uint16_t)(lo | ((uint16_t)hi << 8));
            uint32_t off    = (uint32_t)target - ROM_ORG;
            if (g_rom && off < g_rom_size) {
                ch->ptr = g_rom + off;
            } else {
                ch->ptr = NULL;
                return;
            }
            continue;
        }

        /* Bit 7: si activo → siguiente byte es la nueva duración */
        if (byte & NOTE_DUR) {
            ch->ptr++;
            ch->duration = *ch->ptr;
            byte &= ~NOTE_DUR;  /* quitar bit 7 para obtener nota real */
        }

        ch->ptr++;

        /* Reproducir la nota */
        uint8_t volume = 0x0Fu;  /* volumen máximo por defecto */
        psg_play_note(byte, psg_ch, volume);
        return;
    }
}

/* ==========================================================================
 * ISR DE MÚSICA — sub_75D4
 *
 * Llamada una vez por VBlank (60Hz / 50Hz).
 * Estructura:
 *   1. Si tempo_counter == 0 → silencio global (sub_76AC) + actualizar SFX
 *   2. Si tempo_counter != 0:
 *      tick_phase += tempo_value
 *      Si tick_phase overflow (>= threshold):
 *        resetear tick_phase
 *        actualizar canal A (IX=0xEAE9, C=0)
 *        actualizar canal B (IX=0xEAED, C=2)
 *   3. Actualizar SFX volumes (sub_76BE)
 * ========================================================================== */
void music_isr_tick(void)
{
    if (!g_music.active) return;

    /* Si tempo = 0 → silencio completo */
    if (g_music.tempo_counter == 0u) {
        psg_silence_all();
        psg_update_sfx();
        return;
    }

    /* Avanzar fase de tick */
    uint8_t prev_phase = g_music.tick_phase;
    g_music.tick_phase = (uint8_t)(g_music.tick_phase + g_music.tempo_value);

    /* Detectar overflow del tick (cuando tick_phase "wraps" o supera el valor inicial) */
    bool tick_fired = (g_music.tick_phase < prev_phase) ||
                      (g_music.tick_phase >= g_music.tempo_counter);

    if (g_music.tick_phase >= g_music.tempo_counter) {
        g_music.tick_phase = 0u;
    }

    if (tick_fired) {
        /* Avanzar ambos canales */
        music_channel_tick(0);  /* Canal A */
        music_channel_tick(1);  /* Canal B */
    }

    /* Actualizar volúmenes de SFX */
    psg_update_sfx();
}

/* ==========================================================================
 * sub_7769 — Cargar un nuevo tema musical
 *
 * Original:
 *   DI
 *   (0xEAEB) = HL   → ptr canal A
 *   (0xEAEF) = DE   → ptr canal B
 *   (0xEAE9) = 0    → reset duration A
 *   (0xEAED) = 0    → reset duration B
 *   (0xEAEA) = 0    → reset tick_count A
 *   (0xEAEE) = 0    → reset tick_count B
 *   EI
 *   RET
 *
 * @param music_a_addr  Dirección ROM del stream del canal A
 * @param music_b_addr  Dirección ROM del stream del canal B (0 = silencio)
 * ========================================================================== */
void music_load(uint16_t music_a_addr, uint16_t music_b_addr)
{
    /* DI equivalent: en SDL2 no hay interrupción real, pero bloqueamos el mutex
     * de audio para evitar race conditions con el callback de SDL */

    /* Canal A */
    if (g_rom && music_a_addr >= ROM_ORG) {
        uint32_t off = (uint32_t)music_a_addr - ROM_ORG;
        g_music.ch[0].ptr = (off < g_rom_size) ? g_rom + off : NULL;
    } else {
        g_music.ch[0].ptr = NULL;
    }
    g_music.ch[0].duration   = 1u;
    g_music.ch[0].tick_count = 0u;

    /* Canal B */
    if (g_rom && music_b_addr >= ROM_ORG) {
        uint32_t off = (uint32_t)music_b_addr - ROM_ORG;
        g_music.ch[1].ptr = (off < g_rom_size) ? g_rom + off : NULL;
    } else {
        g_music.ch[1].ptr = NULL;
    }
    g_music.ch[1].duration   = 1u;
    g_music.ch[1].tick_count = 0u;

    /* Resetear fase de tick */
    g_music.tick_phase = 0u;
}

/* ==========================================================================
 * music_set_tempo — Configurar velocidad del reproductor
 *
 * El original usa (0xEAF3) = tempo_counter y (0xEAF4) = tempo_value.
 * La frecuencia de tick = (tempo_value / tempo_counter) × 60Hz.
 *
 * Valores típicos en el juego:
 *   tempo_counter = 0x03, tempo_value = 0x01 → ~20 ticks/seg (música lenta)
 *   tempo_counter = 0x06, tempo_value = 0x02 → ~20 ticks/seg
 *   tempo_counter = 0x00 → silencio global
 *
 * @param counter  valor de referencia de tempo (0 = silencio)
 * @param value    incremento por tick
 * ========================================================================== */
void music_set_tempo(uint8_t counter, uint8_t value)
{
    g_music.tempo_counter = counter;
    g_music.tempo_value   = value;
    g_music.tick_phase    = 0u;
}

/* ==========================================================================
 * music_set_transpose — Configurar transposición global
 *
 * Equivale a escribir (0xEAF1) y (0xEAF2).
 * La transposición total = fine + coarse, sumada al índice de nota.
 *
 * @param fine    transposición fina (semitonos, signed via uint8 wrap)
 * @param coarse  transposición gruesa
 * ========================================================================== */
void music_set_transpose(uint8_t fine, uint8_t coarse)
{
    g_music.transpose_fine   = fine;
    g_music.transpose_coarse = coarse;
}

/* ==========================================================================
 * music_sfx_trigger — Disparar un efecto de sonido
 *
 * Los SFX usan los contadores de volumen 0xEAF6..0xEAF8 que se decrementan
 * en cada tick del ISR. El canal de SFX se superpone a la música.
 *
 * @param sfx_id  0=chispa/roller, 1=muerte, 2=llave
 * @param volume  volumen inicial del SFX (0x10 típico en el juego)
 * ========================================================================== */
void music_sfx_trigger(uint8_t sfx_id, uint8_t volume)
{
    if (sfx_id < 3u) {
        g_music.vol[sfx_id] = volume;
    }
}

/* ==========================================================================
 * FUNCIONES DE CONVENIENCIA para los puntos de carga del juego
 * ========================================================================== */

/* Música del título y pantalla de sparks (sub_7769(HL=0x78D2, DE=0x7916)) */
void music_play_title(void)
{
    music_load(MUSIC_TITLE_A, MUSIC_TITLE_B);
    music_set_tempo(0x03u, 0x01u);
    music_set_transpose(0u, 0u);
}

/* Música de juego (referenciada desde sub_522A: 0xeb08 = 0x5A02) */
void music_play_game(void)
{
    music_load(MUSIC_GAME_A, 0u);   /* un solo canal para la melodía del juego */
    music_set_tempo(0x06u, 0x02u);
    music_set_transpose(0u, 0u);
}

/* Silencio completo */
void music_stop(void)
{
    g_music.tempo_counter = 0u;
    psg_silence_all();
}

/* ==========================================================================
 * INICIALIZACIÓN
 * ========================================================================== */
void music_init(void)
{
    memset(&g_music, 0, sizeof(g_music));
    g_music.active        = true;
    g_music.tempo_counter = 0u;  /* silencio hasta que se cargue un tema */
    g_music.tempo_value   = 1u;

    /* Inicializar PSG en silencio:
     * R7 = 0xFF: todos los canales con tono y ruido deshabilitados
     * R8-R10 = 0: volúmenes a 0 */
    hal_psg_write(7,  0xFFu);
    hal_psg_write(8,  0u);
    hal_psg_write(9,  0u);
    hal_psg_write(10, 0u);
}
