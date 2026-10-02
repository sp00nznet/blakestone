/*
 * dos.c - DOS and the BIOS, as much of them as Blake Stone and the Borland
 * C++ runtime under it actually call.
 *
 * Files: the game's data directory is only ever read. Anything the game
 * creates or writes -- CONFIG, saved games -- goes to a separate save
 * directory, and reads look there first, so a retail install is never modified
 * (a file opened read/write is copied over on first open).
 *
 * Memory: a DOS arena kept on the host side. The program's block starts out
 * owning everything up to A000, as DOS hands it over; Borland's startup then
 * shrinks it (4Ah) and grows its far heap back with 4Ah, and the JAM memory
 * manager takes the rest.
 */
#include "machine.h"
#include <setjmp.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#endif

#define STUB_SEG 0xF000          /* the "BIOS" every unhooked vector points into */
#define ENV_SEG  0x00E0
#define MEM_TOP  0xA000

jmp_buf g_exit_jmp;
int g_exit_code;

uint16_t bios_stub_seg(void) { return STUB_SEG; }

/* ---- the DOS memory arena ------------------------------------------------ */

typedef struct { uint16_t seg, paras, owner; } Block;   /* seg = first usable paragraph */
static Block arena[64];
static int narena;

static void arena_sort(void)
{
    for (int i = 1; i < narena; i++)
        for (int j = i; j > 0 && arena[j].seg < arena[j - 1].seg; j--) {
            Block t = arena[j]; arena[j] = arena[j - 1]; arena[j - 1] = t;
        }
}

/* room above a block before the next one's MCB (1 paragraph) or the top */
static unsigned room_after(int i)
{
    unsigned end = (i + 1 < narena) ? arena[i + 1].seg - 1u : MEM_TOP;
    return end - arena[i].seg;
}

static int arena_find(uint16_t seg)
{
    for (int i = 0; i < narena; i++) if (arena[i].seg == seg) return i;
    return -1;
}

static unsigned largest_free(void)
{
    unsigned best = 0, lo = 0x0060;                   /* below that: IVT, BDA, DOS */
    for (int i = 0; i <= narena; i++) {
        unsigned hi = (i < narena) ? arena[i].seg - 1u : MEM_TOP;
        if (hi > lo + 1 && hi - lo - 1 > best) best = hi - lo - 1;
        if (i < narena) lo = arena[i].seg + arena[i].paras;
    }
    return best;
}

static int arena_alloc(uint16_t paras, uint16_t *seg)
{
    unsigned lo = 0x0060;
    for (int i = 0; i <= narena; i++) {               /* first fit, like DOS's default */
        unsigned hi = (i < narena) ? arena[i].seg - 1u : MEM_TOP;
        if (hi > lo + 1 && hi - lo - 1 >= paras) {
            arena[narena].seg = (uint16_t)(lo + 1);
            arena[narena].paras = paras;
            arena[narena].owner = g_psp_seg;
            *seg = (uint16_t)(lo + 1);
            narena++; arena_sort();
            return 1;
        }
        if (i < narena) lo = arena[i].seg + arena[i].paras;
    }
    return 0;
}

/* ---- file handles ---------------------------------------------------------- */

#define MAXH 40
static FILE *fh[MAXH];

