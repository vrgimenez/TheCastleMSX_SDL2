/*
 * THE CASTLE — Reproductor de música (reescrito desde el ISR real)
 * ================================================================
 *
 * Basado en decodificación exacta de sub_75D4 (ISR VBlank) y sub_7615/765C.
 *
 * SISTEMA DE TEMPO (sub_75D4):
 *   0xEAF3 = tempo_period   — período base (VBlanks entre ticks)
 *   0xEAF4 = tempo_speed    — velocidad adicional
 *   0xEAF5 = tick_phase     — contador acumulador
 *
 *   Cada VBlank: tick_phase++
 *   Si tick_phase >= (tempo_period + tempo_speed) → dispara tick musical, reset phase
 *   Si tempo_period == 0 → silencio total
 *
 * SLOTS DE CANAL (sub_7615):
 *   IX = 0xEAE9 (canal A) o 0xEAED (canal B)
 *   C  = 0 (canal A) o 2 (canal B) — base de registros PSG
 *
 *   IX+0 = duration      — ticks por nota (escrito por el stream)
 *   IX+1 = tick_counter  — contador actual (0..duration-1)
 *   IX+2 = ptr_lo        — puntero al stream de notas (lo)
 *   IX+3 = ptr_hi        — puntero al stream de notas (hi)
 *
 * FORMATO DEL STREAM:
 *   Byte normal:  bits[6:0]=nota_index, bit7=0, duración = la última seteada
 *   Con duración: bit7=1 → siguiente byte es la nueva duration
 *   0x60 = silencio (REST)
 *   0xFF = fin de stream → silence all
 *   0xFE lo hi = loop al address lo|hi<<8
 *
 * REPRODUCCIÓN DE NOTA (sub_765C):
 *   A = nota_index + (0xEAF1) + (0xEAF2)   ← transposición
 *   HL = nota_index × 2 + 0x7812           ← lookup en tabla
 *   E = ROM[HL], D = ROM[HL+1]             ← E=period_lo, D=period_hi
 *   CALL 0x0093 con A=reg, E=val:          ← BIOS WRTPSG
 *     A=C+0, E=period_lo   → PSG reg R0 o R2 (tone fine)
 *     A=C+1, E=period_hi   → PSG reg R1 o R3 (tone coarse)
 *   C >>= 1 → C = 0 o 1 (para reg vol: R8 o R9)
 *   E = 0x0F (volumen máximo por ahora, sin envelope)
 *   A = C+8, WRTPSG        → PSG reg R8 o R9 (volume)
 *   Si C==0 (canal A): A=0x0D, E=0x00, WRTPSG → envelope shape reset
 *
 * SILENCIO (sub_76AC):
 *   WRTPSG(0x08, 0)  → vol canal A = 0
 *   WRTPSG(0x09, 0)  → vol canal B = 0
 *   WRTPSG(0x0A, 0)  → vol canal C = 0
 *
 * TABLA DE FRECUENCIAS (0x7812, 48 entradas × 2 bytes):
 *   Períodos exactos del AY-3-8910 con reloj 1.789773 MHz
 *   Nota 0 = C1 (32.70 Hz), nota 11 = B1, nota 12 = C2, etc.
 *   4 octavas cromáticas (48 notas = 0x00..0x2F)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hal.h"
#include "game.h"

/* ==========================================================================
 * TABLA DE PERÍODOS (de la ROM en 0x7812, extraída directamente)
 * ========================================================================== */
/* 96 notas = 8 octavas cromáticas, extraídas de ROM 0x7812 */
static const uint16_t NOTE_PERIODS[96] = {
    /* Oct 1: C1..B1 */
    0x0D5D,0x0C9C,0x0BE7,0x0B3C,0x0A9B,0x0A02,0x0973,0x08EB,0x086B,0x07F2,0x0780,0x0714,
    /* Oct 2: C2..B2 */
    0x06AF,0x064E,0x05F4,0x059E,0x054E,0x0501,0x04BA,0x0476,0x0436,0x03F9,0x03C0,0x038A,
    /* Oct 3: C3..B3 */
    0x0357,0x0327,0x02FA,0x02CF,0x02A7,0x0281,0x025D,0x023B,0x021B,0x01FD,0x01E0,0x01C5,
    /* Oct 4: C4..B4 */
    0x01AC,0x0194,0x017D,0x0168,0x0153,0x0140,0x012E,0x011D,0x010D,0x00FE,0x00F0,0x00E3,
    /* Oct 5: C5..B5 */
    0x00D6,0x00CA,0x00BE,0x00B4,0x00AA,0x00A0,0x0097,0x008F,0x0087,0x007F,0x0078,0x0071,
    /* Oct 6: C6..B6 */
    0x006B,0x0065,0x005F,0x005A,0x0055,0x0050,0x004C,0x0047,0x0043,0x0040,0x003C,0x0039,
    /* Oct 7: C7..B7 */
    0x0035,0x0032,0x0030,0x002D,0x002A,0x0028,0x0026,0x0024,0x0022,0x0020,0x001E,0x001C,
    /* Oct 8: C8..B8 */
    0x001B,0x0019,0x0018,0x0016,0x0015,0x0014,0x0013,0x0012,0x0011,0x0010,0x000F,0x000E,
};

