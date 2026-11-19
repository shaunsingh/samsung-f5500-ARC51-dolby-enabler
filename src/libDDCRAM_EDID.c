#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_PATH "/dtv/DDCRAM_EDID.log"

typedef struct {
    unsigned char *pu8EDID;
    unsigned short u16Size;
    unsigned int   ePort;
} XC_DDCRAM_PROG_INFO;

typedef void (*fn_ddcr_t)(XC_DDCRAM_PROG_INFO *info, unsigned int unused);
typedef void (*fn_hpd_t)(unsigned int enable, unsigned int port);

static void logmsg(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    fclose(f);
}

static int valid_edid(const unsigned char *b) {
    static const unsigned char hdr[8] = {0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00};
    if (memcmp(b, hdr, 8) != 0) return 0;
    unsigned int s0 = 0, s1 = 0;
    for (int i = 0; i < 128; i++) s0 += b[i];
    for (int i = 128; i < 256; i++) s1 += b[i];
    return ((s0 & 0xff) == 0) && ((s1 & 0xff) == 0);
}

static void summarize_edid(const char *tag, int port, const unsigned char *b) {
    int ok = valid_edid(b);
    unsigned char pa_hi = 0, pa_lo = 0;
    int audio_off = -1, audio_len = 0;
    int sadb_off = -1;
    if (b[128] == 0x02 && b[129] >= 0x03) {
        int dtd = b[130];
        int p = 132;
        while (p < 128 + dtd && p < 255) {
            int t = (b[p] >> 5) & 7;
            int l = b[p] & 0x1f;
            if (p + 1 + l > 255) break;
            if (t == 1) { audio_off = p; audio_len = l; }
            else if (t == 4 && l >= 3) { sadb_off = p; }
            else if (t == 3 && l >= 5 && b[p+1]==0x03 && b[p+2]==0x0c && b[p+3]==0x00) {
                pa_hi = b[p+4]; pa_lo = b[p+5];
            }
            p += 1 + l;
        }
    }
    logmsg("[%s] port=%d valid=%d phys=%d.%d.%d.%d hdr=%02x%02x%02x%02x",
           tag, port, ok,
           (pa_hi>>4)&0xf, pa_hi&0xf, (pa_lo>>4)&0xf, pa_lo&0xf,
           b[0], b[1], b[2], b[3]);
    if (audio_off >= 0) {
        logmsg(" ADB=");
        for (int i = 0; i <= audio_len; i++) logmsg("%02x", b[audio_off+i]);
    }
    if (sadb_off >= 0) {
        logmsg(" SADB=%02x%02x%02x%02x", b[sadb_off], b[sadb_off+1], b[sadb_off+2], b[sadb_off+3]);
    }
    logmsg("\n");
}

static int parse_samygo_args(const char *libpath, const char **argv, int max_args) {
    if (!libpath || max_args < 1) return 0;
    const uint32_t EXTRA_COOKIE = 0x82374021u;
    int argc = 1;
    argv[0] = libpath;
    uintptr_t p = (uintptr_t)(libpath + strlen(libpath) + 1);
    p = (p + 3u) & ~(uintptr_t)3u;
    const uint32_t *extra = (const uint32_t *)p;
    if (extra[0] != EXTRA_COOKIE) return argc;
    uint32_t n = extra[1];
    const uint32_t *offs = &extra[2];
    for (uint32_t i = 0; i < n && argc < max_args; i++) {
        argv[argc++] = (const char *)(p + offs[i]);
    }
    return argc;
}

