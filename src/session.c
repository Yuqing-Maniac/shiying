#define _GNU_SOURCE
#define SHIYING_SESSION_C
#include "session.h"
#include "path.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef SHIYING_WIN
#include <unistd.h>
#include <sys/statvfs.h>
#include "audio.h"
#else
#include <io.h>
#include <unistd.h>
#ifndef F_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#endif
#endif

enum { ARG_N = 96, ARG_L = 512 };
enum { SLOT_EMPTY = 0, SLOT_FLUSH = 1, SLOT_REMUX = 2 };

typedef struct { char b[ARG_N][ARG_L]; int n; } ArgBuf;

static void ap(ArgBuf *a, const char *s) {
    if (!a || a->n >= ARG_N) return;
    snprintf(a->b[a->n], ARG_L, "%s", s ? s : "");
    a->n++;
}
static void apf(ArgBuf *a, const char *fmt, ...) {
    if (!a || a->n >= ARG_N) return;
    va_list v;
    va_start(v, fmt);
    vsnprintf(a->b[a->n], ARG_L, fmt, v);
    va_end(v);
    a->n++;
}

struct SySession {
    SyPlan plan;
    char ffmpeg[1200];
    char stamp[32];
    char tmpdir[1300];
    char vf[80];
    char afilter[240];
    char list_path[1400];
    char partial[1400];
    char commit_dest[1200];
    char err[512];
    char warn[512];
    char last_file[1200];
    int ordinal, seg_next, ord_first;
    SyPhase phase;
    SyHold hold;
    int want_resume, audio_on, audio_started, recovered, sealed_count;
    int list_pending, commit_final, then_final, recovering;
    int live_pid, slot_pid, slot_kind, slot_killed, media_ok;
    double t_accum, t_seg, slot_since, last_dur;
    uint64_t last_bytes;
    int grab, audio_route;
};

static SySession *live_sess;

static const struct { const char *preset, *crf264, *crf265; } enc_row[] = {
    { "ultrafast", "26", "28" },
    { "superfast", "24", "26" },
    { "veryfast", "21", "23" },
};
static const char *X264P = "sliced-threads=1:rc-lookahead=0:sync-lookahead=0";
static const char *X265P = "open-gop=0:repeat-headers=1:rc-lookahead=0:bframes=0";

static int plat_audio_pipe(void);
static int plat_ts_wall(void);
static int plat_cursor_mag(void);
static int plat_audio_ok(void);
static void plat_append_video(SySession *s, ArgBuf *a);
#ifdef SHIYING_WIN
static void plat_append_audio(SySession *s, ArgBuf *a);
static int plat_capture(const char *ff, const char *arg, char *buf, size_t n);
#endif
static void plat_take_begin(SySession *s);
static int plat_fallback(SySession *s, uint64_t sz);
static int plat_spawn(char **argv, int aud_fd, int take_ctl);
static int plat_reap(int pid, int *code);
static void plat_kill(int pid);
static int plat_send_q(void);
static int plat_devices(SyDevice *out, int cap);
static void plat_cursor_tick(SySession *s);
static void plat_cursor_off(void);

#ifdef SHIYING_WIN
#include "plat_win.c"
#else
#include "plat_linux.c"
#endif

static void mix_end(SySession *s) {
#ifndef SHIYING_WIN
    if (s->audio_started) audio_end_fd();
#else
    (void)s;
#endif
}
#ifndef SHIYING_WIN
static void mix_set(SySession *s, int fd, double origin) {
    if (s->audio_on && s->audio_started) audio_set_fd(fd, origin);
}
#endif
static void mix_stop(SySession *s) {
#ifndef SHIYING_WIN
    if (s->audio_started) { audio_stop(); s->audio_started = 0; }
#else
    (void)s;
#endif
}

static void log_tail(char *out, size_t n) {
    char p[1200], line[600], last[600] = "";
    snprintf(p, sizeof p, "%s/ffmpeg.log", g_cache_dir);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(last, sizeof last, "%s", line);
    }
    fclose(f);
    snprintf(out, n, "%s", last);
}

static const char *ext_of(const SySession *s) { return s->plan.container == SY_MKV ? "mkv" : "mp4"; }

static void dest_of(const SySession *s, int ordinal, char *out, size_t n) {
    if (ordinal <= 1)
        snprintf(out, n, "%s/录屏_%s.%s", s->plan.outdir, s->stamp, ext_of(s));
    else
        snprintf(out, n, "%s/录屏_%s_%03d.%s", s->plan.outdir, s->stamp, ordinal, ext_of(s));
}

static void recover_name(const char *dest, char *out, size_t n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", dest);
    char *slash = strrchr(tmp, '/');
    char *dot = strrchr(tmp, '.');
    if (dot && (!slash || dot > slash)) {
        *dot = 0;
        snprintf(out, n, "%s_恢复.%s", tmp, dot + 1);
    } else snprintf(out, n, "%s_恢复.mp4", tmp);
}

static uint64_t seg_size(const SySession *s, int idx) {
    char p[1500];
    struct stat st;
    snprintf(p, sizeof p, "%s/seg_%03d.mkv", s->tmpdir, idx);
    if (stat(p, &st) != 0 || st.st_size < 0) return 0;
    return (uint64_t)st.st_size;
}

static uint64_t current_bytes(const SySession *s) {
    uint64_t n = 0;
    for (int i = s->ord_first; i < s->seg_next; i++) n += seg_size(s, i);
    return n;
}