static void read_asciiz(uint16_t seg, uint16_t off, char *out, int n)
{
    int i = 0;
    for (; i < n - 1; i++) {
        char c = (char)g_cpu.mem[seg_off(seg, (uint16_t)(off + i))];
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* DOS paths carry a drive and backslashes; the game's files are all flat. */
static const char *base_name(const char *p)
{
    const char *b = p;
    if (b[0] && b[1] == ':') b += 2;
    for (const char *s = b; *s; s++) if (*s == '\\' || *s == '/') b = s + 1;
    return b;
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR); }

static void copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[65536]; size_t n;
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static FILE *open_game(const char *dosname, int mode /*0 r,1 w,2 rw*/, int create)
{
    char sp[512], dp[512];
    const char *b = base_name(dosname);
    join(sp, sizeof sp, g_opt.savedir, b);
    join(dp, sizeof dp, g_opt.datadir, b);
    if (create) return fopen(sp, "w+b");
    if (mode == 0) {
        FILE *f = fopen(sp, "rb");
        return f ? f : fopen(dp, "rb");
    }
    if (!exists(sp) && exists(dp)) copy_file(dp, sp);
    return fopen(sp, "r+b");
}

static int new_handle(FILE *f)
{
    for (int i = 5; i < MAXH; i++) if (!fh[i]) { fh[i] = f; return i; }
    fclose(f);
    return -1;
}

/* ---- findfirst / findnext ------------------------------------------------ */

static char find_names[256][13];
static int find_n, find_i;
static uint16_t dta_seg, dta_off;

static int wild_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (; ; s++) { if (wild_match(pat + 1, s)) return 1; if (!*s) return 0; }
        }
        if (!*s) return 0;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return 0;
    }
    return *s == 0;
}

