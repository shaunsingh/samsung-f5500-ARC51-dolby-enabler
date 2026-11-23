/*
 * libArcToggle — bind one remote key to toggle PCM and Dolby Digital on ARC.
 *
 * Target: exeAPP of Samsung PN60F5500AF firmware T-MST12AKUC-2225.0.
 * Any other process or firmware build is logged and left untouched.
 *
 * Architecture:
 *   Both the IR remote (via exeTV KeyCommon -> vdbinder -> APPRemoteControlInputNative)
 *   and the Bluetooth Smart Touch remote (via bsa_server -> BluetoothInputDev) dispatch
 *   through CRemoteControlInput::ProcessRemoteControlEvent via its vtable in exeAPP,
 *   upstream of SMInput::CallRemoteControlEventDelegator and CTvApplicationManager.
 *
 *   Replacing the two ProcessRemoteControlEvent slots in _ZTV19CRemoteControlInput
 *   (primary class vtable +0x30 and IRemoteControlInput subobject vtable +0x44) is an
 *   atomic single-word pointer swap with no code patching, no stub page, and no branch
 *   range limit. Swallowing all events (press, repeat, release) for the armed key prevents
 *   CTvApplicationManager::m_OnKeyDirectMBR from ever seeing the DVR button, eliminating
 *   the universal-remote setup prompt cleanly.
 *
 *   Audio switching calls TCSpeakerProxy::GetInstance(0)->GetExternalOutMode /
 *   SetExternalOutMode on a dedicated worker thread, matching Sound -> Additional Settings
 *   -> SPDIF Output -> Audio Format (0 = PCM, 1 = Dolby Digital, query = 0).
 *
 * Usage (inject resident into exeAPP):
 *   samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so KEY:DVR
 *   samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so KEY:0x72
 *   samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so LOG
 *   samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so PCM|DD|TOGGLE
 *   samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so OFF
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "samygo.h"

#define VERSION  "2.0.0"
#define LOG_PATH "/dtv/ArcToggle.log"
#define say(...) sgo_log(LOG_PATH, __VA_ARGS__)

/* Verified addresses in T-MST12AKUC-2225.0 exeAPP. */
#define VTABLE_ADDR    0x04996838u /* _ZTV19CRemoteControlInput */
#define VTABLE_PRI_OFF 0x30u       /* primary ProcessRemoteControlEvent slot */
#define VTABLE_SEC_OFF 0x44u       /* IRemoteControlInput subobject (this+20) slot */
#define RC_EVENT_ADDR  0x01ed2720u /* CRemoteControlInput::ProcessRemoteControlEvent */
#define RC_THN20_ADDR  0x01ed2718u /* non-virtual thunk to ProcessRemoteControlEvent */
#define SPKR_INST_ADDR 0x0111d4e8u /* TCSpeakerProxy::GetInstance(ESpeaker) */
#define SPKR_SET_ADDR  0x011185e0u /* TCSpeakerProxy::SetExternalOutMode(int, int) */
#define SPKR_GET_ADDR  0x0111bd30u /* TCSpeakerProxy::GetExternalOutMode(int *, int) */
#define THN20_OFFSET   20u

#define KEY_DVR        0x0072u /* 114: Samsung KEY_DVR (triggers MBR setup if unhandled) */

/* Event types passed to CRemoteControlInput::ProcessRemoteControlEvent. */
#define EV_RAW_PRESS   11u
#define EV_RAW_RELEASE 12u
#define EV_KEY_PRESS   20u
#define EV_KEY_RELEASE 21u

/* Held-key repeats arrive every ~100-250 ms; if a release is ever missed, re-arm after this. */
#define REPEAT_TIMEOUT_US 600000ll
#define LOG_LIMIT         64

/* TPCExternalOut::EAudioType */
#define AUDIO_PCM    0
#define AUDIO_DOLBY  1
#define AUDIO_MAX    3
#define SPEAKER_MAIN 0
#define SEEK_APPLY   0

enum { JOB_NONE = 0, JOB_TOGGLE, JOB_PCM, JOB_DOLBY };

typedef int   rc_event_fn(void *self, unsigned long type, uint16_t keycode, void *param);
typedef void *speaker_inst_fn(int speaker);
typedef int   speaker_set_fn(void *self, int mode, int seek);
typedef int   speaker_get_fn(void *self, int *mode, int seek);

