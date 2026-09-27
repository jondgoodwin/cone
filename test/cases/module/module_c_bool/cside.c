/* The C side of module_c_bool: C functions taking and returning 'bool', and
 * structs holding it, and C calling Cone's C-named functions the same way.
 * Compiled by the platform's C compiler, which trusts a 'bool' in a register
 * to hold 0 or 1 in its low byte: each function here answers with that whole
 * byte, so a value not widened as C's convention widens it shows. */

#include <stdbool.h>
#include <stdint.h>

/* An integer the Cone side cannot see through, to make a Bool of */
int32_t c_int(int32_t n) { return n; }

/* The byte a 'bool' argument arrived as */
int32_t bool_byte(bool b) { return *(volatile uint8_t *)&b; }
/* A 'bool' among other arguments */
int32_t bool_mixed(int32_t n, bool a, int8_t k, bool b) {
    return n * 1000 + *(volatile uint8_t *)&a * 100 + k * 10 + *(volatile uint8_t *)&b;
}
/* A 'bool' returned */
bool bool_odd(int32_t n) { return (n & 1) != 0; }

/* A struct holding bools: 8 bytes, so one integer on Win64 */
typedef struct { int32_t n; bool a; bool b; } Flags;
/* 12 bytes, so through a pointer to a copy */
typedef struct { int32_t n; bool a; int32_t m; bool b; } Flags3;

int32_t flags_code(Flags f) { return f.n * 100 + f.a * 10 + f.b; }
int32_t flags3_code(Flags3 f) { return f.n * 1000 + f.a * 100 + f.m * 10 + f.b; }
Flags flags_make(int32_t n) { Flags f = { n, n > 0, n > 1 }; return f; }
void flags_fill(Flags3 *f) { f->n = 4; f->a = true; f->m = 5; f->b = false; }

/* Cone's C-named functions, which C calls */
bool cone_not(bool b);
bool cone_bool_of(int32_t n);
int32_t cone_flags(Flags f);

int32_t c_via_cone_not(void) { return cone_not(false) * 10 + cone_not(true); }
/* The byte Cone's 'bool' came back as, for a value made from a wider one */
int32_t c_via_cone_bool_of(int32_t n) { bool b = cone_bool_of(n); return *(volatile uint8_t *)&b; }
int32_t c_via_cone_flags(void) { Flags f = { 7, true, false }; return cone_flags(f); }
