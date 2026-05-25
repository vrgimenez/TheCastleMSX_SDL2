/*
 * THE CASTLE — SDL2 Hardware Abstraction Layer
 * ============================================
 * Emula el hardware del MSX1 que usa el juego:
 *
 *   VDP  TMS9918A  →  SDL2 texture en modo Screen 2 (256×192)
 *   PSG  AY-3-8910 →  SDL2 audio callback (síntesis por software)
 *   Input joystick →  teclado SDL2 (cursores + Z/X/SPACE)
 *   Vsync          →  SDL_Delay sincronizado a 60 Hz (NTSC) o 50 Hz (PAL)
 *
 * Compile:
 *   gcc -std=c99 -Wall -O2 hal_sdl2.c the_castle.c \
 *       $(sdl2-config --cflags --libs) -lm -o the_castle
 */

#include "hal.h"
#include "game.h"
#include "screen.h"

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ==========================================================================
 * CONSTANTES
 * ========================================================================== */

/* Resolución nativa MSX Screen 2: 256 × 192 pixels */
#define MSX_W   256
#define MSX_H   192

/* Factor de escala de ventana (256×192 → 768×576) */
#define SCALE   3

/* Tamaño de VRAM TMS9918A */
#define VRAM_SIZE  0x4000u   /* 16 KB */

/* Frecuencia de audio */
#define AUDIO_FREQ     44100
#define AUDIO_SAMPLES  512    /* tamaño del buffer de audio */
#define PSG_CHANNELS   3      /* A, B, C */

/* Frecuencia de reloj del AY-3-8910 en MSX: 1.789773 MHz (NTSC) */
#define PSG_CLOCK      1789773.0

/* TMS9918A: 16 colores */
static const uint8_t TMS_PALETTE[16][3] = {
    {   0,   0,   0 }, /* 0: transparent / black  */
    {   0,   0,   0 }, /* 1: black                */
    {  33, 200,  66 }, /* 2: medium green          */
    {  94, 220, 120 }, /* 3: light green           */
    {  84,  85, 237 }, /* 4: dark blue             */
    { 125, 118, 252 }, /* 5: light blue            */
    { 212,  82,  77 }, /* 6: dark red              */
    {  66, 235, 245 }, /* 7: cyan                  */
    { 252,  85,  84 }, /* 8: medium red            */
    { 255, 121, 120 }, /* 9: light red             */
    { 212, 193,  84 }, /* A: dark yellow           */
    { 230, 206, 128 }, /* B: light yellow          */
    {  33, 176,  59 }, /* C: dark green            */
    { 201,  91, 186 }, /* D: magenta               */
    { 204, 204, 204 }, /* E: grey                  */
    { 255, 255, 255 }, /* F: white                 */
};

/* ==========================================================================
 * ESTADO INTERNO
 * ========================================================================== */

/* --- VDP --- */
static uint8_t  vram[VRAM_SIZE];          /* VRAM completa                   */
static uint8_t  vdp_reg[8];               /* Registros VDP R0..R7            */
static uint32_t framebuf[MSX_W * MSX_H];  /* Framebuffer RGBA                */

/* Direcciones de tablas (calculadas desde los registros VDP) */
static uint16_t vdp_name_base;    /* name table (0x1800)       */
static uint16_t vdp_color_base;   /* color table (0x2000)      */
static uint16_t vdp_pat_base;     /* pattern table (0x0000)    */
static uint16_t vdp_spr_attr;     /* sprite attr (0x1B00)      */
static uint16_t vdp_spr_pat;      /* sprite pattern (0x3800)   */

/* --- SDL --- */
static SDL_Window   *window   = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture  *texture  = NULL;  /* MSX_W × MSX_H, RGBA8888 */

