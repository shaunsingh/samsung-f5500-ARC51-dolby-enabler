/*
 * A stand-in for exeTV to run the payloads against.
 *
 * libPDPTune accepts it as the real firmware: the functions and objects that
 * library looks up sit at the firmware's addresses (the Makefile places the
 * .fw.* sections) and begin with the firmware's instructions.
 *
 * Usage: qemu-arm -L <sysroot> fake_exetv /abs/libPDPTune.so /abs/libDDCRAM_EDID.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIRMWARE_FUNCTION(section, symbol, body)                                                                       \
    __asm__(".pushsection " section ", \"ax\", %progbits\n"                                                           \
            ".arm\n"                                                                                                   \
            ".global " symbol "\n"                                                                                     \
            ".type " symbol ", %function\n"                                                                            \
            symbol ":\n"                                                                                               \
            "    push {r4, r5, r6, r7, fp, lr}\n"                                                                      \
            "    add fp, sp, #20\n"                                                                                    \
            "    bl " body "\n"                                                                                        \
            "    sub sp, fp, #20\n"                                                                                    \
            "    pop {r4, r5, r6, r7, fp, pc}\n"                                                                       \
            ".popsection\n")

#define WRITE_I2C       "_ZN8LogicFHD16m_Logic_WriteI2CEtt"
#define READ_I2C        "_ZN8LogicFHD15m_Logic_ReadI2CEtPt"
#define PANEL_TEMP      "_ZN8LogicFHD20t_Logic_GetPanelTempEPi"
#define SUPPORT_OPTION  "_ZN14PrdInfoManager18CheckSupportOptionEN12IProductInfo22ProductSupportOption_kEPb"
#define SCREEN_INSTANCE "_ZN12SsScreenBase8InstanceEN9TPCScreen7EScreenE"
#define GET_GAMMA_MODE  "_ZN12SsScreenBase12GetGammaModeEPN9TPCScreen10EGammaModeE"
#define SET_GAMMA_MODE  "_ZN12SsScreenBase12SetGammaModeEN9TPCScreen10EGammaModeE"

FIRMWARE_FUNCTION(".fw.write_i2c", WRITE_I2C, "body_write_i2c");
FIRMWARE_FUNCTION(".fw.read_i2c", READ_I2C, "body_read_i2c");
FIRMWARE_FUNCTION(".fw.panel_temp", PANEL_TEMP, "body_panel_temp");
FIRMWARE_FUNCTION(".fw.support_option", SUPPORT_OPTION, "body_support_option");
FIRMWARE_FUNCTION(".fw.screen_instance", SCREEN_INSTANCE, "body_screen_instance");
FIRMWARE_FUNCTION(".fw.get_gamma_mode", GET_GAMMA_MODE, "body_get_gamma_mode");
FIRMWARE_FUNCTION(".fw.set_gamma_mode", SET_GAMMA_MODE, "body_set_gamma_mode");

/* MDrv_PNL_SetGammaTbl in libUTOPIA.so begins with push {r4..r8, sl, fp, lr} (0xe92d4df0). */
__asm__(".text\n"
        ".arm\n"
        ".global MDrv_PNL_SetGammaTbl\n"
        ".type MDrv_PNL_SetGammaTbl, %function\n"
        "MDrv_PNL_SetGammaTbl:\n"
        "    push {r4, r5, r6, r7, r8, sl, fp, lr}\n"
        "    add fp, sp, #28\n"
        "    bl body_set_gamma\n"
        "    sub sp, fp, #28\n"
        "    pop {r4, r5, r6, r7, r8, sl, fp, pc}\n");

int     fw_write_i2c(void *logic, uint16_t reg, uint16_t value) __asm__(WRITE_I2C);
int     fw_support_option(void *self, int option, bool *supported) __asm__(SUPPORT_OPTION);
uint8_t MDrv_PNL_SetGammaTbl(int lut_type, uint8_t **gamma_tab, int map_mode);

