/*
 * hires.c - the 3D view's walls redrawn at high resolution.
 *
 * The original keeps running and drawing exactly as it did: game logic, HUD,
 * sprites, floor and ceiling are all still the lifted 1993 code. Two hooks
 * tools/lift.py wraps around it supply what this needs:
 *
 *   hires_hit()   after each of the raycaster's Hit* routines: which texture
 *                 page and column this screen column shows, the wall height,
 *                 and the exact intercept -- the game's own answer to "what
 *                 is in this column", including doors and pushwalls;
 *   g_draw_tag    set while the wall scalers or the floor/ceiling drawer run,
 *                 so vga.c knows who wrote every pixel.
 *
 * At present time the compositor upsamples: each output column sits between
 * two of the game's columns, and where both hit the same texture page its
 * texture coordinate and height are interpolated, so walls are sampled from
 * the 64x64 textures at full output resolution with continuous shading.
 * Layering is by owner: pixels the sprite and weapon code wrote stay on top,
 * hi-res walls come next, and the original floor and ceiling fill the rest.
 *
 * Every DGROUP offset used here is read out of the game's own code at lift
 * time (find_renderer in tools/lift.py), so both games -- and, in principle,
 * other versions -- work without an address table. See docs/renderer.md.
 */
#include "machine.h"

int g_draw_tag;

typedef struct {
    uint16_t seg, off;          /* the texture column: postsource */
    uint16_t wh;                /* wallheight[]: 8x the half-height in pixels */
    uint16_t yi, xi;            /* the intercepts' fractions, for sub-texel */
    uint8_t ok;
} Col;

typedef struct {
    int light, shademax, normalshade, centery;
    uint16_t ls_seg, ls_off;
} Frame;

static Col cur[320], shown[320];
#define WALLH_MAX (320 * 6)
static double wallh[WALLH_MAX];          /* hi-res wall height (wallheight units) per output column */
static Frame fcur, fshown;
static int dirty, have;

static inline uint16_t rd16(uint16_t seg, uint16_t off) { return mem_read16(&g_cpu, seg, off); }

void hires_hit(CPU *c)
{
    const HiresVars *h = &g_hires;
    if (!h->ok) return;
    uint16_t ds = c->ds, x = rd16(ds, h->pixx);
    if (x >= 320) return;
    Col *k = &cur[x];
    k->seg = rd16(ds, h->postseg);
    k->off = rd16(ds, h->postoff);
    k->wh = rd16(ds, (uint16_t)(h->wallheight + 2 * x));
    k->yi = rd16(ds, h->yint);
    k->xi = rd16(ds, h->xint);
    k->ok = 1;
    fcur.light = (rd16(ds, h->lightflag) & 0x800) != 0;
    fcur.shademax = (int16_t)rd16(ds, h->shademax);
    fcur.normalshade = (int16_t)rd16(ds, h->normalshade);
    fcur.ls_seg = rd16(ds, h->ls_seg);
    fcur.ls_off = rd16(ds, h->ls_off);
    fcur.centery = rd16(ds, h->centery);
    dirty = 1;
}

/* ---- floor and ceiling --------------------------------------------------
 *
 * Each span-drawer call draws one VGA plane of one screen row, every fourth
 * pixel: the ceiling row at DI and its mirror, the floor row, at BP+DI. The
 * texture coordinates are packed in ESI (U in the high word, V in the low,
 * 6.10 fixed point each) and step by EDX every four pixels. A captured span
 * becomes, per row, U and V as linear functions of x -- which can then be
 * evaluated at any output pixel, not just every fourth one. */
typedef struct {
    uint16_t di, bp, sh;        /* destination, mirror distance, shade row */
    uint8_t plane, what;
    int32_t u, v, du, dv;       /* at the span start; per four pixels */
} Span;

