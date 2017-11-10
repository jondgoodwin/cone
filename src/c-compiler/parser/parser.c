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

// Parse a term: literal, identifier, etc.
AstNode *parseterm() {
	switch (lexGetType()) {
	case IntNode:
	case FloatNode:
		return lexGetAndNext();
	default:
		// error message
		return NULL;
	}
}

// Parse a prefix operator, e.g.: -
AstNode *parseprefix() {
	AstNode *node;
	if (lexMatch(MinusNode)) {
		AstNode *opnode;
		node = lexGetAndNext();
		opnode = parseprefix();
		// Optimize negative numeric literal
		if (opnode->asttype == IntNode) {
			opnode->v.uintlit = -((int32_t)opnode->v.uintlit);
			return opnode;
		} else if (opnode->asttype == FloatNode) {
			opnode->v.floatlit = -opnode->v.floatlit;
			return opnode;
		} else {
			node->asttype = NegNode;
			node->v.node.n1 = parseprefix();
			return node;
		}
	}
	return parseterm();
}

// Parse a program
void parse() {
	AstNode *node;
	while (lexGetType() != EofNode) {
		node = parseprefix();
		if (node->asttype == IntNode) {
			printf("OMG Found an integer %d\n", node->v.uintlit);
		}
		else if (node->asttype == FloatNode) {
			printf("OMG Found a float %f\n", node->v.floatlit);
		}
	}
}
