/** Parse executable statements and blocks
 * @file
 *
 * The parser translates the lexer's tokens into IR nodes
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
INode *parseSuffix(ParseState *parse, INode *node) {
    // Translate 'this' block sugar to declaring 'this' at start of block
    if (lexIsToken(LCurlyToken)) {
        INode *var = (INode*)newVarDclFull(thisName, VarDclTag, voidType, (INode*)immPerm, node);
        BlockNode *blk = (BlockNode*)parseBlock(parse);
        nodesInsert(&blk->stmts, var, 0);
        return (INode *)blk;
    }
	while (lexIsToken(IfToken) || lexIsToken(WhileToken)) {
		if (lexIsToken(IfToken)) {
			BlockNode *blk;
			IfNode *ifnode = newIfNode();
			lexNextToken();
			nodesAdd(&ifnode->condblk, parseAnyExpr(parse));
			nodesAdd(&ifnode->condblk, (INode*)(blk = newBlockNode()));
			nodesAdd(&blk->stmts, node);
			node = (INode*)ifnode;
		}
		else {
			BlockNode *blk;
			WhileNode *wnode = newWhileNode();
			lexNextToken();
			wnode->condexp = parseAnyExpr(parse);
			wnode->blk = (INode*)(blk = newBlockNode());
			nodesAdd(&blk->stmts, node);
			node = (INode*)wnode;
		}
	}
	parseSemi();
	return node;
}

// Parse an expression statement within a function
INode *parseExpStmt(ParseState *parse) {
	return parseSuffix(parse, (INode *)parseAnyExpr(parse));
}

// Parse a return statement
INode *parseReturn(ParseState *parse) {
	ReturnNode *stmtnode = newReturnNode();
	lexNextToken(); // Skip past 'return'
	if (!lexIsToken(SemiToken))
		stmtnode->exp = parseAnyExpr(parse);
	return parseSuffix(parse, (INode*)stmtnode);
}

// Parse if statement/expression
INode *parseIf(ParseState *parse) {
	IfNode *ifnode = newIfNode();
	lexNextToken();
	nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
	nodesAdd(&ifnode->condblk, parseBlock(parse));
    while (1) {
        // Process final else clause and break loop
        // Note: this code makes "else if" equivalent to "elif"
        if (lexIsToken(ElseToken)) {
            lexNextToken();
            if (!lexIsToken(IfToken)) {
                nodesAdd(&ifnode->condblk, voidType); // else distinguished by a 'void' condition
                nodesAdd(&ifnode->condblk, parseBlock(parse));
                break;
            }
        }
        else if (!lexIsToken(ElifToken))
            break;

        // Elif processing
        lexNextToken();
        nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
        nodesAdd(&ifnode->condblk, parseBlock(parse));
    }
	return (INode *)ifnode;
}

// Parse while block
INode *parseWhile(ParseState *parse) {
	WhileNode *wnode = newWhileNode();
	lexNextToken();
	wnode->condexp = parseSimpleExpr(parse);
	wnode->blk = parseBlock(parse);
	return (INode *)wnode;
}

// Parse each block
INode *parseEach(ParseState *parse) {
    WhileNode *wnode = newWhileNode();
    lexNextToken();
    wnode->condexp = parseSimpleExpr(parse);
    wnode->blk = parseBlock(parse);
    return (INode *)wnode;
}

// Parse a block of statements/expressions
INode *parseBlock(ParseState *parse) {
	BlockNode *blk = newBlockNode();

	if (!lexIsToken(LCurlyToken))
		parseLCurly();
	if (!lexIsToken(LCurlyToken))
		return (INode*)blk;
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

        case EachToken:
            nodesAdd(&blk->stmts, parseEach(parse));
            break;

        case BreakToken:
		{
			INode *node = (INode*)newBreakNode(BreakTag);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(parse, node));
			break;
		}

		case ContinueToken:
		{
            INode *node = (INode*)newBreakNode(ContinueTag);
			lexNextToken();
			nodesAdd(&blk->stmts, parseSuffix(parse, node));
			break;
		}

        case DoToken:
            lexNextToken();
            nodesAdd(&blk->stmts, parseBlock(parse));
            break;

		case LCurlyToken:
			nodesAdd(&blk->stmts, parseBlock(parse));
			break;

		// A local variable declaration, if it begins with a permission
		case PermToken:
			nodesAdd(&blk->stmts, (INode*)parseVarDcl(parse, immPerm, ParseMayConst|ParseMaySig|ParseMayImpl));
			parseSemi();
			break;

		default:
			nodesAdd(&blk->stmts, parseExpStmt(parse));
		}
	}

	parseRCurly();
	return (INode*)blk;
}
