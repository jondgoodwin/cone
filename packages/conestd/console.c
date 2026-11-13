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
 * Standard input's raw read is here too, 'cone_stdinRead': the bytes of the next read, as UTF-8 on a console and
 * as they are anywhere else (stdin's own section below says how), and the lock that keeps one thread's use of
 * the buffer whole (conestd's stdio.cone holds the buffer). It never takes the console's lock above: a read
 * that blocks must not stop printing.
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
#include <string.h>
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

// ---- Standard input (the header above) ----------------------------------------------------------------
static SRWLOCK stdinLock = SRWLOCK_INIT;
void cone_stdinLock(void) { AcquireSRWLockExclusive(&stdinLock); }
void cone_stdinUnlock(void) { ReleaseSRWLockExclusive(&stdinLock); }

// A console's reads are made in UTF-16 (ReadConsoleW, which gives the text exactly whatever the console's code page:
// ReadFile on a console in a legacy code page turns a letter its page lacks into another) and given on as UTF-8. What
// one read gave and the caller's room did not take stays here, and so does a high surrogate that ended a read, until
// its low half arrives
static uint8_t conLeft[16384];
static size_t conLeftPos = 0, conLeftLen = 0;
static WCHAR conHigh = 0;
static int conHasHigh = 0;

static int64_t stdinConsole(HANDLE h, uint8_t *buf, size_t n) {
    while (conLeftPos == conLeftLen) {
        WCHAR w[4096];
        size_t have = 0;
        DWORD got = 0;
        if (conHasHigh) {
            w[0] = conHigh;
            have = 1;
            conHasHigh = 0;
        }
        if (!ReadConsoleW(h, w + have, (DWORD)(4096 - have), &got, NULL))
            return -(int64_t)GetLastError();
        have += got;
        if (got > 0 && have > 0 && w[have - 1] >= 0xD800 && w[have - 1] <= 0xDBFF) {
            // A pair cut between reads: hold the first half back for the next read
            conHigh = w[have - 1];
            conHasHigh = 1;
            have--;
        }
        if (have > 0) {
            // 4096 units are at most 12288 bytes of UTF-8, which conLeft holds
            int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)have, (LPSTR)conLeft, (int)sizeof conLeft, NULL, NULL);
            if (len <= 0)
                return -(int64_t)GetLastError();
            conLeftPos = 0;
            conLeftLen = (size_t)len;
        }
        else if (got == 0)
            return 0;
    }
    size_t take = conLeftLen - conLeftPos;
    if (take > n)
        take = n;
    memcpy(buf, conLeft + conLeftPos, take);
    conLeftPos += take;
    return (int64_t)take;
}

// Up to n bytes of standard input, waiting for at least one: how many (0 at the end of the input), or the system's
// error number made negative. On a console, ReadConsoleW and UTF-8; otherwise (a pipe, a redirected file) ReadFile on
// the handle, which the C runtime's text mode (a '\r' dropped, a 0x1A that ends the input) never sees. Nothing sets a
// mode. A broken pipe is the end of the input.
int64_t cone_stdinRead(uint8_t *buf, size_t n) {
    if (n == 0)
        return 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return -(int64_t)ERROR_INVALID_HANDLE;
    DWORD mode;
    if (GetConsoleMode(h, &mode))
        return stdinConsole(h, buf, n);
    DWORD got = 0;
    DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
    if (!ReadFile(h, buf, want, &got, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
            return 0;
        return -(int64_t)err;
    }
    return (int64_t)got;
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

// ---- Standard input (the header above) ----------------------------------------------------------------
static pthread_mutex_t stdinLock = PTHREAD_MUTEX_INITIALIZER;
void cone_stdinLock(void) { pthread_mutex_lock(&stdinLock); }
void cone_stdinUnlock(void) { pthread_mutex_unlock(&stdinLock); }

// Up to n bytes of standard input, waiting for at least one: how many (0 at the end of the input), or the errno made
// negative. read(0), again after a signal
int64_t cone_stdinRead(uint8_t *buf, size_t n) {
    if (n == 0)
        return 0;
    if (n > 0x40000000u)
        n = 0x40000000u;
    for (;;) {
        ssize_t got = read(0, buf, n);
        if (got >= 0)
            return (int64_t)got;
        if (errno != EINTR)
            return -(int64_t)errno;
    }
}
#endif

// Run 'fn' when the program ends, by 'exit' or by main returning
void cone_consoleAtExit(void (*fn)(void)) { atexit(fn); }
