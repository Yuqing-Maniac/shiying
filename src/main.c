/* 拾影录屏 —— 界面与主循环（Xlib 自绘，单线程事件循环） */
#include "common.h"
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xresource.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../assets/icon_data.h"

/* ---------------- 全局 ---------------- */
static Display *dpy;
static int scr;
static Window root;
static double S = 1.0;               /* 界面缩放（Xft.dpi/96） */
static int has_comp;                 /* 有合成器，可用半透明圆角 */
static XVisualInfo argb_vi;
static int has_argb;
static volatile sig_atomic_t quit_req;
static int hotkey_conflict;

static Atom A_DEL, A_PROTO, A_ICON, A_STATE, A_ABOVE, A_SKIPTB, A_SKIPPG, A_TYPE, A_T_DOCK, A_T_DIALOG, A_T_UTIL, A_T_DESKTOP,
    A_CLIENTS, A_FRAME, A_GTKFRAME, A_NAME, A_UTF8, A_MOTIF, A_PID, A_SINGLE, A_CMD, A_ACTIVE;

#define P(v) ((v) * S)

/* 配色 */
#define C_BG      hexc(0xF3F5F8)
#define C_CARD    hexc(0xFFFFFF)
#define C_BORDER  hexc(0xE3E7ED)
#define C_TEXT    hexc(0x1D2129)
#define C_SUB     hexc(0x86909C)
#define C_ACC     hexc(0x2F7DF6)
#define C_ACC_H   hexc(0x4A8EF8)
#define C_SEG     hexc(0xEEF1F5)
#define C_SEG_H   hexc(0xE1E6ED)
#define C_RED     hexc(0xEE3F35)
#define C_RED_H   hexc(0xF35A50)
#define C_GREEN   hexc(0x19A15F)
#define C_ORANGE  hexc(0xD97B00)
#define C_WHITE   hexc(0xFFFFFF)

/* ---------------- 窗口封装 ---------------- */
typedef struct { int x, y, w, h, id, arg; } Hit;
typedef struct Win {
    Window w;
    int W, H;
    Surf *s;
    XImage *img;
    GC gc;
    int argb, mapped;
    Hit hits[256];
    int nh, hov, hova, prs, prsa;
    void (*draw)(struct Win *);
} Win;

static Win wmain, wbar, wcd, wpick;
static Window wborder[4];
static int borders_on;

enum {
    H_NONE = 0, H_AREA, H_MON_PREV, H_MON_NEXT, H_RESELECT, H_AUDIO, H_FPS, H_QUAL, H_SCALE, H_FMT, H_CD,
    H_CODEC, H_CURSOR, H_SEG, H_UNIT, H_DEV_PREV, H_DEV_NEXT,
    H_OPEN_DIR, H_CHANGE_DIR, H_START, H_RES_PLAY, H_RES_DIR,
    H_BAR_PAUSE, H_BAR_STOP, H_BAR_HIDE,
    H_PK_ENTRY, H_PK_UP, H_PK_QUICK, H_PK_CANCEL, H_PK_OK
};

static int counting;
static double cd_until;
static int seg_focus;
static int audio_before = SY_AUDIO_SYSTEM;
static SyCaps ui_caps;
static SyChoice ui_codec[4], ui_cont[4], ui_curs[4], ui_aud[4];
static int n_codec, n_cont, n_curs, n_aud;
static SyDevice ui_dev[32];
static int n_dev, dev_i = -1;

static int choice_sel(const SyChoice *c, int n, int id) {
    for (int i = 0; i < n; i++) if (c[i].id == id) return i;
    return -1;
}

static void clamp_seg(void) {
    if (cfg.segment_n < 0) cfg.segment_n = 0;
    if (cfg.segment_n > 9999) cfg.segment_n = 9999;
    if (cfg.segment_unit == SY_UNIT_MB && cfg.segment_n > 0 && cfg.segment_n < 5) cfg.segment_n = 5;
}

static void load_caps(void) {
    memset(&ui_caps, 0, sizeof ui_caps);
    const char *ff = ffmpeg_path();
    if (ff) sy_probe(ff, &ui_caps);
    n_codec = sy_codec_choices(&ui_caps, ui_codec, 4);
    n_cont = sy_container_choices(&ui_caps, ui_cont, 4);
    n_curs = sy_cursor_choices(&ui_caps, ui_curs, 4);
    n_aud = sy_audio_choices(ui_aud, 4);
    SyCodec c = (SyCodec)cfg.codec;
    SyContainer k = (SyContainer)cfg.fmt;
    SyCursor u = (SyCursor)cfg.cursor;
    sy_clamp_recorded(&c, &k, &u, &ui_caps);
    cfg.codec = (int)c; cfg.fmt = (int)k; cfg.cursor = (int)u;
    n_dev = sy_devices(ui_dev, 32);
    dev_i = -1;
    if (cfg.audio == SY_AUDIO_DEVICE)
        for (int i = 0; i < n_dev; i++)
            if (!strcmp(ui_dev[i].id, cfg.audio_dev)) dev_i = i;
    if (cfg.audio != SY_AUDIO_DEVICE) audio_before = cfg.audio;
}

static void cycle_dev(int dir) {
    if (n_dev <= 0) { dev_i = -1; cfg.audio_dev[0] = 0; return; }
    if (dir > 0) dev_i = dev_i >= n_dev - 1 ? -1 : dev_i + 1;
    else dev_i = dev_i <= -1 ? n_dev - 1 : dev_i - 1;
    if (dev_i >= 0) {
        if (cfg.audio != SY_AUDIO_DEVICE) audio_before = cfg.audio;
        cfg.audio = SY_AUDIO_DEVICE;
        snprintf(cfg.audio_dev, sizeof cfg.audio_dev, "%s", ui_dev[dev_i].id);
    } else {
        cfg.audio_dev[0] = 0;
        cfg.audio = (audio_before >= SY_AUDIO_SILENCE && audio_before <= SY_AUDIO_BOTH) ? audio_before : SY_AUDIO_SYSTEM;
    }
}

static void hit_add(Win *w, double x, double y, double ww, double hh, int id, int arg) {
    if (w->nh < 256) w->hits[w->nh++] = (Hit){ (int)x, (int)y, (int)ww, (int)hh, id, arg };
}
static int is_hov(Win *w, int id, int arg) { return w->hov == id && w->hova == arg; }

static void win_present(Win *w) {
    if (!w->w || !w->mapped) return;
    XPutImage(dpy, w->w, w->gc, w->img, 0, 0, 0, 0, w->W, w->H);
}
static void win_redraw(Win *w) {
    if (!w->w) return;
    w->nh = 0;
    w->draw(w);
    win_present(w);
}

