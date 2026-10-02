/*
 * selftest.c - the runtime's hardware models, checked without any game data,
 * so CI has something real to run (the conformance harness needs your copy of
 * the games and skips without it).
 *
 * One assert per behaviour the games are known to depend on; each comment
 * says which.
 */
#undef NDEBUG                     /* the asserts ARE the test, in every build */
#include "machine.h"
#include <assert.h>
#include <stdarg.h>

CPU g_cpu;
Options g_opt;
uint64_t host_us(void) { return 0; }
void trace(const char *fmt, ...) { (void)fmt; }
void fatal(const char *fmt, ...) { (void)fmt; abort(); }
void host_audio(const int16_t *s, int frames) { (void)s; (void)frames; }
uint64_t pit_now(void) { return 0; }
uint64_t emu_us(void) { return 0; }

static void out(uint16_t port, uint8_t v) { assert(vga_port_out(port, v) || audio_port_out(port, v)); }
static uint8_t in(uint16_t port) { uint8_t v = 0; assert(vga_port_in(port, &v) || audio_port_in(port, &v)); return v; }

static void test_vga(void)
{
    uint32_t frame[640 * 480]; int w, h;
    vga_init();
    vga_set_mode(0x13);

    /* chain-4: a byte at A000:x is pixel x */
    mem_write8(&g_cpu, 0xA000, 5, 0x2A);
    vga_dac_set(0x2A, 63, 0, 0);
    vga_compose(frame, &w, &h);
    assert(w == 320 && h == 200 && frame[5] == 0xFFFF0000u);

    /* unchain (Mode X): Sequencer 4 bit 3 off, then the Map Mask picks planes */
    out(0x3C4, 4); out(0x3C5, 0x06);
    out(0x3C4, 2); out(0x3C5, 0x0F);
    mem_write8(&g_cpu, 0xA000, 0, 7);              /* pixels 0..3 */
    out(0x3C5, 0x02);
    mem_write8(&g_cpu, 0xA000, 0, 9);              /* pixel 1 only */
    vga_dac_set(7, 0, 63, 0); vga_dac_set(9, 0, 0, 63);
    vga_compose(frame, &w, &h);
    assert(frame[0] == 0xFF00FF00u && frame[1] == 0xFF0000FFu && frame[2] == 0xFF00FF00u);

    /* latches + write mode 1: a read loads all four planes, a write stores
     * them -- the status bar and fonts are blitted out of VRAM this way */
    out(0x3C5, 0x0F);
    (void)mem_read8(&g_cpu, 0xA000, 0);
    out(0x3CE, 5); out(0x3CF, 1);
    mem_write8(&g_cpu, 0xA000, 80, 0xEE);          /* row 1; the byte is ignored */
    out(0x3CF, 0);
    vga_compose(frame, &w, &h);
    assert(frame[320] == 0xFF00FF00u && frame[321] == 0xFF0000FFu);

    /* CRTC start address: page flipping moves the scanout, copies nothing */
    out(0x3D4, 0x0C); out(0x3D5, 0); out(0x3D4, 0x0D); out(0x3D5, 80);
    vga_compose(frame, &w, &h);
    assert(frame[1] == 0xFF0000FFu);
    out(0x3D5, 0);

    /* DAC reads back what was written, 6 bits a channel */
    out(0x3C8, 3); out(0x3C9, 1); out(0x3C9, 2); out(0x3C9, 63);
    out(0x3C7, 3);
    assert(in(0x3C9) == 1 && in(0x3C9) == 2 && in(0x3C9) == 63);
}

static void test_x87(void)
{
    /* 80-bit round trip: the emulator keeps tables as temporary reals */
    double v[] = { 0.0, 1.0, -2.5, 3.141592653589793, 1e-300, 65536.25 };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        x87_wr(&g_cpu, 0x1000, 0, 10, v[i]);
        assert(x87_rd(&g_cpu, 0x1000, 0, 10) == v[i]);
    }
    /* fistp honours the rounding control: Borland's ftol truncates */
    g_x87.cw = 0x0F7F;
    x87_wr(&g_cpu, 0x1000, 0, -4, -2.75);
    assert((int32_t)mem_read32(&g_cpu, 0x1000, 0) == -2);
    g_x87.cw = 0x037F;
    x87_wr(&g_cpu, 0x1000, 0, -4, 2.5);
    assert((int32_t)mem_read32(&g_cpu, 0x1000, 0) == 2);  /* round to even */
    /* the Borland INT 3Eh shortcuts */
    x87_push(0.5); x87_emu3e(&g_cpu, 0xF2);
    assert(fabs(X87_ST(0) - atan(0.5)) < 1e-12); x87_pop();
}

static void test_sound(void)
{
    audio_init();
    /* DSP reset handshake: 1 then 0 to 226h, AAh waiting at 22Ah */
    out(0x226, 1); out(0x226, 0);
    assert(in(0x22E) & 0x80);
    assert(in(0x22A) == 0xAA);
    /* version: SB Pro (3.xx), which SD_Startup accepts */
    out(0x22C, 0xE1);
    assert(in(0x22A) == 3);
    (void)in(0x22A);
    /* AdLib detection: timer 1 started and unmasked reads back as expired */
    out(0x388, 4); out(0x389, 0x60);
    out(0x388, 4); out(0x389, 0x80);
    assert((in(0x388) & 0xE0) == 0);
    out(0x388, 2); out(0x389, 0xFF);
    out(0x388, 4); out(0x389, 0x21);
    assert((in(0x388) & 0xE0) == 0xC0);
}

int main(void)
{
    if (cpu_alloc_mem(&g_cpu) != 0) return 1;
    test_vga();
    test_x87();
    test_sound();
    puts("selftest: all passed");
    return 0;
}

int cpu_alloc_mem(CPU *cpu)
{
    cpu->mem = (uint8_t *)calloc(1, MEM_SIZE);
    return cpu->mem ? 0 : -1;
}