void *vtable_logic_fhd[49] __asm__("_ZTV8LogicFHD") __attribute__((section(".fw.logic_fhd_vtable")));
void *logic_ic __asm__("_ZN8TDcLogic11m_poLogicICE") __attribute__((section(".fw.logic_ic")));

enum {
    DRIVE = 0x806f,
    DEEP_BLACK = 0x809c,
    PEAK_STRETCH = 0x8063,
    DEEP_BLACK_MODE = 0x80bf,
    POWER_SAVE = 0x8064,
    APC_SPEED = 0x89bf,
    APC_OFFSET = 0x89c0,
    SAPC_TIMER = 0x89ee,
    DUTY2 = 0x89da,
    SUB_CONTRAST = 0x8927,
    TEST_PATTERN = 0x8114,
    OTHER = 0x803d
};

static _Atomic uint8_t g_filmmode = 0;

uint8_t fake_get_filmmode(void) __asm__("_Z29mbrg_outputsource_getfilmmodev");
uint8_t fake_get_filmmode(void)
{
    return g_filmmode;
}

uint8_t body_set_filmmode(uint8_t enable)
{
    g_filmmode = enable ? 1 : 0;
    return 1;
}

/* mbrg_outputsource_setfilmmode in libUTOPIA.so begins with push {r4, r5, fp, lr} (0xe92d4830). */
__asm__(".text\n"
        ".arm\n"
        ".global _Z29mbrg_outputsource_setfilmmodeh\n"
        ".type _Z29mbrg_outputsource_setfilmmodeh, %function\n"
        "_Z29mbrg_outputsource_setfilmmodeh:\n"
        "    push {r4, r5, fp, lr}\n"
        "    add fp, sp, #12\n"
        "    bl body_set_filmmode\n"
        "    sub sp, fp, #12\n"
        "    pop {r4, r5, fp, pc}\n");

uint8_t fake_set_filmmode(uint8_t enable) __asm__("_Z29mbrg_outputsource_setfilmmodeh");

uint16_t MApi_XC_GetOutputVFreqX100(void)
{
    return g_filmmode ? 4795 : 5994;
}

static struct { void *vptr; } g_logic = { &vtable_logic_fhd[2] };
static int                    g_screen_dummy;
static _Atomic int            g_osd_gamma = -1;
static _Atomic uint16_t       g_panel[0x10000];
static pthread_mutex_t        g_bus = PTHREAD_MUTEX_INITIALIZER;
static uint8_t                g_gamma_hw[3][386];
static _Atomic int            g_gamma_calls;

static void pack_ramp(uint8_t buf[386], uint16_t max_val)
{
    for (int k = 0; k < 128; k++) {
        uint16_t v0 = (uint16_t)(((uint32_t)(2 * k) * max_val + 127) / 255);
        uint16_t v1 = (uint16_t)(((uint32_t)(2 * k + 1) * max_val + 127) / 255);
        buf[3 * k]     = (uint8_t)((v0 & 0x0fu) | ((v1 & 0x0fu) << 4));
        buf[3 * k + 1] = (uint8_t)(v0 >> 4);
        buf[3 * k + 2] = (uint8_t)(v1 >> 4);
    }
    /* Deliberately leave bytes 384..385 garbage, like Samsung's SdDisplay_SetPictureQuality. */
    buf[384] = 0xaa;
    buf[385] = 0x55;
}

void *body_screen_instance(int screen)
{
    return screen == 0 ? &g_screen_dummy : NULL;
}

int body_get_gamma_mode(void *screen, int *mode)
{
    if (screen != &g_screen_dummy)
        abort();
    *mode = g_osd_gamma;
    return 0;
}

int body_set_gamma_mode(void *screen, int mode)
{
    static int cached_mode = -1;
    if (screen != &g_screen_dummy || mode < -3 || mode > 3)
        return 1;
    g_osd_gamma = mode;
    /* Real PrimePictureShare skips MDrv_PNL_SetGammaTbl when the gamma index matches its RAM cache. */
    if (mode == cached_mode)
        return 0;
    cached_mode = mode;
    uint8_t  r[386], g[386], b[386];
    uint8_t *ptrs[3] = { r, g, b };
    pack_ramp(r, 4095);
    pack_ramp(g, 3900);
    pack_ramp(b, 3500);
    MDrv_PNL_SetGammaTbl(1, ptrs, 1);
    return 0;
}

