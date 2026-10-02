/*
 * audio.c - the three things Blake Stone can make noise with:
 *
 *   AdLib       OPL2 at 388h (and its Sound Blaster mirrors), music and the
 *               FM sound effects; ymfm via opl.cpp.
 *   Sound Blaster  DSP at 220h, 8-bit single-cycle DMA on channel 1, IRQ 5 --
 *               the BLASTER line dos.c puts in the environment. The digitized
 *               effects are paged out of VSWAP and played a block at a time;
 *               each finished block raises the IRQ, and the game's ISR queues
 *               the next.
 *   PC speaker  PIT channel 2 gated through port 61h.
 *
 * Everything is rendered at the OPL's own rate and only as far as emulated
 * time has advanced (machine.c calls audio_advance before each timer IRQ), so
 * a register write lands in the stream where the guest made it.
 */
#include "machine.h"

void    opl_init(void);
void    opl_write(int data_port, uint8_t v);
uint8_t opl_status(void);
int     opl_sample(void);

static uint64_t rendered;             /* samples so far */

/* ---- 8237 DMA ------------------------------------------------------------ */

static struct { uint16_t base_addr, base_count, addr, count; uint8_t page, mode, masked; } dma[4];
static int dma_ff;
static const uint16_t dma_page_port[4] = { 0x87, 0x83, 0x81, 0x82 };

static int dma_port_out(uint16_t port, uint8_t v)
{
    if (port < 8) {
        int ch = port >> 1;
        uint16_t *b = (port & 1) ? &dma[ch].base_count : &dma[ch].base_addr;
        *b = dma_ff ? (uint16_t)((*b & 0xFF) | v << 8) : (uint16_t)((*b & 0xFF00) | v);
        dma_ff ^= 1;
        if (port & 1) dma[ch].count = dma[ch].base_count; else dma[ch].addr = dma[ch].base_addr;
        return 1;
    }
    switch (port) {
    case 0x08: case 0x09: case 0x0E: return 1;
    case 0x0A: dma[v & 3].masked = (v >> 2) & 1; return 1;
    case 0x0B: dma[v & 3].mode = v; return 1;
    case 0x0C: dma_ff = 0; return 1;
    case 0x0D: dma_ff = 0; for (int i = 0; i < 4; i++) dma[i].masked = 1; return 1;
    case 0x0F: for (int i = 0; i < 4; i++) dma[i].masked = (v >> i) & 1; return 1;
    }
    for (int i = 0; i < 4; i++) if (port == dma_page_port[i]) { dma[i].page = v; return 1; }
    return 0;
}

static int dma_port_in(uint16_t port, uint8_t *v)
{
    if (port < 8) {
        int ch = port >> 1;
        uint16_t w = (port & 1) ? dma[ch].count : dma[ch].addr;
        *v = dma_ff ? (uint8_t)(w >> 8) : (uint8_t)w;
        dma_ff ^= 1;
        return 1;
    }
    if (port == 0x08) { *v = 0; return 1; }
    return 0;
}

/* one byte through a DMA channel; returns 0 if the channel is not running */
static int dma_read(int ch, uint8_t *b)
{
    if (dma[ch].masked) return 0;
    *b = g_cpu.mem[((uint32_t)dma[ch].page << 16 | dma[ch].addr) & 0xFFFFF];
    dma[ch].addr++;
    if (dma[ch].count-- == 0) {
        if (dma[ch].mode & 0x10) { dma[ch].addr = dma[ch].base_addr; dma[ch].count = dma[ch].base_count; }
        else dma[ch].masked = 1;
    }
    return 1;
}

/* ---- Sound Blaster DSP ------------------------------------------------------ */

#define SB_BASE 0x220
static struct {
    uint8_t q[16]; int qh, qt;            /* bytes for the read-data port */
    int reset;
    uint8_t cmd; int need, got; uint8_t arg[4];
    uint32_t rate; int speaker;
    int playing, autoinit, silence;
    uint32_t len, pos;                    /* the DSP's block, in bytes */
    double phase; int cur;
    int irq;
    uint8_t mixer_i, mixer[256];
    uint8_t test;
} sb = { .rate = 11025, .cur = 128 };

int  sb_irq_pending(void) { return sb.irq; }
int  sb_irq_line(void) { return 5; }
void sb_irq_ack(void) { sb.irq = 0; }

static void sb_put(uint8_t v) { sb.q[sb.qt] = v; sb.qt = (sb.qt + 1) & 15; }