static const struct {
    const char *symbol;
    uintptr_t   addr;
} g_firmware[] = {
    { "_ZTV19CRemoteControlInput", VTABLE_ADDR },
    { "_ZN19CRemoteControlInput25ProcessRemoteControlEventEmtP18RemoteControlParam", RC_EVENT_ADDR },
    { "_ZThn20_N19CRemoteControlInput25ProcessRemoteControlEventEmtP18RemoteControlParam", RC_THN20_ADDR },
    { "_ZN14TCSpeakerProxy11GetInstanceEN9TCSpeaker8ESpeakerE", SPKR_INST_ADDR },
    { "_ZN14TCSpeakerProxy18SetExternalOutModeEii", SPKR_SET_ADDR },
    { "_ZN14TCSpeakerProxy18GetExternalOutModeEPii", SPKR_GET_ADDR },
};

static bool g_hooked;

/* Configuration shared between lib_init and the remote-input thread. */
static volatile int g_key = -1;
static volatile int g_log;
static volatile int g_logged;

/* Repeat-suppression state for the armed key (input thread only). */
static bool      g_armed_down;
static long long g_armed_us;

/* Worker-thread state for non-blocking vdbinder audio switches. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_job;
static bool            g_busy;

struct ktimespec {
    long sec;
    long nsec;
};

/* Raw ARM EABI clock_gettime(CLOCK_MONOTONIC) avoids glibc 2.38 64-bit time redirects. */
static long long mono_us(void)
{
    struct ktimespec ts = { 0, 0 };
    register long    r0 __asm__("r0") = 1; /* CLOCK_MONOTONIC */
    register long    r1 __asm__("r1") = (long)&ts;
    register long    r7 __asm__("r7") = 263; /* __NR_clock_gettime */
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return (long long)ts.sec * 1000000ll + ts.nsec / 1000ll;
}

static const char *audio_name(int mode)
{
    switch (mode) {
    case 0: return "PCM";
    case 1: return "Dolby";
    case 2: return "DTS";
    case 3: return "DTS Neo:6";
    default: return "unknown";
    }
}

static bool firmware_matches(void)
{
    void *self = dlopen(NULL, RTLD_LAZY);
    if (!self)
        return false;

    int count = (int)(sizeof g_firmware / sizeof g_firmware[0]);
    int matches = 0;
    while (matches < count &&
           (uintptr_t)dlsym(self, g_firmware[matches].symbol) == g_firmware[matches].addr)
        matches++;
    if (matches < count)
        say("unsupported process/firmware: %s is not at 0x%08lx (inject into exeAPP)",
            g_firmware[matches].symbol, (unsigned long)g_firmware[matches].addr);
    dlclose(self);
    return matches == count;
}

static void apply(int job)
{
    speaker_inst_fn *get_instance = (speaker_inst_fn *)SPKR_INST_ADDR;
    speaker_get_fn  *get_mode = (speaker_get_fn *)SPKR_GET_ADDR;
    speaker_set_fn  *set_mode = (speaker_set_fn *)SPKR_SET_ADDR;

    void *speaker = get_instance(SPEAKER_MAIN);
    if (!speaker) {
        say("TCSpeakerProxy::GetInstance(0) returned NULL");
        return;
    }

    int before = -1;
    int grc = get_mode(speaker, &before, SEEK_APPLY);
    if (before < 0 || before > AUDIO_MAX) {
        say("read failed rc=%d mode=%d", grc, before);
        return;
    }

    int next = job == JOB_DOLBY ? AUDIO_DOLBY : AUDIO_PCM;
    if (job == JOB_TOGGLE)
        next = before == AUDIO_PCM ? AUDIO_DOLBY : AUDIO_PCM;

    long long t0 = mono_us();
    int       rc = set_mode(speaker, next, SEEK_APPLY);
    long long took_ms = (mono_us() - t0) / 1000;
    int       after = -1;
    get_mode(speaker, &after, SEEK_APPLY);
    say("%s -> %s set=%d now=%s %ldms", audio_name(before), audio_name(next), rc,
        audio_name(after), (long)took_ms);
}