uint8_t body_set_gamma(int lut_type, uint8_t **gamma_tab, int map_mode)
{
    (void)lut_type;
    if (map_mode == 1 && gamma_tab) {
        for (int ch = 0; ch < 3; ch++)
            memcpy(g_gamma_hw[ch], gamma_tab[ch], 386);
    }
    g_gamma_calls++;
    return 1;
}

int body_write_i2c(void *logic, uint16_t reg, uint16_t value)
{
    if (logic != &g_logic)
        abort();
    pthread_mutex_lock(&g_bus);
    g_panel[reg] = value;
    pthread_mutex_unlock(&g_bus);
    return 0;
}

int body_read_i2c(void *logic, uint16_t reg, uint16_t *value)
{
    if (logic != &g_logic)
        abort();
    *value = g_panel[reg] | (reg == DRIVE ? 0x0100 : 0);
    return 0;
}

int body_panel_temp(void *logic, int *celsius)
{
    (void)logic;
    *celsius = 46;
    return 0;
}

int body_support_option(void *self, int option, bool *supported)
{
    (void)self;
    *supported = option == 1;
    return 1;
}

/* libUTOPIA's DDC RAM interface, as libDDCRAM_EDID uses it. */
enum { PORTS = 4, EDID_SIZE = 256 };

struct ddcram {
    uint8_t *edid;
    uint16_t size;
    uint32_t port;
};

static uint8_t  g_ddcram[PORTS][EDID_SIZE];
static uint32_t g_hpd[16][2];
static int      g_hpd_calls;

void MDrv_HDMI_READ_DDCRAM(struct ddcram *info, uint32_t unused)
{
    (void)unused;
    memcpy(info->edid, g_ddcram[info->port], info->size);
}

void MDrv_HDMI_PROG_DDCRAM(struct ddcram *info, uint32_t unused)
{
    (void)unused;
    memcpy(g_ddcram[info->port], info->edid, info->size);
}

void MApi_XC_Sys_SetHPD(uint32_t enable, uint32_t port)
{
    if (g_hpd_calls < 16) {
        g_hpd[g_hpd_calls][0] = enable;
        g_hpd[g_hpd_calls][1] = port;
    }
    g_hpd_calls++;
}

typedef void lib_init_fn(void *handle, const char *libpath);

static int g_failures;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* The library works on a thread of its own, so results arrive a little later. */
#define EVENTUALLY(condition)                                                                                          \
    do {                                                                                                               \
        for (int tries = 0; !(condition) && tries < 500; tries++)                                                      \
            usleep(10000);                                                                                             \
        CHECK(condition);                                                                                              \
    } while (0)

/* Calls lib_init the way samyGOso does: the words follow the library path. */
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

static lib_init_fn *load(const char *libpath)
{
    void        *library = dlopen(libpath, RTLD_LAZY);
    lib_init_fn *lib_init = library ? (lib_init_fn *)dlsym(library, "lib_init") : NULL;
    if (!lib_init) {
        printf("FAIL %s\n", dlerror());
        exit(1);
    }
    return lib_init;
}

static bool hooked(void *function)
{
    return *(uint32_t *)function != 0xe92d48f0;
}

static bool gamma_hooked(void)
{
    return *(uint32_t *)(void *)MDrv_PNL_SetGammaTbl != 0xe92d4df0;
}

static uint16_t gamma_entry(const uint8_t *buf, int idx)
{
    if (idx == 256)
        return (uint16_t)((buf[384] & 0x0fu) | ((uint16_t)buf[385] << 4));
    const uint8_t *s = buf + 3 * (idx / 2);
    return (idx & 1) ? (uint16_t)((s[0] >> 4) | ((uint16_t)s[2] << 4))
                     : (uint16_t)((s[0] & 0x0fu) | ((uint16_t)s[1] << 4));
}

