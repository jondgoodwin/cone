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
	if (lexGetType()==MinusNode) {
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
AstNode *parse() {
	AstNode *program;
	Nodes **nodes;

	// Create a Block node for the program
	program = lexNewAstNode(BlockNode);
	nodes = (Nodes**) &program->v.info;
	*nodes = nodesNew(8);
	while (lexGetType() != EofNode) {
		nodesAdd(nodes, parseprefix());
	}
	return program;
}
