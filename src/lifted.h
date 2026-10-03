/*
 * lifted.h - what the generated C (work/<game>/gen/) compiles against.
 *
 * The CPU model is pcrecomp's runtime/recomp16/cpu.h, used as-is. Everything
 * the lifted code calls out to -- interrupts, ports, indirect dispatch, the
 * x87 -- is declared here and implemented by the DOS machine in src/.
 * See docs/architecture.md.
 */
#pragma once

#define RECOMP_MEM_HOOK          /* planar VGA: A000 is not plain memory */
#define RECOMP_IRQ               /* loops poll the timer/keyboard/SB */
#include "recomp16/cpu.h"
#include <math.h>

/* ---- x87 (Borland's emulator INTs, decoded as the ops they stand for) ---- */
typedef struct { double st[8]; int top; uint16_t sw, cw; } X87;
extern X87 g_x87;
#define X87_ST(i) g_x87.st[(g_x87.top + (i)) & 7]
static inline void x87_push(double v) { g_x87.top = (g_x87.top - 1) & 7; X87_ST(0) = v; }
static inline void x87_pop(void) { g_x87.top = (g_x87.top + 1) & 7; }
double   x87_rd(CPU *cpu, uint16_t seg, uint16_t off, int kind);
void     x87_wr(CPU *cpu, uint16_t seg, uint16_t off, int kind, double v);
void     x87_cmp(double a, double b);
uint16_t x87_sw(void);
void     x87_arith(int op, double *dst, double src);
void     x87_unhandled(const char *what);
void     x87_emu3e(CPU *cpu, uint8_t fn);

/* ---- the hi-res renderer's hooks (tools/lift.py wraps these in; src/hires.c) ---- */
enum { DRAW_OTHER = 0, DRAW_WALL = 1, DRAW_PLANE = 2, DRAW_SPRITE = 3 };
extern int g_draw_tag;                   /* who is writing video memory right now */
void hires_hit(CPU *cpu);                /* after each of the raycaster's Hit* calls */
void hires_flip(void);                   /* the CRTC start address changed */
void hires_plane(CPU *cpu, int what);
void hires_sprite_col(CPU *cpu, int shaded);  /* before a sprite column's posts are drawn */    /* before a floor/ceiling span: 1 ceiling, 2 floor, 4 shaded */
typedef struct {
    int ok;                              /* found in this game's code at lift time */
    uint16_t yint, xint, pixx, wallheight, postseg, postoff;
    uint16_t lightflag, normalshade, shademax, ls_seg, ls_off, centery;
    uint16_t pl_bp, pl_cx, pl_dxh, pl_dxl, pl_sih, pl_sil, pl_di, pl_texseg, pl_shseg, pl_shoff;
    uint16_t sp_cmdseg, sp_cmdoff, sp_shseg, sp_shoff;
    uint16_t pixelangle, midangle, player, p_angle, p_x, p_y, focal;   /* widescreen */
    int ws_ok;
} HiresVars;
extern const HiresVars g_hires;
void hires_side(CPU *cpu);                      /* after DrawScaleds: the side passes */
void hires_side_call(CPU *cpu, int which);      /* generated: 0 WallRefresh, 1 DrawScaleds */

/* ---- services ---- */
void dos_int21(CPU *cpu);
void bios_int10(CPU *cpu);
void bios_int16(CPU *cpu);
void mouse_int33(CPU *cpu);
void int_handler(CPU *cpu, unsigned num);

uint8_t  port_in8(CPU *cpu, uint16_t port);
uint16_t port_in16(CPU *cpu, uint16_t port);
void     port_out8(CPU *cpu, uint16_t port, uint8_t v);
void     port_out16(CPU *cpu, uint16_t port, uint16_t v);

/* ---- indirect control flow ---- */
void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off);   /* jmp: no frame */
void dispatch_near(CPU *cpu, uint16_t cs, uint16_t off);      /* call: 2-byte frame pushed */
void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off);      /* call: 4-byte frame pushed */
void recomp_div0(const char *what);
int  cpu_lar(CPU *cpu, uint16_t sel, uint16_t *out);
int  cpu_lsl(CPU *cpu, uint16_t sel, uint16_t *out);

typedef struct { uint32_t addr; void (*fn)(CPU *); } RecompFunc;

/* emitted by tools/lift.py into recomp_dispatch.c */
extern const uint16_t g_load_seg, g_psp_seg;
extern const uint16_t g_entry_cs, g_entry_ip, g_entry_ss, g_entry_sp;
extern const char g_game_id[];
extern const unsigned g_func_count;
extern const RecompFunc g_funcs[];

#ifndef BSTONE_SELFTEST            /* tests/selftest.c builds without a lift */
#include "recomp_all.h"
#endif
