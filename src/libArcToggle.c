/*
 * libArcToggle — one remote key swaps PCM and Dolby Digital on ARC.
 *
 * The Sound menu's Audio Format line is TCSpeakerImpl::SetExternalOutMode.
 * That call ignores its receiver and uses external-out info 0. The value is
 * TPCExternalOut::EAudioType: 0 PCM, 1 Dolby Digital, 2 DTS, 3 DTS Neo:6.
 * ExternalOutControl::SetAudioMode programs the output when its query is 0
 * and returns without programming it otherwise, so this payload uses 0.
 *
 * Brightness-sensor frames (da 14/15/16) arrive on t_ProcessNormalBuf and
 * are not remote keys. Parsed micom packets go to t_ParsePacket. LOG prints
 * the command byte, skipping the sensor command da.
 *
 *   samyGOso -T -B -r -l libArcToggle.so
 *   samyGOso -T -B -r -l libArcToggle.so LOG
 *   samyGOso -T -B -r -l libArcToggle.so KEY:0x49
 *   samyGOso -T -B -l libArcToggle.so PCM|DD|TOGGLE
 *   samyGOso -T -B -r -l libArcToggle.so OFF
 *
 * The same library, injected into exeAPP, turns the universal-remote setup
 * prompt off. That dialog is RVUCKey::CRVUCNotAvailableKey::ShowNAPopUpMessage.
 * The DVR key still toggles audio in exeTV; exeAPP just no longer asks to set
 * the button up. OFF in exeAPP puts the prompt back.
 *
 * Inject a hook resident (-r). The library is linked so it is never unmapped:
 * the key thread and the worker outlive lib_init. Results go to
 * /dtv/ArcToggle.log.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "samygo.h"

#define VERSION      "1.4.0"
#define LOG_PATH     "/dtv/ArcToggle.log"
#define EXETV_BASE   0x8000u
#define BRANCH_REACH 0x02000000u
#define INSN_LDR_PC  0xe51ff004u
#define INSN_B       0xea000000u
#define KEY_ADDR     0x011106b4u
#define KEY_INSN     0xe92d4810u /* push {r4, fp, lr} */
#define SET_ADDR     0x00af9a1cu /* TCSpeakerImpl::SetExternalOutMode(int, int) */
#define GET_ADDR     0x00af9a74u /* TCSpeakerImpl::GetExternalOutMode(int *, int) */
#define AUDIO_PCM    0
#define AUDIO_DOLBY  1
#define AUDIO_MAX    3
#define SEEK_APPLY   0
/*
 * Repeats of one press keep arriving for a few seconds, with gaps long enough
 * that a short window fires twice and the format flips back. Anything sooner
 * than this after the previous armed packet is the same press.
 */
#define QUIET_US     3000000
/* A keyboard report of the same press can arrive beside the micom packet. */
#define HID_WINDOW   2000000
#define HID_BEFORE   500000
#define LOG_LIMIT    60
/*
 * exeAPP shows "this button will only work after the universal remote control
 * has been set up" from this function. The first instruction is pc-independent,
 * so replacing it with bx lr returns before CreateNAMessage runs.
 */
#define POPUP_ADDR   0x02c3d200u
#define POPUP_INSN   0xe92d4800u /* push {fp, lr} */
#define POPUP_RET    0xe12fff1eu /* bx lr */
#define POPUP_SYM    "_ZN7RVUCKey20CRVUCNotAvailableKey18ShowNAPopUpMessageEv"
#define HID_ADDR     0x00f9cc8cu
#define HID_INSN     0xe92d4870u /* push {r4, r5, r6, fp, lr} */
#define HID_SYM      "_ZN11USBHIDINPUT21CKeyboardEventHandler24m_SendKeyboardInputEventEmi"
/* exeAPP looks up the setup sentence through this getter. */
#define STR_ADDR     0x016c2f98u
#define STR_INSN0    0xe92d48f0u /* push {r4, r5, r6, r7, fp, lr} */
#define STR_INSN1    0xe28db014u /* add fp, sp, #20 */
#define STR_TEXT     0x05f1076cu
#define STR_SYM      "_ZN16CResourceManager9GetStringEi"

#define say(...) sgo_log(LOG_PATH, __VA_ARGS__)

enum { STUB_ENTRY = 0, STUB_RESUME = 2, STUB_HID = 6, STUB_HID_RESUME = 8 };
enum { JOB_NONE = 0, JOB_TOGGLE, JOB_PCM, JOB_DOLBY };

