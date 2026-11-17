#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "cfg.h"
#include "path.h"
#include "toolbar.h"
#include "win_identity.h"

#define IDI_APP 1

enum {
    ID_MON = 101, ID_AUD, ID_FPS, ID_QUAL, ID_SCALE, ID_FMT, ID_CD, ID_CODEC, ID_CURS,
    ID_SEG, ID_UNIT, ID_DEV, ID_DIR, ID_BROWSE, ID_OPEN, ID_SETTINGS, ID_DEVICE,
    ID_START, ID_PAUSE, ID_STOP, ID_STATUS, ID_REMEMBER, ID_DESKTOP, ID_THUMBNAIL,
    ID_SHORTCUT, ID_RESULT, ID_CLICKS, ID_THEME, ID_MODE = 150, ID_TIMER = 1, ID_INTRO = 2
};
enum { TOOLBAR_W = 684, TOOLBAR_H = 224, SETTINGS_H = 594, PREVIEW_H = 342 };
enum { HOT_START = 1, HOT_PAUSE, HOT_SHOW };
enum { MSG_CLICK_RING = WM_APP + 14, CLICK_RING_SIZE = 66 };
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static HWND hwnd, hmon, haud, hfps, hqual, hscale, hfmt, hcd, hcodec, hcurs;
static HWND hseg, hunit, hdev, hdir, hbrowse, hopen, hstart, hpause, hstop, hstatus;
static HWND hmodes[SY_MODE_COUNT], hsettings, htheme, hremember, hdesktop, hthumbnail, hresult, hclicks;
static HWND settings_children[36];
static unsigned settings_count;
static HFONT font, font_title, font_emph;
static HBRUSH panel_brush, bg_brush;
static HICON app_icon;
static UINT ui_dpi = 96, raise_message;
static int settings_open, preview_w, preview_h, hot_start, modal_dialog;
static unsigned char *preview;
static RECT capture_rect;
static SyShot *shot;
static HWND selector_hwnd, click_window;
static HHOOK click_hook;
static int click_kind;
static double click_until;
static char result_path[1200];
static SySession *sess;
static SyCaps caps;
static SyDevice devs[32];
static SyChoice ch_codec[4], ch_cont[4], ch_curs[4], ch_aud[4];
static int n_codec, n_cont, n_curs, n_aud, n_dev, dev_i = -1, audio_before = SY_AUDIO_SYSTEM;
static char ff_path[1200];
static int counting, quit_after;
static double pending_at, intro_started;
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

static void set_text(HWND h, const wchar_t *text) {
    wchar_t current[1400] = {0};
    GetWindowTextW(h, current, ARRAYSIZE(current));
    if (wcscmp(current, text)) SetWindowTextW(h, text);
}

static void set_u8(HWND h, const char *s) {
    wchar_t w[1400];
    if (to_wide(s ? s : "", w, ARRAYSIZE(w)) == 0) set_text(h, w);
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
    printf(APP_NAME " %s 自检\n", APP_VERSION);
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
    printf(APP_NAME " %s\n用法：" APP_NAME ".exe [--check|--version|--help]\n"
           "  --check   检查录制引擎能否运行，以及是否列出屏幕采集。不会开始录屏。\n", APP_VERSION);
}

static int px(int v) { return MulDiv(v, (int)ui_dpi, 96); }

static HWND control(const wchar_t *cls, const wchar_t *title, DWORD style,
                    int id, int x, int y, int w, int h) {
    return CreateWindowW(cls, title, WS_CHILD | WS_VISIBLE | style,
                         px(x), px(y), px(w), px(h), hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
}

static HWND make_lab(const wchar_t *t, int x, int y, int w) {
    return control(L"STATIC", t, SS_CENTERIMAGE | SS_NOPREFIX, 0, x, y, w, 28);
}

static HWND make_combo(int id, int x, int y, int w) {
    return control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST |
                   CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                   id, x, y, w, 240);
}

