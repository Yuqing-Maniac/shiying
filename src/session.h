#ifndef SHIYING_SESSION_H
#define SHIYING_SESSION_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SY_OK = 0,
    SY_ERR_FFMPEG = 1,
    SY_ERR_DIR = 2,
    SY_ERR_REGION = 3,
    SY_ERR_DISK = 4,
    SY_ERR_CODEC = 5,
    SY_ERR_BUSY = 6
} SyErr;

typedef enum {
    SY_AUDIO_SILENCE = 0,
    SY_AUDIO_SYSTEM = 1,
    SY_AUDIO_MIC = 2,
    SY_AUDIO_BOTH = 3,
    SY_AUDIO_DEVICE = 4
} SyAudio;

typedef enum {
    SY_CURSOR_OFF = 0,
    SY_CURSOR_ORIG = 1,
    SY_CURSOR_MAG = 2
} SyCursor;

typedef enum {
    SY_H264 = 0,
    SY_H265 = 1
} SyCodec;

typedef enum {
    SY_MP4 = 0,
    SY_MKV = 1
} SyContainer;

enum { SY_UNIT_MB = 0, SY_UNIT_GB = 1 };

typedef enum {
    SY_PHASE_IDLE = 0,
    SY_PHASE_LIVE = 1,
    SY_PHASE_PAUSED = 2,
    SY_PHASE_SEALING = 3
} SyPhase;

typedef enum {
    SY_HOLD_NONE = 0,
    SY_HOLD_PAUSE = 1,
    SY_HOLD_STOP = 2,
    SY_HOLD_CUT = 3
} SyHold;

typedef struct SySession SySession;

typedef struct {
    int h264;
    int h265;
    int mp4;
    int mkv;
    int grab_x11;
    int grab_gdi;
    int grab_dda;
    int cursor_mag;
    int audio_ok;
} SyCaps;

typedef struct {
    int id;
    const char *label;
} SyChoice;

typedef struct {
    char id[512];
    char label[256];
} SyDevice;

typedef struct {
    int x, y, w, h;
    int fps;
    int quality;
    int scale;
    SyCodec codec;
    SyContainer container;
    SyCursor cursor;
    SyAudio audio;
    char device_id[512];
    char display[256];
    char outdir[1024];
    uint64_t limit_bytes;
} SyPlan;

typedef struct {
    SyPhase phase;
    SyHold hold;
    double elapsed;
    int sealed_count;
    int recovered;
    char last_file[1200];
    uint64_t last_bytes;
    double last_dur;
    char err[512];
    char warn[512];
} SyStatus;

uint64_t sy_limit_bytes(int n, int unit);
int sy_ffmpeg_resolve(char *out, size_t n);
int sy_probe(const char *ffmpeg, SyCaps *out);
int sy_devices(SyDevice *out, int cap);
int sy_codec_choices(const SyCaps *caps, SyChoice *out, int cap);
int sy_container_choices(const SyCaps *caps, SyChoice *out, int cap);
int sy_cursor_choices(const SyCaps *caps, SyChoice *out, int cap);
int sy_audio_choices(SyChoice *out, int cap);
void sy_clamp_recorded(SyCodec *codec, SyContainer *container, SyCursor *cursor, const SyCaps *caps);

int sy_session_open(SySession **out, const SyPlan *plan, const char *ffmpeg,
                    const SyCaps *caps, SyStatus *st);
void sy_session_pause(SySession *s);
void sy_session_resume(SySession *s);
void sy_session_stop(SySession *s);
void sy_session_poll(SySession *s);
void sy_session_status(const SySession *s, SyStatus *st);
void sy_session_close(SySession *s);

int sy_recover(const char *outdir, const char *ffmpeg, SySession **out, SyStatus *st);

#endif
