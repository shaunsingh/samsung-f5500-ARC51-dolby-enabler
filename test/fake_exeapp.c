/*
 * A stand-in for exeAPP to test libArcToggle under qemu-arm.
 *
 * Places _ZTV19CRemoteControlInput, CRemoteControlInput::ProcessRemoteControlEvent,
 * its +20 non-virtual thunk, and TCSpeakerProxy::{GetInstance,GetExternalOutMode,
 * SetExternalOutMode} at their real T-MST12AKUC-2225.0 exeAPP addresses.
 *
 * Usage: qemu-arm -L <sysroot> fake_exeapp /abs/libArcToggle.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RC_EVENT_SYM "_ZN19CRemoteControlInput25ProcessRemoteControlEventEmtP18RemoteControlParam"
#define RC_THN20_SYM "_ZThn20_N19CRemoteControlInput25ProcessRemoteControlEventEmtP18RemoteControlParam"
#define SPKR_INST    "_ZN14TCSpeakerProxy11GetInstanceEN9TCSpeaker8ESpeakerE"
#define SPKR_SET     "_ZN14TCSpeakerProxy18SetExternalOutModeEii"
#define SPKR_GET     "_ZN14TCSpeakerProxy18GetExternalOutModeEPii"
#define RC_VTABLE    "_ZTV19CRemoteControlInput"

int   fw_rc_event(void *self, unsigned long type, uint16_t key, void *param) __asm__(RC_EVENT_SYM);
int   fw_rc_thn20(void *subobj, unsigned long type, uint16_t key, void *param) __asm__(RC_THN20_SYM);
void *fw_spkr_inst(int speaker) __asm__(SPKR_INST);
int   fw_spkr_set(void *self, int mode, int seek) __asm__(SPKR_SET);
int   fw_spkr_get(void *self, int *mode, int seek) __asm__(SPKR_GET);

static _Atomic int g_forwarded_events;
static _Atomic int g_last_forwarded_key;
static _Atomic int g_audio_mode = 0; /* 0 = PCM, 1 = Dolby */
static _Atomic int g_set_calls;
static int         g_speaker_obj;
static int         g_rc_obj[8];

__attribute__((section(".fw.rc_event"))) int fw_rc_event(void *self, unsigned long type, uint16_t key, void *param)
{
    (void)type;
    (void)param;
    if (self != &g_rc_obj[0])
        abort();
    g_last_forwarded_key = key;
    g_forwarded_events++;
    return 1;
}

/* Real exeAPP thunk is exactly 8 bytes at 0x01ed2718 right before 0x01ed2720. */
__asm__(".pushsection .fw.rc_thn20, \"ax\", %progbits\n"
        ".arm\n"
        ".global " RC_THN20_SYM "\n"
        ".type " RC_THN20_SYM ", %function\n"
        RC_THN20_SYM ":\n"
        "    sub r0, r0, #20\n"
        "    b " RC_EVENT_SYM "\n"
        ".popsection\n");

__attribute__((section(".fw.spkr_inst"))) void *fw_spkr_inst(int speaker)
{
    return speaker == 0 ? &g_speaker_obj : NULL;
}

__attribute__((section(".fw.spkr_set"))) int fw_spkr_set(void *self, int mode, int seek)
{
    if (self != &g_speaker_obj || seek != 0)
        abort();
    g_audio_mode = mode;
    g_set_calls++;
    return 1;
}

__attribute__((section(".fw.spkr_get"))) int fw_spkr_get(void *self, int *mode, int seek)
{
    if (self != &g_speaker_obj || seek != 0)
        abort();
    *mode = g_audio_mode;
    return 1;
}

/* Matches _ZTV19CRemoteControlInput layout: +0x30 (index 12) = primary, +0x44 (index 17) = thn20. */
void *volatile vtable_rc_input[20] __asm__(RC_VTABLE) __attribute__((section(".fw.rc_vtable"))) = {
    [12] = (void *)fw_rc_event,
    [17] = (void *)fw_rc_thn20,
};

typedef int  rc_fn(void *self, unsigned long type, uint16_t key, void *param);
typedef void lib_init_fn(void *handle, const char *libpath);

static int g_failures;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define EVENTUALLY(condition)                                                                                          \
    do {                                                                                                               \
        for (int tries = 0; !(condition) && tries < 300; tries++)                                                      \
            usleep(10000);                                                                                             \
        CHECK(condition);                                                                                              \
    } while (0)

static void inject_into(lib_init_fn *lib_init, const char *libpath, const char *words)
{
    union {
        char     bytes[512];
        uint32_t align;
    } buffer = { .bytes = "" };
    char      copy[256];
    uint32_t *header;
    uint32_t  count = 0;

    strcpy(buffer.bytes, libpath);
    header = (uint32_t *)(buffer.bytes + ((strlen(libpath) + 1 + 3) & ~3u));
    strcpy(copy, words);
    for (char *word = strtok(copy, " "); word; word = strtok(NULL, " "))
        count++;

    char *text = (char *)&header[2 + count];
    header[0] = 0x82374021u;
    header[1] = count;
    strcpy(copy, words);
    count = 0;
    for (char *word = strtok(copy, " "); word; word = strtok(NULL, " ")) {
        header[2 + count++] = (uint32_t)(text - (char *)header);
        text = stpcpy(text, word) + 1;
    }
    lib_init(NULL, buffer.bytes);
}

