#ifndef SHIYING_CFG_H
#define SHIYING_CFG_H

#include "session.h"
#include "toolbar.h"

enum { AREA_SCREEN = 0, AREA_REGION = 1, AREA_WINDOW = 2 };

typedef struct {
    int area;
    int mode;
    int monitor;
    int rx, ry, rw, rh;
    int remember_selection;
    int desktop_copy;
    int show_thumbnail;
    int click_highlight;
    int theme;
    int audio;
    int fps;
    int quality;
    int scale;
    int fmt;
    int countdown;
    int codec;
    int cursor;
    int segment_n;
    int segment_unit;
    char audio_dev[512];
    char outdir[1024];
} Config;

extern Config cfg;
void cfg_load(void);
void cfg_save(void);

#endif