/* --- PSG AY-3-8910 --- */
typedef struct {
    uint16_t tone_period;   /* registros R0/R1, R2/R3, R4/R5 */
    uint8_t  volume;        /* volumen efectivo (0-15) */
    bool     use_env;       /* true → usa envelope generator */
    uint32_t phase;         /* fase del oscilador (acumulador) */
} PsgChannel;

/* Envelope generator state */
static uint16_t psg_env_period;   /* R11 | (R12 << 8) */
static uint8_t  psg_env_shape;    /* R13: 0x08=attack, 0x00=sawdown, etc. */
static uint32_t psg_env_phase;    /* fase del envelope (0..65535) */
static uint8_t  psg_env_vol;      /* volumen actual del envelope (0-15) */

static PsgChannel psg_ch[PSG_CHANNELS];
static uint8_t    psg_noise_period;  /* R6  */
static uint8_t    psg_mixer;         /* R7  */
static uint8_t    psg_regs[16];      /* shadow de todos los registros PSG */
static uint32_t   psg_noise_state = 1;
static uint32_t   psg_noise_phase = 0;

static SDL_AudioDeviceID audio_dev;
static SDL_mutex        *audio_mutex = NULL;

/* --- Input --- */
static uint8_t joy_state[2]; /* estado actual de los dos puertos */

/* --- Timing --- */
static uint64_t frame_start_ticks;
static uint32_t frame_period_ms; /* 16 ms para 60Hz, 20 ms para 50Hz */

/* ==========================================================================
 * FORWARD DECLARATIONS INTERNAS
 * ========================================================================== */
static void vdp_render(void);
static void vdp_render_sprites(void);
static void psg_audio_callback(void *userdata, uint8_t *stream, int len);
static void update_vdp_addresses(void);

/* ==========================================================================
 * INICIALIZACIÓN Y CIERRE
 * ========================================================================== */

bool hal_init(bool pal_timing)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    /* Ventana escalada × SCALE */
    window = SDL_CreateWindow(
        "The Castle (ASCII 1986) — MSX Port",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        MSX_W * SCALE, MSX_H * SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        /* fallback software renderer */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }

    /* Mantiene aspecto aunque se redimensione la ventana */
    SDL_RenderSetLogicalSize(renderer, MSX_W, MSX_H);

    /* Textura RGBA para el framebuffer MSX */
    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        MSX_W, MSX_H);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return false;
    }
    {
        Uint32 fmt;
        SDL_QueryTexture(texture, &fmt, NULL, NULL, NULL);
        SDL_PixelFormat *pf = SDL_AllocFormat(fmt);
        if (pf) {
            for (int i = 0; i < 16; i++)
                g_palette[i] = SDL_MapRGB(pf,
                    TMS_PALETTE[i][0], TMS_PALETTE[i][1], TMS_PALETTE[i][2]);
            SDL_FreeFormat(pf);
        }
    }

    /* Escalado pixelado (sin blur) */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* Audio */
    audio_mutex = SDL_CreateMutex();

    SDL_AudioSpec want = {0}, got = {0};
    want.freq     = AUDIO_FREQ;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = AUDIO_SAMPLES;
    want.callback = psg_audio_callback;
    want.userdata = NULL;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s (audio deshabilitado)\n",
                SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_dev, 0); /* play */
    }

    /* Timing */
    frame_period_ms = pal_timing ? 20 : 16; /* 50 Hz / 60 Hz */
    frame_start_ticks = SDL_GetTicks64();

    /* Estado inicial VDP */
    memset(vram,    0, sizeof(vram));
    memset(vdp_reg, 0, sizeof(vdp_reg));
    memset(framebuf, 0, sizeof(framebuf));

    /* Valores por defecto de Screen 2 (MSX BIOS INITXT + INIT32) */
    vdp_reg[0] = 0x00;  /* modo 0 */
    vdp_reg[1] = 0xC0;  /* 16×16 sprites, pantalla activa */
    vdp_reg[2] = 0x06;  /* name table  @ 0x1800 (0x06 << 10) */
    vdp_reg[3] = 0xFF;  /* color table @ 0x2000 (en modo Gr2, FF) */
    vdp_reg[4] = 0x03;  /* pattern     @ 0x0000 (0x03 << 11 → pero en Gr2 0x0000) */
    vdp_reg[5] = 0x36;  /* sprite attr @ 0x1B00 (0x36 << 7) */
    vdp_reg[6] = 0x07;  /* sprite pat  @ 0x3800 (0x07 << 11) */
    vdp_reg[7] = 0x0F;  /* fondo negro, borde negro */

    update_vdp_addresses();

    /* PSG */
    memset(psg_ch,   0, sizeof(psg_ch));
    memset(psg_regs, 0, sizeof(psg_regs));
    psg_mixer      = 0xFFu; /* todo silenciado */
    psg_env_period = 0u;
    psg_env_shape  = 0u;
    psg_env_phase  = 0u;
    psg_env_vol    = 0u;

    return true;
}

