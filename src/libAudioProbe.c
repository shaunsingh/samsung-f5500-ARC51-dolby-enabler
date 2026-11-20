#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
typedef void *(*instance_fn)(int);
typedef int (*get_audio_fn)(void *, int *, int);
void lib_init(void *handle, const char *libpath)
{
    (void)handle; (void)libpath;
    FILE *log = fopen("/dtv/AudioProbe.log", "a");
    void *exetv = dlopen(0, RTLD_LAZY);
    instance_fn instance = (instance_fn)dlsym(exetv, "_ZN14TPCExternalOut8InstanceENS_12EExternalOutE");
    get_audio_fn get_audio = (get_audio_fn)dlsym(exetv, "_ZN18ExternalOutControl12GetAudioModeEPN14TPCExternalOut10EAudioTypeEN5TPCTv10EQueryTypeE");
    if (log) fprintf(log, "instance=%p get_audio=%p\n", (void *)instance, (void *)get_audio);
    if (instance && get_audio) {
        for (int out = 0; out < 2; out++) {
            void *obj = instance(out);
            if (log) fprintf(log, "out=%d object=%p\n", out, obj);
            for (int query = 0; query < 2 && obj; query++) {
                int mode = -1;
                int rc = get_audio(obj, &mode, query);
                if (log) fprintf(log, "out=%d query=%d mode=%d rc=%d\n", out, query, mode, rc);
            }
        }
    }
    if (log) fclose(log);
    if (exetv) dlclose(exetv);
}
void lib_deinit(void *handle) { (void)handle; }
