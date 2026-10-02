/*
 * vga.c - the VGA the JAM engine programs directly.
 *
 * Blake Stone runs in "Mode X": BIOS mode 13h, then chain-4 switched off, so
 * one byte address at A000 covers four pixels, one per plane, and the Map Mask
 * picks which planes a write lands in. On top of that it leans on the parts of
 * the card a flat framebuffer does not have:
 *
 *   - latches: a read loads one byte from every plane, and write mode 1 stores
 *     those four bytes back -- the engine keeps its status bar and fonts in
 *     off-screen video memory and blits them four pixels per byte this way;
 *   - the CRTC start address, which is how pages flip (nothing is copied);
 *   - the Graphics Controller's set/reset, bit mask and logical ops, which the
 *     bar and fade routines use.
 *
 * VRAM is stored interleaved, vram[offset*4 + plane], which also makes chain-4
 * mode a plain linear store (plane = addr & 3, offset = addr >> 2).
 */
#include "machine.h"
#include "recomp16/platform/font8x8.h"

static uint8_t vram[0x40000];
static uint8_t latch[4];
static uint8_t seq[8], gc[16], crtc[32], attr[32];
static uint8_t seq_i, gc_i, crtc_i, attr_i, attr_flip;
static uint8_t dac[256][3], dac_wi, dac_ri, dac_wc, dac_rc, dac_mask = 0xFF;
static int mode = 3;                 /* BIOS mode: 3 text, 0x13 graphics */
static int chain4 = 0;
static uint8_t misc_out = 0x63;

uint8_t *vga_text_mem(void) { return g_cpu.mem + 0xB8000; }

static inline int chained(void) { return (seq[4] & 8) != 0; }

void vga_set_mode(int m)
{
    mode = m & 0x7F;
    memset(seq, 0, sizeof seq); memset(gc, 0, sizeof gc); memset(crtc, 0, sizeof crtc);
    seq[2] = 0x0F;
    gc[8] = 0xFF;
    gc[6] = 0x05;
    if (mode == 0x13) {
        seq[4] = 0x0E;               /* chain-4, odd/even off, extended memory */
        crtc[0x09] = 0x41;           /* double scan: 200 rows on 400 lines */
        crtc[0x12] = 0x8F;           /* vertical display end 399 (with overflow bit) */
        crtc[0x07] = 0x1F;
        crtc[0x13] = 40;             /* 320 bytes per row in dword mode */
        crtc[0x14] = 0x40;           /* dword mode */
        crtc[0x17] = 0xA3;
        gc[5] = 0x40;
        if (!(m & 0x80)) memset(vram, 0, sizeof vram);
    } else {
        crtc[0x09] = 0x0F;
        if (!(m & 0x80)) {
            uint8_t *t = vga_text_mem();
            for (int i = 0; i < 80 * 25; i++) { t[i * 2] = ' '; t[i * 2 + 1] = 7; }
        }
    }
    for (int i = 0; i < 16; i++) attr[i] = (uint8_t)i;
    attr[0x10] = (mode == 0x13) ? 0x41 : 0x0C;
    g_cpu.mem[0x449] = (uint8_t)mode;
    chain4 = chained();
}

int vga_mode(void) { return mode; }

void vga_init(void)
{
    /* The default VGA palette's first 16 entries, as text mode shows them. */
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    memset(dac, 0, sizeof dac);
    memcpy(dac, ega, sizeof ega);
    /* text mode reads attr -> DAC through the EGA-compatible mapping */
    vga_set_mode(3);
}

/* ---- CPU access to A000-AFFF ------------------------------------------- */