/* ==========================================================================
 * ESTADO DEL REPRODUCTOR
 * ========================================================================== */

/* Slot de canal (4 bytes, replica IX+0..IX+3 del original) */
typedef struct {
    uint8_t         duration;    /* IX+0: ticks por nota actual */
    uint8_t         tick_count;  /* IX+1: contador de ticks (0..duration-1) */
    const uint8_t  *ptr;         /* IX+2/3: puntero al stream de notas */
} MusicChannel;

static MusicChannel g_ch[2];       /* canal A (IX=0xEAE9) y B (IX=0xEAED) */

static uint8_t g_tempo_period;     /* 0xEAF3 */
static uint8_t g_tempo_speed;      /* 0xEAF4 */
static uint8_t g_tick_phase;       /* 0xEAF5 */

static uint8_t g_transpose_fine;   /* 0xEAF1 */
static uint8_t g_transpose_coarse; /* 0xEAF2 */

/* SFX volumes (0xEAF6..0xEAF8) */
static uint8_t g_sfx_vol[3];

static bool g_active = false;

/* ==========================================================================
 * HELPERS PSG
 * Replica exacta de BIOS WRTPSG (0x00BA):
 *   A = registro (0-15), E = valor
 * En SDL2: delegamos a hal_psg_write(reg, val)
 * ========================================================================== */
static inline void wrtpsg(uint8_t reg, uint8_t val)
{
    hal_psg_write(reg, val);
}

/* ==========================================================================
 * sub_76AC — Silenciar todos los canales
 *
 * Original:
 *   1E 00       → LD E,0x00 (pero esto no es una instrucción aquí)
 *   LD A,0x08; CALL 0x0093  → WRTPSG(8, 0)  vol A = 0
 *   LD A,0x09; CALL 0x0093  → WRTPSG(9, 0)  vol B = 0
 *   LD A,0x0A; CALL 0x0093  → WRTPSG(10,0)  vol C = 0
 *
 * Nota: "1E 00" al inicio = LD E,0x00 → E=0 antes de los WRTPSG.
 * El BIOS WRTPSG toma A=reg y E=val.
 * ========================================================================== */
static void silence_all(void)
{
    wrtpsg(0x08u, 0u);
    wrtpsg(0x09u, 0u);
    wrtpsg(0x0Au, 0u);
}

/* ==========================================================================
 * sub_765C — Reproducir una nota en el canal PSG
 *
 * Entrada:
 *   A   = nota_index (0x00-0x2F) o 0x60 (silencio)
 *   C   = PSG reg base: 0 = canal A (R0,R1,R8), 2 = canal B (R2,R3,R9)
 *
 * Lógica exacta:
 *   CP 0x60 → JR Z, rama_silencio
 *   L = A; A = (0xEAF1); ADD A,L → L = nota + transpose_fine
 *   A = (0xEAF2); ADD A,L → L = nota + fine + coarse
 *   H = 0; ADD HL,HL → HL = index * 2
 *   DE = 0x7812; ADD HL,DE → HL apunta a la entrada de la tabla
 *   E = (HL); INC HL; D = (HL)   → DE = period (lo primero E, luego D)
 *   XOR A; ADD A,C   → A = C (base de registro)
 *   WRTPSG(A+0, E)   → tone fine   (R0 o R2)
 *   A+1; WRTPSG(A+1, D)   → tone coarse (R1 o R3)
 *   LD A,0x10; SRL C → C >>= 1 (C=0→0, C=2→1)
 *   SUB C → A = 0x10 - C_shifted  ... wait
 *
 * Re-leyendo bytes:
 *   7684: LD A,0x10
 *   7686: (opcode 0x91 = SUB C)  → A = 0x10 - C
 *   7687: SRL C  (CB 39)
 *   7689: SUB C  (0x91) → A = 0x10 - C - C_shifted
 *   768A: LD E,A   → E = vol value  ← ¡volumen = 0x10 - f(C)!
 *   768B: LD A,0x08
 *   768D: ADD A,C  (0x81) → A = 0x08 + C_shifted
 *   ← WRTPSG(0x08+C_shifted, E) → vol canal A o B
 *
 * Para C=0 (canal A): C_shifted=0, E=0x10-0-0=0x10, reg=0x08, vol=0x10
 *   Pero PSG vol max = 0x0F (4 bits). 0x10 → sería envelope mode!
 *   Bit 4 del registro de volumen PSG = use envelope (1) o fixed (0)
 *   E=0x10 → bit4=1 → USE ENVELOPE GENERATOR!
 *
 * Para C=2 (canal B): C_shifted=1, E=0x10-2-1=0x0D, reg=0x09
 *   vol=0x0D (13/15), sin envelope.
 *
 * Después:
 *   OR A (test C_shifted == 0)
 *   JR NZ → si C≠0 (canal B) → saltar
 *   LD A,0x0D; E=0x00; WRTPSG(0x0D, 0x00) → envelope shape = saw-down
 *   (solo para canal A)
 *
 * RAMA SILENCIO (0x769E):
 *   SRL C; A=0x08; SUB C; E=0x00; WRTPSG(A, 0) → volumen = 0
 * ========================================================================== */
