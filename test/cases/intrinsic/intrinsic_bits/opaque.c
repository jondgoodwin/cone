/* The C side of intrinsic_bits: a number the Cone side cannot see through.
 * Compiled apart from the program and linked beside it, so the optimizer
 * cannot fold a value or an amount that passes through it into a constant:
 * each count, rotate and masked shift is done as the program runs. */

#include <stdint.h>

uint64_t opaque(uint64_t n) { return n; }
