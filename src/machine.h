/*
 * machine.h - the DOS machine under the lifted game: shared state between
 * src/*.c. The generated code never includes this; it sees lifted.h only.
 */
#pragma once
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern CPU g_cpu;

typedef struct {
    const char *datadir;      /* the game's files (read) */
    const char *savedir;      /* where CONFIG and saved games go (read+write) */
    int headless;             /* no window: render offscreen */
    const char *record;       /* --record out.mp4 */
    double seconds;           /* stop after this long (0 = never) */
    const char *keys;         /* scripted input: "ms:key,ms:key" */
    const char *shots;        /* --shot-at "ms:file.bmp,..." */
    const char *wav;          /* --wav out.wav: the audio, no ffmpeg needed */
    int mute;
    int trace;                /* RECOMP_TRACE-ish logging of DOS calls */
    const char *args;         /* game command line tail */
    int scale;                /* window scale factor */
    int fullscreen;
    const char *display;      /* sharp | pixel | crt */
    int hires;                /* hi-res renderer scale, 0 off, -1 default */
    const char *route;        /* --route: the test autopilot (host.c) */
    const char *wide;         /* widescreen aspect: "16:9", "21:9", "off"; NULL default */
    int realtime;             /* headless, but on the wall clock */
} Options;
extern Options g_opt;

uint64_t host_us(void);
void     trace(const char *fmt, ...);
void     fatal(const char *fmt, ...);

/* dos.c */
void dos_boot(CPU *cpu);
void dos_exit(int code);              /* longjmps back to main */
uint16_t bios_stub_seg(void);
int  bios_builtin(CPU *cpu, unsigned num);   /* the BIOS/DOS behind an unhooked vector */
void bios_key_push(uint8_t scan, uint8_t ascii);

/* machine.c: PIC, PIT, keyboard controller, speaker, IRQ delivery */
void machine_init(void);
void kbd_scancode(uint8_t sc);        /* from the host: one byte of a make/break */
void mouse_host(int dx, int dy, int buttons);
int  machine_port_in(uint16_t port, uint8_t *v);
int  machine_port_out(uint16_t port, uint8_t v);
void call_vector(CPU *cpu, unsigned vec);    /* run whatever is in the IVT, as INT would */
uint64_t pit_now(void);
uint64_t emu_us(void);                /* see machine.c: wall clock, or deterministic */
void machine_report(void);               /* emulated time in PIT ticks (1.193182 MHz) */
#define PIT_HZ 1193182.0

/* vga.c */
void vga_init(void);
void vga_start(void);
void vga_set_mode(int m);
int  vga_mode(void);
int  vga_port_in(uint16_t port, uint8_t *v);
int  vga_port_out(uint16_t port, uint8_t v);
void vga_dac_set(int i, uint8_t r, uint8_t g, uint8_t b);
void vga_dac_get(int i, uint8_t *r, uint8_t *g, uint8_t *b);
void vga_compose(uint32_t *out, int *w, int *h);
const uint8_t *vga_vram(void);
const uint8_t *vga_owner(void);
int vga_unchained(void);
unsigned vga_scan_start(void);
unsigned vga_row_bytes(void);
uint32_t vga_color(int i);
int vga_map_mask(void);
void vga_hold(int on);
extern int g_tick_hold, g_recomp_tick_budget;

/* audio.c: OPL2 (opl.cpp), Sound Blaster DSP + 8237 DMA, PC speaker */
void audio_init(void);
void audio_advance(uint64_t pit_ticks);    /* render up to this emulated time */
int  audio_port_in(uint16_t port, uint8_t *v);
int  audio_port_out(uint16_t port, uint8_t v);
void audio_speaker(int gate, uint16_t divisor);
void audio_shutdown(void);

/* host.c: window or offscreen, input, presentation, recording */
void host_init(void);
void host_frame(void);                /* present + pump, at most once per refresh */
void host_audio(const int16_t *stereo, int frames);
void host_shutdown(void);
#define AUDIO_RATE 49716              /* the OPL2's own rate: 3.579545 MHz / 72 */
