# samyGOso payloads for exeTV: ARMv7, soft-float EABI, glibc 2.14.1.
#
#   make            build/*.so
#   make test       run them against test/fake_exetv.c under qemu-arm
#   make lint       shellcheck the boot scripts
#   make prebuilt   copy build/*.so to prebuilt/

CROSS ?= arm-linux-gnueabi-
CC    := $(CROSS)gcc
NM    := $(CROSS)nm
STRIP := $(CROSS)strip
QEMU  ?= qemu-arm -L /usr/$(CROSS:-=)

LIBS := build/libDDCRAM_EDID.so build/libPDPTune.so
ARCH := -march=armv7-a -marm -mfloat-abi=soft
WARN := -std=gnu11 -Wall -Wextra -Werror

# A payload names no libraries of its own: once inside exeTV its imports bind
# to the libc already loaded there, whatever toolchain built it. The hardening
# that distribution compilers default to would import helpers, so it is off.
CFLAGS  := $(ARCH) $(WARN) -O2 -fPIC -fvisibility=hidden -fno-stack-protector -U_FORTIFY_SOURCE
LDFLAGS := -shared -nostdlib -Wl,--hash-style=both

# libPDPTune leaves hooks and a thread behind, so it must never be unloaded.
build/libPDPTune.so: LDFLAGS += -Wl,-z,nodelete

# What a payload may import: symbols the TV's libc, libdl and libpthread have.
# The headers of a newer libc redirect some functions to symbols those lack
# (strtol to __isoc23_strtol, for one), which would only show on the TV.
IMPORTS := __errno_location __isoc99_fscanf close dlclose dlopen dlsym fclose fopen64 free malloc memcmp memset \
           mmap64 mprotect munmap open64 pthread_create pthread_detach pthread_mutex_lock pthread_mutex_unlock \
           read snprintf sprintf strcasecmp strcmp strerror strlen strncasecmp strrchr sysconf usleep vsnprintf \
           write

# Where the firmware has what libPDPTune looks up; see g_firmware there.
FIRMWARE := write_i2c=0x006ece6c read_i2c=0x006ec0d4 panel_temp=0x006ec3e0 support_option=0x00637550 \
            logic_ic=0x01f44b34 logic_fhd_vtable=0x018aea00

all: $(LIBS)

build:
	mkdir -p $@

build/%.so: src/%.c src/samygo.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-soname,$(@F) $< -o $@ -lgcc
	@unknown=$$($(NM) -D --undefined-only $@ | awk '{ print $$2 }' | grep -vxF $(IMPORTS:%=-e %)); \
	if [ -n "$$unknown" ]; then \
		echo "$@ imports symbols the TV may lack:" $$unknown >&2; rm -f $@; exit 1; \
	fi
	$(STRIP) $@

build/fake_exetv: test/fake_exetv.c | build
	$(CC) $(ARCH) $(WARN) -O1 -no-pie -rdynamic $(FIRMWARE:%=-Wl,--section-start=.fw.%) $< -o $@ -lpthread -ldl

test: $(LIBS) build/fake_exetv
	$(QEMU) build/fake_exetv $(abspath build/libPDPTune.so build/libDDCRAM_EDID.so)

lint:
	shellcheck -s sh init/*.init init/run1.sh

prebuilt: $(LIBS)
	cp $(LIBS) prebuilt/

clean:
	rm -rf build

.PHONY: all test lint prebuilt clean
