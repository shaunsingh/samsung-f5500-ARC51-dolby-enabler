#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
typedef void hpd_fn(uint32_t enable, uint32_t port);
void lib_init(void *handle, const char *libpath)
{
    (void)handle; (void)libpath;
    FILE *log = fopen("/dtv/HPD_HDMI3.log", "a");
    void *exetv = dlopen(0, RTLD_LAZY);
    hpd_fn *set_hpd = (hpd_fn *)dlsym(exetv, "MApi_XC_Sys_SetHPD");
    if (log) fprintf(log, "set_hpd=%p\n", (void *)set_hpd);
    if (set_hpd) {
        set_hpd(0, 0x51); /* MStar port 1 = Samsung HDMI3 */
        usleep(800000);
        set_hpd(1, 0x51);
        if (log) fprintf(log, "HDMI3 HPD pulsed low for 800 ms\n");
    }
    if (log) fclose(log);
    if (exetv) dlclose(exetv);
}
void lib_deinit(void *handle) { (void)handle; }