static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char p[1600];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void write_target(SySession *s) {
    char p[1500], dest[1200];
    dest_of(s, s->ordinal, dest, sizeof dest);
    snprintf(p, sizeof p, "%s/target.txt", s->tmpdir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%s\ncodec=%d\ncontainer=%d\n", dest, (int)s->plan.codec, (int)s->plan.container);
    file_fsync(f);
    fclose(f);
}

static void read_target_file(const char *dir, char *dest, size_t nd, int *codec, int *container) {
    char p[1500], line[1200];
    snprintf(p, sizeof p, "%s/target.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (dest && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(dest, nd, "%s", line);
    }
    while (fgets(line, sizeof line, f)) {
        if (codec && !strncmp(line, "codec=", 6)) *codec = atoi(line + 6);
        if (container && !strncmp(line, "container=", 10)) *container = atoi(line + 10);
    }
    fclose(f);
}

static int collect_segs(SySession *s, int *idx, int cap) {
    DIR *d = opendir(s->tmpdir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int i = -1;
        if (sscanf(e->d_name, "seg_%d.mkv", &i) != 1 || i < 0) continue;
        if (seg_size(s, i) < 2048) continue;
        if (n < cap) idx[n++] = i;
    }
    closedir(d);
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (idx[b] < idx[a]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    return n;
}

static int list_dest(const char *list, char *dest, size_t n) {
    FILE *f = fopen(list, "r");
    if (!f) return -1;
    if (!fgets(dest, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    dest[strcspn(dest, "\r\n")] = 0;
    return dest[0] ? 0 : -1;
}

static int write_concat(const SySession *s, const char *list, const char *concat) {
    FILE *in = fopen(list, "r");
    FILE *out = fopen(concat, "w");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return -1; }
    char line[600];
    int first = 1, n = 0;
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        if (first) { first = 0; continue; }
        if (!line[0] || strchr(line, '=')) continue;
        fprintf(out, "file '%s/%s'\n", s->tmpdir, line);
        n++;
    }
    file_fsync(out);
    fclose(in);
    fclose(out);
    return n;
}

static void unlink_listed(SySession *s, const char *list) {
    FILE *f = fopen(list, "r");
    if (f) {
        char line[600];
        int first = 1;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (first) { first = 0; continue; }
            if (!line[0] || strchr(line, '=')) continue;
            char p[1600];
            snprintf(p, sizeof p, "%s/%s", s->tmpdir, line);
            unlink(p);
        }
        fclose(f);
    }
    unlink(list);
    char c[1500];
    snprintf(c, sizeof c, "%s/seal.concat", s->tmpdir);
    unlink(c);
}

static void remember_seal(SySession *s, const char *dest, uint64_t bytes) {
    snprintf(s->last_file, sizeof s->last_file, "%s", dest);
    s->last_bytes = bytes;
    s->sealed_count++;
    s->last_dur = s->t_accum;
}

static int write_list_idx(SySession *s, const char *dest, const int *idx, int n) {
    if (n <= 0) return -1;
    snprintf(s->list_path, sizeof s->list_path, "%s/seal.list", s->tmpdir);
    FILE *f = fopen(s->list_path, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", dest);
    for (int i = 0; i < n; i++) fprintf(f, "seg_%03d.mkv\n", idx[i]);
    file_fsync(f);
    fclose(f);
    s->list_pending = 1;
    return 0;
}

static int write_list(SySession *s, const char *dest) {
    if (s->list_pending) return -1;
    if (s->list_path[0] && access(s->list_path, F_OK) == 0) return -1;
    int idx[64], n = 0;
    for (int i = s->ord_first; i < s->seg_next && n < 64; i++)
        if (seg_size(s, i) >= 2048) idx[n++] = i;
    return write_list_idx(s, dest, idx, n);
}

static void finish_take(SySession *s) {
    int left[64];
    int n = collect_segs(s, left, 64);
    if (!getenv("SHIYING_KEEP_SEGMENTS") && n == 0) rm_tree(s->tmpdir);
    mix_stop(s);
    s->last_dur = s->t_accum;
    s->phase = SY_PHASE_IDLE;
    s->hold = SY_HOLD_NONE;
    s->commit_final = 0;
    s->then_final = 0;
    plat_cursor_off();
}

static void abandon_take(SySession *s) {
    mix_stop(s);
    s->phase = SY_PHASE_IDLE;
    s->hold = SY_HOLD_NONE;
    s->commit_final = 0;
    s->then_final = 0;
    plat_cursor_off();
}

static void on_seal_done(SySession *s, int failed);

static int syi_seal_commit(const char *list, const char *dest) {
    SySession *s = live_sess;
    if (!s || !list || !dest || !dest[0]) return -1;
    struct stat st;
    if (stat(dest, &st) == 0 && st.st_size > 0) {
        unlink_listed(s, list);
        s->list_path[0] = 0;
        s->list_pending = 0;
        remember_seal(s, dest, (uint64_t)st.st_size);
        logf_("sealed %s", dest);
        on_seal_done(s, 0);
        return 0;
    }
    char concat[1500];
    snprintf(concat, sizeof concat, "%s/seal.concat", s->tmpdir);
    snprintf(s->partial, sizeof s->partial, "%s.partial", dest);
    snprintf(s->commit_dest, sizeof s->commit_dest, "%s", dest);
    int nfiles = write_concat(s, list, concat);
    if (nfiles <= 0) {
        if (!s->last_file[0] && !s->err[0]) snprintf(s->err, sizeof s->err, "没有录到有效画面");
        s->list_pending = 0;
        on_seal_done(s, 1);
        return -1;
    }
    int mp4 = 0;
    size_t dl = strlen(dest);
    if (dl >= 4 && !strcmp(dest + dl - 4, ".mp4")) mp4 = 1;
    int hevc = s->plan.codec == SY_H265;
    ArgBuf a = {0};
    ap(&a, s->ffmpeg);
    ap(&a, "-hide_banner"); ap(&a, "-loglevel"); ap(&a, "error"); ap(&a, "-nostdin");
    ap(&a, "-f"); ap(&a, "concat"); ap(&a, "-safe"); ap(&a, "0"); ap(&a, "-i"); ap(&a, concat);
    ap(&a, "-map"); ap(&a, "0"); ap(&a, "-c"); ap(&a, "copy");
    if (mp4) { ap(&a, "-movflags"); ap(&a, "+faststart"); }
    if (mp4 && hevc) { ap(&a, "-tag:v"); ap(&a, "hvc1"); }
    /* 成品先写 dest.partial，扩展名不再是 mp4，格式必须写明。 */
    ap(&a, "-f"); ap(&a, mp4 ? "mp4" : "matroska");
    ap(&a, "-y"); ap(&a, s->partial);
    char *av[ARG_N + 1];
    for (int i = 0; i < a.n; i++) av[i] = a.b[i];
    av[a.n] = NULL;
    int pid = plat_spawn(av, -1, 0);
    if (pid < 0) {
        if (!s->err[0]) snprintf(s->err, sizeof s->err, "无法合并视频");
        unlink(s->partial);
        on_seal_done(s, 1);
        return -1;
    }
    s->slot_pid = pid;
    s->slot_kind = SLOT_REMUX;
    s->slot_since = now_s();
    s->slot_killed = 0;
    s->list_pending = 0;
    return 1;
}

static int launch_pending(SySession *s) {
    char dest[1200];
    if (!s->list_path[0] || list_dest(s->list_path, dest, sizeof dest) != 0) {
        s->list_pending = 0;
        return -1;
    }
    s->list_pending = 1;
    return syi_seal_commit(s->list_path, dest);
}

static void on_seal_done(SySession *s, int failed) {
    if (!s || s->phase == SY_PHASE_IDLE) return;
    s->list_pending = 0;
    if (failed) {
        if (s->commit_final || s->then_final || s->recovering) abandon_take(s);
        return;
    }
    if (s->recovering) return;
    if (s->then_final) {
        s->then_final = 0;
        s->commit_final = 1;
        char dest[1200];
        dest_of(s, s->ordinal, dest, sizeof dest);
        s->list_path[0] = 0;
        if (write_list(s, dest) == 0) launch_pending(s);
        else finish_take(s);
        return;
    }
    if (s->commit_final) finish_take(s);
}

static void finish_remux(SySession *s, int code) {
    struct stat st;
    int ok = code == 0 && stat(s->partial, &st) == 0 && st.st_size > 0;
    if (!ok) {
        unlink(s->partial);
        char tail[300];
        log_tail(tail, sizeof tail);
        if (!s->err[0]) snprintf(s->err, sizeof s->err, "合并视频失败（%s）", tail);
        logf_("save failed: %s", s->err);
        on_seal_done(s, 1);
        return;
    }
    path_fsync(s->partial);
    if (rename(s->partial, s->commit_dest) != 0) {
        unlink(s->partial);
        if (!s->err[0]) snprintf(s->err, sizeof s->err, "无法完成保存");
        on_seal_done(s, 1);
        return;
    }
    unlink_listed(s, s->list_path);
    s->list_path[0] = 0;
    remember_seal(s, s->commit_dest, (uint64_t)st.st_size);
    logf_("sealed %s", s->commit_dest);
    on_seal_done(s, 0);
}

static int start_piece(SySession *s) {
    if (s->live_pid) return -1;
    ArgBuf a = {0};
    int idx = s->seg_next;
    s->afilter[0] = 0;
    int pipe_audio = s->audio_on && plat_audio_pipe();
    double wall = 0;
#ifndef SHIYING_WIN
    double mono = 0;
    if (pipe_audio || plat_ts_wall()) { wall = wall_s(); mono = now_s(); }
#else
    (void)pipe_audio;
#endif
    ap(&a, s->ffmpeg);
    ap(&a, "-hide_banner"); ap(&a, "-loglevel"); ap(&a, "warning"); ap(&a, "-nostats");
    if (plat_ts_wall()) ap(&a, "-copyts");
    ap(&a, "-thread_queue_size"); ap(&a, "16");
    ap(&a, "-probesize"); ap(&a, "32"); ap(&a, "-analyzeduration"); ap(&a, "0");
    plat_append_video(s, &a);
    int aud_fd = -1;
#ifndef SHIYING_WIN
    int pfd[2] = { -1, -1 };
    if (pipe_audio) {
        ap(&a, "-thread_queue_size"); ap(&a, "4096");
        ap(&a, "-probesize"); ap(&a, "32"); ap(&a, "-analyzeduration"); ap(&a, "0");
        ap(&a, "-itsoffset"); apf(&a, "%.6f", wall);
        ap(&a, "-f"); ap(&a, "s16le"); ap(&a, "-ar"); ap(&a, "48000"); ap(&a, "-ac"); ap(&a, "2");
        ap(&a, "-i"); ap(&a, "/proc/self/fd/3");
        if (pipe2(pfd, O_CLOEXEC) != 0) {
            snprintf(s->err, sizeof s->err, "无法创建声音管道");
            return -1;
        }
        fcntl(pfd[1], F_SETPIPE_SZ, 4096);
        fcntl(pfd[1], F_SETFL, O_NONBLOCK);
        aud_fd = pfd[0];
    }
#else
    if (s->audio_on) plat_append_audio(s, &a);
#endif
    ap(&a, "-map"); ap(&a, "0:v");
    if (s->audio_on) {
        if (s->afilter[0]) { ap(&a, "-filter_complex"); ap(&a, s->afilter); ap(&a, "-map"); ap(&a, "[a]"); }
        else { ap(&a, "-map"); ap(&a, "1:a"); }
    }
    if (s->vf[0]) { ap(&a, "-vf"); ap(&a, s->vf); }
    int q = s->plan.quality;
    if (q < 0 || q > 2) q = 1;
    int hevc = s->plan.codec == SY_H265;
    ap(&a, "-c:v"); ap(&a, hevc ? "libx265" : "libx264");
    ap(&a, "-preset"); ap(&a, enc_row[q].preset);
    ap(&a, "-crf"); ap(&a, hevc ? enc_row[q].crf265 : enc_row[q].crf264);
    ap(&a, "-pix_fmt"); ap(&a, "yuv420p");
    ap(&a, "-g"); apf(&a, "%d", s->plan.fps * 4);
    ap(&a, "-threads"); ap(&a, "4");
    if (hevc) { ap(&a, "-x265-params"); ap(&a, X265P); }
    else { ap(&a, "-x264-params"); ap(&a, X264P); }
    if (s->audio_on) { ap(&a, "-c:a"); ap(&a, "aac"); ap(&a, "-b:a"); ap(&a, "128k"); }
    if (plat_ts_wall()) { ap(&a, "-output_ts_offset"); apf(&a, "-%.6f", wall); }
    ap(&a, "-cluster_time_limit"); ap(&a, "2000");
    ap(&a, "-flush_packets"); ap(&a, "1");
    ap(&a, "-f"); ap(&a, "matroska"); ap(&a, "-y");
    apf(&a, "%s/seg_%03d.mkv", s->tmpdir, idx);
    char *av[ARG_N + 1];
    for (int i = 0; i < a.n; i++) av[i] = a.b[i];
    av[a.n] = NULL;
    int pid = plat_spawn(av, aud_fd, 1);
#ifndef SHIYING_WIN
    if (pipe_audio) close(pfd[0]);
#endif
    if (pid < 0) {
#ifndef SHIYING_WIN
        if (pipe_audio) close(pfd[1]);
#endif
        if (!s->err[0]) snprintf(s->err, sizeof s->err, "无法启动录制引擎");
        return -1;
    }
    s->live_pid = pid;
    s->seg_next = idx + 1;
    s->t_seg = now_s();
#ifndef SHIYING_WIN
    if (pipe_audio) mix_set(s, pfd[1], mono);
#endif
    logf_("segment %d pid=%d", idx, pid);
    return 0;
}

static void arm_flush(SySession *s) {
    int pid = s->live_pid;
    mix_end(s);
    if (plat_send_q() != 0) plat_kill(pid);
    double dt = now_s() - s->t_seg;
    if (dt > 0) s->t_accum += dt;
    s->slot_pid = pid;
    s->slot_kind = SLOT_FLUSH;
    s->slot_since = now_s();
    s->slot_killed = 0;
    s->live_pid = 0;
}

static void begin_final_seal(SySession *s) {
    if (s->phase == SY_PHASE_IDLE) return;
    s->phase = SY_PHASE_SEALING;
    plat_cursor_off();
    if (s->list_path[0] && access(s->list_path, F_OK) == 0) {
        s->then_final = 1;
        s->commit_final = 0;
        s->list_pending = 1;
        if (launch_pending(s) < 0) abandon_take(s);
        return;
    }
    s->commit_final = 1;
    char dest[1200];
    dest_of(s, s->ordinal, dest, sizeof dest);
    if (write_list(s, dest) != 0) {
        if (!s->last_file[0] && !s->err[0]) snprintf(s->err, sizeof s->err, "没有录到有效画面");
        if (!s->last_file[0] && !getenv("SHIYING_KEEP_SEGMENTS")) rm_tree(s->tmpdir);
        finish_take(s);
        return;
    }
    if (launch_pending(s) < 0) abandon_take(s);
}

static void rotate_cut(SySession *s) {
    char dest[1200];
    dest_of(s, s->ordinal, dest, sizeof dest);
    if (write_list(s, dest) != 0) { s->hold = SY_HOLD_STOP; return; }
    s->ordinal++;
    s->ord_first = s->seg_next;
    write_target(s);
    arm_flush(s);
    s->hold = SY_HOLD_CUT;
    s->phase = SY_PHASE_LIVE;
    if (start_piece(s) != 0) s->hold = SY_HOLD_STOP;
}

static void finish_pause_flush(SySession *s) {
    uint64_t b = current_bytes(s);
    if (s->plan.limit_bytes > 0 && b >= s->plan.limit_bytes) {
        char dest[1200];
        dest_of(s, s->ordinal, dest, sizeof dest);
        if (write_list(s, dest) == 0) {
            s->ordinal++;
            s->ord_first = s->seg_next;
            write_target(s);
            s->phase = SY_PHASE_PAUSED;
            launch_pending(s);
            return;
        }
    }
    s->phase = SY_PHASE_PAUSED;
    if (s->want_resume && !s->slot_pid) {
        s->want_resume = 0;
        s->hold = SY_HOLD_NONE;
        s->phase = SY_PHASE_LIVE;
        if (start_piece(s) != 0) s->hold = SY_HOLD_STOP;
    }
}

static void on_live_exit(SySession *s, int code) {
    double ran = now_s() - s->t_seg;
    if (ran > 0) s->t_accum += ran;
    uint64_t sz = s->seg_next > 0 ? seg_size(s, s->seg_next - 1) : 0;
    if (sz >= 2048) s->media_ok = 1;
    logf_("segment exited unexpectedly code=%d ran=%.1fs", code, ran);
    mix_end(s);
    if (!s->media_ok && plat_fallback(s, sz)) {
        if (start_piece(s) == 0) return;
    }
    char tail[300];
    log_tail(tail, sizeof tail);
    if (!s->err[0]) snprintf(s->err, sizeof s->err, "录制意外结束（%d）：%s", code, tail);
    s->hold = SY_HOLD_STOP;
    s->want_resume = 0;
    plat_cursor_off();
}

static void on_slot_empty(SySession *s) {
    if (!s || s->phase == SY_PHASE_IDLE || s->slot_pid) return;
    if (s->phase == SY_PHASE_SEALING) {
        if (s->list_pending) {
            launch_pending(s);
            return;
        }
        if (s->recovering) {
            int idx[64];
            int n = collect_segs(s, idx, 64);
            if (n > 0) {
                char base[1200], dest[1200];
                base[0] = 0;
                read_target_file(s->tmpdir, base, sizeof base, NULL, NULL);
                if (!base[0]) dest_of(s, 1, base, sizeof base);
                recover_name(base, dest, sizeof dest);
                s->list_path[0] = 0;
                if (write_list_idx(s, dest, idx, n) == 0) launch_pending(s);
                else finish_take(s);
                return;
            }
            if (!getenv("SHIYING_KEEP_SEGMENTS")) rm_tree(s->tmpdir);
            s->phase = SY_PHASE_IDLE;
            s->hold = SY_HOLD_NONE;
            s->last_dur = s->t_accum;
        }
        return;
    }
    if (s->hold == SY_HOLD_PAUSE && s->want_resume && s->live_pid) {
        s->hold = SY_HOLD_NONE;
        s->want_resume = 0;
        s->phase = SY_PHASE_LIVE;
        return;
    }
    if (s->hold == SY_HOLD_STOP) {
        if (s->live_pid) { arm_flush(s); return; }
        begin_final_seal(s);
        return;
    }
    if (s->hold == SY_HOLD_PAUSE) {
        if (s->live_pid) { arm_flush(s); plat_cursor_off(); return; }
        if (s->phase == SY_PHASE_PAUSED && s->want_resume) {
            s->want_resume = 0;
            s->hold = SY_HOLD_NONE;
            s->phase = SY_PHASE_LIVE;
            if (start_piece(s) != 0) s->hold = SY_HOLD_STOP;
        }
        return;
    }
    if (s->phase == SY_PHASE_LIVE && s->hold == SY_HOLD_NONE && s->live_pid
        && s->plan.limit_bytes > 0 && current_bytes(s) >= s->plan.limit_bytes)
        rotate_cut(s);
}

static double elapsed_of(const SySession *s) {
    if (s->phase == SY_PHASE_LIVE && s->live_pid) {
        double d = now_s() - s->t_seg;
        return s->t_accum + (d > 0 ? d : 0);
    }
    return s->t_accum;
}

static void fill_status(const SySession *s, SyStatus *st) {
    memset(st, 0, sizeof *st);
    if (!s) return;
    st->phase = s->phase;
    st->hold = s->hold;
    st->elapsed = elapsed_of(s);
    st->sealed_count = s->sealed_count;
    st->recovered = s->recovered;
    snprintf(st->last_file, sizeof st->last_file, "%s", s->last_file);
    st->last_bytes = s->last_bytes;
    st->last_dur = s->last_dur;
    snprintf(st->err, sizeof st->err, "%s", s->err);
    snprintf(st->warn, sizeof st->warn, "%s", s->warn);
}

uint64_t sy_limit_bytes(int n, int unit) {
    if (n <= 0) return 0;
    uint64_t u = (uint64_t)n;
    if (unit == SY_UNIT_GB) return u * 1024ull * 1024ull * 1024ull;
    return u * 1024ull * 1024ull;
}

#ifndef SHIYING_WIN
static int copy_file(const char *a, const char *b) {
    FILE *in = fopen(a, "rb");
    if (!in) return -1;
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.part", b);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[1 << 16];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = -1; break; }
    file_fsync(out);
    fclose(in);
    fclose(out);
    chmod(tmp, 0755);
    if (ok == 0) ok = rename(tmp, b);
    return ok;
}
#endif

int sy_ffmpeg_resolve(char *out, size_t n) {
    if (!out || n == 0) return -1;
    static char resolved[1200];
    if (resolved[0] && access(resolved, 0) == 0) {
#ifndef SHIYING_WIN
        if (access(resolved, X_OK) == 0) { snprintf(out, n, "%s", resolved); return 0; }
#else
        snprintf(out, n, "%s", resolved);
        return 0;
#endif
    }
    char c[1200];
#ifdef SHIYING_WIN
    const char *fmts[] = { "%s/录制引擎/ffmpeg.exe", "%s/ffmpeg.exe", NULL };
    for (int i = 0; fmts[i]; i++) {
        snprintf(c, sizeof c, fmts[i], g_exe_dir);
        if (access(c, 0) == 0) { snprintf(resolved, sizeof resolved, "%s", c); snprintf(out, n, "%s", c); return 0; }
    }
    return -1;
#else
    const char *fmts[] = { "%s/录制引擎/ffmpeg", "%s/bin/ffmpeg", "%s/ffmpeg", "%s/../lib/shiying/ffmpeg", NULL };
    for (int i = 0; fmts[i]; i++) {
        snprintf(c, sizeof c, fmts[i], g_exe_dir);
        struct stat st;
        if (stat(c, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        struct statvfs v;
        int noexec = statvfs(c, &v) == 0 && (v.f_flag & ST_NOEXEC);
        if (access(c, X_OK) == 0 && !noexec) {
            snprintf(resolved, sizeof resolved, "%s", c);
            snprintf(out, n, "%s", c);
            return 0;
        }
        char dst[1200];
        snprintf(dst, sizeof dst, "%s/ffmpeg", g_cache_dir);
        struct stat ds;
        if (!(stat(dst, &ds) == 0 && ds.st_size == st.st_size)) copy_file(c, dst);
        if (access(dst, X_OK) == 0) {
            snprintf(resolved, sizeof resolved, "%s", dst);
            snprintf(out, n, "%s", dst);
            return 0;
        }
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char pb[4096];
    snprintf(pb, sizeof pb, "%s", path);
    for (char *t = strtok(pb, ":"); t; t = strtok(NULL, ":")) {
        snprintf(c, sizeof c, "%s/ffmpeg", t);
        if (access(c, X_OK) == 0) {
            snprintf(resolved, sizeof resolved, "%s", c);
            snprintf(out, n, "%s", c);
            return 0;
        }
    }
    return -1;
#endif
}

static int ff_capture(const char *ff, const char *arg, char *buf, size_t n) {
    buf[0] = 0;
#ifndef SHIYING_WIN
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]); close(p[1]);
        execl(ff, ff, "-hide_banner", arg, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    size_t o = 0;
    while (o + 1 < n) {
        ssize_t r = read(p[0], buf + o, n - 1 - o);
        if (r <= 0) break;
        o += (size_t)r;
    }
    buf[o] = 0;
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    return 0;
#else
    return plat_capture(ff, arg, buf, n);
#endif
}

static int has_tok(const char *buf, const char *w) {
    if (!buf || !w || !w[0]) return 0;
    size_t n = strlen(w);
    for (const char *p = buf; (p = strstr(p, w)); p++) {
        int left = p == buf || !isalnum((unsigned char)p[-1]);
        int right = !isalnum((unsigned char)p[n]);
        if (left && right) return 1;
        if (!p[1]) break;
    }
    return 0;
}

int sy_probe(const char *ffmpeg, SyCaps *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!ffmpeg || !ffmpeg[0]) return -1;
    static char cached[1200];
    static SyCaps cc;
    static int have;
    if (have && !strcmp(cached, ffmpeg)) { *out = cc; return 0; }
    char *buf = malloc(512 * 1024);
    if (!buf) return -1;
    if (ff_capture(ffmpeg, "-encoders", buf, 512 * 1024) != 0) { free(buf); return -1; }
    out->h264 = has_tok(buf, "libx264");
    out->h265 = has_tok(buf, "libx265");
    if (ff_capture(ffmpeg, "-formats", buf, 512 * 1024) == 0) {
        out->mp4 = has_tok(buf, "mp4");
        out->mkv = has_tok(buf, "matroska");
    }
    if (ff_capture(ffmpeg, "-devices", buf, 512 * 1024) == 0) {
        out->grab_x11 = has_tok(buf, "x11grab");
        out->grab_gdi = has_tok(buf, "gdigrab");
    }
    if (ff_capture(ffmpeg, "-filters", buf, 512 * 1024) == 0)
        out->grab_dda = has_tok(buf, "ddagrab");
    free(buf);
    out->cursor_mag = plat_cursor_mag();
    out->audio_ok = plat_audio_ok();
    snprintf(cached, sizeof cached, "%s", ffmpeg);
    cc = *out;
    have = 1;
    return 0;
}

int sy_devices(SyDevice *out, int cap) { return plat_devices(out, cap); }

int sy_codec_choices(const SyCaps *caps, SyChoice *out, int cap) {
    int n = 0;
    if (!caps || !out || cap <= 0) return 0;
    if (caps->h264 && n < cap) { out[n].id = SY_H264; out[n].label = "H.264"; n++; }
    if (caps->h265 && n < cap) { out[n].id = SY_H265; out[n].label = "H.265"; n++; }
    return n;
}
int sy_container_choices(const SyCaps *caps, SyChoice *out, int cap) {
    int n = 0;
    if (!caps || !out || cap <= 0) return 0;
    if (caps->mp4 && n < cap) { out[n].id = SY_MP4; out[n].label = "MP4"; n++; }
    if (caps->mkv && n < cap) { out[n].id = SY_MKV; out[n].label = "MKV"; n++; }
    return n;
}
int sy_cursor_choices(const SyCaps *caps, SyChoice *out, int cap) {
    int n = 0;
    if (!out || cap <= 0) return 0;
    if (n < cap) { out[n].id = SY_CURSOR_OFF; out[n].label = "关"; n++; }
    if (n < cap) { out[n].id = SY_CURSOR_ORIG; out[n].label = "原大"; n++; }
    if (caps && caps->cursor_mag && n < cap) { out[n].id = SY_CURSOR_MAG; out[n].label = "放大"; n++; }
    return n;
}
int sy_audio_choices(SyChoice *out, int cap) {
    static const SyChoice all[] = {
        { SY_AUDIO_SILENCE, "无声" },
        { SY_AUDIO_SYSTEM, "系统声音" },
        { SY_AUDIO_MIC, "麦克风" },
        { SY_AUDIO_BOTH, "两者" },
    };
    int n = (int)(sizeof all / sizeof all[0]);
    if (!out || cap <= 0) return 0;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = all[i];
    return n;
}

void sy_clamp_recorded(SyCodec *codec, SyContainer *container, SyCursor *cursor, const SyCaps *caps) {
    SyChoice buf[8];
    if (!codec || !container || !cursor) return;
    int n = sy_codec_choices(caps, buf, 8);
    int ok = 0;
    for (int i = 0; i < n; i++) if (buf[i].id == (int)*codec) ok = 1;
    if (n && !ok) *codec = (SyCodec)buf[0].id;
    n = sy_container_choices(caps, buf, 8);
    ok = 0;
    for (int i = 0; i < n; i++) if (buf[i].id == (int)*container) ok = 1;
    if (n && !ok) *container = (SyContainer)buf[0].id;
    n = sy_cursor_choices(caps, buf, 8);
    ok = 0;
    for (int i = 0; i < n; i++) if (buf[i].id == (int)*cursor) ok = 1;
    if (n && !ok) *cursor = (SyCursor)buf[0].id;
}

static void fail_open(SySession *s, SyStatus *st, int code) {
    if (s) {
        mix_stop(s);
        if (s->tmpdir[0]) rm_tree(s->tmpdir);
        fill_status(s, st);
        if (live_sess == s) live_sess = NULL;
        free(s);
    } else if (st) memset(st, 0, sizeof *st);
    (void)code;
}

int sy_session_open(SySession **out, const SyPlan *plan, const char *ffmpeg,
                    const SyCaps *caps, SyStatus *st) {
    if (out) *out = NULL;
    if (st) memset(st, 0, sizeof *st);
    if (!out || !plan || !st) return SY_ERR_FFMPEG;
    if (live_sess) {
        snprintf(st->err, sizeof st->err, "已有录制正在进行");
        return SY_ERR_BUSY;
    }
    if (!ffmpeg || !ffmpeg[0] || access(ffmpeg, 0) != 0) {
        snprintf(st->err, sizeof st->err, "找不到录制引擎（录制引擎/ffmpeg），请保持程序目录完整");
        return SY_ERR_FFMPEG;
    }
    if (caps) {
        if (plan->codec == SY_H265 && !caps->h265) { snprintf(st->err, sizeof st->err, "当前引擎没有 H.265"); return SY_ERR_CODEC; }
        if (plan->codec == SY_H264 && !caps->h264) { snprintf(st->err, sizeof st->err, "当前引擎没有 H.264"); return SY_ERR_CODEC; }
        if (plan->container == SY_MP4 && !caps->mp4) { snprintf(st->err, sizeof st->err, "当前引擎不能写 MP4"); return SY_ERR_CODEC; }
        if (plan->container == SY_MKV && !caps->mkv) { snprintf(st->err, sizeof st->err, "当前引擎不能写 MKV"); return SY_ERR_CODEC; }
    }
    SySession *s = calloc(1, sizeof *s);
    if (!s) return SY_ERR_FFMPEG;
    s->plan = *plan;
    s->plan.w &= ~1;
    s->plan.h &= ~1;
    if (s->plan.w < 16 || s->plan.h < 16) {
        snprintf(st->err, sizeof st->err, "录制区域太小");
        free(s);
        return SY_ERR_REGION;
    }
    if (s->plan.fps != 10 && s->plan.fps != 15 && s->plan.fps != 24 && s->plan.fps != 30) s->plan.fps = 24;
    if (s->plan.quality < 0 || s->plan.quality > 2) s->plan.quality = 1;
    if (mkdir_p(s->plan.outdir) != 0 || access(s->plan.outdir, W_OK) != 0) {
        snprintf(st->err, sizeof st->err, "保存目录不可写：%s", s->plan.outdir);
        free(s);
        return SY_ERR_DIR;
    }
#ifdef SHIYING_WIN
    {
        ULARGE_INTEGER freeb;
        if (GetDiskFreeSpaceExA(s->plan.outdir, &freeb, NULL, NULL)) {
            double freegb = (double)freeb.QuadPart / 1e9;
            if (freegb < 0.3) {
                snprintf(st->err, sizeof st->err, "保存目录所在磁盘空间不足（剩余 %.1f GB）", freegb);
                free(s);
                return SY_ERR_DISK;
            }
        }
    }
#else
    {
        struct statvfs v;
        if (statvfs(s->plan.outdir, &v) == 0) {
            double freegb = (double)v.f_bavail * (double)v.f_frsize / 1e9;
            if (freegb < 0.3) {
                snprintf(st->err, sizeof st->err, "保存目录所在磁盘空间不足（剩余 %.1f GB）", freegb);
                free(s);
                return SY_ERR_DISK;
            }
        }
    }
#endif
    snprintf(s->ffmpeg, sizeof s->ffmpeg, "%s", ffmpeg);
    path_stamp(s->stamp, sizeof s->stamp);
    snprintf(s->tmpdir, sizeof s->tmpdir, "%s/.shiying-%s", s->plan.outdir, s->stamp);
    if (mkdir_p(s->tmpdir) != 0) {
        snprintf(st->err, sizeof st->err, "无法创建临时目录");
        free(s);
        return SY_ERR_DIR;
    }
    int th = s->plan.scale == 1 ? 1080 : s->plan.scale == 2 ? 720 : 0;
    if (th && s->plan.h > th) snprintf(s->vf, sizeof s->vf, "scale=-2:%d:flags=bilinear", th);
    s->ordinal = 1;
    s->ord_first = 0;
    s->seg_next = 0;
    write_target(s);
    char lp[1200];
    snprintf(lp, sizeof lp, "%s/ffmpeg.log", g_cache_dir);
    FILE *lf = fopen(lp, "w");
    if (lf) { fprintf(lf, "# 拾影录屏 %s 录制日志\n", APP_VERSION); fclose(lf); }
    s->audio_on = s->plan.audio != SY_AUDIO_SILENCE;
#ifndef SHIYING_WIN
    if (s->audio_on) {
        const char *dev = s->plan.audio == SY_AUDIO_DEVICE ? s->plan.device_id : NULL;
        if (audio_start((int)s->plan.audio, dev) != 0) {
            snprintf(s->warn, sizeof s->warn, "%s", audio_error()[0] ? audio_error() : "声音设备不可用，本次为无声录制");
            s->audio_on = 0;
        } else s->audio_started = 1;
    }
#endif
    plat_take_begin(s);
    live_sess = s;
    s->phase = SY_PHASE_LIVE;
    if (start_piece(s) != 0) {
        snprintf(st->err, sizeof st->err, "%s", s->err[0] ? s->err : "无法启动录制引擎");
        fail_open(s, st, 0);
        return SY_ERR_FFMPEG;
    }
    {
        char dest[1200];
        dest_of(s, 1, dest, sizeof dest);
        logf_("begin %dx%d+%d+%d fps=%d q=%d audio=%d cursor=%d codec=%d limit=%llu out=%s",
              s->plan.w, s->plan.h, s->plan.x, s->plan.y, s->plan.fps, s->plan.quality,
              (int)s->plan.audio, (int)s->plan.cursor, (int)s->plan.codec,
              (unsigned long long)s->plan.limit_bytes, dest);
    }
    *out = s;
    fill_status(s, st);
    return SY_OK;
}

void sy_session_pause(SySession *s) {
    if (!s || s->hold == SY_HOLD_STOP) return;
    if (s->phase != SY_PHASE_LIVE && s->phase != SY_PHASE_PAUSED) return;
    s->hold = SY_HOLD_PAUSE;
    s->want_resume = 0;
    plat_cursor_off();
}

void sy_session_resume(SySession *s) {
    if (!s || s->hold == SY_HOLD_STOP || s->phase == SY_PHASE_SEALING || s->phase == SY_PHASE_IDLE) return;
    if (s->phase != SY_PHASE_PAUSED && s->hold != SY_HOLD_PAUSE) return;
    s->want_resume = 1;
}

void sy_session_stop(SySession *s) {
    if (!s || s->phase == SY_PHASE_IDLE) return;
    s->hold = SY_HOLD_STOP;
    s->want_resume = 0;
    plat_cursor_off();
}

void sy_session_poll(SySession *s) {
    if (!s || s->phase == SY_PHASE_IDLE) return;
    if (s->slot_pid && s->slot_kind == SLOT_FLUSH && !s->slot_killed && now_s() - s->slot_since > 12) {
        plat_kill(s->slot_pid);
        s->slot_killed = 1;
        logf_("segment pid=%d killed after timeout", s->slot_pid);
    }
    if (s->live_pid && s->seg_next > 0 && seg_size(s, s->seg_next - 1) >= 2048) s->media_ok = 1;
    int live_exit = 0, slot_exit = 0, live_code = 0, slot_code = 0, kind = s->slot_kind;
    if (s->live_pid) {
        int c = 0, r = plat_reap(s->live_pid, &c);
        if (r == 1) { live_exit = 1; live_code = c; s->live_pid = 0; }
    }
    if (s->slot_pid) {
        int c = 0, r = plat_reap(s->slot_pid, &c);
        if (r == 1) { slot_exit = 1; slot_code = c; s->slot_pid = 0; }
    }
    if (live_exit) on_live_exit(s, live_code);
    if (slot_exit) {
        s->slot_kind = SLOT_EMPTY;
        if (kind == SLOT_REMUX) finish_remux(s, slot_code);
        else if (kind == SLOT_FLUSH) {
            if (s->list_pending) launch_pending(s);
            else if (s->hold == SY_HOLD_STOP && !s->live_pid) begin_final_seal(s);
            else if (s->hold == SY_HOLD_PAUSE && !s->live_pid) finish_pause_flush(s);
        }
        if (s->slot_kind == SLOT_EMPTY && s->hold == SY_HOLD_CUT) s->hold = SY_HOLD_NONE;
    }
    if (!s->slot_pid && s->phase != SY_PHASE_IDLE) on_slot_empty(s);
    plat_cursor_tick(s);
}

void sy_session_status(const SySession *s, SyStatus *st) { if (st) fill_status(s, st); }

void sy_session_close(SySession *s) {
    if (!s) return;
    if (s->live_pid) plat_kill(s->live_pid);
    if (s->slot_pid) plat_kill(s->slot_pid);
    plat_cursor_off();
    mix_stop(s);
    if (live_sess == s) live_sess = NULL;
    free(s);
}

static int find_tmp(const char *outdir, char *found, size_t n) {
    DIR *d = opendir(outdir);
    if (!d) return 0;
    struct dirent *e;
    found[0] = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, ".shiying-", 9) != 0) continue;
        snprintf(found, n, "%s/%s", outdir, e->d_name);
        break;
    }
    closedir(d);
    return found[0] ? 1 : 0;
}

