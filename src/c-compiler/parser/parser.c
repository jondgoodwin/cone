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
AstNode *parseFn(ParseState *parse) {
	NameDclAstNode *fnnode;
	AstNode *sig;

	// Process the function's signature info. I
	sig = parseFnSig(parse);
	if (sig->asttype != VarNameDclNode) {
		errorMsgNode(sig, ErrorNoName, "Functions declarations must be named");
		return sig;
	}
	fnnode = (NameDclAstNode *)sig;
	fnnode->owner = parse->owner;

	// Process statements block that implements function, if provided
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken))
		fnnode->value = parseBlock(parse);
	else
		parseSemi();
	return (AstNode*) fnnode;
}

// Add name declaration to global symbol table if not already defined
void registerGlobalName(NameDclAstNode *name) {
	Symbol *namesym = name->namesym;

	if (namesym->node) {
		errorMsgNode((AstNode *)name, ErrorDupName, "Global name is already defined. Only one allowed.");
		errorMsgNode((AstNode*)namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
	}
	else
		namesym->node = (NamedAstNode*)name;
}

ModuleAstNode *parseModule(ParseState *parse);

// Parse a module's global statement block
ModuleAstNode *parseModuleBlk(ParseState *parse, ModuleAstNode *mod) {
	AstNode *node;
	Nodes **nodes;

	// Create and populate a Module node for the program
	nodes = &mod->nodes;
	while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {

		// 'fn' function definition
		case FnToken:
			nodesAdd(nodes, node=parseFn(parse));
			if (isNameDclNode(node))
				registerGlobalName((NameDclAstNode *)node);
			break;

		// 'struct' definition
		case StructToken:
		case AllocToken:
			nodesAdd(nodes, node = parseStruct(parse));
			registerGlobalName((NameDclAstNode *)node);
			break;

		case ModToken:
			nodesAdd(nodes, parseModule(parse));
			break;

		// A global variable declaration, if it begins with a permission
		case IdentToken: {
			NamedAstNode *perm = lex->val.ident->node;
			if (perm && perm->asttype == PermNameDclNode) {
				nodesAdd(nodes, node = (AstNode*)parseVarDcl(parse, immPerm));
				parseSemi();
				if (isNameDclNode(node)) {
					NameDclAstNode *vardcl = (NameDclAstNode*)node;
					registerGlobalName(vardcl);
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
	return mod;
}

// Parse a submodule within a program
ModuleAstNode *parseModule(ParseState *parse) {
	NamedAstNode *svowner = parse->owner;
	ModuleAstNode *mod;
	mod = newModuleNode();
	parse->owner = (NamedAstNode *)mod;
	lexNextToken();
	// Process mod name
	if (lexIsToken(IdentToken)) {
		mod->namesym = lex->val.ident;
		lexNextToken();
	}
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken)) {
		lexNextToken();
		parseModuleBlk(parse, mod);
		parseRCurly();
	}
	else
		parseSemi();
	parse->owner = svowner;
	return mod;
}

// Parse a program = the main module
ModuleAstNode *parsePgm() {
	ParseState parse;
	ModuleAstNode *mod;
	mod = newModuleNode();
	parse.owner = (NamedAstNode *)mod;
	return parseModuleBlk(&parse, mod);
}
