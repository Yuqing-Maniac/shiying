/* 拾影录屏 —— 银河麒麟 V10 轻量录屏工具
 * 公共声明。界面直接走 X11，字体、声音、多屏等库在运行时按需加载，
 * 编译产物只依赖 libX11 与 glibc，录制引擎为同目录下的静态 ffmpeg。 */
#ifndef SHIYING_COMMON_H
#define SHIYING_COMMON_H
#define _GNU_SOURCE
#include "path.h"
#include "cfg.h"
#include "audio.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* ---------- 绘制 ---------- */
typedef struct { uint32_t *px; int w, h; } Surf;
Surf  *surf_new(int w, int h);
void   surf_free(Surf *s);
void   surf_clear(Surf *s, uint32_t c);
void   fill_rect(Surf *s, double x, double y, double w, double h, uint32_t c);
void   fill_rrect(Surf *s, double x, double y, double w, double h, double r, uint32_t c);
void   stroke_rrect(Surf *s, double x, double y, double w, double h, double r, double lw, uint32_t c);
void   fill_circle(Surf *s, double cx, double cy, double r, uint32_t c);
void   fill_poly(Surf *s, const double *xy, int n, uint32_t c);
void   blit_argb(Surf *s, const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh);
uint32_t rgba(int r, int g, int b, int a);
uint32_t hexc(uint32_t rgb);
uint32_t alpha_mul(uint32_t c, double a);

int    font_init(void);
double text_width(const char *utf8, double size, int bold);
double draw_text(Surf *s, double x, double y_baseline, const char *utf8, double size, int bold, uint32_t c);
double font_ascent(double size);
const char *font_path(void);

/* ---------- 显示器 ---------- */
typedef struct { int x, y, w, h, primary; char name[64]; } Mon;
extern Mon  g_mons[16];
extern int  g_nmons;
extern int  g_root_w, g_root_h;
void mons_refresh(Display *d);

/* ---------- 录制引擎 ---------- */
enum { REC_IDLE, REC_COUNTDOWN, REC_RUNNING, REC_PAUSED, REC_SAVING };
typedef struct {
    int    state;
    int    hold;
    int    recovered;
    int    sealed_count;
    char   err[512];
    char   warn[512];
    char   result[1200];
    double result_size_mb, result_dur;
} Rec;
extern Rec rec;
const char *ffmpeg_path(void);
int    rec_begin(Display *d, int x, int y, int w, int h);
void   rec_pause(void);
void   rec_resume(void);
void   rec_stop(void);
void   rec_poll(void);
double rec_elapsed(void);
int    rec_recover(void);

#endif
