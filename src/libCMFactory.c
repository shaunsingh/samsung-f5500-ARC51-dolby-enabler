/* Read-only dump of Samsung factory ColorMapping tables and live XC bank 0x14. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>

typedef int (*fn_get_unique_t)(void *, unsigned, int *, int, int);
typedef unsigned (*fn_rd_t)(int, unsigned, unsigned);
static FILE *lf;
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    if (!lf) return;
    va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap);
    fputc('\n', lf); fflush(lf);
}
void lib_init(void *_h, const char *libpath)
{
    (void)_h; (void)libpath;
    lf = fopen("/dtv/CMFactory.log", "a");
    void *h = dlopen(0, RTLD_LAZY);
    fn_get_unique_t get_unique = (fn_get_unique_t)dlsym(h,
        "_ZN13DBFactoryBase17GetUniqueItemDataEjPi10CDSource_k14CDResolution_k");
    fn_rd_t rd = (fn_rd_t)dlsym(h, "_Z25VenderVideo_Read2BytesMsk15SdVV_Bank_Num_kjj");
    void *factory = *(void **)0x1f5b2fc;
    log_msg("=== CMFactory get=%p rd=%p factory=%p ===", (void *)get_unique, (void *)rd, factory);
    if (!get_unique || !rd || !factory) goto out;
    for (unsigned id = 0x2b0; id <= 0x2e5; id++) {
        int value = -999;
        int rc = get_unique(factory, id, &value, 0, 0x89);
        log_msg("FACTORY %#x %d rc=%d", id, value, rc);
    }
    for (unsigned reg = 0; reg < 0x60; reg += 16) {
        char line[256]; int n = snprintf(line, sizeof line, "BANK14 %#x", reg);
        for (unsigned r = reg; r < reg + 16 && r < 0x60; r++)
            n += snprintf(line + n, sizeof line - n, " %u", rd(0x14, r, 0xffff));
        log_msg("%s", line);
    }
out:
    log_msg("=== CMFactory done ===");
    if (lf) fclose(lf);
    lf = NULL;
}
void lib_deinit(void *_h) { (void)_h; }