typedef int set_mode_fn(void *self, int mode, int seek);
typedef int get_mode_fn(void *self, int *mode, int seek);
typedef int key_fn(void *self, const unsigned char *packet);
typedef void hid_fn(void *self, unsigned long key, int extra);

static long long now_us(void);

static const struct {
    const char *symbol;
    uintptr_t   addr;
} g_firmware[] = {
    { "_ZN22TDcPrimeMicomInterface13t_ParsePacketEPh", KEY_ADDR },
    { "_ZN13TCSpeakerImpl18SetExternalOutModeEii", SET_ADDR },
    { "_ZN13TCSpeakerImpl18GetExternalOutModeEPii", GET_ADDR },
};

static set_mode_fn *g_set_mode;
static get_mode_fn *g_get_mode;

static uint32_t *g_stub;
static bool      g_hooked;
static bool      g_hid_hooked;

/* Written from lib_init, read from the key thread. Aligned word stores. */
static volatile int  g_key = -1;
static volatile int  g_log;
static volatile int  g_logged;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_job;
static bool            g_busy;

/* Last armed packet, so a burst toggles once. Other commands do not touch it. */
static long long g_armed_us;
static bool      g_armed_seen;

/* Keyboard usage that arrived with the armed key. Later reports of it are dropped. */
static int       g_last_hid = -1;
static long long g_last_hid_us;
static int       g_block_hid = -1;
static int       g_hid_seen[12];
static int       g_hid_seen_n;

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
    void *exetv = dlopen(NULL, RTLD_LAZY);
    if (!exetv)
        return false;

    int matches = 0;
    int count = (int)(sizeof g_firmware / sizeof g_firmware[0]);
    while (matches < count &&
           (uintptr_t)dlsym(exetv, g_firmware[matches].symbol) == g_firmware[matches].addr)
        matches++;
    if (matches < count)
        say("unsupported firmware: %s is not at 0x%08lx", g_firmware[matches].symbol,
            (unsigned long)g_firmware[matches].addr);
    dlclose(exetv);
    if (matches < count)
        return false;
    if (*(uint32_t *)KEY_ADDR != KEY_INSN && !g_hooked) {
        say("t_ParsePacket starts with %08x, left alone", *(unsigned *)KEY_ADDR);
        return false;
    }
    g_set_mode = (set_mode_fn *)SET_ADDR;
    g_get_mode = (get_mode_fn *)GET_ADDR;
    return true;
}

static bool read_mode(int *mode)
{
    *mode = -1;
    int rc = g_get_mode(NULL, mode, SEEK_APPLY);
    if (*mode < 0 || *mode > AUDIO_MAX) {
        say("read failed rc=%d mode=%d", rc, *mode);
        return false;
    }
    return true;
}

static void apply(int job)
{
    int before = -1;
    if (!read_mode(&before))
        return;

    int next = job == JOB_DOLBY ? AUDIO_DOLBY : AUDIO_PCM;
    if (job == JOB_TOGGLE)
        next = before == AUDIO_PCM ? AUDIO_DOLBY : AUDIO_PCM;

    long long t0 = now_us();
    int rc = g_set_mode(NULL, next, SEEK_APPLY);
    long long took = (now_us() - t0) / 1000;
    int after = -1;
    g_get_mode(NULL, &after, SEEK_APPLY);
    say("%s -> %s set=%d now=%s %ldms", audio_name(before), audio_name(next), rc,
        audio_name(after), (long)took);
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
    int error = pthread_create(&thread, NULL, worker, NULL);
    if (error) {
        pthread_mutex_lock(&g_lock);
        g_busy = false;
        pthread_mutex_unlock(&g_lock);
        say("cannot start a thread: %s", strerror(error));
        return;
    }
    pthread_detach(thread);
}

/* The toolchain's gettimeofday binds __gettimeofday64, which this glibc lacks. */
struct ktime {
    long sec;
    long usec;
};

static long long now_us(void)
{
    struct ktime tv = {0, 0};
    register long r0 __asm__("r0") = (long)&tv;
    register long r1 __asm__("r1") = 0;
    register long r7 __asm__("r7") = 78; /* __NR_gettimeofday */
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return (long long)tv.sec * 1000000 + tv.usec;
}

/* Command da is the brightness sensor. Anything else is logged once. */
static unsigned char g_seen[256];

