/*
 * libCMProbe.c - probe for the Samsung ColorMapping 3x3x3 LUT (XC bank 0x14).
 *
 * DUMP reads the 96 words of bank 0x14. SET:<label>:reg=delta,... perturbs the
 * 81-word LUT transiently and always restores the original values on exit.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "samygo.h"

#define LOG      "/dtv/CMProbe.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

#define LUT_BANK 0x14
#define LUT_REGS 81

typedef unsigned (*fn_rd_t)(int, unsigned, unsigned);
typedef void (*fn_wr_t)(int, unsigned, unsigned, unsigned);

static fn_rd_t rd;
static fn_wr_t wr;

/* Decimal parser using only symbols present in the TV's glibc 2.14.1. */
static int parse_i(const char *s, const char **end, int *out)
{
    int sign = 1, v = 0, any = 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        any = 1;
    }
    if (!any)
        return 0;
    *out = sign * v;
    if (end)
        *end = s;
    return 1;
}

static void dump_regs(const char *tag)
{
    for (unsigned base = 0; base < 0x60; base += 16) {
        char   line[256];
        size_t pos = 0;
        int    n = snprintf(line, sizeof line, "[%s] bk14+%02x", tag, base);
        if (n > 0)
            pos = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        for (unsigned r = base; r < base + 16 && r < 0x60 && pos < sizeof line - 1; r++) {
            n = snprintf(line + pos, sizeof line - pos, " %u", rd(LUT_BANK, r, 0xffff));
            if (n > 0)
                pos += (size_t)n < sizeof line - pos ? (size_t)n : sizeof line - 1 - pos;
        }
        say("%s", line);
    }
}

static void write_lut(const unsigned *v)
{
    for (unsigned r = 1; r <= LUT_REGS; r++)
        wr(LUT_BANK, r, v[r - 1], 0x1fff);
}

void lib_init(void *handle, const char *libpath)
{
    (void)handle;
    const char *argv[32];
    int         argc = sgo_args(libpath, argv, 32);
    say("=== CMProbe init argc=%d ===", argc);

    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return;

    rd = (fn_rd_t)dlsym(exetv, "_Z25VenderVideo_Read2BytesMsk15SdVV_Bank_Num_kjj");
    wr = (fn_wr_t)dlsym(exetv, "_Z26VenderVideo_Write2BytesMsk15SdVV_Bank_Num_kjjj");
    dlclose(exetv);

    say("syms rd=%p wr=%p", (void *)rd, (void *)wr);
    if (!rd || !wr) {
        say("missing symbols");
        return;
    }

    unsigned orig[LUT_REGS];
    for (unsigned r = 1; r <= LUT_REGS; r++)
        orig[r - 1] = rd(LUT_BANK, r, 0x1fff);

    int touched = 0;
    for (int i = 0; i < argc && i < 32; i++) {
        const char *a = argv[i];
        if (strcasecmp(a, "DUMP") == 0) {
            dump_regs("dump");
        } else if (strncasecmp(a, "SET:", 4) == 0) {
            unsigned    v[LUT_REGS];
            char        label[32] = "set";
            const char *p = a + 4;
            const char *c = strchr(p, ':');
            if (c && (size_t)(c - p) < sizeof label) {
                memcpy(label, p, (size_t)(c - p));
                label[c - p] = '\0';
                p = c + 1;
            }
            for (int k = 0; k < LUT_REGS; k++)
                v[k] = 512;
            while (*p) {
                int         d, reg_i;
                const char *rest;
                if (!parse_i(p, &rest, &reg_i) || *rest != '=')
                    break;
                if (!parse_i(rest + 1, &rest, &d))
                    break;
                if (reg_i >= 1 && reg_i <= LUT_REGS)
                    v[reg_i - 1] = (unsigned)(512 + d);
                p = rest;
                if (*p == ',')
                    p++;
            }
            write_lut(v);
            touched = 1;
            usleep(120000);
            dump_regs(label);
        } else if (strcasecmp(a, "RESTORE") == 0) {
            write_lut(orig);
            touched = 0;
            usleep(120000);
            dump_regs("restored");
        }
    }

    if (touched) {
        write_lut(orig);
        int ok = 1;
        for (unsigned r = 1; r <= LUT_REGS; r++)
            if (rd(LUT_BANK, r, 0x1fff) != orig[r - 1])
                ok = 0;
        say("final restore %s", ok ? "OK" : "MISMATCH");
    }
    say("=== CMProbe done ===");
}

void lib_deinit(void *handle)
{
    (void)handle;
}
