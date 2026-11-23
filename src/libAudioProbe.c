/* Read-only probe of TPCExternalOut::GetAudioMode on exeTV. */
#include <dlfcn.h>

#include "samygo.h"

#define LOG      "/dtv/AudioProbe.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

typedef void *(*instance_fn)(int);
typedef int (*get_audio_fn)(void *, int *, int);

void lib_init(void *handle, const char *libpath)
{
    (void)handle;
    (void)libpath;

    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return;

    instance_fn  instance = (instance_fn)dlsym(exetv, "_ZN14TPCExternalOut8InstanceENS_12EExternalOutE");
    get_audio_fn get_audio = (get_audio_fn)dlsym(
        exetv, "_ZN18ExternalOutControl12GetAudioModeEPN14TPCExternalOut10EAudioTypeEN5TPCTv10EQueryTypeE");
    dlclose(exetv);

    say("instance=%p get_audio=%p", (void *)instance, (void *)get_audio);
    if (!instance || !get_audio)
        return;

    for (int out = 0; out < 2; out++) {
        void *obj = instance(out);
        say("out=%d object=%p", out, obj);
        for (int query = 0; query < 2 && obj; query++) {
            int mode = -1;
            int rc = get_audio(obj, &mode, query);
            say("out=%d query=%d mode=%d rc=%d", out, query, mode, rc);
        }
    }
}

void lib_deinit(void *handle)
{
    (void)handle;
}
