/*
 * Helpers shared by the samyGOso payloads in this repository.
 *
 * samyGOso (v1.2.5) attaches to a running process with ptrace, borrows its
 * main thread wherever that thread happens to be, and makes it run:
 *
 *     h = dlopen(libpath, RTLD_LAZY);
 *     lib_init(h, libpath);
 *     if (!resident) { lib_deinit(h); dlclose(h); }      // "-r" = resident
 *
 * The process's other threads keep running meanwhile. Injecting a library
 * that is still loaded runs lib_init again in the same image.
 */
#ifndef SAMYGO_H
#define SAMYGO_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SGO_EXPORT __attribute__((visibility("default")))

SGO_EXPORT void lib_init(void *handle, const char *libpath);
SGO_EXPORT void lib_deinit(void *handle);

/*
 * The words given after the library on the samyGOso command line follow the
 * library path in the same buffer: after the path's NUL, aligned to 4 bytes,
 * come a cookie, a count and one offset per word, each relative to the cookie.
 *
 * Stores up to max words and returns how many there are.
 */
#define SGO_ARGS_COOKIE 0x82374021u

static inline int sgo_args(const char *libpath, const char **argv, int max)
{
    uintptr_t base = ((uintptr_t)libpath + strlen(libpath) + 1 + 3) & ~(uintptr_t)3;
    const uint32_t *header = (const uint32_t *)base;
    if (header[0] != SGO_ARGS_COOKIE)
        return 0;

    int argc = (int)header[1];
    for (int i = 0; i < argc && i < max; i++)
        argv[i] = (const char *)(base + header[2 + i]);
    return argc;
}

/* Appends one line with a single write(), so lines from different threads never interleave. */
__attribute__((format(printf, 2, 3)))
static inline void sgo_log(const char *path, const char *format, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, format);
    int length = vsnprintf(line, sizeof line - 1, format, ap);
    va_end(ap);
    if (length < 0)
        return;
    if (length > (int)sizeof line - 2)
        length = (int)sizeof line - 2;
    line[length++] = '\n';

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, line, (size_t)length) < 0) {
        /* Nowhere left to report a failed log write. */
    }
    close(fd);
}

#endif
