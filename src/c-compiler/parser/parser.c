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
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>

// We expect semicolon since statement has run its course
void parseSemi() {
	if (lex->toktype != SemiToken)
		errorMsgLex(ErrorNoSemi, "Expected semicolon - skipping forward to find it");
	while (lex->toktype != SemiToken) {
		if (lex->toktype == EofToken || lex->toktype == RCurlyToken)
			return;
		lexNextToken();
	}
	lexNextToken();
}

// Expect right curly brace and move past
void parseRCurly() {
	if (lex->toktype == RCurlyToken)
		lexNextToken();
	else
		errorMsgLex(ErrorNoRCurly, "Expected closing brace '}'");
}

// Parse a function block
AstNode *parseFn() {
	FnBlkAstNode *fnnode;
	astNewNode(fnnode, FnBlkAstNode, FnBlkNode);
	parseStmtBlock(&fnnode->nodes);
	return (AstNode*) fnnode;
}

// Parse a program's global area
AstNode *parse() {
	GlobalAstNode *global;
	Nodes **nodes;

	// Create and populate a Program node for the program
	astNewNode(global, GlobalAstNode, GlobalNode);
	nodes = (Nodes**) &global->nodes;
	*nodes = nodesNew(8);
	while (lex->toktype != EofToken) {
		switch (lex->toktype) {
		case FnToken:
			lexNextToken();
			nodesAdd(nodes, parseFn());
			break;
		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area type, var or function statement");						
		}
	}
	return (AstNode*) global;
}
