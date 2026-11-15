/** backtrace - the stack at a panic, as raw frames
 * @file
 *
 * A panic prints 'stack backtrace:' and the frames this file captures when
 * the environment variable CONE_BACKTRACE is set to anything but empty or
 * '0' (panic.cone). The capture is for a process that is about to abort:
 * a fixed array from the caller, no allocation of its own, no symbol loading.
 * Names and lines come later, from Congo, which reads each frame's module
 * and offset against the program's debug information.
 *
 * Windows walks by the unwind data (.pdata) with RtlCaptureStackBackTrace and
 * names the module with RtlPcToFileHeader and GetModuleFileNameA. Linux walks
 * .eh_frame with _Unwind_Backtrace (glibc's backtrace() can allocate on its
 * first call, to load libgcc_s) and names the module with dladdr.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Whether CONE_BACKTRACE asks for frames: set, not empty, not "0"
int32_t cone_backtraceOn(void) {
    const char *v = getenv("CONE_BACKTRACE");
    if (v == NULL || v[0] == 0) return 0;
    if (v[0] == '0' && v[1] == 0) return 0;
    return 1;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Up to max return addresses of the calling thread's stack, after skipping
// 'skip' frames beyond this function's own; how many were found
uint32_t cone_captureFrames(uint64_t *frames, uint32_t max, uint32_t skip) {
    void *found[64];
    if (max > 64) max = 64;
    USHORT n = RtlCaptureStackBackTrace(1 + skip, max, found, NULL);
    for (USHORT i = 0; i < n; i++) frames[i] = (uint64_t)(uintptr_t)found[i];
    return n;
}

// The module holding addr: its file's base name into name (cap bytes, NUL
// ended; length returned, 0 for none) and addr's offset from its base
uint32_t cone_frameModule(uint64_t addr, uint8_t *name, uint32_t cap, uint64_t *offset) {
    PVOID base = NULL;
    *offset = 0;
    if (cap == 0) return 0;
    name[0] = 0;
    if (RtlPcToFileHeader((PVOID)(uintptr_t)addr, &base) == NULL) return 0;
    char path[512];
    DWORD n = GetModuleFileNameA((HMODULE)base, path, sizeof path);
    if (n == 0 || n >= sizeof path) return 0;
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t len = strlen(b);
    if (len >= cap) len = cap - 1;
    memcpy(name, b, len);
    name[len] = 0;
    *offset = addr - (uint64_t)(uintptr_t)base;
    return (uint32_t)len;
}

#else
#include <dlfcn.h>
#include <unwind.h>

struct walk { uint64_t *frames; uint32_t max, count, skip; };

static _Unwind_Reason_Code visit(struct _Unwind_Context *ctx, void *arg) {
    struct walk *w = (struct walk *)arg;
    uintptr_t pc = (uintptr_t)_Unwind_GetIP(ctx);
    if (pc == 0) return _URC_END_OF_STACK;
    if (w->skip > 0) { w->skip--; return _URC_NO_REASON; }
    if (w->count >= w->max) return _URC_END_OF_STACK;
    w->frames[w->count++] = pc;
    return _URC_NO_REASON;
}

uint32_t cone_captureFrames(uint64_t *frames, uint32_t max, uint32_t skip) {
    struct walk w = { frames, max, 0, 1 + skip };
    _Unwind_Backtrace(visit, &w);
    return w.count;
}

uint32_t cone_frameModule(uint64_t addr, uint8_t *name, uint32_t cap, uint64_t *offset) {
    Dl_info info;
    *offset = 0;
    if (cap == 0) return 0;
    name[0] = 0;
    if (dladdr((void *)(uintptr_t)addr, &info) == 0 || info.dli_fname == NULL) return 0;
    const char *b = info.dli_fname;
    for (const char *p = info.dli_fname; *p; p++) if (*p == '/') b = p + 1;
    size_t len = strlen(b);
    if (len == 0) return 0;
    if (len >= cap) len = cap - 1;
    memcpy(name, b, len);
    name[len] = 0;
    *offset = addr - (uint64_t)(uintptr_t)info.dli_fbase;
    return (uint32_t)len;
}
#endif
