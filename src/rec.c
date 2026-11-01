#include "common.h"
#include "session.h"

Rec rec;
static SySession *sess;

const char *ffmpeg_path(void) {
    static char buf[1200];
    if (sy_ffmpeg_resolve(buf, sizeof buf) != 0) return NULL;
    return buf;
}

static void apply_status(const SyStatus *st) {
    rec.hold = (int)st->hold;
    rec.recovered = st->recovered;
    rec.sealed_count = st->sealed_count;
    snprintf(rec.err, sizeof rec.err, "%s", st->err);
    snprintf(rec.warn, sizeof rec.warn, "%s", st->warn);
    if (st->phase == SY_PHASE_IDLE) {
        if (st->last_file[0]) {
            snprintf(rec.result, sizeof rec.result, "%s", st->last_file);
            rec.result_size_mb = st->last_bytes / 1048576.0;
            rec.result_dur = st->last_dur;
        }
        rec.state = REC_IDLE;
        return;
    }
    if (st->phase == SY_PHASE_SEALING || st->hold == SY_HOLD_STOP) rec.state = REC_SAVING;
    else if (st->phase == SY_PHASE_PAUSED || st->hold == SY_HOLD_PAUSE) rec.state = REC_PAUSED;
    else rec.state = REC_RUNNING;
}

static void pull(void) {
    if (!sess) return;
    SyStatus st;
    sy_session_status(sess, &st);
    apply_status(&st);
}

int rec_begin(Display *d, int x, int y, int w, int h) {
    memset(&rec, 0, sizeof rec);
    if (sess) { snprintf(rec.err, sizeof rec.err, "已有录制正在进行"); return -1; }
    char ff[1200];
    if (sy_ffmpeg_resolve(ff, sizeof ff) != 0) {
        snprintf(rec.err, sizeof rec.err, "找不到录制引擎（录制引擎/ffmpeg），请保持程序目录完整");
        return -1;
    }
    SyCaps caps;
    memset(&caps, 0, sizeof caps);
    sy_probe(ff, &caps);
    int n = cfg.segment_n;
    int u = cfg.segment_unit;
    if (n < 0) n = 0;
    if (u == SY_UNIT_MB && n > 0 && n < 5) n = 5;
    SyPlan plan;
    memset(&plan, 0, sizeof plan);
    plan.x = x; plan.y = y; plan.w = w; plan.h = h;
    plan.fps = cfg.fps; plan.quality = cfg.quality; plan.scale = cfg.scale;
    plan.codec = (SyCodec)cfg.codec;
    plan.container = (SyContainer)cfg.fmt;
    plan.cursor = (SyCursor)cfg.cursor;
    plan.audio = (SyAudio)cfg.audio;
    plan.limit_bytes = sy_limit_bytes(n, u);
    if (cfg.audio == SY_AUDIO_DEVICE)
        snprintf(plan.device_id, sizeof plan.device_id, "%s", cfg.audio_dev);
    snprintf(plan.display, sizeof plan.display, "%s", DisplayString(d));
    snprintf(plan.outdir, sizeof plan.outdir, "%s", cfg.outdir);
    sy_clamp_recorded(&plan.codec, &plan.container, &plan.cursor, &caps);
    SyStatus st;
    int rc = sy_session_open(&sess, &plan, ff, &caps, &st);
    apply_status(&st);
    if (rc != 0) {
        sess = NULL;
        rec.state = REC_IDLE;
        if (!rec.err[0]) snprintf(rec.err, sizeof rec.err, "无法开始录制");
        return -1;
    }
    return 0;
}

void rec_pause(void) {
    if (!sess) return;
    sy_session_pause(sess);
    pull();
}

void rec_resume(void) {
    if (!sess) return;
    sy_session_resume(sess);
    pull();
}

void rec_stop(void) {
    if (!sess) return;
    sy_session_stop(sess);
    pull();
}

void rec_poll(void) {
    if (!sess) return;
    sy_session_poll(sess);
    SyStatus st;
    sy_session_status(sess, &st);
    apply_status(&st);
    if (st.phase == SY_PHASE_IDLE) {
        sy_session_close(sess);
        sess = NULL;
    }
}

double rec_elapsed(void) {
    if (!sess) return rec.result_dur;
    SyStatus st;
    sy_session_status(sess, &st);
    return st.elapsed;
}

int rec_recover(void) {
    if (sess) return 0;
    char ff[1200];
    if (sy_ffmpeg_resolve(ff, sizeof ff) != 0) return 0;
    SyStatus st;
    if (!sy_recover(cfg.outdir, ff, &sess, &st)) return 0;
    apply_status(&st);
    rec.recovered = 1;
    rec.state = REC_SAVING;
    return 1;
}