static void find_scan(const char *dir, const char *pat)
{
#ifdef _WIN32
    char q[512]; snprintf(q, sizeof q, "%s/*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(q, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (strlen(fd.cFileName) > 12 || !wild_match(pat, fd.cFileName)) continue;
        int dup = 0;
        for (int i = 0; i < find_n; i++) if (!_stricmp(find_names[i], fd.cFileName)) dup = 1;
        if (!dup && find_n < 256) {
            for (int k = 0; k < 13; k++) find_names[find_n][k] = (char)toupper((unsigned char)fd.cFileName[k]);
            find_n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
}

static int find_fill(void)
{
    if (find_i >= find_n) return 0;
    const char *n = find_names[find_i++];
    char sp[512], dp[512]; struct stat st = {0};
    join(sp, sizeof sp, g_opt.savedir, n); join(dp, sizeof dp, g_opt.datadir, n);
    if (stat(sp, &st) != 0) stat(dp, &st);
    uint8_t *d = &g_cpu.mem[seg_off(dta_seg, dta_off)];
    memset(d, 0, 43);
    d[0x15] = 0x20;
    d[0x16] = 0; d[0x17] = 0x60;                       /* 12:00 */
    d[0x18] = 0x21; d[0x19] = 0x1B;                    /* 1993-09-01 */
    d[0x1A] = (uint8_t)st.st_size; d[0x1B] = (uint8_t)(st.st_size >> 8);
    d[0x1C] = (uint8_t)(st.st_size >> 16); d[0x1D] = (uint8_t)(st.st_size >> 24);
    strncpy((char *)d + 0x1E, n, 12);
    return 1;
}

/* ---- text console ------------------------------------------------------------ */

static void tty_putc(uint8_t c)
{
    uint8_t *t = g_cpu.mem + 0xB8000;
    uint8_t *cx = &g_cpu.mem[0x450], *cy = &g_cpu.mem[0x451];
    if (vga_mode() != 3) return;
    if (c == '\r') *cx = 0;
    else if (c == '\n') (*cy)++;
    else if (c == 8) { if (*cx) (*cx)--; }
    else if (c == 7) {}
    else {
        t[(*cy * 80 + *cx) * 2] = c;
        if (++*cx >= 80) { *cx = 0; (*cy)++; }
    }
    if (*cy >= 25) {
        memmove(t, t + 160, 160 * 24);
        for (int i = 0; i < 80; i++) { t[24 * 160 + i * 2] = ' '; t[24 * 160 + i * 2 + 1] = 7; }
        *cy = 24;
    }
}

static void con_write(const uint8_t *p, int n)
{
    fwrite(p, 1, n, stdout);
    for (int i = 0; i < n; i++) tty_putc(p[i]);
}

/* ---- BIOS keyboard buffer ----------------------------------------------------- */

static uint16_t keybuf[32];
static int kb_head, kb_tail;

void bios_key_push(uint8_t scan, uint8_t ascii)
{
    int n = (kb_tail + 1) & 31;
    if (n != kb_head) { keybuf[kb_tail] = (uint16_t)(scan << 8 | ascii); kb_tail = n; }
}

static int key_wait(int peek, uint16_t *out)
{
    for (;;) {
        if (kb_head != kb_tail) {
            *out = keybuf[kb_head];
            if (!peek) kb_head = (kb_head + 1) & 31;
            return 1;
        }
        if (peek) return 0;
        recomp_tick(&g_cpu);
#ifdef _WIN32
        Sleep(1);
#endif
    }
}

/* ---- boot ------------------------------------------------------------------- */

/* the load module and its relocations, emitted by tools/lift.py */
extern const unsigned g_image_len, g_nrelocs;
extern const uint16_t g_relocs[][2];
extern const uint8_t g_image[];

void dos_boot(CPU *cpu)
{
    uint8_t *m = cpu->mem;
    /* IVT: every vector into the stub segment, offset = vector number */
    for (int i = 0; i < 256; i++) {
        m[i * 4] = (uint8_t)i; m[i * 4 + 1] = 0;
        m[i * 4 + 2] = STUB_SEG & 0xFF; m[i * 4 + 3] = STUB_SEG >> 8;
    }
    /* BIOS data area */
    m[0x410] = 0x63; m[0x411] = 0x00;                  /* VGA colour, 1 floppy, no FPU bit needed */
    m[0x413] = 640 & 0xFF; m[0x414] = 640 >> 8;
    m[0x44A] = 80; m[0x463] = 0xD4; m[0x464] = 0x03; m[0x484] = 24;
    m[0x487] = 0x60; m[0x488] = 0x09; m[0x489] = 0x11;
    m[0xFFFFE] = 0xFC;                                 /* model byte: AT */

    /* environment, with the BLASTER line SD_Startup parses */
    {
        char env[512]; int n = 0;
        const char *vars[] = { "COMSPEC=C:\\COMMAND.COM", "PATH=C:\\",
                               "BLASTER=A220 I5 D1 T3", NULL };
        for (int i = 0; vars[i]; i++) { strcpy(env + n, vars[i]); n += (int)strlen(vars[i]) + 1; }
        env[n++] = 0; env[n++] = 1; env[n++] = 0;
        n += sprintf(env + n, "C:\\%s.EXE", strcmp(g_game_id, "ps") ? "BS_AOG" : "BS_FIRE") + 1;
        memcpy(m + ENV_SEG * 16, env, n);
    }
    arena[narena++] = (Block){ ENV_SEG, 0x1F, g_psp_seg };

    /* PSP */
    uint8_t *psp = m + g_psp_seg * 16;
    psp[0] = 0xCD; psp[1] = 0x20;
    psp[2] = MEM_TOP & 0xFF; psp[3] = MEM_TOP >> 8;
    psp[0x2C] = ENV_SEG & 0xFF; psp[0x2D] = ENV_SEG >> 8;
    {
        const char *a = g_opt.args ? g_opt.args : "";
        int n = (int)strlen(a);
        if (n > 125) n = 125;
        psp[0x80] = (uint8_t)(n ? n + 1 : 0);
        if (n) { psp[0x81] = ' '; memcpy(psp + 0x82, a, n); }
        psp[0x81 + (n ? n + 1 : 0)] = 0x0D;
    }
    arena[narena++] = (Block){ g_psp_seg, (uint16_t)(MEM_TOP - g_psp_seg), g_psp_seg };
    arena_sort();

    /* image + relocations, as the DOS loader does */
    memcpy(m + g_load_seg * 16, g_image, g_image_len);
    for (unsigned i = 0; i < g_nrelocs; i++) {
        uint32_t a = (uint32_t)(g_load_seg + g_relocs[i][1]) * 16 + g_relocs[i][0];
        uint16_t w = (uint16_t)((m[a] | m[a + 1] << 8) + g_load_seg);
        m[a] = (uint8_t)w; m[a + 1] = (uint8_t)(w >> 8);
    }

    cpu->cs = (uint16_t)(g_load_seg + g_entry_cs); cpu->ip = g_entry_ip;
    cpu->ss = (uint16_t)(g_load_seg + g_entry_ss); cpu->sp = g_entry_sp;
    cpu->ds = cpu->es = g_psp_seg;
    cpu->flags = 0x0202;
    dta_seg = g_psp_seg; dta_off = 0x80;
}

void dos_exit(int code)
{
    g_exit_code = code;
    longjmp(g_exit_jmp, 1);
}

/* ---- INT 21h ------------------------------------------------------------- */

#define CF_SET(c)   ((c)->flags |= FLAG_CF)
#define CF_CLR(c)   ((c)->flags &= ~FLAG_CF)
static void fail(CPU *c, uint16_t err) { c->ax = err; CF_SET(c); }

void dos_int21(CPU *c)
{
    char name[260];
    uint8_t ah = c->ah;
    if (g_opt.trace) trace("[dos] AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X DS=%04X\n",
                           ah, c->al, c->bx, c->cx, c->dx, c->ds);
    switch (ah) {
    case 0x02: { uint8_t ch = c->dl; con_write(&ch, 1); c->al = ch; break; }
    case 0x06:
        if (c->dl == 0xFF) {
            uint16_t k;
            if (key_wait(1, &k)) { key_wait(0, &k); c->al = (uint8_t)k; c->flags &= ~FLAG_ZF; }
            else { c->al = 0; c->flags |= FLAG_ZF; }
        } else { uint8_t ch = c->dl; con_write(&ch, 1); }
        break;
    case 0x07: case 0x08: { uint16_t k; key_wait(0, &k); c->al = (uint8_t)k; break; }
    case 0x09: {
        uint16_t o = c->dx; uint8_t ch;
        while ((ch = g_cpu.mem[seg_off(c->ds, o++)]) != '$') con_write(&ch, 1);
        c->al = '$'; break;
    }
    case 0x0B: { uint16_t k; c->al = key_wait(1, &k) ? 0xFF : 0; break; }
    case 0x0C: kb_head = kb_tail; if (c->al == 6 || c->al == 7 || c->al == 8) { c->ah = c->al; dos_int21(c); } break;
    case 0x0D: break;
    case 0x0E: c->al = 26; break;
    case 0x19: c->al = 2; break;                      /* C: */
    case 0x1A: dta_seg = c->ds; dta_off = c->dx; break;
    case 0x25: {
        uint32_t a = c->al * 4u;
        g_cpu.mem[a] = (uint8_t)c->dx; g_cpu.mem[a + 1] = (uint8_t)(c->dx >> 8);
        g_cpu.mem[a + 2] = (uint8_t)c->ds; g_cpu.mem[a + 3] = (uint8_t)(c->ds >> 8);
        trace("[dos] set vector %02X -> %04X:%04X\n", c->al, c->ds, c->dx);
        break;
    }
    case 0x2A: { time_t t = time(NULL); struct tm *tm = localtime(&t);
        c->cx = (uint16_t)(tm->tm_year + 1900); c->dh = (uint8_t)(tm->tm_mon + 1);
        c->dl = (uint8_t)tm->tm_mday; c->al = (uint8_t)tm->tm_wday; break; }
    case 0x2C: { time_t t = time(NULL); struct tm *tm = localtime(&t);
        c->ch = (uint8_t)tm->tm_hour; c->cl = (uint8_t)tm->tm_min; c->dh = (uint8_t)tm->tm_sec;
        c->dl = (uint8_t)((host_us() / 10000) % 100); break; }
    case 0x2F: c->es = dta_seg; c->bx = dta_off; break;
    case 0x30: c->ax = 0x0005; c->bx = 0; c->cx = 0; break;   /* DOS 5.0 */
    case 0x33: if (c->al == 0) c->dl = 0; break;
    case 0x35: {
        uint32_t a = c->al * 4u;
        c->bx = (uint16_t)(g_cpu.mem[a] | g_cpu.mem[a + 1] << 8);
        c->es = (uint16_t)(g_cpu.mem[a + 2] | g_cpu.mem[a + 3] << 8);
        break;
    }
    case 0x36:                                       /* free disk space: plenty */
        c->ax = 64; c->bx = 0x7FFF; c->cx = 512; c->dx = 0xFFFF; break;
    case 0x3B: CF_CLR(c); break;                    /* chdir: there is one directory */
    case 0x3C: {
        read_asciiz(c->ds, c->dx, name, sizeof name);
        FILE *f = open_game(name, 1, 1);
        int h = f ? new_handle(f) : -1;
        trace("[dos] create %s -> %d\n", name, h);
        if (h < 0) { fail(c, 3); break; }
        c->ax = (uint16_t)h; CF_CLR(c); break;
    }
    case 0x3D: {
        read_asciiz(c->ds, c->dx, name, sizeof name);
        FILE *f = open_game(name, c->al & 3, 0);
        int h = f ? new_handle(f) : -1;
        trace("[dos] open %s mode %d -> %d\n", name, c->al & 3, h);
        {   /* BSTONE_BT_OPEN=<substring>: who opens this file (debug builds) */
            const char *w = getenv("BSTONE_BT_OPEN");
            void host_backtrace(void);
            if (w && strstr(name, w)) host_backtrace();
        }
        if (h < 0) { fail(c, 2); break; }
        c->ax = (uint16_t)h; CF_CLR(c); break;
    }
    case 0x3E:
        if (c->bx < MAXH && fh[c->bx]) { fclose(fh[c->bx]); fh[c->bx] = NULL; }
        CF_CLR(c); break;
    case 0x3F: {
        uint16_t h = c->bx, n = c->cx;
        if (h == 0) { uint16_t k; key_wait(0, &k); g_cpu.mem[seg_off(c->ds, c->dx)] = (uint8_t)k; c->ax = 1; CF_CLR(c); break; }
        if (h >= MAXH || !fh[h]) { fail(c, 6); break; }
        /* Linear, not wrapped at the segment end: DOS normalises DS:DX to a
         * paragraph before the transfer, so a read that runs past offset FFFF
         * carries on into the next 64K. Wrapping it back to DS:0000 -- as an
         * earlier version of this did -- overwrote whatever sat at the bottom
         * of the segment, which for Planet Strike's loader was its stack. */
        size_t got = fread(&g_cpu.mem[seg_off(c->ds, c->dx)], 1, n, fh[h]);
        c->ax = (uint16_t)got; CF_CLR(c); break;
    }
    case 0x40: {
        uint16_t h = c->bx, n = c->cx;
        if (h == 1 || h == 2) { con_write(&g_cpu.mem[seg_off(c->ds, c->dx)], n); c->ax = n; CF_CLR(c); break; }
        if (h >= MAXH || !fh[h]) { fail(c, 6); break; }
        if (n == 0) {                                /* truncate at the current position */
#ifdef _WIN32
            fflush(fh[h]); _chsize(_fileno(fh[h]), ftell(fh[h]));
#endif
            c->ax = 0; CF_CLR(c); break;
        }
        c->ax = (uint16_t)fwrite(&g_cpu.mem[seg_off(c->ds, c->dx)], 1, n, fh[h]);
        CF_CLR(c); break;
    }
    case 0x41: {
        read_asciiz(c->ds, c->dx, name, sizeof name);
        char sp[512]; join(sp, sizeof sp, g_opt.savedir, base_name(name));
        if (remove(sp) != 0) { fail(c, 2); break; }
        CF_CLR(c); break;
    }
    case 0x42: {
        uint16_t h = c->bx;
        if (h >= MAXH || !fh[h]) { fail(c, 6); break; }
        long off = (long)(int32_t)((uint32_t)c->cx << 16 | c->dx);
        fseek(fh[h], off, c->al == 0 ? SEEK_SET : c->al == 1 ? SEEK_CUR : SEEK_END);
        long p = ftell(fh[h]);
        c->ax = (uint16_t)p; c->dx = (uint16_t)(p >> 16); CF_CLR(c); break;
    }
    case 0x43: {
        read_asciiz(c->ds, c->dx, name, sizeof name);
        FILE *f = open_game(name, 0, 0);
        if (!f) { fail(c, 2); break; }
        fclose(f); c->cx = 0x20; CF_CLR(c); break;
    }
    case 0x44:
        switch (c->al) {
        case 0x00: c->dx = (c->bx < 3) ? 0x80D3 : 0x0002; break;
        case 0x01: break;
        case 0x06: c->al = 0xFF; break;
        case 0x07: c->al = 0xFF; break;
        case 0x08: c->ax = 1; break;
        default: c->ax = 1; CF_SET(c); return;
        }
        CF_CLR(c); break;
    case 0x45: {
        if (c->bx >= MAXH || !fh[c->bx]) { fail(c, 6); break; }
#ifdef _WIN32
        int d = _dup(_fileno(fh[c->bx]));
        FILE *f = d >= 0 ? _fdopen(d, "r+b") : NULL;
#else
        FILE *f = NULL;
#endif
        int h = f ? new_handle(f) : -1;
        if (h < 0) { fail(c, 4); break; }
        c->ax = (uint16_t)h; CF_CLR(c); break;
    }
    case 0x47: g_cpu.mem[seg_off(c->ds, c->si)] = 0; CF_CLR(c); break;
    case 0x48: {
        uint16_t seg;
        if (arena_alloc(c->bx, &seg)) { c->ax = seg; CF_CLR(c); }
        else { c->bx = (uint16_t)largest_free(); fail(c, 8); }
        trace("[dos] alloc %04X paras -> %s %04X\n", c->bx, (c->flags & FLAG_CF) ? "fail" : "ok", c->ax);
        break;
    }
    case 0x49: {
        int i = arena_find(c->es);
        if (i < 0) { fail(c, 9); break; }
        arena[i] = arena[--narena]; arena_sort(); CF_CLR(c); break;
    }
    case 0x4A: {
        int i = arena_find(c->es);
        if (i < 0) { fail(c, 9); break; }
        unsigned max = room_after(i);
        trace("[dos] resize %04X to %04X (max %04X)\n", c->es, c->bx, max);
        if (c->bx > max) { arena[i].paras = (uint16_t)max; c->bx = (uint16_t)max; fail(c, 8); break; }
        arena[i].paras = c->bx; CF_CLR(c); break;
    }
    case 0x4C: dos_exit(c->al); break;
    case 0x4E: {
        read_asciiz(c->ds, c->dx, name, sizeof name);
        find_n = find_i = 0;
        find_scan(g_opt.savedir, base_name(name));
        find_scan(g_opt.datadir, base_name(name));
        trace("[dos] findfirst %s: %d\n", name, find_n);
        if (!find_fill()) { fail(c, 18); break; }
        CF_CLR(c); break;
    }
    case 0x4F: if (!find_fill()) { fail(c, 18); break; } CF_CLR(c); break;
    case 0x51: case 0x62: c->bx = g_psp_seg; break;
    case 0x56: {
        char to[260], a[512], b[512];
        read_asciiz(c->ds, c->dx, name, sizeof name);
        read_asciiz(c->es, c->di, to, sizeof to);
        join(a, sizeof a, g_opt.savedir, base_name(name));
        join(b, sizeof b, g_opt.savedir, base_name(to));
        if (rename(a, b) != 0) { fail(c, 2); break; }
        CF_CLR(c); break;
    }
    case 0x57: if (c->al == 0) { c->cx = 0x6000; c->dx = 0x1B21; } CF_CLR(c); break;
    default:
        fprintf(stderr, "[dos] unhandled INT 21h AH=%02X AL=%02X\n", ah, c->al);
        fail(c, 1);
    }
}

/* ---- INT 10h ------------------------------------------------------------- */

void bios_int10(CPU *c)
{
    switch (c->ah) {
    case 0x00: vga_set_mode(c->al); g_cpu.mem[0x450] = g_cpu.mem[0x451] = 0; break;
    case 0x01: break;
    case 0x02: g_cpu.mem[0x450] = c->dl; g_cpu.mem[0x451] = c->dh; break;
    case 0x03: c->dl = g_cpu.mem[0x450]; c->dh = g_cpu.mem[0x451]; c->cx = 0x0D0E; break;
    case 0x06: case 0x07: {                         /* scroll window (only full clears matter) */
        uint8_t *t = g_cpu.mem + 0xB8000;
        for (int r = c->ch; r <= c->dh && r < 25; r++)
            for (int x = c->cl; x <= c->dl && x < 80; x++) { t[(r * 80 + x) * 2] = ' '; t[(r * 80 + x) * 2 + 1] = c->bh; }
        break;
    }
    case 0x08: {
        uint8_t *t = g_cpu.mem + 0xB8000 + (g_cpu.mem[0x451] * 80 + g_cpu.mem[0x450]) * 2;
        c->al = t[0]; c->ah = t[1]; break;
    }
    case 0x09: case 0x0A: {
        uint8_t *t = g_cpu.mem + 0xB8000 + (g_cpu.mem[0x451] * 80 + g_cpu.mem[0x450]) * 2;
        for (int i = 0; i < c->cx && t < g_cpu.mem + 0xB8000 + 4000; i++, t += 2) {
            t[0] = c->al; if (c->ah == 0x09) t[1] = c->bl;
        }
        break;
    }
    case 0x0E: tty_putc(c->al); break;
    case 0x0F: c->al = (uint8_t)vga_mode(); c->ah = 80; c->bh = 0; break;
    case 0x10:
        if (c->al == 0x10) vga_dac_set(c->bx, c->dh, c->ch, c->cl);
        else if (c->al == 0x12) {
            for (int i = 0; i < c->cx; i++) {
                uint32_t a = seg_off(c->es, (uint16_t)(c->dx + i * 3));
                vga_dac_set(c->bx + i, g_cpu.mem[a], g_cpu.mem[a + 1], g_cpu.mem[a + 2]);
            }
        } else if (c->al == 0x15) { uint8_t r, g, b; vga_dac_get(c->bx, &r, &g, &b); c->dh = r; c->ch = g; c->cl = b; }
        else if (c->al == 0x17) {
            for (int i = 0; i < c->cx; i++) {
                uint32_t a = seg_off(c->es, (uint16_t)(c->dx + i * 3));
                vga_dac_get(c->bx + i, &g_cpu.mem[a], &g_cpu.mem[a + 1], &g_cpu.mem[a + 2]);
            }
        }
        break;
    case 0x11: break;                               /* font loads: text mode only */
    case 0x12: if (c->bl == 0x10) { c->bh = 0; c->bl = 3; c->cx = 0x0009; } break;
    case 0x1A: if (c->al == 0) { c->al = 0x1A; c->bx = 0x0008; } break;   /* VGA, colour */
    default: trace("[bios] INT 10h AH=%02X\n", c->ah);
    }
}

/* ---- INT 16h ------------------------------------------------------------- */

void bios_int16(CPU *c)
{
    uint16_t k;
    switch (c->ah) {
    case 0x00: case 0x10: key_wait(0, &k); c->ax = k; break;
    case 0x01: case 0x11:
        if (key_wait(1, &k)) { c->ax = k; c->flags &= ~FLAG_ZF; }
        else c->flags |= FLAG_ZF;
        recomp_tick(c);
        break;
    case 0x02: c->al = g_cpu.mem[0x417]; break;
    default: break;
    }
}

/* ---- INT 33h mouse ----------------------------------------------------------- */

static int mx = 320, my = 100, mbuttons, mick_x, mick_y, mouse_on;

void mouse_host(int dx, int dy, int buttons)
{
    mick_x += dx; mick_y += dy; mbuttons = buttons;
    mx += dx; my += dy;
    if (mx < 0) mx = 0; if (mx > 639) mx = 639;
    if (my < 0) my = 0; if (my > 199) my = 199;
}

void mouse_int33(CPU *c)
{
    switch (c->ax) {
    case 0x00: mouse_on = !g_opt.headless; c->ax = mouse_on ? 0xFFFF : 0; c->bx = 2; break;
    case 0x03: c->bx = (uint16_t)mbuttons; c->cx = (uint16_t)mx; c->dx = (uint16_t)my; break;
    case 0x04: mx = c->cx; my = c->dx; break;
    case 0x05: case 0x06: c->ax = (uint16_t)mbuttons; c->bx = 0; break;
    case 0x0B: c->cx = (uint16_t)mick_x; c->dx = (uint16_t)mick_y; mick_x = mick_y = 0; break;
    case 0x24: c->bx = 0x0626; c->cx = 0x0400; break;
    default: break;
    }
}

/* ---- everything else ------------------------------------------------------------ */

static const uint8_t scan_ascii[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0,'*', 0,' '};

/* The handlers behind vectors nobody hooked. Returns 0 if there is none. */
int bios_builtin(CPU *c, unsigned n)
{
    switch (n) {
    case 0x08: {                                    /* timer: BDA tick, then INT 1Ch */
        uint32_t t = (uint32_t)(g_cpu.mem[0x46C] | g_cpu.mem[0x46D] << 8 | g_cpu.mem[0x46E] << 16 | (uint32_t)g_cpu.mem[0x46F] << 24) + 1;
        memcpy(&g_cpu.mem[0x46C], &t, 4);
        call_vector(c, 0x1C);
        return 1;
    }
    case 0x09: {                                    /* keyboard: scancode -> BIOS buffer */
        uint8_t sc;
        machine_port_in(0x60, &sc);
        if (!(sc & 0x80) && sc < 0x3A) bios_key_push(sc, scan_ascii[sc]);
        else if (!(sc & 0x80)) bios_key_push(sc, 0);
        return 1;
    }
    case 0x10: bios_int10(c); return 1;
    case 0x11: c->ax = (uint16_t)(g_cpu.mem[0x410] | g_cpu.mem[0x411] << 8); return 1;
    case 0x12: c->ax = 640; return 1;
    case 0x15:
        c->flags |= FLAG_CF; c->ah = 0x86; return 1;  /* no joystick BIOS, no extended services */
    case 0x16: bios_int16(c); return 1;
    case 0x1A:
        if (c->ah == 0) {
            c->dx = (uint16_t)(g_cpu.mem[0x46C] | g_cpu.mem[0x46D] << 8);
            c->cx = (uint16_t)(g_cpu.mem[0x46E] | g_cpu.mem[0x46F] << 8);
            c->al = 0;
        }
        return 1;
    case 0x1B: case 0x1C: case 0x23: case 0x24: return 1;
    case 0x21: dos_int21(c); return 1;
    case 0x2F:                                      /* no XMS, no Windows */
        if (c->ax == 0x4300 || c->ax == 0x1600) c->al = 0;
        return 1;
    case 0x33: mouse_int33(c); return 1;
    case 0x67: c->ah = 0x80; return 1;              /* no EMM loaded */
    }
    if (n < 8 || (n >= 0x70 && n < 0x78) || (n >= 0x0A && n < 0x10)) return 1;   /* spurious IRQs */
    return 0;
}