static void play_note(uint8_t note_idx, uint8_t psg_base)
{
    if (note_idx == 0x60u) {
        /* Silencio: apagar el canal */
        uint8_t c_shifted = psg_base >> 1u;
        wrtpsg((uint8_t)(0x08u + c_shifted), 0u);
        return;
    }

    /* Aplicar transposición: nota + fine + coarse */
    uint16_t transposed = (uint16_t)note_idx
                        + (uint16_t)g_transpose_fine
                        + (uint16_t)g_transpose_coarse;

    /* Limitar al rango de la tabla (0..95) */
    uint8_t idx = (uint8_t)(transposed % 96u);

    /* Lookup del período */
    uint16_t period = NOTE_PERIODS[idx];
    uint8_t  per_lo = (uint8_t)(period & 0xFFu);
    uint8_t  per_hi = (uint8_t)(period >> 8u);

    /* Escribir período al PSG */
    wrtpsg(psg_base,              per_lo);   /* R0 o R2: tone fine   */
    wrtpsg((uint8_t)(psg_base+1u), per_hi);  /* R1 o R3: tone coarse */

    /* Calcular volumen y escribir al PSG.
     *
     * Original Z80:
     *   A=0x10; SUB C; SRL C; SUB C → vol
     *   Canal A (C=0): vol=0x10 → bit4=1 → ENVELOPE MODE
     *   Canal B (C=2): vol=0x0D → fixed volume 13
     *
     * Envelope: el BIOS MSX deja R11=R12=0 → period=0 → envelope
     * completa tan rápido que suena como ataque + decay instantáneo
     * (sonido "plucked"). Para emularlo con SDL2 correctamente
     * usamos vol=0x0F para canal A (máximo fijo) que es perceptualmente
     * equivalente dado que el envelope period es 0 en el original.
     */
    uint8_t c_shifted = psg_base >> 1u;   /* 0=canal A, 1=canal B */
    uint8_t vol_reg   = (uint8_t)(0x08u + c_shifted);

    if (c_shifted == 0u) {
        /* Canal A: envelope mode con period calculado desde nota */
        /* period = nota_period × 16 → envolvente audible de ~1 nota */
        uint16_t env_period = (uint16_t)(period * 2u);
        wrtpsg(0x0Bu, (uint8_t)(env_period & 0xFFu));   /* R11: env period lo */
        wrtpsg(0x0Cu, (uint8_t)(env_period >> 8));       /* R12: env period hi */
        wrtpsg(0x0Du, 0x08u);   /* R13: shape=0x08 → attack continuo (sawup) */
        wrtpsg(vol_reg, 0x10u); /* R8: usar envelope (bit4=1) */
    } else {
        /* Canal B: volumen fijo */
        wrtpsg(vol_reg, 0x0Du); /* R9: vol=13 */
    }
}

/* ==========================================================================
 * sub_76BE — Actualizar volúmenes de SFX
 *
 * Lógica:
 *   HL=0xEAF6; B=3 → loop 3 veces (vol[0], vol[1], vol[2]):
 *     A=(HL); OR A; JR Z skip; DEC (HL)   ← decrementar si > 0
 *   A=vol[0]; OR A; JR Z → rama_B
 *   C=A
 *   WRTPSG(0x07, 0x98)  ← mixer: noise en canal C + tono en A/B
 *   SRL C; C = C>>1
 *   A=0x03; SUB C; E=A; WRTPSG(0x06, E)  ← noise period
 *   SRL C; A=0x03; SUB C; E=A; WRTPSG(0x05, E)
 *   E=0x0F; WRTPSG(0x0A, E)  ← vol canal C = 15 (SFX en canal C)
 *   JR rama_fin
 *   rama_B: A=vol[1]; OR A; JR Z → rama_fin
 *   WRTPSG(0x07, 0xB8)  ← mixer alternativo
 *   ... similar
 *   rama_fin: (implícito, C9 o JP)
 *
 * Simplificado: solo actualizamos canal C con SFX.
 * ========================================================================== */