static HWND make_btn(const wchar_t *t, int id, int x, int y, int w, int h) {
    return control(L"BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, id, x, y, w, h);
}

static HWND settings_child(HWND control_hwnd) {
    if (control_hwnd && settings_count < ARRAYSIZE(settings_children))
        settings_children[settings_count++] = control_hwnd;
    return control_hwnd;
}

static HWND settings_combo(const wchar_t *label, int id, int x, int y, int w) {
    settings_child(make_lab(label, x, y, 68));
    return settings_child(make_combo(id, x + 70, y, w - 70));
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
    if (cfg.audio == SY_AUDIO_DEVICE && dev_i >= 0) select_data(hdev, dev_i);
    else select_data(hdev, -1);
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
    select_data(hmon, cfg.monitor >= 0 && cfg.monitor < nmons ? cfg.monitor : -1);
}

static void fill_choices(void) {
    SendMessageW(haud, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n_aud; i++) add_u8(haud, ch_aud[i].label, ch_aud[i].id);
    if (n_dev > 0) add_item(haud, L"指定音频设备", SY_AUDIO_DEVICE);
    select_data(haud, cfg.audio);

    SendMessageW(hfps, CB_RESETCONTENT, 0, 0);
    add_item(hfps, L"10 FPS", 10);
    add_item(hfps, L"15 FPS", 15);
    add_item(hfps, L"24 FPS", 24);
    add_item(hfps, L"30 FPS", 30);
    add_item(hfps, L"60 FPS", 60);
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
    add_item(hdev, L"默认设备", -1);
    for (int i = 0; i < n_dev; i++) add_u8(hdev, devs[i].label, i);
    show_dev();
    show_dir();
    SendMessageW(hremember, BM_SETCHECK, cfg.remember_selection ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hdesktop, BM_SETCHECK, cfg.desktop_copy ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hthumbnail, BM_SETCHECK, cfg.show_thumbnail ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hclicks, BM_SETCHECK, cfg.click_highlight ? BST_CHECKED : BST_UNCHECKED, 0);
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

static RECT screen_rect(void) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return (RECT){ x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                  y + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

static RECT monitor_rect(void) {
    load_mons();
    if (cfg.monitor >= 0 && cfg.monitor < nmons) {
        Mon *m = &mons[cfg.monitor];
        return (RECT){ m->x, m->y, m->x + m->w, m->y + m->h };
    }
    return screen_rect();
}

typedef struct {
    RECT desktop, selected;
    POINT press;
    int window_mode, dragging, done, accepted;
} Selector;

typedef struct { POINT point; RECT rect; HWND overlay; } WindowTarget;

static BOOL CALLBACK find_target(HWND candidate, LPARAM param) {
    WindowTarget *target = (WindowTarget *)param;
    if (candidate == hwnd || candidate == target->overlay || !IsWindowVisible(candidate) ||
        IsIconic(candidate) || (GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TRANSPARENT) ||
        candidate == GetShellWindow() || candidate == GetDesktopWindow()) return TRUE;
    wchar_t class_name[64];
    GetClassNameW(candidate, class_name, ARRAYSIZE(class_name));
    if (!wcscmp(class_name, L"Progman") || !wcscmp(class_name, L"WorkerW") ||
        !wcscmp(class_name, L"Shell_TrayWnd") || !wcscmp(class_name, L"Shell_SecondaryTrayWnd"))
        return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(candidate, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    RECT rect;
    if (FAILED(DwmGetWindowAttribute(candidate, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof rect)) &&
        !GetWindowRect(candidate, &rect)) return TRUE;
    if (!PtInRect(&rect, target->point)) return TRUE;
    target->rect = rect;
    return FALSE;
}

static void select_at(HWND overlay, Selector *selection, POINT point) {
    if (selection->window_mode) {
        WindowTarget target = { point, { 0 }, overlay };
        EnumWindows(find_target, (LPARAM)&target);
        IntersectRect(&selection->selected, &target.rect, &selection->desktop);
    } else if (selection->dragging) {
        RECT bounds = {
            selection->press.x < point.x ? selection->press.x : point.x,
            selection->press.y < point.y ? selection->press.y : point.y,
            (selection->press.x > point.x ? selection->press.x : point.x) + 1,
            (selection->press.y > point.y ? selection->press.y : point.y) + 1
        };
        IntersectRect(&selection->selected, &bounds, &selection->desktop);
    }
    InvalidateRect(overlay, NULL, FALSE);
}

static int valid_rect(RECT rect) {
    return rect.right - rect.left >= 16 && rect.bottom - rect.top >= 16;
}

static void paint_selector(HWND overlay, const Selector *selection) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(overlay, &ps);
    RECT client;
    GetClientRect(overlay, &client);
    FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    RECT selected = selection->selected;
    if (valid_rect(selected)) {
        OffsetRect(&selected, -selection->desktop.left, -selection->desktop.top);
        HBRUSH fill = CreateSolidBrush(RGB(38, 50, 66));
        HBRUSH border = CreateSolidBrush(RGB(135, 198, 255));
        FillRect(dc, &selected, fill);
        FrameRect(dc, &selected, border);
        InflateRect(&selected, -1, -1);
        FrameRect(dc, &selected, border);
        DeleteObject(border);
        DeleteObject(fill);
    }
    MONITORINFO mon = { .cbSize = sizeof mon };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mon);
    RECT caption = mon.rcMonitor;
    OffsetRect(&caption, -selection->desktop.left, -selection->desktop.top);
    caption.top += px(36);
    caption.bottom = caption.top + px(40);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT previous = SelectObject(dc, font_emph);
    const wchar_t *tip = selection->window_mode
        ? L"单击选择窗口　·　Esc 取消"
        : L"拖动框选区域　·　Enter 确认　·　Esc 取消";
    DrawTextW(dc, tip, -1, &caption, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, previous);
    EndPaint(overlay, &ps);
}

static LRESULT CALLBACK selector_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    Selector *sel = (Selector *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return TRUE;
    }
    if (!sel) return DefWindowProcW(w, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: paint_selector(w, sel); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(w, &p);
        select_at(w, sel, p);
        return 0;
    }
    case WM_LBUTTONDOWN:
        GetCursorPos(&sel->press);
        if (!sel->window_mode) { sel->dragging = 1; SetCapture(w); }
        select_at(w, sel, sel->press);
        return 0;
    case WM_LBUTTONUP: {
        POINT point;
        GetCursorPos(&point);
        select_at(w, sel, point);
        sel->dragging = 0;
        if (GetCapture() == w) ReleaseCapture();
        if (valid_rect(sel->selected)) sel->accepted = sel->done = 1;
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN && valid_rect(sel->selected)) sel->accepted = sel->done = 1;
        else if (wp == VK_ESCAPE) sel->done = 1;
        return 0;
    case WM_RBUTTONDOWN: case WM_CLOSE: sel->done = 1; return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) sel->done = 1; return 0;
    default: return DefWindowProcW(w, msg, wp, lp);
    }
}

static int select_rectangle(int window_mode, RECT *out) {
    Selector selection = {0};
    selection.desktop = screen_rect();
    selection.window_mode = window_mode;
    if (!window_mode && cfg.remember_selection && cfg.area == AREA_REGION &&
        cfg.rw >= 16 && cfg.rh >= 16) {
        RECT previous = { cfg.rx, cfg.ry, cfg.rx + cfg.rw, cfg.ry + cfg.rh };
        IntersectRect(&selection.selected, &previous, &selection.desktop);
    }
    HWND overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                      L"ShiyingSelector", SY_WIN_TITLE, WS_POPUP,
                      selection.desktop.left, selection.desktop.top,
                      selection.desktop.right - selection.desktop.left,
                      selection.desktop.bottom - selection.desktop.top,
                      NULL, NULL, GetModuleHandleW(NULL), &selection);
    if (!overlay) return 0;
    SetLayeredWindowAttributes(overlay, 0, 162, LWA_ALPHA);
    selector_hwnd = overlay;
    ShowWindow(overlay, SW_SHOW);
    SetForegroundWindow(overlay);
    SetFocus(overlay);
    if (window_mode) {
        POINT p;
        GetCursorPos(&p);
        select_at(overlay, &selection, p);
    }
    while (!selection.done) {
        MSG message;
        int next = GetMessageW(&message, NULL, 0, 0);
        if (next <= 0) {
            if (next == 0) PostQuitMessage((int)message.wParam);
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(overlay);
    selector_hwnd = NULL;
    if (selection.accepted) *out = selection.selected;
    return selection.accepted;
}

static LRESULT CALLBACK click_ring_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg != WM_PAINT) return DefWindowProcW(w, msg, wp, lp);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN edge = CreatePen(PS_SOLID, px(6), RGB(29, 33, 41));
    HPEN ring = CreatePen(PS_SOLID, px(3), click_kind == WM_RBUTTONDOWN ? RGB(91, 181, 255) : RGB(255, 204, 67));
    HGDIOBJ previous_pen = SelectObject(dc, edge);
    InflateRect(&r, -px(8), -px(8));
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ring);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
    DeleteObject(edge);
    DeleteObject(ring);
    EndPaint(w, &ps);
    return 0;
}

static LRESULT CALLBACK detect_click(int code, WPARAM kind, LPARAM data) {
    if (code == HC_ACTION && phase == SY_PHASE_LIVE &&
        (kind == WM_LBUTTONDOWN || kind == WM_RBUTTONDOWN || kind == WM_MBUTTONDOWN)) {
        const MSLLHOOKSTRUCT *event = (const MSLLHOOKSTRUCT *)data;
        if (PtInRect(&capture_rect, event->pt))
            PostMessageW(hwnd, MSG_CLICK_RING, kind, MAKELPARAM(event->pt.x, event->pt.y));
    }
    return CallNextHookEx(NULL, code, kind, data);
}