static bool unlocked_all(void)
{
    bool p = false, wb = false, cs = false;
    CHECK(fw_support_option(NULL, 0x49, &p) == 1);
    CHECK(fw_support_option(NULL, 0x4a, &wb) == 1);
    CHECK(fw_support_option(NULL, 0x51, &cs) == 1);
    return p && wb && cs;
}

static bool wb10p(void)
{
    bool supported = false;
    CHECK(fw_support_option(NULL, 0x4a, &supported) == 1);
    return supported;
}

static atomic_bool g_stop;
static atomic_int  g_bad;

static void *firmware_thread(void *arg)
{
    uint16_t other = (uint16_t)(0x7000 + (intptr_t)arg);

    for (uint16_t i = 0; !g_stop; i++) {
        bool supported = false;

        fw_write_i2c(&g_logic, other, i);
        if (g_panel[other] != i)
            g_bad++;
        fw_write_i2c(&g_logic, DRIVE, 0x05);
        uint16_t drive = g_panel[DRIVE];
        if (drive != 0x05 && drive != 0x11)
            g_bad++;
        if (fw_support_option(NULL, 1, &supported) != 1 || !supported)
            g_bad++;
        if (fw_support_option(NULL, 0x4a, &supported) != 1)
            g_bad++;
    }
    return NULL;
}

static lib_init_fn *g_pdptune;
static const char  *g_pdptune_path;

static void inject(const char *words)
{
    inject_into(g_pdptune, g_pdptune_path, words);
}