static void set_wm_icon(Window w) {
    int sizes[3] = { 32, 48, 64 };
    const unsigned int *data[3] = { ICON32, ICON48, ICON64 };
    int total = 0;
    for (int i = 0; i < 3; i++) total += 2 + sizes[i] * sizes[i];
    unsigned long *buf = malloc(sizeof(unsigned long) * total);
    int k = 0;
    for (int i = 0; i < 3; i++) {
        buf[k++] = sizes[i]; buf[k++] = sizes[i];
        for (int j = 0; j < sizes[i] * sizes[i]; j++) {
            unsigned int p = data[i][j], a = p >> 24;
            unsigned int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
            buf[k++] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    XChangeProperty(dpy, w, A_ICON, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)buf, total);
    free(buf);
}

static void set_props(Window w, const char *title, Atom type) {
    XClassHint ch = { (char *)APP_ID, (char *)APP_ID };
    XSetClassHint(dpy, w, &ch);
    XStoreName(dpy, w, "Shiying");
    XChangeProperty(dpy, w, A_NAME, A_UTF8, 8, PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    long pid = getpid();
    XChangeProperty(dpy, w, A_PID, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
    if (type) XChangeProperty(dpy, w, A_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
    set_wm_icon(w);
    XSetWMProtocols(dpy, w, &A_DEL, 1);
}

static void no_decor(Window w) {
    long hints[5] = { 2, 0, 0, 0, 0 };   /* MWM_HINTS_DECORATIONS = 2，decorations = 0 */
    XChangeProperty(dpy, w, A_MOTIF, A_MOTIF, 32, PropModeReplace, (unsigned char *)hints, 5);
}

static void win_create(Win *w, int x, int y, int W, int H, int argb, int override, const char *title, Atom type) {
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    unsigned long mask = CWEventMask | CWBackPixel | CWBorderPixel;
    a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | StructureNotifyMask | KeyPressMask;
    Visual *vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    if (argb && has_argb) {
        vis = argb_vi.visual; depth = 32;
        a.colormap = XCreateColormap(dpy, root, vis, AllocNone);
        mask |= CWColormap;
        a.background_pixel = 0;
        w->argb = 1;
    } else {
        a.background_pixel = 0xF3F5F8;
        w->argb = 0;
    }
    a.border_pixel = 0;
    if (override) { a.override_redirect = True; mask |= CWOverrideRedirect; }
    w->W = W; w->H = H;
    w->w = XCreateWindow(dpy, root, x, y, W, H, 0, depth, InputOutput, vis, mask, &a);
    w->gc = XCreateGC(dpy, w->w, 0, NULL);
    w->s = surf_new(W, H);
    w->img = XCreateImage(dpy, vis, depth, ZPixmap, 0, (char *)w->s->px, W, H, 32, 0);
    set_props(w->w, title, type);
}

static void fixed_size(Window w, int W, int H, int x, int y) {
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize | PMaxSize | PPosition | USPosition;
    sh->min_width = sh->max_width = W;
    sh->min_height = sh->max_height = H;
    sh->x = x; sh->y = y;
    XSetWMNormalHints(dpy, w, sh);
    XFree(sh);
}

static void set_state_atoms(Window w, Atom *list, int n) {
    XChangeProperty(dpy, w, A_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, n);
}

static void no_focus(Window w) {
    XWMHints *h = XAllocWMHints();
    h->flags = InputHint;
    h->input = False;
    XSetWMHints(dpy, w, h);
    XFree(h);
}

static void spawn_detached(const char *prog, const char *arg) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            int nul = open("/dev/null", O_RDWR);
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            signal(SIGPIPE, SIG_DFL);
            execlp(prog, prog, arg, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void open_path(const char *p) {
    if (access("/usr/bin/xdg-open", X_OK) == 0) spawn_detached("xdg-open", p);
    else if (access("/usr/bin/peony", X_OK) == 0) spawn_detached("peony", p);
    else spawn_detached("gio", p);
}

/* ---------------- 绘制小部件 ---------------- */
static double TX(const char *t, double sz, int b) { return text_width(t, P(sz), b); }

static void text_c(Surf *s, double cx, double cy, const char *t, double sz, int bold, uint32_t c) {
    double w = TX(t, sz, bold);
    double asc = font_ascent(P(sz));
    draw_text(s, cx - w / 2, cy + asc * 0.36 + P(sz) * 0.02, t, P(sz), bold, c);
}
static void text_l(Surf *s, double x, double cy, const char *t, double sz, int bold, uint32_t c) {
    double asc = font_ascent(P(sz));
    draw_text(s, x, cy + asc * 0.36 + P(sz) * 0.02, t, P(sz), bold, c);
}

/* 超长文字从前面省略 */
static void elide_front(char *out, size_t n, const char *t, double maxw, double sz) {
    if (TX(t, sz, 0) <= maxw) { snprintf(out, n, "%s", t); return; }
    const char *p = t;
    while (*p) {
        p++;
        while ((*p & 0xC0) == 0x80) p++;
        char tmp[1200];
        snprintf(tmp, sizeof tmp, "…%s", p);
        if (TX(tmp, sz, 0) <= maxw) { snprintf(out, n, "%s", tmp); return; }
    }
    snprintf(out, n, "…");
}

static void segmented(Win *w, double x, double y, double ww, double hh, const char **items, int n, int sel, int id) {
    Surf *s = w->s;
    fill_rrect(s, x, y, ww, hh, P(8), C_SEG);
    double iw = ww / n;
    for (int i = 0; i < n; i++) {
        double ix = x + i * iw;
        if (i == sel) fill_rrect(s, ix + P(2), y + P(2), iw - P(4), hh - P(4), P(6), C_ACC);
        else if (is_hov(w, id, i)) fill_rrect(s, ix + P(2), y + P(2), iw - P(4), hh - P(4), P(6), C_SEG_H);
        text_c(s, ix + iw / 2, y + hh / 2, items[i], 13, 0, i == sel ? C_WHITE : C_TEXT);
        hit_add(w, ix, y, iw, hh, id, i);
    }
}

static void small_btn(Win *w, double x, double y, double ww, double hh, const char *t, int id, int arg) {
    Surf *s = w->s;
    fill_rrect(s, x, y, ww, hh, P(6), is_hov(w, id, arg) ? hexc(0xEEF2F7) : C_WHITE);
    stroke_rrect(s, x, y, ww, hh, P(6), P(1), C_BORDER);
    text_c(s, x + ww / 2, y + hh / 2, t, 12.5, 0, C_TEXT);
    hit_add(w, x, y, ww, hh, id, arg);
}

static void link_btn(Win *w, double x, double cy, const char *t, int id, int arg, uint32_t c) {
    double tw = TX(t, 12.5, 0);
    text_l(w->s, x, cy, t, 12.5, 0, is_hov(w, id, arg) ? C_ACC_H : c);
    if (is_hov(w, id, arg)) fill_rect(w->s, x, cy + P(9), tw, P(1), C_ACC_H);
    hit_add(w, x - P(4), cy - P(12), tw + P(8), P(24), id, arg);
}

static void card(Surf *s, double x, double y, double ww, double hh) {
    fill_rrect(s, x, y + P(1), ww, hh, P(12), hexc(0xE9ECF1));
    fill_rrect(s, x, y, ww, hh, P(12), C_CARD);
    stroke_rrect(s, x, y, ww, hh, P(12), P(1), C_BORDER);
}

static void fmt_dur(char *o, size_t n, double sec) {
    int t = (int)sec;
    snprintf(o, n, "%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
}

/* ---------------- 状态 ---------------- */
static char status_msg[1400];
static uint32_t status_col;
static char warn_msg[300];
static int ui_state_prev = REC_IDLE;
static double pending_start_at;      /* 倒计时结束后延迟启动的时刻 */
static int pending_x, pending_y, pending_w, pending_h;
static int bar_hidden;
static int select_then_start;

static void set_status(const char *msg, uint32_t c) {
    snprintf(status_msg, sizeof status_msg, "%s", msg);
    status_col = c;
}

static void target_rect(int *x, int *y, int *w, int *h) {
    if (cfg.area == AREA_REGION && cfg.rw > 0 && cfg.rh > 0) {
        *x = cfg.rx; *y = cfg.ry; *w = cfg.rw; *h = cfg.rh;
    } else if (cfg.monitor >= 0 && cfg.monitor < g_nmons) {
        Mon *m = &g_mons[cfg.monitor];
        *x = m->x; *y = m->y; *w = m->w; *h = m->h;
    } else {
        *x = 0; *y = 0; *w = g_root_w; *h = g_root_h;
    }
    /* 裁到根窗口内 */
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > g_root_w) *w = g_root_w - *x;
    if (*y + *h > g_root_h) *h = g_root_h - *y;
}

/* ---------------- 主窗口 ---------------- */
#define MAIN_W 480
#define MAIN_H 760
#define CARD_SET_H 326

static void draw_main(Win *w) {
    Surf *s = w->s;
    surf_clear(s, C_BG);
    double X = P(16), CW = P(MAIN_W - 32);
    /* 标题区 */
    blit_argb(s, S >= 1.5 ? ICON64 : ICON48, S >= 1.5 ? 64 : 48, S >= 1.5 ? 64 : 48, (int)P(20), (int)P(18), (int)P(40), (int)P(40));
    text_l(s, P(70), P(30), APP_NAME, 19, 1, C_TEXT);
    text_l(s, P(70), P(50), "轻量录屏 · 免安装 · 银河麒麟", 12, 0, C_SUB);
    char ver[32];
    snprintf(ver, sizeof ver, "v%s", APP_VERSION);
    text_l(s, P(MAIN_W - 20) - TX(ver, 11.5, 0), P(30), ver, 11.5, 0, C_SUB);

    /* 卡片一：录制范围 */
    double y = P(72);
    card(s, X, y, CW, P(116));
    text_l(s, X + P(16), y + P(22), "录制范围", 13, 1, C_TEXT);
    static const char *areas[] = { "全屏", "自选区域 / 窗口" };
    segmented(w, X + P(16), y + P(38), CW - P(32), P(34), areas, 2, cfg.area, H_AREA);
    double ry = y + P(94);
    char buf[300];
    if (cfg.area == AREA_SCREEN) {
        int x0, y0, ww, hh;
        target_rect(&x0, &y0, &ww, &hh);
        if (cfg.monitor < 0) snprintf(buf, sizeof buf, "%s　%d × %d", g_nmons > 1 ? "全部屏幕" : "整个屏幕", ww, hh);
        else snprintf(buf, sizeof buf, "屏幕 %d%s　%d × %d", cfg.monitor + 1, g_mons[cfg.monitor].primary ? "（主屏）" : "", ww, hh);
        text_c(s, X + CW / 2, ry, buf, 13, 0, C_TEXT);
        if (g_nmons > 1) {
            double bx = X + P(26), bx2 = X + CW - P(26);
            uint32_t c1 = is_hov(w, H_MON_PREV, 0) ? C_ACC : C_SUB, c2 = is_hov(w, H_MON_NEXT, 0) ? C_ACC : C_SUB;
            double t1[] = { bx + P(4), ry - P(7), bx + P(4), ry + P(7), bx - P(4), ry };
            double t2[] = { bx2 - P(4), ry - P(7), bx2 - P(4), ry + P(7), bx2 + P(4), ry };
            fill_poly(s, t1, 3, c1);
            fill_poly(s, t2, 3, c2);
            hit_add(w, bx - P(14), ry - P(14), P(28), P(28), H_MON_PREV, 0);
            hit_add(w, bx2 - P(14), ry - P(14), P(28), P(28), H_MON_NEXT, 0);
        }
    } else {
        if (cfg.rw > 0) snprintf(buf, sizeof buf, "区域 %d × %d　起点 (%d, %d)", cfg.rw & ~1, cfg.rh & ~1, cfg.rx, cfg.ry);
        else snprintf(buf, sizeof buf, "尚未框选，开始录制时先框选");
        text_l(s, X + P(18), ry, buf, 13, 0, cfg.rw > 0 ? C_TEXT : C_SUB);
        const char *lk = cfg.rw > 0 ? "重新框选" : "现在框选";
        link_btn(w, X + CW - P(18) - TX(lk, 12.5, 0), ry, lk, H_RESELECT, 0, C_ACC);
    }

    /* 卡片二：录制设置 */
    y += P(116 + 10);
    card(s, X, y, CW, P(CARD_SET_H));
    text_l(s, X + P(16), y + P(22), "录制设置", 13, 1, C_TEXT);
    double ly = y + P(38), rowh = P(40), segh = P(30);
    double lx = X + P(16), lw = P(46);
    double half = (CW - P(32) - P(16)) / 2;
    double rx = lx + half + P(16);
    const char *alab[4];
    for (int i = 0; i < n_aud; i++) alab[i] = ui_aud[i].label;
    text_l(s, lx, ly + segh / 2, "声音", 13, 0, C_SUB);
    if (n_aud) segmented(w, lx + lw, ly, CW - P(32) - lw, segh, alab, n_aud, choice_sel(ui_aud, n_aud, cfg.audio), H_AUDIO);
    ly += rowh;
    static const char *fps[] = { "10", "15", "24", "30" };
    int fsel = cfg.fps == 10 ? 0 : cfg.fps == 15 ? 1 : cfg.fps == 24 ? 2 : 3;
    text_l(s, lx, ly + segh / 2, "帧率", 13, 0, C_SUB);
    segmented(w, lx + lw, ly, half - lw, segh, fps, 4, fsel, H_FPS);
    static const char *ql[] = { "流畅", "标准", "高清" };
    text_l(s, rx, ly + segh / 2, "画质", 13, 0, C_SUB);
    segmented(w, rx + lw, ly, half - lw, segh, ql, 3, cfg.quality, H_QUAL);
    ly += rowh;
    static const char *sc[] = { "原始", "1080P", "720P" };
    text_l(s, lx, ly + segh / 2, "分辨率", 13, 0, C_SUB);
    segmented(w, lx + lw, ly, half - lw, segh, sc, 3, cfg.scale, H_SCALE);
    const char *flab[4];
    for (int i = 0; i < n_cont; i++) flab[i] = ui_cont[i].label;
    text_l(s, rx, ly + segh / 2, "格式", 13, 0, C_SUB);
    if (n_cont) segmented(w, rx + lw, ly, half - lw, segh, flab, n_cont, choice_sel(ui_cont, n_cont, cfg.fmt), H_FMT);
    ly += rowh;
    static const char *cd[] = { "关闭", "3 秒", "5 秒" };
    int cdsel = cfg.countdown == 0 ? 0 : cfg.countdown == 3 ? 1 : 2;
    text_l(s, lx, ly + segh / 2, "倒计时", 13, 0, C_SUB);
    segmented(w, lx + lw, ly, half - lw, segh, cd, 3, cdsel, H_CD);
    const char *clab[4];
    for (int i = 0; i < n_codec; i++) clab[i] = ui_codec[i].label;
    text_l(s, rx, ly + segh / 2, "编码", 13, 0, C_SUB);
    if (n_codec) segmented(w, rx + lw, ly, half - lw, segh, clab, n_codec, choice_sel(ui_codec, n_codec, cfg.codec), H_CODEC);
    else text_l(s, rx + lw, ly + segh / 2, "无", 13, 0, C_SUB);
    ly += rowh;
    const char *ulab[4];
    for (int i = 0; i < n_curs; i++) ulab[i] = ui_curs[i].label;
    text_l(s, lx, ly + segh / 2, "指针", 13, 0, C_SUB);
    if (n_curs) segmented(w, lx + lw, ly, CW - P(32) - lw, segh, ulab, n_curs, choice_sel(ui_curs, n_curs, cfg.cursor), H_CURSOR);
    ly += rowh;
    text_l(s, lx, ly + segh / 2, "分段", 13, 0, C_SUB);
    {
        char num[16];
        snprintf(num, sizeof num, "%d", cfg.segment_n);
        double bx = lx + lw, bw = P(72);
        fill_rrect(s, bx, ly, bw, segh, P(6), C_WHITE);
        stroke_rrect(s, bx, ly, bw, segh, P(6), P(seg_focus ? 1.6 : 1), seg_focus ? C_ACC : C_BORDER);
        text_c(s, bx + bw / 2, ly + segh / 2, num, 13, 0, C_TEXT);
        hit_add(w, bx, ly, bw, segh, H_SEG, 0);
        static const char *un[] = { "MB", "GB" };
        segmented(w, bx + bw + P(8), ly, P(120), segh, un, 2, cfg.segment_unit == SY_UNIT_GB ? 1 : 0, H_UNIT);
    }
    ly += rowh;
    text_l(s, lx, ly + segh / 2, "设备", 13, 0, C_SUB);
    {
        const char *show = "未指定";
        if (cfg.audio == SY_AUDIO_DEVICE) {
            if (dev_i >= 0 && dev_i < n_dev) show = ui_dev[dev_i].label;
            else if (cfg.audio_dev[0]) show = cfg.audio_dev;
        }
        double ax = lx + lw + P(8), ax2 = X + CW - P(28);
        char eld[300];
        elide_front(eld, sizeof eld, show, ax2 - ax - P(36), 13);
        text_c(s, (ax + ax2) / 2, ly + segh / 2, eld, 13, 0, C_TEXT);
        if (n_dev > 0) {
            uint32_t c1 = is_hov(w, H_DEV_PREV, 0) ? C_ACC : C_SUB, c2 = is_hov(w, H_DEV_NEXT, 0) ? C_ACC : C_SUB;
            double cy = ly + segh / 2;
            double t1[] = { ax + P(4), cy - P(6), ax + P(4), cy + P(6), ax - P(4), cy };
            double t2[] = { ax2 - P(4), cy - P(6), ax2 - P(4), cy + P(6), ax2 + P(4), cy };
            fill_poly(s, t1, 3, c1);
            fill_poly(s, t2, 3, c2);
            hit_add(w, ax - P(14), ly, P(28), segh, H_DEV_PREV, 0);
            hit_add(w, ax2 - P(14), ly, P(28), segh, H_DEV_NEXT, 0);
        }
    }

    /* 卡片三：保存位置 */
    y += P(CARD_SET_H + 10);
    card(s, X, y, CW, P(52));
    text_l(s, X + P(16), y + P(26), "保存到", 13, 0, C_SUB);
    double bw = P(52);
    double px0 = X + P(16) + P(52), pmax = CW - P(16) - P(52) - bw * 2 - P(14);
    char el[1200];
    elide_front(el, sizeof el, cfg.outdir, pmax, 12.5);
    text_l(s, px0, y + P(26), el, 12.5, 0, C_TEXT);
    small_btn(w, X + CW - P(16) - bw * 2 - P(8), y + P(12), bw, P(28), "打开", H_OPEN_DIR, 0);
    small_btn(w, X + CW - P(16) - bw, y + P(12), bw, P(28), "更改", H_CHANGE_DIR, 0);

    /* 开始按钮 */
    y += P(52 + 14);
    int busy = counting || rec.state != REC_IDLE;
    uint32_t bc = busy ? hexc(0xF2A19B) : is_hov(w, H_START, 0) ? C_RED_H : C_RED;
    fill_rrect(s, X, y, CW, P(48), P(12), bc);
    const char *bt = rec.state == REC_SAVING ? "正在保存…" : "开始录制";
    double tw = TX(bt, 16, 1);
    double cx = X + CW / 2 - (tw + P(22)) / 2;
    if (rec.state != REC_SAVING) {
        fill_circle(s, cx + P(7), y + P(24), P(7), C_WHITE);
        fill_circle(s, cx + P(7), y + P(24), P(3.5), bc);
    }
    text_l(s, cx + P(22), y + P(24), bt, 16, 1, C_WHITE);
    hit_add(w, X, y, CW, P(48), H_START, 0);

    /* 快捷键提示 */
    y += P(48 + 18);
    if (hotkey_conflict)
        text_c(s, X + CW / 2, y, "全局快捷键被其他程序占用，可在系统快捷键中绑定命令「拾影录屏 --toggle」", 11.5, 0, C_ORANGE);
    else
        text_c(s, X + CW / 2, y, "Ctrl+Alt+R 开始/停止　Ctrl+Alt+P 暂停/继续　Ctrl+Alt+H 隐藏控制条", 11.5, 0, C_SUB);

    /* 状态区 */
    y += P(22);
    if (warn_msg[0]) {
        text_c(s, X + CW / 2, y, warn_msg, 12, 0, C_ORANGE);
        y += P(20);
    }
    if (rec.state == REC_SAVING) {
        text_c(s, X + CW / 2, y + P(6), rec.recovered ? "正在恢复上次未保存的录像…" : "正在合并保存，请稍候…", 13, 0, C_ACC);
    } else if (status_msg[0]) {
        /* 最多两行，按宽度断开 */
        const char *m = status_msg;
        double maxw = CW - P(8);
        char line[700];
        for (int ln = 0; ln < 2 && *m; ln++) {
            int len = 0, last_fit = 0;
            const char *q = m;
            while (*q) {
                const char *nx = q + 1;
                while ((*nx & 0xC0) == 0x80) nx++;
                len = (int)(nx - m);
                if (len >= (int)sizeof line - 1) break;
                memcpy(line, m, len); line[len] = 0;
                if (TX(line, 12.5, 0) > maxw) break;
                last_fit = len;
                q = nx;
            }
            if (!last_fit) break;
            memcpy(line, m, last_fit); line[last_fit] = 0;
            text_c(s, X + CW / 2, y + ln * P(19), line, 12.5, 0, status_col);
            m += last_fit;
        }
        if (rec.result[0] && status_col == C_GREEN) {
            double ly2 = y + P(24);
            const char *a = "播放", *b = "打开所在文件夹";
            double total = TX(a, 12.5, 0) + TX(b, 12.5, 0) + P(28);
            double sx = X + CW / 2 - total / 2;
            link_btn(w, sx, ly2, a, H_RES_PLAY, 0, C_ACC);
            link_btn(w, sx + TX(a, 12.5, 0) + P(28), ly2, b, H_RES_DIR, 0, C_ACC);
        }
    }
}

/* ---------------- 录制控制条 ---------------- */
#define BAR_W 360
#define BAR_H 46

static void draw_bar(Win *w) {
    Surf *s = w->s;
    surf_clear(s, 0);
    uint32_t bg = w->argb ? rgba(28, 30, 34, 236) : hexc(0x1E2024);
    fill_rrect(s, 0, 0, w->W, w->H, w->argb ? P(BAR_H / 2.0) : 0, bg);
    double cy = w->H / 2.0;
    int paused = rec.state == REC_PAUSED;
    double t = now_s();
    double pulse = paused ? 1 : 0.55 + 0.45 * (0.5 + 0.5 * sin(t * 4));
    fill_circle(s, P(22), cy, P(6), alpha_mul(paused ? hexc(0xF5B731) : C_RED, pulse));
    char tb[32];
    fmt_dur(tb, sizeof tb, rec_elapsed());
    text_l(s, P(36), cy, tb, 15, 1, C_WHITE);
        if (paused) text_l(s, P(36) + TX("00:00:00", 15, 1) + P(8), cy, "已暂停", 11.5, 0, hexc(0xF5B731));
        else if (rec.hold == SY_HOLD_CUT) text_l(s, P(36) + TX("00:00:00", 15, 1) + P(8), cy, "正在完成上一段", 11.5, 0, hexc(0xC8CCD2));
        else {
        /* 声音电平 */
        double lv = -1;
        for (int i = 0; i < 2; i++) { double l = audio_level(i); if (l > lv) lv = l; }
        if (lv >= 0) {
            double bx = P(36) + TX("00:00:00", 15, 1) + P(10);
            for (int i = 0; i < 4; i++) {
                double hh = P(4 + i * 3);
                int on = lv * 6 > i * 0.6 + 0.05;
                fill_rrect(s, bx + i * P(5), cy + P(7) - hh, P(3), hh, P(1), on ? hexc(0x4CD07D) : hexc(0x4A4F57));
            }
        }
    }
    /* 按钮：暂停/继续、停止、隐藏 */
    double bx = w->W - P(3 * 36 + 8);
    for (int i = 0; i < 3; i++) {
        double cx = bx + i * P(36) + P(18);
        int id = i == 0 ? H_BAR_PAUSE : i == 1 ? H_BAR_STOP : H_BAR_HIDE;
        if (is_hov(w, id, 0)) fill_circle(s, cx, cy, P(15), hexc(0x3A3E46));
        if (i == 0) {
            if (paused) {
                double tri[] = { cx - P(4), cy - P(7), cx - P(4), cy + P(7), cx + P(7), cy };
                fill_poly(s, tri, 3, C_WHITE);
            } else {
                fill_rrect(s, cx - P(6), cy - P(7), P(4), P(14), P(1), C_WHITE);
                fill_rrect(s, cx + P(2), cy - P(7), P(4), P(14), P(1), C_WHITE);
            }
        } else if (i == 1) {
            fill_rrect(s, cx - P(7), cy - P(7), P(14), P(14), P(3), C_RED);
        } else {
            fill_rrect(s, cx - P(7), cy - P(1), P(14), P(2.2), P(1), hexc(0xC8CCD2));
        }
        hit_add(w, cx - P(17), 0, P(34), w->H, id, 0);
    }
}

/* ---------------- 倒计时 ---------------- */
static void draw_cd(Win *w) {
    Surf *s = w->s;
    surf_clear(s, 0);
    fill_rrect(s, 0, 0, w->W, w->H, w->argb ? P(28) : 0, w->argb ? rgba(20, 22, 26, 215) : hexc(0x16181C));
    int left = (int)ceil(cd_until - now_s());
    if (left < 1) left = 1;
    char b[8];
    snprintf(b, sizeof b, "%d", left);
    text_c(s, w->W / 2.0, w->H / 2.0 - P(10), b, 72, 1, C_WHITE);
    text_c(s, w->W / 2.0, w->H - P(24), "单击取消", 12, 0, hexc(0xA9AEB6));
}

/* ---------------- 选择目录 ---------------- */
#define PK_W 460
#define PK_H 520
static char pk_dir[1024];
static char (*pk_names)[256];
static int pk_n, pk_scroll;

static int cmpstr(const void *a, const void *b) { return strcoll((const char *)a, (const char *)b); }

static void pk_load(const char *dir) {
    char real[1024];
    if (!realpath(dir, real)) snprintf(real, sizeof real, "%s", dir);
    snprintf(pk_dir, sizeof pk_dir, "%s", real);
    if (!pk_names) pk_names = malloc(sizeof(*pk_names) * 2000);
    pk_n = 0; pk_scroll = 0;
    DIR *d = opendir(pk_dir);
    if (!d) return;
    struct dirent *e;
    char p[1400];
    while ((e = readdir(d)) && pk_n < 2000) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", pk_dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(pk_names[pk_n++], 256, "%s", e->d_name);
    }
    closedir(d);
    qsort(pk_names, pk_n, 256, cmpstr);
}

static void draw_pick(Win *w) {
    Surf *s = w->s;
    surf_clear(s, C_BG);
    double X = P(16), CW = w->W - P(32);
    text_l(s, X, P(26), "选择保存目录", 15, 1, C_TEXT);
    /* 快捷位置 */
    static const char *ql[] = { "主目录", "桌面", "视频", "外接存储" };
    double qx = X, qy = P(44);
    for (int i = 0; i < 4; i++) {
        double bw = TX(ql[i], 12.5, 0) + P(24);
        small_btn(w, qx, qy, bw, P(28), ql[i], H_PK_QUICK, i);
        qx += bw + P(8);
    }
    /* 当前路径 */
    card(s, X, P(82), CW, P(36));
    char el[1200];
    elide_front(el, sizeof el, pk_dir, CW - P(90), 12.5);
    text_l(s, X + P(12), P(100), el, 12.5, 0, C_TEXT);
    small_btn(w, X + CW - P(66), P(86), P(60), P(28), "上一级", H_PK_UP, 0);
    /* 列表 */
    double ly = P(128), lh = w->H - P(128) - P(64), rh = P(32);
    card(s, X, ly, CW, lh);
    int vis = (int)(lh / rh) - 0;
    if (pk_scroll > pk_n - vis) pk_scroll = pk_n - vis;
    if (pk_scroll < 0) pk_scroll = 0;
    if (pk_n == 0) text_c(s, X + CW / 2, ly + lh / 2, "（此处没有子文件夹）", 12.5, 0, C_SUB);
    for (int i = 0; i < vis && pk_scroll + i < pk_n; i++) {
        int idx = pk_scroll + i;
        double ry = ly + P(2) + i * rh;
        if (ry + rh > ly + lh) break;
        if (is_hov(w, H_PK_ENTRY, idx)) fill_rrect(s, X + P(4), ry, CW - P(8), rh, P(6), hexc(0xEAF2FE));
        /* 文件夹图标 */
        fill_rrect(s, X + P(14), ry + P(10), P(8), P(4), P(1), hexc(0xF4B63F));
        fill_rrect(s, X + P(14), ry + P(12), P(18), P(12), P(2), hexc(0xF7C55B));
        char nm[300];
        elide_front(nm, sizeof nm, pk_names[idx], CW - P(60), 13);
        text_l(s, X + P(42), ry + rh / 2, nm, 13, 0, C_TEXT);
        hit_add(w, X + P(4), ry, CW - P(8), rh, H_PK_ENTRY, idx);
    }
    if (pk_n > vis) {
        double th = lh * vis / pk_n, ty = ly + (lh - th) * pk_scroll / (double)(pk_n - vis);
        fill_rrect(s, X + CW - P(7), ty + P(2), P(4), th - P(4), P(2), hexc(0xC9CDD4));
    }
    /* 底部按钮 */
    double by = w->H - P(50);
    double bw = P(96);
    small_btn(w, X + CW - bw * 2 - P(10), by, bw, P(34), "取消", H_PK_CANCEL, 0);
    fill_rrect(s, X + CW - bw, by, bw, P(34), P(6), is_hov(w, H_PK_OK, 0) ? C_ACC_H : C_ACC);
    text_c(s, X + CW - bw / 2, by + P(17), "选择此目录", 12.5, 0, C_WHITE);
    hit_add(w, X + CW - bw, by, bw, P(34), H_PK_OK, 0);
    text_l(s, X, by + P(17), "单击文件夹进入", 12, 0, C_SUB);
}

static void pick_open(void) {
    pk_load(cfg.outdir[0] && access(cfg.outdir, F_OK) == 0 ? cfg.outdir : home_dir());
    if (!wpick.w) {
        int W = (int)P(PK_W), H = (int)P(PK_H);
        win_create(&wpick, 0, 0, W, H, 0, 0, "选择保存目录", A_T_DIALOG);
        wpick.draw = draw_pick;
        XSetTransientForHint(dpy, wpick.w, wmain.w);
        fixed_size(wpick.w, W, H, 0, 0);
    }
    Window ch; int mx, my;
    XTranslateCoordinates(dpy, wmain.w, root, 0, 0, &mx, &my, &ch);
    int x = mx + (wmain.W - wpick.W) / 2, y = my + P(40);
    fixed_size(wpick.w, wpick.W, wpick.H, x, y);
    XMoveWindow(dpy, wpick.w, x, y);
    XMapRaised(dpy, wpick.w);
    wpick.mapped = 1;
    win_redraw(&wpick);
}
static void pick_close(void) {
    if (wpick.w && wpick.mapped) { XUnmapWindow(dpy, wpick.w); wpick.mapped = 0; }
}

/* ---------------- 区域选择浮层 ---------------- */
typedef struct { int x, y, w, h; } Rect;
static Rect clients[256];
static int nclients;

static long *get_prop_card(Window w, Atom a, int *n) {
    Atom t; int f; unsigned long cnt, after; unsigned char *d = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy, w, a, 0, 1024, False, AnyPropertyType, &t, &f, &cnt, &after, &d) != Success || !d) return NULL;
    if (f != 32) { XFree(d); return NULL; }
    *n = (int)cnt;
    return (long *)d;
}

static int xerr_ignore(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void collect_clients(void) {
    nclients = 0;
    int n;
    long *list = get_prop_card(root, A_CLIENTS, &n);
    if (!list) return;
    XErrorHandler old = XSetErrorHandler(xerr_ignore);
    for (int i = 0; i < n && nclients < 256; i++) {
        Window c = (Window)list[i];
        if (c == wmain.w || c == wbar.w || c == wpick.w) continue;
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, c, &wa) || wa.map_state != IsViewable) continue;
        int tn;
        long *ty = get_prop_card(c, A_TYPE, &tn);
        int skip = 0;
        for (int k = 0; ty && k < tn; k++) if ((Atom)ty[k] == A_T_DESKTOP || (Atom)ty[k] == A_T_DOCK) skip = 1;
        if (ty) XFree(ty);
        if (skip) continue;
        int ax, ay; Window ch;
        XTranslateCoordinates(dpy, c, root, 0, 0, &ax, &ay, &ch);
        Rect r = { ax, ay, wa.width, wa.height };
        int fn;
        long *fe = get_prop_card(c, A_FRAME, &fn);
        if (fe && fn == 4) { r.x -= fe[0]; r.w += fe[0] + fe[1]; r.y -= fe[2]; r.h += fe[2] + fe[3]; }
        if (fe) XFree(fe);
        long *ge = get_prop_card(c, A_GTKFRAME, &fn);   /* GTK 客户端装饰的阴影要去掉 */
        if (ge && fn == 4) { r.x += ge[0]; r.w -= ge[0] + ge[1]; r.y += ge[2]; r.h -= ge[2] + ge[3]; }
        if (ge) XFree(ge);
        if (r.w < 20 || r.h < 20) continue;
        clients[nclients++] = r;
    }
    XSync(dpy, False);
    XSetErrorHandler(old);
    XFree(list);
}

static Rect clip_root(Rect r) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > g_root_w) r.w = g_root_w - r.x;
    if (r.y + r.h > g_root_h) r.h = g_root_h - r.y;
    return r;
}

