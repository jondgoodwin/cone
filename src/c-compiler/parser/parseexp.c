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
AstNode *parseTerm() {
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
	case LParenToken:
		{
			lexNextToken();
			parseExp();
			parseRParen();
		}
	default:
		errorMsgLex(ErrorBadTerm, "Invalid term value: expected variable, literal, etc.");
		return NULL;
	}
}

// Parse the postfix operators: '.', '::', '()'
AstNode *parsePostfix() {
	AstNode *node = parseTerm();
	while (1) {
		switch (lex->toktype) {

		// Function call with possible parameters
		case LParenToken:
		{
			FnCallAstNode *fncall = newFnCallAstNode(node);
			lexNextToken();
			fncall->parms = newNodes(8);
			if (!lexIsToken(RParenToken))
				while (1) {
					nodesAdd(&fncall->parms, parseExp());
					if (lexIsToken(CommaToken))
						lexNextToken();
					else
						break;
				}
			parseRParen();
			node = (AstNode *)fncall;
		}
		default:
			return node;
		}
	}
}

// Parse a prefix operator, e.g.: -
AstNode *parsePrefix() {
	/*
	if (lexIsToken(DashToken)) {
		// newAstNode(node, UnaryAstNode, UnaryNode);
		lexNextToken();
		node->expnode = parsePrefix();
		return (AstNode *)node;
	}
	*/
	return parsePostfix();
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

