/** Parse executable statements and blocks
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../ir/nametbl.h"
#include "lexer.h"

#include <stdio.h>

// Parse control flow suffixes
AstNode *parseSuffix(ParseState *parse, AstNode *node) {
	while (lexIsToken(IfToken) || lexIsToken(WhileToken)) {
		if (lexIsToken(IfToken)) {
			BlockAstNode *blk;
			IfAstNode *ifnode = newIfNode();
			lexNextToken();
			nodesAdd(&ifnode->condblk, parseExpr(parse));
			nodesAdd(&ifnode->condblk, (AstNode*)(blk = newBlockNode()));
			nodesAdd(&blk->stmts, node);
			node = (AstNode*)ifnode;
		}
		else {
			BlockAstNode *blk;
			WhileAstNode *wnode = newWhileNode();
			lexNextToken();
			wnode->condexp = parseExpr(parse);
			wnode->blk = (AstNode*)(blk = newBlockNode());
			nodesAdd(&blk->stmts, node);
			node = (AstNode*)wnode;
		}
	}
	parseSemi();
	return node;
}

// Parse an expression statement within a function
AstNode *parseExpStmt(ParseState *parse) {
	return parseSuffix(parse, (AstNode *)parseExpr(parse));
}

// Parse a return statement
AstNode *parseReturn(ParseState *parse) {
	ReturnAstNode *stmtnode = newReturnNode();
	lexNextToken(); // Skip past 'return'
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseExpr(parse);
	return parseSuffix(parse, (AstNode*)stmtnode);
}

// Parse if statement/expression
AstNode *parseIf(ParseState *parse) {
	IfAstNode *ifnode = newIfNode();
	lexNextToken();
	nodesAdd(&ifnode->condblk, parseExpr(parse));
	nodesAdd(&ifnode->condblk, parseBlock(parse));
	while (lexIsToken(ElifToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, parseExpr(parse));
		nodesAdd(&ifnode->condblk, parseBlock(parse));
	}
	if (lexIsToken(ElseToken)) {
		lexNextToken();
		nodesAdd(&ifnode->condblk, voidType);
		nodesAdd(&ifnode->condblk, parseBlock(parse));
	}
	return (AstNode *)ifnode;
}

// Parse while block
AstNode *parseWhile(ParseState *parse) {
	WhileAstNode *wnode = newWhileNode();
	lexNextToken();
	wnode->condexp = parseExpr(parse);
	wnode->blk = parseBlock(parse);
	return (AstNode *)wnode;
}

// Parse a block of statements/expressions
AstNode *parseBlock(ParseState *parse) {
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
			nodesAdd(&blk->stmts, parseReturn(parse));
			break;

		case IfToken:
			nodesAdd(&blk->stmts, parseIf(parse));
			break;

		case WhileToken:
			nodesAdd(&blk->stmts, parseWhile(parse));
			break;

		case BreakToken:
		{
			AstNode *node;
			newAstNode(node, AstNode, BreakNode);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(parse, node));
			break;
		}

		case ContinueToken:
		{
			AstNode *node;
			newAstNode(node, AstNode, ContinueNode);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(parse, node));
			break;
		}

		case LCurlyToken:
			nodesAdd(&blk->stmts, parseBlock(parse));
			break;

		// A local variable declaration, if it begins with a permission
		case PermToken:
			nodesAdd(&blk->stmts, (AstNode*)parseVarDcl(parse, immPerm, ParseMaySig|ParseMayImpl));
			parseSemi();
			break;

		default:
			nodesAdd(&blk->stmts, parseExpStmt(parse));
		}
	}

	parseRCurly();
	return (AstNode*)blk;
}
