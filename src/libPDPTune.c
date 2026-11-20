/*
 * libPDPTune - picture tuning for the Samsung PN60F5500 plasma.
 *
 * A samyGOso payload for exeTV of firmware T-MST12AKUC-2225.0; it leaves any
 * other build alone. The words of one injection are the whole configuration,
 * and what is not named goes back to stock:
 *
 *   UNLOCK                  offer 10p White Balance in the picture menu
 *   DRIVE:STANDARD          drive Movie and Relax with the waveform the
 *                           firmware uses for Dynamic, Standard and Natural
 *   DEEPBLACK:ON|OFF        hold the logic board's DeepBlack on or off
 *   PEAKSTRETCH:ON|OFF      hold the logic board's PeakStretch on or off
 *   DUTY2:<79..160>         replace this panel's fixed duty_2 of 128 (boosts
 *                           above 128 apply only when APC_OFFSET is 0)
 *   SUBCONTRAST:<100..160>  replace Sub Contrast (factory: 128 Movie, 132 Std)
 *   GAMMA:TOE|2.3|2.4|BT1886  compose a 12-bit EOTF curve onto exeTV's gamma
 *                           LUT in MDrv_PNL_SetGammaTbl (opt-in)
 *   OFF                     nothing: stock registers, hooks removed
 *   DUMP                    log the registers involved; changes nothing
 *
 * Inject it resident (samyGOso -r). Results go to /dtv/PDPTune.log.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#include "gamma_tables.h"
#include "samygo.h"

#define VERSION  "2.1.0"
#define LOG      "/dtv/PDPTune.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

/*
 * exeTV is not position independent, so a build is identified by where its
 * symbols are. Every address and constant below was read out of this build.
 */
enum { F_WRITE_I2C, F_READ_I2C, F_PANEL_TEMP, F_SUPPORT_OPTION, F_LOGIC_IC, F_LOGIC_FHD_VTABLE, F_COUNT };

static const struct {
    const char *symbol;
    uintptr_t   addr;
} g_firmware[F_COUNT] = {
    [F_WRITE_I2C]        = { "_ZN8LogicFHD16m_Logic_WriteI2CEtt", 0x006ece6c },
    [F_READ_I2C]         = { "_ZN8LogicFHD15m_Logic_ReadI2CEtPt", 0x006ec0d4 },
    [F_PANEL_TEMP]       = { "_ZN8LogicFHD20t_Logic_GetPanelTempEPi", 0x006ec3e0 },
    [F_SUPPORT_OPTION]   = { "_ZN14PrdInfoManager18CheckSupportOptionEN12IProductInfo22ProductSupportOption_kEPb",
                             0x00637550 },
    [F_LOGIC_IC]         = { "_ZN8TDcLogic11m_poLogicICE", 0x01f44b34 },
    [F_LOGIC_FHD_VTABLE] = { "_ZTV8LogicFHD", 0x018aea00 },
};

typedef int     write_i2c_fn(void *logic, uint16_t reg, uint16_t value);
typedef int     read_i2c_fn(void *logic, uint16_t reg, uint16_t *value);
typedef int     panel_temp_fn(void *logic, int *celsius);
typedef int     support_option_fn(void *self, int option, bool *supported);
typedef uint8_t set_gamma_fn(int lut_type, uint8_t **gamma_tab, int map_mode);

#define EXETV_BASE    0x8000u /* lowest address of the exeTV image */
#define OPTION_WB_10P 0x4a    /* IProductInfo::ProductSupportOption_k */
#define REG_APC_OFFSET 0x89c0 /* (1024 - duty) >> 1; 0 at full Cell Light 20 */

static bool firmware_matches(void)
{
    void *exetv = dlopen(NULL, RTLD_LAZY);
    int   matches = 0;
    if (!exetv)
        return false;

    while (matches < F_COUNT && (uintptr_t)dlsym(exetv, g_firmware[matches].symbol) == g_firmware[matches].addr)
        matches++;
    if (matches < F_COUNT)
        say("unsupported firmware: %s is not at 0x%08lx", g_firmware[matches].symbol,
            (unsigned long)g_firmware[matches].addr);
    dlclose(exetv);
    return matches == F_COUNT;
}

