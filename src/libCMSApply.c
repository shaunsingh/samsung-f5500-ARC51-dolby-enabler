/* Apply Samsung CMS through the official TCScreenImpl API, with an identity-cube reset. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dlfcn.h>
#define EXTRA_COOKIE 0x82374021u
typedef struct { uint8_t r, g, b; } __attribute__((packed)) rgb_t;
typedef void *(*fn_get_screen_t)(void);
typedef int (*fn_set_cs_t)(void *, int, int);
typedef int (*fn_get_cs_t)(void *, int *, int);
typedef int (*fn_set_custom_t)(void *, int, const rgb_t *);
typedef int (*fn_get_custom_t)(void *, int, rgb_t *);
typedef int (*fn_save_t)(void *, int);
static FILE *lf;
static void log_msg(const char *fmt, ...)
{
    va_list ap; if (!lf) return;
    va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap);
    fputc('\n', lf); fflush(lf);
}
static int parse_args(const char *path, const char **argv, int cap)
{
    int argc = 1; argv[0] = path;
    uintptr_t p = (uintptr_t)(path + strlen(path) + 1); p = (p + 3u) & ~3u;
    const uint32_t *extra = (const uint32_t *)p;
    if (extra[0] != EXTRA_COOKIE) return argc;
    uint32_t n = extra[1]; const uint32_t *offs = &extra[2];
    for (uint32_t i = 0; i < n && argc < cap; i++) argv[argc++] = (const char *)(p + offs[i]);
    return argc;
}
void lib_init(void *_h, const char *path)
{
    (void)_h;
    const char *argv[8]; int argc = parse_args(path, argv, 8);
    int bt709 = argc > 1 && strcasecmp(argv[1], "BT709") == 0;
    lf = fopen("/dtv/CMSApply.log", "a");
    log_msg("=== CMSApply %s ===", bt709 ? "BT709" : "NATIVE");
    void *h = dlopen(0, RTLD_LAZY);
    fn_get_screen_t get_screen = (fn_get_screen_t)dlsym(h, "_ZN5TCAPI9GetScreenEv");
    fn_set_cs_t set_cs = (fn_set_cs_t)dlsym(h, "_ZN12TCScreenImpl13SetColorSpaceEN8TCScreen17EDetailColorSpaceEN4TCMW5ESeekE");
    fn_get_cs_t get_cs = (fn_get_cs_t)dlsym(h, "_ZN12TCScreenImpl13GetColorSpaceEPN8TCScreen17EDetailColorSpaceEN4TCMW5ESeekE");
    fn_set_custom_t set_custom = (fn_set_custom_t)dlsym(h, "_ZN12TCScreenImpl19SetColorSpaceCustomEiPK6TPTRGB");
    fn_get_custom_t get_custom = (fn_get_custom_t)dlsym(h, "_ZN12TCScreenImpl19GetColorSpaceCustomEiP6TPTRGB");
    fn_save_t save = (fn_save_t)dlsym(h, "_ZN12TCScreenImpl22SaveCurrentPictureModeEN8TCScreen12EPictureModeE");
    log_msg("syms %p %p %p %p %p %p", (void *)get_screen, (void *)set_cs, (void *)get_cs,
            (void *)set_custom, (void *)get_custom, (void *)save);
    if (!get_screen || !set_cs || !get_cs || !set_custom || !get_custom || !save) goto out;
    void *screen = get_screen();
    static const rgb_t identity[6] = {
        {50,0,0}, {0,50,0}, {0,0,50}, {50,50,0}, {0,50,50}, {50,0,50}
    };
    static const rgb_t preset[6] = {
        {46,0,17}, {34,57,16}, {20,0,49}, {48,56,21}, {37,56,51}, {46,0,50}
    };
    int mode = -1, rc = get_cs(screen, &mode, 0);
    log_msg("before mode=%d rc=%d", mode, rc);
    for (int i = 0; i < 6; i++) {
        rgb_t v = {-1,-1,-1}; rc = get_custom(screen, i, &v);
        log_msg("before custom %d %d %d %d rc=%d", i, v.r, v.g, v.b, rc);
    }
    rc = set_cs(screen, 3, 0); log_msg("select custom rc=%d", rc);
    for (int i = 0; i < 6; i++) {
        rc = set_custom(screen, i, &identity[i]);
        log_msg("identity %d rc=%d", i, rc);
    }
    usleep(100000);
    if (bt709) {
        for (int i = 0; i < 6; i++) {
            rc = set_custom(screen, i, &preset[i]);
            log_msg("preset %d %d %d %d rc=%d", i, preset[i].r, preset[i].g, preset[i].b, rc);
        }
        rc = set_cs(screen, 3, 0); log_msg("reselect custom rc=%d", rc);
    } else {
        rc = set_cs(screen, 1, 1); log_msg("select native rc=%d", rc);
    }
    usleep(100000);
    for (int q = 0; q < 3; q++) {
        mode = -1; rc = get_cs(screen, &mode, q);
        log_msg("after mode query=%d mode=%d rc=%d", q, mode, rc);
    }
    for (int i = 0; i < 6; i++) {
        rgb_t v = {-1,-1,-1}; rc = get_custom(screen, i, &v);
        log_msg("after custom %d %d %d %d rc=%d", i, v.r, v.g, v.b, rc);
    }
    rc = save(screen, 2); log_msg("save movie rc=%d", rc);
out:
    log_msg("=== CMSApply done ===");
    if (lf) fclose(lf);
    lf = NULL;
}
void lib_deinit(void *_h) { (void)_h; }
