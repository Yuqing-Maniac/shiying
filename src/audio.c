/* 声音采集：运行时加载 libpulse-simple，系统声音取默认输出的 monitor，麦克风取默认输入。
 * 两路在本进程按墙钟节拍混音，写入当前段 ffmpeg 的标准输入（s16le 48k 双声道）。
 * 以墙钟驱动输出样本数，保证与 x11grab 的视频时间轴一致。 */
#include "common.h"
#include "session.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <poll.h>

#define RATE 48000
#define CH 2
#define CHUNK 960                 /* 20 ms */
#define RING (RATE * 3)           /* 3 s 容量（帧） */
#define PREFILL 1920              /* 40 ms 抖动缓冲 */

static struct {
    pa_simple *(*new_)(const char *, const char *, pa_stream_direction_t, const char *, const char *,
                       const pa_sample_spec *, const pa_channel_map *, const pa_buffer_attr *, int *);
    int (*read)(pa_simple *, void *, size_t, int *);
    void (*free_)(pa_simple *);
    const char *(*strerror)(int);
} pa;
static int pa_loaded = -1;
static char aerr[256];

typedef struct {
    int on;
    pa_simple *s;
    pthread_t th;
    pthread_mutex_t mu;
    int16_t *buf;          /* 环形缓冲，交错双声道 */
    long head, count;      /* 读位置、可用帧数 */
    int primed;
    long target;           /* 抖动缓冲目标（帧），出现欠载时自适应加大 */
    volatile double level;
} Src;
static Src srcs[2];
static pthread_t mix_th;
static volatile int running;

/* 输出端：当前段（act）与正在收尾的旧段（drain）。
 * 混音结果按全局样本序号排成一条时间轴：样本 n 对应单调时钟 g_origin + n/48000。
 * 每个输出端从自己段起点对应的序号开始写，旧段一直写到 ffmpeg 退出，保证每段声音与画面同时结束。 */
typedef struct { int fd; long pos; double opened; } Out;   /* pos 为字节位置（管道可能只写入半帧） */
static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static Out o_act = { -1, 0, 0 }, o_drain = { -1, 0, 0 };
static volatile int act_gen;            /* 当前段变更计数，用于中断阻塞写 */
static double g_origin;
#define HIST RATE                       /* 混音历史 1 s */
static int16_t hist[HIST * CH];
static volatile long g_written;         /* 已产生的样本总数 */

const char *audio_error(void) { return aerr; }

int audio_available(void) {
    if (pa_loaded >= 0) return pa_loaded;
    void *h = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *h2 = dlopen("libpulse.so.0", RTLD_NOW | RTLD_GLOBAL);
    pa_loaded = 0;
    if (h) {
        pa.new_ = dlsym(h, "pa_simple_new");
        pa.read = dlsym(h, "pa_simple_read");
        pa.free_ = dlsym(h, "pa_simple_free");
        if (h2) pa.strerror = dlsym(h2, "pa_strerror");
        if (pa.new_ && pa.read && pa.free_) pa_loaded = 1;
    }
    if (!pa_loaded) snprintf(aerr, sizeof aerr, "未找到 PulseAudio 声音服务库，只能录制无声视频");
    return pa_loaded;
}

double audio_level(int i) { return (i >= 0 && i < 2 && srcs[i].on) ? srcs[i].level : -1; }

static void ring_push(Src *s, const int16_t *d, int frames) {
    pthread_mutex_lock(&s->mu);
    for (int i = 0; i < frames; i++) {
        long pos;
        if (s->count == RING) { s->head = (s->head + 1) % RING; s->count--; }   /* 满则丢最旧 */
        pos = (s->head + s->count) % RING;
        s->buf[pos * 2] = d[i * 2];
        s->buf[pos * 2 + 1] = d[i * 2 + 1];
        s->count++;
    }
    pthread_mutex_unlock(&s->mu);
}

