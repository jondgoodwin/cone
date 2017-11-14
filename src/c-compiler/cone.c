/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "cone.h"
#include "shared/fileio.h"
#include "shared/symbol.h"
#include "shared/ast.h"
#include "shared/error.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "genllvm/genllvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

void main(int argv, char **argc) {
	char *src;

	// Output compiler name and release level
	puts(CONE_RELEASE);

	if (argv<2)
		errorExit(ExitOpts, "Specify a Cone program to compile.");

	src = fileLoad(argc[1]);
	if (!src)
		errorExit(ExitNF, "Cannot find or read source file.");
		
	symInit();
	lexInject(argc[1], src);
	genllvm(parse());

	errorSummary();
#ifdef _DEBUG
	getchar();	// Hack for VS debugging
#endif
}