static Rect hover_target(int px, int py) {
    for (int i = nclients - 1; i >= 0; i--) {
        Rect r = clients[i];
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return clip_root(r);
    }
    for (int i = 0; i < g_nmons; i++) {
        Mon *m = &g_mons[i];
        if (px >= m->x && px < m->x + m->w && py >= m->y && py < m->y + m->h) return (Rect){ m->x, m->y, m->w, m->h };
    }
    return (Rect){ 0, 0, g_root_w, g_root_h };
}

/* 浮层：返回 1 选中，0 取消 */
static int run_selector(Rect *out) {
    mons_refresh(dpy);
    collect_clients();
    int W = g_root_w, H = g_root_h;
    XImage *shot = XGetImage(dpy, root, 0, 0, W, H, AllPlanes, ZPixmap);
    if (!shot) return 0;
    int depth = DefaultDepth(dpy, scr);
    Pixmap bright = XCreatePixmap(dpy, root, W, H, depth), dim = XCreatePixmap(dpy, root, W, H, depth);
    GC gc = XCreateGC(dpy, root, 0, NULL);
    XPutImage(dpy, bright, gc, shot, 0, 0, 0, 0, W, H);
    if (shot->bits_per_pixel == 32) {
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)(shot->data + (size_t)y * shot->bytes_per_line);
            for (int x = 0; x < W; x++) {
                uint32_t p = row[x];
                row[x] = (((p >> 16 & 255) * 115 >> 8) << 16) | (((p >> 8 & 255) * 115 >> 8) << 8) | ((p & 255) * 115 >> 8);
            }
        }
    } else {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned long p = XGetPixel(shot, x, y);
                unsigned long r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                XPutPixel(shot, x, y, ((r * 45 / 100) << 16) | ((g * 45 / 100) << 8) | (b * 45 / 100));
            }
    }
    XPutImage(dpy, dim, gc, shot, 0, 0, 0, 0, W, H);
    XDestroyImage(shot);
    /* 提示条画进暗图 */
    {
        const char *tip = "拖动鼠标框选区域　·　单击选择窗口或整屏　·　右键 / Esc 取消";
        int tw = (int)(TX(tip, 14, 0) + P(40)), th = (int)P(40);
        Surf *ts = surf_new(tw, th);
        surf_clear(ts, hexc(0x2B2E33));
        fill_rrect(ts, 0, 0, tw, th, 0, hexc(0x2B2E33));
        text_c(ts, tw / 2.0, th / 2.0, tip, 14, 0, C_WHITE);
        XImage *ti = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, 0, (char *)ts->px, tw, th, 32, 0);
        Mon *m = &g_mons[0];
        XPutImage(dpy, dim, gc, ti, 0, 0, m->x + (m->w - tw) / 2, m->y + (int)P(28), tw, th);
        ti->data = NULL; XDestroyImage(ti); surf_free(ts);
    }
    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixmap = dim;
    a.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | ExposureMask;
    Window ov = XCreateWindow(dpy, root, 0, 0, W, H, 0, depth, InputOutput, DefaultVisual(dpy, scr),
                              CWOverrideRedirect | CWBackPixmap | CWEventMask, &a);
    set_props(ov, "选择录制区域", 0);
    XMapRaised(dpy, ov);
    Cursor cur = XCreateFontCursor(dpy, XC_crosshair);
    for (int i = 0; i < 50; i++) {
        if (XGrabPointer(dpy, ov, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, cur, CurrentTime) == GrabSuccess) break;
        usleep(20000);
    }
    for (int i = 0; i < 50; i++) {
        if (XGrabKeyboard(dpy, ov, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
        usleep(20000);
    }
    XGCValues gv;
    gv.line_width = (int)fmax(2, P(2));
    gv.foreground = 0x2F7DF6;
    GC lgc = XCreateGC(dpy, ov, GCLineWidth | GCForeground, &gv);

    int dragging = 0, pressed = 0, sx = 0, sy = 0, result = 0, done = 0;
    Rect cur_r = { 0, 0, 0, 0 }, old_r = { 0, 0, 0, 0 }, old_lab = { 0, 0, 0, 0 };
    Window rw, cw; int rx, ry, wx, wy; unsigned int mk;
    XQueryPointer(dpy, root, &rw, &cw, &rx, &ry, &wx, &wy, &mk);
    cur_r = hover_target(rx, ry);
    int need = 1;
    while (!done) {
        if (need) {
            int pad = gv.line_width + 2;
            if (old_r.w) XCopyArea(dpy, dim, ov, gc, old_r.x - pad, old_r.y - pad, old_r.w + 2 * pad, old_r.h + 2 * pad, old_r.x - pad, old_r.y - pad);
            if (old_lab.w) XCopyArea(dpy, dim, ov, gc, old_lab.x, old_lab.y, old_lab.w, old_lab.h, old_lab.x, old_lab.y);
            Rect r = cur_r;
            if (r.w > 0 && r.h > 0) {
                XCopyArea(dpy, bright, ov, gc, r.x, r.y, r.w, r.h, r.x, r.y);
                XDrawRectangle(dpy, ov, lgc, r.x + gv.line_width / 2, r.y + gv.line_width / 2, r.w - gv.line_width, r.h - gv.line_width);
                char lab[64];
                snprintf(lab, sizeof lab, "%d × %d", r.w & ~1, r.h & ~1);
                int lw = (int)(TX(lab, 12.5, 1) + P(16)), lh = (int)P(24);
                int lx = r.x, ly = r.y - lh - (int)P(4);
                if (ly < 0) ly = r.y + (int)P(4), lx = r.x + (int)P(4);
                if (lx + lw > W) lx = W - lw;
                Surf *ls = surf_new(lw, lh);
                surf_clear(ls, hexc(0x2F7DF6));
                text_c(ls, lw / 2.0, lh / 2.0, lab, 12.5, 1, C_WHITE);
                XImage *li = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, 0, (char *)ls->px, lw, lh, 32, 0);
                XPutImage(dpy, ov, gc, li, 0, 0, lx, ly, lw, lh);
                li->data = NULL; XDestroyImage(li); surf_free(ls);
                old_lab = (Rect){ lx, ly, lw, lh };
            }
            old_r = r;
            XFlush(dpy);
            need = 0;
        }
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.window == ov && ev.xexpose.count == 0) { old_r.w = 0; old_lab.w = 0; XClearWindow(dpy, ov); need = 1; }
        else if (ev.type == MotionNotify) {
            while (XCheckTypedWindowEvent(dpy, ov, MotionNotify, &ev)) {}
            int mx = ev.xmotion.x_root, my = ev.xmotion.y_root;
            if (pressed && !dragging && (abs(mx - sx) > 4 || abs(my - sy) > 4)) dragging = 1;
            Rect nr;
            if (dragging) {
                nr.x = mx < sx ? mx : sx; nr.y = my < sy ? my : sy;
                nr.w = abs(mx - sx) + 1; nr.h = abs(my - sy) + 1;
                nr = clip_root(nr);
            } else if (!pressed) nr = hover_target(mx, my);
            else nr = cur_r;
            if (memcmp(&nr, &cur_r, sizeof nr)) { cur_r = nr; need = 1; }
        } else if (ev.type == ButtonPress) {
            if (ev.xbutton.button == 3) { done = 1; result = 0; }
            else if (ev.xbutton.button == 1) { pressed = 1; sx = ev.xbutton.x_root; sy = ev.xbutton.y_root; }
        } else if (ev.type == ButtonRelease && ev.xbutton.button == 1 && pressed) {
            if (!dragging) cur_r = hover_target(ev.xbutton.x_root, ev.xbutton.y_root);
            if (cur_r.w >= 16 && cur_r.h >= 16) { *out = cur_r; result = 1; done = 1; }
            else { pressed = dragging = 0; }
        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) { done = 1; result = 0; }
            else if ((ks == XK_Return || ks == XK_KP_Enter) && cur_r.w >= 16) { *out = cur_r; result = 1; done = 1; }
        }
    }
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XDestroyWindow(dpy, ov);
    XFreeCursor(dpy, cur);
    XFreeGC(dpy, lgc);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, bright);
    XFreePixmap(dpy, dim);
    XSync(dpy, False);
    return result;
}