int recomp_mem_write8(CPU *cpu, uint32_t a, uint8_t v)
{
#ifdef BSTONE_DEBUG
    { void watch_write(uint32_t, uint8_t); watch_write(a, v); }
#endif
    if (a - 0xA0000u >= 0x10000u) return 0;
    (void)cpu;
    uint32_t off = a - 0xA0000u;
    if (chain4) { vram[off] = v; return 1; }           /* plane a&3, offset a>>2 */
    uint8_t mask = seq[2] & 0x0F;
    uint8_t *p = &vram[off * 4];
    int wmode = gc[5] & 3;
    if (wmode == 0 && !(gc[1] & 0x0F) && !(gc[3]) && gc[8] == 0xFF) {
        if (mask & 1) p[0] = v;
        if (mask & 2) p[1] = v;
        if (mask & 4) p[2] = v;
        if (mask & 8) p[3] = v;
        return 1;
    }
    if (wmode == 1) {
        for (int i = 0; i < 4; i++) if (mask & (1 << i)) p[i] = latch[i];
        return 1;
    }
    uint8_t rot = gc[3] & 7, fn = (gc[3] >> 3) & 3, bm = gc[8];
    uint8_t r = (uint8_t)((v >> rot) | (v << (8 - rot)));
    if (wmode == 3) { bm &= r; }
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        uint8_t d;
        if (wmode == 2)       d = (v & (1 << i)) ? 0xFF : 0;
        else if (wmode == 3)  d = (gc[0] & (1 << i)) ? 0xFF : 0;
        else                  d = (gc[1] & (1 << i)) ? ((gc[0] & (1 << i)) ? 0xFF : 0) : r;
        switch (fn) {
        case 1: d &= latch[i]; break;
        case 2: d |= latch[i]; break;
        case 3: d ^= latch[i]; break;
        }
        p[i] = (uint8_t)((d & bm) | (latch[i] & ~bm));
    }
    return 1;
}

int recomp_mem_read8(CPU *cpu, uint32_t a, uint8_t *out)
{
    if (a - 0xA0000u >= 0x10000u) return 0;
    (void)cpu;
    uint32_t off = a - 0xA0000u;
    if (chain4) { *out = vram[off]; return 1; }
    uint8_t *p = &vram[off * 4];
    latch[0] = p[0]; latch[1] = p[1]; latch[2] = p[2]; latch[3] = p[3];
    if (gc[5] & 8) {                                   /* read mode 1: colour compare */
        uint8_t m = 0xFF;                              /* bits where every cared-about plane matched */
        for (int i = 0; i < 4; i++)
            if (gc[7] & (1 << i)) m &= (uint8_t)~(latch[i] ^ ((gc[2] & (1 << i)) ? 0xFF : 0));
        *out = m;
        return 1;
    }
    *out = latch[gc[4] & 3];
    return 1;
}

/* ---- ports 3C0-3DA --------------------------------------------------------- */

static uint64_t t0_us;

/* 70 Hz refresh, the last ~10% of it in vertical retrace. Real time, because
 * VL_WaitVBL is how the engine paces its fades and the intro. */
static uint8_t input_status(void)
{
    static uint8_t de;
    uint64_t us = host_us() - t0_us;
    uint64_t ph = us % 14286;
    de ^= 1;                                           /* display enable flickers per line */
    return (uint8_t)(((ph >= 12900) ? 0x08 : 0) | ((ph >= 12900) ? 1 : de));
}

int vga_port_in(uint16_t port, uint8_t *v)
{
    switch (port) {
    case 0x3C0: *v = attr_i; return 1;
    case 0x3C1: *v = attr[attr_i & 0x1F]; return 1;
    case 0x3C2: *v = 0x10; return 1;
    case 0x3C4: *v = seq_i; return 1;
    case 0x3C5: *v = seq[seq_i & 7]; return 1;
    case 0x3C6: *v = dac_mask; return 1;
    case 0x3C7: *v = 3; return 1;
    case 0x3C8: *v = dac_wi; return 1;
    case 0x3C9: *v = dac[dac_ri][dac_rc]; if (++dac_rc == 3) { dac_rc = 0; dac_ri++; } return 1;
    case 0x3CC: *v = misc_out; return 1;
    case 0x3CE: *v = gc_i; return 1;
    case 0x3CF: *v = gc[gc_i & 15]; return 1;
    case 0x3D4: *v = crtc_i; return 1;
    case 0x3D5: *v = crtc[crtc_i & 31]; return 1;
    case 0x3DA: attr_flip = 0; *v = input_status(); return 1;
    }
    return 0;
}

