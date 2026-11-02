#include "cfg.h"
#include "path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Config cfg;

static void default_outdir(char *out) {
    const char *home = home_dir();
#ifdef _WIN32
    snprintf(out, 1024, "%s/Videos/录屏", home);
    struct stat st;
    if (stat(out, &st) != 0) {
        char vid[1024];
        snprintf(vid, sizeof vid, "%s/Videos", home);
        if (stat(vid, &st) != 0) snprintf(out, 1024, "%s/录屏", home);
    }
#else
    char p[1100], line[1200];
    snprintf(p, sizeof p, "%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(p, "r");
    char vid[1024] = "";
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "XDG_VIDEOS_DIR=", 15) == 0) {
                char *s = strchr(line, '"');
                char *e = s ? strrchr(s + 1, '"') : NULL;
                if (s && e) {
                    *e = 0;
                    s++;
                    if (strncmp(s, "$HOME", 5) == 0) snprintf(vid, sizeof vid, "%s%s", home, s + 5);
                    else snprintf(vid, sizeof vid, "%s", s);
                }
            }
        }
        fclose(f);
    }
    struct stat st;
    if (!vid[0] || stat(vid, &st) != 0 || strcmp(vid, home) == 0) {
        snprintf(vid, sizeof vid, "%s/视频", home);
        if (stat(vid, &st) != 0) snprintf(vid, sizeof vid, "%s/Videos", home);
        if (stat(vid, &st) != 0) snprintf(vid, sizeof vid, "%s/视频", home);
    }
    snprintf(out, 1024, "%s/录屏", vid);
#endif
}

static void cfg_defaults(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.area = AREA_SCREEN;
    cfg.monitor = -1;
    cfg.audio = SY_AUDIO_SYSTEM;
    cfg.fps = 24;
    cfg.quality = 1;
    cfg.scale = 0;
    cfg.fmt = SY_MP4;
    cfg.countdown = 3;
    cfg.codec = SY_H264;
    cfg.cursor = SY_CURSOR_ORIG;
    cfg.segment_n = 2;
    cfg.segment_unit = SY_UNIT_GB;
    default_outdir(cfg.outdir);
}

enum { CF_INT = 0, CF_STR = 1 };

static const struct {
    const char *key;
    int kind;
    size_t off;
    size_t cap;
} keys[] = {
    { "area", CF_INT, offsetof(Config, area), 0 },
    { "monitor", CF_INT, offsetof(Config, monitor), 0 },
    { "rx", CF_INT, offsetof(Config, rx), 0 },
    { "ry", CF_INT, offsetof(Config, ry), 0 },
    { "rw", CF_INT, offsetof(Config, rw), 0 },
    { "rh", CF_INT, offsetof(Config, rh), 0 },
    { "audio", CF_INT, offsetof(Config, audio), 0 },
    { "fps", CF_INT, offsetof(Config, fps), 0 },
    { "quality", CF_INT, offsetof(Config, quality), 0 },
    { "scale", CF_INT, offsetof(Config, scale), 0 },
    { "format", CF_INT, offsetof(Config, fmt), 0 },
    { "countdown", CF_INT, offsetof(Config, countdown), 0 },
    { "codec", CF_INT, offsetof(Config, codec), 0 },
    { "cursor", CF_INT, offsetof(Config, cursor), 0 },
    { "segment_n", CF_INT, offsetof(Config, segment_n), 0 },
    { "segment_unit", CF_INT, offsetof(Config, segment_unit), 0 },
    { "audio_dev", CF_STR, offsetof(Config, audio_dev), 512 },
    { "outdir", CF_STR, offsetof(Config, outdir), 1024 },
};

static void clamp_loaded(void) {
    if (cfg.fps != 10 && cfg.fps != 15 && cfg.fps != 24 && cfg.fps != 30) cfg.fps = 24;
    if (cfg.quality < 0 || cfg.quality > 2) cfg.quality = 1;
    if (cfg.scale < 0 || cfg.scale > 2) cfg.scale = 0;
    if (cfg.audio < SY_AUDIO_SILENCE || cfg.audio > SY_AUDIO_DEVICE) cfg.audio = SY_AUDIO_SYSTEM;
    if (cfg.audio == SY_AUDIO_DEVICE && !cfg.audio_dev[0]) cfg.audio = SY_AUDIO_SYSTEM;
    if (cfg.fmt != SY_MP4 && cfg.fmt != SY_MKV) cfg.fmt = SY_MP4;
    if (cfg.countdown != 0 && cfg.countdown != 3 && cfg.countdown != 5) cfg.countdown = 3;
    if (cfg.area != AREA_REGION) cfg.area = AREA_SCREEN;
    if (cfg.codec != SY_H264 && cfg.codec != SY_H265) cfg.codec = SY_H264;
    if (cfg.cursor < SY_CURSOR_OFF || cfg.cursor > SY_CURSOR_MAG) cfg.cursor = SY_CURSOR_ORIG;
    if (cfg.segment_unit != SY_UNIT_MB && cfg.segment_unit != SY_UNIT_GB) cfg.segment_unit = SY_UNIT_GB;
    if (cfg.segment_n < 0) cfg.segment_n = 0;
    if (cfg.segment_n > 9999) cfg.segment_n = 9999;
    if (cfg.segment_unit == SY_UNIT_MB && cfg.segment_n > 0 && cfg.segment_n < 5) cfg.segment_n = 5;
}

void cfg_load(void) {
    cfg_defaults();
    char p[1100], line[1200];
    snprintf(p, sizeof p, "%s/config.ini", g_conf_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    int saw_cursor = 0, saw_mouse = 0, mouse_val = 1;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(k, "mouse")) {
            saw_mouse = 1;
            mouse_val = atoi(v);
            continue;
        }
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            if (strcmp(k, keys[i].key)) continue;
            char *slot = (char *)&cfg + keys[i].off;
            if (keys[i].kind == CF_STR) snprintf(slot, keys[i].cap, "%s", v);
            else *(int *)slot = atoi(v);
            if (!strcmp(k, "cursor")) saw_cursor = 1;
            break;
        }
    }
    fclose(f);
    if (!saw_cursor && saw_mouse) cfg.cursor = mouse_val ? SY_CURSOR_ORIG : SY_CURSOR_OFF;
    clamp_loaded();
}

void cfg_save(void) {
    mkdir_p(g_conf_dir);
    char p[1100], tmp[1110];
    snprintf(p, sizeof p, "%s/config.ini", g_conf_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# 拾影录屏 配置文件\n");
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char *slot = (char *)&cfg + keys[i].off;
        if (keys[i].kind == CF_STR) fprintf(f, "%s=%s\n", keys[i].key, slot);
        else fprintf(f, "%s=%d\n", keys[i].key, *(int *)slot);
    }
    file_fsync(f);
    fclose(f);
    rename(tmp, p);
}
