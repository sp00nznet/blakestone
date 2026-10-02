/*
 * machine.c - the PC around the CPU: PIC, PIT, keyboard controller, speaker
 * gate, and getting interrupts into lifted code.
 *
 * Lifted code runs to completion inside C calls, so nothing can interrupt it
 * the way an IRQ interrupts a real CPU. Instead every loop back-edge counts
 * down a budget (RECOMP_TICK in cpu.h) and recomp_tick() runs here: it works
 * out how much emulated time has passed, delivers the timer interrupts that
 * fell due in order -- rendering audio up to each one, so the music the
 * engine's 700 Hz timer plays lands where it was written -- then keyboard and
 * Sound Blaster IRQs, and gives the host a frame. Delivering at back-edges is
 * also where a real 386 would most likely have taken them: the JAM engine
 * spins on TimeCount, which its own timer ISR advances.
 *
 * See docs/architecture.md, "Time".
 */
#include "machine.h"
#ifdef _WIN32
#include <windows.h>
#ifdef BSTONE_DEBUG
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

CPU g_cpu;
int g_recomp_tick_budget = 1;

/* ---- host clock ------------------------------------------------------------ */

uint64_t host_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart);
#else
    return 0;
#endif
}

static uint64_t t_start;
static uint64_t emu_now;              /* PIT ticks, what the guest has been shown */

uint64_t pit_now(void) { return emu_now; }

/* ---- PIC ----------------------------------------------------------------- */

static uint8_t pic_mask[2] = { 0xB8, 0xFF };   /* timer, keyboard, cascade, IRQ 5/7? set by the game */
static int in_irq;                     /* one interrupt at a time; no nesting */

/* ---- PIT ----------------------------------------------------------------- */

static uint32_t pit_div[3] = { 65536, 65536, 65536 };
static uint8_t  pit_mode_access[3] = { 3, 3, 3 };
static int      pit_flip[3];
static uint16_t pit_latch[3];
static int      pit_latched[3];
static uint64_t next_t0;
static int      t0_pending;

static uint16_t pit_count(int ch)
{
    uint64_t period = pit_div[ch];
    return (uint16_t)(period - (emu_now % period));
}

/* ---- keyboard controller ---------------------------------------------------- */

static uint8_t kq[64];
static int kq_head, kq_tail;
static uint8_t port60, port61 = 0x00;

void kbd_scancode(uint8_t sc)
{
    int n = (kq_tail + 1) & 63;
    if (n != kq_head) { kq[kq_tail] = sc; kq_tail = n; }
}

/* ---- ports ------------------------------------------------------------- */

int machine_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x20: case 0xA0: *v = 0; return 1;
    case 0x21: *v = pic_mask[0]; return 1;
    case 0xA1: *v = pic_mask[1]; return 1;
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        uint16_t c = pit_latched[ch] ? pit_latch[ch] : pit_count(ch);
        int acc = pit_mode_access[ch];
        if (acc == 1) *v = (uint8_t)c;
        else if (acc == 2) *v = (uint8_t)(c >> 8);
        else { *v = pit_flip[ch] ? (uint8_t)(c >> 8) : (uint8_t)c; pit_flip[ch] ^= 1;
               if (!pit_flip[ch]) pit_latched[ch] = 0; }
        return 1;
    }
    case 0x60: *v = port60; return 1;
    case 0x61: {
        static uint8_t refresh;
        refresh ^= 0x10;
        *v = (uint8_t)((port61 & 0x0F) | refresh);
        return 1;
    }
    case 0x64: *v = (uint8_t)(0x14 | (kq_head != kq_tail ? 1 : 0)); return 1;
    case 0x201: *v = 0xFF; return 1;           /* no joystick: the axis bits never fall */
    }
    return 0;
}

int machine_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x20: case 0xA0: return 1;            /* EOI: nothing is held in service */
    case 0x21: pic_mask[0] = v; return 1;
    case 0xA1: pic_mask[1] = v; return 1;
    case 0x43: {
        int ch = v >> 6;
        if (ch == 3) return 1;                 /* read-back: not used */
        if (((v >> 4) & 3) == 0) { pit_latch[ch] = pit_count(ch); pit_latched[ch] = 1; pit_flip[ch] = 0; return 1; }
        pit_mode_access[ch] = (v >> 4) & 3;
        pit_flip[ch] = 0;
        return 1;
    }
    case 0x40: case 0x41: case 0x42: {
        int ch = port - 0x40;
        static uint16_t acc[3];
        int a = pit_mode_access[ch];
        if (a == 1) acc[ch] = (acc[ch] & 0xFF00) | v;
        else if (a == 2) acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8);
        else if (!pit_flip[ch]) { acc[ch] = (acc[ch] & 0xFF00) | v; pit_flip[ch] = 1; return 1; }
        else { acc[ch] = (uint16_t)((acc[ch] & 0xFF) | v << 8); pit_flip[ch] = 0; }
        pit_div[ch] = acc[ch] ? acc[ch] : 65536;
        if (ch == 0) { next_t0 = emu_now + pit_div[0]; trace("[pit] timer 0 at %.1f Hz\n", PIT_HZ / pit_div[0]); }
        if (ch == 2) audio_speaker((port61 & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    }
    case 0x60: case 0x64: return 1;            /* LEDs, controller commands */
    case 0x61:
        port61 = v;
        audio_speaker((v & 3) == 3, (uint16_t)pit_div[2]);
        return 1;
    case 0x201: return 1;
    }
    return 0;
}

