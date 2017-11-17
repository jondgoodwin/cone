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
#include "lexer.h"

#include <stdio.h>

// Parse a term: literal, identifier, etc.
AstNode *parseterm() {
	switch (lex->toktype) {
	case IntLitToken:
		{
			ULitAstNode *node;
			astNewNode(node, ULitAstNode, ULitNode);
			node->uintlit = lex->val.uintlit;
			node->vtype = lex->langtype;
			lexNextToken();
			return (AstNode *)node;
		}
	case FloatLitToken:
		{
			FLitAstNode *node;
			astNewNode(node, FLitAstNode, FLitNode);
			node->floatlit = lex->val.floatlit;
			node->vtype = lex->langtype;
			lexNextToken();
			return (AstNode *)node;
		}
	default:
		// error message
		return NULL;
	}
}

// Parse a prefix operator, e.g.: -
AstNode *parseprefix() {
	if (lex->toktype==DashToken) {
		UnaryAstNode *node;
		AstNode *opnode;
		astNewNodeAndNext(node, UnaryAstNode, UnaryNode);
		opnode = parseprefix();
		// Optimize negative numeric literal
		if (opnode->asttype == ULitNode) {
			((ULitAstNode*)opnode)->uintlit = -(int32_t)((ULitAstNode*)opnode)->uintlit;
			return (AstNode *)opnode;
		} else if (opnode->asttype == FLitNode) {
			((FLitAstNode*)opnode)->floatlit = -(int32_t)((FLitAstNode*)opnode)->floatlit;
			return (AstNode *)opnode;
		} else {
			node->expnode = parseprefix();
			return (AstNode *)node;
		}
	}
	return parseterm();
}

// Parse a program
AstNode *parse() {
	BlockAstNode *program;
	Nodes **nodes;

	// Create a Block node for the program
	astNewNode(program, BlockAstNode, BlockNode);
	nodes = (Nodes**) &program->nodes;
	*nodes = nodesNew(8);
	while (lex->toktype != EofToken) {
		nodesAdd(nodes, parseprefix());
	}
	return (AstNode*) program;
}
