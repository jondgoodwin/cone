/** Parse executable statements and blocks
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>

// Parse an expression statement within a function
AstNode *parseExpStmt() {
	AstNode *stmtnode;
	stmtnode = parseExp();
	parseSemi();
	return stmtnode;
}

// Parse a statement block inside a control structure
void parseStmtBlock(Nodes **nodes) {
	if (lexIsToken(LCurlyToken))
		lexNextToken();

	*nodes = newNodes(8);
	while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
		nodesAdd(nodes, parseExpStmt());
	}

	parseRCurly();
}