static int warned_ports[0x400];

uint8_t port_in8(CPU *cpu, uint16_t port)
{
    uint8_t v = 0xFF;
    (void)cpu;
    if (vga_port_in(port, &v) || audio_port_in(port, &v) || machine_port_in(port, &v)) return v;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] in %03X\n", port);
    return 0xFF;
}

void port_out8(CPU *cpu, uint16_t port, uint8_t v)
{
    (void)cpu;
    if (vga_port_out(port, v) || audio_port_out(port, v) || machine_port_out(port, v)) return;
    if (port < 0x400 && !warned_ports[port]++) trace("[port] out %03X = %02X\n", port, v);
}

uint16_t port_in16(CPU *cpu, uint16_t port)
{
    return (uint16_t)(port_in8(cpu, port) | port_in8(cpu, (uint16_t)(port + 1)) << 8);
}

void port_out16(CPU *cpu, uint16_t port, uint16_t v)
{
    port_out8(cpu, port, (uint8_t)v);
    port_out8(cpu, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

/* ---- dispatch ----------------------------------------------------------- */

static void (*lookup(uint16_t seg, uint16_t off))(CPU *)
{
    int32_t lin = ((int32_t)seg - (int32_t)g_load_seg) * 16 + off;
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if ((int32_t)g_funcs[mid].addr < lin) lo = mid + 1; else hi = mid;
    }
    return (lo < g_func_count && (int32_t)g_funcs[lo].addr == lin) ? g_funcs[lo].fn : NULL;
}

/* An address the guest jumped to that has no lifted function. Recorded so the
 * next `tools/lift.py` run makes it an entry point (load segment form; the
 * lifter converts it back). */
static void miss(const char *how, uint16_t seg, uint16_t off)
{
    static uint32_t seen[512]; static int nseen;
    uint32_t key = (uint32_t)seg << 16 | off;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 512) seen[nseen++] = key;
    fprintf(stderr, "[miss] %s %04X:%04X (image %05X)\n", how, seg, off,
            (unsigned)(((int32_t)seg - g_load_seg) * 16 + off));
    { void host_backtrace(void); host_backtrace(); }
    char p[256]; snprintf(p, sizeof p, "work/%s/misses.txt", g_game_id);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%04X:%04X\n", seg, off); fclose(f); }
}

/* `jmp far [old]` / `pushf; call far [old]` into the BIOS: chain to the
 * built-in handler and return through its IRET. */
static void stub_iret(CPU *cpu, uint16_t off)
{
    bios_builtin(cpu, off);
    cpu->sp += 4;
    cpu->flags = pop16(cpu);
}

void recomp_dispatch(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    /* A routine that pops its own return address and jumps to it (Borland's
     * _setargv, which builds argv on the stack under itself) jumps to the
     * lifter's 0xFFFF sentinel: that is a return to the C caller, and the
     * guest has already taken the address off the stack. */
    if (off == 0xFFFF) return;
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else miss("jmp", seg, off);
}

void dispatch_near(CPU *cpu, uint16_t cs, uint16_t off)
{
    void (*fn)(CPU *) = lookup(cs, off);
    if (fn) fn(cpu); else { miss("call near", cs, off); cpu->sp += 2; }
}

void dispatch_far(CPU *cpu, uint16_t seg, uint16_t off)
{
    if (seg == bios_stub_seg()) { stub_iret(cpu, off); return; }
    void (*fn)(CPU *) = lookup(seg, off);
    if (fn) fn(cpu); else { miss("call far", seg, off); cpu->sp += 4; }
}

/* INT n: whatever the IVT holds, with the frame a real INT pushes. */
void call_vector(CPU *cpu, unsigned vec)
{
    uint8_t *v = &cpu->mem[vec * 4];
    uint16_t off = (uint16_t)(v[0] | v[1] << 8), seg = (uint16_t)(v[2] | v[3] << 8);
    if (seg == bios_stub_seg()) {
        if (!bios_builtin(cpu, off)) trace("[int] no handler for INT %02X\n", vec);
        return;
    }
    void (*fn)(CPU *) = lookup(seg, off);
    if (!fn) { miss("int", seg, off); return; }
    push16(cpu, cpu->flags);
    push16(cpu, seg);
    push16(cpu, 0xFFFF);
    cpu->flags &= ~(FLAG_IF | FLAG_TF);
    fn(cpu);                                   /* its IRET pops the frame */
}

