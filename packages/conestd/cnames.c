/** cnames - the C library's names that Cone cannot spell
 * @file
 *
 * Some of C's names are macros, inline functions or variables that differ by C
 * library: 'stdout' is glibc's variable, Darwin's '__stdoutp' and the Windows
 * runtime's call of '__acrt_iob_func'; 'errno' is '__errno_location()', '__error()'
 * or '_errno()'; 'time' and 'timespec_get' are inline functions over '_time64'
 * and '_timespec64_get' in the Windows runtime; and a variadic function such as
 * 'fprintf' takes its values in a way Cone declares no function for. A function
 * here answers each by the C library's own header, the same on every system
 * that has one, so the layers over it (libc's bindings, conestd's printing)
 * name these functions and nothing of any C library's internals. libc's
 * 'stdin()', 'stdout()', 'stderr()', 'errno()', 'time' and 'timespec_get' bind
 * them, and conestd's printing of a float calls the last.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

FILE *cone_stdin(void) { return stdin; }
FILE *cone_stdout(void) { return stdout; }
FILE *cone_stderr(void) { return stderr; }

int *cone_errno(void) { return &errno; }

// C's 'time': the seconds since 1970 (UTC), also stored at 't' unless it is null
int64_t cone_time(int64_t *t) {
    int64_t now = (int64_t)time(NULL);
    if (t != NULL)
        *t = now;
    return now;
}

// The POSIX error numbers that differ by C library (the UCRT's ENAMETOOLONG is
// 38 and glibc's 36, its ENOTEMPTY 41 and glibc's 39), which the posix package
// declares as globals of their own names. Each is a macro, so the header's value
// is taken first and the macro removed before its name is used for the global
enum {
    cone_value_ENAMETOOLONG = ENAMETOOLONG,
    cone_value_ENOTEMPTY = ENOTEMPTY
};
#undef ENAMETOOLONG
#undef ENOTEMPTY
const int ENAMETOOLONG = cone_value_ENAMETOOLONG;
const int ENOTEMPTY = cone_value_ENOTEMPTY;

// libc's 'Timespec': 'tv_sec' at 0 and a 32-bit 'tv_nsec' at 8, 16 bytes
struct ConeTimespec {
    int64_t tv_sec;
    int32_t tv_nsec;
};

// C11's 'timespec_get', its result in libc's layout
int cone_timespec_get(struct ConeTimespec *ts, int base) {
    struct timespec now;
    int result = timespec_get(&now, base);
    if (result != 0) {
        ts->tv_sec = (int64_t)now.tv_sec;
        ts->tv_nsec = (int32_t)now.tv_nsec;
    }
    return result;
}

// A float as C's '%g' writes it
int cone_fprintFloat(FILE *stream, double x) { return fprintf(stream, "%g", x); }