/*
 * Logic board registers this library can override. The firmware rewrites them
 * when the picture mode, the Black Optimizer or the cell light changes, so an
 * override has to sit in the write path to last.
 */
enum { K_DRIVE, K_DEEP_BLACK, K_PEAK_STRETCH, K_DUTY2, K_SUB_CONTRAST, K_COUNT };
enum { KEEP = -1, ANY = -1, UNKNOWN = -1 };

#define DRIVE_MOVIE     0x05 /* 2D Movie and Relax */
#define DRIVE_STANDARD  0x11 /* 2D Dynamic, Standard and Natural */
#define DEEP_BLACK_ON   3    /* Black Optimizer "Dark Room" */
#define PEAK_STRETCH_ON 1    /* Black Optimizer "Bright Room" */
#define DUTY2_FIXED     0x80 /* what a 60" panel gets whenever it is lit */

static struct knob {
    const char *name;
    uint16_t    reg;
    uint16_t    mask;  /* bits of a read-back that hold the value written */
    int         from;  /* the one firmware value that is replaced, or ANY */
    int         to;    /* its replacement, or KEEP */
    int         stock; /* what the firmware last asked for, or UNKNOWN */
    int         sent;  /* what the logic board was last given */
} g_knobs[K_COUNT] = {
    /* The logic board reports status in the high byte of DrivingMode. */
    [K_DRIVE]        = { "DrivingMode", 0x806f, 0x00ff, DRIVE_MOVIE, KEEP, UNKNOWN, UNKNOWN },
    [K_DEEP_BLACK]   = { "DeepBlack",   0x809c, 0xffff, ANY,         KEEP, UNKNOWN, UNKNOWN },
    [K_PEAK_STRETCH] = { "PeakStretch", 0x8063, 0xffff, ANY,         KEEP, UNKNOWN, UNKNOWN },
    [K_DUTY2]        = { "duty_2",      0x89da, 0x00ff, DUTY2_FIXED, KEEP, UNKNOWN, UNKNOWN },
    [K_SUB_CONTRAST] = { "SubContrast", 0x8927, 0xffff, ANY,         KEEP, UNKNOWN, UNKNOWN },
};

/* Serialises the workers and the knob state they share with the write hook. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_apc_offset = UNKNOWN;

static struct knob *knob_for(uint16_t reg)
{
    for (struct knob *knob = g_knobs; knob < g_knobs + K_COUNT; knob++)
        if (knob->reg == reg)
            return knob;
    return NULL;
}

static uint16_t tuned(const struct knob *knob, uint16_t stock)
{
    if (knob->to == KEEP || (knob->from != ANY && knob->from != stock))
        return stock;
    /* Respect Cell Light < 20, Eco, and thermal ABL: only boost duty_2 when APC_OFFSET == 0. */
    if (knob == &g_knobs[K_DUTY2] && knob->to > DUTY2_FIXED && g_apc_offset != 0)
        return stock;
    return (uint16_t)knob->to;
}

/*
 * Hooks. The first instruction of a hooked function is replaced by a branch,
 * which is a single aligned word store: a thread entering the function runs
 * either the old instruction or the new one, never a mix, so hooks can come
 * and go while exeTV runs.
 *
 * A branch reaches 32 MiB and this library is mapped much further away, so
 * the branch lands on a stub in a page mapped into a gap of the exeTV image:
 *
 *     entry:   ldr pc, [pc, #-4]     ; to the detour
 *              .word detour
 *     resume:  <first instruction>   ; the original function, past its hook
 *              ldr pc, [pc, #-4]
 *              .word function + 4
 */
#define INSN_LDR_PC_NEXT 0xe51ff004u /* ldr pc, [pc, #-4] */
#define INSN_B           0xea000000u
#define BRANCH_REACH     0x02000000u

enum { H_WRITE_I2C, H_SUPPORT_OPTION, H_SET_GAMMA, H_COUNT };
enum { STUB_ENTRY = 0, STUB_RESUME = 2, STUB_WORDS = 5 };

static write_i2c_fn      on_write_i2c;
static support_option_fn on_support_option;
static set_gamma_fn      on_set_gamma;

