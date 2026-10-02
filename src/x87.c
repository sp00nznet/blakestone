/*
 * x87.c - the FPU, for the Borland emulator calls the lifter turned back into
 * x87 operations. The engine only uses floating point to build its trig and
 * projection tables at startup (and printf), so doubles are precise enough;
 * 80-bit operands convert on the way in and out.
 */
#include "machine.h"

X87 g_x87 = { {0}, 0, 0, 0x037F };

static double ext80_to_double(const uint8_t *p)
{
    uint64_t m = 0;
    for (int i = 7; i >= 0; i--) m = m << 8 | p[i];
    int e = (p[8] | p[9] << 8) & 0x7FFF, s = p[9] & 0x80;
    double v = (e == 0 && m == 0) ? 0.0 : ldexp((double)m, e - 16383 - 63);
    return s ? -v : v;
}

static void double_to_ext80(double v, uint8_t *p)
{
    memset(p, 0, 10);
    if (v == 0) { if (signbit(v)) p[9] = 0x80; return; }
    int s = v < 0, e;
    double f = frexp(fabs(v), &e);                     /* f in [0.5,1) */
    uint64_t m = (uint64_t)ldexp(f, 64);
    e = e - 1 + 16383;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(m >> (i * 8));
    p[8] = (uint8_t)e; p[9] = (uint8_t)((e >> 8) | (s ? 0x80 : 0));
}

static double round_cw(double v)
{
    switch ((g_x87.cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

double x87_rd(CPU *cpu, uint16_t seg, uint16_t off, int kind)
{
    uint8_t b[10];
    int n = kind < 0 ? -kind : kind;
    for (int i = 0; i < n; i++) b[i] = mem_read8(cpu, seg, (uint16_t)(off + i));
    switch (kind) {
    case 4:  { float f; memcpy(&f, b, 4); return f; }
    case 8:  { double d; memcpy(&d, b, 8); return d; }
    case 10: return ext80_to_double(b);
    case -2: { int16_t v; memcpy(&v, b, 2); return v; }
    case -4: { int32_t v; memcpy(&v, b, 4); return v; }
    case -8: { int64_t v; memcpy(&v, b, 8); return (double)v; }
    }
    return 0;
}

void x87_wr(CPU *cpu, uint16_t seg, uint16_t off, int kind, double v)
{
    uint8_t b[10];
    int n = kind < 0 ? -kind : kind;
    switch (kind) {
    case 4:  { float f = (float)v; memcpy(b, &f, 4); break; }
    case 8:  memcpy(b, &v, 8); break;
    case 10: double_to_ext80(v, b); break;
    case -2: { double r = round_cw(v); int16_t i = (r >= -32768 && r <= 32767) ? (int16_t)r : (int16_t)0x8000; memcpy(b, &i, 2); break; }
    case -4: { double r = round_cw(v); int32_t i = (r >= -2147483648.0 && r <= 2147483647.0) ? (int32_t)r : (int32_t)0x80000000; memcpy(b, &i, 4); break; }
    case -8: { int64_t i = (int64_t)round_cw(v); memcpy(b, &i, 8); break; }
    default: return;
    }
    for (int i = 0; i < n; i++) mem_write8(cpu, seg, (uint16_t)(off + i), b[i]);
}

void x87_cmp(double a, double b)
{
    g_x87.sw &= ~0x4500;                               /* C3 C2 C0 */
    if (isnan(a) || isnan(b)) g_x87.sw |= 0x4500;
    else if (a < b) g_x87.sw |= 0x0100;
    else if (a == b) g_x87.sw |= 0x4000;
}

uint16_t x87_sw(void) { return (uint16_t)((g_x87.sw & ~0x3800) | ((g_x87.top & 7) << 11)); }

void x87_arith(int op, double *dst, double src)
{
    switch (op) {
    case 0: *dst += src; break;
    case 1: *dst *= src; break;
    case 2: case 3: x87_cmp(*dst, src); break;
    case 4: *dst -= src; break;
    case 5: *dst = src - *dst; break;
    case 6: *dst /= src; break;
    case 7: *dst = src / *dst; break;
    }
}

void x87_unhandled(const char *what) { fprintf(stderr, "[x87] unhandled %s\n", what); }

/* Borland's INT 3Eh shortcuts, as its math library uses them when there is
 * no 387 (beside each one sits the 387 instruction it stands in for). */
void x87_emu3e(CPU *cpu, uint8_t fn)
{
    (void)cpu;
    switch (fn) {
    case 0xEC: X87_ST(0) = sin(X87_ST(0)); break;
    case 0xF0: X87_ST(0) = tan(X87_ST(0)); break;
    case 0xF2: X87_ST(0) = atan(X87_ST(0)); break;
    default: fprintf(stderr, "[x87] unknown INT 3Eh function %02X\n", fn);
    }
}
