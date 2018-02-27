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

// Parse control flow suffixes
AstNode *parseSuffix(AstNode *node) {
	while (lexIsToken(IfToken) || lexIsToken(WhileToken)) {
		if (lexIsToken(IfToken)) {
			BlockAstNode *blk;
			IfAstNode *ifnode = newIfNode();
			lexNextToken();
			nodesAdd(&ifnode->condblk, parseExpr());
			nodesAdd(&ifnode->condblk, (AstNode*)(blk = newBlockNode()));
			nodesAdd(&blk->stmts, node);
			node = (AstNode*)ifnode;
		}
		else {
			BlockAstNode *blk;
			WhileAstNode *wnode = newWhileNode();
			lexNextToken();
			wnode->condexp = parseExpr();
			wnode->blk = (AstNode*)(blk = newBlockNode());
			nodesAdd(&blk->stmts, node);
			node = (AstNode*)wnode;
		}
	}
	parseSemi();
	return node;
}

// Parse an expression statement within a function
AstNode *parseExpStmt() {
	return parseSuffix((AstNode *)parseExpr());
}

// Parse a return statement
AstNode *parseReturn() {
	ReturnAstNode *stmtnode = newReturnNode();
	lexNextToken(); // Skip past 'return'
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseExpr();
	return parseSuffix((AstNode*)stmtnode);
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

// Parse while block
AstNode *parseWhile() {
	WhileAstNode *wnode = newWhileNode();
	lexNextToken();
	wnode->condexp = parseExpr();
	wnode->blk = parseBlock();
	return (AstNode *)wnode;
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

		case WhileToken:
			nodesAdd(&blk->stmts, parseWhile());
			break;

		case BreakToken:
		{
			AstNode *node;
			newAstNode(node, AstNode, BreakNode);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(node));
			break;
		}

		case ContinueToken:
		{
			AstNode *node;
			newAstNode(node, AstNode, ContinueNode);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(node));
			break;
		}

		case LCurlyToken:
			nodesAdd(&blk->stmts, parseBlock());
			break;

		// A local variable declaration, if it begins with a permission
		case IdentToken: {
			NamedAstNode *perm = lex->val.ident->node;
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