static void sb_command(void)
{
    uint8_t *a = sb.arg;
    if (sb.cmd != 0x14) trace("[sb] DSP %02X\n", sb.cmd);
    switch (sb.cmd) {
    case 0x10: sb.cur = a[0]; break;
    case 0x14: case 0x91:                              /* 91h: block size from 48h */
        if (sb.cmd == 0x14) sb.len = (uint32_t)(a[0] | a[1] << 8) + 1;
        sb.pos = 0; sb.playing = 1; sb.autoinit = 0; sb.silence = 0;
        trace("[sb] play %u bytes at %u Hz from %X\n", sb.len, sb.rate, (unsigned)dma[1].page << 16 | dma[1].addr);
        break;
    case 0x1C: case 0x90: sb.pos = 0; sb.playing = 1; sb.autoinit = 1; sb.silence = 0; break;
    case 0x48: sb.len = (uint32_t)(a[0] | a[1] << 8) + 1; break;
    case 0x40: sb.rate = 1000000u / (256u - a[0]); break;
    case 0x80: sb.len = (uint32_t)(a[0] | a[1] << 8) + 1; sb.pos = 0; sb.playing = 1; sb.silence = 1; sb.autoinit = 0; break;
    case 0xD0: sb.playing = 0; break;
    case 0xD4: sb.playing = 1; break;
    case 0xD1: sb.speaker = 1; break;
    case 0xD3: sb.speaker = 0; break;
    case 0xD8: sb_put(sb.speaker ? 0xFF : 0); break;
    case 0xDA: sb.autoinit = 0; break;
    case 0xE0: sb_put((uint8_t)~a[0]); break;
    case 0xE1: sb_put(3); sb_put(2); break;       /* SB Pro, DSP 3.02 */
    case 0xE4: sb.test = a[0]; break;
    case 0xE8: sb_put(sb.test); break;
    case 0xF2: sb.irq = 1; break;
    case 0x20: sb_put(0x80); break;
    default: trace("[sb] DSP command %02X\n", sb.cmd);
    }
}

static int sb_args(uint8_t c)
{
    switch (c) {
    case 0x10: case 0x40: case 0xE0: case 0xE4: return 1;
    case 0x14: case 0x16: case 0x17: case 0x48: case 0x80: return 2;
    }
    return 0;
}

static int16_t sb_sample(void)
{
    if (!sb.playing) return 0;
    sb.phase += (double)sb.rate / AUDIO_RATE;
    while (sb.phase >= 1.0) {
        sb.phase -= 1.0;
        uint8_t b = 128;
        if (!sb.silence && !dma_read(1, &b)) break;
        sb.cur = b;
        if (++sb.pos >= sb.len) {
            sb.irq = 1;
            if (sb.autoinit) sb.pos = 0;
            else { sb.playing = 0; break; }
        }
    }
    return sb.speaker ? (int16_t)((sb.cur - 128) * 96) : 0;
}

/* ---- PC speaker --------------------------------------------------------- */

static int spk_on;
static double spk_phase, spk_step;

void audio_speaker(int gate, uint16_t divisor)
{
    spk_on = gate && divisor > 16;
    spk_step = spk_on ? (PIT_HZ / divisor) / AUDIO_RATE : 0;
}

/* ---- ports ---------------------------------------------------------------- */

int audio_port_in(uint16_t port, uint8_t *v)
{
    if (dma_port_in(port, v)) return 1;
    switch (port) {
    case 0x388: case SB_BASE + 8: case SB_BASE: case SB_BASE + 2: *v = opl_status(); return 1;
    case 0x389: case SB_BASE + 9: case SB_BASE + 1: case SB_BASE + 3: *v = 0xFF; return 1;
    case SB_BASE + 0x5: *v = sb.mixer[sb.mixer_i]; return 1;
    case SB_BASE + 0xA:
        if (sb.qh != sb.qt) { *v = sb.q[sb.qh]; sb.qh = (sb.qh + 1) & 15; } else *v = 0xFF;
        return 1;
    case SB_BASE + 0xC: *v = 0x00; return 1;           /* ready for a command */
    case SB_BASE + 0xE: *v = (sb.qh != sb.qt) ? 0x80 : 0x00; sb.irq = 0; return 1;
    }
    return 0;
}

int audio_port_out(uint16_t port, uint8_t v)
{
    if (dma_port_out(port, v)) return 1;
    switch (port) {
    case 0x388: case SB_BASE + 8: case SB_BASE: case SB_BASE + 2: opl_write(0, v); return 1;
    case 0x389: case SB_BASE + 9: case SB_BASE + 1: case SB_BASE + 3: opl_write(1, v); return 1;
    case SB_BASE + 0x4: sb.mixer_i = v; return 1;
    case SB_BASE + 0x5: sb.mixer[sb.mixer_i] = v; return 1;
    case SB_BASE + 0x6:
        if (v & 1) sb.reset = 1;
        else if (sb.reset) { sb.reset = 0; sb.qh = sb.qt = 0; sb.playing = 0; sb.need = 0; sb_put(0xAA); }
        return 1;
    case SB_BASE + 0xC:
        if (sb.need) { sb.arg[sb.got++] = v; if (--sb.need == 0) sb_command(); }
        else { sb.cmd = v; sb.got = 0; sb.need = sb_args(v); if (!sb.need) sb_command(); }
        return 1;
    }
    return 0;
}

/* ---- rendering -------------------------------------------------------------- */

void audio_init(void) { opl_init(); }

void audio_advance(uint64_t pit_ticks)
{
    static int16_t buf[2048 * 2];
    uint64_t want = (uint64_t)((double)pit_ticks * AUDIO_RATE / PIT_HZ);
    while (rendered < want) {
        int n = (int)((want - rendered) > 2048 ? 2048 : (want - rendered));
        for (int i = 0; i < n; i++) {
            int s = opl_sample() + sb_sample();
            if (spk_on) { spk_phase += spk_step; if (spk_phase >= 1) spk_phase -= 1; s += spk_phase < 0.5 ? 3000 : -3000; }
            if (g_opt.mute) s = 0;
            if (s > 32767) s = 32767; if (s < -32768) s = -32768;
            buf[i * 2] = buf[i * 2 + 1] = (int16_t)s;
        }
        host_audio(buf, n);
        rendered += n;
    }
}

void audio_shutdown(void) {}