void hal_quit(void)
{
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    if (audio_mutex) SDL_DestroyMutex(audio_mutex);
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    SDL_Quit();
}

/* Flag persistente de quit — una vez activado, nunca se desactiva */
static bool g_quit_requested = false;

/* ==========================================================================
 * CONSULTA DE ESTADO — sin consumir eventos
 * ========================================================================== */
bool hal_is_running(void)
{
    return !g_quit_requested;
}

/* ==========================================================================
 * PUMP DE EVENTOS — llamar una vez por frame
 * Retorna false si el usuario cerró la ventana.
 * ========================================================================== */
bool hal_poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) g_quit_requested = true;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            g_quit_requested = true;
    }
    if (g_quit_requested) return false;

    /* Actualizar estado de joystick desde el teclado:
     *
     * Puerto 1 (jugador):   Cursores = dirección, Z = fire1, X = fire2
     * Puerto 2 (no usado):  sin mapear
     *
     * Formato de retorno de BIOS GTSTCK:
     *   0=nada, 1=arriba, 2=arriba-der, 3=der, 4=abajo-der,
     *   5=abajo, 6=abajo-izq, 7=izq, 8=arriba-izq
     * fire: bits separados (BIOS GTTRIG)
     */
    const uint8_t *keys = SDL_GetKeyboardState(NULL);

    bool up    = keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W];
    bool down  = keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S];
    bool left  = keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A];
    bool right = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D];
    bool fire  = keys[SDL_SCANCODE_Z]     || keys[SDL_SCANCODE_SPACE]
              || keys[SDL_SCANCODE_LCTRL];

    uint8_t dir = 0;
    if (up    && !left && !right) dir = 1;
    if (up    &&  right)          dir = 2;
    if (right && !up   && !down)  dir = 3;
    if (down  &&  right)          dir = 4;
    if (down  && !left && !right) dir = 5;
    if (down  &&  left)           dir = 6;
    if (left  && !up   && !down)  dir = 7;
    if (up    &&  left)           dir = 8;

    /* bit 0 = fire1, bits[4:1] = no usados aquí */
    joy_state[0] = dir | (fire ? 0x10 : 0x00);
    joy_state[1] = 0;

    return true;
}

/* ==========================================================================
 * VDP — REGISTROS
 * ========================================================================== */

/* Recalcula las direcciones base de las tablas VDP desde los registros */
static void update_vdp_addresses(void)
{
    /* Screen 2 (Graphics II):
     *   Name table   = R2[3:1] × 0x400   (normalmente 0x1800)
     *   Color table  = siempre  0x2000   en Screen 2
     *   Pattern      = siempre  0x0000   en Screen 2
     *   Sprite attr  = R5[6:1] × 0x80
     *   Sprite pat   = R6[2:0] × 0x800
     */
    vdp_name_base  = (uint16_t)((vdp_reg[2] & 0x0E) << 10);
    vdp_color_base = 0x2000u;   /* fijo en modo Screen 2 */
    vdp_pat_base   = 0x0000u;   /* fijo en modo Screen 2 */
    vdp_spr_attr   = (uint16_t)((vdp_reg[5] & 0x7E) << 7);
    vdp_spr_pat    = (uint16_t)((vdp_reg[6] & 0x07) << 11);
}

