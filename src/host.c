/*
 * host.c - the modern machine's side: a Win32 window (GDI, nearest-neighbour,
 * 4:3), waveOut audio, keyboard as raw PC scancodes -- or, with --headless,
 * none of that: frames are composed offscreen and, with --record, piped to
 * ffmpeg with the audio muxed in at the end. Headless never touches the
 * desktop, so it works over RDP and from CI (repo rules, section 13).
 *
 * Scripted input (--keys "ms:KEY,...") presses and releases keys at fixed
 * points in emulated time, which is how the harness walks the menus.
 */
#include "machine.h"
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define FRAME_MAX (3840 * 1200)                 /* 640x200 (21:9 and wider) at --hires 6 */
static uint32_t frame[FRAME_MAX];
static uint32_t big[FRAME_MAX];
static int fw = 320, fh = 200;
static int scan_rows = 200;                     /* the CRT's rows behind the frame */
static int hires_on;
int hires_compose(uint32_t *out, int S, int *w, int *h);
void hires_set_wide(double aspect);
int hires_wide_cols(void);
static double wide_aspect, frame_aspect = 4.0 / 3;   /* the chosen widescreen; this frame's shape */
static int wide_on;

/* What is on screen now: the hi-res renderer's frame when it is on and the
 * game is in its 3D mode, the plain VGA picture otherwise. */
static void compose(void)
{
    if (hires_on && g_opt.hires > 0 && hires_compose(frame, g_opt.hires, &fw, &fh)) {
        scan_rows = 200;
        frame_aspect = 4.0 / 3 * fw / (320.0 * g_opt.hires);   /* 320 columns show at 4:3 */
        return;
    }
    vga_compose(frame, &fw, &fh);
    scan_rows = fh;
    frame_aspect = 4.0 / 3;
}

/* ---- recording ------------------------------------------------------------ */

#define REC_FPS 35
static FILE *rec_video, *rec_audio;
static uint64_t rec_frames, rec_samples;
static char rec_vpath[512], rec_apath[512];

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, rate = AUDIO_RATE, br = AUDIO_RATE * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int rec_w = 640, rec_h = 400;

static void rec_open(void)
{
    if (g_opt.hires > 0) { rec_w = (320 + 2 * hires_wide_cols()) * g_opt.hires; rec_h = 200 * g_opt.hires; }
    snprintf(rec_vpath, sizeof rec_vpath, "%s.video.mp4", g_opt.record);
    snprintf(rec_apath, sizeof rec_apath, "%s.audio.wav", g_opt.record);
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
             "-framerate %d -i - -vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p "
             "-crf 18 \"%s\"", rec_w, rec_h, REC_FPS, rec_w < 1280 ? 1280 : rec_w,
             (rec_w < 1280 ? 1280 : rec_w) * 3 / 4, rec_vpath);
#ifdef _WIN32
    rec_video = _popen(cmd, "wb");
#endif
    if (!rec_video) fatal("cannot start ffmpeg (is it on PATH?)");
    rec_audio = fopen(rec_apath, "wb");
    if (rec_audio) wav_header(rec_audio, 0);
}

static void rec_close(void)
{
    if (!rec_video) return;
#ifdef _WIN32
    _pclose(rec_video);
#endif
    rec_video = NULL;
    if (rec_audio) { wav_header(rec_audio, (uint32_t)rec_samples); fclose(rec_audio); rec_audio = NULL; }
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
             rec_vpath, rec_apath, g_opt.record);
    if (system(cmd) == 0) { remove(rec_vpath); remove(rec_apath); }
    fprintf(stderr, "[rec] %s: %llu frames\n", g_opt.record, (unsigned long long)rec_frames);
}

/* Every frame to the encoder at one size, whatever mode the game is in */
static const uint32_t *as_rec(void)
{
    if (fw == rec_w && fh == rec_h) return frame;
    for (int y = 0; y < rec_h; y++)
        for (int x = 0; x < rec_w; x++)
            big[y * rec_w + x] = frame[(y * fh / rec_h) * fw + x * fw / rec_w];
    return big;
}

static FILE *wav_out;
static uint64_t wav_samples;

void host_snapshot(const char *path);

/* --shot-at "ms:file,...": frames at fixed points in emulated time, for the
 * conformance harness (tools/conformance.py) */