static void stop_click_rings(void) {
    if (click_hook) UnhookWindowsHookEx(click_hook);
    click_hook = NULL;
    click_until = 0;
    if (click_window) DestroyWindow(click_window);
    click_window = NULL;
}

static int start_click_rings(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    click_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                      WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                  L"ShiyingClickRing", L"", WS_POPUP, 0, 0,
                                  px(CLICK_RING_SIZE), px(CLICK_RING_SIZE), NULL, NULL, inst, NULL);
    if (click_window) {
        SetLayeredWindowAttributes(click_window, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
        click_hook = SetWindowsHookExW(WH_MOUSE_LL, detect_click, inst, 0);
    }
    if (click_hook) return 1;
    stop_click_rings();
    return 0;
}

static void show_click_ring(int button, POINT point) {
    if (!click_window || !sess || phase != SY_PHASE_LIVE) return;
    HWND target = WindowFromPoint(point);
    if (target == hwnd || GetAncestor(target, GA_ROOT) == hwnd) return;
    click_kind = button;
    click_until = now_s() + 0.30;
    int size = px(CLICK_RING_SIZE);
    SetWindowPos(click_window, HWND_TOPMOST, point.x - size / 2, point.y - size / 2, size, size,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(click_window, NULL, FALSE);
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

static int locked(void) { return counting || pending_at > 0 || sess || shot || selector_hwnd || modal_dialog; }
static int record_mode(void) { return cfg.mode >= SY_RECORD_SCREEN; }
static void layout_window(void);
static void refresh_tally(void);

static void sync_ui(void) {
    static BOOL capture_timer;
    BOOL busy = counting || pending_at > 0 || sess || shot || quit_after;
    if (busy && !capture_timer) capture_timer = SetTimer(hwnd, ID_TIMER, 50, NULL) != 0;
    else if (!busy && capture_timer) {
        KillTimer(hwnd, ID_TIMER);
        capture_timer = FALSE;
    }
    refresh_tally();
    BOOL ready = !locked();
    for (unsigned i = 0; i < settings_count; i++) EnableWindow(settings_children[i], ready);
    for (int i = 0; i < SY_MODE_COUNT; i++) EnableWindow(hmodes[i], ready);
    EnableWindow(hsettings, ready);
    EnableWindow(hstart, ready);
    BOOL can_pause = sess && (phase == SY_PHASE_LIVE || phase == SY_PHASE_PAUSED);
    BOOL can_stop = counting || pending_at > 0 || can_pause;
    ShowWindow(hpause, can_pause ? SW_SHOWNA : SW_HIDE);
    ShowWindow(hstop, can_stop ? SW_SHOWNA : SW_HIDE);
    ShowWindow(hstart, can_stop ? SW_HIDE : SW_SHOWNA);
    set_text(hpause, phase == SY_PHASE_PAUSED ? L"继续" : L"暂停");
    set_text(hstop, sess ? L"停止" : L"取消");
    set_text(hstart, record_mode() ? L"录制" : L"截取");
    if (counting) {
        int left = (int)(cd_until - now_s() + 0.999);
        if (left < 1) left = 1;
        char b[64];
        snprintf(b, sizeof b, "倒计时 %d 秒", left);
        set_u8(hstatus, b);
    }
}

static void restore_capture_window(void) {
    SetWindowDisplayAffinity(hwnd, WDA_NONE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

static void erase_preview(void) {
    if (!preview) return;
    free(preview);
    preview = NULL;
    layout_window();
}

static void do_start(void) {
    SyPlan plan = {0};
    plan.x = capture_rect.left;
    plan.y = capture_rect.top;
    plan.w = capture_rect.right - capture_rect.left;
    plan.h = capture_rect.bottom - capture_rect.top;
    plan.fps = cfg.fps;
    plan.quality = cfg.quality;
    plan.scale = cfg.scale;
    plan.codec = (SyCodec)cfg.codec;
    plan.container = (SyContainer)cfg.fmt;
    plan.cursor = (SyCursor)cfg.cursor;
    plan.audio = (SyAudio)cfg.audio;
    plan.desktop_copy = cfg.desktop_copy;
    plan.thumbnail = cfg.show_thumbnail;
    plan.limit_bytes = sy_limit_bytes(cfg.segment_n, cfg.segment_unit);
    if (cfg.audio == SY_AUDIO_DEVICE) snprintf(plan.device_id, sizeof plan.device_id, "%s", cfg.audio_dev);
    snprintf(plan.outdir, sizeof plan.outdir, "%s", cfg.outdir);
    SyStatus st = {0};
    if (!record_mode()) {
        if (sy_shot_begin(&shot, &plan, ff_path, &caps, &st) != 0) {
            restore_capture_window();
            set_u8(hstatus, st.err[0] ? st.err : "无法截图");
        }
        return;
    }
    sy_clamp_recorded(&plan.codec, &plan.container, &plan.cursor, &caps);
    if (sy_session_open(&sess, &plan, ff_path, &caps, &st) != 0) {
        restore_capture_window();
        set_u8(hstatus, st.err[0] ? st.err : "无法开始录制");
        return;
    }
    phase = st.phase;
    paint_status(&st);
    if (cfg.click_highlight && !start_click_rings()) set_u8(hstatus, "无法启用鼠标点击高亮");
    DWORD affinity = 0;
    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) &&
        GetWindowDisplayAffinity(hwnd, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    } else if (!hot_start) {
        restore_capture_window();
    } else {
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
    }
}

static void hide_before_capture(void) {
    ShowWindow(hwnd, SW_HIDE);
    DwmFlush();
    pending_at = now_s() + 0.18;
}

static void request_start(void) {
    if (locked()) return;
    if (!ff_path[0]) {
        set_u8(hstatus, "找不到录制引擎（录制引擎/ffmpeg.exe），请保持程序目录完整");
        return;
    }
    read_seg();
    int area = cfg.mode % 3;
    if (area == 0) {
        capture_rect = monitor_rect();
    } else {
        ShowWindow(hwnd, SW_HIDE);
        DwmFlush();
        int accepted = select_rectangle(area == 1, &capture_rect);
        ShowWindow(hwnd, SW_SHOW);
        if (!accepted || quit_after) return;
    }
    if (!valid_rect(capture_rect)) { set_u8(hstatus, "捕捉区域无效"); return; }
    if (area == 2) {
        cfg.area = AREA_REGION;
        cfg.rx = capture_rect.left;
        cfg.ry = capture_rect.top;
        cfg.rw = capture_rect.right - capture_rect.left;
        cfg.rh = capture_rect.bottom - capture_rect.top;
    }
    cfg_save();
    erase_preview();
    settings_open = 0;
    layout_window();
    if (cfg.countdown > 0) {
        counting = 1;
        cd_until = now_s() + cfg.countdown;
    } else {
        hide_before_capture();
    }
    sync_ui();
}

static void cancel_or_stop(void) {
    if (counting || pending_at > 0) {
        counting = 0;
        pending_at = 0;
        restore_capture_window();
        set_u8(hstatus, "就绪");
    } else if (sess) {
        sy_session_stop(sess);
    }
}

static void on_sel(int id) {
    if (locked()) return;
    switch (id) {
    case ID_MON: cfg.monitor = item_data(hmon); break;
    case ID_AUD: {
        int d = item_data(haud);
        if (d < 0) break;
        if (d == SY_AUDIO_DEVICE) {
            if (n_dev <= 0) break;
            if (cfg.audio != SY_AUDIO_DEVICE) audio_before = cfg.audio;
            dev_i = dev_i < 0 ? 0 : dev_i;
            snprintf(cfg.audio_dev, sizeof cfg.audio_dev, "%s", devs[dev_i].id);
        } else {
            cfg.audio_dev[0] = 0;
            dev_i = -1;
            audio_before = d;
        }
        cfg.audio = d;
        show_dev();
        break;
    }
    case ID_DEVICE: {
        int d = item_data(hdev);
        if (d >= 0 && d < n_dev) {
            if (cfg.audio != SY_AUDIO_DEVICE) audio_before = cfg.audio;
            dev_i = d;
            cfg.audio = SY_AUDIO_DEVICE;
            snprintf(cfg.audio_dev, sizeof cfg.audio_dev, "%s", devs[d].id);
        } else {
            dev_i = -1;
            cfg.audio_dev[0] = 0;
            if (cfg.audio == SY_AUDIO_DEVICE) cfg.audio = audio_before;
        }
        select_data(haud, cfg.audio);
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

static void browse_dir(void) {
    if (locked()) return;
    IFileOpenDialog *dialog = NULL;
    IShellItem *folder = NULL;
    HRESULT result = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_IFileOpenDialog, (void **)&dialog);
    if (FAILED(result)) {
        set_u8(hstatus, "无法打开目录选择器");
        return;
    }
    FILEOPENDIALOGOPTIONS options;
    if (SUCCEEDED(dialog->lpVtbl->GetOptions(dialog, &options)))
        dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->lpVtbl->SetTitle(dialog, L"选择保存目录");
    wchar_t current[1200];
    if (to_wide(cfg.outdir, current, ARRAYSIZE(current)) == 0 &&
        SUCCEEDED(SHCreateItemFromParsingName(current, NULL, &IID_IShellItem, (void **)&folder))) {
        dialog->lpVtbl->SetFolder(dialog, folder);
        folder->lpVtbl->Release(folder);
        folder = NULL;
    }
    modal_dialog = 1;
    result = dialog->lpVtbl->Show(dialog, hwnd);
    modal_dialog = 0;
    if (SUCCEEDED(result) && SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &folder))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(folder->lpVtbl->GetDisplayName(folder, SIGDN_FILESYSPATH, &path))) {
            int needed = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
            if (needed > 0 && needed <= (int)sizeof cfg.outdir) {
                to_u8(path, cfg.outdir, sizeof cfg.outdir);
                slash(cfg.outdir);
                show_dir();
                cfg_save();
            } else set_u8(hstatus, "保存目录路径过长");
            CoTaskMemFree(path);
        }
        folder->lpVtbl->Release(folder);
    }
    dialog->lpVtbl->Release(dialog);
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
        stop_click_rings();
        sy_session_close(sess);
        sess = NULL;
        restore_capture_window();
    }
}

static void poll_shot(void) {
    if (!shot) return;
    SyStatus st = {0};
    if (!sy_shot_poll(shot, &st)) return;
    restore_capture_window();
    if (st.last_file[0]) {
        char name[400], status[440];
        snprintf(result_path, sizeof result_path, "%s", st.last_file);
        base_name(result_path, name, sizeof name);
        snprintf(status, sizeof status, "已保存 %s", name);
        set_u8(hstatus, status);
        const unsigned char *pixels = sy_shot_preview(shot, &preview_w, &preview_h);
        if (pixels && preview_w > 0 && preview_h > 0) {
            size_t size = (size_t)preview_w * preview_h * 4;
            preview = malloc(size);
            if (preview) memcpy(preview, pixels, size);
        }
        layout_window();
    } else {
        set_u8(hstatus, st.err[0] ? st.err : "无法截图");
    }
    sy_shot_close(shot);
    shot = NULL;
}

static BOOL CALLBACK set_child_font(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

static void create_controls(void) {
    static const wchar_t *const mode_text[] = {
        L"截取全屏", L"截取窗口", L"截取区域", L"录制全屏", L"录制窗口", L"录制区域"
    };
    htheme = make_btn(cfg.theme ? L"浅色模式" : L"深色模式", ID_THEME, 472, 14, 96, 30);
    hsettings = make_btn(L"设置", ID_SETTINGS, 582, 14, 84, 30);
    for (int i = 0; i < SY_MODE_COUNT; i++) {
        int x = (i < SY_RECORD_SCREEN ? 18 : 348) + 12 + (i % 3) * 100;
        hmodes[i] = make_btn(mode_text[i], ID_MODE + i, x, 88, 94, 66);
    }
    hstatus = control(L"STATIC", L"", SS_NOPREFIX | SS_WORDELLIPSIS,
                      ID_STATUS, 20, 175, 446, 43);
    hstart = make_btn(L"录制", ID_START, 574, 174, 92, 30);
    hpause = make_btn(L"暂停", ID_PAUSE, 475, 174, 88, 30);
    hstop = make_btn(L"停止", ID_STOP, 574, 174, 92, 30);
    hresult = make_btn(L"打开截图", ID_RESULT, 166, 242, 110, 30);

    hmon = settings_combo(L"显示器", ID_MON, 36, 268, 300);
    hcd = settings_combo(L"倒计时", ID_CD, 354, 268, 294);
    haud = settings_combo(L"声音", ID_AUD, 36, 306, 300);
    hdev = settings_combo(L"音频设备", ID_DEVICE, 354, 306, 294);
    hscale = settings_combo(L"分辨率", ID_SCALE, 36, 344, 300);
    hfps = settings_combo(L"帧率", ID_FPS, 354, 344, 294);
    hqual = settings_combo(L"画质", ID_QUAL, 36, 382, 300);
    hcurs = settings_combo(L"指针", ID_CURS, 354, 382, 294);
    hfmt = settings_combo(L"容器", ID_FMT, 36, 420, 155);
    hcodec = settings_combo(L"编码", ID_CODEC, 208, 420, 128);
    settings_child(make_lab(L"分段", 354, 420, 68));
    hseg = settings_child(control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                                  ID_SEG, 424, 420, 78, 25));
    SendMessageW(hseg, EM_SETLIMITTEXT, 4, 0);
    hunit = settings_child(make_combo(ID_UNIT, 512, 420, 136));

    settings_child(make_lab(L"保存到", 36, 460, 68));
    hdir = settings_child(control(L"STATIC", L"", SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
                                  ID_DIR, 106, 460, 356, 27));
    hbrowse = settings_child(make_btn(L"更改", ID_BROWSE, 474, 460, 78, 28));
    hopen = settings_child(make_btn(L"打开", ID_OPEN, 562, 460, 86, 28));
    hremember = settings_child(control(L"BUTTON", L"记住上次选择", WS_TABSTOP | BS_AUTOCHECKBOX,
                                       ID_REMEMBER, 36, 504, 140, 24));
    hthumbnail = settings_child(control(L"BUTTON", L"显示截图缩略图", WS_TABSTOP | BS_AUTOCHECKBOX,
                                        ID_THUMBNAIL, 186, 504, 150, 24));
    hdesktop = settings_child(control(L"BUTTON", L"录像另存桌面副本", WS_TABSTOP | BS_AUTOCHECKBOX,
                                      ID_DESKTOP, 354, 504, 166, 24));
    hclicks = settings_child(control(L"BUTTON", L"显示鼠标点击", WS_TABSTOP | BS_AUTOCHECKBOX,
                                     ID_CLICKS, 536, 504, 112, 24));
    settings_child(make_btn(L"创建快捷方式", ID_SHORTCUT, 536, 536, 112, 30));
    settings_child(make_lab(L"色彩：SDR  ·  当前采集引擎暂不支持 Rec.2100 HLG", 36, 537, 479));
}

static COLORREF color(unsigned rgb) {
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static RECT box(int x, int y, int w, int h) {
    return (RECT){ px(x), px(y), px(x + w), px(y + h) };
}

static void round_box(HDC dc, const RECT *r, COLORREF fill, COLORREF stroke, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, px(radius * 2), px(radius * 2));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void draw_label(HDC dc, RECT r, const wchar_t *text, HFONT f, COLORREF ink, UINT align) {
    HGDIOBJ old_font = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | align);
    SelectObject(dc, old_font);
}

static void apply_theme(void) {
    HBRUSH panel = CreateSolidBrush(color(SY_UI_PANEL));
    HBRUSH background = CreateSolidBrush(color(SY_UI_BG));
    if (panel && background) {
        if (panel_brush) DeleteObject(panel_brush);
        if (bg_brush) DeleteObject(bg_brush);
        panel_brush = panel;
        bg_brush = background;
    } else {
        if (panel) DeleteObject(panel);
        if (background) DeleteObject(background);
    }
    if (!hwnd) return;
    BOOL dark = cfg.theme != 0;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    COLORREF title_bg = dark ? color(SY_UI_PANEL) : DWMWA_COLOR_DEFAULT;
    COLORREF title_fg = dark ? color(SY_UI_TEXT) : DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &title_bg, sizeof title_bg);
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &title_fg, sizeof title_fg);
    if (htheme) set_text(htheme, dark ? L"浅色模式" : L"深色模式");
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