static void *reader(void *arg) {
    Src *s = arg;
    int16_t tmp[CHUNK * CH];
    int err;
    while (running) {
        if (pa.read(s->s, tmp, sizeof tmp, &err) < 0) {
            usleep(20000);
            continue;
        }
        int peak = 0;
        for (int i = 0; i < CHUNK * CH; i++) { int v = abs(tmp[i]); if (v > peak) peak = v; }
        double lv = peak / 32768.0;
        s->level = s->level * 0.6 + lv * 0.4;
        ring_push(s, tmp, CHUNK);
    }
    return NULL;
}

/* 把 [o->pos, g_written) 区间的样本写给输出端。block=1 时管道满则等待（当前段），否则只尽力写（旧段）。
 * 返回 -1 表示该端已失效。 */
static int flush_out(Out *o, int block, int gen) {
    const long FB = CH * 2;                       /* 每帧字节数 */
    while (o->fd >= 0) {
        long endb = g_written * FB;
        if (o->pos < endb - (long)(HIST - 1) * FB) o->pos = endb - (long)(HIST - 1) * FB;   /* 落后超过历史长度：跳过 */
        if (o->pos >= endb) return 0;
        long off = o->pos % ((long)HIST * FB);
        long n = endb - o->pos;
        if (n > (long)HIST * FB - off) n = (long)HIST * FB - off;
        ssize_t w = write(o->fd, (char *)hist + off, (size_t)n);
        if (w > 0) { o->pos += w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            if (!block) return 0;
            struct pollfd pf = { o->fd, POLLOUT, 0 };
            poll(&pf, 1, 20);
            if (!running || act_gen != gen) return 0;
            if (pf.revents & (POLLERR | POLLHUP)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void *mixer(void *arg) {
    (void)arg;
    static int32_t acc[HIST * CH];
    while (running) {
        usleep(10000);
        long due = (long)((now_s() - g_origin) * RATE) - g_written;
        if (due > HIST / 2) due = HIST / 2;
        if (due > 0) {
            memset(acc, 0, sizeof(int32_t) * due * CH);
            for (int i = 0; i < 2; i++) {
                Src *s = &srcs[i];
                if (!s->on) continue;
                pthread_mutex_lock(&s->mu);
                if (!s->primed && s->count >= s->target) s->primed = 1;
                if (s->primed) {
                    if (s->count > due + s->target + RATE / 5) {   /* 积压过多，丢旧数据控制延迟 */
                        long drop = s->count - due - s->target;
                        s->head = (s->head + drop) % RING; s->count -= drop;
                    }
                    long take = s->count < due ? s->count : due;
                    for (long k = 0; k < take; k++) {
                        long pos = (s->head + k) % RING;
                        acc[k * 2] += s->buf[pos * 2];
                        acc[k * 2 + 1] += s->buf[pos * 2 + 1];
                    }
                    s->head = (s->head + take) % RING; s->count -= take;
                    if (take < due) {                  /* 欠载：重新积累缓冲，并加大缓冲目标（上限 300 ms） */
                        s->primed = 0;
                        if (s->target < RATE * 3 / 10) s->target = s->target * 3 / 2;
                    }
                }
                pthread_mutex_unlock(&s->mu);
            }
            for (long k = 0; k < due; k++) {
                long h = ((g_written + k) % HIST) * CH;
                for (int c = 0; c < CH; c++) {
                    int32_t v = acc[k * CH + c];
                    hist[h + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            __sync_synchronize();
            g_written += due;
        }
        /* 旧段：尽力写，ffmpeg 退出（EPIPE）或超时后关闭 */
        pthread_mutex_lock(&out_mu);
        Out d = o_drain;
        pthread_mutex_unlock(&out_mu);
        if (d.fd >= 0) {
            int r = flush_out(&d, 0, 0);
            pthread_mutex_lock(&out_mu);
            if (r < 0 || now_s() - d.opened > 6) { close(d.fd); o_drain.fd = -1; }
            else o_drain.pos = d.pos;
            pthread_mutex_unlock(&out_mu);
        }
        /* 当前段 */
        pthread_mutex_lock(&out_mu);
        Out a = o_act;
        int gen = act_gen;
        pthread_mutex_unlock(&out_mu);
        if (a.fd >= 0) {
            int r = flush_out(&a, 1, gen);
            pthread_mutex_lock(&out_mu);
            if (act_gen == gen) {
                if (r < 0) { close(a.fd); o_act.fd = -1; }
                else o_act.pos = a.pos;
            }
            pthread_mutex_unlock(&out_mu);
        }
    }
    return NULL;
}

static int open_src(Src *s, const char *dev, const char *name) {
    pa_sample_spec ss = { PA_SAMPLE_S16LE, RATE, CH };
    pa_buffer_attr ba = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, CHUNK * CH * 2 };
    int err = 0;
    s->s = pa.new_(NULL, APP_NAME, PA_STREAM_RECORD, dev, name, &ss, NULL, &ba, &err);
    if (!s->s) {
        snprintf(aerr, sizeof aerr, "打开%s失败：%s", name, pa.strerror ? pa.strerror(err) : "未知错误");
        return -1;
    }
    s->buf = calloc(RING * CH, sizeof(int16_t));
    s->head = s->count = 0; s->primed = 0; s->level = 0; s->target = PREFILL;
    pthread_mutex_init(&s->mu, NULL);
    s->on = 1;
    return 0;
}

int audio_start(int mode, const char *device) {
    aerr[0] = 0;
    if (mode == SY_AUDIO_SILENCE) return 0;
    if (running) return -1;
    if (!audio_available()) return -1;
    memset(srcs, 0, sizeof srcs);
    int ok = 0;
    if (mode == SY_AUDIO_DEVICE) {
        if (!device || !device[0]) return -1;
        ok = open_src(&srcs[0], device, "指定设备") == 0;
    } else {
        if (mode == SY_AUDIO_SYSTEM || mode == SY_AUDIO_BOTH) ok += open_src(&srcs[0], "@DEFAULT_MONITOR@", "系统声音") == 0;
        if (mode == SY_AUDIO_MIC || mode == SY_AUDIO_BOTH) ok += open_src(&srcs[1], NULL, "麦克风") == 0;
    }
    if (!ok) return -1;
    g_origin = now_s();
    g_written = 0;
    o_act.fd = o_drain.fd = -1;
    running = 1;
    for (int i = 0; i < 2; i++) if (srcs[i].on) pthread_create(&srcs[i].th, NULL, reader, &srcs[i]);
    pthread_create(&mix_th, NULL, mixer, NULL);
    return 0;
}

/* 新段开始：fd 为声音管道写端，origin 为本段时间原点（单调时钟） */
void audio_set_fd(int fd, double origin) {
    pthread_mutex_lock(&out_mu);
    if (o_act.fd >= 0) {                 /* 理论上已先调用 audio_end_fd */
        if (o_drain.fd >= 0) close(o_drain.fd);
        o_drain = o_act;
        o_drain.opened = now_s();
    }
    o_act.fd = fd;
    o_act.pos = (long)((origin - g_origin) * RATE + 0.5) * CH * 2;
    if (o_act.pos < 0) o_act.pos = 0;
    o_act.opened = now_s();
    act_gen++;
    pthread_mutex_unlock(&out_mu);
}

/* 当前段结束：转为旧段继续供声，直到 ffmpeg 退出 */
void audio_end_fd(void) {
    pthread_mutex_lock(&out_mu);
    if (o_act.fd >= 0) {
        if (o_drain.fd >= 0) close(o_drain.fd);
        o_drain = o_act;
        o_drain.opened = now_s();
        o_act.fd = -1;
    }
    act_gen++;
    pthread_mutex_unlock(&out_mu);
}

void audio_stop(void) {
    if (!running) return;
    running = 0;
    pthread_join(mix_th, NULL);
    if (o_act.fd >= 0) close(o_act.fd);
    if (o_drain.fd >= 0) close(o_drain.fd);
    o_act.fd = o_drain.fd = -1;
    for (int i = 0; i < 2; i++) if (srcs[i].on) {
        pthread_join(srcs[i].th, NULL);
        pa.free_(srcs[i].s);
        free(srcs[i].buf);
        srcs[i].on = 0;
    }
}