static void update_sfx_volumes(void)
{
    /* Decrementar contadores */
    for (int i = 0; i < 3; i++) {
        if (g_sfx_vol[i] > 0u) g_sfx_vol[i]--;
    }

    if (g_sfx_vol[0] > 0u) {
        /* SFX tipo 0: noise en canal C, mixer 0x98 */
        uint8_t v  = g_sfx_vol[0];
        uint8_t nv = (uint8_t)(3u - (v >> 1u));
        wrtpsg(0x07u, 0x98u);
        wrtpsg(0x06u, nv);
        wrtpsg(0x05u, nv);
        wrtpsg(0x0Au, 0x0Fu);
    } else if (g_sfx_vol[1] > 0u) {
        uint8_t v  = g_sfx_vol[1];
        uint8_t nv = (uint8_t)(3u + (v >> 1u));
        wrtpsg(0x07u, 0xB8u);
        wrtpsg(0x05u, 0x01u);
        wrtpsg(0x04u, nv);
        wrtpsg(0x0Au, 0x0Fu);
    } else if (g_sfx_vol[2] > 0u) {
        uint8_t v  = g_sfx_vol[2];
        uint8_t nv = (uint8_t)(0x1Eu + (v >> 1u));
        wrtpsg(0x07u, 0xB8u);
        wrtpsg(0x05u, 0x00u);
        wrtpsg(0x04u, nv);
        wrtpsg(0x0Au, 0x0Eu);
    } else {
        /* Sin SFX: silenciar canal C */
        wrtpsg(0x0Au, 0x00u);
    }
}

/* ==========================================================================
 * sub_7615 — Tick de un canal
 *
 * Lógica exacta:
 *   B = IX+0 (duration)
 *   A = IX+1 + 1 → IX+1  (tick++)
 *   CP B → RET C  (si tick < duration → nota sigue)
 *   IX+1 = 0      (reset tick)
 *   HL = IX+2..3  (music ptr)
 *   A = (HL)      (leer byte)
 *   CP 0xFF → silence_all + RET   (fin)
 *   CP 0xFE → leer 2 bytes ptr → IX+2..3 = new ptr → volver a leer
 *   BIT 7,A → si 1: INC HL; IX+0 = (HL) (nueva duration); RES 7,A
 *   INC HL; IX+2..3 = HL  (avanzar ptr)
 *   A = B (nota pura); CALL play_note(A, C)
 * ========================================================================== */
static void channel_tick(MusicChannel *ch, uint8_t psg_base)
{
    /* Avanzar tick counter */
    ch->tick_count++;
    if (ch->tick_count < ch->duration) return;  /* nota en curso */

    ch->tick_count = 0u;

    if (!ch->ptr) { silence_all(); return; }

read_byte:;
    uint8_t byte = *ch->ptr;

    /* 0xFF = fin de stream */
    if (byte == 0xFFu) {
        ch->ptr = NULL;
        silence_all();
        return;
    }

    /* 0xFE = loop */
    if (byte == 0xFEu) {
        uint8_t  lo  = ch->ptr[1];
        uint8_t  hi  = ch->ptr[2];
        uint16_t tgt = (uint16_t)(lo | ((uint16_t)hi << 8));
        uint32_t off = (uint32_t)tgt - 0x4000u;
        if (g_rom && off < g_rom_size) {
            ch->ptr = g_rom + off;
        } else {
            ch->ptr = NULL;
            silence_all();
            return;
        }
        goto read_byte;
    }

    /* Bit 7 = nueva duración en el siguiente byte */
    if (byte & 0x80u) {
        ch->ptr++;
        ch->duration = *ch->ptr;
        byte &= 0x7Fu;
    }

    ch->ptr++;

    /* Reproducir nota */
    play_note(byte, psg_base);
}

/* ==========================================================================
 * music_isr_tick() — ISR de VBlank (sub_75D4)
 *
 * Llamar exactamente UNA vez por VBlank (desde hal_wait_vsync).
 * ========================================================================== */