static struct hook {
    const char *name;
    int         function; /* F_* index, or -1 if resolved dynamically via symbol */
    const char *symbol;   /* used when function < 0 */
    uintptr_t   addr;
    uint32_t    insn; /* its first instruction, which must not depend on pc */
    void      (*detour)(void);
    uint32_t   *stub;
    bool        installed;
} g_hooks[H_COUNT] = {
    /* First two begin with push {r4, r5, r6, r7, fp, lr}; SetGammaTbl with push {r4..r8, sl, fp, lr}. */
    [H_WRITE_I2C]      = { "LogicFHD::m_Logic_WriteI2C", F_WRITE_I2C, NULL, 0, 0xe92d48f0,
                           (void (*)(void))on_write_i2c, NULL, false },
    [H_SUPPORT_OPTION] = { "PrdInfoManager::CheckSupportOption", F_SUPPORT_OPTION, NULL, 0, 0xe92d48f0,
                           (void (*)(void))on_support_option, NULL, false },
    [H_SET_GAMMA]      = { "MDrv_PNL_SetGammaTbl", -1, "MDrv_PNL_SetGammaTbl", 0, 0xe92d4df0,
                           (void (*)(void))on_set_gamma, NULL, false },
};

/*
 * Maps a page that a branch can reach from every address in [lowest, highest].
 * The first free one past the start of exeTV lies between the segments of
 * exeTV itself (or below libUTOPIA.so for gamma), where nothing else asks to
 * be mapped.
 */
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

static bool hook_resolve(struct hook *hook)
{
    if (hook->addr)
        return true;
    if (hook->function >= 0) {
        hook->addr = g_firmware[hook->function].addr;
        return true;
    }
    void *self = dlopen(NULL, RTLD_LAZY);
    if (!self)
        return false;
    hook->addr = (uintptr_t)dlsym(self, hook->symbol);
    dlclose(self);
    if (!hook->addr) {
        say("%s: symbol not found", hook->name);
        return false;
    }
    return true;
}

static void write_stub(uint32_t *stub, const struct hook *hook)
{
    stub[STUB_ENTRY] = INSN_LDR_PC_NEXT;
    stub[STUB_ENTRY + 1] = (uint32_t)(uintptr_t)hook->detour;
    stub[STUB_RESUME] = hook->insn;
    stub[STUB_RESUME + 1] = INSN_LDR_PC_NEXT;
    stub[STUB_RESUME + 2] = (uint32_t)(hook->addr + 4);
}

static bool hook_prepare(struct hook *hook)
{
    if (!hook_resolve(hook))
        return false;

    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    if (hook == &g_hooks[H_SET_GAMMA]) {
        uint32_t *page = map_stub_page(size, hook->addr, hook->addr);
        if (!page) {
            say("%s: no free page within branch reach", hook->name);
            return false;
        }
        write_stub(page, hook);
        if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
            say("%s: cannot make hook stub executable: %s", hook->name, strerror(errno));
            munmap(page, size);
            return false;
        }
        __builtin___clear_cache((char *)page, (char *)page + size);
        hook->stub = page;
        return true;
    }

    /* exeTV hooks share one stub page within branch reach of both. */
    uintptr_t lowest = UINTPTR_MAX, highest = 0;
    for (int i = 0; i < H_SET_GAMMA; i++) {
        hook_resolve(&g_hooks[i]);
        lowest = g_hooks[i].addr < lowest ? g_hooks[i].addr : lowest;
        highest = g_hooks[i].addr > highest ? g_hooks[i].addr : highest;
    }
    uint32_t *page = map_stub_page(size, lowest, highest);
    if (!page) {
        say("no free page within branch reach of the functions to hook");
        return false;
    }
    for (int i = 0; i < H_SET_GAMMA; i++)
        write_stub(page + i * STUB_WORDS, &g_hooks[i]);
    if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
        say("cannot make the hook stubs executable: %s", strerror(errno));
        munmap(page, size);
        return false;
    }
    __builtin___clear_cache((char *)page, (char *)page + size);
    for (int i = 0; i < H_SET_GAMMA; i++)
        g_hooks[i].stub = page + i * STUB_WORDS;
    return true;
}

