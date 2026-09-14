# Builds the headless bench harness. Requires the libretro core to already be
# built (make -C ../libretro); its filename is platform-dependent, so use
# ./core_path.sh rather than assuming .so.
CC ?= gcc
CFLAGS ?= -O2 -Wall

# mingw-w64 has no libdl (dlopen is POSIX/glibc-only; bench_runner.c uses
# dlfcn_win32.h instead, see there for why) and produces a .exe, not a
# bare binary. Detected from $(CC)'s name so a cross-build still works with
# `make CC=x86_64-w64-mingw32-gcc-posix` and no other flags.
ifneq (,$(findstring mingw,$(CC)))
	EXTRA_LIBS :=
	EXE := .exe
else
	EXTRA_LIBS := -ldl
	EXE :=
endif

bench_runner$(EXE): bench_runner.c
	$(CC) $(CFLAGS) -o $@ $< $(EXTRA_LIBS)

clean:
	rm -f bench_runner bench_runner.exe

.PHONY: clean