/*
 * The armed key. One press is a burst of the same command. The first packet
 * requests the switch; the rest are eaten. The frame is cleared so the rest
 * of the micom path has nothing to turn into the universal-control prompt,
 * and the original parser is not called.
 */
static int on_key(void *self, const unsigned char *packet)
{
    unsigned cmd = packet ? packet[0] : 0;
    unsigned b1 = packet ? packet[1] : 0;
    unsigned b2 = packet ? packet[2] : 0;
    unsigned b3 = packet ? packet[3] : 0;
    if (!g_seen[cmd]) {
        g_seen[cmd] = 1;
        say("cmd %02x", cmd);
    }
    if (cmd != 0xda && g_log && g_logged < LOG_LIMIT) {
        g_logged++;
        say("pkt %02x %02x %02x %02x", cmd, b1, b2, b3);
    } else if (cmd != 0xda && g_log && g_logged == LOG_LIMIT) {
        g_logged++;
        say("key log full");
    }
    if (g_key >= 0 && (int)cmd == g_key) {
        long long now = now_us();
        bool fresh = !g_armed_seen || now - g_armed_us >= QUIET_US;
        g_armed_seen = true;
        g_armed_us = now;
        if (g_last_hid >= 0 && now - g_last_hid_us < HID_BEFORE && g_last_hid != g_block_hid) {
            g_block_hid = g_last_hid;
            say("hid block %lx", (unsigned long)g_block_hid);
        }
        if (fresh)
            request(JOB_TOGGLE);
        if (packet) {
            unsigned char *frame = (unsigned char *)packet;
            memset(frame, 0, 7);
        }
        return 0;
    }
    return ((key_fn *)(g_stub + STUB_RESUME))(self, packet);
}

/* The same press is also a USB keyboard usage. Drop that usage so exeAPP never sees it. */
static void on_hid(void *self, unsigned long key, int extra)
{
    long long now = now_us();
    int i, seen = 0;
    for (i = 0; i < g_hid_seen_n; i++) {
        if (g_hid_seen[i] == (int)key)
            seen = 1;
    }
    if (!seen && g_hid_seen_n < (int)(sizeof g_hid_seen / sizeof g_hid_seen[0])) {
        g_hid_seen[g_hid_seen_n++] = (int)key;
        say("hid %lx", key);
    }
    g_last_hid = (int)key;
    g_last_hid_us = now;
    if (g_block_hid >= 0 && (int)key == g_block_hid)
        return;
    if (g_armed_seen && now - g_armed_us < HID_WINDOW) {
        g_block_hid = (int)key;
        say("hid block %lx", key);
        return;
    }
    ((hid_fn *)(g_stub + STUB_HID_RESUME))(self, key, extra);
}

static uint32_t *map_stub_page(size_t size, uintptr_t lowest, uintptr_t highest)
{
    FILE         *maps = fopen("/proc/self/maps", "r");
    unsigned long start, end, gap = 0;
    void         *page = NULL;
    if (!maps)
        return NULL;

    while (!page && fscanf(maps, "%lx-%lx%*[^\n]", &start, &end) == 2) {
        if (gap >= EXETV_BASE && start - gap >= size && gap + size <= lowest + BRANCH_REACH &&
            highest + 8 <= gap + BRANCH_REACH) {
            page = mmap((void *)gap, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page != (void *)gap) {
                if (page != MAP_FAILED)
                    munmap(page, size);
                page = NULL;
            }
        }
        gap = end;
    }
    fclose(maps);
    return page;
}

