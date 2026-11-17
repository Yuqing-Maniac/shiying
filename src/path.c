#define _GNU_SOURCE
#include "path.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <shlobj.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#include <pwd.h>
#endif

char g_exe_dir[1024];
char g_cache_dir[1024];
char g_conf_dir[1024];

double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void path_stamp(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    struct tm *lt = localtime(&t);
    if (!lt) { snprintf(out, n, "00000000_000000"); return; }
    tm = *lt;
#else
    localtime_r(&t, &tm);
#endif
    snprintf(out, n, "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void logf_(const char *fmt, ...) {
    char p[1100];
    snprintf(p, sizeof p, "%s/shiying.log", g_cache_dir);
    FILE *f = fopen(p, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    struct tm *lt = localtime(&t);
    if (!lt) { fclose(f); return; }
    tm = *lt;
#else
    localtime_r(&t, &tm);
#endif
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int mkdir_p(const char *path) {
    char tmp[1100];
    if (!path || !path[0] || strlen(path) >= sizeof tmp) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[n - 1] = 0;
    char *first = tmp + 1;
#ifdef _WIN32
    if (n >= 3 && tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\')) first = tmp + 3;
    else if (n >= 2 && strspn(tmp, "/\\") == 2) {
        char *server = strpbrk(tmp + 2, "/\\");
        char *share = server ? strpbrk(server + 1, "/\\") : NULL;
        if (!share) return GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES ? -1 : 0;
        first = share + 1;
    }
#endif
    for (char *p = first; *p; p++)
        if (*p == '/' || *p == '\\') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

const char *home_dir(void) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
    if (h && *h) return h;
    h = getenv("HOME");
    return (h && *h) ? h : ".";
#else
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/tmp";
#endif
}

int sy_desktop_dir(char *out, size_t n) {
    if (!out || !n) return -1;
    out[0] = 0;
#ifdef _WIN32
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop)) ||
        !WideCharToMultiByte(CP_UTF8, 0, desktop, -1, out, (int)n, NULL, NULL)) return -1;
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return 0;
#else
    const char *home = home_dir();
    char path[1200], line[1200];
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config && config[0] == '/') snprintf(path, sizeof path, "%s/user-dirs.dirs", config);
    else snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home);
    FILE *file = fopen(path, "r");
    if (file) {
        const char key[] = "XDG_DESKTOP_DIR=\"";
        while (fgets(line, sizeof line, file)) {
            if (strncmp(line, key, sizeof key - 1)) continue;
            char *value = line + sizeof key - 1;
            char *last = strrchr(value, '"');
            if (!last) break;
            *last = 0;
            int len = -1;
            if (!strcmp(value, "$HOME") || !strncmp(value, "$HOME/", 6))
                len = snprintf(out, n, "%s%s", home, value + 5);
            else if (value[0] == '/') len = snprintf(out, n, "%s", value);
            fclose(file);
            return len >= 0 && (size_t)len < n ? 0 : -1;
        }
        fclose(file);
    }
    const char *names[] = { "桌面", "Desktop" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        struct stat st;
        int len = snprintf(out, n, "%s/%s", home, names[i]);
        if (len >= 0 && (size_t)len < n && stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
#endif
}

void file_fsync(FILE *f) {
    if (!f) return;
    fflush(f);
    int fd = fileno(f);
    if (fd < 0) return;
#ifdef _WIN32
    _commit(fd);
#else
    fsync(fd);
#endif
}

int path_fsync(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    file_fsync(f);
    fclose(f);
    return 0;
}
