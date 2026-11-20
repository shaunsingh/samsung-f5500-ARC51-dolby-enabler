/*
 * libCMProbe.c - read-only probe for the Samsung ColorMapping 3x3x3 LUT (XC bank 0x14)
 * and MStar pixel readback (AFTER_DLC / PRE_GAMMA). DIFF mode perturbs the LUT
 * transiently and always restores the registers it read on entry.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_PATH     "/dtv/CMProbe.log"
#define EXTRA_COOKIE 0x82374021u
#define LUT_BANK     0x14
#define LUT_REGS     81

typedef struct { int stage; uint16_t x, y; uint32_t r, g, b; } px_t;
typedef int (*fn_getpix_t)(px_t *, int);
typedef unsigned (*fn_rd_t)(int, unsigned, unsigned);
typedef void (*fn_wr_t)(int, unsigned, unsigned, unsigned);

static fn_getpix_t getpix;
static fn_rd_t rd;
static fn_wr_t wr;
static FILE *lf;

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    if (!lf) return;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fflush(lf);
}

static void msleep(int ms)
{
    usleep((useconds_t)ms * 1000);
}

/* Decimal parse that stays on symbols this TV's libc 2.14.1 actually has. */
static int parse_i(const char *s, const char **end, int *out)
{
    int sign = 1, v = 0, any = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (!any) return 0;
    *out = sign * v;
    if (end) *end = s;
    return 1;
}

static int parse_pair(const char *s, const char *key, int *a, int *b)
{
    size_t n = strlen(key);
    const char *rest;
    if (strncmp(s, key, n) != 0 || s[n] != ':') return 0;
    if (!parse_i(s + n + 1, &rest, a) || *rest != ':') return 0;
    if (!parse_i(rest + 1, &rest, b) || *rest != '\0') return 0;
    return 1;
}

static int parse_samygo_args(const char *libpath, const char **argv, int max_args)
{
    if (!libpath || max_args < 1) return 0;
    int argc = 1;
    argv[0] = libpath;
    uintptr_t p = (uintptr_t)(libpath + strlen(libpath) + 1);
    p = (p + 3u) & ~(uintptr_t)3u;
    const uint32_t *extra = (const uint32_t *)p;
    if (extra[0] != EXTRA_COOKIE) return argc;
    uint32_t n = extra[1];
    const uint32_t *offs = &extra[2];
    for (uint32_t i = 0; i < n && argc < max_args; i++)
        argv[argc++] = (const char *)(p + offs[i]);
    return argc;
}

static void dump_regs(const char *tag)
{
    char line[1024];
    int n = 0;
    n += snprintf(line + n, sizeof line - n, "[%s] bk14", tag);
    for (unsigned r = 0; r < 0x60; r++) {
        n += snprintf(line + n, sizeof line - n, " %u", rd(LUT_BANK, r, 0xffff));
        if (n > 900) { log_msg("%s", line); n = snprintf(line, sizeof line, "[%s] bk14+", tag); }
    }
    log_msg("%s", line);
}

static void read_point(const char *tag, int x, int y)
{
    px_t a = { 1, (uint16_t)x, (uint16_t)y, 0, 0, 0 };
    px_t b = { 2, (uint16_t)x, (uint16_t)y, 0, 0, 0 };
    int ra = getpix(&a, 0);
    int rb = getpix(&b, 0);
    log_msg("[%s] %d %d dlc %u %u %u pre %u %u %u rc %d %d", tag, x, y,
            a.r, a.g, a.b, b.r, b.g, b.b, ra, rb);
}

static void read_grid(const char *tag, int nx, int ny)
{
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            read_point(tag, (2 * i + 1) * 1920 / (2 * nx), (2 * j + 1) * 1080 / (2 * ny));
}

static void write_lut(const unsigned *v)
{
    for (unsigned r = 1; r <= LUT_REGS; r++)
        wr(LUT_BANK, r, v[r - 1], 0x1fff);
}

void lib_init(void *_h, const char *libpath)
{
    (void)_h;
    const char *argv[32];
    int argc = parse_samygo_args(libpath, argv, 32);
    lf = fopen(LOG_PATH, "a");
    log_msg("=== CMProbe init argc=%d ===", argc);

    void *h = dlopen(0, RTLD_LAZY);
    getpix = (fn_getpix_t)dlsym(h, "MApi_XC_Get_Pixel_RGB");
    rd = (fn_rd_t)dlsym(h, "_Z25VenderVideo_Read2BytesMsk15SdVV_Bank_Num_kjj");
    wr = (fn_wr_t)dlsym(h, "_Z26VenderVideo_Write2BytesMsk15SdVV_Bank_Num_kjjj");
    log_msg("syms getpix=%p rd=%p wr=%p", (void *)getpix, (void *)rd, (void *)wr);
    if (!getpix || !rd || !wr) { log_msg("missing symbols"); goto out; }

    unsigned orig[LUT_REGS];
    for (unsigned r = 1; r <= LUT_REGS; r++) orig[r - 1] = rd(LUT_BANK, r, 0x1fff);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int nx, ny, x, y;
        if (strcasecmp(a, "DUMP") == 0) {
            dump_regs("dump");
        } else if (parse_pair(a, "GRID", &nx, &ny)) {
            read_grid("grid", nx, ny);
        } else if (parse_pair(a, "PT", &x, &y)) {
            read_point("pt", x, y);
        } else if (strncasecmp(a, "SET:", 4) == 0) {
            /* SET:<label>:reg=delta,reg=delta,... applied on top of an identity LUT, grid read, then restore */
            unsigned v[LUT_REGS];
            char label[32] = "set";
            const char *p = a + 4;
            const char *c = strchr(p, ':');
            if (c && c - p < (int)sizeof label) { memcpy(label, p, c - p); label[c - p] = 0; p = c + 1; }
            for (int k = 0; k < LUT_REGS; k++) v[k] = 512;
            while (*p) {
                int d;
                const char *rest;
                int reg_i;
                if (!parse_i(p, &rest, &reg_i) || *rest != '=') break;
                if (!parse_i(rest + 1, &rest, &d)) break;
                if (reg_i >= 1 && reg_i <= (int)LUT_REGS) v[reg_i - 1] = (unsigned)(512 + d);
                p = rest;
                if (*p == ',') p++;
            }
            write_lut(v);
            msleep(120);
            dump_regs(label);
            for (int k = i + 1; k < argc && strncasecmp(argv[k], "GRID:", 5) != 0; k++) ;
            log_msg("[%s] begin", label);
        } else if (strcasecmp(a, "RESTORE") == 0) {
            write_lut(orig);
            msleep(120);
            dump_regs("restored");
        }
    }
    write_lut(orig);
    {
        int ok = 1;
        for (unsigned r = 1; r <= LUT_REGS; r++)
            if (rd(LUT_BANK, r, 0x1fff) != orig[r - 1]) ok = 0;
        log_msg("final restore %s", ok ? "OK" : "MISMATCH");
    }
out:
    log_msg("=== CMProbe done ===");
    if (lf) fclose(lf);
    lf = NULL;
}

void lib_deinit(void *_h) { (void)_h; }