int sy_recover(const char *outdir, const char *ffmpeg, SySession **out, SyStatus *st) {
    if (out) *out = NULL;
    if (st) memset(st, 0, sizeof *st);
    if (!outdir || !ffmpeg || !out || !st) return 0;
    if (live_sess) return 0;
    char found[1300];
    if (!find_tmp(outdir, found, sizeof found)) return 0;
    SySession *s = calloc(1, sizeof *s);
    if (!s) return 0;
    snprintf(s->ffmpeg, sizeof s->ffmpeg, "%s", ffmpeg);
    snprintf(s->tmpdir, sizeof s->tmpdir, "%s", found);
    snprintf(s->plan.outdir, sizeof s->plan.outdir, "%s", outdir);
    int codec = SY_H264, container = SY_MP4;
    char dest[1200];
    dest[0] = 0;
    read_target_file(found, dest, sizeof dest, &codec, &container);
    s->plan.codec = (SyCodec)codec;
    s->plan.container = (SyContainer)container;
    if (dest[0]) {
        snprintf(s->stamp, sizeof s->stamp, "recover");
    } else {
        path_stamp(s->stamp, sizeof s->stamp);
        snprintf(dest, sizeof dest, "%s/录屏_恢复_%s.mp4", outdir, s->stamp);
    }
    snprintf(s->list_path, sizeof s->list_path, "%s/seal.list", found);
    int idx[64];
    int nseg = collect_segs(s, idx, 64);
    int has_list = access(s->list_path, F_OK) == 0;
    if (!has_list && nseg == 0) {
        logf_("recover: no valid segment in %s, removed", found);
        rm_tree(found);
        free(s);
        return 0;
    }
    s->recovering = 1;
    s->recovered = 1;
    s->phase = SY_PHASE_SEALING;
    s->seg_next = 0;
    for (int i = 0; i < nseg; i++) if (idx[i] >= s->seg_next) s->seg_next = idx[i] + 1;
    if (!has_list) {
        char rec[1200];
        recover_name(dest, rec, sizeof rec);
        s->list_path[0] = 0;
        if (write_list_idx(s, rec, idx, nseg) != 0) {
            rm_tree(found);
            free(s);
            return 0;
        }
    } else s->list_pending = 1;
    live_sess = s;
    *out = s;
    fill_status(s, st);
    logf_("recovering %s", found);
    return 1;
}
