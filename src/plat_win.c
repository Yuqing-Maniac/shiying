#ifndef SHIYING_SESSION_C
#error "plat_win.c is included by session.c only"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

static int plat_audio_pipe(void) { return 0; }
static int plat_ts_wall(void) { return 0; }
static int plat_cursor_mag(void) { return 1; }
static int plat_audio_ok(void) { return 1; }

enum { GRAB_DDA = 1, GRAB_GDI = 2 };
enum { ROUTE_WASAPI = 0, ROUTE_DSHOW = 1 };

static int mon_i;
static HANDLE job_handle;
static HANDLE ctl_write = INVALID_HANDLE_VALUE;

enum { PROC_N = 16 };
static struct { DWORD pid; HANDLE proc; } procs[PROC_N];

typedef struct { int x, y, w, h; } WMon;
static WMon wmons[16];
static int nwmons;

static BOOL CALLBACK mon_cb(HMONITOR h, HDC dc, LPRECT r, LPARAM p) {
    (void)h; (void)dc; (void)p;
    if (nwmons >= 16 || !r) return TRUE;
    wmons[nwmons].x = r->left;
    wmons[nwmons].y = r->top;
    wmons[nwmons].w = r->right - r->left;
    wmons[nwmons].h = r->bottom - r->top;
    nwmons++;
    return TRUE;
}

static void load_mons(void) {
    nwmons = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
}

static int full_monitor(const SySession *s) {
    load_mons();
    for (int i = 0; i < nwmons; i++)
        if (wmons[i].x == s->plan.x && wmons[i].y == s->plan.y && wmons[i].w == s->plan.w && wmons[i].h == s->plan.h)
            return i;
    return -1;
}

static const char *draw_flag(const SySession *s) {
    return s->plan.cursor == SY_CURSOR_ORIG ? "1" : "0";
}

static void plat_append_video(SySession *s, ArgBuf *a) {
    if (s->grab == GRAB_DDA) {
        ap(a, "-f"); ap(a, "lavfi");
        ap(a, "-i");
        apf(a, "ddagrab=output_idx=%d:draw_mouse=%s:framerate=%d:video_size=%dx%d:offset_x=0:offset_y=0:output_fmt=bgra:dup_frames=1",
            mon_i, draw_flag(s), s->plan.fps, s->plan.w, s->plan.h);
    } else {
        ap(a, "-f"); ap(a, "gdigrab");
        ap(a, "-framerate"); apf(a, "%d", s->plan.fps);
        ap(a, "-offset_x"); apf(a, "%d", s->plan.x);
        ap(a, "-offset_y"); apf(a, "%d", s->plan.y);
        ap(a, "-video_size"); apf(a, "%dx%d", s->plan.w, s->plan.h);
        ap(a, "-draw_mouse"); ap(a, draw_flag(s));
        ap(a, "-i"); ap(a, "desktop");
    }
}

static int widen(const char *s, wchar_t *out, int n) {
    if (!s) s = "";
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0 ? 0 : -1;
}

static void append_arg(wchar_t *cmd, size_t cap, const char *arg) {
    wchar_t w[1200];
    if (widen(arg ? arg : "", w, 1200) != 0) return;
    size_t len = wcslen(cmd);
    int quote = w[0] == 0 || wcschr(w, L' ') || wcschr(w, L'\t') || wcschr(w, L'"');
    if (len && len + 1 < cap) cmd[len++] = L' ';
    if (quote && len + 1 < cap) cmd[len++] = L'"';
    for (wchar_t *p = w; *p && len + 3 < cap; p++) {
        if (*p == L'"') cmd[len++] = L'\\';
        cmd[len++] = *p;
    }
    if (quote && len + 1 < cap) cmd[len++] = L'"';
    cmd[len] = 0;
}

static int ensure_job(void) {
    if (job_handle) return 0;
    job_handle = CreateJobObjectW(NULL, NULL);
    if (!job_handle) return -1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &li, sizeof li)) return -1;
    return 0;
}

