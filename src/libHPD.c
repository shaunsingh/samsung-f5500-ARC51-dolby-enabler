/* Pulse hot-plug detect low for 800 ms on HDMI3 (MStar port 0x51). */
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

#include "samygo.h"

#define LOG      "/dtv/HPD_HDMI3.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

typedef void hpd_fn(uint32_t enable, uint32_t port);

void lib_init(void *handle, const char *libpath)
{
    (void)handle;
    (void)libpath;

    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return;

    hpd_fn *set_hpd = (hpd_fn *)dlsym(exetv, "MApi_XC_Sys_SetHPD");
    dlclose(exetv);

    say("set_hpd=%p", (void *)set_hpd);
    if (set_hpd) {
        set_hpd(0, 0x51); /* MStar port 1 = Samsung HDMI3 */
        usleep(800000);
        set_hpd(1, 0x51);
        say("HDMI3 HPD pulsed low for 800 ms");
    }
}

void lib_deinit(void *handle)
{
    (void)handle;
}