static void shots_due(uint64_t now)
{
    static const char *p;
    static int init;
    if (!init) { init = 1; p = g_opt.shots; }
    while (p && *p) {
        double ms; char path[260]; int n = 0;
        if (sscanf(p, "%lf:%259[^,]%n", &ms, path, &n) < 2) { p = NULL; return; }
        if (now < (uint64_t)(ms * PIT_HZ / 1000.0)) return;
        host_snapshot(path);
        p += n; if (*p == ',') p++;
    }
}

/* ---- scripted keys ----------------------------------------------------------- */

typedef struct { uint64_t at; uint8_t sc; uint8_t up; } KeyEv;
static KeyEv kev[512];
static int nkev, kev_i;

static int key_by_name(const char *s)
{
    static const struct { const char *n; int sc; } t[] = {
        {"ESC",1},{"ENTER",0x1C},{"SPACE",0x39},{"UP",0x48|0x100},{"DOWN",0x50|0x100},
        {"LEFT",0x4B|0x100},{"RIGHT",0x4D|0x100},{"CTRL",0x1D},{"ALT",0x38},{"SHIFT",0x2A},
        {"TAB",0x0F},{"BKSP",0x0E},{"Y",0x15},{"N",0x31},{"F1",0x3B},{"F2",0x3C},{"F3",0x3D},
        {"F10",0x44},{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{NULL,0}};
    for (int i = 0; t[i].n; i++) if (!_stricmp(s, t[i].n)) return t[i].sc;
    return (int)strtol(s, NULL, 16);
}

static void keys_parse(void)
{
    const char *p = g_opt.keys;
    while (p && *p && nkev < 500) {
        char name[32]; double ms; int hold = 120, n = 0;
        if (sscanf(p, "%lf:%31[^,]%n", &ms, name, &n) < 2) break;
        char *h = strchr(name, '+');            /* KEY+holdms */
        if (h) { *h = 0; hold = atoi(h + 1); }
        int sc = key_by_name(name);
        uint64_t at = (uint64_t)(ms * PIT_HZ / 1000.0);
        kev[nkev++] = (KeyEv){ at, (uint8_t)sc, 0 };
        kev[nkev++] = (KeyEv){ at + (uint64_t)(hold * PIT_HZ / 1000.0), (uint8_t)sc, 1 };
        p += n; if (*p == ',') p++;
    }
    /* sort by time (insertion; small) */
    for (int i = 1; i < nkev; i++)
        for (int j = i; j > 0 && kev[j].at < kev[j - 1].at; j--) { KeyEv t = kev[j]; kev[j] = kev[j - 1]; kev[j - 1] = t; }
}

static int ext_of(int sc) { return sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D; }

static void keys_due(void)
{
    while (kev_i < nkev && kev[kev_i].at <= pit_now()) {
        int sc = kev[kev_i].sc;
        if (ext_of(sc)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (kev[kev_i].up ? 0x80 : 0)));
        kev_i++;
    }
}

/* ---- window ---------------------------------------------------------------- */

#ifdef _WIN32
static HWND wnd;
static int quit_req;
static int captured;

static int mouse_buttons_now(void)
{
    return ((GetKeyState(VK_LBUTTON) < 0) ? 1 : 0) | ((GetKeyState(VK_RBUTTON) < 0) ? 2 : 0)
         | ((GetKeyState(VK_MBUTTON) < 0) ? 4 : 0);
}

void host_snapshot(const char *path);

/* F12: the current frame to screenshots\<game>-NNN.bmp */
static void screenshot_key(void)
{
    char p[256];
    CreateDirectoryA("screenshots", NULL);
    for (int n = 0; n < 1000; n++) {
        snprintf(p, sizeof p, "screenshots/%s-%03d.bmp", g_game_id, n);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { host_snapshot(p); return; }
    }
}
static BITMAPINFO bmi;

