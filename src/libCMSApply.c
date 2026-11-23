/*
 * libCMSApply - apply the BT.709 Custom color-space preset (or revert to
 * Native) through TCScreenImpl, resetting the cube to identity first so
 * averaged nodes are deterministic.
 *
 * Usage: samyGOso -T -B -l libCMSApply.so BT709   (or no arg for Native)
 */
#include <dlfcn.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>

#include "samygo.h"

#define LOG      "/dtv/CMSApply.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

typedef struct {
    uint8_t r, g, b;
} __attribute__((packed)) rgb_t;

typedef void *(*fn_get_screen_t)(void);
typedef int (*fn_set_cs_t)(void *, int, int);
typedef int (*fn_get_cs_t)(void *, int *, int);
typedef int (*fn_set_custom_t)(void *, int, const rgb_t *);
typedef int (*fn_get_custom_t)(void *, int, rgb_t *);
typedef int (*fn_save_t)(void *, int);

void lib_init(void *handle, const char *libpath)
{
    (void)handle;
    const char *argv[8];
    int         argc = sgo_args(libpath, argv, 8);
    int         bt709 = argc > 0 && strcasecmp(argv[0], "BT709") == 0;

    say("=== CMSApply %s ===", bt709 ? "BT709" : "NATIVE");
    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return;

    fn_get_screen_t get_screen = (fn_get_screen_t)dlsym(exetv, "_ZN5TCAPI9GetScreenEv");
    fn_set_cs_t     set_cs = (fn_set_cs_t)dlsym(exetv, "_ZN12TCScreenImpl13SetColorSpaceEN8TCScreen17EDetailColorSpaceEN4TCMW5ESeekE");
    fn_get_cs_t     get_cs = (fn_get_cs_t)dlsym(exetv, "_ZN12TCScreenImpl13GetColorSpaceEPN8TCScreen17EDetailColorSpaceEN4TCMW5ESeekE");
    fn_set_custom_t set_custom = (fn_set_custom_t)dlsym(exetv, "_ZN12TCScreenImpl19SetColorSpaceCustomEiPK6TPTRGB");
    fn_get_custom_t get_custom = (fn_get_custom_t)dlsym(exetv, "_ZN12TCScreenImpl19GetColorSpaceCustomEiP6TPTRGB");
    fn_save_t       save = (fn_save_t)dlsym(exetv, "_ZN12TCScreenImpl22SaveCurrentPictureModeEN8TCScreen12EPictureModeE");
    dlclose(exetv);

    if (!get_screen || !set_cs || !get_cs || !set_custom || !get_custom || !save) {
        say("missing TCScreenImpl symbols, nothing done");
        return;
    }

    void *screen = get_screen();
    if (!screen) {
        say("TCAPI::GetScreen() returned NULL");
        return;
    }

    static const rgb_t identity[6] = {
        { 50, 0, 0 }, { 0, 50, 0 }, { 0, 0, 50 }, { 50, 50, 0 }, { 0, 50, 50 }, { 50, 0, 50 },
    };
    static const rgb_t preset[6] = {
        { 46, 0, 17 }, { 34, 57, 16 }, { 20, 0, 49 }, { 48, 56, 21 }, { 37, 56, 51 }, { 46, 0, 50 },
    };

    int mode = -1;
    int rc = get_cs(screen, &mode, 0);
    say("before mode=%d rc=%d", mode, rc);

    rc = set_cs(screen, 3, 0);
    say("select custom rc=%d", rc);
    for (int i = 0; i < 6; i++)
        set_custom(screen, i, &identity[i]);
    usleep(100000);

    if (bt709) {
        for (int i = 0; i < 6; i++) {
            rc = set_custom(screen, i, &preset[i]);
            say("preset %d: %u,%u,%u rc=%d", i, preset[i].r, preset[i].g, preset[i].b, rc);
        }
        rc = set_cs(screen, 3, 0);
        say("reselect custom rc=%d", rc);
    } else {
        rc = set_cs(screen, 1, 0);
        say("select native rc=%d", rc);
    }
    usleep(100000);

    mode = -1;
    rc = get_cs(screen, &mode, 0);
    say("after mode=%d rc=%d", mode, rc);
    for (int i = 0; i < 6; i++) {
        rgb_t v = { 0, 0, 0 };
        rc = get_custom(screen, i, &v);
        say("after custom %d: %u,%u,%u rc=%d", i, v.r, v.g, v.b, rc);
    }
    rc = save(screen, 2);
    say("save movie rc=%d", rc);
    say("=== CMSApply done ===");
}

void lib_deinit(void *handle)
{
    (void)handle;
}
