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
 * 'cone_processors' and 'cone_flushProcessWriteBuffers', and the process
 * package's 'cone_environ' (the variable 'environ', which Cone cannot name).
 * Everything else the
 * packages need on Linux and macOS (pthread_create and pthread_join,
 * sched_yield, nanosleep) is POSIX, and the packages bind it by name.
 *
 * 'cone_environ' and 'cone_processors' are POSIX, and one definition serves
 * both systems. The rest is each system's own. macOS has no futex: it has
 * 'os_sync_wait_on_address' and its wakes (macOS 14.4 and later), and its
 * thread identity is 'pthread_threadid_np'. It has no membarrier either, and
 * the barrier is made another way (its comment says why).
 *
 * Nothing here is compiled on Windows.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

// Before any include: glibc declares 'syscall' only under _GNU_SOURCE, and the
// first header included fixes the feature set for every header after it
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

// Not empty on any system (an empty source file is not ISO C)
typedef int cone_oscalls_t;

#if defined(__linux__) || defined(__APPLE__)

#include <unistd.h>

// The environment as the C library holds it now, an array of 'NAME=value'
// strings ending in a null pointer: 'environ' is a variable Cone cannot
// name, and setenv may have moved it since the program started. The process
// package passes it to the program it starts with posix_spawn.
char **cone_environ(void) {
    extern char **environ;
    return environ;
}

// How many logical processors are online, at least 1
int cone_processors(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

#endif

#if defined(__linux__)

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

#elif defined(__APPLE__)

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/thread_state.h>
#include <os/os_sync_wait_on_address.h>

// As on Linux: sleep while *word holds 'expected', until woken or 'timeoutNs'
// passes (UINT64_MAX is no limit); 0 only if the timeout passed, 1 for every
// other return. The wait takes no zero timeout (EINVAL), so no time is a
// timeout unless the word already differs, as the futex answers it. Without
// the SHARED flag the word is this process's alone.
int cone_futexWait(uint32_t *word, uint32_t expected, uint64_t timeoutNs) {
    int result;
    if (timeoutNs == UINT64_MAX)
        result = os_sync_wait_on_address(word, expected, sizeof *word, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    else if (timeoutNs == 0)
        return __atomic_load_n(word, __ATOMIC_SEQ_CST) != expected;
    else
        result = os_sync_wait_on_address_with_timeout(word, expected, sizeof *word,
            OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, timeoutNs);
    return !(result == -1 && errno == ETIMEDOUT);
}

// Wake one thread asleep on *word, or (INT_MAX) all. A wake with no sleeper
// answers ENOENT, which is no error here.
void cone_futexWake(uint32_t *word, int count) {
    if (count == INT_MAX)
        os_sync_wake_by_address_all(word, sizeof *word, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    else
        os_sync_wake_by_address_any(word, sizeof *word, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// The kernel's 64-bit id for the calling thread, which the system never
// reuses, in the low 32 bits the packages hold; never 0
uint32_t cone_threadId(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (uint32_t)id != 0 ? (uint32_t)id : 1u;
}

// Every processor running a thread of this process drains its stores, as if
// each had made a full fence where it stands (the Linux half says what for).
// macOS has no membarrier, and the Linux fallback is no barrier on Apple
// silicon: the processors invalidate each other's translations in hardware,
// so changing a page's protection interrupts none of them (measured on an M2:
// no thread of the process stopped during any flush). What does stop every
// running thread is the way .NET makes this barrier on macOS: asking the
// kernel for each thread's registers. To read them the kernel interrupts the
// processor running the thread, which saves its state and hands it to the
// asking processor; that hand-over is ordered after every store the thread
// made before it, so each is visible once the call returns (measured: every
// running thread stopped inside every flush). A thread starting or ending
// while the list is read has no registers to give: the kernel answers an
// invalid port, an invalid argument, terminated, or (for the call itself) a
// server that died, and the thread state cannot be read either. One not yet
// running has made no stores; one ending made its last before it began to
// end, which the kernel's own locking published. Any other refusal leaves
// no correct answer.
void cone_flushProcessWriteBuffers(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mach_port_t task = mach_task_self();
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(task, &threads, &count) != KERN_SUCCESS)
        abort();
    thread_act_t self = pthread_mach_thread_np(pthread_self());
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (threads[i] != self) {
            uintptr_t sp;
            uintptr_t registers[128];
            size_t n = sizeof registers / sizeof registers[0];
            kern_return_t got = thread_get_register_pointer_values(threads[i], &sp, &n, registers);
            if (got != KERN_SUCCESS && got != KERN_INSUFFICIENT_BUFFER_SIZE
                && got != MACH_SEND_INVALID_DEST && got != KERN_INVALID_ARGUMENT
                && got != KERN_TERMINATED && got != MIG_SERVER_DIED)
                abort();
        }
        mach_port_deallocate(task, threads[i]);
    }
    vm_deallocate(task, (vm_address_t)threads, count * sizeof threads[0]);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#endif
