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
#include "../shared/symbol.h"
#include "lexer.h"

#include <stdio.h>

// Parse an expression statement within a function
AstNode *parseExpStmt() {
	AstNode *node = (AstNode *) parseExpr();
	parseSemi();
	return node;
}

// Parse a return statement
AstNode *parseReturn() {
	ReturnAstNode *stmtnode = newReturnNode();
	lexNextToken(); // Skip past 'return'
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseExpr();
	parseSemi();
	return (AstNode*) stmtnode;
}

// Parse if statement/expression
AstNode *parseIf() {
	IfAstNode *ifnode = newIfNode();
	lexNextToken();
	nodesAdd(&ifnode->condblk, parseExpr());
	nodesAdd(&ifnode->condblk, parseBlock());
	while (lexIsToken(ElifToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, parseExpr());
		nodesAdd(&ifnode->condblk, parseBlock());
	}
	if (lexIsToken(ElseToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, voidType);
		nodesAdd(&ifnode->condblk, parseBlock());
	}
	return (AstNode *)ifnode;
}

// Parse a block of statements/expressions
AstNode *parseBlock() {
	BlockAstNode *blk = newBlockNode();

	if (!lexIsToken(LCurlyToken))
		parseLCurly();
	if (!lexIsToken(LCurlyToken))
		return (AstNode*)blk;
	lexNextToken();

	blk->stmts = newNodes(8);
	while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {
		case SemiToken:
			lexNextToken();
			break;

		case RetToken:
			nodesAdd(&blk->stmts, parseReturn());
			break;

		case IfToken:
			nodesAdd(&blk->stmts, parseIf());
			break;

		case LCurlyToken:
			nodesAdd(&blk->stmts, parseBlock());
			break;

		// A local variable declaration, if it begins with a permission
		case IdentToken: {
			AstNode *perm = lex->val.ident->node;
			if (perm && perm->asttype == PermNameDclNode) {
				NameDclAstNode *local;
				nodesAdd(&blk->stmts, (AstNode*)(local = parseVarDcl(immPerm)));
				parseSemi();
				break;
			}
		}
		// Notice, this falls through to below if not a permission

		default:
			nodesAdd(&blk->stmts, parseExpStmt());
		}
	}

	parseRCurly();
	return (AstNode*)blk;
}
