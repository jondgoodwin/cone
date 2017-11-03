/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "cone.h"
#include "shared/ast.h"
#include "shared/error.h"
#include "parser/lexer.h"

#include <stdio.h>
#include <stdlib.h>

// Very stupid parser
void parse() {
	AstNode *node;
	while (1) {
		if (lexType()==EofNode)
			break;
		if (node = lexMatch(LitNode)) {
			printf("OMG Found a number %d\n", node->v.uintlit);
		}
		else {
			lexGetNextToken();
			puts("Danger Danger Will Robinson");
		}
	}
}

void main(int argv, char **argc) {
	Lexer *lex;

	// Output compiler name and release level
	puts(CONE_RELEASE);

	if (argv<2) {
		puts("Specify a Cone program to compile");
		exit(1);
	}

	lex = lexNew(argc[1]);
	parse();

	getchar();
}