static bool text_poke(uint32_t *at, uint32_t insn)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    void  *page = (void *)((uintptr_t)at & ~(uintptr_t)(size - 1));

    /* Other threads are executing this page, so it has to stay executable. */
    if (mprotect(page, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    *at = insn;
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    mprotect(page, size, PROT_READ | PROT_EXEC);
    return true;
}

static bool hook_set(struct hook *hook, bool on)
{
    if (hook->installed == on)
        return true;
    if (!hook->stub && !hook_prepare(hook))
        return false;

    uint32_t *at = (uint32_t *)hook->addr;
    uint32_t  branch = INSN_B | ((((uintptr_t)hook->stub - (uintptr_t)(at + 2)) >> 2) & 0x00ffffffu);
    uint32_t  expected = on ? hook->insn : branch;
    if (*at != expected) {
        say("%s: found %08x instead of %08x, left alone", hook->name, (unsigned)*at, (unsigned)expected);
        return false;
    }
    if (!text_poke(at, on ? branch : hook->insn)) {
        say("%s: cannot patch: %s", hook->name, strerror(errno));
        return false;
    }
    hook->installed = on;
    say("%s %s", hook->name, on ? "hooked" : "unhooked");
    return true;
}

/* Firmware calls. A hooked function is entered past its hook. */

static int write_i2c(void *logic, uint16_t reg, uint16_t value)
{
    return ((write_i2c_fn *)(g_hooks[H_WRITE_I2C].stub + STUB_RESUME))(logic, reg, value);
}

static int read_i2c(void *logic, uint16_t reg, uint16_t *value)
{
    return ((read_i2c_fn *)g_firmware[F_READ_I2C].addr)(logic, reg, value);
}

static int on_write_i2c(void *logic, uint16_t reg, uint16_t value)
{
    if (reg == REG_APC_OFFSET) {
        pthread_mutex_lock(&g_lock);
        g_apc_offset = value;
        pthread_mutex_unlock(&g_lock);
        return write_i2c(logic, reg, value);
    }

    struct knob *knob = knob_for(reg);
    if (!knob)
        return write_i2c(logic, reg, value);

    pthread_mutex_lock(&g_lock);
    knob->stock = value;
    uint16_t want = tuned(knob, value);
    int      result = write_i2c(logic, reg, want);
    if (result == 0)
        knob->sent = want;
    pthread_mutex_unlock(&g_lock);
    return result;
}

static int on_support_option(void *self, int option, bool *supported)
{
    if (option == OPTION_WB_10P) {
        if (supported)
            *supported = true;
        return 1;
    }
    return ((support_option_fn *)(g_hooks[H_SUPPORT_OPTION].stub + STUB_RESUME))(self, option, supported);
}

/*
 * 12-bit hardware gamma LUT in libUTOPIA.so (MDrv_PNL_SetGammaTbl, map_mode 1).
 *
 * SdDisplay_SetPictureQuality packs 256 12-bit entries per channel into 384 bytes
 * (two entries per 3 bytes: low nibbles in s[0], high bytes in s[1] and s[2]).
 * Hal_PNL_Set12BitGammaPerChannel also reads entry 256 at bytes 384..385 for the
 * top interpolation slope, so we emit 386 bytes with entry[256] = entry[255].
 */
#define GAMMA_PACKED_STOCK 384
#define GAMMA_PACKED_OUT   386

static int     g_gamma_mode = GAMMA_NONE;
static bool    g_gamma_have_stock;
static int     g_gamma_lut_type = 1;
static uint8_t g_gamma_stock[3][GAMMA_PACKED_STOCK];
static uint8_t g_gamma_out[3][GAMMA_PACKED_OUT];

static uint8_t set_gamma_orig(int lut_type, uint8_t **gamma_tab, int map_mode)
{
    return ((set_gamma_fn *)(g_hooks[H_SET_GAMMA].stub + STUB_RESUME))(lut_type, gamma_tab, map_mode);
}

static void unpack_gamma_12bit(const uint8_t src[GAMMA_PACKED_STOCK], uint16_t dst[256])
{
    for (int k = 0; k < 128; k++) {
        const uint8_t *s = src + 3 * k;
        dst[2 * k]     = (uint16_t)((s[0] & 0x0fu) | ((uint16_t)s[1] << 4));
        dst[2 * k + 1] = (uint16_t)((s[0] >> 4) | ((uint16_t)s[2] << 4));
    }
}

static void pack_gamma_12bit(const uint16_t src[256], uint8_t dst[GAMMA_PACKED_OUT])
{
    for (int k = 0; k < 128; k++) {
        uint16_t v0 = src[2 * k] & 0x0fffu;
        uint16_t v1 = src[2 * k + 1] & 0x0fffu;
        uint8_t *d = dst + 3 * k;
        d[0] = (uint8_t)((v0 & 0x0fu) | ((v1 & 0x0fu) << 4));
        d[1] = (uint8_t)(v0 >> 4);
        d[2] = (uint8_t)(v1 >> 4);
    }
    /* Guard entry 256 so Hal_PNL_Set12BitGammaPerChannel sees zero slope at peak. */
    uint16_t peak = src[255] & 0x0fffu;
    dst[384] = (uint8_t)(peak & 0x0fu);
    dst[385] = (uint8_t)(peak >> 4);
}

static void compose_gamma_channel(const uint8_t stock_packed[GAMMA_PACKED_STOCK], int mode,
                                  uint8_t out_packed[GAMMA_PACKED_OUT])
{
    uint16_t stock[256], out[256];
    unpack_gamma_12bit(stock_packed, stock);

    if (mode == GAMMA_NONE) {
        pack_gamma_12bit(stock, out_packed);
        return;
    }

    const uint16_t *curve = g_gamma_curves[mode];
    for (int i = 0; i < 256; i++) {
        uint32_t scaled = (uint32_t)curve[i] * 255u;
        uint32_t idx = scaled / 4095u;
        uint32_t frac = scaled % 4095u;
        if (idx >= 255u) {
            out[i] = stock[255];
        } else {
            int32_t d = (int32_t)stock[idx + 1] - (int32_t)stock[idx];
            int32_t num = d * (int32_t)frac;
            int32_t delta = num >= 0 ? (num + 2047) / 4095 : (num - 2047) / 4095;
            int32_t v = (int32_t)stock[idx] + delta;
            out[i] = (uint16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
        }
    }
    pack_gamma_12bit(out, out_packed);
}

static uint8_t apply_gamma_locked(void)
{
    uint8_t *ptrs[3] = { g_gamma_out[0], g_gamma_out[1], g_gamma_out[2] };
    for (int ch = 0; ch < 3; ch++)
        compose_gamma_channel(g_gamma_stock[ch], g_gamma_mode, g_gamma_out[ch]);
    return set_gamma_orig(g_gamma_lut_type, ptrs, 1);
}

static uint8_t on_set_gamma(int lut_type, uint8_t **gamma_tab, int map_mode)
{
    if (map_mode != 1 || !gamma_tab || !gamma_tab[0] || !gamma_tab[1] || !gamma_tab[2])
        return set_gamma_orig(lut_type, gamma_tab, map_mode);

    pthread_mutex_lock(&g_lock);
    if (!g_hooks[H_SET_GAMMA].installed) {
        pthread_mutex_unlock(&g_lock);
        return set_gamma_orig(lut_type, gamma_tab, map_mode);
    }
    g_gamma_lut_type = lut_type;
    for (int ch = 0; ch < 3; ch++)
        memcpy(g_gamma_stock[ch], gamma_tab[ch], GAMMA_PACKED_STOCK);
    g_gamma_have_stock = true;
    uint8_t result = apply_gamma_locked();
    pthread_mutex_unlock(&g_lock);
    return result;
}

/* The logic board object, once exeTV has created it and if it is a LogicFHD. */
static void *logic_board(void)
{
    void *const *slot = (void *const *)g_firmware[F_LOGIC_IC].addr;
    uintptr_t    vptr = g_firmware[F_LOGIC_FHD_VTABLE].addr + 2 * sizeof(void *);

    for (int tenths = 0; !*slot && tenths < 100; tenths++)
        usleep(100000);

    void *logic = *slot;
    if (!logic) {
        say("exeTV has no logic board object");
    } else if (*(const uintptr_t *)logic != vptr) {
        say("the logic board is not a LogicFHD");
        logic = NULL;
    }
    return logic;
}

/* Brings every knob's register in line with its override. */
static void sync_knobs(void *logic)
{
    uint16_t apc;
    if (read_i2c(logic, REG_APC_OFFSET, &apc) == 0)
        g_apc_offset = apc;

    for (struct knob *knob = g_knobs; knob < g_knobs + K_COUNT; knob++) {
        if (knob->stock == UNKNOWN) {
            uint16_t value;
            if (read_i2c(logic, knob->reg, &value) != 0) {
                say("%-11s 0x%04x: cannot be read", knob->name, knob->reg);
                continue;
            }
            knob->stock = knob->sent = value & knob->mask;
        }

        uint16_t want = tuned(knob, (uint16_t)knob->stock);
        if (want != knob->sent) {
            if (write_i2c(logic, knob->reg, want) == 0)
                knob->sent = want;
            else
                say("%-11s 0x%04x: cannot be written", knob->name, knob->reg);
        }
        say("%-11s 0x%04x: firmware 0x%04x, panel 0x%04x", knob->name, knob->reg, (unsigned)knob->stock,
            (unsigned)knob->sent);
    }
}

struct request {
    bool configure; /* false leaves the configuration as it is */
    bool dump;
    bool unlock;
    int  gamma;
    int  to[K_COUNT];
};

/*
 * A write that entered m_Logic_WriteI2C just before it was hooked is not seen
 * by the hook. This gives it time to reach the bus before the registers are
 * read back.
 */
#define HOOK_SETTLE_US 100000

static void configure(const struct request *request, void *logic)
{
    struct hook *hook = &g_hooks[H_WRITE_I2C];
    bool         tune = false;
    for (int i = 0; i < K_COUNT; i++) {
        g_knobs[i].to = logic ? request->to[i] : KEEP;
        tune |= g_knobs[i].to != KEEP;
    }

    if (tune && !hook->installed && hook_set(hook, true)) {
        usleep(HOOK_SETTLE_US);
        g_apc_offset = UNKNOWN;
        for (int i = 0; i < K_COUNT; i++)
            g_knobs[i].stock = g_knobs[i].sent = UNKNOWN;
    }
    if (logic && hook->installed) {
        sync_knobs(logic);
        if (!tune)
            hook_set(hook, false);
    }

    hook_set(&g_hooks[H_SUPPORT_OPTION], request->unlock);

    struct hook *ghook = &g_hooks[H_SET_GAMMA];
    g_gamma_mode = request->gamma;
    if (g_gamma_mode != GAMMA_NONE) {
        if (hook_set(ghook, true)) {
            if (g_gamma_have_stock) {
                apply_gamma_locked();
                say("gamma %s applied to firmware LUT", g_gamma_names[g_gamma_mode]);
            } else {
                say("gamma %s armed (applies on next firmware gamma load)", g_gamma_names[g_gamma_mode]);
            }
        }
    } else if (ghook->installed) {
        if (g_gamma_have_stock) {
            apply_gamma_locked();
            say("gamma restored to firmware LUT");
        }
        hook_set(ghook, false);
        g_gamma_have_stock = false;
    }
}

static void dump_register(void *logic, const char *name, uint16_t reg)
{
    uint16_t value;
    if (read_i2c(logic, reg, &value) == 0)
        say("dump %-11s 0x%04x = 0x%04x", name, reg, value);
    else
        say("dump %-11s 0x%04x: cannot be read", name, reg);
}

static void dump(void *logic)
{
    for (int i = 0; i < H_COUNT; i++)
        say("dump %s is %s", g_hooks[i].name, g_hooks[i].installed ? "hooked" : "not hooked");
    say("dump gamma mode %s (stock LUT %s)", g_gamma_names[g_gamma_mode],
        g_gamma_have_stock ? "captured" : "not captured");
    if (!logic)
        return;

    for (int i = 0; i < K_COUNT; i++)
        dump_register(logic, g_knobs[i].name, g_knobs[i].reg);
    dump_register(logic, "APC offset", 0x89c0);
    dump_register(logic, "SubBright", 0x8926);

    int celsius;
    if (((panel_temp_fn *)g_firmware[F_PANEL_TEMP].addr)(logic, &celsius) == 0)
        say("dump panel temperature %d C", celsius);
}

/*
 * Everything that talks to the firmware runs here, on a thread of our own.
 * The thread samyGOso borrowed was stopped at an arbitrary point and may hold
 * any of the firmware's locks, so it must not wait for one.
 */
static void *worker(void *arg)
{
    static enum { UNTRIED, SUPPORTED, UNSUPPORTED } firmware;
    struct request *request = arg;

    pthread_mutex_lock(&g_lock);
    if (firmware == UNTRIED)
        firmware = firmware_matches() ? SUPPORTED : UNSUPPORTED;
    if (firmware == SUPPORTED) {
        void *logic = logic_board();
        if (request->configure)
            configure(request, logic);
        if (request->dump)
            dump(logic);
    }
    say("done");
    pthread_mutex_unlock(&g_lock);

    free(request);
    return NULL;
}

/* The text after "<key>:" if the word starts with that, ignoring case. */
static const char *value_of(const char *word, const char *key)
{
    size_t length = strlen(key);
    return strncasecmp(word, key, length) == 0 && word[length] == ':' ? word + length + 1 : NULL;
}

static bool parse_switch(const char *text, int on, int *out)
{
    if (strcasecmp(text, "ON") == 0)
        *out = on;
    else if (strcasecmp(text, "OFF") == 0)
        *out = 0;
    else
        return false;
    return true;
}

static bool parse_number(const char *text, int low, int high, int *out)
{
    int value = 0;
    if (!*text)
        return false;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > high)
            return false;
        value = value * 10 + (*text - '0');
    }
    if (value < low || value > high)
        return false;
    *out = value;
    return true;
}

