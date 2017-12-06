/** Parser
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

// Expect right curly brace. If not found, search for '}' or ';'
void parseRCurly() {
	if (!lexIsToken(RCurlyToken))
		errorMsgLex(ErrorNoRCurly, "Expected closing brace '}' - skipping forward to find it");
	while (! lexIsToken(RCurlyToken) && ! lexIsToken(SemiToken)) {
		if (lexIsToken(EofToken))
			return;
		lexNextToken();
	}
	lexNextToken();
}

// Expect left curly brace. If not found, search for '{'
void parseLCurly() {
	errorMsgLex(ErrorNoLCurly, "Expected opening brace '{' - skipping forward to find it");
	while (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken) && !lexIsToken(EofToken)) {
		lexNextToken();
	}
}

// Parse a function block
AstNode *parseFn() {
	FnImplAstNode *fnnode;
	FnSigAstNode *sig;

	// Process the function's signature info, then put info in new AST node
	sig = (FnSigAstNode*) parseFnSig();
	fnnode = newFnImplNode(sig->name, (AstNode*) sig);

	// Process statements block that implements function, if provided
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken))
		parseStmtBlock(&fnnode->nodes);
	else
		parseSemi();
	return (AstNode*) fnnode;
}

// Parse a program's global area
PgmAstNode *parse() {
	PgmAstNode *pgm;
	Nodes **nodes;

	// Create and populate a Program node for the program
	pgm = newPgmNode();
	nodes = &pgm->nodes;
	while (! lexIsToken( EofToken)) {
		switch (lex->toktype) {
		case FnToken:
			nodesAdd(nodes, parseFn());
			break;
		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area type, var or function statement");
			while (!lexIsToken(SemiToken) && !lexIsToken(EofToken)) {
				lexNextToken();
			}
			lexNextToken();
		}
	}
	return pgm;
}
