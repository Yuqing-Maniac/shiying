#ifndef SHIYING_PATH_H
#define SHIYING_PATH_H

#include <stdio.h>

#define APP_NAME    "拾影录屏"
#define APP_ID      "shiying"
#define APP_VERSION "1.1.0"

extern char g_exe_dir[1024];
extern char g_cache_dir[1024];
extern char g_conf_dir[1024];

double now_s(void);
double wall_s(void);
void   path_stamp(char *out, size_t n);
void   logf_(const char *fmt, ...);
int    mkdir_p(const char *path);
const char *home_dir(void);
void   file_fsync(FILE *f);
int    path_fsync(const char *path);

#endif