static bool parse_word(const char *word, struct request *request)
{
    const char *value;

    if (strcasecmp(word, "DUMP") == 0) {
        request->dump = true;
        return true;
    }
    request->configure = true;

    if (strcasecmp(word, "OFF") == 0)
        return true;
    if (strcasecmp(word, "UNLOCK") == 0) {
        request->unlock = true;
        return true;
    }
    if (strcasecmp(word, "DRIVE:STANDARD") == 0) {
        request->to[K_DRIVE] = DRIVE_STANDARD;
        return true;
    }
    if ((value = value_of(word, "DEEPBLACK")))
        return parse_switch(value, DEEP_BLACK_ON, &request->to[K_DEEP_BLACK]);
    if ((value = value_of(word, "PEAKSTRETCH")))
        return parse_switch(value, PEAK_STRETCH_ON, &request->to[K_PEAK_STRETCH]);
    if ((value = value_of(word, "DUTY2")))
        return parse_number(value, 79, 160, &request->to[K_DUTY2]);
    if ((value = value_of(word, "SUBCONTRAST"))) {
        /* The register holds the factory menu's Sub Contrast shifted left by 4. */
        int level;
        if (!parse_number(value, 100, 160, &level))
            return false;
        request->to[K_SUB_CONTRAST] = level << 4;
        return true;
    }
    if ((value = value_of(word, "GAMMA"))) {
        for (int m = GAMMA_TOE; m < GAMMA_COUNT; m++) {
            if (strcasecmp(value, g_gamma_names[m]) == 0) {
                request->gamma = m;
                return true;
            }
        }
        return false;
    }
    return false;
}