static COLORREF mix_color(COLORREF from, COLORREF to, int amount) {
    int rest = 255 - amount;
    return RGB((GetRValue(from) * rest + GetRValue(to) * amount) / 255,
               (GetGValue(from) * rest + GetGValue(to) * amount) / 255,
               (GetBValue(from) * rest + GetBValue(to) * amount) / 255);
}

static void stop_intro(void) {
    if (!intro_started) return;
    intro_started = 0;
    KillTimer(hwnd, ID_INTRO);
    RECT r = box(0, 0, SY_INTRO_WIDTH, SY_INTRO_HEIGHT);
    InvalidateRect(hwnd, &r, FALSE);
}

static void start_intro(void) {
    BOOL enabled = TRUE;
    HIGHCONTRASTW contrast = { .cbSize = sizeof contrast };
    if ((SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled) ||
        (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON))) return;
    intro_started = now_s();
    if (!SetTimer(hwnd, ID_INTRO, SY_INTRO_FRAME_MS, NULL)) intro_started = 0;
}

static void draw_brand_icon(HDC dc) {
    if (!intro_started) {
        DrawIconEx(dc, px(18), px(14), app_icon, px(32), px(32), 0, NULL, DI_NORMAL);
        return;
    }
    SyIntroFrame frame = sy_intro_frame(now_s() - intro_started);
    int size = (int)(frame.icon_size + 0.5);
    DrawIconEx(dc, px(34) - px(size) / 2, px(30) - px(size) / 2, app_icon, px(size), px(size), 0, NULL, DI_NORMAL);
    if (frame.focus_alpha > 0) {
        int focus = (int)(frame.focus + 0.5), arm = (int)(frame.arm + 0.5);
        HPEN pen = CreatePen(PS_SOLID, px(2),
            mix_color(color(SY_UI_BG), color(SY_UI_ACCENT), (int)(255 * frame.focus_alpha)));
        if (pen) {
            HGDIOBJ old_pen = SelectObject(dc, pen);
            for (int y = -1; y <= 1; y += 2) {
                for (int x = -1; x <= 1; x += 2) {
                    int corner_x = 34 + x * focus, corner_y = 30 + y * focus;
                    MoveToEx(dc, px(corner_x - x * arm), px(corner_y), NULL);
                    LineTo(dc, px(corner_x), px(corner_y));
                    LineTo(dc, px(corner_x), px(corner_y - y * arm));
                }
            }
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
    }
    if (frame.sweep > 0 && frame.sweep_alpha > 0) {
        RECT line = box(60, 44, (int)frame.sweep, 2);
        COLORREF tone = mix_color(color(SY_UI_BG), color(SY_UI_ACCENT), (int)(255 * frame.sweep_alpha));
        round_box(dc, &line, tone, tone, 1);
    }
    if (frame.tally_alpha > 0) {
        RECT lamp = box(60 + (int)frame.sweep - 3, 42, 6, 6);
        COLORREF tone = mix_color(color(SY_UI_BG), color(SY_UI_RECORD), (int)(255 * frame.tally_alpha));
        round_box(dc, &lamp, tone, tone, 3);
    }
}

static void paint_combo(const DRAWITEMSTRUCT *item) {
    HDC dc = item->hDC;
    RECT r = item->rcItem;
    BOOL open_item = !(item->itemState & ODS_COMBOBOXEDIT);
    BOOL selected = open_item && (item->itemState & ODS_SELECTED);
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    HBRUSH brush = CreateSolidBrush(color(selected ? SY_UI_TINT : SY_UI_PANEL));
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    if (item->itemID == (UINT)-1) return;
    LRESULT chars = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, (WPARAM)item->itemID, 0);
    wchar_t value[260];
    if (chars < 0 || (size_t)chars >= ARRAYSIZE(value) ||
        SendMessageW(item->hwndItem, CB_GETLBTEXT, (WPARAM)item->itemID, (LPARAM)value) == CB_ERR) return;
    r.left += px(5);
    r.right -= px(4);
    draw_label(dc, r, value, font, color(disabled ? SY_UI_SUB : selected ? SY_UI_ACCENT : SY_UI_TEXT),
               DT_LEFT | DT_END_ELLIPSIS);
    if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT))
        DrawFocusRect(dc, &item->rcItem);
}

