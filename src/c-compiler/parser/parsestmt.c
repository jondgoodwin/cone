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

// Parse a variable declaration
AstNode *parseVarDcl() {
	Symbol *namesym = NULL;
	AstNode *vtype;
	PermAstNode *perm;
	AstNode *val;

	// Grab the permission type that we already know is there
	perm = (PermAstNode*)((NameDclAstNode *)lex->val.ident->node)->value;
	lexNextToken();

	// Obtain variable's name
	if (lexIsToken(IdentToken)) {
		namesym = lex->val.ident;
		lexNextToken();
	}
	else {
		errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
		return (AstNode*)newVoidNode();
	}

	// Get value type, if provided
	if ((vtype = parseVtype()) == NULL)
		vtype = voidType;

	// Get initialization value after '=', if provided
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		val = parseExp();
	}
	else
		val = NULL;
	parseSemi();

	return (AstNode*) newNameDclNode(namesym, VarNameDclNode, vtype, perm, val);
}

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

// Parse a statement block inside a control structure
void parseStmtBlock(Nodes **nodes) {
	if (!lexIsToken(LCurlyToken))
		parseLCurly();
	if (!lexIsToken(LCurlyToken))
		return;
	lexNextToken();

	*nodes = newNodes(8);
	while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {
		case RetToken:
			nodesAdd(nodes, parseReturn());
			break;
		default:
			nodesAdd(nodes, parseExpStmt());
		}
	}

	parseRCurly();
}