typedef struct {
    uint8_t ok, byte, shaded;   /* byte: 0 ceiling texel, 1 floor texel */
    uint16_t sh;
    double u0, ux, v0, vx;      /* u(x) = u0 + ux*x */
} RowFit;

#define MAXSPAN 4096
static Span spans[MAXSPAN];
static int nspans;
static RowFit rows[200];
static uint16_t sh_seg_cur;

void hires_plane(CPU *c, int what)
{
    const HiresVars *h = &g_hires;
    if (!h->ok || nspans >= MAXSPAN) return;
    uint16_t ds = c->ds;
    Span *s = &spans[nspans++];
    int mask = vga_map_mask();
    s->plane = (uint8_t)(mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : 3);
    s->what = (uint8_t)what;
    s->di = rd16(ds, h->pl_di);
    s->bp = rd16(ds, h->pl_bp);
    s->u = rd16(ds, h->pl_sih); s->v = rd16(ds, h->pl_sil);
    s->du = (int16_t)rd16(ds, h->pl_dxh); s->dv = (int16_t)rd16(ds, h->pl_dxl);
    s->sh = (what & 4) && h->pl_shoff ? rd16(ds, h->pl_shoff) : 0;
    if ((what & 4) && h->pl_shseg) sh_seg_cur = rd16(ds, h->pl_shseg);
}

static uint16_t sh_seg;          /* the shading tables' segment, this frame */

/* At a flip the spans can be placed: the new start address is the page
 * they were drawn into. */
static void fit_rows(unsigned start, unsigned rowb)
{
    memset(rows, 0, sizeof rows);
    for (int i = 0; i < nspans; i++) {
        const Span *s = &spans[i];
        for (int half = 0; half < 2; half++) {
            if (!(s->what & (half ? 2 : 1))) continue;
            unsigned rel = (unsigned)((half ? s->di + s->bp : s->di) - start) & 0xFFFF;
            unsigned y = rel / rowb, xs = (rel % rowb) * 4 + s->plane;
            if (y >= 200 || xs >= 320) continue;
            RowFit *r = &rows[y];
            r->ok = 1; r->byte = (uint8_t)half; r->shaded = (s->what & 4) != 0; r->sh = s->sh;
            r->ux = s->du / 4.0; r->vx = s->dv / 4.0;
            r->u0 = s->u - r->ux * xs; r->v0 = s->v - r->vx * xs;
        }
    }
    nspans = 0;
}

/* The plane texel at a continuous screen position, or -1. */
static int plane_texel(double fx, double fy)
{
    const HiresVars *h = &g_hires;
    int y0 = (int)floor(fy);
    double t = fy - y0;
    const RowFit *a = (y0 >= 0 && y0 < 200 && rows[y0].ok) ? &rows[y0] : NULL;
    const RowFit *b = (y0 + 1 < 200 && y0 + 1 >= 0 && rows[y0 + 1].ok) ? &rows[y0 + 1] : NULL;
    if (!a && !b) return -1;
    if (!a || (b && a->byte != b->byte)) { if (!a || t >= 0.5) { a = b; } b = NULL; }
    double u = a->u0 + a->ux * fx, v = a->v0 + a->vx * fx;
    if (b) {
        u += (b->u0 + b->ux * fx - u) * t;
        v += (b->v0 + b->vx * fx - v) * t;
    }
    unsigned ui = ((unsigned)(int64_t)floor(u) >> 10) & 63, vi = ((unsigned)(int64_t)floor(v) >> 10) & 63;
    unsigned idx = vi * 64 + ui;
    int texel = g_cpu.mem[((uint32_t)h->pl_texseg * 16 + idx * 2 + a->byte) & 0xFFFFF];
    if (a->shaded && sh_seg)
        texel = g_cpu.mem[((uint32_t)sh_seg * 16 + ((a->sh & 0xFF00) | (unsigned)texel)) & 0xFFFFF];
    return texel;
}