void hal_vdp_write_reg(uint8_t reg, uint8_t val)
{
    if (reg >= 8) return;
    vdp_reg[reg] = val;
    if (reg == 7) screen_set_border(val & 0x0Fu);
    update_vdp_addresses();
}

/* ==========================================================================
 * VDP — ACCESO A VRAM
 * ========================================================================== */

void hal_vdp_write_vram(uint16_t addr, uint8_t val)
{
    /* Pattern table (0x0000-0x17FF) → g_bg_tiles, ignore third */
    if (addr < 0x1800u) {
        uint16_t off = addr % 0x0800u;
        uint8_t  idx = (uint8_t)(off / 8u);
        uint8_t  row = (uint8_t)(off % 8u);
        g_bg_tiles[idx][row * 2u] = val;
        return;
    }
    /* Name table (0x1800-0x1AFF) → g_screen_buf */
    if (addr >= 0x1800u && addr < 0x1B00u) {
        uint16_t off = addr - 0x1800u;
        screen_put((uint8_t)(off % 32u), (uint8_t)(off / 32u), val);
        return;
    }
    /* Color table (0x2000-0x37FF) → g_bg_tiles, ignore third */
    if (addr >= 0x2000u && addr < 0x3800u) {
        uint16_t off = (addr - 0x2000u) % 0x0800u;
        uint8_t  idx = (uint8_t)(off / 8u);
        uint8_t  row = (uint8_t)(off % 8u);
        g_bg_tiles[idx][row * 2u + 1u] = val;
        return;
    }
    /* Everything else (sprites, etc.) → vram */
    vram[addr & (VRAM_SIZE - 1u)] = val;
}

uint8_t hal_vdp_read_vram(uint16_t addr)
{
    /* Pattern table → g_bg_tiles */
    if (addr < 0x1800u) {
        uint16_t off = addr % 0x0800u;
        uint8_t  idx = (uint8_t)(off / 8u);
        uint8_t  row = (uint8_t)(off % 8u);
        return g_bg_tiles[idx][row * 2u];
    }
    /* Name table → g_screen_buf */
    if (addr >= 0x1800u && addr < 0x1B00u) {
        uint16_t off = addr - 0x1800u;
        return g_screen_buf[off / 32u][off % 32u];
    }
    /* Color table → g_bg_tiles */
    if (addr >= 0x2000u && addr < 0x3800u) {
        uint16_t off = (addr - 0x2000u) % 0x0800u;
        uint8_t  idx = (uint8_t)(off / 8u);
        uint8_t  row = (uint8_t)(off % 8u);
        return g_bg_tiles[idx][row * 2u + 1u];
        return 0;
    }
    return vram[addr & (VRAM_SIZE - 1u)];
}

void hal_vdp_fill_vram(uint16_t addr, uint8_t val, uint16_t count)
{
    while (count--) {
        hal_vdp_write_vram(addr, val);
        addr++;
    }
}

void hal_vdp_copy_to_vram(uint16_t dst, const uint8_t *src, uint16_t count)
{
    while (count--) {
        hal_vdp_write_vram(dst, *src++);
        dst++;
    }
}

void hal_vdp_copy_from_vram(uint16_t src, uint8_t *dst, uint16_t count)
{
    while (count--) {
        *dst++ = hal_vdp_read_vram(src);
        src++;
    }
}

/* ==========================================================================
 * VDP — MODOS DE PANTALLA
 * ========================================================================== */