static void draw_mode_icon(HDC dc, int cx, int cy, int mode, COLORREF ink, COLORREF fill) {
    int x = cx - px(11), y = cy - px(8);
    HPEN pen = CreatePen(PS_SOLID, px(2), ink);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (mode % 3) {
    case 0:
        RoundRect(dc, x, y, x + px(22), y + px(15), px(4), px(4));
        MoveToEx(dc, cx, y + px(15), NULL); LineTo(dc, cx, y + px(19));
        MoveToEx(dc, cx - px(6), y + px(19), NULL); LineTo(dc, cx + px(7), y + px(19));
        break;
    case 1:
        MoveToEx(dc, x + px(5), y + px(3), NULL); LineTo(dc, x + px(5), y - px(1));
        LineTo(dc, x + px(23), y - px(1)); LineTo(dc, x + px(23), y + px(12));
        RoundRect(dc, x, y + px(4), x + px(19), y + px(18), px(4), px(4));
        MoveToEx(dc, x, y + px(8), NULL); LineTo(dc, x + px(19), y + px(8));
        break;
    case 2:
        for (int i = 0; i <= 1; i++) {
            int side = x + px(i * 22);
            MoveToEx(dc, side, y + px(6), NULL); LineTo(dc, side, y); LineTo(dc, side + px(i ? -7 : 7), y);
            MoveToEx(dc, side, y + px(11), NULL); LineTo(dc, side, y + px(17));
            LineTo(dc, side + px(i ? -7 : 7), y + px(17));
        }
        break;
    }
    if (mode >= SY_RECORD_SCREEN) {
        HBRUSH dot = CreateSolidBrush(ink);
        HPEN outline = CreatePen(PS_SOLID, px(2), fill);
        SelectObject(dc, dot);
        SelectObject(dc, outline);
        Ellipse(dc, x + px(16), y + px(11), x + px(26), y + px(21));
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        DeleteObject(outline);
        DeleteObject(dot);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void paint_button(const DRAWITEMSTRUCT *item) {
    int id = (int)item->CtlID;
    HDC dc = item->hDC;
    RECT r = item->rcItem;
    COLORREF bg = color(SY_UI_BG);
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL is_mode = id >= ID_MODE && id < ID_MODE + SY_MODE_COUNT;
    COLORREF disk = color(SY_UI_PANEL), border = color(SY_UI_BORDER);
    COLORREF ink = disabled ? color(SY_UI_SUB) : color(SY_UI_TEXT);
    wchar_t caption[96];
    GetWindowTextW(item->hwndItem, caption, ARRAYSIZE(caption));
    if (is_mode) {
        int mode = id - ID_MODE;
        BOOL selected = cfg.mode == mode;
        bg = color(SY_UI_PANEL);
        if (selected || pressed) {
            unsigned tone = mode >= SY_RECORD_SCREEN ? SY_UI_RECORD : SY_UI_ACCENT;
            disk = color(mode >= SY_RECORD_SCREEN ? SY_UI_RECORD_TINT : SY_UI_TINT);
            ink = border = color(tone);
        } else border = disk;
        FillRect(dc, &r, panel_brush);
        round_box(dc, &r, disk, border, 10);
        draw_mode_icon(dc, (r.left + r.right) / 2, px(23), mode, ink, disk);
        static const wchar_t *const labels[] = { L"全屏", L"窗口", L"区域" };
        RECT label = { r.left, px(36), r.right, r.bottom - px(5) };
        draw_label(dc, label, labels[mode % 3], selected ? font_emph : font, ink, DT_CENTER);
    } else {
        BOOL primary = id == ID_START || id == ID_STOP;
        if (primary && !disabled) {
            disk = border = color((id == ID_STOP || record_mode()) ? SY_UI_RECORD : SY_UI_ACCENT);
            ink = color(SY_UI_ON_ACCENT);
        } else if (pressed || (id == ID_SETTINGS && settings_open)) {
            disk = color(SY_UI_TINT);
            border = ink = color(SY_UI_ACCENT);
        }
        if (id == ID_BROWSE || id == ID_OPEN || id == ID_SHORTCUT || id == ID_RESULT)
            bg = color(SY_UI_PANEL);
        HBRUSH bg_brush = CreateSolidBrush(bg);
        FillRect(dc, &r, bg_brush);
        DeleteObject(bg_brush);
        round_box(dc, &r, disk, border, 9);
        draw_label(dc, r, caption, primary ? font_emph : font, ink, DT_CENTER);
    }
    if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&r, -px(4), -px(4));
        DrawFocusRect(dc, &r);
    }
}

