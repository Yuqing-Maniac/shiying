/* 软件绘制：抗锯齿圆角矩形、圆、多边形、FreeType 文字。
 * FreeType 与 fontconfig 运行时 dlopen，缺失时程序给出提示而不是无法启动。 */
#include "common.h"
#include <math.h>
#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

/* ---------------- 表面与颜色 ---------------- */
Surf *surf_new(int w, int h) {
    Surf *s = calloc(1, sizeof(Surf));
    s->w = w; s->h = h;
    s->px = calloc((size_t)w * h, 4);
    return s;
}
void surf_free(Surf *s) { if (s) { free(s->px); free(s); } }
void surf_clear(Surf *s, uint32_t c) { for (int i = 0; i < s->w * s->h; i++) s->px[i] = c; }

uint32_t rgba(int r, int g, int b, int a) {
    return ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
}
uint32_t hexc(uint32_t rgb) { return 0xff000000u | rgb; }
uint32_t alpha_mul(uint32_t c, double a) {
    if (a >= 1) return c;
    if (a <= 0) return 0;
    uint32_t A = (uint32_t)(((c >> 24) & 255) * a + 0.5), R = (uint32_t)(((c >> 16) & 255) * a + 0.5);
    uint32_t G = (uint32_t)(((c >> 8) & 255) * a + 0.5), B = (uint32_t)((c & 255) * a + 0.5);
    return (A << 24) | (R << 16) | (G << 8) | B;
}

static inline void blend(uint32_t *d, uint32_t c, double cov) {
    if (cov <= 0.002) return;
    if (cov > 1) cov = 1;
    uint32_t sa = (c >> 24) & 255;
    if (sa == 255 && cov >= 0.998) { *d = c; return; }
    double a = sa / 255.0 * cov, ia = 1 - a;
    uint32_t dv = *d;
    double r = ((c >> 16) & 255) * cov + ((dv >> 16) & 255) * ia;
    double g = ((c >> 8) & 255) * cov + ((dv >> 8) & 255) * ia;
    double b = (c & 255) * cov + (dv & 255) * ia;
    double A = sa * cov + ((dv >> 24) & 255) * ia;
    *d = ((uint32_t)(A + .5) << 24) | ((uint32_t)(r + .5) << 16) | ((uint32_t)(g + .5) << 8) | (uint32_t)(b + .5);
}

static inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* 像素中心落在圆角矩形内的覆盖率 */
static double rrect_cov(double px, double py, double x0, double y0, double x1, double y1, double r) {
    if (r < 0) r = 0;
    double cx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
    double cy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
    double dx = px - cx, dy = py - cy;
    if ((dx != 0 && dy != 0) && r > 0) {
        double d = sqrt(dx * dx + dy * dy);
        return clamp01(r - d + 0.5);
    }
    double cvx = clamp01((px - x0) + 0.5) < clamp01((x1 - px) + 0.5) ? clamp01((px - x0) + 0.5) : clamp01((x1 - px) + 0.5);
    double cvy = clamp01((py - y0) + 0.5) < clamp01((y1 - py) + 0.5) ? clamp01((py - y0) + 0.5) : clamp01((y1 - py) + 0.5);
    if (r > 0 && (dx != 0 || dy != 0)) {
        double d = sqrt(dx * dx + dy * dy);
        double cr = clamp01(r - d + 0.5);
        double m = cvx < cvy ? cvx : cvy;
        return cr < m ? cr : m;
    }
    return cvx < cvy ? cvx : cvy;
}

void fill_rect(Surf *s, double x, double y, double w, double h, uint32_t c) {
    fill_rrect(s, x, y, w, h, 0, c);
}

void fill_rrect(Surf *s, double x, double y, double w, double h, double r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int ix0 = (int)floor(x), iy0 = (int)floor(y), ix1 = (int)ceil(x + w), iy1 = (int)ceil(y + h);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > s->w) ix1 = s->w;
    if (iy1 > s->h) iy1 = s->h;
    for (int py = iy0; py < iy1; py++) {
        uint32_t *row = s->px + (size_t)py * s->w;
        double pcy = py + 0.5;
        for (int px = ix0; px < ix1; px++) {
            double pcx = px + 0.5;
            /* 内部快速路径 */
            if (pcx > x + r + 1 && pcx < x + w - r - 1 && pcy > y + 1 && pcy < y + h - 1) { blend(&row[px], c, 1); continue; }
            if (pcy > y + r + 1 && pcy < y + h - r - 1 && pcx > x + 1 && pcx < x + w - 1) { blend(&row[px], c, 1); continue; }
            blend(&row[px], c, rrect_cov(pcx, pcy, x, y, x + w, y + h, r));
        }
    }
}