void hal_vdp_init_screen2(void)
{
    /* Screen 2 (Graphics II): 256×192, 16 colores por fila de 8 pixels */
    vdp_reg[0] = 0x02;  /* M3=1 → Graphics II */
    vdp_reg[1] = 0xC0;  /* IE=1 (int vsync), M1=0, M2=0, sprites 16×16 */
    vdp_reg[2] = 0x06;  /* name table 0x1800  */
    vdp_reg[3] = 0xFF;  /* color table 0x2000 (en Gr2 = 0xFF×64) */
    vdp_reg[4] = 0x03;  /* pattern 0x0000 (en Gr2) */
    vdp_reg[5] = 0x36;  /* sprite attr 0x1B00 */
    vdp_reg[6] = 0x07;  /* sprite pat 0x3800 */
    vdp_reg[7] = 0x01;  /* borde negro */
    update_vdp_addresses();
}

void hal_vdp_disable_screen(void)
{
    /* apagar el bit de pantalla activa (bit 6 de R1) */
    vdp_reg[1] &= ~0x40u;
}

void hal_vdp_clear_sprites(void)
{
    /* Poner el sprite Y de todos los slots a 0xD0 (= sprite terminador en TMS9918) */
    for (int i = 0; i < 32; i++) {
        vram[vdp_spr_attr + (uint16_t)(i * 4)] = 0xD0;
    }
}

/* ==========================================================================
 * VDP — RENDERIZADO
 *
 * Ya no se usa VRAM para pattern/color table. El renderizado lee:
 *   - g_screen_buf[32×24] para los índices de tiles
 *   - g_bg_tiles[256][16] para los datos de cada tile (formato intercalado)
 *   - vram[] solo para sprites (attr + pattern)
 * ========================================================================== */

static void vdp_render(void)
{
    /* Format: 0xAABBGGRR for SDL_RGBA8888 on LE */
    uint32_t border_rgba = g_palette[0];
    if ((vdp_reg[7] & 0x0Fu)) {
        border_rgba = g_palette[vdp_reg[7] & 0x0Fu];
    }

    /* Screen off: just border */
    if (!(vdp_reg[1] & 0x40u)) {
        for (int i = 0; i < MSX_W * MSX_H; i++)
            framebuf[i] = border_rgba;
        return;
    }

    /* Render background from flat tile arrays */
    screen_render(framebuf, MSX_W, MSX_H);

    /* Render sprites on top (still reads from vram[]) */
    vdp_render_sprites();
}

