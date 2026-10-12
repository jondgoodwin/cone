/** cnames - the C library's names that Cone cannot spell
 * @file
 *
 * Some of C's names are macros, inline functions or variables that differ by C
 * library: 'stdout' is glibc's variable, Darwin's '__stdoutp' and the Windows
 * runtime's call of '__acrt_iob_func'; 'errno' is '__errno_location()', '__error()'
 * or '_errno()'; 'time' and 'timespec_get' are inline functions over '_time64'
 * and '_timespec64_get' in the Windows runtime; and EILSEQ and the locale
 * categories are numbers that differ between C libraries. A function or a
 * global here answers each by the C library's own header, the same on every
 * system that has one, so the layers over it (libc's bindings) name these and
 * nothing of any C library's internals. libc's 'stdin()', 'stdout()',
 * 'stderr()', 'errno()', 'time', 'timespec_get', 'EILSEQ' and 'LC_ALL' and the
 * other locale categories bind them.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

FILE *cone_stdin(void) { return stdin; }
FILE *cone_stdout(void) { return stdout; }
FILE *cone_stderr(void) { return stderr; }

int *cone_errno(void) { return &errno; }

// The numbers that differ by C library (the UCRT's EILSEQ is 42 and glibc's 84,
// its LC_ALL 0 and glibc's 6; the UCRT's ENAMETOOLONG is 38 and glibc's 36, its
// ENOTEMPTY 41 and glibc's 39), which libc (and, for the last two, posix)
// declares as globals of their own names.
// Each is a macro, so the header's value is taken first and the macro removed
// before its name is used for the global
enum {
    cone_value_ENAMETOOLONG = ENAMETOOLONG,
    cone_value_ENOTEMPTY = ENOTEMPTY,
    cone_value_EILSEQ = EILSEQ,
    cone_value_LC_ALL = LC_ALL,
    cone_value_LC_COLLATE = LC_COLLATE,
    cone_value_LC_CTYPE = LC_CTYPE,
    cone_value_LC_MONETARY = LC_MONETARY,
    cone_value_LC_NUMERIC = LC_NUMERIC,
    cone_value_LC_TIME = LC_TIME
};
#undef ENAMETOOLONG
#undef ENOTEMPTY
#undef EILSEQ
#undef LC_ALL
#undef LC_COLLATE
#undef LC_CTYPE
#undef LC_MONETARY
#undef LC_NUMERIC
#undef LC_TIME
const int ENAMETOOLONG = cone_value_ENAMETOOLONG;
const int ENOTEMPTY = cone_value_ENOTEMPTY;
const int EILSEQ = cone_value_EILSEQ;
const int LC_ALL = cone_value_LC_ALL;
const int LC_COLLATE = cone_value_LC_COLLATE;
const int LC_CTYPE = cone_value_LC_CTYPE;
const int LC_MONETARY = cone_value_LC_MONETARY;
const int LC_NUMERIC = cone_value_LC_NUMERIC;
const int LC_TIME = cone_value_LC_TIME;

// C's 'time': the seconds since 1970 (UTC), also stored at 't' unless it is null
int64_t cone_time(int64_t *t) {
    int64_t now = (int64_t)time(NULL);
    if (t != NULL)
        *t = now;
    return now;
}

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
