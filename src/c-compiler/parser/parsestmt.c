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
	StmtExpAstNode *stmtnode;
	stmtnode = newStmtExpNode();
	stmtnode->exp = parseExp();
	parseSemi();
	return (AstNode*) stmtnode;
}

// Parse a return statement
AstNode *parseReturn() {
	StmtExpAstNode *stmtnode;
	lexNextToken(); // Skip past 'return'
	stmtnode = newReturnNode();
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseExp();
	parseSemi();
	return (AstNode*) stmtnode;
}

// Parse a statement block inside a control structure
void parseStmtBlock(Nodes **nodes) {
	if (!lexIsToken(LCurlyToken))
		parseLCurly();
	if (!lexIsToken(LCurlyToken))
		return;
	lexNextToken();

	*nodes = newNodes(8);
	while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {
		case RetToken:
			nodesAdd(nodes, parseReturn());
			break;
		default:
			nodesAdd(nodes, parseExpStmt());
		}
	}

	parseRCurly();
}
