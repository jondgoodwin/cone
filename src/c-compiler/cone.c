/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "cone.h"
#include "shared/globals.h"
#include "shared/fileio.h"
#include "shared/symbol.h"
#include "ast/ast.h"
#include "shared/error.h"
#include "parser/keyword.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "genllvm/genllvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

void options(int argv, char **argc) {
	target.ptrsize = 32;
	#if _WIN64 || __x86_64__ || __ppc64__
		target.ptrsize = 64;
	#endif
}

void main(int argv, char **argc) {
	char *src;

	// Initialize compiler's global structures
	options(argv, argc);
	symInit();
	keywordInit();

	// Output compiler name and release level
	puts(CONE_RELEASE);

	// Load specified source file
	if (argv<2)
		errorExit(ExitOpts, "Specify a Cone program to compile.");
	src = fileLoad(argc[1]);
	if (!src)
		errorExit(ExitNF, "Cannot find or read source file.");

	// Parse and generate
	lexInject(argc[1], src);
	typInit();
	genllvm(parse());

	// Close up everything necessary
	errorSummary();
#ifdef _DEBUG
	getchar();	// Hack for VS debugging
#endif
}