static void layout_window(void) {
    BOOL show_preview = preview && !settings_open;
    for (unsigned i = 0; i < settings_count; i++)
        ShowWindow(settings_children[i], settings_open ? SW_SHOWNA : SW_HIDE);
    ShowWindow(hresult, show_preview ? SW_SHOWNA : SW_HIDE);
    set_text(hsettings, settings_open ? L"收起设置" : L"设置");
    RECT frame = box(0, 0, TOOLBAR_W, settings_open ? SETTINGS_H : show_preview ? PREVIEW_H : TOOLBAR_H);
    AdjustWindowRect(&frame, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
    RECT current;
    GetWindowRect(hwnd, &current);
    int x = current.left, y = current.top, w = frame.right - frame.left, h = frame.bottom - frame.top;
    MONITORINFO monitor = { .cbSize = sizeof monitor };
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x + w > monitor.rcWork.right) x = monitor.rcWork.right - w;
        if (y + h > monitor.rcWork.bottom) y = monitor.rcWork.bottom - h;
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
    }
    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void refresh_tally(void) {
    static BOOL shown;
    BOOL live = phase == SY_PHASE_LIVE;
    if (shown == live) return;
    shown = live;
    RECT tally = box(398, 67, 20, 16);
    InvalidateRect(hwnd, &tally, FALSE);
}

