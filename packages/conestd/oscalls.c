/** oscalls - the operating system's calls that Cone cannot spell, one set a system
 * @file
 *
 * The sync and thread packages sleep, wake and identify threads through the
 * operating system. On Windows they call kernel32 and the Synchronization
 * library directly. On Linux what they need is not a C library function Cone
 * can declare: the futex is one system call with a variadic C wrapper
 * ('syscall'), the thread's identity another ('gettid' is not in every C
 * library), the processor count a 'sysconf' name whose number differs between
 * systems, and the process-wide memory barrier a system call with a fallback
 * when the kernel refuses it. This file is those, each under the name the
 * packages bind: 'cone_futexWait', 'cone_futexWake', 'cone_threadId',
 * 'cone_processors' and 'cone_flushProcessWriteBuffers'. Everything else the
 * packages need on Linux (pthread_create and pthread_join, sched_yield,
 * nanosleep) is POSIX, and the packages bind it by name; those are shared with
 * macOS, which this file's Linux half is not: macOS has no futex, so its
 * 'cone_futexWait' and 'cone_futexWake' would be 'os_sync_wait_on_address'
 * (or '__ulock_wait'), its thread identity 'pthread_threadid_np' and its
 * barrier another kind of fence.
 *
 * Nothing here is compiled on Windows.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

// Not empty on any system (an empty source file is not ISO C)
typedef int cone_oscalls_t;

#if defined(__linux__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <linux/membarrier.h>

// Sleep while *word holds 'expected', until woken or 'timeoutNs' passes
// (UINT64_MAX is no limit). 0 only if the timeout passed; 1 for every other
// return, a wake, a word that held another value, or a signal, since a caller
// loops over what it waits for. The futex is private: the word is this
// process's alone, so the kernel hashes it by address and need not look up its
// page.
int cone_futexWait(uint32_t *word, uint32_t expected, uint64_t timeoutNs) {
    struct timespec limit;
    struct timespec *timeout = NULL;
    if (timeoutNs != UINT64_MAX) {
        limit.tv_sec = (time_t)(timeoutNs / 1000000000u);
        limit.tv_nsec = (long)(timeoutNs % 1000000000u);
        timeout = &limit;
    }
    long result = syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected, timeout, NULL, 0);
    return !(result == -1 && errno == ETIMEDOUT);
}

// Wake up to 'count' threads asleep on *word: one, or (INT_MAX) all
void cone_futexWake(uint32_t *word, int count) {
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

// The kernel's id for the calling thread, which is never 0
uint32_t cone_threadId(void) {
    return (uint32_t)syscall(SYS_gettid);
}

// How many logical processors are online, at least 1
int cone_processors(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

// Every processor running a thread of this process drains its stores, as if
// each had made a full fence where it stands: the heavy half of an
// asymmetric fence (Windows' FlushProcessWriteBuffers). The kernel's
// 'membarrier' with MEMBARRIER_CMD_PRIVATE_EXPEDITED does exactly this, after
// the process has registered for it once. It exists since Linux 4.14 and a
// sandbox may refuse it, so when the registration fails the same effect comes
// from the other way .NET makes it: changing the protection of a dirty page of
// the process's own makes the kernel interrupt every processor running it, to
// flush their translations, and an interrupt is a full fence on the processor
// that takes it. Only one thread changes the page at a time.
static pthread_once_t barrierOnce = PTHREAD_ONCE_INIT;
static int barrierKernel = 0;         // 1 if the kernel's membarrier registered
static void *barrierPage = NULL;      // otherwise, a page to change the protection of
static size_t barrierPageSize = 0;
static pthread_mutex_t barrierLock = PTHREAD_MUTEX_INITIALIZER;

static void barrierInit(void) {
#ifndef CONE_NO_MEMBARRIER
    if (syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0) {
        barrierKernel = 1;
        return;
    }
#endif
    long size = sysconf(_SC_PAGESIZE);
    barrierPageSize = size > 0 ? (size_t)size : 4096;
    barrierPage = mmap(NULL, barrierPageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (barrierPage == MAP_FAILED)
        abort();      // no way to give what the callers pair on
}

void cone_flushProcessWriteBuffers(void) {
    pthread_once(&barrierOnce, barrierInit);
    if (barrierKernel) {
        if (syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0)
            return;
        abort();      // registered, and now refused: nothing else is correct
    }
    pthread_mutex_lock(&barrierLock);
    mprotect(barrierPage, barrierPageSize, PROT_READ | PROT_WRITE);
    // Dirty, so that the kernel cannot skip the flush of a page nobody wrote
    __atomic_add_fetch((size_t *)barrierPage, 1, __ATOMIC_SEQ_CST);
    mprotect(barrierPage, barrierPageSize, PROT_NONE);
    pthread_mutex_unlock(&barrierLock);
}

#endif