/* ---------------- 录制流程 ---------------- */
static void main_show(void) {
    if (wmain.mapped) { XRaiseWindow(dpy, wmain.w); return; }
    Mon *m = &g_mons[0];
    int x = m->x + (m->w - wmain.W) / 2, y = m->y + (m->h - wmain.H) / 2;
    if (y < m->y) y = m->y;
    fixed_size(wmain.w, wmain.W, wmain.H, x, y);
    XMoveWindow(dpy, wmain.w, x, y);
    XMapRaised(dpy, wmain.w);
    wmain.mapped = 1;
    win_redraw(&wmain);
}
static void main_hide(void) {
    if (!wmain.mapped) return;
    XWithdrawWindow(dpy, wmain.w, scr);
    wmain.mapped = 0;
    pick_close();
}

static void borders_show(int x, int y, int w, int h) {
    int b = (int)fmax(2, P(2)), g = (int)P(3);
    int full = 0;
    for (int i = 0; i < g_nmons; i++) if (g_mons[i].x == x && g_mons[i].y == y && g_mons[i].w == w && g_mons[i].h == h) full = 1;
    if ((x == 0 && y == 0 && w == g_root_w && h == g_root_h) || full) return;
    Rect rs[4] = { { x - g - b, y - g - b, w + 2 * (g + b), b }, { x - g - b, y + h + g, w + 2 * (g + b), b },
                   { x - g - b, y - g, b, h + 2 * g }, { x + w + g, y - g, b, h + 2 * g } };
    for (int i = 0; i < 4; i++) {
        if (!wborder[i]) {
            XSetWindowAttributes a;
            a.override_redirect = True;
            a.background_pixel = 0xEE3F35;
            wborder[i] = XCreateWindow(dpy, root, 0, 0, 10, 10, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
            set_props(wborder[i], "录制边框", 0);
        }
        XMoveResizeWindow(dpy, wborder[i], rs[i].x, rs[i].y, rs[i].w > 0 ? rs[i].w : 1, rs[i].h > 0 ? rs[i].h : 1);
        XMapRaised(dpy, wborder[i]);
    }
    borders_on = 1;
}
static void borders_hide(void) {
    for (int i = 0; i < 4; i++) if (wborder[i]) XUnmapWindow(dpy, wborder[i]);
    borders_on = 0;
}

static void bar_place(int x, int y, int w, int h) {
    int bw = wbar.W, bh = wbar.H;
    int cx = x + w / 2, cy = y + h / 2;
    /* 多屏时优先放到未被录制的屏幕上 */
    for (int i = 0; i < g_nmons && g_nmons > 1; i++) {
        Mon *m = &g_mons[i];
        int inter = !(m->x >= x + w || m->x + m->w <= x || m->y >= y + h || m->y + m->h <= y);
        if (!inter) { int bx = m->x + (m->w - bw) / 2, by = m->y + (int)P(40); fixed_size(wbar.w, bw, bh, bx, by); XMoveWindow(dpy, wbar.w, bx, by); return; }
    }
    Mon *mm = &g_mons[0];
    for (int i = 0; i < g_nmons; i++) {
        Mon *m = &g_mons[i];
        if (cx >= m->x && cx < m->x + m->w && cy >= m->y && cy < m->y + m->h) mm = m;
    }
    int bx = mm->x + (mm->w - bw) / 2, by = mm->y + (int)P(12);
    /* 区域录制时尽量放在区域外 */
    if (!(x == mm->x && y == mm->y && w == mm->w && h == mm->h)) {
        if (y - mm->y > bh + P(20)) { by = y - bh - (int)P(14); bx = x + (w - bw) / 2; }
        else if (mm->y + mm->h - (y + h) > bh + P(60)) { by = y + h + (int)P(14); bx = x + (w - bw) / 2; }
        if (bx < mm->x) bx = mm->x + 4;
        if (bx + bw > mm->x + mm->w) bx = mm->x + mm->w - bw - 4;
    }
    fixed_size(wbar.w, bw, bh, bx, by);
    XMoveWindow(dpy, wbar.w, bx, by);
}

static void bar_show(void) {
    if (bar_hidden) return;
    XMapRaised(dpy, wbar.w);
    Atom st[3] = { A_ABOVE, A_SKIPTB, A_SKIPPG };
    set_state_atoms(wbar.w, st, 3);
    wbar.mapped = 1;
    win_redraw(&wbar);
}
static void bar_hide(void) {
    if (wbar.mapped) { XUnmapWindow(dpy, wbar.w); wbar.mapped = 0; }
}

static void cd_show(int x, int y, int w, int h) {
    int cw = wcd.W, ch = wcd.H;
    XMoveWindow(dpy, wcd.w, x + (w - cw) / 2, y + (h - ch) / 2);
    XMapRaised(dpy, wcd.w);
    wcd.mapped = 1;
    win_redraw(&wcd);
}
static void cd_hide(void) {
    if (wcd.mapped) { XUnmapWindow(dpy, wcd.w); wcd.mapped = 0; }
}

static void do_start(void) {
    int x = pending_x, y = pending_y, w = pending_w, h = pending_h;
    warn_msg[0] = 0;
    if (rec_begin(dpy, x, y, w, h) != 0) {
        set_status(rec.err, C_RED);
        main_show();
        return;
    }
    if (rec.warn[0]) snprintf(warn_msg, sizeof warn_msg, "%s", rec.warn);
    else if (cfg.audio != SY_AUDIO_SILENCE && audio_level(0) < 0 && audio_level(1) < 0)
        snprintf(warn_msg, sizeof warn_msg, "%s", audio_error()[0] ? audio_error() : "声音设备不可用，本次为无声录制");
    else if (cfg.audio == SY_AUDIO_BOTH && (audio_level(0) < 0 || audio_level(1) < 0))
        snprintf(warn_msg, sizeof warn_msg, "%s", audio_error());
    status_msg[0] = 0;
    bar_place(x, y, w, h);
    bar_show();
    borders_show(x, y, w, h);
    XFlush(dpy);
}

static void request_start(void) {
    if (rec.state != REC_IDLE || counting) return;
    if (cfg.area == AREA_REGION && (cfg.rw <= 0 || cfg.rh <= 0)) {
        select_then_start = 1;
        return;          /* 主循环里执行框选（需先隐藏主窗口） */
    }
    seg_focus = 0;
    clamp_seg();
    cfg_save();
    mons_refresh(dpy);
    target_rect(&pending_x, &pending_y, &pending_w, &pending_h);
    if (pending_w < 16 || pending_h < 16) { set_status("录制区域无效，请重新框选", C_RED); return; }
    main_hide();
    bar_hidden = 0;
    rec.err[0] = 0;
    counting = 1;
    cd_until = now_s() + (cfg.countdown > 0 ? cfg.countdown : 0);
    if (cfg.countdown > 0) cd_show(pending_x, pending_y, pending_w, pending_h);
    pending_start_at = 0;
}

static void cancel_countdown(void) {
    cd_hide();
    counting = 0;
    pending_start_at = 0;
    main_show();
}

static void request_stop(void) {
    if (counting) { cancel_countdown(); return; }
    if (rec.state != REC_RUNNING && rec.state != REC_PAUSED) return;
    rec_stop();
    bar_hide();
    borders_hide();
    set_status("", C_TEXT);
    main_show();
}

static void toggle_pause(void) {
    if (rec.state == REC_RUNNING) rec_pause();
    else if (rec.state == REC_PAUSED) rec_resume();
    if (wbar.mapped) win_redraw(&wbar);
}

static void do_select(int then_start) {
    int was = wmain.mapped;
    main_hide();
    XSync(dpy, False);
    usleep(250000);          /* 等待窗口管理器收起主窗口 */
    Rect r;
    if (run_selector(&r)) {
        cfg.area = AREA_REGION;
        cfg.rx = r.x; cfg.ry = r.y; cfg.rw = r.w & ~1; cfg.rh = r.h & ~1;
        cfg_save();
        if (then_start) { request_start(); return; }
    }
    if (was || !then_start) main_show();
    else main_show();
}

/* ---------------- 点击处理 ---------------- */
static void on_click(Win *w, int id, int arg) {
    if (w == &wmain) {
        if ((rec.state != REC_IDLE || counting) && id != H_OPEN_DIR && id != H_RES_PLAY && id != H_RES_DIR) return;
        if (id != H_SEG && seg_focus) { seg_focus = 0; clamp_seg(); }
        switch (id) {
        case H_AREA:
            cfg.area = arg;
            if (arg == AREA_REGION && cfg.rw <= 0) { cfg_save(); do_select(0); return; }
            break;
        case H_MON_PREV: cfg.monitor = cfg.monitor <= -1 ? g_nmons - 1 : cfg.monitor - 1; break;
        case H_MON_NEXT: cfg.monitor = cfg.monitor >= g_nmons - 1 ? -1 : cfg.monitor + 1; break;
        case H_RESELECT: do_select(0); return;
        case H_AUDIO:
            if (arg >= 0 && arg < n_aud) {
                cfg.audio = ui_aud[arg].id;
                cfg.audio_dev[0] = 0;
                dev_i = -1;
                audio_before = cfg.audio;
            }
            if (cfg.audio != SY_AUDIO_SILENCE && !audio_available()) set_status(audio_error(), C_ORANGE);
            break;
        case H_FPS: { static const int f[] = { 10, 15, 24, 30 }; cfg.fps = f[arg]; break; }
        case H_QUAL: cfg.quality = arg; break;
        case H_SCALE: cfg.scale = arg; break;
        case H_FMT: if (arg >= 0 && arg < n_cont) cfg.fmt = ui_cont[arg].id; break;
        case H_CD: cfg.countdown = arg == 0 ? 0 : arg == 1 ? 3 : 5; break;
        case H_CODEC: if (arg >= 0 && arg < n_codec) cfg.codec = ui_codec[arg].id; break;
        case H_CURSOR: if (arg >= 0 && arg < n_curs) cfg.cursor = ui_curs[arg].id; break;
        case H_SEG:
            seg_focus = 1;
            XSetInputFocus(dpy, wmain.w, RevertToParent, CurrentTime);
            break;
        case H_UNIT:
            cfg.segment_unit = arg ? SY_UNIT_GB : SY_UNIT_MB;
            if (!seg_focus) clamp_seg();
            break;
        case H_DEV_PREV: cycle_dev(-1); break;
        case H_DEV_NEXT: cycle_dev(1); break;
        case H_OPEN_DIR: mkdir_p(cfg.outdir); open_path(cfg.outdir); return;
        case H_CHANGE_DIR: pick_open(); return;
        case H_START: request_start(); return;
        case H_RES_PLAY: if (rec.result[0]) open_path(rec.result); return;
        case H_RES_DIR: open_path(cfg.outdir); return;
        }
        cfg_save();
        win_redraw(&wmain);
    } else if (w == &wbar) {
        if (id == H_BAR_PAUSE) toggle_pause();
        else if (id == H_BAR_STOP) request_stop();
        else if (id == H_BAR_HIDE) { bar_hidden = 1; bar_hide(); }
    } else if (w == &wpick) {
        char p[1400];
        switch (id) {
        case H_PK_ENTRY:
            snprintf(p, sizeof p, "%s/%s", strcmp(pk_dir, "/") ? pk_dir : "", pk_names[arg]);
            pk_load(p);
            break;
        case H_PK_UP: {
            snprintf(p, sizeof p, "%s", pk_dir);
            char *sl = strrchr(p, '/');
            if (sl && sl != p) *sl = 0; else snprintf(p, sizeof p, "/");
            pk_load(p);
            break;
        }
        case H_PK_QUICK: {
            const char *h = home_dir();
            if (arg == 0) snprintf(p, sizeof p, "%s", h);
            else if (arg == 1) { snprintf(p, sizeof p, "%s/桌面", h); if (access(p, F_OK)) snprintf(p, sizeof p, "%s/Desktop", h); }
            else if (arg == 2) { snprintf(p, sizeof p, "%s", cfg.outdir); char *sl = strrchr(p, '/'); if (sl) *sl = 0; }
            else {
                char u[300];
                const char *user = getenv("USER");
                snprintf(u, sizeof u, "/media/%s", user ? user : "");
                snprintf(p, sizeof p, "%s", access(u, F_OK) == 0 ? u : "/media");
            }
            pk_load(p);
            break;
        }
        case H_PK_CANCEL: pick_close(); return;
        case H_PK_OK:
            if (access(pk_dir, W_OK) != 0) { set_status("该目录没有写入权限，请换一个目录", C_RED); pick_close(); win_redraw(&wmain); return; }
            snprintf(cfg.outdir, sizeof cfg.outdir, "%s", pk_dir);
            cfg_save();
            pick_close();
            set_status("", C_TEXT);
            win_redraw(&wmain);
            return;
        }
        w->hov = 0;
        win_redraw(&wpick);
    } else if (w == &wcd) {
        cancel_countdown();
    }
}

/* ---------------- 全局快捷键 ---------------- */
static int grab_failed;
static int xerr_grab(Display *d, XErrorEvent *e) { (void)d; if (e->error_code == BadAccess) grab_failed = 1; return 0; }
static KeyCode kc_r, kc_p, kc_h;

static void grab_keys(void) {
    kc_r = XKeysymToKeycode(dpy, XK_r);
    kc_p = XKeysymToKeycode(dpy, XK_p);
    kc_h = XKeysymToKeycode(dpy, XK_h);
    unsigned int mods[4] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    XErrorHandler old = XSetErrorHandler(xerr_grab);
    for (int i = 0; i < 4; i++) {
        unsigned int m = ControlMask | Mod1Mask | mods[i];
        XGrabKey(dpy, kc_r, m, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, kc_p, m, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, kc_h, m, root, True, GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);
    XSetErrorHandler(old);
    hotkey_conflict = grab_failed;
}

enum { CMD_SHOW = 1, CMD_TOGGLE = 2, CMD_PAUSE = 3, CMD_STOP = 4, CMD_QUIT = 5 };

static void handle_cmd(int c) {
    switch (c) {
    case CMD_SHOW:
        if (counting) break;
        if (rec.state == REC_IDLE || rec.state == REC_SAVING) main_show();
        else if (bar_hidden) { bar_hidden = 0; bar_show(); }
        break;
    case CMD_TOGGLE:
        if (counting) { cancel_countdown(); break; }
        if (rec.state == REC_IDLE) request_start();
        else request_stop();
        break;
    case CMD_PAUSE: toggle_pause(); break;
    case CMD_STOP: request_stop(); break;
    case CMD_QUIT: quit_req = 1; break;
    }
}

/* ---------------- 事件分发 ---------------- */
static int bar_drag, drag_dx, drag_dy;

static Win *win_of(Window w) {
    if (w == wmain.w) return &wmain;
    if (w == wbar.w) return &wbar;
    if (w == wcd.w) return &wcd;
    if (wpick.w && w == wpick.w) return &wpick;
    return NULL;
}

static void find_hit(Win *w, int x, int y, int *id, int *arg) {
    *id = 0; *arg = 0;
    for (int i = w->nh - 1; i >= 0; i--) {
        Hit *h = &w->hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) { *id = h->id; *arg = h->arg; return; }
    }
}

static void handle_event(XEvent *ev) {
    if (ev->type == KeyPress && (ev->xkey.state & ControlMask) && (ev->xkey.state & Mod1Mask)) {
        KeyCode k = ev->xkey.keycode;
        if (k == kc_r) { handle_cmd(CMD_TOGGLE); return; }
        if (k == kc_p) { handle_cmd(CMD_PAUSE); return; }
        if (k == kc_h) {
            if (counting) { main_show(); return; }
            if (rec.state == REC_RUNNING || rec.state == REC_PAUSED) {
                if (bar_hidden || !wbar.mapped) { bar_hidden = 0; bar_show(); }
                else { bar_hidden = 1; bar_hide(); }
            } else main_show();
            return;
        }
    }
    if (ev->type == KeyPress && seg_focus) {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab) {
            seg_focus = 0;
            clamp_seg();
            cfg_save();
            win_redraw(&wmain);
            return;
        }
        if (rec.state == REC_IDLE && !counting) {
            int d = -1;
            if (ks >= XK_0 && ks <= XK_9) d = (int)(ks - XK_0);
            else if (ks >= XK_KP_0 && ks <= XK_KP_9) d = (int)(ks - XK_KP_0);
            if (ks == XK_BackSpace || ks == XK_Delete) cfg.segment_n /= 10;
            else if (d >= 0) {
                long n = (long)cfg.segment_n * 10 + d;
                if (n <= 9999) cfg.segment_n = (int)n;
            }
            win_redraw(&wmain);
        }
        return;
    }
    if (ev->type == ClientMessage) {
        if (ev->xclient.message_type == A_CMD) { handle_cmd((int)ev->xclient.data.l[0]); return; }
        if (ev->xclient.message_type == A_PROTO && (Atom)ev->xclient.data.l[0] == A_DEL) {
            if (ev->xclient.window == wmain.w) {
                if (rec.state == REC_SAVING) { set_status("正在保存，完成后再关闭", C_ORANGE); win_redraw(&wmain); }
                else quit_req = 1;
            } else if (wpick.w && ev->xclient.window == wpick.w) pick_close();
            return;
        }
    }
    if (ev->type == SelectionClear) { /* 另一实例接管（不应发生），忽略 */ return; }
    Win *w = win_of(ev->xany.window);
    if (!w) return;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0) { if (!w->nh) win_redraw(w); else win_present(w); }
        break;
    case MapNotify: w->mapped = 1; win_redraw(w); break;
    case UnmapNotify: if (w != &wmain) w->mapped = 0; break;
    case MotionNotify: {
        while (XCheckTypedWindowEvent(dpy, w->w, MotionNotify, ev)) {}
        if (w == &wbar && bar_drag) {
            XMoveWindow(dpy, wbar.w, ev->xmotion.x_root - drag_dx, ev->xmotion.y_root - drag_dy);
            break;
        }
        int id, arg;
        find_hit(w, ev->xmotion.x, ev->xmotion.y, &id, &arg);
        if (id != w->hov || arg != w->hova) { w->hov = id; w->hova = arg; win_redraw(w); }
        break;
    }
    case LeaveNotify:
        if (w->hov) { w->hov = 0; win_redraw(w); }
        break;
    case ButtonPress:
        if (ev->xbutton.button == 4 || ev->xbutton.button == 5) {
            if (w == &wpick) { pk_scroll += ev->xbutton.button == 4 ? -3 : 3; win_redraw(w); }
            break;
        }
        if (ev->xbutton.button != 1) break;
        find_hit(w, ev->xbutton.x, ev->xbutton.y, &w->prs, &w->prsa);
        if (w == &wbar && !w->prs) {
            Window ch; int bx, by;
            XTranslateCoordinates(dpy, wbar.w, root, 0, 0, &bx, &by, &ch);
            bar_drag = 1; drag_dx = ev->xbutton.x_root - bx; drag_dy = ev->xbutton.y_root - by;
        }
        if (w == &wcd) cancel_countdown();
        break;
    case ButtonRelease:
        if (ev->xbutton.button != 1) break;
        if (bar_drag) { bar_drag = 0; break; }
        {
            int id, arg;
            find_hit(w, ev->xbutton.x, ev->xbutton.y, &id, &arg);
            if (id && id == w->prs && arg == w->prsa) on_click(w, id, arg);
            w->prs = 0;
        }
        break;
    }
}

