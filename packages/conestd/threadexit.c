/** threadexit - a thread's exit callbacks, one slot for the process
 * @file
 *
 * A subsystem that keeps something for each thread (the buffer pool's cache,
 * later cnmalloc's stock) gives it back when the thread ends, whoever started
 * the thread: Cone's own threads, the actors runtime's workers, a thread
 * pool's, one made by linked C code. The operating system calls a function
 * when a thread ends only through a slot of its own: a fiber-local slot with a
 * callback on Windows (FlsAlloc), a key with a destructor on Linux
 * (pthread_key_create). This file makes ONE such slot for the process, so the
 * subsystems share it rather than each taking one:
 *
 *   uint32_t cone_threadExitAdd(void (*fn)(void))
 *       Register fn, once, at the subsystem's start (not per thread). Answers
 *       its number, 1 to 16, or 0 when the table is full or the system refused
 *       the slot: then nothing is called, and the subsystem goes on without.
 *       The slot is made by the first call. Safe from any thread.
 *   void cone_threadExitArm(uint32_t id)
 *       The calling thread wants fn 'id' called when it ends. Called the first
 *       time the subsystem needs it on a thread (the slow path: a thread that
 *       never asks pays nothing, and arming twice is harmless). 0 does nothing.
 *
 * When an armed thread ends, its registered functions run on it, in the order
 * they were registered, each once, while its thread-locals are still there.
 *
 * What the systems do, measured (lane 687, 18 Nov 2026, Windows 11 26200; the
 * probe is in that lane's report): at a thread's end Windows runs the slots'
 * callbacks in slot order, without the loader lock, with the thread's
 * '@threadlocal' values readable. A value set inside a callback is taken
 * (FlsSetValue answers true) but earns no second call. At the process's end
 * (C's exit, or ExitProcess) the other threads are stopped where they stand,
 * with no callback, and then the callback of the thread that ends the process
 * DOES run, holding the loader lock, with RtlDllShutdownInProgress() true. A
 * stopped thread may have held any lock, so nothing may be given back then:
 * the callback asks RtlDllShutdownInProgress first and calls nothing while it
 * is true. On Linux (glibc) a key's destructor runs at pthread_exit or the
 * return from the thread's function, after C++'s thread_local destructors,
 * and up to PTHREAD_DESTRUCTOR_ITERATIONS rounds (4) while a destructor sets a
 * value again; exit() runs none. So a function armed again while the thread's
 * callbacks run may get a second call (Linux) or not (Windows): a subsystem
 * marks its thread finished in its own function, and a later use on that
 * thread goes to its shared stock, as the buffer pool does.
 *
 * What a registered function may do: it runs as the thread is being torn
 * down. It takes only its own locks, allocates nothing (an allocator's own
 * callback may run before or after it), and calls nothing that might load a
 * library. Not covered: a thread killed outright (TerminateThread); on
 * Windows, a program that runs fibers (the slot is a fiber's: its callback
 * runs when the fiber is deleted, and Cone's subsystems keep their state per
 * thread); a thread whose first use of a subsystem comes after the slot's
 * callbacks have run (a thread-local's destructor in C++ on Windows), which
 * arms a slot nobody calls again.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stdint.h>
#include <stddef.h>

typedef void (*cone_ThreadExitFn)(void);

#define maxExitFns 16

static cone_ThreadExitFn exitFns[maxExitFns];
static volatile uint32_t exitCount = 0;     // entries of exitFns published; written only by Add, with release
static int slotState = 0;                   // 0 not made, 1 made, -1 refused (under the Add lock)

// The table is read WITHOUT a lock by a thread's end: at exit() that callback
// runs holding the loader lock, and any lock of ours inside it would add a
// lock-order edge nothing needs. So Add writes an entry first and then
// publishes the count with a release store; the callback reads the count with
// an acquire load, and takes only entries below it. The lock only serialises
// Add against Add.
static void publishCount(uint32_t n);
static uint32_t loadCount(void);

// Run the armed functions of the thread ending: each bit of 'mask' is one
static void runArmed(uintptr_t mask) {
    uint32_t n = loadCount();
    for (uint32_t i = 0; i < n && i < maxExitFns; i++) {
        if (mask & ((uintptr_t)1 << i))
            exitFns[i]();
    }
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef BOOLEAN (NTAPI *ShutdownFn)(void);
static ShutdownFn shutdownInProgress = NULL;
static DWORD exitSlot = FLS_OUT_OF_INDEXES;
static SRWLOCK addLock = SRWLOCK_INIT;

static void WINAPI exitCallback(void *value) {
    // The process is ending: the other threads were stopped holding whatever
    // they held, and this one holds the loader lock. Nothing is given back
    if (shutdownInProgress != NULL && shutdownInProgress())
        return;
    runArmed((uintptr_t)value);
}

static int makeSlot(void) {
    // Looked up now, never in the callback (which must load nothing)
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt != NULL)
        shutdownInProgress = (ShutdownFn)(void (*)(void))GetProcAddress(nt, "RtlDllShutdownInProgress");
    exitSlot = FlsAlloc(exitCallback);
    return exitSlot == FLS_OUT_OF_INDEXES ? -1 : 1;
}

// x64 and arm64 Windows: an Interlocked exchange is a full barrier (so it
// releases), and a compare-exchange with Acquire semantics reads with acquire
static void publishCount(uint32_t n) { InterlockedExchange((volatile LONG *)&exitCount, (LONG)n); }
static uint32_t loadCount(void) { return (uint32_t)InterlockedCompareExchangeAcquire((volatile LONG *)&exitCount, 0, 0); }

static void lockAdd(void) { AcquireSRWLockExclusive(&addLock); }
static void unlockAdd(void) { ReleaseSRWLockExclusive(&addLock); }

void cone_threadExitArm(uint32_t id) {
    if (id == 0 || id > maxExitFns)
        return;
    uintptr_t v = (uintptr_t)FlsGetValue(exitSlot);
    uintptr_t bit = (uintptr_t)1 << (id - 1);
    if ((v & bit) == 0)
        FlsSetValue(exitSlot, (void *)(v | bit));
}

#else
#include <pthread.h>

static pthread_key_t exitKey;
static pthread_mutex_t addMutex = PTHREAD_MUTEX_INITIALIZER;

// glibc has set the key's value to null before it calls this
static void exitCallback(void *value) {
    runArmed((uintptr_t)value);
}

static int makeSlot(void) {
    return pthread_key_create(&exitKey, exitCallback) == 0 ? 1 : -1;
}

static void publishCount(uint32_t n) { __atomic_store_n(&exitCount, n, __ATOMIC_RELEASE); }
static uint32_t loadCount(void) { return __atomic_load_n(&exitCount, __ATOMIC_ACQUIRE); }

static void lockAdd(void) { pthread_mutex_lock(&addMutex); }
static void unlockAdd(void) { pthread_mutex_unlock(&addMutex); }

void cone_threadExitArm(uint32_t id) {
    if (id == 0 || id > maxExitFns)
        return;
    uintptr_t v = (uintptr_t)pthread_getspecific(exitKey);
    uintptr_t bit = (uintptr_t)1 << (id - 1);
    if ((v & bit) == 0)
        pthread_setspecific(exitKey, (void *)(v | bit));
}
#endif

uint32_t cone_threadExitAdd(cone_ThreadExitFn fn) {
    uint32_t id = 0;
    lockAdd();
    if (slotState == 0)
        slotState = makeSlot();
    uint32_t n = exitCount;          // only Add writes it, and Add holds the lock
    if (slotState == 1 && n < maxExitFns && fn != NULL) {
        exitFns[n] = fn;             // the entry first,
        publishCount(n + 1);         // then the count, with release: a thread arms only an id it was given
        id = n + 1;
    }
    unlockAdd();
    return id;
}
