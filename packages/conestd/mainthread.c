/** mainthread - which thread the program started on, recorded before 'main'
 * @file
 *
 * The one part of conestd Cone cannot say. A panic names the thread it is on
 * when that is not the one the program started on (panic.cone), so the
 * starting thread's identity must be taken on it, before 'main' runs and
 * before any other thread exists. C has a way to run a function then: an
 * entry in the C runtime's table of initializers, '.CRT$XCU' (or a
 * constructor, outside Microsoft's C). Cone has none: no attribute places a
 * global in a named section or marks a function a constructor, and a
 * module's 'init' runs only when the program calls 'initAll()'.
 *
 * panic.cone reads 'cone_mainThread', which is also what makes the linker
 * take this object, and with it the initializer, out of conestd.lib.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint64_t threadId(void) { return GetCurrentThreadId(); }
#else
#include <pthread.h>
static uint64_t threadId(void) { return (uint64_t)pthread_self(); }
#endif

// The thread the program started on, as panic.cone compares a thread's
// identity with it
uint64_t cone_mainThread = 0;

static void mainThreadInit(void) {
    cone_mainThread = threadId();
}
#ifdef _MSC_VER
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*cone_mainThreadEntry)(void) = mainThreadInit;
#else
__attribute__((constructor)) static void mainThreadCtor(void) { mainThreadInit(); }
#endif
