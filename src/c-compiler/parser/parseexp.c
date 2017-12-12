/** Parse expressions
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

// Parse a term: literal, identifier, etc.
AstNode *parseterm() {
	switch (lex->toktype) {
	case IntLitToken:
		{
			ULitAstNode *node;
			node = newULitNode(lex->val.uintlit, lex->langtype);
			lexNextToken();
			return (AstNode *)node;
		}
	case FloatLitToken:
		{
			FLitAstNode *node;
			node = newFLitNode(lex->val.floatlit, lex->langtype);
			lexNextToken();
			return (AstNode *)node;
		}
	case IdentToken:
		{
			NameUseAstNode *node;
			node = newNameUseNode(lex->val.ident);
			lexNextToken();
			return (AstNode*)node;
		}
	default:
		errorMsgLex(ErrorBadTerm, "Invalid term value: expected variable, literal, etc.");
		return NULL;
	}
}

// Parse a prefix operator, e.g.: -
AstNode *parsePrefix() {
	if (lexIsToken(DashToken)) {
		UnaryAstNode *node;
		AstNode *opnode;
		newAstNode(node, UnaryAstNode, UnaryNode);
		lexNextToken();
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

// Parse an assignment expression
AstNode *parseAssign() {
	AstNode *lval = parsePrefix();
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		AstNode *rval = parsePrefix();
		return (AstNode*) newAssignAstNode(NormalAssign, lval, rval);
	}
	else
		return lval;
}

// Parse an expression
AstNode *parseExp() {
	return parseAssign();
}