/* ---------------- 定时逻辑 ---------------- */
static double last_bar;
static void tick(void) {
    double t = now_s();
    rec_poll();
    if (counting) {
        if (t >= cd_until) {
            if (wcd.mapped) { cd_hide(); XSync(dpy, False); pending_start_at = t + 0.3; }
            else if (!pending_start_at) pending_start_at = t + 0.35;
            if (t >= pending_start_at) { pending_start_at = 0; counting = 0; do_start(); }
        } else if (wcd.mapped) win_redraw(&wcd);
    }
    if ((rec.state == REC_RUNNING || rec.state == REC_PAUSED) && wbar.mapped && t - last_bar > 0.1) {
        last_bar = t;
        win_redraw(&wbar);
    }
    /* 录制意外结束：收起控制条并回到主窗口 */
    if (rec.state == REC_SAVING && (wbar.mapped || borders_on)) { bar_hide(); borders_hide(); main_show(); }
    if (ui_state_prev == REC_SAVING && rec.state == REC_IDLE) {
        if (rec.result[0]) {
            const char *nm = strrchr(rec.result, '/');
            char d1[32], msg[1400];
            fmt_dur(d1, sizeof d1, rec.result_dur);
            const char *base = nm ? nm + 1 : rec.result;
            if (rec.recovered) snprintf(msg, sizeof msg, "已恢复上次未保存的录像：%s（%.1f MB）", base, rec.result_size_mb);
            else if (rec.sealed_count > 1)
                snprintf(msg, sizeof msg, "已保存 %d 段，最新 %s（%s，%.1f MB）", rec.sealed_count, base, d1, rec.result_size_mb);
            else snprintf(msg, sizeof msg, "已保存 %s（%s，%.1f MB）", base, d1, rec.result_size_mb);
            if (rec.warn[0]) snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "　%s", rec.warn);
            if (rec.err[0]) snprintf(msg + strlen(msg), sizeof msg - strlen(msg), "　注意：%s", rec.err);
            set_status(msg, rec.err[0] ? C_ORANGE : C_GREEN);
            if (rec.err[0]) status_col = C_GREEN;   /* 仍显示播放链接 */
        } else set_status(rec.err[0] ? rec.err : "保存失败", C_RED);
        main_show();
        win_redraw(&wmain);
    }
    if (ui_state_prev != rec.state && wmain.mapped) win_redraw(&wmain);
    ui_state_prev = rec.state;
    if (select_then_start) {
        select_then_start = 0;
        do_select(1);
    }
}