void stroke_rrect(Surf *s, double x, double y, double w, double h, double r, double lw, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int ix0 = (int)floor(x) - 1, iy0 = (int)floor(y) - 1, ix1 = (int)ceil(x + w) + 1, iy1 = (int)ceil(y + h) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > s->w) ix1 = s->w;
    if (iy1 > s->h) iy1 = s->h;
    double ir = r - lw; if (ir < 0) ir = 0;
    for (int py = iy0; py < iy1; py++) {
        uint32_t *row = s->px + (size_t)py * s->w;
        for (int px = ix0; px < ix1; px++) {
            double pcx = px + 0.5, pcy = py + 0.5;
            if (pcx > x + lw + r + 1 && pcx < x + w - lw - r - 1 && pcy > y + lw + 1 && pcy < y + h - lw - 1) continue;
            double o = rrect_cov(pcx, pcy, x, y, x + w, y + h, r);
            double i = rrect_cov(pcx, pcy, x + lw, y + lw, x + w - lw, y + h - lw, ir);
            blend(&row[px], c, o - i);
        }
    }
}

void fill_circle(Surf *s, double cx, double cy, double r, uint32_t c) {
    fill_rrect(s, cx - r, cy - r, 2 * r, 2 * r, r, c);
}

/* 4×4 超采样的偶奇规则多边形（用于小图标） */
void fill_poly(Surf *s, const double *xy, int n, uint32_t c) {
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (int i = 0; i < n; i++) {
        if (xy[2 * i] < minx) minx = xy[2 * i];
        if (xy[2 * i] > maxx) maxx = xy[2 * i];
        if (xy[2 * i + 1] < miny) miny = xy[2 * i + 1];
        if (xy[2 * i + 1] > maxy) maxy = xy[2 * i + 1];
    }
    int x0 = (int)floor(minx), y0 = (int)floor(miny), x1 = (int)ceil(maxx), y1 = (int)ceil(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double tx = px + (sx + 0.5) / 4, ty = py + (sy + 0.5) / 4;
                    int in = 0;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        double xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
                        if (((yi > ty) != (yj > ty)) && (tx < (xj - xi) * (ty - yi) / (yj - yi) + xi)) in = !in;
                    }
                    hit += in;
                }
            if (hit) blend(&s->px[(size_t)py * s->w + px], c, hit / 16.0);
        }
}

/* 双线性缩放贴图（源为预乘 ARGB） */
void blit_argb(Surf *s, const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= s->h) continue;
        double fy = (y + 0.5) * sh / (double)dh - 0.5;
        int y0 = (int)floor(fy); double wy = fy - y0;
        int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 >= sh ? sh - 1 : y0 + 1;
        if (ya >= sh) ya = sh - 1;
        for (int x = 0; x < dw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= s->w) continue;
            double fx = (x + 0.5) * sw / (double)dw - 0.5;
            int x0 = (int)floor(fx); double wx = fx - x0;
            int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 >= sw ? sw - 1 : x0 + 1;
            if (xa >= sw) xa = sw - 1;
            uint32_t p[4] = { src[ya * sw + xa], src[ya * sw + xb], src[yb * sw + xa], src[yb * sw + xb] };
            double w4[4] = { (1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy };
            double ch[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++)
                for (int b = 0; b < 4; b++) ch[b] += ((p[k] >> (b * 8)) & 255) * w4[k];
            uint32_t c = 0;
            for (int b = 0; b < 4; b++) c |= (uint32_t)(ch[b] + 0.5) << (b * 8);
            blend(&s->px[(size_t)ty * s->w + tx], c | 0, 1.0);
        }
    }
}

