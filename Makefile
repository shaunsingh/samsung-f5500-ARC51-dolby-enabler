CC      ?= arm-linux-gnueabi-gcc
LD      ?= arm-linux-gnueabi-ld
SYSLIB  ?= ./syslib

CFLAGS  := -march=armv7-a -mfloat-abi=soft -fPIC -O2 \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector \
           -Wall -Wextra

all: build/libDDCRAM_EDID.so

build:
	mkdir -p build

build/libDDCRAM_EDID.o: src/libDDCRAM_EDID.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# Link directly against the TV's own glibc 2.14.1 libraries pulled from
# /mtd_uncompexe/lib/ (libc-2.14.1.so -> libc.so.6, libdl-2.14.1.so -> libdl.so.2,
# libgcc_s.so.1) so that only GLIBC_2.4 symbols are referenced.
build/libDDCRAM_EDID.so: build/libDDCRAM_EDID.o
	$(LD) -shared -soname libDDCRAM_EDID.so -o $@ $< \
		$(SYSLIB)/libdl.so.2 $(SYSLIB)/libc.so.6 $(SYSLIB)/libgcc_s.so.1

clean:
	rm -rf build

.PHONY: all clean