static bool text_poke(uint32_t *at, uint32_t insn)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    void  *page = (void *)((uintptr_t)at & ~(uintptr_t)(size - 1));

    if (mprotect(page, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    *at = insn;
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    mprotect(page, size, PROT_READ | PROT_EXEC);
    return true;
}

static uint32_t branch_to(const uint32_t *at, const uint32_t *stub)
{
    return INSN_B | ((((uintptr_t)stub - (uintptr_t)(at + 2)) >> 2) & 0x00ffffffu);
}

static bool hook_install(void)
{
    if (g_hooked)
        return true;

    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uint32_t *page = g_stub;
    if (!page) {
        page = map_stub_page(size, KEY_ADDR, HID_ADDR);
        if (!page) {
            say("no stub page within branch reach");
            return false;
        }
        page[STUB_ENTRY] = INSN_LDR_PC;
        page[STUB_ENTRY + 1] = (uint32_t)(uintptr_t)on_key;
        page[STUB_RESUME] = KEY_INSN;
        page[STUB_RESUME + 1] = INSN_LDR_PC;
        page[STUB_RESUME + 2] = KEY_ADDR + 4;
        {
            void *self = dlopen(NULL, RTLD_LAZY);
            void *sym = self ? dlsym(self, HID_SYM) : NULL;
            if (self)
                dlclose(self);
            if ((uintptr_t)sym == HID_ADDR && *(uint32_t *)HID_ADDR == HID_INSN) {
                page[STUB_HID] = INSN_LDR_PC;
                page[STUB_HID + 1] = (uint32_t)(uintptr_t)on_hid;
                page[STUB_HID_RESUME] = HID_INSN;
                page[STUB_HID_RESUME + 1] = INSN_LDR_PC;
                page[STUB_HID_RESUME + 2] = HID_ADDR + 4;
            }
        }
        if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
            say("stub mprotect: %s", strerror(errno));
            munmap(page, size);
            return false;
        }
        __builtin___clear_cache((char *)page, (char *)page + size);
        g_stub = page;
    }

    uint32_t *at = (uint32_t *)KEY_ADDR;
    if (*at != KEY_INSN) {
        say("found %08x instead of %08x, left alone", *at, KEY_INSN);
        g_stub = NULL;
        munmap(page, size);
        return false;
    }
    if (!text_poke(at, branch_to(at, page))) {
        g_stub = NULL;
        say("cannot patch: %s", strerror(errno));
        munmap(page, size);
        return false;
    }
    g_hooked = true;
    if (page[STUB_HID] == INSN_LDR_PC) {
        uint32_t *hid = (uint32_t *)HID_ADDR;
        if (*hid == HID_INSN && text_poke(hid, branch_to(hid, page + STUB_HID))) {
            g_hid_hooked = true;
            say("hid listening");
        }
    }
    say("listening");
    return true;
}

static void hook_remove(void)
{
    if (g_hid_hooked) {
        uint32_t *hid = (uint32_t *)HID_ADDR;
        if (*hid == branch_to(hid, g_stub + STUB_HID) && text_poke(hid, HID_INSN))
            g_hid_hooked = false;
    }
    if (!g_hooked)
        return;
    uint32_t *at = (uint32_t *)KEY_ADDR;
    if (*at != branch_to(at, g_stub)) {
        say("entry is %08x, not our hook", *at);
        g_hooked = false;
        return;
    }
    if (!text_poke(at, KEY_INSN)) {
        say("cannot unhook: %s", strerror(errno));
        return;
    }
    g_hooked = false;
    say("unhooked");
}

/* True when this process is exeAPP and owns the setup prompt. */
static bool popup_here(uint32_t **at)
{
    void *self = dlopen(NULL, RTLD_LAZY);
    void *sym;
    *at = NULL;
    if (!self)
        return false;
    sym = dlsym(self, POPUP_SYM);
    dlclose(self);
    if (!sym)
        return false;
    if ((uintptr_t)sym != POPUP_ADDR) {
        say("popup is at %08lx, left alone", (unsigned long)(uintptr_t)sym);
        return true;
    }
    *at = sym;
    return true;
}

static void popup_silence(uint32_t *at)
{
    if (*at == POPUP_RET) {
        say("popup quiet");
        return;
    }
    if (*at != POPUP_INSN) {
        say("popup starts with %08x, left alone", *at);
        return;
    }
    if (!text_poke(at, POPUP_RET)) {
        say("cannot quiet popup: %s", strerror(errno));
        return;
    }
    say("popup quiet");
}

static void popup_restore(uint32_t *at)
{
    if (!at)
        return;
    if (*at == POPUP_INSN) {
        say("popup restored");
        return;
    }
    if (*at != POPUP_RET) {
        say("popup entry is %08x, left alone", *at);
        return;
    }
    if (!text_poke(at, POPUP_INSN)) {
        say("cannot restore popup: %s", strerror(errno));
        return;
    }
    say("popup restored");
}

static uint32_t g_str_stub[4] __attribute__((aligned(16)));
static bool g_str_hooked;
static const unsigned short g_blank_text = 0;

typedef void *str_fn(void *self, int id);

