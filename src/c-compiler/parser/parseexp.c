/** Parse expressions
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
		errorMsgLex(ErrorBadTerm, "Invalid term value: expected variable, literal, etc.");
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

// Parse an expression
AstNode *parseExp() {
	return parsePrefix();
}

