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

#define WRITE_I2C      "_ZN8LogicFHD16m_Logic_WriteI2CEtt"
#define READ_I2C       "_ZN8LogicFHD15m_Logic_ReadI2CEtPt"
#define PANEL_TEMP     "_ZN8LogicFHD20t_Logic_GetPanelTempEPi"
#define SUPPORT_OPTION "_ZN14PrdInfoManager18CheckSupportOptionEN12IProductInfo22ProductSupportOption_kEPb"

FIRMWARE_FUNCTION(".fw.write_i2c", WRITE_I2C, "body_write_i2c");
FIRMWARE_FUNCTION(".fw.read_i2c", READ_I2C, "body_read_i2c");
FIRMWARE_FUNCTION(".fw.panel_temp", PANEL_TEMP, "body_panel_temp");
FIRMWARE_FUNCTION(".fw.support_option", SUPPORT_OPTION, "body_support_option");

int fw_write_i2c(void *logic, uint16_t reg, uint16_t value) __asm__(WRITE_I2C);
int fw_support_option(void *self, int option, bool *supported) __asm__(SUPPORT_OPTION);

void *vtable_logic_fhd[49] __asm__("_ZTV8LogicFHD") __attribute__((section(".fw.logic_fhd_vtable")));
void *logic_ic __asm__("_ZN8TDcLogic11m_poLogicICE") __attribute__((section(".fw.logic_ic")));

enum { DRIVE = 0x806f, DEEP_BLACK = 0x809c, PEAK_STRETCH = 0x8063, DUTY2 = 0x89da, SUB_CONTRAST = 0x8927, OTHER = 0x803d };

static struct { void *vptr; } g_logic = { &vtable_logic_fhd[2] };
static _Atomic uint16_t      g_panel[0x10000];
static pthread_mutex_t       g_bus = PTHREAD_MUTEX_INITIALIZER;

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
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    fw_write_i2c(&g_logic, SUB_CONTRAST, 0x0800);

    puts("bad arguments change nothing");
    inject("DRIVE:STANDARD DUTY2:161");
    inject("DRIVE:FAST");
    inject("SUBCONTRAST:13x");
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
    CHECK(hooked(fw_write_i2c) && wb10p());
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
    inject("DEEPBLACK:ON PEAKSTRETCH:ON DUTY2:139 SUBCONTRAST:132 DUMP");
    EVENTUALLY(g_panel[SUB_CONTRAST] == 0x0840 && !hooked(fw_support_option));
    CHECK(g_panel[DRIVE] == 0x05 && g_panel[DEEP_BLACK] == 3 && g_panel[PEAK_STRETCH] == 1 && g_panel[DUTY2] == 139);
    CHECK(hooked(fw_write_i2c) && !wb10p());
    fw_write_i2c(&g_logic, DUTY2, 0);
    CHECK(g_panel[DUTY2] == 0);
    fw_write_i2c(&g_logic, DUTY2, 0x80);
    CHECK(g_panel[DUTY2] == 139);
    fw_write_i2c(&g_logic, DEEP_BLACK, 0);
    fw_write_i2c(&g_logic, PEAK_STRETCH, 1);
    CHECK(g_panel[DEEP_BLACK] == 3 && g_panel[PEAK_STRETCH] == 1);

    puts("OFF restores what the firmware last asked for");
    inject("DEEPBLACK:OFF");
    EVENTUALLY(g_panel[DUTY2] == 0x80);
    CHECK(g_panel[DEEP_BLACK] == 0 && g_panel[PEAK_STRETCH] == 1 && g_panel[SUB_CONTRAST] == 0x0800);
    inject("OFF");
    EVENTUALLY(!hooked(fw_write_i2c));
    CHECK(g_panel[DRIVE] == 0x05 && g_panel[DEEP_BLACK] == 0 && g_panel[PEAK_STRETCH] == 1);
    CHECK(g_panel[DUTY2] == 0x80 && g_panel[SUB_CONTRAST] == 0x0800 && !hooked(fw_support_option));

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