/* ---------------- 启动 ---------------- */
static void on_signal(int s) { (void)s; quit_req = 1; }

static double detect_scale(void) {
    const char *e = getenv("SHIYING_SCALE");
    if (e && atof(e) > 0.5) return atof(e);
    double dpi = 0;
    char *rms = XResourceManagerString(dpy);
    if (rms) {
        const char *p = strstr(rms, "Xft.dpi:");
        if (p) dpi = atof(p + 8);
    }
    if (dpi < 1) {
        const char *q = getenv("QT_SCALE_FACTOR");
        if (q && atof(q) > 0.5) return atof(q);
        return 1.0;
    }
    double s = dpi / 96.0;
    s = floor(s * 4 + 0.5) / 4;
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    return s;
}

static void init_atoms(void) {
#define AT(v, n) v = XInternAtom(dpy, n, False)
    AT(A_DEL, "WM_DELETE_WINDOW"); AT(A_PROTO, "WM_PROTOCOLS"); AT(A_ICON, "_NET_WM_ICON");
    AT(A_STATE, "_NET_WM_STATE"); AT(A_ABOVE, "_NET_WM_STATE_ABOVE"); AT(A_SKIPTB, "_NET_WM_STATE_SKIP_TASKBAR");
    AT(A_SKIPPG, "_NET_WM_STATE_SKIP_PAGER"); AT(A_TYPE, "_NET_WM_WINDOW_TYPE"); AT(A_T_DOCK, "_NET_WM_WINDOW_TYPE_DOCK");
    AT(A_T_DIALOG, "_NET_WM_WINDOW_TYPE_DIALOG"); AT(A_T_UTIL, "_NET_WM_WINDOW_TYPE_UTILITY"); AT(A_T_DESKTOP, "_NET_WM_WINDOW_TYPE_DESKTOP");
    AT(A_CLIENTS, "_NET_CLIENT_LIST_STACKING"); AT(A_FRAME, "_NET_FRAME_EXTENTS"); AT(A_GTKFRAME, "_GTK_FRAME_EXTENTS");
    AT(A_NAME, "_NET_WM_NAME"); AT(A_UTF8, "UTF8_STRING"); AT(A_MOTIF, "_MOTIF_WM_HINTS"); AT(A_PID, "_NET_WM_PID");
    AT(A_SINGLE, "_SHIYING_INSTANCE"); AT(A_CMD, "_SHIYING_COMMAND"); AT(A_ACTIVE, "_NET_ACTIVE_WINDOW");
#undef AT
}

