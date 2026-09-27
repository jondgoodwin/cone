/** panic - the end of a program that cannot go on
 * @file
 *
 * Every panic ends here: core's 'panic' (and 'assert', 'unreachable' and
 * 'todo', which call it), and the checks the compiler inserts (genlPanic): an
 * index out of bounds, a slice's range out of bounds, a region's allocation
 * that failed. What happens is one fixed sequence:
 *
 * - stdout is flushed, so what the program printed before is kept;
 * - one line goes to stderr, 'panic at <file>:<line>: <message>', naming the
 *   thread ('panic in thread <id> at ...') when it is not the one the program
 *   started on;
 * - the hook the program set with core's 'setPanicHook', if any, is called
 *   with the message and the location. It runs after the line is written, so
 *   a hook that fails cannot lose the report;
 * - the program ends through the C library's 'abort': SIGABRT, or on Windows
 *   a fail-fast (exit status 0xC0000409), where a debugger stops. Nothing
 *   unwinds, and no finalizer runs.
 *
 * A second panic on a thread already panicking -- the hook panicking, say --
 * writes one line and aborts at once.
 *
 * Cone passes a '&[]u8' as its two words, the pointer and the length, so each
 * slice parameter below is two.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#define THREADLOCAL __declspec(thread)
#define NORETURN __declspec(noreturn)
#else
#include <pthread.h>
#define THREADLOCAL _Thread_local
#define NORETURN __attribute__((noreturn))
#endif

// The hook a program sets with core's 'setPanicHook', or NULL: a Cone
// function taking the message, the file and the line
typedef void (*ConePanicHook)(const char *msg, size_t msglen,
                              const char *file, size_t filelen, uint32_t line);
static ConePanicHook volatile panicHook = NULL;

// Whether this thread is panicking already
static THREADLOCAL int panicking = 0;

// This thread's identity, and the thread the program started on
#ifdef _WIN32
typedef unsigned long ConeThreadId;
static ConeThreadId threadId(void) { return GetCurrentThreadId(); }
#else
typedef unsigned long ConeThreadId;
static ConeThreadId threadId(void) { return (unsigned long)pthread_self(); }
#endif
static ConeThreadId mainThread = 0;

// Record the thread the program starts on, before 'main' runs: the C
// runtime's initializers run on it
static void panicInit(void) {
    mainThread = threadId();
}
#ifdef _MSC_VER
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*cone_panicInitEntry)(void) = panicInit;
#else
__attribute__((constructor)) static void panicInitCtor(void) { panicInit(); }
#endif

void cone_setPanicHook(ConePanicHook hook) {
#ifdef _WIN32
    _InterlockedExchangePointer((void * volatile *)&panicHook, (void *)hook);
#else
    __atomic_store_n(&panicHook, hook, __ATOMIC_SEQ_CST);
#endif
}

static ConePanicHook panicHookGet(void) {
#ifdef _WIN32
    return (ConePanicHook)_InterlockedCompareExchangePointer((void * volatile *)&panicHook, NULL, NULL);
#else
    return __atomic_load_n(&panicHook, __ATOMIC_SEQ_CST);
#endif
}

NORETURN void cone_panic(const char *msg, size_t msglen,
                         const char *file, size_t filelen, uint32_t line) {
    fflush(stdout);
    if (panicking) {
        fprintf(stderr, "panic while panicking, at %.*s:%u: %.*s\n",
            (int)filelen, file, (unsigned)line, (int)msglen, msg);
        fflush(stderr);
        abort();
    }
    panicking = 1;

    ConeThreadId self = threadId();
    if (self == mainThread)
        fprintf(stderr, "panic at %.*s:%u: %.*s\n",
            (int)filelen, file, (unsigned)line, (int)msglen, msg);
    else
        fprintf(stderr, "panic in thread %lu at %.*s:%u: %.*s\n",
            self, (int)filelen, file, (unsigned)line, (int)msglen, msg);
    fflush(stderr);

    ConePanicHook hook = panicHookGet();
    if (hook)
        hook(msg, msglen, file, filelen, line);
    fflush(stdout);
    fflush(stderr);
    abort();
}

// The compiler's checks: each formats what it reports, then panics

NORETURN void cone_panicIndex(size_t index, size_t count,
                              const char *file, size_t filelen, uint32_t line) {
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "index %zu is out of bounds for a count of %zu", index, count);
    cone_panic(msg, (size_t)len, file, filelen, line);
}

NORETURN void cone_panicSlice(size_t start, size_t end, size_t count,
                              const char *file, size_t filelen, uint32_t line) {
    char msg[160];
    int len = start > end
        ? snprintf(msg, sizeof(msg), "slice %zu..%zu starts after it ends", start, end)
        : snprintf(msg, sizeof(msg), "slice %zu..%zu is out of bounds for a count of %zu", start, end, count);
    cone_panic(msg, (size_t)len, file, filelen, line);
}

NORETURN void cone_panicAlloc(size_t size,
                              const char *file, size_t filelen, uint32_t line) {
    char msg[96];
    int len = snprintf(msg, sizeof(msg), "out of memory allocating %zu bytes", size);
    cone_panic(msg, (size_t)len, file, filelen, line);
}