int vga_port_out(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_flip) attr_i = v; else attr[attr_i & 0x1F] = v;
        attr_flip ^= 1; return 1;
    case 0x3C2: misc_out = v; return 1;
    case 0x3C4: seq_i = v; return 1;
    case 0x3C5: seq[seq_i & 7] = v; if ((seq_i & 7) == 4) chain4 = chained(); return 1;
    case 0x3C6: dac_mask = v; return 1;
    case 0x3C7: dac_ri = v; dac_rc = 0; return 1;
    case 0x3C8: dac_wi = v; dac_wc = 0; return 1;
    case 0x3C9: dac[dac_wi][dac_wc] = v & 0x3F; if (++dac_wc == 3) { dac_wc = 0; dac_wi++; } return 1;
    case 0x3CE: gc_i = v; return 1;
    case 0x3CF: gc[gc_i & 15] = v; return 1;
    case 0x3D4: crtc_i = v; return 1;
    case 0x3D5: crtc[crtc_i & 31] = v; return 1;
    }
    return 0;
}

/* INT 10h palette services land here too. */
void vga_dac_set(int i, uint8_t r, uint8_t g, uint8_t b) { dac[i & 255][0] = r & 63; dac[i & 255][1] = g & 63; dac[i & 255][2] = b & 63; }
void vga_dac_get(int i, uint8_t *r, uint8_t *g, uint8_t *b) { *r = dac[i & 255][0]; *g = dac[i & 255][1]; *b = dac[i & 255][2]; }

/* ---- scanout --------------------------------------------------------------- */

static inline uint32_t rgb(int i)
{
    i &= dac_mask;
    return 0xFF000000u | ((uint32_t)(dac[i][0] * 255 / 63) << 16)
         | ((uint32_t)(dac[i][1] * 255 / 63) << 8) | (uint32_t)(dac[i][2] * 255 / 63);
}

/* What the monitor shows right now, as 0xAARRGGBB. Graphics: 320 x rows, rows
 * from the CRTC (200 for everything Blake Stone does). Text: 640x400. */
void vga_compose(uint32_t *out, int *w, int *h)
{
    if (mode != 0x13) {
        static uint32_t blink;
        const uint8_t *t = vga_text_mem();
        int cx = g_cpu.mem[0x450], cy = g_cpu.mem[0x451];
        blink++;
        *w = 640; *h = 400;
        for (int r = 0; r < 25; r++)
            for (int c = 0; c < 80; c++) {
                uint8_t ch = t[(r * 80 + c) * 2], at = t[(r * 80 + c) * 2 + 1];
                uint32_t fg = rgb(attr[at & 15] & 0x3F), bg = rgb(attr[(at >> 4) & 7] & 0x3F);
                if ((at & 0x80) && (blink & 32)) fg = bg;
                for (int y = 0; y < 16; y++) {
                    uint8_t bits = font8x8_cp437[ch][y >> 1];
                    if (r == cy && c == cx && y >= 13 && (blink & 16)) bits = 0xFF;
                    uint32_t *o = out + (r * 16 + y) * 640 + c * 8;
                    for (int x = 0; x < 8; x++) o[x] = (bits & (0x80 >> x)) ? fg : bg;
                }
            }
        return;
    }
    unsigned start = ((unsigned)crtc[0x0C] << 8) | crtc[0x0D];
    unsigned vde = crtc[0x12] | ((crtc[0x07] & 2) << 7) | ((crtc[0x07] & 0x40) << 3);
    unsigned scan = (crtc[0x09] & 0x1F) + 1;
    int rows = (int)((vde + 1) / scan);
    if (rows < 200 || rows > 480) rows = 200;
    *w = 320; *h = rows;
    if (chain4) {
        /* dword mode: the start address and the row offset count 4-byte units */
        unsigned row = crtc[0x13] ? crtc[0x13] * 8u : 320u;
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < 320; x++)
                out[y * 320 + x] = rgb(vram[(start * 4 + y * row + x) & 0x3FFFF]);
        return;
    }
    unsigned row = crtc[0x13] ? crtc[0x13] * 2u : 80u;
    for (int y = 0; y < rows; y++) {
        unsigned base = start + y * row;
        for (int x = 0; x < 320; x++)
            out[y * 320 + x] = rgb(vram[(((base + (x >> 2)) & 0xFFFF) << 2) | (x & 3)]);
    }
}

void vga_start(void) { t0_us = host_us(); }