static void exe_dir(void) {
    char p[1024];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) { snprintf(g_exe_dir, sizeof g_exe_dir, "."); return; }
    p[n] = 0;
    char *sl = strrchr(p, '/');
    if (sl) *sl = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", p);
}

/* 自检：在目标机终端运行 shiying --check，逐项列出运行条件 */
static int self_check(void) {
    int bad = 0;
    printf("拾影录屏 %s 自检\n程序目录：%s\n", APP_VERSION, g_exe_dir);
    const char *ff = ffmpeg_path();
    printf("[%s] 录制引擎：%s\n", ff ? "通过" : "失败", ff ? ff : "未找到 录制引擎/ffmpeg");
    if (!ff) bad++;
    else {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "'%s' -hide_banner -devices 2>/dev/null | grep -c x11grab", ff);
        FILE *p = popen(cmd, "r");
        int n = 0;
        if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
        printf("[%s] 引擎可运行且含屏幕采集（x11grab）\n", n > 0 ? "通过" : "失败");
        if (n <= 0) bad++;
    }
    Display *d = XOpenDisplay(NULL);
    printf("[%s] 图形界面连接（DISPLAY=%s）\n", d ? "通过" : "失败", getenv("DISPLAY") ? getenv("DISPLAY") : "");
    if (!d) bad++;
    const char *wl = getenv("WAYLAND_DISPLAY");
    if (wl && *wl) printf("[提示] 检测到 Wayland 会话，屏幕采集只支持 X11 会话，请在登录界面选择 X11/UKUI 会话\n");
    int fr = font_init();
    printf("[%s] 中文字体：%s\n", fr == 0 ? "通过" : "失败", fr == 0 ? font_path() : (fr == -1 ? "缺少 libfreetype" : "未找到中文字体"));
    if (fr) bad++;
    if (d) {
        mons_refresh(d);
        printf("[通过] 显示器 %d 块，桌面总尺寸 %d × %d\n", g_nmons, g_root_w, g_root_h);
        for (int i = 0; i < g_nmons; i++) printf("       屏幕 %d：%d × %d @ (%d, %d)%s\n", i + 1, g_mons[i].w, g_mons[i].h, g_mons[i].x, g_mons[i].y, g_mons[i].primary ? " 主屏" : "");
        int comp = XGetSelectionOwner(d, XInternAtom(d, "_NET_WM_CM_S0", False)) != None;
        printf("[信息] 窗口合成：%s\n", comp ? "开启（控制条为圆角半透明）" : "关闭");
    }
    int pa = audio_available();
    printf("[%s] 声音服务库 libpulse-simple：%s\n", pa ? "通过" : "提示", pa ? "可用" : "不可用，只能无声录制");
    if (pa) {
        int r = audio_start(SY_AUDIO_BOTH, NULL);
        printf("[%s] 打开系统声音与麦克风%s%s\n", r == 0 && !audio_error()[0] ? "通过" : "提示", audio_error()[0] ? "：" : "", audio_error());
        if (r == 0) audio_stop();
    }
    printf("[信息] 保存目录：%s（%s）\n", cfg.outdir, access(cfg.outdir, W_OK) == 0 ? "可写" : "尚未创建或不可写，首次录制时自动创建");
    printf("%s\n", bad ? "自检未通过，请按上面失败项处理。" : "自检通过。");
    if (d) XCloseDisplay(d);
    return bad ? 1 : 0;
}

