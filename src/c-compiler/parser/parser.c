/** Parser
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../shared/ast.h"
#include "lexer.h"

#include <stdio.h>

// Very stupid parser
void parse() {
	AstNode *node;
	while (1) {
		if (lexGetType()==EofNode)
			break;
		if (node = lexMatch(IntNode)) {
			printf("OMG Found an integer %d\n", node->v.uintlit);
		}
		else if (node = lexMatch(FloatNode)) {
			printf("OMG Found a float %f\n", node->v.floatlit);
		}
		else {
			lexGetNextToken();
			puts("Danger Danger Will Robinson");
		}
	}
}