/* ---- sprites ---------------------------------------------------------------
 *
 * Actors, objects and the weapon are drawn a screen column at a time: the
 * per-column routine gets the column's height (wallheight units) and walks the
 * column's post list -- {end*2, source, start*2} words, terminated by 0 -- in
 * the sprite's page, whose t_compshape header {leftpix, rightpix, dataofs[]}
 * says which texel column that list belongs to. Capturing each column gives
 * the sprite back: its page, its height and, by fitting the texel column of
 * every captured screen column, its exact centre. Then it can be drawn at any
 * resolution from its own posts, occluded by the hi-res walls the same way the
 * game occludes it (a column is hidden where the wall is taller). */
typedef struct {
    uint16_t x, height, seg, off, shseg, shbase;
    uint8_t shaded, w;          /* w: pixels the map mask covers */
} SCol;

typedef struct {
    uint16_t seg, height, shseg, shbase;
    uint8_t shaded;
    double xc;                  /* the centre, in screen pixels */
    int x0, x1;                 /* the screen columns the game drew */
} Sprite;

#define MAXSCOL 4096
#define MAXSPR 256
static SCol scols[MAXSCOL];
static int nscols;
static Sprite sprs[MAXSPR], sprs_shown[MAXSPR];
static int nsprs, nsprs_shown;

void hires_sprite_col(CPU *c, int shaded)
{
    const HiresVars *h = &g_hires;
    if (!h->ok || !h->sp_cmdseg || nscols >= MAXSCOL) return;
    uint16_t ds = c->ds;
    SCol *k = &scols[nscols++];
    int mask = vga_map_mask();
    int plane = mask & 1 ? 0 : mask & 2 ? 1 : mask & 4 ? 2 : 3, w = 0;
    while (plane + w < 4 && mask >> (plane + w) & 1) w++;   /* one call, several pixels */
    k->w = (uint8_t)(w ? w : 1);
    k->height = rd16(c->ss, (uint16_t)(c->sp + 4));    /* past the far return address */
    k->x = (uint16_t)(rd16(c->ss, (uint16_t)(c->sp + 6)) * 4 + plane);   /* resolved at the flip */
    k->seg = rd16(ds, h->sp_cmdseg);
    k->off = rd16(ds, h->sp_cmdoff);
    k->shaded = (uint8_t)shaded;
    k->shseg = shaded ? rd16(ds, h->sp_shseg) : 0;
    k->shbase = shaded ? (uint16_t)(rd16(ds, h->sp_shoff) & 0xFF00) : 0;
}

static uint16_t seg_word(uint16_t seg, unsigned off) { return (uint16_t)(g_cpu.mem[(seg * 16u + off) & 0xFFFFF] | g_cpu.mem[(seg * 16u + off + 1) & 0xFFFFF] << 8); }

/* Which texel column of its shape a post list is. */
static int shape_column(uint16_t seg, uint16_t off)
{
    int left = seg_word(seg, 0), right = seg_word(seg, 2);
    if (left < 0 || right > 63 || left > right) return -1;
    for (int k = 0; k <= right - left; k++)
        if (seg_word(seg, 4 + 2 * k) == off) return left + k;
    return -1;
}

/* Group the frame's columns into sprites and fit each one's centre. A pixel
 * centre p shows texel column c when xc + (c-32)k <= p < xc + (c-31)k, with
 * k = height/256 pixels per texel; every column narrows the interval. */
static int cmp_px(const void *a, const void *b) { return ((const int *)a)[0] - ((const int *)b)[0]; }

/* One sprite from columns already known to belong together: fit its centre.
 * A texel is height/64 pixels wide (the art is square, so the sprite's half
 * height is height/2), and pixel centre p shows texel column c when
 * xc + (c-32)k <= p < xc + (c-31)k. Every column narrows the interval. */
