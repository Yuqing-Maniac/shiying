#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "cfg.h"
#include "path.h"

#define IDI_APP 1

enum {
    ID_MON = 101, ID_AUD, ID_FPS, ID_QUAL, ID_SCALE, ID_FMT, ID_CD, ID_CODEC, ID_CURS,
    ID_SEG, ID_UNIT, ID_DEV, ID_PREV, ID_NEXT, ID_DIR, ID_BROWSE, ID_OPEN,
    ID_START, ID_PAUSE, ID_STOP, ID_STATUS, ID_TIMER = 1
};

static HWND hwnd, hmon, haud, hfps, hqual, hscale, hfmt, hcd, hcodec, hcurs;
static HWND hseg, hunit, hdev, hprev, hnext, hdir, hbrowse, hopen, hstart, hpause, hstop, hstatus;
static HFONT font;
static SySession *sess;
static SyCaps caps;
static SyDevice devs[32];
static SyChoice ch_codec[4], ch_cont[4], ch_curs[4], ch_aud[4];
static int n_codec, n_cont, n_curs, n_aud, n_dev, dev_i = -1, audio_before = SY_AUDIO_SYSTEM;
static char ff_path[1200];
static int counting, quit_after;
static double cd_until;
static SyPhase phase;

typedef struct { int x, y, w, h; } Mon;
static Mon mons[16];
static int nmons;

static BOOL CALLBACK mon_cb(HMONITOR h, HDC dc, LPRECT r, LPARAM p) {
    (void)h; (void)dc; (void)p;
    if (nmons >= 16 || !r) return TRUE;
    mons[nmons].x = r->left;
    mons[nmons].y = r->top;
    mons[nmons].w = r->right - r->left;
    mons[nmons].h = r->bottom - r->top;
    nmons++;
    return TRUE;
}

static void load_mons(void) {
    nmons = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
}

static void slash(char *s) {
    if (!s) return;
    for (; *s; s++) if (*s == '\\') *s = '/';
}

static int to_wide(const char *s, wchar_t *out, int n) {
    if (!s) s = "";
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0 ? 0 : -1;
}

static void to_u8(const wchar_t *w, char *out, size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
}

static void set_u8(HWND h, const char *s) {
    wchar_t w[1400];
    if (to_wide(s ? s : "", w, 1400) != 0) return;
    SetWindowTextW(h, w);
}

static void bind_console(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return;
    HANDLE err;
    if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &err, 0, FALSE, DUPLICATE_SAME_ACCESS)) err = out;
    SetStdHandle(STD_OUTPUT_HANDLE, out);
    SetStdHandle(STD_ERROR_HANDLE, err);
    int fd1 = _open_osfhandle((intptr_t)out, _O_TEXT);
    int fd2 = _open_osfhandle((intptr_t)err, _O_TEXT);
    if (fd1 >= 0) _dup2(fd1, 1);
    if (fd2 >= 0) _dup2(fd2, 2);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void set_dirs(void) {
    wchar_t w[1024];
    if (!GetModuleFileNameW(NULL, w, 1024)) return;
    char u8[1200];
    to_u8(w, u8, sizeof u8);
    slash(u8);
    char *sl = strrchr(u8, '/');
    if (sl) *sl = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", u8);
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s/.cache/shiying", home_dir());
    snprintf(g_conf_dir, sizeof g_conf_dir, "%s/.config/shiying", home_dir());
    slash(g_cache_dir);
    slash(g_conf_dir);
    mkdir_p(g_cache_dir);
    mkdir_p(g_conf_dir);
}

static int self_check(void) {
    bind_console();
    printf("拾影录屏 %s 自检\n", APP_VERSION);
    char ff[1200];
    if (sy_ffmpeg_resolve(ff, sizeof ff) != 0) {
        printf("[失败] 录制引擎：未找到 录制引擎/ffmpeg.exe\n自检未通过。\n");
        return 1;
    }
    printf("[通过] 录制引擎：%s\n", ff);
    SyCaps c;
    memset(&c, 0, sizeof c);
    if (sy_probe(ff, &c) != 0) {
        printf("[失败] 引擎未能运行\n自检未通过。\n");
        return 1;
    }
    printf("[%s] H.264\n", c.h264 ? "通过" : "无");
    printf("[%s] H.265\n", c.h265 ? "通过" : "无");
    printf("[%s] gdigrab\n", c.grab_gdi ? "通过" : "无");
    printf("[%s] ddagrab\n", c.grab_dda ? "通过" : "无");
    int ok = c.grab_gdi || c.grab_dda;
    printf("%s\n", ok ? "自检通过。" : "自检未通过，引擎没有列出屏幕采集。");
    return ok ? 0 : 1;
}