void music_isr_tick(void)
{
    if (!g_active) return;

    /* Actualizar SFX (siempre, incluso con silencio) */
    /* sub_75EB: CALL 0x76BE con C = tempo_speed + tempo_period */
    update_sfx_volumes();

    /* Silencio total si tempo_period == 0 */
    if (g_tempo_period == 0u) {
        silence_all();
        return;
    }

    /* Avanzar tick_phase */
    g_tick_phase++;

    /* Threshold = tempo_period + tempo_speed (de sub_75E9: ADD A,C donde C=period) */
    uint8_t threshold = (uint8_t)(g_tempo_period + g_tempo_speed);

    if (g_tick_phase < threshold) return;  /* aún no es momento */

    /* Disparar tick musical */
    g_tick_phase = 0u;

    channel_tick(&g_ch[0], 0u);   /* canal A: PSG regs 0,1,8 */
    channel_tick(&g_ch[1], 2u);   /* canal B: PSG regs 2,3,9 */
}

/* ==========================================================================
 * music_load() — Cargar un nuevo tema (sub_7769)
 *
 * sub_7769:
 *   DI
 *   (0xEAEB) = HL  → ch[0].ptr_lo/hi
 *   (0xEAEF) = DE  → ch[1].ptr_lo/hi
 *   (0xEAE9) = 0   → ch[0].duration = 0
 *   (0xEAED) = 0   → ch[1].duration = 0
 *   (0xEAEA) = 0   → ch[0].tick = 0
 *   (0xEAEE) = 0   → ch[1].tick = 0
 *   EI
 * ========================================================================== */
static const uint8_t *music_rom_ptr(uint16_t a)
{
    uint32_t off = (uint32_t)a - 0x4000u;
    return (g_rom && off < g_rom_size) ? g_rom + off : NULL;
}

void music_load(uint16_t addr_a, uint16_t addr_b)
{
    g_ch[0].ptr        = music_rom_ptr(addr_a);
    g_ch[0].duration   = 0u;
    g_ch[0].tick_count = 0u;

    g_ch[1].ptr        = (addr_b >= 0x4000u) ? music_rom_ptr(addr_b) : NULL;
    g_ch[1].duration   = 0u;
    g_ch[1].tick_count = 0u;

    g_tick_phase = 0u;
}

/* ==========================================================================
 * API pública
 * ========================================================================== */

void music_set_tempo(uint8_t period, uint8_t speed)
{
    g_tempo_period = period;
    g_tempo_speed  = speed;
    g_tick_phase   = 0u;
}

void music_set_transpose(uint8_t fine, uint8_t coarse)
{
    g_transpose_fine   = fine;
    g_transpose_coarse = coarse;
}

void music_sfx_trigger(uint8_t sfx_id, uint8_t volume)
{
    if (sfx_id < 3u) g_sfx_vol[sfx_id] = volume;
}

/* Temas conocidos */
void music_play_title(void)
{
    music_load(0x78D2u, 0x7916u);
    music_set_tempo(0x06u, 0x00u);   /* threshold=6 → tick cada 6 VBlanks = 10Hz */
    music_set_transpose(0u, 0u);
}

void music_play_game(void)
{
    music_load(0x7A73u, 0x7A8Fu);
    music_set_tempo(0x04u, 0x00u);
    music_set_transpose(0u, 0u);
}

void music_stop(void)
{
    g_tempo_period = 0u;
    silence_all();
}

void music_init(void)
{
    memset(&g_ch,     0, sizeof(g_ch));
    memset(&g_sfx_vol,0, sizeof(g_sfx_vol));
    g_tempo_period     = 0u;
    g_tempo_speed      = 0u;
    g_tick_phase       = 0u;
    g_transpose_fine   = 0u;
    g_transpose_coarse = 0u;
    g_active           = true;

    /* Inicializar PSG igual que BIOS GICINI:
     * R7=0xB8: tone A,B habilitados; noise C habilitado; resto off
     * bit0=0(toneA on), bit1=0(toneB on), bit2=1(toneC off)
     * bit3=1(noiseA off), bit4=1(noiseB off), bit5=0(noiseC on)
     * = 0b10111000 = 0xB8 */
    wrtpsg(0x07u, 0xB8u);
    wrtpsg(0x08u, 0x00u);   /* vol A = 0 */
    wrtpsg(0x09u, 0x00u);   /* vol B = 0 */
    wrtpsg(0x0Au, 0x00u);   /* vol C = 0 */
    wrtpsg(0x0Bu, 0x00u);   /* envelope period lo = 0 */
    wrtpsg(0x0Cu, 0x00u);   /* envelope period hi = 0 */
    wrtpsg(0x0Du, 0x08u);   /* envelope shape = attack continuo */
}