static void paint_toolbar(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    FillRect(dc, &r, bg_brush);
    draw_brand_icon(dc);
    draw_label(dc, box(60, 12, 260, 36), SY_WIN_TITLE, font_title, color(SY_UI_TEXT), DT_LEFT);
    if (ps.rcPaint.bottom <= px(SY_INTRO_HEIGHT) && ps.rcPaint.right <= px(SY_INTRO_WIDTH)) {
        EndPaint(w, &ps);
        return;
    }
    for (int group = 0; group < 2; group++) {
        int x = 18 + group * 330;
        RECT card = box(x, 58, 318, 110);
        round_box(dc, &card, color(SY_UI_PANEL), color(SY_UI_BORDER), 13);
        draw_label(dc, box(x + 13, 62, 140, 26), group ? L"录屏" : L"截图",
                   font_emph, color(group ? SY_UI_RECORD : SY_UI_SUB), DT_LEFT);
        if (group && phase == SY_PHASE_LIVE) {
            RECT glow = box(400, 68, 14, 14);
            RECT lamp = box(403, 71, 8, 8);
            round_box(dc, &glow, color(SY_UI_RECORD_TINT), color(SY_UI_RECORD_TINT), 7);
            round_box(dc, &lamp, color(SY_UI_RECORD), color(SY_UI_RECORD), 4);
        }
    }
    if (settings_open) {
        RECT settings = box(18, 224, 648, 351);
        round_box(dc, &settings, color(SY_UI_PANEL), color(SY_UI_BORDER), 13);
        draw_label(dc, box(36, 231, 160, 28), L"采集设置", font_emph, color(SY_UI_TEXT), DT_LEFT);
    } else if (preview) {
        RECT card = box(18, 224, 648, 98);
        round_box(dc, &card, color(SY_UI_PANEL), color(SY_UI_BORDER), 13);
        int w = 112, h = MulDiv(preview_h, w, preview_w);
        if (h > 72) { h = 72; w = MulDiv(preview_w, h, preview_h); }
        BITMAPINFO image = {0};
        image.bmiHeader.biSize = sizeof image.bmiHeader;
        image.bmiHeader.biWidth = preview_w;
        image.bmiHeader.biHeight = -preview_h;
        image.bmiHeader.biPlanes = 1;
        image.bmiHeader.biBitCount = 32;
        image.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, px(36 + (112 - w) / 2), px(237 + (72 - h) / 2), px(w), px(h),
                      0, 0, preview_w, preview_h, preview, &image, DIB_RGB_COLORS, SRCCOPY);
        draw_label(dc, box(166, 278, 463, 25), L"截图已保存到所选目录", font, color(SY_UI_SUB), DT_LEFT);
    }
    EndPaint(w, &ps);
}

static void choose_mode(int mode) {
    if (locked()) return;
    int previous = cfg.mode;
    cfg.mode = mode;
    cfg_save();
    InvalidateRect(hmodes[previous], NULL, FALSE);
    InvalidateRect(hmodes[cfg.mode], NULL, FALSE);
    InvalidateRect(hstart, NULL, FALSE);
    set_u8(hstatus, sy_mode_labels[cfg.mode]);
}

static void pause_or_resume(void) {
    if (!sess) return;
    if (phase == SY_PHASE_LIVE) sy_session_pause(sess);
    else if (phase == SY_PHASE_PAUSED) sy_session_resume(sess);
}

