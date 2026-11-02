#ifndef SHIYING_CFG_H
#define SHIYING_CFG_H

#include "session.h"

enum { AREA_SCREEN = 0, AREA_REGION = 1 };

typedef struct {
    int area;
    int monitor;
    int rx, ry, rw, rh;
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
