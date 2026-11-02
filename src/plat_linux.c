#ifndef SHIYING_SESSION_C
#error "plat_linux.c is included by session.c only"
#endif

#include "audio.h"
#include "path.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pulse/pulseaudio.h>

static int ctl_fd = -1;

static int plat_audio_pipe(void) { return 1; }
static int plat_ts_wall(void) { return 1; }
static int plat_cursor_mag(void);
static int plat_audio_ok(void) { return audio_available(); }
static void plat_take_begin(SySession *s) { (void)s; }
static int plat_fallback(SySession *s, uint64_t sz) { (void)s; (void)sz; return 0; }

static void plat_append_video(SySession *s, ArgBuf *a) {
    int draw = s->plan.cursor == SY_CURSOR_ORIG;
    ap(a, "-f"); ap(a, "x11grab");
    ap(a, "-framerate"); apf(a, "%d", s->plan.fps);
    ap(a, "-draw_mouse"); ap(a, draw ? "1" : "0");
    ap(a, "-video_size"); apf(a, "%dx%d", s->plan.w, s->plan.h);
    ap(a, "-i"); apf(a, "%s+%d,%d", s->plan.display[0] ? s->plan.display : getenv("DISPLAY") ? getenv("DISPLAY") : ":0",
                     s->plan.x, s->plan.y);
}

static int plat_spawn(char **argv, int aud_fd, int take_ctl) {
    int cfd[2] = { -1, -1 };
    if (take_ctl && pipe2(cfd, O_CLOEXEC) != 0) return -1;
    char lp[1200];
    snprintf(lp, sizeof lp, "%s/ffmpeg.log", g_cache_dir);
    pid_t pid = fork();
    if (pid < 0) {
        if (take_ctl) { close(cfd[0]); close(cfd[1]); }
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        /* 父进程被 kill -9 时内核代发 SIGKILL。已改名的成品不在这个进程里。 */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1) _exit(1);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        int nul = open("/dev/null", O_RDWR);
        int in = nul;
        if (take_ctl) {
            int hi = fcntl(cfd[0], F_DUPFD, 20);
            if (hi >= 0) in = hi;
        }
        int hi_au = aud_fd >= 0 ? fcntl(aud_fd, F_DUPFD, 20) : -1;
        dup2(in, 0);
        dup2(nul, 1);
        int lf = open(lp, O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(lf >= 0 ? lf : nul, 2);
        if (hi_au >= 0) dup2(hi_au, 3);
        execv(argv[0], argv);
        _exit(127);
    }
    if (take_ctl) {
        close(cfd[0]);
        /* 合并进程不接管这条管道，否则会关掉仍在录的那一路 q。 */
        if (ctl_fd >= 0) close(ctl_fd);
        ctl_fd = cfd[1];
    }
    return (int)pid;
}

static int plat_reap(int pid, int *code) {
    if (pid <= 0) return 0;
    int st;
    pid_t r = waitpid((pid_t)pid, &st, WNOHANG);
    if (r == 0) return 0;
    if (r < 0) {
        if (code) *code = -1;
        return 1;
    }
    if (code) *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return 1;
}

static void plat_kill(int pid) {
    if (pid > 0) kill((pid_t)pid, SIGKILL);
}

static int plat_send_q(void) {
    int ok = -1;
    if (ctl_fd >= 0) {
        if (write(ctl_fd, "q", 1) == 1) ok = 0;
        close(ctl_fd);
        ctl_fd = -1;
    }
    return ok;
}

static int dev_n;
static SyDevice *dev_out;
static int dev_cap;

static void on_source(pa_context *c, const pa_source_info *i, int eol, void *ud) {
    (void)c; (void)ud;
    if (eol || !i || dev_n >= dev_cap) return;
    snprintf(dev_out[dev_n].id, sizeof dev_out[dev_n].id, "%s", i->name ? i->name : "");
    snprintf(dev_out[dev_n].label, sizeof dev_out[dev_n].label, "%s",
             i->description && i->description[0] ? i->description : dev_out[dev_n].id);
    if (dev_out[dev_n].id[0]) dev_n++;
}