/* ---------------- 字体（运行时加载 FreeType） ---------------- */
static void *ft_lib;
static struct {
    FT_Error (*Init_FreeType)(FT_Library *);
    FT_Error (*New_Face)(FT_Library, const char *, FT_Long, FT_Face *);
    FT_Error (*Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
    FT_UInt  (*Get_Char_Index)(FT_Face, FT_ULong);
    FT_Error (*Load_Glyph)(FT_Face, FT_UInt, FT_Int32);
    FT_Error (*Render_Glyph)(FT_GlyphSlot, FT_Render_Mode);
    void     (*GlyphSlot_Embolden)(FT_GlyphSlot);
} ft;
static FT_Library g_ftl;
static FT_Face    g_face[2];      /* 0 常规 1 粗体（可能与常规相同，再加合成粗体） */
static int        g_synth_bold;
static char       g_font_path[2][1024];
static int        g_cur_px[2] = { -1, -1 };

const char *font_path(void) { return g_font_path[0]; }

typedef struct { uint32_t cp; int16_t sz; uint8_t bold, used; int16_t w, h, left, top; float adv; uint8_t *bm; } Glyph;
#define GCACHE 4096
static Glyph gcache[GCACHE];
static int   gcount;

static void *dl_any(const char **names) {
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

static int face_has_cjk(FT_Face f) { return ft.Get_Char_Index(f, 0x5F55) != 0 && ft.Get_Char_Index(f, 'A') != 0; }

/* fontconfig 匹配简体中文字体 */
static int fc_match(const char *pattern, char *out, int *index) {
    static const char *fcn[] = { "libfontconfig.so.1", "libfontconfig.so", NULL };
    void *h = dl_any(fcn);
    if (!h) return -1;
    FcConfig *(*InitLoad)(void) = dlsym(h, "FcInitLoadConfigAndFonts");
    FcPattern *(*NameParse)(const FcChar8 *) = dlsym(h, "FcNameParse");
    FcBool (*ConfigSubst)(FcConfig *, FcPattern *, FcMatchKind) = dlsym(h, "FcConfigSubstitute");
    void (*DefaultSubst)(FcPattern *) = dlsym(h, "FcDefaultSubstitute");
    FcPattern *(*FontMatch)(FcConfig *, FcPattern *, FcResult *) = dlsym(h, "FcFontMatch");
    FcResult (*GetString)(const FcPattern *, const char *, int, FcChar8 **) = dlsym(h, "FcPatternGetString");
    FcResult (*GetInt)(const FcPattern *, const char *, int, int *) = dlsym(h, "FcPatternGetInteger");
    void (*Destroy)(FcPattern *) = dlsym(h, "FcPatternDestroy");
    if (!InitLoad || !NameParse || !ConfigSubst || !DefaultSubst || !FontMatch || !GetString || !GetInt || !Destroy) return -1;
    static FcConfig *conf;
    if (!conf) conf = InitLoad();
    if (!conf) return -1;
    FcPattern *p = NameParse((const FcChar8 *)pattern);
    if (!p) return -1;
    ConfigSubst(conf, p, FcMatchPattern);
    DefaultSubst(p);
    FcResult r;
    FcPattern *m = FontMatch(conf, p, &r);
    Destroy(p);
    if (!m) return -1;
    FcChar8 *file = NULL; int idx = 0;
    int ok = GetString(m, "file", 0, &file) == FcResultMatch;
    if (ok) { snprintf(out, 1024, "%s", (char *)file); GetInt(m, "index", 0, &idx); *index = idx; }
    Destroy(m);
    return ok ? 0 : -1;
}

static int open_face(const char *path, int idx, FT_Face *f) {
    if (ft.New_Face(g_ftl, path, idx, f)) return -1;
    if (!face_has_cjk(*f)) { *f = NULL; return -1; }
    return 0;
}

int font_init(void) {
    static const char *ftn[] = { "libfreetype.so.6", "libfreetype.so", NULL };
    ft_lib = dl_any(ftn);
    if (!ft_lib) return -1;
    ft.Init_FreeType = dlsym(ft_lib, "FT_Init_FreeType");
    ft.New_Face = dlsym(ft_lib, "FT_New_Face");
    ft.Set_Pixel_Sizes = dlsym(ft_lib, "FT_Set_Pixel_Sizes");
    ft.Get_Char_Index = dlsym(ft_lib, "FT_Get_Char_Index");
    ft.Load_Glyph = dlsym(ft_lib, "FT_Load_Glyph");
    ft.Render_Glyph = dlsym(ft_lib, "FT_Render_Glyph");
    ft.GlyphSlot_Embolden = dlsym(ft_lib, "FT_GlyphSlot_Embolden");
    if (!ft.Init_FreeType || !ft.New_Face || !ft.Set_Pixel_Sizes || !ft.Get_Char_Index || !ft.Load_Glyph || !ft.Render_Glyph) return -1;
    if (ft.Init_FreeType(&g_ftl)) return -1;

    char path[1024]; int idx = 0;
    if (fc_match("sans-serif:lang=zh-cn", path, &idx) == 0 && open_face(path, idx, &g_face[0]) == 0)
        snprintf(g_font_path[0], 1024, "%s", path);
    if (!g_face[0]) {
        static const struct { const char *p; int i; } cand[] = {
            { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2 },
            { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2 },
            { "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 2 },
            { "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf", 0 },
            { "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0 },
            { "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0 },
            { "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc", 0 },
            { "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0 },
            { "/usr/share/fonts/truetype/arphic/uming.ttc", 0 },
            { NULL, 0 } };
        for (int i = 0; cand[i].p; i++)
            if (open_face(cand[i].p, cand[i].i, &g_face[0]) == 0) { snprintf(g_font_path[0], 1024, "%s", cand[i].p); break; }
    }
    if (!g_face[0]) return -2;
    if (fc_match("sans-serif:lang=zh-cn:weight=bold", path, &idx) == 0 && strcmp(path, g_font_path[0]) != 0 &&
        open_face(path, idx, &g_face[1]) == 0) {
        snprintf(g_font_path[1], 1024, "%s", path);
    } else {
        g_face[1] = g_face[0];
        g_synth_bold = 1;
    }
    return 0;
}

static void gcache_flush(void) {
    for (int i = 0; i < GCACHE; i++) { free(gcache[i].bm); gcache[i].bm = NULL; gcache[i].used = 0; }
    gcount = 0;
}

static Glyph *get_glyph(uint32_t cp, double size, int bold) {
    int16_t sz = (int16_t)(size + 0.5);
    uint32_t hsh = (cp * 2654435761u) ^ (uint32_t)(sz * 40503) ^ (uint32_t)(bold * 977);
    for (int k = 0; k < GCACHE; k++) {
        Glyph *g = &gcache[(hsh + k) & (GCACHE - 1)];
        if (!g->used) break;
        if (g->cp == cp && g->sz == sz && g->bold == bold) return g;
    }
    if (gcount > GCACHE * 3 / 4) gcache_flush();
    FT_Face f = g_face[bold ? 1 : 0];
    int fi = bold ? 1 : 0;
    if (g_cur_px[fi] != sz || (g_face[1] == g_face[0])) { ft.Set_Pixel_Sizes(f, 0, sz); g_cur_px[fi] = sz; if (g_face[1] == g_face[0]) g_cur_px[1 - fi] = sz; }
    FT_UInt gi = ft.Get_Char_Index(f, cp);
    if (ft.Load_Glyph(f, gi, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) return NULL;
    if (bold && g_synth_bold && ft.GlyphSlot_Embolden) ft.GlyphSlot_Embolden(f->glyph);
    if (ft.Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL)) return NULL;
    Glyph *g = NULL;
    for (int k = 0; k < GCACHE; k++) {
        Glyph *c = &gcache[(hsh + k) & (GCACHE - 1)];
        if (!c->used) { g = c; break; }
    }
    if (!g) return NULL;
    FT_Bitmap *b = &f->glyph->bitmap;
    g->used = 1; g->cp = cp; g->sz = sz; g->bold = (uint8_t)bold;
    g->w = (int16_t)b->width; g->h = (int16_t)b->rows;
    g->left = (int16_t)f->glyph->bitmap_left; g->top = (int16_t)f->glyph->bitmap_top;
    g->adv = f->glyph->advance.x / 64.0f;
    g->bm = malloc((size_t)g->w * g->h + 1);
    for (int y = 0; y < g->h; y++) memcpy(g->bm + y * g->w, b->buffer + y * b->pitch, g->w);
    gcount++;
    return g;
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s;
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { (*p)++; return '?'; }
    s++;
    for (int i = 0; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = (const char *)s + i; return '?'; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p = (const char *)s + n;
    return c;
}

double text_width(const char *t, double size, int bold) {
    double w = 0;
    while (*t) {
        uint32_t cp = utf8_next(&t);
        Glyph *g = get_glyph(cp, size, bold);
        if (g) w += g->adv;
    }
    return w;
}

double font_ascent(double size) {
    FT_Face f = g_face[0];
    ft.Set_Pixel_Sizes(f, 0, (FT_UInt)(size + 0.5));
    g_cur_px[0] = (int)(size + 0.5);
    if (g_face[1] == g_face[0]) g_cur_px[1] = g_cur_px[0];
    return f->size->metrics.ascender / 64.0;
}

double draw_text(Surf *s, double x, double yb, const char *t, double size, int bold, uint32_t c) {
    double pen = x;
    int iy = (int)(yb + 0.5);
    while (*t) {
        uint32_t cp = utf8_next(&t);
        Glyph *g = get_glyph(cp, size, bold);
        if (!g) continue;
        int gx = (int)(pen + 0.5) + g->left, gy = iy - g->top;
        for (int y = 0; y < g->h; y++) {
            int ty = gy + y;
            if (ty < 0 || ty >= s->h) continue;
            for (int xx = 0; xx < g->w; xx++) {
                int tx = gx + xx;
                if (tx < 0 || tx >= s->w) continue;
                uint8_t v = g->bm[y * g->w + xx];
                if (v) blend(&s->px[(size_t)ty * s->w + tx], c, v / 255.0);
            }
        }
        pen += g->adv;
    }
    return pen - x;
}
