/** console - the operating system's side of the console, one set a system
 * @file
 *
 * The console's engine is Cone (stdio.cone): it decides what waits, what goes
 * out now and in what order. What it asks of the operating system is here,
 * because Cone cannot say it: the lock that keeps one record whole against
 * another thread's (the platform's lightweight one: an SRWLOCK on Windows, a
 * mutex that is plain data when zero on POSIX), whether a descriptor is a
 * terminal, the write itself (the C runtime's low-level write, which does not
 * go through C's 'stdout' buffer), a write of several pieces at once where the
 * system has one (writev on POSIX; Windows has no gathered write for a
 * console or a pipe, so the pieces go out one after another under the lock,
 * still whole with respect to every other record), and a function to run at
 * exit, when what waits is written.
 *
 * No function reports a failure: a write that fails is dropped, as the C
 * library's would have been for output nobody reads. ONE RULE, for standard
 * output alone: a write to descriptor 1 that fails because the reader has gone
 * (EPIPE: on POSIX, a pipe or socket with no reader, once SIGPIPE does not end
 * the program as it does by default; on Windows the system's ERROR_NO_DATA or
 * ERROR_BROKEN_PIPE, which the C runtime reports as EINVAL) ENDS THE PROGRAM, quietly: no
 * message, nothing more written, exit status 141 (128 + SIGPIPE, what the
 * signal would have given a shell), on both systems. This is what 'prog | head'
 * has always meant, kept for a program in which something has made SIGPIPE
 * ignored (iocore's adoptPipe does, process-wide). A failed write to standard
 * error stays dropped.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

// Standard output's reader has gone: end the program now and quietly (the header: ONE RULE). Not 'exit': what waits to be
// written would only be written to the same closed pipe, and nothing should run that could write to it
static void consoleReaderGone(int fd) {
    if (fd == 1)
        _Exit(141);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
static SRWLOCK consoleLock = SRWLOCK_INIT;
void cone_consoleLock(void) { AcquireSRWLockExclusive(&consoleLock); }
void cone_consoleUnlock(void) { ReleaseSRWLockExclusive(&consoleLock); }
int cone_consoleTryLock(void) { return TryAcquireSRWLockExclusive(&consoleLock) != 0; }
int cone_consoleIsTty(int fd) { return _isatty(fd) != 0; }

// All n bytes, in as many writes as the system takes them in
void cone_consoleWrite(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        unsigned chunk = n > 0x40000000u ? 0x40000000u : (unsigned)n;
        int wrote = _write(fd, p, chunk);
        if (wrote <= 0) {
            // The C runtime says EINVAL for a pipe with no reader (the system's ERROR_NO_DATA, or ERROR_BROKEN_PIPE),
            // so the system's own error is read too
            DWORD err = GetLastError();
            if (errno == EPIPE || err == ERROR_NO_DATA || err == ERROR_BROKEN_PIPE)
                consoleReaderGone(fd);
            return;
        }
        p += wrote;
        n -= (size_t)wrote;
    }
}

// 'count' pieces, each a pointer and a length, one after another
void cone_consoleWriteParts(int fd, const size_t *parts, size_t count) {
    for (size_t i = 0; i < count; ++i)
        cone_consoleWrite(fd, (const uint8_t *)parts[2 * i], parts[2 * i + 1]);
}
#else
#include <errno.h>
#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>
#include <limits.h>
static pthread_mutex_t consoleLock = PTHREAD_MUTEX_INITIALIZER;
void cone_consoleLock(void) { pthread_mutex_lock(&consoleLock); }
void cone_consoleUnlock(void) { pthread_mutex_unlock(&consoleLock); }
int cone_consoleTryLock(void) { return pthread_mutex_trylock(&consoleLock) == 0; }
int cone_consoleIsTty(int fd) { return isatty(fd) != 0; }

void cone_consoleWrite(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        ssize_t wrote = write(fd, p, n);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE)
                consoleReaderGone(fd);
            return;
        }
        p += wrote;
        n -= (size_t)wrote;
    }
}

// 'count' pieces, each a pointer and a length, in as few calls as the system
// takes them in (up to IOV_MAX pieces to a call; a short write carries on from
// where it stopped)
void cone_consoleWriteParts(int fd, const size_t *parts, size_t count) {
    struct iovec iov[64];
    while (count > 0) {
        size_t batch = count > 64 ? 64 : count;
        size_t total = 0;
        for (size_t i = 0; i < batch; ++i) {
            iov[i].iov_base = (void *)parts[2 * i];
            iov[i].iov_len = parts[2 * i + 1];
            total += iov[i].iov_len;
        }
        size_t start = 0;
        while (total > 0) {
            ssize_t wrote = writev(fd, iov + start, (int)(batch - start));
            if (wrote < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EPIPE)
                    consoleReaderGone(fd);
                return;
            }
            total -= (size_t)wrote;
            size_t left = (size_t)wrote;
            while (start < batch && left >= iov[start].iov_len) {
                left -= iov[start].iov_len;
                ++start;
            }
            if (start < batch && left > 0) {
                iov[start].iov_base = (char *)iov[start].iov_base + left;
                iov[start].iov_len -= left;
            }
        }
        parts += 2 * batch;
        count -= batch;
    }
}
#endif

// Run 'fn' when the program ends, by 'exit' or by main returning
void cone_consoleAtExit(void (*fn)(void)) { atexit(fn); }