static void fit_one(const int (*pc)[3], int n, const SCol *proto)
{
    if (nsprs >= MAXSPR || n <= 0) return;
    double k = proto->height / 64.0, lo = -1e9, hi = 1e9;
    for (int m = 0; m < n; m++) {
        /* every pixel centre of the span shows texel column c */
        double a = pc[m][0] + pc[m][2] - 0.5 - (pc[m][1] - 31) * k;
        double b = pc[m][0] + 0.5 - (pc[m][1] - 32) * k;
        if (a > lo) lo = a;
        if (b < hi) hi = b;
    }
    Sprite *sp = &sprs[nsprs++];
    sp->seg = proto->seg; sp->height = proto->height;
    sp->shaded = proto->shaded; sp->shseg = proto->shseg; sp->shbase = proto->shbase;
    sp->x0 = pc[0][0]; sp->x1 = pc[n - 1][0] + pc[n - 1][2] - 1;
    sp->xc = lo <= hi ? (lo + hi) / 2 : (sp->x0 + sp->x1 + 1) / 2.0;
    if (getenv("BSTONE_SPRITE_DEBUG"))
        fprintf(stderr, "[spr] seg=%04X h=%u cols=%d x=%d..%d xc=%.2f [%g,%g]\n",
                sp->seg, sp->height, n, sp->x0, sp->x1, sp->xc, lo, hi);
}

/* Group the frame's columns into sprites: consecutive captures of the same
 * shape at the same height, split wherever the screen columns are not
 * contiguous (two copies of one object at one distance). */
static void fit_sprites(unsigned start)
{
    static int pc[MAXSCOL][3];
    nsprs = 0;
    for (int i = 0; i < nscols; ) {
        int j = i;
        while (j < nscols && scols[j].seg == scols[i].seg && scols[j].height == scols[i].height) j++;
        int n = 0;
        for (int m = i; m < j; m++) {
            int c = shape_column(scols[m].seg, scols[m].off);
            if (c < 0) continue;
            pc[n][0] = (int)((((unsigned)(scols[m].x - start * 4)) & 0x3FFFF) % 320);
            pc[n][1] = c;
            pc[n][2] = scols[m].w;
            n++;
        }
        qsort(pc, n, sizeof pc[0], cmp_px);
        int a = 0;
        for (int m = 1; m <= n; m++)
            if (m == n || pc[m][0] > pc[m - 1][0] + pc[m - 1][2]) {
                fit_one((const int (*)[3])&pc[a], m - a, &scols[i]);
                a = m;
            }
        i = j;
    }
    nscols = 0;
}

/* The game flips pages by moving the CRTC start address once a frame is
 * drawn; the columns captured since the last flip belong to that frame. */
void hires_flip(void)
{
    { static int n; if (getenv("BSTONE_HIRES_DEBUG") && n++ < 12)
        fprintf(stderr, "[hires] flip start=%04X dirty=%d spans=%d\n", vga_scan_start(), dirty, nspans); }
    if (!dirty && !nspans && !nscols) return;
    memcpy(shown, cur, sizeof shown);
    fshown = fcur;
    memset(cur, 0, sizeof cur);
    sh_seg = sh_seg_cur;
    fit_rows(vga_scan_start(), vga_row_bytes());
    fit_sprites(vga_scan_start());
    memcpy(sprs_shown, sprs, sizeof(Sprite) * nsprs);
    nsprs_shown = nsprs;
    dirty = 0;
    have = 1;
}

/* A column's texture coordinate in texels from the start of its segment:
 * the column the game chose, plus where in that texel the ray landed. The
 * fraction comes from whichever intercept agrees with the chosen column
 * (vertical walls use y, horizontal x; textures facing the other way run
 * backwards); a door's texture slides, so nothing agrees and it gets the
 * texel's middle. */
