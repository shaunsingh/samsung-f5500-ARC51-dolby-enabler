/*
 * libDDCRAM_EDID - program the HDMI EDIDs the MStar MST12 serves to sources.
 *
 * A samyGOso payload for exeTV. The receiver answers EDID reads from on-chip
 * DDC RAM, which the firmware fills at power-on. Words on the command line:
 *
 *   DUMP   save each port's EDID to /dtv/DDCRAM_port<n>.bin (the default)
 *   LOAD   program DDCRAM_port<n>.bin from the library's directory into
 *          every port whose EDID differs from it
 *   HPD    pulse hot-plug detect on reprogrammed ports so sources re-read
 *
 * The result is logged to /dtv/DDCRAM_EDID.log.
 */
#include <dlfcn.h>
#include <stdbool.h>

#include "samygo.h"

#define LOG      "/dtv/DDCRAM_EDID.log"
#define say(...) sgo_log(LOG, __VA_ARGS__)

#define PORTS      4
#define EDID_SIZE  256
#define HPD_PORT0  0x50 /* MApi_XC_Sys_SetHPD's number for the first HDMI input */
#define HPD_LOW_US 800000

/* XC_DDCRAM_PROG_INFO of libUTOPIA */
struct ddcram {
    uint8_t *edid;
    uint16_t size;
    uint32_t port;
};

typedef void ddcram_fn(struct ddcram *info, uint32_t unused);
typedef void hpd_fn(uint32_t enable, uint32_t port);

static bool edid_valid(const uint8_t *edid)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    uint8_t              sum[2] = { 0, 0 };

    for (int i = 0; i < EDID_SIZE; i++)
        sum[i / 128] += edid[i];
    return memcmp(edid, header, sizeof header) == 0 && sum[0] == 0 && sum[1] == 0;
}

static int hex(char *out, const uint8_t *bytes, int count)
{
    for (int i = 0; i < count; i++)
        sprintf(out + 2 * i, "%02x", bytes[i]);
    return 2 * count;
}

/* Logs what matters for audio: the physical address, the audio and the speaker allocation blocks. */
static void describe(const char *what, unsigned port, const uint8_t *edid)
{
    const uint8_t *cea = edid + 128;
    char           audio[2 * 32 + 1] = "-", speakers[2 * 32 + 1] = "-";
    unsigned       address = 0;

    if (cea[0] == 0x02 && cea[1] >= 0x03) {
        int end = cea[2] < 127 ? cea[2] : 127;
        for (int at = 4, length; at < end; at += 1 + length) {
            const uint8_t *block = cea + at;
            length = block[0] & 0x1f;
            if (at + 1 + length > end)
                break;
            switch (block[0] >> 5) {
            case 1:
                hex(audio, block, 1 + length);
                break;
            case 4:
                hex(speakers, block, 1 + length);
                break;
            case 3:
                if (length >= 5 && block[1] == 0x03 && block[2] == 0x0c && block[3] == 0x00)
                    address = (unsigned)block[4] << 8 | block[5];
                break;
            }
        }
    }
    say("[%s] port=%u valid=%d phys=%u.%u.%u.%u ADB=%s SADB=%s", what, port, edid_valid(edid), address >> 12,
        address >> 8 & 15, address >> 4 & 15, address & 15, audio, speakers);
}

static void transfer(ddcram_fn *function, unsigned port, uint8_t *edid)
{
    struct ddcram info = { .edid = edid, .size = EDID_SIZE, .port = port };
    function(&info, 0);
}

static bool load_file(const char *path, uint8_t *edid)
{
    uint8_t extra;
    int     fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool whole = read(fd, edid, EDID_SIZE) == EDID_SIZE && read(fd, &extra, 1) == 0;
    close(fd);
    return whole;
}

static void save_file(const char *path, const uint8_t *edid)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, edid, EDID_SIZE) != EDID_SIZE)
        say("cannot write %s", path);
    close(fd);
}

void lib_init(void *handle, const char *libpath)
{
    (void)handle;

    const char *argv[8];
    int         argc = sgo_args(libpath, argv, 8);
    if (argc > 8) {
        say("too many arguments, nothing done");
        return;
    }
    bool dump = false, load = false, pulse = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "DUMP") == 0)
            dump = true;
        else if (strcmp(argv[i], "LOAD") == 0)
            load = true;
        else if (strcmp(argv[i], "HPD") == 0)
            pulse = true;
        else {
            say("bad argument '%s', nothing done", argv[i]);
            return;
        }
    }
    if (!load)
        dump = true;

    void      *exetv = dlopen(NULL, RTLD_LAZY);
    ddcram_fn *read_ddcram = NULL, *prog_ddcram = NULL;
    hpd_fn    *set_hpd = NULL;
    if (exetv) {
        read_ddcram = (ddcram_fn *)dlsym(exetv, "MDrv_HDMI_READ_DDCRAM");
        prog_ddcram = (ddcram_fn *)dlsym(exetv, "MDrv_HDMI_PROG_DDCRAM");
        set_hpd = (hpd_fn *)dlsym(exetv, "MApi_XC_Sys_SetHPD");
        dlclose(exetv);
    }
    if (!read_ddcram || !prog_ddcram || !set_hpd) {
        say("this exeTV lacks the MStar DDC RAM functions, nothing done");
        return;
    }

    const char *slash = strrchr(libpath, '/');
    int         directory = slash ? (int)(slash - libpath) : 0;
    bool        changed[PORTS] = { false }, any_changed = false;

    for (unsigned port = 0; port < PORTS; port++) {
        uint8_t now[EDID_SIZE] = { 0 }, want[EDID_SIZE];
        char    path[256];

        transfer(read_ddcram, port, now);
        if (dump) {
            describe("DUMP", port, now);
            snprintf(path, sizeof path, "/dtv/DDCRAM_port%u.bin", port);
            save_file(path, now);
        }
        if (!load)
            continue;

        snprintf(path, sizeof path, "%.*s/DDCRAM_port%u.bin", directory, slash ? libpath : ".", port);
        if (!load_file(path, want) || !edid_valid(want)) {
            say("[LOAD-SKIP] port=%u: %s is not a valid %d-byte EDID", port, path, EDID_SIZE);
            continue;
        }
        if (memcmp(now, want, EDID_SIZE) == 0) {
            describe("LOAD-ALREADY-OK", port, now);
            continue;
        }

        transfer(prog_ddcram, port, want);
        memset(now, 0, sizeof now);
        transfer(read_ddcram, port, now);
        changed[port] = memcmp(now, want, EDID_SIZE) == 0;
        any_changed |= changed[port];
        describe(changed[port] ? "LOAD-OK" : "LOAD-MISMATCH", port, now);
    }

    if (pulse && any_changed) {
        for (unsigned port = 0; port < PORTS; port++)
            if (changed[port])
                set_hpd(0, HPD_PORT0 + port);
        usleep(HPD_LOW_US);
        for (unsigned port = 0; port < PORTS; port++) {
            if (changed[port]) {
                set_hpd(1, HPD_PORT0 + port);
                say("[HPD] port=%u pulsed", port);
            }
        }
    }
}

void lib_deinit(void *handle)
{
    (void)handle;
}