void int_handler(CPU *cpu, unsigned num) { call_vector(cpu, num); }

static void irq(CPU *cpu, int line)
{
    in_irq = 1;
    call_vector(cpu, line < 8 ? 8 + line : 0x70 + line - 8);
    in_irq = 0;
}

static int irq_ok(CPU *cpu, int line)
{
    return !in_irq && (cpu->flags & FLAG_IF) && !(pic_mask[line >> 3] & (1 << (line & 7)));
}

int  sb_irq_pending(void);
int  sb_irq_line(void);
void sb_irq_ack(void);

void recomp_tick(CPU *cpu)
{
    static int busy;
    g_recomp_tick_budget = 2000;
    if (busy) return;
    busy = 1;

    uint64_t target = (uint64_t)((double)(host_us() - t_start) * (PIT_HZ / 1e6));
    if (target > next_t0 + (uint64_t)(PIT_HZ / 10)) {
        /* far behind (a long load, a debugger): drop the backlog rather than
         * replay a storm of ticks the game would see as a time warp anyway */
        next_t0 = target - pit_div[0];
    }
    while (next_t0 <= target) {
        audio_advance(next_t0);
        emu_now = next_t0;
        next_t0 += pit_div[0];
        t0_pending = 1;
        if (irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
        else break;                            /* masked or cli: wait, one pending at most */
    }
    if (t0_pending && irq_ok(cpu, 0)) { t0_pending = 0; irq(cpu, 0); }
    if (target > emu_now) { audio_advance(target); emu_now = target; }

    while (kq_head != kq_tail && irq_ok(cpu, 1)) {
        port60 = kq[kq_head];
        kq_head = (kq_head + 1) & 63;
        irq(cpu, 1);
    }
    if (sb_irq_pending() && irq_ok(cpu, sb_irq_line())) { sb_irq_ack(); irq(cpu, sb_irq_line()); }

    host_frame();
    busy = 0;
}

void machine_init(void)
{
    t_start = host_us();
    next_t0 = pit_div[0];
}

void recomp_div0(const char *what)
{
    fprintf(stderr, "[cpu] divide by zero (%s)\n", what);
    call_vector(&g_cpu, 0);
}

int cpu_lar(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }
int cpu_lsl(CPU *cpu, uint16_t sel, uint16_t *out) { (void)cpu; (void)sel; (void)out; return 0; }

int cpu_alloc_mem(CPU *cpu)
{
    cpu->mem = (uint8_t *)calloc(1, MEM_SIZE);
    return cpu->mem ? 0 : -1;
}

/* ---- debugging: BSTONE_WATCH=<linear hex> reports every write to that byte
 * with the lifted functions most recently entered (build with
 * -DBSTONE_TRACE=ON so the lifted code records them). */
#ifdef RECOMP_TRACE
static const char *ring[64];
static unsigned ring_n;
void recomp_enter(const char *fn) { ring[ring_n++ & 63] = fn; }
void recomp_trace_dump(const char *why)
{
    fprintf(stderr, "  [%s] last functions:", why);
    for (unsigned i = ring_n > 12 ? ring_n - 12 : 0; i < ring_n; i++) fprintf(stderr, " %s", ring[i & 63]);
    fprintf(stderr, "\n");
}
#endif

/* The host stack, symbolised (debug builds carry a PDB): lifted functions
 * keep their fn_ names, so this is the guest call chain. */
void host_backtrace(void)
{
#if defined(BSTONE_DEBUG) && defined(_WIN32)
    static int init;
    HANDLE p = GetCurrentProcess();
    if (!init) { init = 1; SymInitialize(p, NULL, TRUE); }
    void *fr[16]; USHORT n = CaptureStackBackTrace(1, 16, fr, NULL);
    char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127;
    fprintf(stderr, "   ");
    for (USHORT i = 0; i < n; i++)
        if (SymFromAddr(p, (DWORD64)(size_t)fr[i], NULL, si)) fprintf(stderr, " %s", si->Name);
    fprintf(stderr, "\n");
#endif
}

void watch_write(uint32_t a, uint8_t v)
{
    static uint32_t w = 1;
    if (w == 1) { const char *e = getenv("BSTONE_WATCH"); w = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (!w || (a != w && a != w + 1)) return;
    fprintf(stderr, "[watch] %05X = %02X\n", a, v);
    host_backtrace();
}