static void *worker(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int job = g_job;
        g_job = JOB_NONE;
        if (job == JOB_NONE) {
            g_busy = false;
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        pthread_mutex_unlock(&g_lock);
        apply(job);
    }
}

static void request(int job)
{
    pthread_mutex_lock(&g_lock);
    g_job = job;
    bool start = !g_busy;
    if (start)
        g_busy = true;
    pthread_mutex_unlock(&g_lock);
    if (!start)
        return;

    pthread_t thread;
    int       error = pthread_create(&thread, NULL, worker, NULL);
    if (error) {
        pthread_mutex_lock(&g_lock);
        g_busy = false;
        pthread_mutex_unlock(&g_lock);
        say("cannot start worker thread: %s", strerror(error));
        return;
    }
    pthread_detach(thread);
}

static int on_rc_event(void *self, unsigned long type, uint16_t keycode, void *param)
{
    if (g_log) {
        if (g_logged < LOG_LIMIT) {
            g_logged++;
            say("key 0x%04x (%u) type=%lu", (unsigned)keycode, (unsigned)keycode, type);
        } else if (g_logged == LOG_LIMIT) {
            g_logged++;
            say("key log full");
        }
    }

    int armed = g_key;
    if (armed >= 0 && (int)keycode == armed) {
        if (type == EV_RAW_RELEASE || type == EV_KEY_RELEASE) {
            g_armed_down = false;
        } else if (type == EV_RAW_PRESS || type == EV_KEY_PRESS) {
            long long now = mono_us();
            bool      fresh = !g_armed_down || (now - g_armed_us) >= REPEAT_TIMEOUT_US;
            g_armed_down = true;
            g_armed_us = now;
            if (fresh)
                request(JOB_TOGGLE);
        }
        /* Swallow press, repeat, and release so MBR / universal-remote prompt never runs. */
        return 1;
    }

    return ((rc_event_fn *)RC_EVENT_ADDR)(self, type, keycode, param);
}

static int on_rc_event_thn20(void *subobj, unsigned long type, uint16_t keycode, void *param)
{
    return on_rc_event((char *)subobj - THN20_OFFSET, type, keycode, param);
}

static bool vtable_poke(uintptr_t *pri, uintptr_t pri_val, uintptr_t *sec, uintptr_t sec_val)
{
    size_t    pagesz = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t)pri < (uintptr_t)sec ? (uintptr_t)pri : (uintptr_t)sec;
    uintptr_t hi = (uintptr_t)pri > (uintptr_t)sec ? (uintptr_t)pri : (uintptr_t)sec;
    uintptr_t start = lo & ~(uintptr_t)(pagesz - 1);
    size_t    len = (((hi + sizeof(uintptr_t)) - start) + pagesz - 1) & ~(pagesz - 1);

    /* _ZTV19CRemoteControlInput sits in exeAPP's R-X segment, so keep PROT_EXEC while patching. */
    if (mprotect((void *)start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        say("vtable mprotect: %s", strerror(errno));
        return false;
    }
    __atomic_store_n(pri, pri_val, __ATOMIC_RELEASE);
    __atomic_store_n(sec, sec_val, __ATOMIC_RELEASE);
    mprotect((void *)start, len, PROT_READ | PROT_EXEC);
    return true;
}

static bool hook_install(void)
{
    if (g_hooked)
        return true;

    uintptr_t *pri = (uintptr_t *)(VTABLE_ADDR + VTABLE_PRI_OFF);
    uintptr_t *sec = (uintptr_t *)(VTABLE_ADDR + VTABLE_SEC_OFF);
    if (*pri != RC_EVENT_ADDR || *sec != RC_THN20_ADDR) {
        say("vtable mismatch: pri=0x%08lx sec=0x%08lx, left alone", (unsigned long)*pri, (unsigned long)*sec);
        return false;
    }
    if (!vtable_poke(pri, (uintptr_t)on_rc_event, sec, (uintptr_t)on_rc_event_thn20))
        return false;

    g_hooked = true;
    say("CRemoteControlInput hooked");
    return true;
}

