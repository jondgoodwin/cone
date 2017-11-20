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
			node->perm = immPerm;
			lexNextToken();
			return (AstNode *)node;
		}
	case FloatLitToken:
		{
			FLitAstNode *node;
			astNewNode(node, FLitAstNode, FLitNode);
			node->floatlit = lex->val.floatlit;
			node->vtype = lex->langtype;
			node->perm = immPerm;
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

// Parse a function block
AstNode *parseFn() {
	FnBlkAstNode *fnnode;
	Nodes **nodes;

	if (lex->toktype == LCurlyToken)
		lexNextToken();

	// Create and populate a function block node
	astNewNode(fnnode, FnBlkAstNode, FnBlkNode);
	nodes = (Nodes**) &fnnode->nodes;
	*nodes = nodesNew(8);
	while (lex->toktype != EofToken && lex->toktype != RCurlyToken) {
		nodesAdd(nodes, parseprefix());
	}

	if (lex->toktype == RCurlyToken)
		lexNextToken();

	return (AstNode*) fnnode;
}

// Parse a program
AstNode *parse() {
	PgmAstNode *program;
	Nodes **nodes;

	// Create and populate a Program node for the program
	astNewNode(program, PgmAstNode, PgmNode);
	nodes = (Nodes**) &program->nodes;
	*nodes = nodesNew(8);
	while (lex->toktype != EofToken) {
		switch (lex->toktype) {
		case FnToken:
			lexNextToken();
			nodesAdd(nodes, parseFn());
			break;
		default:
			nodesAdd(nodes, parseprefix());
		}
	}
	return (AstNode*) program;
}
