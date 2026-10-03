/* The C side of core_shift_defined: a number the Cone side cannot see
 * through. Compiled apart from the program and linked beside it, so the
 * optimizer, which never sees this body, cannot fold a shift amount that
 * passes through it into a constant: the shift is done as the program runs. */

#include <stdint.h>

uint64_t opaque(uint64_t n) { return n; }
