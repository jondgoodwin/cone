/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "conec.h"
#include "coneopts.h"
#include "shared/fileio.h"
#include "ast/nametbl.h"
#include "ast/ast.h"
#include "shared/error.h"
#include "parser/keyword.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "genllvm/genllvm.h"

#include <stdio.h>
#include <time.h>
#include <assert.h>

clock_t startTime;

int main(int argc, char **argv) {
	ConeOptions coneopt;
	ModuleAstNode *modnode;
	int ok;
	char *srcfn;
	char *src;

	// Start measuring processing time for compilation
	startTime = clock();

	// Get compiler's options from passed arguments
	ok = coneOptSet(&coneopt, &argc, argv);
	if (ok <= 0)
		exit(ok == 0 ? 0 : ExitOpts);
	if (argc < 2)
		errorExit(ExitOpts, "Specify a Cone program to compile.");
	srcfn = argv[1];

	// Initialize compiler's global structures
	lexInject("*compiler*", "");
	nameInit();
	keywordInit();
	typeInit();

	// Load specified source file
	src = fileLoad(srcfn);
	if (!src)
		errorExit(ExitNF, "Cannot find or read source file.");

	// Parse and generate
	lexInject(srcfn, src);
	modnode = parsePgm();
	if (errors == 0) {
		astPasses(modnode);
		if (errors == 0) {
			if (coneopt.print_ast)
				astPrint(coneopt.output, srcfn, (AstNode*)modnode);
			genllvm(&coneopt, modnode);
		}
	}

	// Close up everything necessary
	errorSummary();
#ifdef _DEBUG
	getchar();	// Hack for VS debugging
#endif
}
