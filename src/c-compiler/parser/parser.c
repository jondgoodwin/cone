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

void parseRCurly(AstNode *node) {
	if (lex->toktype == RCurlyToken)
		lexNextToken();
}

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
AstNode *parsePrefix() {
	if (lex->toktype==DashToken) {
		UnaryAstNode *node;
		AstNode *opnode;
		astNewNodeAndNext(node, UnaryAstNode, UnaryNode);
		opnode = parsePrefix();
		// Optimize negative numeric literal
		if (opnode->asttype == ULitNode) {
			((ULitAstNode*)opnode)->uintlit = -(int32_t)((ULitAstNode*)opnode)->uintlit;
			return (AstNode *)opnode;
		} else if (opnode->asttype == FLitNode) {
			((FLitAstNode*)opnode)->floatlit = -(int32_t)((FLitAstNode*)opnode)->floatlit;
			return (AstNode *)opnode;
		} else {
			node->expnode = parsePrefix();
			return (AstNode *)node;
		}
	}
	return parseterm();
}

// Parse a statement within a function
AstNode *parseStmt() {
	AstNode *stmtnode;
	stmtnode = parsePrefix();
	parseSemi();
	return stmtnode;
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
		nodesAdd(nodes, parseStmt());
	}

	if (lex->toktype == RCurlyToken)
		lexNextToken();

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
			nodesAdd(nodes, parsePrefix());
		}
	}
	return (AstNode*) global;
}