void lib_init(void *_h, const char *libpath) {
    (void)_h;
    int do_dump = 0, do_load = 0, do_hpd = 0;
    const char *argv[32];
    int argc = parse_samygo_args(libpath, argv, 32);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "DUMP") == 0) do_dump = 1;
        else if (strcmp(argv[i], "LOAD") == 0) do_load = 1;
        else if (strcmp(argv[i], "HPD") == 0) do_hpd = 1;
    }
    if (!do_dump && !do_load) do_dump = 1;

    logmsg("=== libDDCRAM_EDID start (argc=%d path=[%s] DUMP=%d LOAD=%d HPD=%d) ===\n",
           argc, libpath ? libpath : "", do_dump, do_load, do_hpd);

    void *h = dlopen(0, RTLD_LAZY);
    if (!h) {
        logmsg("dlopen(0) failed\n");
        return;
    }

    fn_ddcr_t fn_read = (fn_ddcr_t)dlsym(h, "MDrv_HDMI_READ_DDCRAM");
    fn_ddcr_t fn_prog = (fn_ddcr_t)dlsym(h, "MDrv_HDMI_PROG_DDCRAM");
    fn_hpd_t  fn_hpd  = (fn_hpd_t)dlsym(h, "MApi_XC_Sys_SetHPD");
    logmsg("syms: READ=%p PROG=%p HPD=%p\n", fn_read, fn_prog, fn_hpd);

    if (do_dump && fn_read) {
        for (unsigned int port = 0; port < 4; port++) {
            unsigned char buf[256];
            memset(buf, 0, sizeof(buf));
            XC_DDCRAM_PROG_INFO info;
            info.pu8EDID = buf;
            info.u16Size = 256;
            info.ePort = port;
            fn_read(&info, 0);
            summarize_edid("DUMP", (int)port, buf);
            char outp[64];
            snprintf(outp, sizeof(outp), "/dtv/DDCRAM_port%u.bin", port);
            FILE *f = fopen(outp, "wb");
            if (f) { fwrite(buf, 1, 256, f); fclose(f); }
        }
    }

    if (do_load && fn_prog && fn_read) {
        int updated[4] = {0, 0, 0, 0};
        for (unsigned int port = 0; port < 4; port++) {
            char inp[128];
            snprintf(inp, sizeof(inp), "/mnt/opt/privateer/usr/libso/DDCRAM_port%u.bin", port);
            FILE *f = fopen(inp, "rb");
            if (!f) {
                logmsg("[LOAD] port=%u no file %s (skip)\n", port, inp);
                continue;
            }
            unsigned char buf[256];
            size_t n = fread(buf, 1, 256, f);
            fclose(f);
            if (n != 256 || !valid_edid(buf)) {
                logmsg("[LOAD] port=%u invalid EDID (%u bytes), refusing to write\n", port, (unsigned)n);
                continue;
            }

            XC_DDCRAM_PROG_INFO curi;
            unsigned char cur[256];
            memset(cur, 0, sizeof(cur));
            curi.pu8EDID = cur;
            curi.u16Size = 256;
            curi.ePort = port;
            fn_read(&curi, 0);
            if (memcmp(cur, buf, 256) == 0) {
                summarize_edid("LOAD-ALREADY-OK", (int)port, cur);
                continue;
            }

            XC_DDCRAM_PROG_INFO info;
            info.pu8EDID = buf;
            info.u16Size = 256;
            info.ePort = port;
            fn_prog(&info, 0);

            unsigned char rb[256];
            memset(rb, 0, sizeof(rb));
            XC_DDCRAM_PROG_INFO rbi;
            rbi.pu8EDID = rb;
            rbi.u16Size = 256;
            rbi.ePort = port;
            fn_read(&rbi, 0);
            int match = (memcmp(buf, rb, 256) == 0);
            summarize_edid(match ? "LOAD-OK" : "LOAD-MISMATCH", (int)port, rb);
            if (match) updated[port] = 1;
        }
        if (do_hpd && fn_hpd) {
            for (unsigned int port = 0; port < 4; port++) {
                if (updated[port]) {
                    logmsg("[HPD] pulsing port=%u (0x%x) LOW\n", port, 0x50 + port);
                    fn_hpd(0, 0x50 + port);
                }
            }
            usleep(800000);
            for (unsigned int port = 0; port < 4; port++) {
                if (updated[port]) {
                    logmsg("[HPD] pulsing port=%u (0x%x) HIGH\n", port, 0x50 + port);
                    fn_hpd(1, 0x50 + port);
                }
            }
        }
    }

    dlclose(h);
    logmsg("=== libDDCRAM_EDID done ===\n");
}

void lib_deinit(void *_h) {
    (void)_h;
}