static int plat_devices(SyDevice *out, int cap) {
    if (!out || cap <= 0 || !audio_available()) return 0;
    static pa_mainloop *(*ml_new)(void);
    static void (*ml_free)(pa_mainloop *);
    static pa_mainloop_api *(*ml_api)(pa_mainloop *);
    static int (*ml_iter)(pa_mainloop *, int, int *);
    static pa_context *(*ctx_new)(pa_mainloop_api *, const char *);
    static void (*ctx_unref)(pa_context *);
    static int (*ctx_connect)(pa_context *, const char *, pa_context_flags_t, const pa_spawn_api *);
    static void (*ctx_disc)(pa_context *);
    static pa_context_state_t (*ctx_state)(const pa_context *);
    static pa_operation *(*get_src)(pa_context *, pa_source_info_cb_t, void *);
    static void (*op_unref)(pa_operation *);
    static pa_operation_state_t (*op_state)(const pa_operation *);
    static int ready;
    if (!ready) {
        void *h = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) return 0;
        ml_new = dlsym(h, "pa_mainloop_new");
        ml_free = dlsym(h, "pa_mainloop_free");
        ml_api = dlsym(h, "pa_mainloop_get_api");
        ml_iter = dlsym(h, "pa_mainloop_iterate");
        ctx_new = dlsym(h, "pa_context_new");
        ctx_unref = dlsym(h, "pa_context_unref");
        ctx_connect = dlsym(h, "pa_context_connect");
        ctx_disc = dlsym(h, "pa_context_disconnect");
        ctx_state = dlsym(h, "pa_context_get_state");
        get_src = dlsym(h, "pa_context_get_source_info_list");
        op_unref = dlsym(h, "pa_operation_unref");
        op_state = dlsym(h, "pa_operation_get_state");
        ready = ml_new && ml_free && ml_api && ml_iter && ctx_new && ctx_unref && ctx_connect && ctx_disc
            && ctx_state && get_src && op_unref && op_state;
        if (!ready) return 0;
    }
    pa_mainloop *ml = ml_new();
    if (!ml) return 0;
    pa_context *ctx = ctx_new(ml_api(ml), APP_NAME);
    if (!ctx) { ml_free(ml); return 0; }
    dev_out = out; dev_cap = cap; dev_n = 0;
    if (ctx_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        ctx_unref(ctx); ml_free(ml); return 0;
    }
    double until = now_s() + 2.0;
    pa_operation *op = NULL;
    while (now_s() < until) {
        ml_iter(ml, 0, NULL);
        pa_context_state_t stt = ctx_state(ctx);
        if (stt == PA_CONTEXT_FAILED || stt == PA_CONTEXT_TERMINATED) break;
        if (stt == PA_CONTEXT_READY && !op) op = get_src(ctx, on_source, NULL);
        if (op && op_state(op) != PA_OPERATION_RUNNING) break;
        if (!op) usleep(10000);
    }
    if (op) op_unref(op);
    ctx_disc(ctx);
    ctx_unref(ctx);
    ml_free(ml);
    return dev_n;
}

enum { CUR_MAX = 256 };

typedef struct {
    short x, y;
    unsigned short width, height;
    unsigned short xhot, yhot;
    unsigned long cursor_serial;
    unsigned long *pixels;
} SyCurImage;