static void toggle_capture_window(void) {
    if (selector_hwnd || modal_dialog || shot || pending_at > 0) return;
    if (sess && hot_start && IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

static void open_result(void) {
    wchar_t path[1200];
    if (to_wide(result_path, path, ARRAYSIZE(path)) == 0)
        ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void on_action(int id) {
    if (id >= ID_MODE && id < ID_MODE + SY_MODE_COUNT) {
        choose_mode(id - ID_MODE);
        return;
    }
    switch (id) {
    case ID_BROWSE: browse_dir(); break;
    case ID_OPEN: open_dir(); break;
    case ID_START: request_start(); break;
    case ID_PAUSE: pause_or_resume(); break;
    case ID_STOP: cancel_or_stop(); break;
    case ID_RESULT: open_result(); break;
    case ID_THEME:
        cfg.theme = !cfg.theme;
        apply_theme();
        cfg_save();
        break;
    case ID_SETTINGS:
        if (!locked()) {
            settings_open = !settings_open;
            layout_window();
        }
        break;
    case ID_REMEMBER:
        cfg.remember_selection = SendMessageW(hremember, BM_GETCHECK, 0, 0) == BST_CHECKED;
        cfg_save();
        break;
    case ID_THUMBNAIL:
        cfg.show_thumbnail = SendMessageW(hthumbnail, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!cfg.show_thumbnail) erase_preview();
        cfg_save();
        break;
    case ID_DESKTOP:
        cfg.desktop_copy = SendMessageW(hdesktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
        cfg_save();
        break;
    case ID_CLICKS:
        cfg.click_highlight = SendMessageW(hclicks, BM_GETCHECK, 0, 0) == BST_CHECKED;
        cfg_save();
        break;
    case ID_SHORTCUT: {
        if (locked()) break;
        modal_dialog = 1;
        int result = sy_win_shortcuts(hwnd);
        modal_dialog = 0;
        set_u8(hstatus, result == 0 ? "已更新桌面和开始菜单快捷方式" : "无法创建快捷方式");
        break;
    }
    }
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (raise_message && msg == raise_message) {
        if (!selector_hwnd && !modal_dialog && !shot && pending_at <= 0) {
            ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(w);
        }
        return 0;
    }
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == BN_CLICKED) stop_intro();
        if (code == CBN_SELCHANGE) on_sel(id);
        else if (id == ID_SEG && code == EN_KILLFOCUS) { read_seg(); cfg_save(); }
        else if (code == BN_CLICKED) on_action(id);
        sync_ui();
        return 0;
    }
    case MSG_CLICK_RING: {
        POINT point = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        show_click_ring((int)wp, point);
        return 0;
    }
    case WM_TIMER:
        if (wp == ID_INTRO) {
            if (now_s() - intro_started >= SY_INTRO_SECONDS) stop_intro();
            else {
                RECT r = box(0, 0, SY_INTRO_WIDTH, SY_INTRO_HEIGHT);
                InvalidateRect(w, &r, FALSE);
            }
            return 0;
        }
        if (click_until > 0 && (phase != SY_PHASE_LIVE || now_s() >= click_until)) {
            click_until = 0;
            ShowWindow(click_window, SW_HIDE);
        }
        if (counting && now_s() >= cd_until) {
            counting = 0;
            hide_before_capture();
        }
        if (pending_at > 0 && now_s() >= pending_at) {
            pending_at = 0;
            do_start();
        }
        poll_sess();
        poll_shot();
        sync_ui();
        if (quit_after && !locked()) DestroyWindow(w);
        return 0;
    case WM_HOTKEY:
        stop_intro();
        if (wp == HOT_START && !selector_hwnd && !modal_dialog) {
            if (counting || sess || pending_at > 0) cancel_or_stop();
            else if (!shot) {
                if (!record_mode()) choose_mode(SY_RECORD_SCREEN + cfg.mode);
                request_start();
            }
        } else if (wp == HOT_PAUSE) pause_or_resume();
        else if (wp == HOT_SHOW) toggle_capture_window();
        sync_ui();
        return 0;
    case WM_DISPLAYCHANGE:
        load_mons();
        if (!locked()) fill_mons();
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lp;
        if (item->CtlType == ODT_COMBOBOX) {
            item->itemHeight = px(24);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lp;
        if (item->CtlType == ODT_BUTTON) {
            paint_button(item);
            return TRUE;
        }
        if (item->CtlType == ODT_COMBOBOX) {
            paint_combo(item);
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        stop_intro();
        break;
    case WM_PAINT: paint_toolbar(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, color(SY_UI_SUB));
        if ((HWND)lp == hstatus) {
            SetBkColor((HDC)wp, color(SY_UI_BG));
            return (INT_PTR)bg_brush;
        }
        SetBkColor((HDC)wp, color(SY_UI_PANEL));
        return (INT_PTR)panel_brush;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, color(SY_UI_TEXT));
        SetBkColor((HDC)wp, color(SY_UI_PANEL));
        return (INT_PTR)panel_brush;
    case WM_CLOSE:
        if (modal_dialog) return 0;
        read_seg();
        cfg_save();
        if (selector_hwnd) {
            PostMessageW(selector_hwnd, WM_CLOSE, 0, 0);
            quit_after = 1;
            return 0;
        }
        counting = 0;
        pending_at = 0;
        if (sess || shot) {
            if (sess && (phase == SY_PHASE_LIVE || phase == SY_PHASE_PAUSED)) sy_session_stop(sess);
            quit_after = 1;
            return 0;
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        stop_intro();
        KillTimer(w, ID_TIMER);
        UnregisterHotKey(w, HOT_START);
        UnregisterHotKey(w, HOT_PAUSE);
        UnregisterHotKey(w, HOT_SHOW);
        stop_click_rings();
        sy_win_identity_clear();
        if (shot) { sy_shot_close(shot); shot = NULL; }
        free(preview);
        preview = NULL;
        if (sess) { sy_session_stop(sess); sy_session_close(sess); sess = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HFONT make_font(int size, int weight) {
    return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
}

static int run_window(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(co)) sy_win_install_shortcuts(0);
    cfg_load();
    load_mons();
    load_engine();
    HDC screen = GetDC(NULL);
    if (screen) {
        int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        if (dpi > 0) ui_dpi = (UINT)dpi;
        ReleaseDC(NULL, screen);
    }

    app_icon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    HICON small_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = app_icon;
    wc.hIconSm = small_icon ? small_icon : app_icon;
    wc.lpszClassName = L"ShiyingMain";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = selector_proc;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"ShiyingSelector";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = click_ring_proc;
    wc.hCursor = NULL;
    wc.lpszClassName = L"ShiyingClickRing";
    RegisterClassExW(&wc);

    MONITORINFO monitor = { .cbSize = sizeof monitor };
    POINT cursor = {0};
    GetCursorPos(&cursor);
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        int fit_width = (monitor.rcWork.right - monitor.rcWork.left - 28) * 96 / TOOLBAR_W;
        int fit_height = (monitor.rcWork.bottom - monitor.rcWork.top - 48) * 96 / SETTINGS_H;
        if (fit_width >= 72 && (UINT)fit_width < ui_dpi) ui_dpi = (UINT)fit_width;
        if (fit_height >= 72 && (UINT)fit_height < ui_dpi) ui_dpi = (UINT)fit_height;
    } else {
        monitor.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        monitor.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    font = make_font(14, FW_NORMAL);
    font_emph = make_font(14, FW_SEMIBOLD);
    font_title = make_font(18, FW_SEMIBOLD);
    RECT r = box(0, 0, TOOLBAR_W, TOOLBAR_H);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    int width = r.right - r.left, height = r.bottom - r.top;
    int x = (monitor.rcWork.left + monitor.rcWork.right - width) / 2;
    int y = (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2;
    hwnd = CreateWindowW(L"ShiyingMain", SY_WIN_TITLE, style, x, y, width, height,
                        NULL, NULL, inst, NULL);
    if (!hwnd) {
        DeleteObject(font);
        DeleteObject(font_emph);
        DeleteObject(font_title);
        if (SUCCEEDED(co)) CoUninitialize();
        return 1;
    }
    sy_win_identity_set(hwnd);
    apply_theme();
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)app_icon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    create_controls();
    if (font) EnumChildWindows(hwnd, set_child_font, (LPARAM)font);
    fill_mons();
    fill_choices();
    if (!ff_path[0]) set_u8(hstatus, "找不到录制引擎（录制引擎/ffmpeg.exe），请保持程序目录完整");
    else set_u8(hstatus, sy_mode_labels[cfg.mode]);
    if (ff_path[0]) {
        SyStatus st = {0};
        if (sy_recover(cfg.outdir, ff_path, &sess, &st)) {
            phase = st.phase;
            paint_status(&st);
        }
    }
    layout_window();
    sync_ui();
    hot_start = RegisterHotKey(hwnd, HOT_START, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
    RegisterHotKey(hwnd, HOT_PAUSE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    RegisterHotKey(hwnd, HOT_SHOW, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
    start_intro();
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        int id = GetDlgCtrlID(msg.hwnd);
        BOOL toolbar_focus = msg.hwnd == hwnd || msg.hwnd == hstart ||
                             (id >= ID_MODE && id < ID_MODE + SY_MODE_COUNT);
        if (toolbar_focus && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && !locked()) {
            stop_intro();
            request_start();
        } else if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(panel_brush);
    DeleteObject(bg_brush);
    DeleteObject(font);
    DeleteObject(font_emph);
    DeleteObject(font_title);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

static int run_single_window(HINSTANCE inst) {
    raise_message = RegisterWindowMessageW(L"Shiying.Capture.Raise");
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\Shiying.Capture.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD acquired = WaitForSingleObject(mutex, 0);
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
            for (int tries = 0; tries < 60; tries++) {
                HWND previous = FindWindowW(L"ShiyingMain", NULL);
                if (previous) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(previous, &pid);
                    if (pid) AllowSetForegroundWindow(pid);
                    PostMessageW(previous, raise_message, 0, 0);
                    break;
                }
                acquired = WaitForSingleObject(mutex, 50);
                if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) break;
            }
        }
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return 0;
        }
    }
    int result = run_window(inst);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
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
    if (sy_win_launch_canonical()) return 0;
    SetProcessDPIAware();
    sy_win_identity_init();
    return run_single_window(inst);
}
