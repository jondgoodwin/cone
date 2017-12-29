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
	StmtExpAstNode *stmtnode;
	stmtnode = newStmtExpNode();
	stmtnode->exp = parseExp();
	parseSemi();
	return (AstNode*) stmtnode;
}

// Parse a return statement
AstNode *parseReturn() {
	StmtExpAstNode *stmtnode;
	lexNextToken(); // Skip past 'return'
	stmtnode = newReturnNode();
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseExp();
	parseSemi();
	return (AstNode*) stmtnode;
}

// Parse if statement/expression
AstNode *parseIf() {
	IfAstNode *ifnode = newIfNode();
	lexNextToken();
	nodesAdd(&ifnode->condblk, parseExp());
	nodesAdd(&ifnode->condblk, parseStmtBlock());
	while (lexIsToken(ElifToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, parseExp());
		nodesAdd(&ifnode->condblk, parseStmtBlock());
	}
	if (lexIsToken(ElseToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, voidType);
		nodesAdd(&ifnode->condblk, parseStmtBlock());
	}
	return (AstNode *)ifnode;
}

// Parse a statement block inside a control structure
AstNode *parseStmtBlock() {
	BlockAstNode *blk = newBlockNode();

	if (!lexIsToken(LCurlyToken))
		parseLCurly();
	if (!lexIsToken(LCurlyToken))
		return (AstNode*)blk;
	lexNextToken();

	blk->stmts = newNodes(8);
	while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {
		case RetToken:
			nodesAdd(&blk->stmts, parseReturn());
			break;

		case IfToken:
			nodesAdd(&blk->stmts, parseIf());
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