/* Replace the setup sentence with an empty one and name the caller. */
static void *on_str(void *self, int id)
{
    void *text = ((str_fn *)g_str_stub)(self, id);
    if ((uintptr_t)text == STR_TEXT) {
        say("prompt id %d from %08lx", id, (unsigned long)__builtin_return_address(0));
        return (void *)&g_blank_text;
    }
    return text;
}

static bool text_poke2(uint32_t *at, uint32_t first, uint32_t second)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t span = (uintptr_t)(at + 2) - ((uintptr_t)at & ~(uintptr_t)(size - 1));
    void *page = (void *)((uintptr_t)at & ~(uintptr_t)(size - 1));
    size_t bytes = (span + size - 1) & ~(size - 1);

    if (mprotect(page, bytes, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    at[0] = first;
    at[1] = second;
    __builtin___clear_cache((char *)at, (char *)(at + 2));
    mprotect(page, bytes, PROT_READ | PROT_EXEC);
    return true;
}

static void string_hook(void)
{
    void *self = dlopen(NULL, RTLD_LAZY);
    void *sym = self ? dlsym(self, STR_SYM) : NULL;
    uint32_t *at = (uint32_t *)STR_ADDR;
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    void *stub_page = (void *)((uintptr_t)g_str_stub & ~(uintptr_t)(size - 1));
    if (self)
        dlclose(self);
    if ((uintptr_t)sym != STR_ADDR || at[0] != STR_INSN0 || at[1] != STR_INSN1) {
        say("string getter left alone");
        return;
    }
    g_str_stub[0] = STR_INSN0;
    g_str_stub[1] = STR_INSN1;
    g_str_stub[2] = INSN_LDR_PC;
    g_str_stub[3] = STR_ADDR + 8;
    if (mprotect(stub_page, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        say("string stub: %s", strerror(errno));
        return;
    }
    __builtin___clear_cache((char *)g_str_stub, (char *)(g_str_stub + 4));
    if (!text_poke2(at, INSN_LDR_PC, (uint32_t)(uintptr_t)on_str)) {
        say("cannot hook prompt text: %s", strerror(errno));
        return;
    }
    g_str_hooked = true;
    say("prompt text hooked");
}

static void string_unhook(void)
{
    uint32_t *at = (uint32_t *)STR_ADDR;
    if (!g_str_hooked)
        return;
    if (at[0] == INSN_LDR_PC && at[1] == (uint32_t)(uintptr_t)on_str &&
        text_poke2(at, STR_INSN0, STR_INSN1)) {
        g_str_hooked = false;
        say("prompt text restored");
    }
}

static bool parse_u8(const char *text, int *out)
{
    unsigned value = 0;
    bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
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
        if (!hex && digit > 9)
            return false;
        value = hex ? (value << 4) | digit : value * 10u + digit;
        if (value > 255)
            return false;
    }
    if (value < 1)
        return false;
    *out = (int)value;
    return true;
}

enum { ONCE_NONE, ONCE_PCM, ONCE_DOLBY, ONCE_TOGGLE };

void lib_init(void *handle, const char *libpath)
{
    (void)handle;

    const char *argv[8];
    int argc = sgo_args(libpath, argv, 8);
    if (argc > 8) {
        say("libArcToggle " VERSION ": too many arguments, nothing done");
        return;
    }

    bool off = false;
    bool log_keys = false;
    bool saw_key = false;
    int key = 0;
    int once = ONCE_NONE;

    for (int i = 0; i < argc; i++) {
        const char *word = argv[i];
        int chosen = ONCE_NONE;
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
            if (!parse_u8(word + 4, &key)) {
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

    {
        uint32_t *popup = NULL;
        if (popup_here(&popup)) {
            if (off) {
                popup_restore(popup);
                string_unhook();
            } else {
                popup_silence(popup);
                string_hook();
            }
            return;
        }
    }

    if (!firmware_matches())
        return;

    if (off) {
        hook_remove();
        return;
    }

    /* No words, or LOG, only prints keys. KEY: arms the toggle. */
    if (argc == 0)
        log_keys = true;
    bool watch = argc == 0 || saw_key || log_keys;
    if (watch) {
        g_key = saw_key ? key : -1;
        g_log = log_keys;
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
            say("libArcToggle " VERSION " key %02x%s", g_key, log_keys ? " logging" : "");
        else
            say("libArcToggle " VERSION " logging keys");
    } else if (once != ONCE_NONE)
        say("libArcToggle " VERSION " once");
}

void lib_deinit(void *handle)
{
    (void)handle;
}