static void test_pdptune(const char *libpath)
{
    g_pdptune = load(libpath);
    g_pdptune_path = libpath;

    /* The picture as the firmware leaves it in Movie mode. */
    logic_ic = &g_logic;
    fw_write_i2c(&g_logic, DRIVE, 0x05);
    fw_write_i2c(&g_logic, DEEP_BLACK_MODE, 0);
    fw_write_i2c(&g_logic, POWER_SAVE, 1);
    fw_write_i2c(&g_logic, APC_SPEED, 0xc1ff);
    fw_write_i2c(&g_logic, SAPC_TIMER, 1);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    fw_write_i2c(&g_logic, SUB_CONTRAST, 0x0800);

    puts("bad arguments change nothing");
    inject("DRIVE:STANDARD DUTY2:256");
    inject("DRIVE:FAST");
    inject("SUBCONTRAST:13x");
    inject("SUBCONTRAST:221");
    inject("APC_OFFSET:512");
    inject("DEEPBLACK:");
    inject("");
    usleep(300000);
    CHECK(!hooked(fw_write_i2c) && !hooked(fw_support_option) && g_panel[DRIVE] == 0x05);

    puts("DUMP alone changes nothing");
    inject("DUMP");
    usleep(300000);
    CHECK(!hooked(fw_write_i2c) && !hooked(fw_support_option) && g_panel[DRIVE] == 0x05);

    puts("UNLOCK DRIVE:STANDARD");
    inject("UNLOCK DRIVE:STANDARD");
    EVENTUALLY(g_panel[DRIVE] == 0x11 && hooked(fw_support_option));
    CHECK(hooked(fw_write_i2c) && wb10p() && unlocked_all());
    bool supported = true;
    CHECK(fw_support_option(NULL, 2, &supported) == 1 && !supported);
    CHECK(g_panel[DUTY2] == 0x80 && g_panel[SUB_CONTRAST] == 0x0800 && g_panel[DEEP_BLACK] == 0);

    puts("the override follows the firmware");
    fw_write_i2c(&g_logic, DRIVE, 0x11);
    CHECK(g_panel[DRIVE] == 0x11);
    fw_write_i2c(&g_logic, DRIVE, 0x03);
    CHECK(g_panel[DRIVE] == 0x03);
    fw_write_i2c(&g_logic, DRIVE, 0x05);
    CHECK(g_panel[DRIVE] == 0x11);
    fw_write_i2c(&g_logic, OTHER, 0x1234);
    CHECK(g_panel[OTHER] == 0x1234);

    puts("the same words again change nothing");
    inject("unlock drive:standard");
    usleep(300000);
    CHECK(g_panel[DRIVE] == 0x11 && hooked(fw_write_i2c) && wb10p());

    puts("a new configuration replaces the old one");
    fw_write_i2c(&g_logic, APC_OFFSET, 0);
    inject("DEEPBLACK:ON PEAKSTRETCH:ON DUTY2:139 SUBCONTRAST:132 DUMP");
    EVENTUALLY(g_panel[SUB_CONTRAST] == 0x0840 && !hooked(fw_support_option));
    CHECK(g_panel[DRIVE] == 0x05 && g_panel[DEEP_BLACK] == 3 && g_panel[PEAK_STRETCH] == 1 && g_panel[DUTY2] == 139);
    CHECK(hooked(fw_write_i2c) && !wb10p());
    fw_write_i2c(&g_logic, DUTY2, 0);
    CHECK(g_panel[DUTY2] == 0);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    CHECK(g_panel[DUTY2] == 139);
    /* Thermal / Eco / Cell Light < 20 dimming (APC_OFFSET > 0) holds duty_2 at stock 128. */
    fw_write_i2c(&g_logic, APC_OFFSET, 50);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    CHECK(g_panel[DUTY2] == 0x80);
    fw_write_i2c(&g_logic, APC_OFFSET, 0);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    CHECK(g_panel[DUTY2] == 139);
    fw_write_i2c(&g_logic, DEEP_BLACK, 0);
    fw_write_i2c(&g_logic, PEAK_STRETCH, 1);
    CHECK(g_panel[DEEP_BLACK] == 3 && g_panel[PEAK_STRETCH] == 1);

    puts("extended max-performance registers override APC/SAPC/DeepBlackMode/PowerSave and restore on OFF");
    inject("DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF APC_OFFSET:0 DUTY2:200 SUBCONTRAST:140 PATTERN:BOX9 CINEMA_SMOOTH:ON");
    EVENTUALLY(g_panel[DUTY2] == 200 && g_panel[SUB_CONTRAST] == (140 << 4) && g_panel[TEST_PATTERN] == 0x0106 && g_filmmode == 1);
    CHECK(g_panel[DEEP_BLACK_MODE] == 1 && g_panel[POWER_SAVE] == 0);
    CHECK(g_panel[APC_SPEED] == 0x41ff && g_panel[SAPC_TIMER] == 0 && g_panel[APC_OFFSET] == 0);
    /* With APC_OFFSET:0 explicitly armed, firmware thermal/Eco dimming is overridden and DUTY2 stays boosted. */
    fw_write_i2c(&g_logic, APC_OFFSET, 50);
    CHECK(g_panel[APC_OFFSET] == 0);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    CHECK(g_panel[DUTY2] == 200);
    /* Firmware attempts to reset Cinema Smooth to 0 on source switch are intercepted while armed. */
    fake_set_filmmode(0);
    CHECK(g_filmmode == 1);

    puts("OFF restores what the firmware last asked for");
    inject("DEEPBLACK:OFF");
    EVENTUALLY(g_panel[DUTY2] == 0x80 && g_panel[TEST_PATTERN] == 0 && g_filmmode == 0);
    CHECK(g_panel[DEEP_BLACK] == 0 && g_panel[PEAK_STRETCH] == 1 && g_panel[SUB_CONTRAST] == 0x0800);
    CHECK(g_panel[DEEP_BLACK_MODE] == 0 && g_panel[POWER_SAVE] == 1 && g_panel[APC_SPEED] == 0xc1ff);
    CHECK(g_panel[SAPC_TIMER] == 1 && g_panel[APC_OFFSET] == 50);
    fw_write_i2c(&g_logic, APC_OFFSET, 0);
    inject("OFF");
    EVENTUALLY(!hooked(fw_write_i2c) && g_filmmode == 0);
    CHECK(g_panel[DRIVE] == 0x05 && g_panel[DEEP_BLACK] == 0 && g_panel[PEAK_STRETCH] == 1);
    CHECK(g_panel[DUTY2] == 0x80 && g_panel[SUB_CONTRAST] == 0x0800 && g_panel[TEST_PATTERN] == 0 && !hooked(fw_support_option));

    puts("opt-in 12-bit gamma LUT auto-syncs on boot, composes onto firmware table, and restores on OFF");
    CHECK(!gamma_hooked() && g_osd_gamma == -1);
    uint8_t stock_r[386], stock_g[386], stock_b[386];
    pack_ramp(stock_r, 4095);
    pack_ramp(stock_g, 3900); /* Warm2-style channel scaling must be preserved at peak */
    pack_ramp(stock_b, 3500);
    int before = g_gamma_calls;
    inject("OSD_GAMMA:0 GAMMA:BT1886 DUMP");
    EVENTUALLY(gamma_hooked() && g_gamma_calls == before + 1);
    CHECK(g_osd_gamma == 0);
    CHECK(gamma_entry(g_gamma_hw[0], 0) == 0 && gamma_entry(g_gamma_hw[0], 255) == 4095);
    CHECK(gamma_entry(g_gamma_hw[1], 255) == 3900 && gamma_entry(g_gamma_hw[2], 255) == 3500);
    CHECK(gamma_entry(g_gamma_hw[0], 256) == 4095 && gamma_entry(g_gamma_hw[1], 256) == 3900);
    /* BT.1886 lifts near-black out of plasma MLL (entry 5 > linear 80) and deepens midtones (entry 128 < 2056). */
    CHECK(gamma_entry(g_gamma_hw[0], 5) > gamma_entry(stock_r, 5));
    CHECK(gamma_entry(g_gamma_hw[0], 128) < gamma_entry(stock_r, 128));

    /* Live switch to GAMMA:2.4 immediately re-applies to the captured stock LUT without touching OSD Gamma. */
    before = g_gamma_calls;
    inject("GAMMA:2.4");
    EVENTUALLY(g_gamma_calls == before + 1);
    CHECK(gamma_entry(g_gamma_hw[0], 5) < gamma_entry(stock_r, 5));
    CHECK(gamma_entry(g_gamma_hw[0], 255) == 4095 && gamma_entry(g_gamma_hw[0], 256) == 4095);

    /* OFF restores the exact stock LUT (with clean entry 256) and unhooks SetGammaTbl. */
    before = g_gamma_calls;
    inject("OFF");
    EVENTUALLY(!gamma_hooked());
    CHECK(g_gamma_calls == before + 1);
    CHECK(memcmp(g_gamma_hw[0], stock_r, 384) == 0 && memcmp(g_gamma_hw[1], stock_g, 384) == 0);
    CHECK(gamma_entry(g_gamma_hw[2], 256) == 3500);

    /* Re-enabling GAMMA:BT1886 when OSD Gamma is ALREADY 0 still busts the RAM cache in one HW write. */
    before = g_gamma_calls;
    inject("OSD_GAMMA:0 GAMMA:BT1886");
    EVENTUALLY(gamma_hooked() && g_gamma_calls == before + 1);
    CHECK(g_osd_gamma == 0 && gamma_entry(g_gamma_hw[0], 5) > gamma_entry(stock_r, 5));
    inject("OFF");
    EVENTUALLY(!gamma_hooked());

    puts("hooks come and go under load");
    pthread_t threads[3];
    for (intptr_t i = 0; i < 3; i++)
        pthread_create(&threads[i], NULL, firmware_thread, (void *)i);
    for (int round = 0; round < 10; round++) {
        inject("UNLOCK DRIVE:STANDARD");
        EVENTUALLY(hooked(fw_write_i2c) && hooked(fw_support_option));
        inject("OFF");
        EVENTUALLY(!hooked(fw_write_i2c) && !hooked(fw_support_option));
    }
    g_stop = true;
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);
    CHECK(g_bad == 0);
    usleep(300000);
    CHECK(g_panel[DRIVE] == 0x05);
}