static void remember_proc(DWORD pid, HANDLE proc) {
    for (int i = 0; i < PROC_N; i++) if (!procs[i].proc) {
        procs[i].pid = pid;
        procs[i].proc = proc;
        return;
    }
    CloseHandle(proc);
}

static HANDLE find_proc(int pid) {
    for (int i = 0; i < PROC_N; i++)
        if (procs[i].proc && procs[i].pid == (DWORD)pid) return procs[i].proc;
    return NULL;
}

static void forget_proc(int pid) {
    for (int i = 0; i < PROC_N; i++) if (procs[i].proc && procs[i].pid == (DWORD)pid) {
        CloseHandle(procs[i].proc);
        procs[i].proc = NULL;
        procs[i].pid = 0;
    }
}

static HANDLE open_nul(DWORD access) {
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static int spawn_wide(wchar_t *cmd, HANDLE in, HANDLE out, HANDLE err, int suspended, PROCESS_INFORMATION *pi) {
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    DWORD flags = CREATE_NO_WINDOW;
    if (suspended) flags |= CREATE_SUSPENDED;
    memset(pi, 0, sizeof *pi);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si, pi)) return -1;
    return 0;
}

static void bind_job(HANDLE proc) {
    if (proc && ensure_job() == 0) AssignProcessToJobObject(job_handle, proc);
}

