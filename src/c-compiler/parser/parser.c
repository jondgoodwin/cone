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

// Expect right parenthesis. If not found, search for it or '}' or ';'
void parseRParen() {
	if (!lexIsToken(RParenToken))
		errorMsgLex(ErrorNoRParen, "Expected right parenthesis - skipping forward to find it");
	while (!lexIsToken(RParenToken)) {
		if (lexIsToken(EofToken) || lexIsToken(SemiToken) || lexIsToken(RCurlyToken))
			return;
		lexNextToken();
	}
	lexNextToken();
}

// Parse a function block
AstNode *parseFn() {
	NameDclAstNode *fnnode;
	AstNode *sig;

	// Process the function's signature info. I
	sig = parseFnSig();
	if (sig->asttype != VarNameDclNode) {
		errorMsgNode(sig, ErrorNoName, "Functions declarations must be named");
		return sig;
	}
	fnnode = (NameDclAstNode *)sig;

	// Process statements block that implements function, if provided
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken)) {
		fnnode->value = (AstNode *) newBlockNode();
		parseStmtBlock(&((BlockAstNode*)fnnode->value)->stmts);
	} else
		parseSemi();
	return (AstNode*) fnnode;
}

// Add name declaration to global symbol table if not already defined
void registerGlobalName(NameDclAstNode *name) {
	Symbol *namesym = name->namesym;

	if (namesym->node) {
		errorMsgNode((AstNode *)name, ErrorDupName, "Global name is already defined. Only one allowed.");
		errorMsgNode(namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
	}
	else
		namesym->node = (AstNode*)name;
}

// Parse a program's global area
PgmAstNode *parse() {
	AstNode *node;
	PgmAstNode *pgm;
	Nodes **nodes;

	// Create and populate a Program node for the program
	pgm = newPgmNode();
	nodes = &pgm->nodes;
	while (! lexIsToken( EofToken)) {
		switch (lex->toktype) {

		// 'fn' function definition
		case FnToken:
			nodesAdd(nodes, node=parseFn());
			if (isNameDclNode(node))
				registerGlobalName((NameDclAstNode *)node);
			break;

		// A global variable declaration, if it begins with a permission
		case IdentToken: {
			AstNode *perm = lex->val.ident->node;
			if (perm && perm->asttype == PermNameDclNode) {
				nodesAdd(nodes, node = (AstNode*)parseVarDcl(immPerm));
				parseSemi();
				if (isNameDclNode(node)) {
					NameDclAstNode *vardcl = (NameDclAstNode*)node;
					registerGlobalName(vardcl);
					if (vardcl->value) {
						if (!litIsLiteral(vardcl->value))
							errorMsgNode(node, ErrorNotLit, "Global variable may only be initialized with a literal.");
						else {
							// Type inference is straightforward for literal initializers
							if (vardcl->vtype == voidType)
								vardcl->vtype = ((TypedAstNode *)vardcl->value)->vtype;
						}
					}
				}
				break;
			}
		}
		// Notice, this falls through to below if not a permission

		// Unknown statement
		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
			while (!lexIsToken(SemiToken) && !lexIsToken(EofToken)) {
				lexNextToken();
			}
			lexNextToken();
		}
	}
	return pgm;
}
