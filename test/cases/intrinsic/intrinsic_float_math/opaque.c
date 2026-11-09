/* The C side of intrinsic_float_math: a number the Cone side cannot see
 * through. Compiled apart from the program and linked beside it, so the
 * optimizer, which never sees these bodies, cannot fold a call on a value that
 * passes through one: the math runs as the program runs, in the C library's
 * functions, not in the compiler's constant folder. */

double opaque64(double x) { return x; }
float opaque32(float x) { return x; }
int opaquei(int x) { return x; }