static int plat_capture(const char *ff, const char *arg, char *buf, size_t n) {
    if (!buf || n < 2) return -1;
    buf[0] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t cmd[4096];
    cmd[0] = 0;
    append_arg(cmd, 4096, ff);
    append_arg(cmd, 4096, "-hide_banner");
    append_arg(cmd, 4096, arg);
    HANDLE nul = open_nul(GENERIC_READ);
    PROCESS_INFORMATION pi;
    int rc = spawn_wide(cmd, nul, wr, wr, 0, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (rc != 0) { CloseHandle(rd); return -1; }
    bind_job(pi.hProcess);
    size_t o = 0;
    for (;;) {
        DWORD got = 0;
        if (o + 1 >= n) { TerminateProcess(pi.hProcess, 1); break; }
        if (!ReadFile(rd, buf + o, (DWORD)(n - 1 - o), &got, NULL) || got == 0) break;
        o += got;
    }
    buf[o] = 0;
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return 0;
}

static char dshow_name[256];

static int dshow_list(const char *ff) {
    dshow_name[0] = 0;
    char *buf = malloc(256 * 1024);
    if (!buf) return -1;
    wchar_t cmd[4096];
    cmd[0] = 0;
    append_arg(cmd, 4096, ff);
    append_arg(cmd, 4096, "-hide_banner");
    append_arg(cmd, 4096, "-list_devices");
    append_arg(cmd, 4096, "true");
    append_arg(cmd, 4096, "-f");
    append_arg(cmd, 4096, "dshow");
    append_arg(cmd, 4096, "-i");
    append_arg(cmd, 4096, "dummy");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { free(buf); return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = open_nul(GENERIC_READ);
    PROCESS_INFORMATION pi;
    int rc = spawn_wide(cmd, nul, wr, wr, 0, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (rc != 0) { CloseHandle(rd); free(buf); return -1; }
    bind_job(pi.hProcess);
    size_t o = 0;
    for (;;) {
        DWORD got = 0;
        if (o + 1 >= 256 * 1024) break;
        if (!ReadFile(rd, buf + o, (DWORD)(256 * 1024 - 1 - o), &got, NULL) || got == 0) break;
        o += got;
    }
    buf[o] = 0;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    int audio = 0;
    for (char *p = buf; *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (strstr(p, "audio devices")) audio = 1;
        else if (strstr(p, "video devices")) audio = 0;
        else if (audio) {
            char *q1 = strchr(p, '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 > q1 + 1) {
                size_t n = (size_t)(q2 - q1 - 1);
                if (n >= sizeof dshow_name) n = sizeof dshow_name - 1;
                memcpy(dshow_name, q1 + 1, n);
                dshow_name[n] = 0;
                break;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(buf);
    return dshow_name[0] ? 0 : -1;
}

static void wasapi_in(ArgBuf *a, int loopback, const char *dev) {
    ap(a, "-f"); ap(a, "wasapi");
    if (loopback) { ap(a, "-loopback"); ap(a, "1"); }
    ap(a, "-i"); ap(a, dev && dev[0] ? dev : "default");
}

static void plat_append_audio(SySession *s, ArgBuf *a) {
    if (s->audio_route == ROUTE_DSHOW) {
        if (!dshow_name[0]) dshow_list(s->ffmpeg);
        s->afilter[0] = 0;
        ap(a, "-f"); ap(a, "dshow");
        ap(a, "-i");
        if (dshow_name[0]) apf(a, "audio=%s", dshow_name);
        else ap(a, "audio=virtual-audio-capturer");
        return;
    }
    const char *id = s->plan.device_id;
    if (s->plan.audio == SY_AUDIO_DEVICE && id[0]) {
        int loop = 0;
        const char *dev = id;
        if (!strncmp(id, "ren:", 4)) { loop = 1; dev = id + 4; }
        else if (!strncmp(id, "cap:", 4)) dev = id + 4;
        wasapi_in(a, loop, dev);
        return;
    }
    if (s->plan.audio == SY_AUDIO_BOTH) {
        wasapi_in(a, 1, "default");
        wasapi_in(a, 0, "default");
        snprintf(s->afilter, sizeof s->afilter, "[1:a][2:a]amix=inputs=2:duration=longest:dropout_transition=0[a]");
        return;
    }
    if (s->plan.audio == SY_AUDIO_SYSTEM) wasapi_in(a, 1, "default");
    else wasapi_in(a, 0, "default");
}

static void plat_take_begin(SySession *s) {
    s->audio_route = ROUTE_WASAPI;
    int idx = full_monitor(s);
    if (idx >= 0) { s->grab = GRAB_DDA; mon_i = idx; }
    else s->grab = GRAB_GDI;
    dshow_name[0] = 0;
}

static int plat_fallback(SySession *s, uint64_t sz) {
    if (s->media_ok || sz >= 2048) return 0;
    if (s->grab == GRAB_DDA) {
        s->grab = GRAB_GDI;
        logf_("grab fallback gdigrab");
        return 1;
    }
    if (s->audio_on && s->audio_route == ROUTE_WASAPI) {
        s->audio_route = ROUTE_DSHOW;
        s->afilter[0] = 0;
        logf_("audio fallback dshow");
        return 1;
    }
    if (s->audio_on) {
        s->audio_on = 0;
        snprintf(s->warn, sizeof s->warn, "声音设备不可用，本次为无声录制");
        logf_("audio dropped");
        return 1;
    }
    return 0;
}

static int plat_spawn(char **argv, int aud_fd, int take_ctl) {
    (void)aud_fd;
    if (ensure_job() != 0) return -1;
    static wchar_t cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv && argv[i]; i++) append_arg(cmd, 32768, argv[i]);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = INVALID_HANDLE_VALUE;
    HANDLE nul_in = NULL;
    if (take_ctl) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else nul_in = open_nul(GENERIC_READ);
    wchar_t logp[1200];
    char logu[1200];
    snprintf(logu, sizeof logu, "%s/ffmpeg.log", g_cache_dir);
    HANDLE logh = INVALID_HANDLE_VALUE;
    if (widen(logu, logp, 1200) == 0)
        logh = CreateFileW(logp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul_out = open_nul(GENERIC_WRITE);
    HANDLE err = logh != INVALID_HANDLE_VALUE ? logh : nul_out;
    PROCESS_INFORMATION pi;
    int rc = spawn_wide(cmd, take_ctl ? in_r : nul_in, nul_out, err, 1, &pi);
    if (in_r) CloseHandle(in_r);
    if (nul_in && nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (nul_out && nul_out != INVALID_HANDLE_VALUE) CloseHandle(nul_out);
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
    if (rc != 0) {
        if (in_w != INVALID_HANDLE_VALUE) CloseHandle(in_w);
        return -1;
    }
    if (!AssignProcessToJobObject(job_handle, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (in_w != INVALID_HANDLE_VALUE) CloseHandle(in_w);
        return -1;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    remember_proc(pi.dwProcessId, pi.hProcess);
    if (take_ctl) {
        if (ctl_write != INVALID_HANDLE_VALUE) CloseHandle(ctl_write);
        ctl_write = in_w;
    }
    return (int)pi.dwProcessId;
}

static int plat_reap(int pid, int *code) {
    HANDLE h = find_proc(pid);
    if (!h) return 0;
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) return 0;
    DWORD c = 1;
    GetExitCodeProcess(h, &c);
    if (code) *code = (int)c;
    forget_proc(pid);
    return 1;
}

static void plat_kill(int pid) {
    HANDLE h = find_proc(pid);
    if (h) TerminateProcess(h, 1);
}

static int plat_send_q(void) {
    int ok = -1;
    if (ctl_write != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        if (WriteFile(ctl_write, "q", 1, &w, NULL) && w == 1) ok = 0;
        CloseHandle(ctl_write);
        ctl_write = INVALID_HANDLE_VALUE;
    }
    return ok;
}

typedef struct IMMDevEnum IMMDevEnum;
typedef struct IMMDevCol IMMDevCol;
typedef struct IMMDev IMMDev;
typedef struct IPropStore IPropStore;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMMDevEnum *, const IID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IMMDevEnum *);
    ULONG (STDMETHODCALLTYPE *Release)(IMMDevEnum *);
    HRESULT (STDMETHODCALLTYPE *EnumAudioEndpoints)(IMMDevEnum *, int, DWORD, IMMDevCol **);
    HRESULT (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(IMMDevEnum *, int, int, IMMDev **);
    HRESULT (STDMETHODCALLTYPE *GetDevice)(IMMDevEnum *, LPCWSTR, IMMDev **);
    HRESULT (STDMETHODCALLTYPE *RegisterEndpointNotificationCallback)(IMMDevEnum *, void *);
    HRESULT (STDMETHODCALLTYPE *UnregisterEndpointNotificationCallback)(IMMDevEnum *, void *);
} IMMDevEnumVtbl;
struct IMMDevEnum { const IMMDevEnumVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMMDevCol *, const IID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IMMDevCol *);
    ULONG (STDMETHODCALLTYPE *Release)(IMMDevCol *);
    HRESULT (STDMETHODCALLTYPE *GetCount)(IMMDevCol *, UINT *);
    HRESULT (STDMETHODCALLTYPE *Item)(IMMDevCol *, UINT, IMMDev **);
} IMMDevColVtbl;
struct IMMDevCol { const IMMDevColVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMMDev *, const IID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IMMDev *);
    ULONG (STDMETHODCALLTYPE *Release)(IMMDev *);
    HRESULT (STDMETHODCALLTYPE *Activate)(IMMDev *, const IID *, DWORD, void *, void **);
    HRESULT (STDMETHODCALLTYPE *OpenPropertyStore)(IMMDev *, DWORD, IPropStore **);
    HRESULT (STDMETHODCALLTYPE *GetId)(IMMDev *, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *GetState)(IMMDev *, DWORD *);
} IMMDevVtbl;
struct IMMDev { const IMMDevVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPropStore *, const IID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IPropStore *);
    ULONG (STDMETHODCALLTYPE *Release)(IPropStore *);
    HRESULT (STDMETHODCALLTYPE *GetCount)(IPropStore *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *GetAt)(IPropStore *, DWORD, void *);
    HRESULT (STDMETHODCALLTYPE *GetValue)(IPropStore *, const PROPERTYKEY *, PROPVARIANT *);
    HRESULT (STDMETHODCALLTYPE *SetValue)(IPropStore *, const PROPERTYKEY *, const PROPVARIANT *);
    HRESULT (STDMETHODCALLTYPE *Commit)(IPropStore *);
} IPropStoreVtbl;
struct IPropStore { const IPropStoreVtbl *lpVtbl; };

static const CLSID CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const PROPERTYKEY PKEY_FRIENDLY = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

static void utf8_from_wide(const wchar_t *w, char *out, size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
}

static void push_dev(SyDevice *out, int cap, int *n, const char *prefix, const wchar_t *id, const wchar_t *label) {
    if (*n >= cap || !id || !id[0]) return;
    char id8[400], lab8[240];
    utf8_from_wide(id, id8, sizeof id8);
    utf8_from_wide(label && label[0] ? label : id, lab8, sizeof lab8);
    snprintf(out[*n].id, sizeof out[*n].id, "%s%s", prefix, id8);
    snprintf(out[*n].label, sizeof out[*n].label, "%s", lab8);
    (*n)++;
}

static void enum_flow(IMMDevEnum *en, int flow, const char *prefix, SyDevice *out, int cap, int *n) {
    IMMDevCol *col = NULL;
    if (en->lpVtbl->EnumAudioEndpoints(en, flow, 1, &col) < 0 || !col) return;
    UINT count = 0;
    col->lpVtbl->GetCount(col, &count);
    for (UINT i = 0; i < count && *n < cap; i++) {
        IMMDev *dev = NULL;
        if (col->lpVtbl->Item(col, i, &dev) < 0 || !dev) continue;
        LPWSTR id = NULL;
        wchar_t *label = NULL;
        IPropStore *ps = NULL;
        if (dev->lpVtbl->GetId(dev, &id) >= 0 && dev->lpVtbl->OpenPropertyStore(dev, 0, &ps) >= 0 && ps) {
            PROPVARIANT var;
            memset(&var, 0, sizeof var);
            if (ps->lpVtbl->GetValue(ps, &PKEY_FRIENDLY, &var) >= 0 && var.vt == VT_LPWSTR) label = var.pwszVal;
            push_dev(out, cap, n, prefix, id, label);
            PropVariantClear(&var);
            ps->lpVtbl->Release(ps);
        }
        if (id) CoTaskMemFree(id);
        dev->lpVtbl->Release(dev);
    }
    col->lpVtbl->Release(col);
}

static int plat_devices(SyDevice *out, int cap) {
    if (!out || cap <= 0) return 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int inited = hr == S_OK || hr == S_FALSE;
    IMMDevEnum *en = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    int n = 0;
    if (hr >= 0 && en) {
        enum_flow(en, 0, "ren:", out, cap, &n);
        enum_flow(en, 1, "cap:", out, cap, &n);
        en->lpVtbl->Release(en);
    }
    if (inited && hr != RPC_E_CHANGED_MODE) CoUninitialize();
    (void)hr;
    return n;
}

enum { CUR_MAX = 256 };

/* 不调用 SetSystemCursor。那个调用在进程被结束后仍改着系统指针。
 * ShowCursor 的计数随进程结束恢复。 */

static HWND mag_win;
static HDC mag_dc;
static HBITMAP mag_bmp;
static void *mag_bits;
static unsigned char *mag_scratch;
static size_t mag_scratch_n;
static int mag_on, mag_hidden, mag_w, mag_h;

static void plat_cursor_off(void) {
    if (mag_win) ShowWindow(mag_win, SW_HIDE);
    if (mag_hidden) { ShowCursor(TRUE); mag_hidden = 0; }
    mag_on = 0;
}

static LRESULT CALLBACK mag_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(w, m, wp, lp);
}

static int mag_ready(void) {
    if (mag_win) return 1;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = mag_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ShiyingMag";
    RegisterClassW(&wc);
    mag_win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                              L"ShiyingMag", L"", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    return mag_win != NULL;
}

static int ensure_dib(int dw, int dh) {
    if (dw < 1 || dh < 1 || dw > CUR_MAX * 2 || dh > CUR_MAX * 2) return 0;
    if (mag_bmp && mag_bits && mag_w == dw && mag_h == dh) return 1;
    if (mag_bmp) { DeleteObject(mag_bmp); mag_bmp = NULL; mag_bits = NULL; }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    if (!mag_dc) mag_dc = CreateCompatibleDC(screen);
    mag_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &mag_bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!mag_dc || !mag_bmp || !mag_bits) return 0;
    SelectObject(mag_dc, mag_bmp);
    mag_w = dw;
    mag_h = dh;
    return 1;
}

static void fill_icon(HCURSOR cur, int dw, int dh, COLORREF bg) {
    RECT rc = { 0, 0, dw, dh };
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(mag_dc, &rc, br);
    DeleteObject(br);
    DrawIconEx(mag_dc, 0, 0, cur, dw, dh, 0, NULL, DI_NORMAL);
}

static void plat_cursor_tick(SySession *s) {
    int want = s && s->phase == SY_PHASE_LIVE && s->plan.cursor == SY_CURSOR_MAG
        && s->hold != SY_HOLD_PAUSE && s->hold != SY_HOLD_STOP;
    if (!want) { plat_cursor_off(); return; }
    if (!mag_ready()) return;
    CURSORINFO ci;
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetCursorInfo(&ci) || !ci.hCursor) return;
    ICONINFO ii;
    memset(&ii, 0, sizeof ii);
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    BITMAP bm;
    memset(&bm, 0, sizeof bm);
    int cw = 0, ch = 0;
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof bm, &bm) >= (int)sizeof bm) {
        cw = bm.bmWidth;
        ch = bm.bmHeight;
    } else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof bm, &bm) >= (int)sizeof bm) {
        cw = bm.bmWidth;
        ch = bm.bmHeight / 2;
    }
    int hotx = (int)ii.xHotspot, hoty = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (cw < 1 || ch < 1 || cw > CUR_MAX || ch > CUR_MAX) return;
    int dw = cw * 2, dh = ch * 2;
    if (!ensure_dib(dw, dh)) return;
    size_t nbytes = (size_t)dw * (size_t)dh * 4;
    if (mag_scratch_n < nbytes) {
        unsigned char *grown = realloc(mag_scratch, nbytes);
        if (!grown) return;
        mag_scratch = grown;
        mag_scratch_n = nbytes;
    }
    fill_icon(ci.hCursor, dw, dh, RGB(0, 0, 0));
    memcpy(mag_scratch, mag_bits, nbytes);
    fill_icon(ci.hCursor, dw, dh, RGB(255, 255, 255));
    for (int i = 0; i < dw * dh; i++) {
        unsigned char *b = mag_scratch + (size_t)i * 4;
        unsigned char *w = (unsigned char *)mag_bits + (size_t)i * 4;
        int db = (int)w[0] - (int)b[0]; if (db < 0) db = 0;
        int dg = (int)w[1] - (int)b[1]; if (dg < 0) dg = 0;
        int dr = (int)w[2] - (int)b[2]; if (dr < 0) dr = 0;
        int mx = db; if (dg > mx) mx = dg; if (dr > mx) mx = dr;
        int a = 255 - mx;
        if (a < 16) { w[0] = w[1] = w[2] = w[3] = 0; continue; }
        int r = (int)b[2] * 255 / a; if (r > 255) r = 255;
        int g = (int)b[1] * 255 / a; if (g > 255) g = 255;
        int bl = (int)b[0] * 255 / a; if (bl > 255) bl = 255;
        w[0] = (unsigned char)bl; w[1] = (unsigned char)g; w[2] = (unsigned char)r; w[3] = (unsigned char)a;
    }
    POINT dst = { ci.ptScreenPos.x - hotx * 2, ci.ptScreenPos.y - hoty * 2 };
    SIZE sz = { dw, dh };
    POINT srcpt = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(mag_win, NULL, &dst, &sz, mag_dc, &srcpt, 0, &bf, ULW_ALPHA);
    if (!mag_on) {
        ShowWindow(mag_win, SW_SHOWNOACTIVATE);
        ShowCursor(FALSE);
        mag_hidden = 1;
        mag_on = 1;
    }
}