static void usage(void) {
    bind_console();
    printf("拾影录屏 %s\n用法：拾影录屏.exe [--check|--version|--help]\n"
           "  --check   检查录制引擎能否运行，以及是否列出屏幕采集。不会开始录屏。\n", APP_VERSION);
}

static HWND make_lab(const wchar_t *t, int x, int y, int w) {
    return CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, x, y, w, 24, hwnd, NULL, NULL, NULL);
}

static HWND make_combo(int id, int x, int y, int w) {
    return CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                         x, y, w, 220, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
}

static HWND make_btn(const wchar_t *t, int id, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, w, h, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
}

static void add_item(HWND cb, const wchar_t *text, int data) {
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0) SendMessageW(cb, CB_SETITEMDATA, i, (LPARAM)data);
}

static int item_data(HWND cb) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return -1;
    return (int)SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

static void select_data(HWND cb, int data) {
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        if ((int)SendMessageW(cb, CB_GETITEMDATA, i, 0) == data) {
            SendMessageW(cb, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

static void add_u8(HWND cb, const char *text, int data) {
    wchar_t w[256];
    if (to_wide(text, w, 256) != 0) return;
    add_item(cb, w, data);
}

static void clamp_seg(void) {
    if (cfg.segment_n < 0) cfg.segment_n = 0;
    if (cfg.segment_n > 9999) cfg.segment_n = 9999;
    if (cfg.segment_unit == SY_UNIT_MB && cfg.segment_n > 0 && cfg.segment_n < 5) cfg.segment_n = 5;
}

static void write_seg(void) {
    char n[16];
    snprintf(n, sizeof n, "%d", cfg.segment_n);
    set_u8(hseg, n);
}

static void read_seg(void) {
    wchar_t w[16];
    GetWindowTextW(hseg, w, 16);
    char b[16];
    to_u8(w, b, sizeof b);
    cfg.segment_n = atoi(b);
    clamp_seg();
    write_seg();
}

static void show_dev(void) {
    const char *s = "未指定";
    if (cfg.audio == SY_AUDIO_DEVICE) {
        if (dev_i >= 0 && dev_i < n_dev) s = devs[dev_i].label;
        else if (cfg.audio_dev[0]) s = cfg.audio_dev;
    }
    set_u8(hdev, s);
}

static void show_dir(void) { set_u8(hdir, cfg.outdir); }

static void fill_mons(void) {
    SendMessageW(hmon, CB_RESETCONTENT, 0, 0);
    add_item(hmon, L"全部屏幕", -1);
    for (int i = 0; i < nmons; i++) {
        wchar_t t[80];
        _snwprintf(t, 80, L"屏幕 %d   %d × %d", i + 1, mons[i].w, mons[i].h);
        add_item(hmon, t, i);
    }
    if (cfg.area == AREA_REGION && cfg.rw >= 16 && cfg.rh >= 16) {
        wchar_t t[80];
        _snwprintf(t, 80, L"区域 %d × %d", cfg.rw & ~1, cfg.rh & ~1);
        add_item(hmon, t, -2);
        select_data(hmon, -2);
    } else select_data(hmon, cfg.monitor);
}

static void fill_choices(void) {
    SendMessageW(haud, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n_aud; i++) add_u8(haud, ch_aud[i].label, ch_aud[i].id);
    if (cfg.audio == SY_AUDIO_DEVICE) SendMessageW(haud, CB_SETCURSEL, (WPARAM)-1, 0);
    else select_data(haud, cfg.audio);

    SendMessageW(hfps, CB_RESETCONTENT, 0, 0);
    add_item(hfps, L"10", 10); add_item(hfps, L"15", 15); add_item(hfps, L"24", 24); add_item(hfps, L"30", 30);
    select_data(hfps, cfg.fps);

    SendMessageW(hqual, CB_RESETCONTENT, 0, 0);
    add_item(hqual, L"流畅", 0); add_item(hqual, L"标准", 1); add_item(hqual, L"高清", 2);
    select_data(hqual, cfg.quality);

    SendMessageW(hscale, CB_RESETCONTENT, 0, 0);
    add_item(hscale, L"原始", 0); add_item(hscale, L"1080P", 1); add_item(hscale, L"720P", 2);
    select_data(hscale, cfg.scale);

    SendMessageW(hfmt, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n_cont; i++) add_u8(hfmt, ch_cont[i].label, ch_cont[i].id);
    select_data(hfmt, cfg.fmt);

    SendMessageW(hcd, CB_RESETCONTENT, 0, 0);
    add_item(hcd, L"关闭", 0); add_item(hcd, L"3 秒", 3); add_item(hcd, L"5 秒", 5);
    select_data(hcd, cfg.countdown);

    SendMessageW(hcodec, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n_codec; i++) add_u8(hcodec, ch_codec[i].label, ch_codec[i].id);
    select_data(hcodec, cfg.codec);

    SendMessageW(hcurs, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n_curs; i++) add_u8(hcurs, ch_curs[i].label, ch_curs[i].id);
    select_data(hcurs, cfg.cursor);

    SendMessageW(hunit, CB_RESETCONTENT, 0, 0);
    add_item(hunit, L"MB", SY_UNIT_MB);
    add_item(hunit, L"GB", SY_UNIT_GB);
    select_data(hunit, cfg.segment_unit);
    write_seg();
    show_dev();
    show_dir();
}

static void load_engine(void) {
    memset(&caps, 0, sizeof caps);
    ff_path[0] = 0;
    if (sy_ffmpeg_resolve(ff_path, sizeof ff_path) != 0) ff_path[0] = 0;
    else sy_probe(ff_path, &caps);
    n_codec = sy_codec_choices(&caps, ch_codec, 4);
    n_cont = sy_container_choices(&caps, ch_cont, 4);
    n_curs = sy_cursor_choices(&caps, ch_curs, 4);
    n_aud = sy_audio_choices(ch_aud, 4);
    SyCodec c = (SyCodec)cfg.codec;
    SyContainer k = (SyContainer)cfg.fmt;
    SyCursor u = (SyCursor)cfg.cursor;
    sy_clamp_recorded(&c, &k, &u, &caps);
    cfg.codec = (int)c;
    cfg.fmt = (int)k;
    cfg.cursor = (int)u;
    n_dev = ff_path[0] ? sy_devices(devs, 32) : 0;
    dev_i = -1;
    if (cfg.audio == SY_AUDIO_DEVICE) {
        for (int i = 0; i < n_dev; i++)
            if (!strcmp(devs[i].id, cfg.audio_dev)) dev_i = i;
        audio_before = SY_AUDIO_SYSTEM;
    } else audio_before = cfg.audio;
}

static void plan_rect(int *x, int *y, int *w, int *h) {
    load_mons();
    if (cfg.area == AREA_REGION && cfg.rw >= 16 && cfg.rh >= 16) {
        *x = cfg.rx; *y = cfg.ry; *w = cfg.rw; *h = cfg.rh;
        return;
    }
    if (cfg.monitor >= 0 && cfg.monitor < nmons) {
        *x = mons[cfg.monitor].x; *y = mons[cfg.monitor].y;
        *w = mons[cfg.monitor].w; *h = mons[cfg.monitor].h;
        return;
    }
    if (nmons == 1) {
        *x = mons[0].x; *y = mons[0].y; *w = mons[0].w; *h = mons[0].h;
        return;
    }
    *x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static void base_name(const char *path, char *out, size_t n) {
    const char *s = path ? strrchr(path, '/') : NULL;
    const char *b = path ? strrchr(path, '\\') : NULL;
    if (b && (!s || b > s)) s = b;
    snprintf(out, n, "%s", s ? s + 1 : (path ? path : ""));
}

static void paint_status(const SyStatus *st) {
    char b[1400];
    if (!st) { set_u8(hstatus, ""); return; }
    if (st->phase == SY_PHASE_IDLE) {
        if (st->last_file[0]) {
            char base[400];
            base_name(st->last_file, base, sizeof base);
            double mb = (double)st->last_bytes / 1e6;
            if (st->sealed_count > 1)
                snprintf(b, sizeof b, "已保存 %d 段，最新 %s（%.1f MB）", st->sealed_count, base, mb);
            else snprintf(b, sizeof b, "已保存 %s（%.1f MB）", base, mb);
            if (st->recovered) {
                char t[1400];
                snprintf(t, sizeof t, "已恢复。%s", b);
                snprintf(b, sizeof b, "%s", t);
            }
        } else if (st->err[0]) snprintf(b, sizeof b, "%s", st->err);
        else snprintf(b, sizeof b, "%s", "就绪");
        if (st->warn[0] && st->last_file[0]) {
            size_t n = strlen(b);
            snprintf(b + n, sizeof b - n, "\n%s", st->warn);
        }
        if (st->err[0] && st->last_file[0]) {
            size_t n = strlen(b);
            snprintf(b + n, sizeof b - n, "\n%s", st->err);
        }
        set_u8(hstatus, b);
        return;
    }
    int sec = (int)st->elapsed;
    if (sec < 0) sec = 0;
    int n = snprintf(b, sizeof b, "%02d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    if (st->phase == SY_PHASE_PAUSED) n += snprintf(b + n, sizeof b - n, "  已暂停");
    if (st->phase == SY_PHASE_SEALING) n += snprintf(b + n, sizeof b - n, "  正在保存…");
    if (st->hold == SY_HOLD_CUT) n += snprintf(b + n, sizeof b - n, "  正在完成上一段");
    if (st->warn[0]) snprintf(b + n, sizeof b - n, "\n%s", st->warn);
    if (st->err[0]) snprintf(b, sizeof b, "%s", st->err);
    set_u8(hstatus, b);
}

static int locked(void) { return counting || sess != NULL; }

static void enable_settings(BOOL on) {
    HWND hs[] = { hmon, haud, hfps, hqual, hscale, hfmt, hcd, hcodec, hcurs, hseg, hunit, hprev, hnext, hbrowse };
    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++) EnableWindow(hs[i], on);
}

static void sync_ui(void) {
    enable_settings(locked() ? FALSE : TRUE);
    EnableWindow(hstart, locked() ? FALSE : TRUE);
    EnableWindow(hpause, sess && (phase == SY_PHASE_LIVE || phase == SY_PHASE_PAUSED));
    EnableWindow(hstop, counting || (sess && phase != SY_PHASE_IDLE));
    SetWindowTextW(hpause, phase == SY_PHASE_PAUSED ? L"继续" : L"暂停");
    if (counting) {
        int left = (int)(cd_until - now_s() + 0.999);
        if (left < 1) left = 1;
        char b[64];
        snprintf(b, sizeof b, "倒计时 %d", left);
        set_u8(hstatus, b);
    }
}

static void do_start(void) {
    if (sess) return;
    if (!ff_path[0]) {
        set_u8(hstatus, "找不到录制引擎（录制引擎/ffmpeg.exe），请保持程序目录完整");
        return;
    }
    read_seg();
    int x, y, w, h;
    plan_rect(&x, &y, &w, &h);
    if (w < 16 || h < 16) { set_u8(hstatus, "录制区域无效"); return; }
    SyPlan plan;
    memset(&plan, 0, sizeof plan);
    plan.x = x; plan.y = y; plan.w = w; plan.h = h;
    plan.fps = cfg.fps;
    plan.quality = cfg.quality;
    plan.scale = cfg.scale;
    plan.codec = (SyCodec)cfg.codec;
    plan.container = (SyContainer)cfg.fmt;
    plan.cursor = (SyCursor)cfg.cursor;
    plan.audio = (SyAudio)cfg.audio;
    plan.limit_bytes = sy_limit_bytes(cfg.segment_n, cfg.segment_unit);
    if (cfg.audio == SY_AUDIO_DEVICE) snprintf(plan.device_id, sizeof plan.device_id, "%s", cfg.audio_dev);
    snprintf(plan.outdir, sizeof plan.outdir, "%s", cfg.outdir);
    sy_clamp_recorded(&plan.codec, &plan.container, &plan.cursor, &caps);
    SyStatus st;
    memset(&st, 0, sizeof st);
    int rc = sy_session_open(&sess, &plan, ff_path, &caps, &st);
    if (rc != 0) {
        sess = NULL;
        set_u8(hstatus, st.err[0] ? st.err : "无法开始录制");
        return;
    }
    phase = st.phase;
    paint_status(&st);
}

static void request_start(void) {
    if (sess || counting) return;
    read_seg();
    cfg_save();
    if (cfg.countdown > 0) {
        counting = 1;
        cd_until = now_s() + cfg.countdown;
        sync_ui();
        return;
    }
    do_start();
}

static void on_sel(int id) {
    if (locked()) return;
    switch (id) {
    case ID_MON: {
        int d = item_data(hmon);
        if (d == -2) cfg.area = AREA_REGION;
        else { cfg.area = AREA_SCREEN; cfg.monitor = d; }
        break;
    }
    case ID_AUD: {
        int d = item_data(haud);
        if (d < 0) break;
        cfg.audio = d;
        cfg.audio_dev[0] = 0;
        dev_i = -1;
        audio_before = d;
        show_dev();
        break;
    }
    case ID_FPS: cfg.fps = item_data(hfps); break;
    case ID_QUAL: cfg.quality = item_data(hqual); break;
    case ID_SCALE: cfg.scale = item_data(hscale); break;
    case ID_FMT: cfg.fmt = item_data(hfmt); break;
    case ID_CD: cfg.countdown = item_data(hcd); break;
    case ID_CODEC: cfg.codec = item_data(hcodec); break;
    case ID_CURS: cfg.cursor = item_data(hcurs); break;
    case ID_UNIT:
        cfg.segment_unit = item_data(hunit);
        read_seg();
        break;
    default: return;
    }
    cfg_save();
}

static void cycle_dev(int dir) {
    if (locked() || n_dev <= 0) return;
    if (dir > 0) dev_i = dev_i >= n_dev - 1 ? -1 : dev_i + 1;
    else dev_i = dev_i <= -1 ? n_dev - 1 : dev_i - 1;
    if (dev_i >= 0) {
        if (cfg.audio != SY_AUDIO_DEVICE) audio_before = cfg.audio;
        cfg.audio = SY_AUDIO_DEVICE;
        snprintf(cfg.audio_dev, sizeof cfg.audio_dev, "%s", devs[dev_i].id);
        SendMessageW(haud, CB_SETCURSEL, (WPARAM)-1, 0);
    } else {
        cfg.audio_dev[0] = 0;
        cfg.audio = (audio_before >= SY_AUDIO_SILENCE && audio_before <= SY_AUDIO_BOTH) ? audio_before : SY_AUDIO_SYSTEM;
        select_data(haud, cfg.audio);
    }
    show_dev();
    cfg_save();
}

static void browse_dir(void) {
    if (locked()) return;
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择保存目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(id, path)) {
        to_u8(path, cfg.outdir, sizeof cfg.outdir);
        slash(cfg.outdir);
        show_dir();
        cfg_save();
    }
    CoTaskMemFree(id);
}

static void open_dir(void) {
    mkdir_p(cfg.outdir);
    wchar_t w[1200];
    if (to_wide(cfg.outdir, w, 1200) != 0) return;
    ShellExecuteW(hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void poll_sess(void) {
    if (!sess) return;
    sy_session_poll(sess);
    SyStatus st;
    sy_session_status(sess, &st);
    phase = st.phase;
    paint_status(&st);
    if (st.phase == SY_PHASE_IDLE) {
        sy_session_close(sess);
        sess = NULL;
        phase = SY_PHASE_IDLE;
    }
}

static BOOL CALLBACK set_child_font(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

static void create_controls(void) {
    int x = 16, w = 448, y = 16, lw = 64, cw = w - lw;
    make_lab(L"拾影录屏", x, y, w);
    y = 52;
    make_lab(L"屏幕", x, y, lw); hmon = make_combo(ID_MON, x + lw, y, cw);
    y = 92;
    make_lab(L"声音", x, y, lw); haud = make_combo(ID_AUD, x + lw, y, cw);
    y = 132;
    make_lab(L"帧率", x, y, lw); hfps = make_combo(ID_FPS, x + lw, y, 150);
    make_lab(L"画质", x + 230, y, lw); hqual = make_combo(ID_QUAL, x + 230 + lw, y, 154);
    y = 172;
    make_lab(L"分辨率", x, y, lw); hscale = make_combo(ID_SCALE, x + lw, y, 150);
    make_lab(L"格式", x + 230, y, lw); hfmt = make_combo(ID_FMT, x + 230 + lw, y, 154);
    y = 212;
    make_lab(L"倒计时", x, y, lw); hcd = make_combo(ID_CD, x + lw, y, 150);
    make_lab(L"编码", x + 230, y, lw); hcodec = make_combo(ID_CODEC, x + 230 + lw, y, 154);
    y = 252;
    make_lab(L"指针", x, y, lw); hcurs = make_combo(ID_CURS, x + lw, y, cw);
    y = 292;
    make_lab(L"分段", x, y, lw);
    hseg = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                         x + lw, y, 72, 24, hwnd, (HMENU)(INT_PTR)ID_SEG, NULL, NULL);
    SendMessageW(hseg, EM_SETLIMITTEXT, 4, 0);
    hunit = make_combo(ID_UNIT, x + lw + 80, y, 100);
    y = 332;
    make_lab(L"设备", x, y, lw);
    hprev = make_btn(L"←", ID_PREV, x + lw, y, 32, 24);
    hdev = CreateWindowW(L"STATIC", L"未指定", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                        x + lw + 40, y, cw - 80, 24, hwnd, (HMENU)(INT_PTR)ID_DEV, NULL, NULL);
    hnext = make_btn(L"→", ID_NEXT, x + w - 32, y, 32, 24);
    y = 376;
    make_lab(L"保存到", x, y, lw);
    hdir = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
                        x + lw, y, cw, 24, hwnd, (HMENU)(INT_PTR)ID_DIR, NULL, NULL);
    y = 412;
    hbrowse = make_btn(L"更改", ID_BROWSE, x + w - 168, y, 76, 28);
    hopen = make_btn(L"打开", ID_OPEN, x + w - 84, y, 76, 28);
    y = 460;
    hstart = make_btn(L"开始录制", ID_START, x, y, w, 40);
    y = 516;
    hpause = make_btn(L"暂停", ID_PAUSE, x, y, 210, 36);
    hstop = make_btn(L"停止", ID_STOP, x + 238, y, 210, 36);
    y = 568;
    hstatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, 170, hwnd, (HMENU)(INT_PTR)ID_STATUS, NULL, NULL);
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == CBN_SELCHANGE) on_sel(id);
        else if (id == ID_SEG && code == EN_KILLFOCUS) { read_seg(); cfg_save(); }
        else if (code == BN_CLICKED) {
            if (id == ID_PREV) cycle_dev(-1);
            else if (id == ID_NEXT) cycle_dev(1);
            else if (id == ID_BROWSE) browse_dir();
            else if (id == ID_OPEN) open_dir();
            else if (id == ID_START) request_start();
            else if (id == ID_PAUSE && sess) {
                if (phase == SY_PHASE_LIVE) sy_session_pause(sess);
                else if (phase == SY_PHASE_PAUSED) sy_session_resume(sess);
                poll_sess();
            } else if (id == ID_STOP) {
                if (counting) { counting = 0; set_u8(hstatus, "就绪"); }
                else if (sess) sy_session_stop(sess);
                poll_sess();
            }
        }
        sync_ui();
        return 0;
    }
    case WM_TIMER:
        if (counting && now_s() >= cd_until) {
            counting = 0;
            do_start();
        }
        poll_sess();
        sync_ui();
        if (quit_after && !sess && !counting) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        read_seg();
        cfg_save();
        if (counting) counting = 0;
        if (sess) {
            if (phase == SY_PHASE_LIVE || phase == SY_PHASE_PAUSED) sy_session_stop(sess);
            quit_after = 1;
            return 0;
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        if (sess) { sy_session_stop(sess); sy_session_close(sess); sess = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(w, msg, wp, lp);
    }
}

static int run_window(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    cfg_load();
    load_mons();
    load_engine();
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ShiyingMain";
    RegisterClassExW(&wc);
    RECT r = { 0, 0, 480, 760 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    hwnd = CreateWindowW(L"ShiyingMain", L"拾影录屏", style,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                        NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    create_controls();
    font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
    if (font) EnumChildWindows(hwnd, set_child_font, (LPARAM)font);
    fill_mons();
    fill_choices();
    if (!ff_path[0]) set_u8(hstatus, "找不到录制引擎（录制引擎/ffmpeg.exe），请保持程序目录完整");
    else set_u8(hstatus, "就绪");
    if (ff_path[0]) {
        SyStatus st;
        memset(&st, 0, sizeof st);
        if (sy_recover(cfg.outdir, ff_path, &sess, &st)) {
            phase = st.phase;
            paint_status(&st);
        }
    }
    sync_ui();
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, ID_TIMER, 50, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (font) DeleteObject(font);
    CoUninitialize();
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    set_dirs();
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int check = 0, help = 0, version = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--check")) check = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) help = 1;
        else if (!wcscmp(argv[i], L"--version")) version = 1;
    }
    if (argv) LocalFree(argv);
    if (version) { bind_console(); printf("%s\n", APP_VERSION); return 0; }
    if (help) { usage(); return 0; }
    if (check) return self_check();
    return run_window(inst);
}