/* Dispatch through the IRemoteControlInput subobject slot (this+20), matching BT & IR callers. */
static int send_via_subobj(unsigned long type, uint16_t key)
{
    rc_fn *fn = (rc_fn *)vtable_rc_input[17];
    return fn((char *)&g_rc_obj[0] + 20, type, key, NULL);
}

/* Dispatch through the primary CRemoteControlInput slot. */
static int send_via_primary(unsigned long type, uint16_t key)
{
    rc_fn *fn = (rc_fn *)vtable_rc_input[12];
    return fn(&g_rc_obj[0], type, key, NULL);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);

    void        *library = dlopen(argv[1], RTLD_LAZY);
    lib_init_fn *lib_init = library ? (lib_init_fn *)dlsym(library, "lib_init") : NULL;
    if (!lib_init) {
        printf("FAIL %s\n", dlerror());
        return 1;
    }

    puts("bad arguments change nothing");
    inject_into(lib_init, argv[1], "KEY:0");
    inject_into(lib_init, argv[1], "KEY:70000");
    inject_into(lib_init, argv[1], "KEY:BOGUS");
    inject_into(lib_init, argv[1], "PCM DD");
    inject_into(lib_init, argv[1], "OFF LOG");
    CHECK(vtable_rc_input[12] == (void *)fw_rc_event && vtable_rc_input[17] == (void *)fw_rc_thn20);

    puts("default arming hooks vtable on KEY:DVR (0x72) and swallows DVR presses/repeats/releases");
    inject_into(lib_init, argv[1], "");
    CHECK(vtable_rc_input[12] != (void *)fw_rc_event && vtable_rc_input[17] != (void *)fw_rc_thn20);

    /* Unrelated key passes straight through. */
    CHECK(send_via_subobj(11, 0x001f) == 1);
    CHECK(g_forwarded_events == 1 && g_last_forwarded_key == 0x001f);

    /* First DVR press toggles PCM -> Dolby and is swallowed (never forwarded to MBR). */
    CHECK(send_via_subobj(11, 0x0072) == 1);
    EVENTUALLY(g_audio_mode == 1 && g_set_calls == 1);
    CHECK(g_forwarded_events == 1);

    /* Held-key repeats while down are swallowed and do NOT flip back. */
    CHECK(send_via_subobj(11, 0x0072) == 1);
    CHECK(send_via_subobj(11, 0x0072) == 1);
    usleep(100000);
    CHECK(g_audio_mode == 1 && g_set_calls == 1 && g_forwarded_events == 1);

    /* Release is also swallowed and immediately re-arms the next tap. */
    CHECK(send_via_subobj(12, 0x0072) == 1);
    CHECK(g_forwarded_events == 1);

    /* Immediate second tap toggles Dolby -> PCM via primary slot too. */
    CHECK(send_via_primary(20, 0x0072) == 1);
    EVENTUALLY(g_audio_mode == 0 && g_set_calls == 2);
    CHECK(send_via_primary(21, 0x0072) == 1);
    CHECK(g_forwarded_events == 1);

    puts("16-bit keycodes (>255) work and one-shot DD/PCM/TOGGLE work");
    inject_into(lib_init, argv[1], "KEY:0x112");
    /* 0x72 is now forwarded again. */
    CHECK(send_via_subobj(11, 0x0072) == 1);
    CHECK(g_forwarded_events == 2 && g_last_forwarded_key == 0x0072);
    /* 0x112 toggles and is swallowed. */
    CHECK(send_via_subobj(11, 0x0112) == 1);
    EVENTUALLY(g_audio_mode == 1 && g_set_calls == 3);
    CHECK(send_via_subobj(12, 0x0112) == 1);
    CHECK(g_forwarded_events == 2);

    inject_into(lib_init, argv[1], "PCM");
    EVENTUALLY(g_audio_mode == 0 && g_set_calls == 4);
    inject_into(lib_init, argv[1], "DD");
    EVENTUALLY(g_audio_mode == 1 && g_set_calls == 5);
    inject_into(lib_init, argv[1], "TOGGLE");
    EVENTUALLY(g_audio_mode == 0 && g_set_calls == 6);

    puts("OFF restores both vtable slots cleanly");
    inject_into(lib_init, argv[1], "OFF");
    CHECK(vtable_rc_input[12] == (void *)fw_rc_event && vtable_rc_input[17] == (void *)fw_rc_thn20);
    CHECK(send_via_subobj(11, 0x0112) == 1);
    CHECK(g_forwarded_events == 3 && g_last_forwarded_key == 0x0112);

    puts(g_failures ? "FAILED" : "PASSED");
    return g_failures != 0;
}