static void vdp_render_sprites(void)
{
    /* TMS9918A: sprites 16×16 si bit 1 de R1 está activo, sino 8×8.
     * Magnificación (MAG = bit 0 de R1) dobla el tamaño. */
    bool size16 = (vdp_reg[1] & 0x02u) != 0;
    bool mag    = (vdp_reg[1] & 0x01u) != 0;

    /* Contador de sprites por scanline (máx 4 visibles en MSX) */
    uint8_t sprites_on_line[MSX_H];
    memset(sprites_on_line, 0, sizeof(sprites_on_line));

    for (int s = 0; s < 32; s++) {
        uint16_t attr_addr = (uint16_t)(vdp_spr_attr + (uint16_t)s * 4u);
        uint8_t  y_raw     = vram[attr_addr + 0];
        uint8_t  x_raw     = vram[attr_addr + 1];
        uint8_t  pat_num   = vram[attr_addr + 2];
        uint8_t  color_ec  = vram[attr_addr + 3];

        /* Y=0xD0 = fin de lista de sprites */
        if (y_raw == 0xD0) break;

        /* Y en TMS9918 está desplazado: posición real = Y+1 */
        int sy = (int)(uint8_t)(y_raw + 1);
        int sx = (int)x_raw;

        /* Early clock bit: si bit7 del color está activo, X -= 32 */
        if (color_ec & 0x80u) sx -= 32;

        uint8_t color_idx = color_ec & 0x0Fu;
        if (color_idx == 0) continue; /* color 0 = transparente */

        uint32_t rgba = g_palette[color_idx];

        /* En sprites 16×16 el número de patrón ignora los 2 bits bajos */
        if (size16) pat_num &= 0xFCu;
        uint16_t pat_base = (uint16_t)(vdp_spr_pat + (uint16_t)pat_num * 8u);

        int pat_rows = size16 ? 16 : 8;

        for (int row = 0; row < pat_rows; row++) {
            int py = sy + row * (mag ? 2 : 1);
            if (py < 0 || py >= MSX_H) continue;

            /* Límite de 4 sprites por scanline */
            if (sprites_on_line[py] >= 4) continue;
            sprites_on_line[py]++;

            /* Los dos bytes del patrón (izquierda + derecha para 16px ancho) */
            int bytes_wide = size16 ? 2 : 1;
            for (int bw = 0; bw < bytes_wide; bw++) {
                /* En sprites 16×16: bytes 0-15 = mitad izq, 16-31 = mitad der */
                uint16_t paddr = pat_base + (uint16_t)(bw * 16 + row);
                uint8_t  pbyte = vram[paddr];

                for (int bit = 7; bit >= 0; bit--) {
                    if (!(pbyte & (1u << bit))) continue; /* pixel transparente */

                    int px = sx + bw * 8 * (mag ? 2 : 1) + (7 - bit) * (mag ? 2 : 1);
                    if (px < 0 || px >= MSX_W) continue;

                    framebuf[py * MSX_W + px] = rgba;
                    if (mag) {
                        /* duplicar pixel para magnificación */
                        if (px + 1 < MSX_W)
                            framebuf[py * MSX_W + px + 1] = rgba;
                        if (py + 1 < MSX_H) {
                            framebuf[(py+1) * MSX_W + px] = rgba;
                            if (px + 1 < MSX_W)
                                framebuf[(py+1) * MSX_W + px + 1] = rgba;
                        }
                    }
                }
            }
        }
    }
}

/* ==========================================================================
 * VDP — PRESENTACIÓN (llamar una vez por frame)
 * ========================================================================== */

