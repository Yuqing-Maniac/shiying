#ifndef SHIYING_TOOLBAR_H
#define SHIYING_TOOLBAR_H

enum {
    SY_CAPTURE_SCREEN, SY_CAPTURE_WINDOW, SY_CAPTURE_REGION,
    SY_RECORD_SCREEN, SY_RECORD_WINDOW, SY_RECORD_REGION,
    SY_MODE_COUNT
};

#define SY_UI_COLOR(light, dark) (cfg.theme ? (dark) : (light))
#define SY_UI_BG          SY_UI_COLOR(0xF2F5FA, 0x111727)
#define SY_UI_PANEL       SY_UI_COLOR(0xFFFFFF, 0x1B2335)
#define SY_UI_TEXT        SY_UI_COLOR(0x17233A, 0xEBF0FA)
#define SY_UI_SUB         SY_UI_COLOR(0x50627C, 0xA4B4CE)
#define SY_UI_BORDER      SY_UI_COLOR(0xDCE5F3, 0x33415B)
#define SY_UI_ACCENT      SY_UI_COLOR(0x255DC7, 0x83B6FF)
#define SY_UI_TINT        SY_UI_COLOR(0xEAF2FF, 0x213653)
#define SY_UI_RECORD      SY_UI_COLOR(0xBA3246, 0xFF8590)
#define SY_UI_RECORD_TINT SY_UI_COLOR(0xFCEEF0, 0x422736)
#define SY_UI_SEG         SY_UI_COLOR(0xEEF2F8, 0x252F44)
#define SY_UI_SEG_H       SY_UI_COLOR(0xE0E9F6, 0x35425D)
#define SY_UI_WHITE       0xFFFFFF
#define SY_UI_ON_ACCENT   SY_UI_COLOR(0xFFFFFF, 0x0B1324)

/* 对焦框收拢后，蓝光扫过标题下沿并短暂点亮红灯。 */
#define SY_INTRO_SECONDS 0.62
#define SY_INTRO_FRAME_MS 33
#define SY_INTRO_WIDTH 158
#define SY_INTRO_HEIGHT 56

typedef struct {
    double icon_size, focus, arm, focus_alpha;
    double sweep, sweep_alpha, tally_alpha;
} SyIntroFrame;

static inline double sy_intro_unit(double value) {
    if (value < 0) return 0;
    if (value > 1) return 1;
    return value;
}

static inline SyIntroFrame sy_intro_frame(double elapsed) {
    if (elapsed >= SY_INTRO_SECONDS) return (SyIntroFrame){.icon_size = 32};
    double t = sy_intro_unit(elapsed / SY_INTRO_SECONDS);
    double movement = 1 - sy_intro_unit(t * 2.4);
    double ease = movement * movement * movement;
    double sweep = sy_intro_unit((t - 0.24) / 0.49);
    double fade = 1 - sy_intro_unit((t - 0.68) / 0.32);
    double tally = sy_intro_unit((t - 0.62) / 0.12) * fade;
    return (SyIntroFrame){
        32 - 6 * ease, 20 + 5 * ease, 6 + 3 * (1 - ease),
        0.85 * (1 - sy_intro_unit((t - 0.20) / 0.48)),
        84 * sweep, 0.74 * fade, tally
    };
}

static const char *const sy_mode_labels[SY_MODE_COUNT] = {
    "截取全屏", "截取窗口", "截取区域",
    "录制全屏", "录制窗口", "录制区域"
};

#endif