static Display *cdpy;
static Window cwin;
static GC cgc;
static XImage *cimage;
static char *cbuf;
static int c_on, c_ready, cur_dw, cur_dh;
static void (*p_hide)(Display *, Window);
static void (*p_show)(Display *, Window);
static void (*p_shape)(Display *, Window, int, int, int, Pixmap, int);
static void (*p_rects)(Display *, Window, int, int, int, XRectangle *, int, int, int);
static void *(*p_getcur)(Display *);
static int xignore(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void cursor_load(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    void *xf = dlopen("libXfixes.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!xf) xf = dlopen("libXfixes.so.1", RTLD_NOW | RTLD_LOCAL);
    void *xe = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    if (xf) {
        p_hide = dlsym(xf, "XFixesHideCursor");
        p_show = dlsym(xf, "XFixesShowCursor");
        p_getcur = dlsym(xf, "XFixesGetCursorImage");
    }
    if (xe) {
        p_shape = dlsym(xe, "XShapeCombineMask");
        p_rects = dlsym(xe, "XShapeCombineRectangles");
    }
}

static int plat_cursor_mag(void) {
    cursor_load();
    return p_getcur && p_hide && p_show && p_shape ? 1 : 0;
}

static void plat_cursor_off(void) {
    if (!c_on || !cdpy) return;
    if (cwin) XUnmapWindow(cdpy, cwin);
    if (p_show) p_show(cdpy, DefaultRootWindow(cdpy));
    XFlush(cdpy);
    c_on = 0;
}

static int cursor_window(const char *disp) {
    if (c_ready) return cwin != 0;
    c_ready = 1;
    cursor_load();
    cdpy = XOpenDisplay(disp && disp[0] ? disp : NULL);
    if (!cdpy) return 0;
    int scr = DefaultScreen(cdpy);
    Visual *vis = DefaultVisual(cdpy, scr);
    int depth = DefaultDepth(cdpy, scr);
    XSetWindowAttributes at;
    memset(&at, 0, sizeof at);
    at.override_redirect = True;
    at.background_pixel = BlackPixel(cdpy, scr);
    cwin = XCreateWindow(cdpy, RootWindow(cdpy, scr), 0, 0, 16, 16, 0, depth, InputOutput, vis,
                         CWOverrideRedirect | CWBackPixel, &at);
    if (!cwin) return 0;
    cgc = XCreateGC(cdpy, cwin, 0, NULL);
    XStoreName(cdpy, cwin, "拾影指针");
    return 1;
}

static int cursor_image(int dw, int dh) {
    if (dw < 1 || dh < 1 || dw > CUR_MAX * 2 || dh > CUR_MAX * 2) return 0;
    if (cimage && cur_dw == dw && cur_dh == dh) return 1;
    if (cimage) {
        XDestroyImage(cimage);
        cimage = NULL;
        cbuf = NULL;
    }
    cbuf = malloc((size_t)dw * (size_t)dh * 4);
    if (!cbuf) return 0;
    int scr = DefaultScreen(cdpy);
    cimage = XCreateImage(cdpy, DefaultVisual(cdpy, scr), (unsigned)DefaultDepth(cdpy, scr), ZPixmap, 0, cbuf,
                          (unsigned)dw, (unsigned)dh, 32, 0);
    if (!cimage) {
        free(cbuf);
        cbuf = NULL;
        return 0;
    }
    cur_dw = dw;
    cur_dh = dh;
    XResizeWindow(cdpy, cwin, (unsigned)dw, (unsigned)dh);
    return 1;
}

static unsigned long rgb_pixel(unsigned r, unsigned g, unsigned b) {
    Visual *v = DefaultVisual(cdpy, DefaultScreen(cdpy));
    unsigned long rm = v->red_mask, gm = v->green_mask, bm = v->blue_mask;
    int rs = 0, gs = 0, bs = 0;
    while (rm && !(rm & 1)) { rm >>= 1; rs++; }
    while (gm && !(gm & 1)) { gm >>= 1; gs++; }
    while (bm && !(bm & 1)) { bm >>= 1; bs++; }
    if (!rm) rm = 255;
    if (!gm) gm = 255;
    if (!bm) bm = 255;
    return ((unsigned long)(r * rm / 255) << rs) | ((unsigned long)(g * gm / 255) << gs) | ((unsigned long)(b * bm / 255) << bs);
}

static void plat_cursor_tick(SySession *s) {
    int want = s && s->phase == SY_PHASE_LIVE && s->plan.cursor == SY_CURSOR_MAG
        && s->hold != SY_HOLD_PAUSE && s->hold != SY_HOLD_STOP;
    if (!want) { plat_cursor_off(); return; }
    if (!cursor_window(s->plan.display) || !p_getcur) return;
    Window root, child;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(cdpy, DefaultRootWindow(cdpy), &root, &child, &rx, &ry, &wx, &wy, &mask)) return;
    XErrorHandler old = XSetErrorHandler(xignore);
    SyCurImage *im = (SyCurImage *)p_getcur(cdpy);
    XSetErrorHandler(old);
    if (!im || !im->pixels || !im->width || !im->height) {
        if (im) XFree(im);
        return;
    }
    unsigned sw = im->width, sh = im->height;
    if (sw > CUR_MAX) sw = CUR_MAX;
    if (sh > CUR_MAX) sh = CUR_MAX;
    int dw = (int)sw * 2, dh = (int)sh * 2;
    if (!cursor_image(dw, dh)) { XFree(im); return; }
    XPoint *pts = malloc((size_t)dw * (size_t)dh * sizeof *pts);
    int np = 0;
    for (int y = 0; y < dh; y++) {
        unsigned syy = (unsigned)y / 2;
        if (syy >= im->height) syy = im->height - 1;
        for (int x = 0; x < dw; x++) {
            unsigned sxx = (unsigned)x / 2;
            if (sxx >= im->width) sxx = im->width - 1;
            unsigned long p = im->pixels[syy * im->width + sxx];
            unsigned a = (p >> 24) & 255u;
            unsigned r = (p >> 16) & 255u;
            unsigned g = (p >> 8) & 255u;
            unsigned b = p & 255u;
            if (a && a < 255 && r <= a && g <= a && b <= a) {
                r = r * 255u / a;
                g = g * 255u / a;
                b = b * 255u / a;
            }
            XPutPixel(cimage, x, y, a ? rgb_pixel(r, g, b) : 0);
            if (pts && a > 16) {
                pts[np].x = (short)x;
                pts[np].y = (short)y;
                np++;
            }
        }
    }
    int px = rx - (int)im->xhot * 2;
    int py = ry - (int)im->yhot * 2;
    XFree(im);
    Pixmap shape = XCreatePixmap(cdpy, cwin, (unsigned)dw, (unsigned)dh, 1);
    GC mg = XCreateGC(cdpy, shape, 0, NULL);
    XSetForeground(cdpy, mg, 0);
    XFillRectangle(cdpy, shape, mg, 0, 0, (unsigned)dw, (unsigned)dh);
    if (np) {
        XSetForeground(cdpy, mg, 1);
        XDrawPoints(cdpy, shape, mg, pts, np, CoordModeOrigin);
    }
    p_shape(cdpy, cwin, 0, 0, 0, shape, 0);
    if (p_rects) p_rects(cdpy, cwin, 2, 0, 0, NULL, 0, 0, 0);
    XFreeGC(cdpy, mg);
    XFreePixmap(cdpy, shape);
    free(pts);
    XPutImage(cdpy, cwin, cgc, cimage, 0, 0, 0, 0, (unsigned)dw, (unsigned)dh);
    XMoveWindow(cdpy, cwin, px, py);
    if (!c_on) {
        XMapRaised(cdpy, cwin);
        if (p_hide) p_hide(cdpy, DefaultRootWindow(cdpy));
        c_on = 1;
    }
    XFlush(cdpy);
}
