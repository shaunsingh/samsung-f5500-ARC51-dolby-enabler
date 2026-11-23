/* Read-only dump of Samsung factory ColorMapping tables and live XC bank 0x14. */
#include <dlfcn.h>
#include <stdint.h>

#include "samygo.h"

#define LOG      "/dtv/CMFactory.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

typedef int (*fn_get_unique_t)(void *, unsigned, int *, int, int);
typedef unsigned (*fn_rd_t)(int, unsigned, unsigned);

void lib_init(void *handle, const char *libpath)
{
    (void)handle;
    (void)libpath;

    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return;

    fn_get_unique_t get_unique = (fn_get_unique_t)dlsym(
        exetv, "_ZN13DBFactoryBase17GetUniqueItemDataEjPi10CDSource_k14CDResolution_k");
    fn_rd_t rd = (fn_rd_t)dlsym(exetv, "_Z25VenderVideo_Read2BytesMsk15SdVV_Bank_Num_kjj");
    void **factory_slot = (void **)dlsym(exetv, "_ZN13DBFactoryBase16m_pDBFactoryBaseE");
    dlclose(exetv);

    void *factory = factory_slot ? *factory_slot : NULL;
    say("=== CMFactory get=%p rd=%p factory=%p ===", (void *)get_unique, (void *)rd, factory);
    if (!get_unique || !rd || !factory) {
        say("=== CMFactory done ===");
        return;
    }

    for (unsigned id = 0x2b0; id <= 0x2e5; id++) {
        int value = -999;
        int rc = get_unique(factory, id, &value, 0, 0x89);
        say("FACTORY %#x %d rc=%d", id, value, rc);
    }
    for (unsigned reg = 0; reg < 0x60; reg += 16) {
        char   line[256];
        size_t pos = 0;
        int    n = snprintf(line, sizeof line, "BANK14 %#x", reg);
        if (n > 0)
            pos = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        for (unsigned r = reg; r < reg + 16 && r < 0x60 && pos < sizeof line - 1; r++) {
            n = snprintf(line + pos, sizeof line - pos, " %u", rd(0x14, r, 0xffff));
            if (n > 0)
                pos += (size_t)n < sizeof line - pos ? (size_t)n : sizeof line - 1 - pos;
        }
        say("%s", line);
    }
    say("=== CMFactory done ===");
}

void lib_deinit(void *handle)
{
    (void)handle;
}