static void hook_remove(void)
{
    if (!g_hooked)
        return;

    uintptr_t *pri = (uintptr_t *)(VTABLE_ADDR + VTABLE_PRI_OFF);
    uintptr_t *sec = (uintptr_t *)(VTABLE_ADDR + VTABLE_SEC_OFF);
    if (*pri != (uintptr_t)on_rc_event || *sec != (uintptr_t)on_rc_event_thn20) {
        say("vtable slots changed externally, left alone");
        g_hooked = false;
        return;
    }
    if (!vtable_poke(pri, RC_EVENT_ADDR, sec, RC_THN20_ADDR))
        return;

    g_hooked = false;
    g_armed_down = false;
    say("CRemoteControlInput unhooked");
}

static bool parse_key(const char *text, int *out)
{
    if (strcasecmp(text, "DVR") == 0) {
        *out = KEY_DVR;
        return true;
    }

    unsigned value = 0;
    bool     hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hex)
        text += 2;
    if (!*text)
        return false;
    for (; *text; text++) {
        unsigned digit;
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (hex && *text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a' + 10);
        else if (hex && *text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A' + 10);
        else
            return false;
        value = hex ? (value << 4) | digit : value * 10u + digit;
        if (value > 65535u)
            return false;
    }
    if (value < 1u)
        return false;
    *out = (int)value;
    return true;
}

enum { ONCE_NONE, ONCE_PCM, ONCE_DOLBY, ONCE_TOGGLE };

void lib_init(void *handle, const char *libpath)
{
    (void)handle;

    const char *argv[8];
    int         argc = sgo_args(libpath, argv, 8);
    if (argc > 8) {
        say("libArcToggle " VERSION ": too many arguments, nothing done");
        return;
    }

    bool off = false;
    bool log_keys = false;
    bool saw_key = false;
    int  key = 0;
    int  once = ONCE_NONE;

    for (int i = 0; i < argc; i++) {
        const char *word = argv[i];
        int         chosen = ONCE_NONE;
        if (strcasecmp(word, "OFF") == 0)
            off = true;
        else if (strcasecmp(word, "LOG") == 0)
            log_keys = true;
        else if (strcasecmp(word, "PCM") == 0)
            chosen = ONCE_PCM;
        else if (strcasecmp(word, "DD") == 0 || strcasecmp(word, "DOLBY") == 0)
            chosen = ONCE_DOLBY;
        else if (strcasecmp(word, "TOGGLE") == 0)
            chosen = ONCE_TOGGLE;
        else if (strncasecmp(word, "KEY:", 4) == 0) {
            if (!parse_key(word + 4, &key)) {
                say("libArcToggle " VERSION ": bad argument '%s', nothing done", word);
                return;
            }
            saw_key = true;
        } else {
            say("libArcToggle " VERSION ": bad argument '%s', nothing done", word);
            return;
        }
        if (chosen != ONCE_NONE) {
            if (once != ONCE_NONE) {
                say("libArcToggle " VERSION ": conflicting format arguments, nothing done");
                return;
            }
            once = chosen;
        }
    }
    if (off && (log_keys || saw_key || once != ONCE_NONE)) {
        say("libArcToggle " VERSION ": OFF takes no other arguments, nothing done");
        return;
    }

    if (!firmware_matches())
        return;

    if (off) {
        g_key = -1;
        g_log = 0;
        hook_remove();
        return;
    }

    /* No words defaults to KEY:DVR (0x72). LOG alone logs without arming a toggle. */
    if (argc == 0) {
        saw_key = true;
        key = KEY_DVR;
    }
    bool watch = saw_key || log_keys;
    if (watch) {
        g_key = saw_key ? key : -1;
        g_armed_down = false;
        g_log = log_keys ? 1 : 0;
        if (log_keys)
            g_logged = 0;
        if (!hook_install() && once == ONCE_NONE)
            return;
    }

    if (once == ONCE_PCM)
        request(JOB_PCM);
    else if (once == ONCE_DOLBY)
        request(JOB_DOLBY);
    else if (once == ONCE_TOGGLE)
        request(JOB_TOGGLE);

    if (g_hooked && watch) {
        if (g_key >= 0)
            say("libArcToggle " VERSION ": armed key 0x%04x (%d)%s", g_key, g_key,
                log_keys ? " + logging" : "");
        else
            say("libArcToggle " VERSION ": logging keys");
    } else if (once != ONCE_NONE) {
        say("libArcToggle " VERSION ": once");
    }
}

void lib_deinit(void *handle)
{
    (void)handle;
}