void hal_vdp_present(void)
{
    vdp_render();

    /* Subir framebuffer a la textura SDL */
    SDL_UpdateTexture(texture, NULL, framebuf, MSX_W * (int)sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* ==========================================================================
 * PSG AY-3-8910 — SÍNTESIS DE AUDIO
 *
 * El AY-3-8910 tiene 3 canales de tono + 1 de ruido + envolvente.
 * Síntesis simplificada: onda cuadrada por canal, mezclada a S16.
 * ========================================================================== */

void hal_psg_write(uint8_t reg, uint8_t val)
{
    if (reg >= 16) return;

    SDL_LockMutex(audio_mutex);
    psg_regs[reg] = val;

    switch (reg) {
        /* Tone period: canal A (R0=fine, R1=coarse) */
        case 0: case 1:
            psg_ch[0].tone_period = (uint16_t)(psg_regs[0] | ((psg_regs[1] & 0x0F) << 8));
            break;
        /* Tone period: canal B (R2=fine, R3=coarse) */
        case 2: case 3:
            psg_ch[1].tone_period = (uint16_t)(psg_regs[2] | ((psg_regs[3] & 0x0F) << 8));
            break;
        /* Tone period: canal C (R4=fine, R5=coarse) */
        case 4: case 5:
            psg_ch[2].tone_period = (uint16_t)(psg_regs[4] | ((psg_regs[5] & 0x0F) << 8));
            break;
        /* Noise period (R6) */
        case 6:
            psg_noise_period = val & 0x1Fu;
            break;
        /* Mixer (R7): bits[2:0]=tone enable (invertido), bits[5:3]=noise enable */
        case 7:
            psg_mixer = val;
            break;
        /* Volúmenes (R8, R9, R10) — bits[3:0], bit4=env (no implementado) */
        case  8:
            /* bit4=1: envelope mode → use max volume; bit4=0: fixed volume */
            psg_ch[0].volume    = (val & 0x10u) ? 0x0Fu : (val & 0x0Fu);
            psg_ch[0].use_env   = (val & 0x10u) != 0u;
            break;
        case  9:
            psg_ch[1].volume    = (val & 0x10u) ? 0x0Fu : (val & 0x0Fu);
            psg_ch[1].use_env   = (val & 0x10u) != 0u;
            break;
        case 10:
            psg_ch[2].volume    = (val & 0x10u) ? 0x0Fu : (val & 0x0Fu);
            psg_ch[2].use_env   = (val & 0x10u) != 0u;
            break;
        case 11:
            psg_env_period = (uint16_t)((psg_regs[11]) | ((uint16_t)psg_regs[12] << 8));
            psg_env_phase  = 0u;
            break;
        case 12:
            psg_env_period = (uint16_t)((psg_regs[11]) | ((uint16_t)psg_regs[12] << 8));
            psg_env_phase  = 0u;
            break;
        case 13:
            psg_env_shape  = val;
            psg_env_phase  = 0u;
            psg_env_vol    = (val & 0x04u) ? 0u : 0x0Fu; /* attack starts at 0 or 15 */
            break;
        default: break;
    }

    SDL_UnlockMutex(audio_mutex);
}

uint8_t hal_psg_read(uint8_t reg)
{
    if (reg >= 16) return 0xFF;
    return psg_regs[reg];
}

/*
 * Callback de audio SDL — se llama desde el hilo de audio.
 * Genera AUDIO_SAMPLES samples S16 mezclando los 3 canales.
 */
static void psg_audio_callback(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    int16_t *out   = (int16_t *)stream;
    int      nsamples = len / 2;

    SDL_LockMutex(audio_mutex);

    /* Tabla de volúmenes AY: escala logarítmica estándar */
    static const uint16_t vol_table[16] = {
        0, 340, 480, 680, 960, 1360, 1920, 2720,
        3840, 5440, 7680, 10880, 15360, 21760, 30720, 43520
    };

    /* Incremento de fase por sample para cada canal:
     * freq_hz = PSG_CLOCK / (16 × tone_period)
     * phase_inc = freq_hz / AUDIO_FREQ × 0x10000 (fixed-point 16:16)
     */
    uint32_t phase_inc[PSG_CHANNELS];
    for (int c = 0; c < PSG_CHANNELS; c++) {
        uint16_t tp = psg_ch[c].tone_period;
        if (tp == 0) tp = 1;
        double freq = PSG_CLOCK / (16.0 * tp);
        phase_inc[c] = (uint32_t)(freq / AUDIO_FREQ * 65536.0);
    }

    /* Ruido: LFSR de 17 bits */
    uint32_t noise_inc = psg_noise_period ? (uint32_t)(
        (PSG_CLOCK / (16.0 * psg_noise_period)) / AUDIO_FREQ * 65536.0) : 0;

    /* Incremento de envelope */
    uint32_t env_inc = 0u;
    if (psg_env_period > 0u) {
        double env_freq = PSG_CLOCK / (256.0 * psg_env_period);
        env_inc = (uint32_t)(env_freq / AUDIO_FREQ * 65536.0);
    }

    for (int i = 0; i < nsamples; i++) {
        int32_t mixed = 0;

        /* Actualizar envelope generator */
        if (env_inc > 0u) {
            uint32_t prev_ep = psg_env_phase;
            psg_env_phase += env_inc;
            if (psg_env_phase < prev_ep) {
                /* overflow: un ciclo de envelope completo */
                bool attack  = (psg_env_shape & 0x04u) != 0u;
                bool alternate = (psg_env_shape & 0x02u) != 0u;
                bool hold    = (psg_env_shape & 0x01u) != 0u;
                if (hold) {
                    psg_env_vol = attack ? 0x0Fu : 0x00u;
                } else if (alternate) {
                    /* invertir dirección */
                    psg_env_shape ^= 0x04u;
                }
            }
            /* Volumen del envelope en este sample */
            bool env_attack = (psg_env_shape & 0x04u) != 0u;
            uint8_t ep_vol = (uint8_t)((psg_env_phase >> 12) & 0x0Fu);
            psg_env_vol = env_attack ? ep_vol : (uint8_t)(0x0Fu - ep_vol);
        }

        /* Actualizar LFSR de ruido */
        psg_noise_phase += noise_inc;
        if (psg_noise_phase >= 0x10000u) {
            psg_noise_phase -= 0x10000u;
            /* LFSR de 17 bits: polinomio x^17 + x^14 + 1 */
            uint32_t bit = ((psg_noise_state >> 16) ^ (psg_noise_state >> 13)) & 1u;
            psg_noise_state = (psg_noise_state << 1) | bit;
        }
        bool noise_out = (psg_noise_state >> 16) & 1u;

        for (int c = 0; c < PSG_CHANNELS; c++) {
            /* Avanzar fase del oscilador */
            psg_ch[c].phase += phase_inc[c];
            bool tone_out = (psg_ch[c].phase >> 16) & 1u;

            /* Mixer: bit c = tone enable (0=activo), bit c+3 = noise enable */
            bool tone_en  = !((psg_mixer >> c)       & 1u);
            bool noise_en = !((psg_mixer >> (c + 3)) & 1u);

            bool output = (tone_en  ? tone_out  : true)
                        & (noise_en ? noise_out : true);

            uint8_t  eff_vol = psg_ch[c].use_env ? psg_env_vol : psg_ch[c].volume;
            uint16_t vol     = vol_table[eff_vol & 0x0Fu];
            mixed += output ? (int32_t)vol : -(int32_t)vol;
        }

        /* Clamp a S16 */
        if (mixed >  32767) mixed =  32767;
        if (mixed < -32768) mixed = -32768;
        out[i] = (int16_t)mixed;
    }

    SDL_UnlockMutex(audio_mutex);
}

/* ==========================================================================
 * INPUT
 * ========================================================================== */

/*
 * Emula BIOS GTSTCK(port):
 *   port 0 = joystick 1, port 1 = joystick 2
 *   Retorna 0-8 (dirección) igual que la BIOS MSX.
 */
uint8_t hal_joystick_read(uint8_t port)
{
    if (port >= 2) return 0;
    return joy_state[port] & 0x0Fu;  /* solo los 4 bits de dirección */
}

/*
 * Emula BIOS GTTRIG(port): retorna si el botón fire está pulsado.
 * En el juego se usa como "any key" en las pantallas de título.
 */
bool hal_key_pressed(void)
{
    /* fire1 del joystick 1 OR cualquier tecla del teclado */
    return (joy_state[0] & 0x10u) != 0;
}

/* ==========================================================================
 * TIMING / VSYNC
 * ========================================================================== */

/*
 * Espera hasta completar el frame (16 ms a 60Hz o 20 ms a 50Hz).
 * Equivale al VBlank interrupt del TMS9918A.
 * Después llama al presentador del VDP para que la imagen sea visible.
 */
void hal_wait_vsync(void)
{
    music_isr_tick();   /* VBlank ISR: advance music player */
    hal_vdp_present();

    uint64_t now     = SDL_GetTicks64();
    uint64_t elapsed = now - frame_start_ticks;

    if (elapsed < frame_period_ms) {
        SDL_Delay((uint32_t)(frame_period_ms - elapsed));
    }
    frame_start_ticks = SDL_GetTicks64();
}

void hal_delay(uint8_t frames)
{
    for (int i = 0; i < frames; i++) {
        hal_poll_events();
        if (!hal_is_running()) return;
        hal_wait_vsync();
    }
}
