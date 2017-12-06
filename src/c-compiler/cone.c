/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "cone.h"
#include "coneopts.h"
#include "shared/fileio.h"
#include "shared/symbol.h"
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

void main(int argc, char **argv) {
	ConeOptions coneopt;
	PgmAstNode *pgmast;
	int ok;
	char *srcfn;
	char *src;

	// Get compiler's instruction options from passed arguments
	ok = coneOptSet(&coneopt, &argc, argv);
	if (ok <= 0)
		exit(ok == 0 ? 0 : ExitOpts);
	if (argc < 2)
		errorExit(ExitOpts, "Specify a Cone program to compile.");
	srcfn = argv[1];

	// Initialize compiler's global structures
	startTime = clock();
	lexInject("*compiler*", "");
	symInit();
	keywordInit();
	typeInit();

	// Load specified source file
	src = fileLoad(srcfn);
	if (!src)
		errorExit(ExitNF, "Cannot find or read source file.");

	// Parse and generate
	lexInject(srcfn, src);
	pgmast = parse();
	if (errors == 0) {
		astPasses(pgmast);
		if (errors == 0) {
			if (coneopt.print_ast)
				astPrint(coneopt.output, srcfn, (AstNode*)pgmast);
			genllvm(&coneopt, pgmast);
		}
	}

	// Close up everything necessary
	errorSummary();
#ifdef _DEBUG
	getchar();	// Hack for VS debugging
#endif
}