static void toggle_fullscreen(void)
{
    static WINDOWPLACEMENT prev = { sizeof prev };
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (st & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(wnd, &prev);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &prev);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* Display modes (F11 cycles, --display picks):
 *   sharp  4:3, nearest neighbour -- what a 1993 monitor's geometry was
 *   pixel  the largest whole multiple of the frame, square pixels (16:10)
 *   crt    4:3 with scanlines: every source row lit in its middle and dimmed
 *          at its edges, as an electron beam drew it                       */
static const char *display_names[] = { "sharp", "pixel", "crt" };
static int display_mode;
static uint32_t *crt_buf;
static size_t crt_cap;

static void crt_compose(int w, int h)
{
    if ((size_t)w * h > crt_cap) {
        free(crt_buf);
        crt_cap = (size_t)w * h;
        crt_buf = malloc(crt_cap * 4);
    }
    for (int y = 0; y < h; y++) {
        /* where in its source row this output line falls: 0..255 */
        int pos = (int)(((int64_t)y * scan_rows * 256 / h) & 255);
        int d = pos < 128 ? pos : 255 - pos;          /* 0 at the edges, 127 mid-row */
        int gain = 150 + d * 106 / 127;               /* 150..256: edges ~60% bright */
        const uint32_t *src = frame + (y * fh / h) * fw;
        uint32_t *o = crt_buf + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x * fw / w];
            uint32_t r = ((p >> 16) & 255) * gain >> 8, g = ((p >> 8) & 255) * gain >> 8,
                     b = (p & 255) * gain >> 8;
            o[x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
}

static void paint(HDC dc)
{
    RECT r; GetClientRect(wnd, &r);
    int cw = r.right, ch = r.bottom, w, h;
    if (display_mode == 1) {                          /* pixel: whole multiples */
        int k = cw / fw < ch / fh ? cw / fw : ch / fh;
        if (k < 1) k = 1;
        w = fw * k; h = fh * k;
    } else {                                          /* sharp, crt: 4:3, or the widescreen's shape */
        w = cw; h = (int)(cw / frame_aspect);
        if (h > ch) { h = ch; w = (int)(ch * frame_aspect); }
    }
    int x = (cw - w) / 2, y = (ch - h) / 2;
    PatBlt(dc, 0, 0, cw, y, BLACKNESS); PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
    PatBlt(dc, 0, 0, x, ch, BLACKNESS); PatBlt(dc, x + w, 0, cw - x - w, ch, BLACKNESS);
    if (display_mode == 2 && h >= scan_rows * 2) {
        crt_compose(w, h);
        bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
        SetDIBitsToDevice(dc, x, y, w, h, 0, 0, 0, h, crt_buf, &bmi, DIB_RGB_COLORS);
        return;
    }
    bmi.bmiHeader.biWidth = fw; bmi.bmiHeader.biHeight = -fh;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, fw, fh, frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: case WM_SYSKEYUP: case WM_KEYUP: {
        int up = (m == WM_KEYUP || m == WM_SYSKEYUP);
        if (wp == VK_RETURN && (lp & (1 << 29))) { if (!up) toggle_fullscreen(); return 0; }
        if (wp == VK_F4 && (lp & (1 << 29))) { quit_req = 1; return 0; }
        if (wp == VK_F12) { if (!up) screenshot_key(); return 0; }
        if (wp == VK_F10) {                           /* widescreen -> original -> hi-res 4:3 -> */
            if (!up) {
                if (hires_on && wide_on) { hires_on = 0; wide_on = 0; }
                else if (!hires_on) hires_on = 1;
                else wide_on = 1;
                hires_set_wide(wide_on ? wide_aspect : 0);
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        if (wp == VK_F11) { if (!up) { display_mode = (display_mode + 1) % 3; InvalidateRect(h, NULL, FALSE); } return 0; }
        if (!up && (lp & (1 << 30))) return 0;      /* the game does its own repeat */
        int sc = (lp >> 16) & 0xFF;
        if (lp & (1 << 24)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (up ? 0x80 : 0)));
        return 0;
    }
    case WM_SYSCHAR: return 0;                       /* no menu beep on Alt */

    /* Mouse: click in the window to capture it (the game turns with it when
     * MOUSE ENABLED is on in its options); leaving the window releases it.
     * Raw input, so the motion is the device's, not the cursor's. */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (!captured) {
            RAWINPUTDEVICE rid = { 1, 2, RIDEV_INPUTSINK, h };
            RECT r; GetClientRect(h, &r); MapWindowPoints(h, NULL, (POINT *)&r, 2);
            RegisterRawInputDevices(&rid, 1, sizeof rid);
            ClipCursor(&r); ShowCursor(FALSE); captured = 1;
            return 0;
        }
        /* fall through: a click while captured is a button for the game */
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (captured) mouse_host(0, 0, (int)(((wp & MK_LBUTTON) ? 1 : 0) | ((wp & MK_RBUTTON) ? 2 : 0) | ((wp & MK_MBUTTON) ? 4 : 0)));
        return 0;
    case WM_INPUT: {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (captured && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            mouse_host(ri.data.mouse.lLastX, ri.data.mouse.lLastY, mouse_buttons_now());
        break;
    }
    case WM_KILLFOCUS:
        if (captured) { ClipCursor(NULL); ShowCursor(TRUE); captured = 0; }
        break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* waveOut: a ring the main thread fills and a few buffers cycling under it */
#define WBUF 8
#define WLEN 1024
static HWAVEOUT wout;
static WAVEHDR whdr[WBUF];
static int16_t wdata[WBUF][WLEN * 2];
static int wfill, wcur;

static void wave_open(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, AUDIO_RATE, AUDIO_RATE * 4, 4, 16, 0 };
    if (waveOutOpen(&wout, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wout = NULL; return; }
    for (int i = 0; i < WBUF; i++) {
        whdr[i].lpData = (LPSTR)wdata[i]; whdr[i].dwBufferLength = WLEN * 4;
        waveOutPrepareHeader(wout, &whdr[i], sizeof whdr[i]);
        whdr[i].dwFlags |= WHDR_DONE;
    }
}

static void wave_push(const int16_t *s, int frames)
{
    while (frames > 0) {
        WAVEHDR *h = &whdr[wcur];
        if (!(h->dwFlags & WHDR_DONE)) return;       /* all queued: drop rather than block */
        int n = WLEN - wfill; if (n > frames) n = frames;
        memcpy(&wdata[wcur][wfill * 2], s, n * 4);
        wfill += n; s += n * 2; frames -= n;
        if (wfill == WLEN) {
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(wout, h, sizeof *h);
            wfill = 0; wcur = (wcur + 1) % WBUF;
        }
    }
}
#endif

/* Gamepad: an XInput pad pressed as the keys the game already knows, so it
 * works in the menus and in play with no joystick calibration. The games'
 * own joystick support (port 201h) would want calibrating per session.
 *   stick / d-pad  arrows        RT  Ctrl (fire)     LT  Alt (strafe)
 *   A  Space (use)  X  Enter     RB  Right Shift (run)
 *   B / Start  Esc               Back  Tab                               */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);

static void pad_poll(void)
{
    static XInputGetState_t get;
    static int tried;
    static uint32_t held;
    static const struct { uint32_t bit; uint8_t sc; uint8_t ext; } map[] = {
        {1 << 0, 0x48, 1}, {1 << 1, 0x50, 1}, {1 << 2, 0x4B, 1}, {1 << 3, 0x4D, 1},
        {1 << 4, 0x1D, 0}, {1 << 5, 0x38, 0}, {1 << 6, 0x39, 0}, {1 << 7, 0x1C, 0},
        {1 << 8, 0x36, 0}, {1 << 9, 0x01, 0}, {1 << 10, 0x0F, 0}};
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } s;

    if (!tried) {
        HMODULE m;
        tried = 1;
        m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    if (!get || get(0, &s) != ERROR_SUCCESS) return;

    uint32_t now = 0;
    WORD b = s.buttons;
    if ((b & 0x0001) || s.ly >  12000) now |= 1 << 0;   /* up */
    if ((b & 0x0002) || s.ly < -12000) now |= 1 << 1;   /* down */
    if ((b & 0x0004) || s.lx < -12000) now |= 1 << 2;   /* left */
    if ((b & 0x0008) || s.lx >  12000) now |= 1 << 3;   /* right */
    if (s.rt > 64) now |= 1 << 4;
    if (s.lt > 64) now |= 1 << 5;
    if (b & 0x1000) now |= 1 << 6;                       /* A */
    if (b & 0x4000) now |= 1 << 7;                       /* X */
    if (b & 0x0200) now |= 1 << 8;                       /* RB */
    if (b & (0x2000 | 0x0010)) now |= 1 << 9;            /* B, Start */
    if (b & 0x0020) now |= 1 << 10;                      /* Back */

    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        uint32_t m = map[i].bit;
        if ((now ^ held) & m) {
            if (map[i].ext) kbd_scancode(0xE0);
            kbd_scancode((uint8_t)(map[i].sc | ((now & m) ? 0 : 0x80)));
        }
    }
    held = now;
}

void host_audio(const int16_t *s, int frames)
{
    if (rec_audio) { fwrite(s, 4, frames, rec_audio); rec_samples += frames; }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_samples += frames; }
#ifdef _WIN32
    if (wout) wave_push(s, frames);
#endif
}

void host_init(void)
{
    if (g_opt.hires < 0) g_opt.hires = g_opt.headless ? 0 : 4;   /* on in a window */
    hires_on = g_opt.hires > 0;
    /* widescreen: on in a window unless --widescreen off; "W:H" or a ratio */
    const char *ws = g_opt.wide ? g_opt.wide : g_opt.headless ? "off" : "16:9";
    double aw = 0, ah = 1;
    if (sscanf(ws, "%lf:%lf", &aw, &ah) >= 1 && ah > 0) wide_aspect = aw / ah;
    if (wide_aspect <= 4.0 / 3) wide_aspect = 0;
    wide_on = wide_aspect > 0;
    if (!wide_aspect) wide_aspect = 16.0 / 9;           /* what F10 turns on */
    hires_set_wide(wide_on ? wide_aspect : 0);
    keys_parse();
    if (g_opt.record) rec_open();
    if (g_opt.wav && (wav_out = fopen(g_opt.wav, "wb")) != NULL) wav_header(wav_out, 0);
#ifdef _WIN32
    if (g_opt.headless) return;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "bstone";
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    int s = g_opt.scale ? g_opt.scale : 3;
    RECT r = { 0, 0, wide_on && hires_on ? (int)(240 * s * wide_aspect) : 320 * s, 240 * s };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("bstone", strcmp(g_game_id, "ps") ? "Blake Stone: Aliens of Gold" : "Blake Stone: Planet Strike",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    if (g_opt.fullscreen) toggle_fullscreen();
    for (int i = 0; i < 3; i++)
        if (g_opt.display && !strcmp(g_opt.display, display_names[i])) display_mode = i;
    if (!g_opt.mute) wave_open();
#endif
}

void host_frame(void)
{
    static uint64_t last_present;
    keys_due();
    uint64_t now = pit_now();
    shots_due(now);
    if (g_opt.seconds > 0 && now >= (uint64_t)(g_opt.seconds * PIT_HZ)) dos_exit(0);
    if (rec_video) {
        uint64_t per = (uint64_t)(PIT_HZ / REC_FPS);
        if ((rec_frames + 1) * per <= now) {
            compose();
            const uint32_t *f = as_rec();
            while ((rec_frames + 1) * per <= now) { fwrite(f, 4, (size_t)rec_w * rec_h, rec_video); rec_frames++; }
        }
    }
#ifdef _WIN32
    if (!wnd) return;
    if (now - last_present < (uint64_t)(PIT_HZ / 70)) return;
    last_present = now;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (GetForegroundWindow() == wnd) pad_poll();
    if (quit_req) dos_exit(0);
    compose();
    HDC dc = GetDC(wnd); paint(dc); ReleaseDC(wnd, dc);
#endif
}

/* The last thing on screen, as a BMP -- a run nobody watched still says where
 * it got to. */
void host_snapshot(const char *path)
{
    compose();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = 54 + fw * fh * 4, off = 54, hs = 40, zero = 0, img = fw * fh * 4;
    int32_t w = fw, h = -fh; uint16_t planes = 1, bpp = 32;
    fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&hs, 4, 1, f); fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f); fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f); fwrite(&zero, 4, 1, f); fwrite(&img, 4, 1, f);
    fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f);
    fwrite(frame, 4, fw * fh, f);
    fclose(f);
}

void host_shutdown(void)
{
    rec_close();
    if (wav_out) { wav_header(wav_out, (uint32_t)wav_samples); fclose(wav_out); wav_out = NULL; }
#ifdef _WIN32
    if (wout) { waveOutReset(wout); waveOutClose(wout); }
    if (wnd) DestroyWindow(wnd);
#endif
}
