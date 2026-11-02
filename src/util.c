#include "common.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <X11/extensions/Xrandr.h>

Mon  g_mons[16];
int  g_nmons;
int  g_root_w, g_root_h;

void mons_refresh(Display *d) {
    int scr = DefaultScreen(d);
    g_root_w = DisplayWidth(d, scr);
    g_root_h = DisplayHeight(d, scr);
    g_nmons = 0;
    static void *h;
    static int tried;
    if (!tried) {
        tried = 1;
        h = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    }
    if (h) {
        XRRMonitorInfo *(*getm)(Display *, Window, Bool, int *) = dlsym(h, "XRRGetMonitors");
        void (*freem)(XRRMonitorInfo *) = dlsym(h, "XRRFreeMonitors");
        if (getm && freem) {
            int n = 0;
            XRRMonitorInfo *m = getm(d, RootWindow(d, scr), True, &n);
            for (int i = 0; m && i < n && g_nmons < 16; i++) {
                Mon *o = &g_mons[g_nmons++];
                o->x = m[i].x; o->y = m[i].y; o->w = m[i].width; o->h = m[i].height; o->primary = m[i].primary;
                char *nm = m[i].name ? XGetAtomName(d, m[i].name) : NULL;
                snprintf(o->name, sizeof o->name, "%s", nm ? nm : "");
                if (nm) XFree(nm);
            }
            if (m) freem(m);
        }
    }
    if (g_nmons == 0) {
        g_mons[0] = (Mon){ 0, 0, g_root_w, g_root_h, 1, "" };
        g_nmons = 1;
    }
    for (int i = 1; i < g_nmons; i++)
        if (g_mons[i].primary) { Mon t = g_mons[0]; g_mons[0] = g_mons[i]; g_mons[i] = t; }
    if (cfg.monitor >= g_nmons) cfg.monitor = -1;
}