static void usage(void) {
    printf("拾影录屏 %s\n用法：拾影录屏 [--toggle|--pause|--stop|--show|--quit|--version]\n"
           "  --toggle  开始/停止录制（可绑定到系统自定义快捷键）\n"
           "  --pause   暂停/继续\n  --stop    停止并保存\n  --check   运行环境自检\n", APP_VERSION);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    int cmd = CMD_SHOW, check = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--toggle")) cmd = CMD_TOGGLE;
        else if (!strcmp(argv[i], "--pause")) cmd = CMD_PAUSE;
        else if (!strcmp(argv[i], "--stop")) cmd = CMD_STOP;
        else if (!strcmp(argv[i], "--show")) cmd = CMD_SHOW;
        else if (!strcmp(argv[i], "--quit")) cmd = CMD_QUIT;
        else if (!strcmp(argv[i], "--version")) { printf("%s\n", APP_VERSION); return 0; }
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    }
    exe_dir();
    const char *xc = getenv("XDG_CACHE_HOME"), *xf = getenv("XDG_CONFIG_HOME");
    if (xc && *xc) snprintf(g_cache_dir, sizeof g_cache_dir, "%s/shiying", xc);
    else snprintf(g_cache_dir, sizeof g_cache_dir, "%s/.cache/shiying", home_dir());
    if (xf && *xf) snprintf(g_conf_dir, sizeof g_conf_dir, "%s/shiying", xf);
    else snprintf(g_conf_dir, sizeof g_conf_dir, "%s/.config/shiying", home_dir());
    mkdir_p(g_cache_dir);
    if (check) { cfg_load(); return self_check(); }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "无法连接图形界面（DISPLAY=%s）\n", getenv("DISPLAY") ? getenv("DISPLAY") : ""); return 1; }
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    init_atoms();

    /* 单实例：已在运行则把命令转给它 */
    Window owner = XGetSelectionOwner(dpy, A_SINGLE);
    if (owner != None) {
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = owner;
        e.xclient.message_type = A_CMD;
        e.xclient.format = 32;
        e.xclient.data.l[0] = cmd;
        XSendEvent(dpy, owner, False, NoEventMask, &e);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return 0;
    }
    if (cmd == CMD_QUIT || cmd == CMD_STOP || cmd == CMD_PAUSE) { XCloseDisplay(dpy); return 0; }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (font_init() != 0) {
        fprintf(stderr, "未找到可用的中文字体或 FreeType 库\n");
        return 2;
    }
    S = detect_scale();
    has_comp = XGetSelectionOwner(dpy, XInternAtom(dpy, "_NET_WM_CM_S0", False)) != None;
    has_argb = has_comp && XMatchVisualInfo(dpy, scr, 32, TrueColor, &argb_vi);
    cfg_load();
    mons_refresh(dpy);
    logf_("start v%s exe=%s scale=%.2f comp=%d mons=%d font=%s ffmpeg=%s", APP_VERSION, g_exe_dir, S, has_comp, g_nmons, font_path(),
          ffmpeg_path() ? ffmpeg_path() : "(none)");

    /* 主窗口 */
    int MW = (int)P(MAIN_W), MH = (int)P(MAIN_H);
    win_create(&wmain, 0, 0, MW, MH, 0, 0, APP_NAME, 0);
    wmain.draw = draw_main;
    XSetSelectionOwner(dpy, A_SINGLE, wmain.w, CurrentTime);
    load_caps();
    /* 控制条 */
    win_create(&wbar, 0, 0, (int)P(BAR_W), (int)P(BAR_H), 1, 0, "拾影录屏 · 录制中", A_T_DOCK);
    wbar.draw = draw_bar;
    no_decor(wbar.w);
    no_focus(wbar.w);
    fixed_size(wbar.w, wbar.W, wbar.H, 0, 0);
    /* 倒计时 */
    win_create(&wcd, 0, 0, (int)P(150), (int)P(150), 1, 1, "倒计时", 0);
    wcd.draw = draw_cd;

    grab_keys();
    if (!ffmpeg_path()) set_status("找不到录制引擎（录制引擎/ffmpeg），请保持程序目录完整", C_RED);
    if (rec_recover()) ui_state_prev = REC_SAVING;
    if (cmd == CMD_TOGGLE) request_start();
    else main_show();

    int xfd = ConnectionNumber(dpy);
    while (1) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle_event(&ev);
        }
        if (quit_req) {
            if (counting) { counting = 0; pending_start_at = 0; cd_hide(); }
            else if (rec.state == REC_RUNNING || rec.state == REC_PAUSED) request_stop();
            if (!counting && rec.state == REC_IDLE) break;
        }
        int busy = counting || rec.state != REC_IDLE || wbar.mapped;
        struct pollfd pf = { xfd, POLLIN, 0 };
        poll(&pf, 1, busy ? 50 : 400);
        tick();
        XFlush(dpy);
    }
    cfg_save();
    XCloseDisplay(dpy);
    return 0;
}