#define MAX_WORDS 16

void lib_init(void *handle, const char *libpath)
{
    (void)handle;

    struct request request = { .to = { KEEP, KEEP, KEEP, KEEP, KEEP } };
    const char    *argv[MAX_WORDS];
    int            argc = sgo_args(libpath, argv, MAX_WORDS);
    char           words[160] = "";
    size_t         length = 0;

    if (argc > MAX_WORDS) {
        say("libPDPTune " VERSION ": too many arguments, nothing done");
        return;
    }
    for (int i = 0; i < argc; i++) {
        if (!parse_word(argv[i], &request)) {
            say("libPDPTune " VERSION ": bad argument '%s', nothing done", argv[i]);
            return;
        }
        if (length < sizeof words)
            length += (size_t)snprintf(words + length, sizeof words - length, " %s", argv[i]);
    }
    if (!request.configure && !request.dump) {
        say("libPDPTune " VERSION ": no arguments, nothing done");
        return;
    }
    say("libPDPTune " VERSION ":%s", words);

    struct request *copy = malloc(sizeof *copy);
    if (!copy)
        return;
    *copy = request;

    pthread_t thread;
    int       error = pthread_create(&thread, NULL, worker, copy);
    if (error) {
        say("cannot start a thread: %s", strerror(error));
        free(copy);
        return;
    }
    pthread_detach(thread);
}

/* The library is linked -z nodelete and its hooks stay until OFF. */
void lib_deinit(void *handle)
{
    (void)handle;
}