/* A two-block EDID that is valid as far as libDDCRAM_EDID checks. */
static void make_edid(uint8_t *edid, uint8_t audio_channels)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    const uint8_t        cea[] = { 0x02, 0x03, 0x12, 0x00, 0x23, 0x09, 0x07, 0x07, 0x83, audio_channels,
                                   0x00, 0x00, 0x65, 0x03, 0x0c, 0x00, 0x30, 0x00 };
    uint8_t              sum = 0;

    memset(edid, 0, EDID_SIZE);
    memcpy(edid, header, sizeof header);
    edid[127] = 0x06; /* the header sums to 0xfa */
    memcpy(edid + 128, cea, sizeof cea);
    for (int i = 128; i < 255; i++)
        sum += edid[i];
    edid[255] = (uint8_t)-sum;
}

static void edid_file(const char *libpath, int port, const uint8_t *edid, size_t size)
{
    char copy[256], path[300];

    strcpy(copy, libpath);
    snprintf(path, sizeof path, "%s/DDCRAM_port%d.bin", dirname(copy), port);
    unlink(path);
    if (edid) {
        int fd = open(path, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0 && write(fd, edid, size) == (ssize_t)size);
        close(fd);
    }
}

static void test_ddcram_edid(const char *libpath)
{
    lib_init_fn *lib_init = load(libpath);
    uint8_t      stock[EDID_SIZE], surround[EDID_SIZE], broken[EDID_SIZE];

    make_edid(stock, 0x01);
    make_edid(surround, 0x0f);
    memcpy(broken, surround, EDID_SIZE);
    broken[200] ^= 1;
    for (int port = 0; port < PORTS; port++)
        memcpy(g_ddcram[port], stock, EDID_SIZE);

    puts("LOAD programs only valid files that differ, HPD pulses only those ports");
    edid_file(libpath, 0, stock, EDID_SIZE);
    edid_file(libpath, 1, surround, EDID_SIZE);
    edid_file(libpath, 2, broken, EDID_SIZE);
    edid_file(libpath, 3, NULL, 0);
    inject_into(lib_init, libpath, "LOAD HPD");
    CHECK(memcmp(g_ddcram[0], stock, EDID_SIZE) == 0 && memcmp(g_ddcram[1], surround, EDID_SIZE) == 0);
    CHECK(memcmp(g_ddcram[2], stock, EDID_SIZE) == 0 && memcmp(g_ddcram[3], stock, EDID_SIZE) == 0);
    CHECK(g_hpd_calls == 2 && g_hpd[0][0] == 0 && g_hpd[0][1] == 0x51 && g_hpd[1][0] == 1 && g_hpd[1][1] == 0x51);

    puts("a second LOAD finds nothing to do");
    inject_into(lib_init, libpath, "LOAD HPD");
    CHECK(g_hpd_calls == 2 && memcmp(g_ddcram[1], surround, EDID_SIZE) == 0);

    puts("files of the wrong size and bad arguments are refused");
    edid_file(libpath, 3, surround, EDID_SIZE - 1);
    edid_file(libpath, 2, surround, EDID_SIZE);
    inject_into(lib_init, libpath, "LOAD");
    CHECK(memcmp(g_ddcram[3], stock, EDID_SIZE) == 0 && memcmp(g_ddcram[2], surround, EDID_SIZE) == 0);
    edid_file(libpath, 0, surround, EDID_SIZE);
    inject_into(lib_init, libpath, "LOAD NOW");
    inject_into(lib_init, libpath, "DUMP");
    CHECK(g_hpd_calls == 2 && memcmp(g_ddcram[0], stock, EDID_SIZE) == 0);

    for (int port = 0; port < PORTS; port++)
        edid_file(libpath, port, NULL, 0);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_pdptune(argv[1]);
    test_ddcram_edid(argv[2]);

    puts(g_failures ? "FAILED" : "PASSED");
    return g_failures != 0;
}