static double texcoord(const Col *k)
{
    int c = (k->off >> 6) & 63;
    uint16_t src[2] = { k->yi, k->xi };
    for (int i = 0; i < 2; i++) {
        int cc = (src[i] >> 10) & 63;
        double f = (src[i] & 0x3FF) / 1024.0;
        if (cc == c) return (k->off >> 6) + f;
        if (63 - cc == c) return (k->off >> 6) + 1.0 - f;
    }
    return (k->off >> 6) + 0.5;
}

static int scale_of(int S) { return S < 1 ? 1 : S > 6 ? 6 : S; }

/* The whole screen at S x 320 by S x 200, hi-res walls in the 3D view. */
static int compose_inner(uint32_t *out, int S, int *w, int *h);
static uint64_t c_us, c_n;
void hires_report(void) { if (c_n) fprintf(stderr, "[hires] %llu frames, %.2f ms each\n", (unsigned long long)c_n, c_us / 1000.0 / c_n); }

int hires_compose(uint32_t *out, int S, int *w, int *h)
{
    uint64_t t = host_us();
    int r = compose_inner(out, S, w, h);
    c_us += host_us() - t; c_n++;
    return r;
}

static int compose_inner(uint32_t *out, int S, int *w, int *h)
{
    S = scale_of(S);
    if (!vga_unchained()) return 0;
    const uint8_t *vram = vga_vram(), *own = vga_owner();
    const uint8_t *mem = g_cpu.mem;
    unsigned start = vga_scan_start(), row = vga_row_bytes();
    static uint8_t px[200][320], ow[200][320];
    int vx0 = 320, vx1 = -1, vy0 = 200, vy1 = -1;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            unsigned a = ((((start + y * row + (x >> 2)) & 0xFFFF) << 2) | (x & 3));
            px[y][x] = vram[a];
            ow[y][x] = own[a];
            if (own[a] == DRAW_WALL || own[a] == DRAW_PLANE) {
                if (x < vx0) vx0 = x; if (x > vx1) vx1 = x;
                if (y < vy0) vy0 = y; if (y > vy1) vy1 = y;
            }
        }
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = vga_color(i);
    const int W = 320 * S;
    *w = W; *h = 200 * S;

    /* the original, enlarged: one output row per source row, then copied */
    for (int y = 0; y < 200; y++) {
        uint32_t *o = out + (size_t)y * S * W;
        for (int x = 0; x < 320; x++) {
            uint32_t c = pal[px[y][x]];
            for (int k = 0; k < S; k++) o[x * S + k] = c;
        }
        for (int r = 1; r < S; r++) memcpy(o + (size_t)r * W, o, (size_t)W * 4);
    }
    if (!have || vx1 < vx0 || vx1 - vx0 + 1 > 320) return 1;

    /* floor and ceiling, row by row: each output row's texture fit is fixed,
     * so u and v step by a constant along it (16.16 fixed point) */
    const HiresVars *hv = &g_hires;
    for (int Y = vy0 * S; Y < (vy1 + 1) * S; Y++) {
        double fy = (Y + 0.5) / S - 0.5;
        int y0 = (int)floor(fy);
        double t = fy - y0;
        const RowFit *a = (y0 >= 0 && y0 < 200 && rows[y0].ok) ? &rows[y0] : NULL;
        const RowFit *b = (y0 + 1 < 200 && rows[y0 + 1].ok) ? &rows[y0 + 1] : NULL;
        if (!a && !b) continue;
        if (!a || (b && a->byte != b->byte)) { if (!a || t >= 0.5) a = b; b = NULL; }
        double u0 = a->u0, ux = a->ux, v0 = a->v0, vx = a->vx;
        if (b) {
            u0 += (b->u0 - u0) * t; ux += (b->ux - ux) * t;
            v0 += (b->v0 - v0) * t; vx += (b->vx - vx) * t;
        }
        /* at output column X the source position is (X + 0.5)/S - 0.5 */
        double fx0 = (vx0 * S + 0.5) / S - 0.5;
        int64_t U = (int64_t)((u0 + ux * fx0) * 65536.0), dU = (int64_t)(ux / S * 65536.0);
        int64_t V = (int64_t)((v0 + vx * fx0) * 65536.0), dV = (int64_t)(vx / S * 65536.0);
        uint32_t tex = (uint32_t)hv->pl_texseg * 16 + a->byte;
        uint32_t shade = (a->shaded && sh_seg) ? (uint32_t)sh_seg * 16 + (a->sh & 0xFF00) : 0;
        const uint8_t *owr = ow[Y / S];
        uint32_t *o = out + (size_t)Y * W + vx0 * S;
        for (int x = vx0; x <= vx1; x++) {
            int mine = owr[x] == DRAW_WALL || owr[x] == DRAW_PLANE || (owr[x] == DRAW_SPRITE && nsprs_shown);
            for (int k = 0; k < S; k++, o++, U += dU, V += dV) {
                if (!mine) continue;
                unsigned ui = (unsigned)(U >> 26) & 63, vi = (unsigned)(V >> 26) & 63;
                unsigned texel = mem[(tex + (vi * 64 + ui) * 2) & 0xFFFFF];
                if (shade) texel = mem[(shade + texel) & 0xFFFFF];
                *o = pal[texel];
            }
        }
    }

    /* walls, column by column, over the planes */
    const Frame *f = &fshown;
    int ncol = vx1 - vx0 + 1;
    double yc = (double)(vy0 + f->centery) * S;               /* the horizon, in output rows */
    for (int i = 0; i < WALLH_MAX; i++) wallh[i] = 0;
    for (int X = vx0 * S; X < (vx1 + 1) * S; X++) {
        double u = (X + 0.5) / S - 0.5 - vx0;
        int c0 = (int)floor(u);
        double t = u - c0;
        if (c0 < 0) { c0 = 0; t = 0; }
        if (c0 >= ncol - 1) { c0 = ncol - 1; t = 0; }
        const Col *a = &shown[c0], *b = &shown[c0 + (c0 + 1 < ncol)];
        if (!a->ok) continue;
        double tc = texcoord(a), hh = a->wh / 8.0;
        uint16_t seg = a->seg;
        if (b->ok && b != a) {
            double tb = texcoord(b);
            if (b->seg == a->seg && fabs(tb - tc) < 2.0) {     /* same surface: interpolate */
                tc += (tb - tc) * t;
                hh += (b->wh / 8.0 - hh) * t;
            } else if (t >= 0.5) {                            /* an edge: nearest */
                tc = tb; hh = b->wh / 8.0; seg = b->seg;
            }
        }
        if (X - vx0 * S < WALLH_MAX) wallh[X - vx0 * S] = hh * 8;
        if (hh <= 0) continue;
        /* The game's own column choice is ground truth: the sub-texel fraction
         * may move between the two neighbours' columns but never past them --
         * at 63.99 + a fraction it would read the next column in memory, and
         * that showed as a dashed line down the seam. */
        int col = (int)floor(tc), ca = a->off >> 6, cb = b->off >> 6;
        int lo = seg == b->seg && b->ok ? (ca < cb ? ca : cb) : (seg == a->seg ? ca : cb);
        int hi = seg == b->seg && b->ok ? (ca < cb ? cb : ca) : lo;
        if (seg != a->seg) lo = hi = cb;
        if (col < lo) col = lo; if (col > hi) col = hi;
        uint32_t tex = (uint32_t)seg * 16 + (uint32_t)col * 64;
        uint32_t shade = 0;
        if (f->light && f->normalshade) {
            int i = f->shademax - (int)(63.0 * hh / f->normalshade);
            if (i < 0) i = 0; if (i > 63) i = 63;
            shade = (uint32_t)f->ls_seg * 16 + f->ls_off + (uint32_t)i * 256;
        }
        /* rows the wall covers: |(Y + 0.5 - yc) / S| < hh */
        int y_lo = (int)ceil(yc - hh * S - 0.5), y_hi = (int)floor(yc + hh * S - 0.5);
        if (y_lo < vy0 * S) y_lo = vy0 * S;
        if (y_hi > (vy1 + 1) * S - 1) y_hi = (vy1 + 1) * S - 1;
        double r0 = 32.0 + (y_lo + 0.5 - yc) / S / hh * 32.0, dr = 32.0 / (S * hh);
        int64_t R = (int64_t)(r0 * 65536.0), dR = (int64_t)(dr * 65536.0);
        int x = X / S;
        uint32_t *o = out + (size_t)y_lo * W + X;
        for (int Y = y_lo; Y <= y_hi; Y++, o += W, R += dR) {
            uint8_t ox = ow[Y / S][x];
            if (ox != DRAW_WALL && ox != DRAW_PLANE && !(ox == DRAW_SPRITE && nsprs_shown)) continue;
            int r = (int)(R >> 16);
            if (r < 0) r = 0; if (r > 63) r = 63;
            unsigned texel = mem[(tex + r) & 0xFFFFF];
            if (shade) texel = mem[(shade + texel) & 0xFFFFF];
            *o = pal[texel];
        }
    }
    /* sprites, back to front as the game drew them, over everything but the
     * overlays the game put on top of them (text, the fizzle) */
    for (int si = 0; si < nsprs_shown; si++) {
        const Sprite *sp = &sprs_shown[si];
        double k = sp->height / 64.0, hh = sp->height / 2.0;   /* texel width; half height */
        if (k <= 0) continue;
        int left = seg_word(sp->seg, 0), right = seg_word(sp->seg, 2);
        double xa = sp->xc + (left - 32) * k, xb = sp->xc + (right + 1 - 32) * k;
        int X0 = (int)floor(xa * S), X1 = (int)ceil(xb * S);
        if (X0 < vx0 * S) X0 = vx0 * S;
        if (X1 > (vx1 + 1) * S) X1 = (vx1 + 1) * S;
        uint32_t shade = sp->shaded ? (uint32_t)sp->shseg * 16 + sp->shbase : 0;
        for (int X = X0; X < X1; X++) {
            if (X - vx0 * S < WALLH_MAX && wallh[X - vx0 * S] > sp->height * 4.0) continue;   /* behind a wall (wall units) */
            int c = (int)floor(((X + 0.5) / S - sp->xc) / k) + 32;
            if (c < left || c > right) continue;
            unsigned cmd = seg_word(sp->seg, 4 + 2 * (c - left));
            int x = X / S;
            for (int guard = 0; guard < 64; guard++, cmd += 6) {
                int end = seg_word(sp->seg, cmd) >> 1;
                if (!end) break;
                int src = seg_word(sp->seg, cmd + 2), st = seg_word(sp->seg, cmd + 4) >> 1;
                int Ya = (int)ceil(yc + (st - 32) / 32.0 * hh * S - 0.5);
                int Yb = (int)ceil(yc + (end - 32) / 32.0 * hh * S - 0.5);
                if (Ya < vy0 * S) Ya = vy0 * S;
                if (Yb > (vy1 + 1) * S) Yb = (vy1 + 1) * S;
                for (int Y = Ya; Y < Yb; Y++) {
                    uint8_t ox = ow[Y / S][x];
                    if (ox == DRAW_OTHER) continue;
                    int r = (int)floor(32.0 + (Y + 0.5 - yc) / S / hh * 32.0);
                    if (r < st) r = st;
                    if (r >= end) r = end - 1;
                    unsigned texel = mem[((uint32_t)sp->seg * 16 + (uint16_t)(src + r)) & 0xFFFFF];
                    if (shade) texel = mem[(shade + texel) & 0xFFFFF];
                    out[(size_t)Y * W + X] = pal[texel];
                }
            }
        }
    }
    return 1;
}
