/*
 * Minimal dlopen/dlsym/dlclose/dlerror shim for MinGW-w64 builds of
 * bench_runner. mingw-w64 does not ship <dlfcn.h> (dlopen is a POSIX/glibc
 * API; Windows' native equivalent is LoadLibrary/GetProcAddress/FreeLibrary),
 * so this translates the handful of calls bench_runner.c actually makes.
 * Not a general-purpose dlfcn-win32 replacement, just enough for this file.
 */
#ifndef _S9X_BENCH_DLFCN_WIN32_H_
#define _S9X_BENCH_DLFCN_WIN32_H_

#include <windows.h>
#include <stdio.h>

/* bench_runner.c only ever passes RTLD_NOW | RTLD_LOCAL; Windows has no
 * lazy-binding/global-symbol-visibility equivalent to distinguish, so both
 * flags are accepted and ignored. */
#define RTLD_NOW	0x0002
#define RTLD_LOCAL	0x0000

static char s9x_dlerror_buf[256];

static inline void *dlopen(const char *path, int flags)
{
	(void) flags;
	HMODULE h = LoadLibraryA(path);
	if (!h)
		snprintf(s9x_dlerror_buf, sizeof(s9x_dlerror_buf),
			"LoadLibraryA failed, GetLastError=%lu", GetLastError());
	return (void *) h;
}

static inline void *dlsym(void *handle, const char *name)
{
	FARPROC p = GetProcAddress((HMODULE) handle, name);
	if (!p)
		snprintf(s9x_dlerror_buf, sizeof(s9x_dlerror_buf),
			"GetProcAddress(%s) failed, GetLastError=%lu", name, GetLastError());
	return (void *) p;
}

static inline int dlclose(void *handle)
{
	return FreeLibrary((HMODULE) handle) ? 0 : -1;
}

static inline char *dlerror(void)
{
	return s9x_dlerror_buf[0] ? s9x_dlerror_buf : NULL;
}

#endif
