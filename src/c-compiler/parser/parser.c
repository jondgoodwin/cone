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
	if (!lexIsToken(SemiToken))
		errorMsgLex(ErrorNoSemi, "Expected semicolon - skipping forward to find it");
	while (! lexIsToken(SemiToken)) {
		if (lexIsToken(EofToken) || lexIsToken(RCurlyToken))
			return;
		lexNextToken();
	}
	lexNextToken();
}

// Expect right curly brace and move past
void parseRCurly() {
	if (lexIsToken(RCurlyToken))
		lexNextToken();
	else
		errorMsgLex(ErrorNoRCurly, "Expected closing brace '}'");
}

// Parse a function block
AstNode *parseFn() {
	FnBlkAstNode *fnnode;
	Symbol *fnsym;
	Symbol nosym;
	astNewNode(fnnode, FnBlkAstNode, FnBlkNode);

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		fnsym = lex->val.ident;
		lexNextToken();
	}
	else {
		// For anonymous function, create and populate fake symbol table entry
		fnsym = &nosym;
		fnsym->name = NULL;
		fnsym->node = NULL;
	}

	// Process parameter declarations
	if (lexIsToken(LParenToken)) {
		lexNextToken();
		if (lexIsToken(RParenToken))
			lexNextToken();
		else
			errorMsgLex(ErrorNoRParen, "Expected right parenthesis that ends parameters");
	}
	else
		errorMsgLex(ErrorNoLParen, "Expected left parenthesis for parameter declarations");

	// Error out if name is already used but types don't match

	// Process implementation block, if provided
	if (lexIsToken(LCurlyToken)) {
		// If func is already fully defined with an implementation, error out
		if (fnsym->node && fnsym->node->asttype == FnBlkNode &&
			((FnBlkAstNode*)fnsym->node)->nodes)
			errorMsgNode((AstNode *)fnnode, ErrorFnDupImpl, "Function already has an implementation");
		else
			fnsym->node = (AstNode*)fnnode;
		parseStmtBlock(&fnnode->nodes);
	} else {
		parseSemi();
		// Attach AST to symbol, if it is not defined already
		if (fnsym->node == NULL)
			fnsym->node = (AstNode*)fnnode;
	}
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
	while (! lexIsToken( EofToken)) {
		switch (lex->toktype) {
		case FnToken:
			lexNextToken();
			nodesAdd(nodes, parseFn());
			break;
		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area type, var or function statement");						
		}
	}
	return (AstNode*) global;
}
