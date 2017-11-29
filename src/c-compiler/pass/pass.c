/** Option handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "pass.h"

#include <string.h>
#include <stdio.h>

void passOptInit(pass_opt_t* options) {
	memset(options, 0, sizeof(pass_opt_t));
	// options->limit = PASS_ALL;
	// options->verbosity = VERBOSITY_INFO;
	// options->check.errors = errors_alloc();
}
