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
#include "../ast/nametbl.h"
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

void parseStmts(ParseState *parse, ModuleAstNode *mod);

// Parse include statement
void parseInclude(ParseState *parse) {
	char *filename;
	lexNextToken();
	switch (lex->toktype) {
	case IdentToken:
		filename = &lex->val.ident->namestr;
		lexNextToken();
		break;
	case StrLitToken:
		filename = lex->val.strlit;
		lexNextToken();
		break;
	default:
		return;
	}
	parseSemi();

	lexInjectFile(filename);
	parseStmts(parse, parse->mod);
	if (lex->toktype != EofToken) {
		// error message
	}
	lexPop();
}

ModuleAstNode *parseModule(ParseState *parse);

void parseStmts(ParseState *parse, ModuleAstNode *mod) {
	Nodes **modnodes = &mod->nodes;
	AstNode *node;
	Name *alias;

	// Create and populate a Module node for the program
	while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken)) {
		alias = NULL;
		switch (lex->toktype) {

		// 'fn' function definition
		case FnToken:
			node = parseFn(parse);
			break;

		// 'struct' definition
		case StructToken:
		case AllocToken:
			node = parseStruct(parse);
			break;

		case IncludeToken:
			parseInclude(parse);
			continue;

		case ModToken:
			node = (AstNode*)parseModule(parse);
			break;

		// A global variable declaration, if it begins with a permission
		case IdentToken: {
			NamedAstNode *perm = lex->val.ident->node;
			if (perm && perm->asttype == PermNameDclNode) {
				node = (AstNode*)parseVarDcl(parse, immPerm);
				parseSemi();
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
			continue; // restart loop for next statement
		}

		// Add parsed node to module's nodes, module's namednodes and global name table
		// Note: will generate an error if name is a duplicate
		nodesAdd(modnodes, node);
		if (isNamedNode(node)) {
			modAddNamedNode(mod, (NamedAstNode*)node, alias);
		}
	}
}

// Parse a module's global statement block
ModuleAstNode *parseModuleBlk(ParseState *parse, ModuleAstNode *mod) {
	parse->mod = mod;
	modHook((ModuleAstNode*)mod->owner, mod);
	parseStmts(parse, mod);
	modHook(mod, (ModuleAstNode*)mod->owner);
	parse->mod = (ModuleAstNode*)mod->owner;
	return mod;
}

// Parse a submodule within a program
ModuleAstNode *parseModule(ParseState *parse) {
	NamedAstNode *svowner = parse->owner;
	ModuleAstNode *mod;
	mod = newModuleNode();
	mod->owner = svowner;
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
	parse.pgmmod = mod;
	parse.owner = (NamedAstNode *)mod;
	return parseModuleBlk(&parse, mod);
}
