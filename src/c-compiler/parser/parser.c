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
#include "../shared/fileio.h"
#include "../ast/nametbl.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// Skip to next statement
void parseSkipToNextStmt() {
	while (!lexIsToken(SemiToken)) {
		if (lexIsToken(EofToken) || lexIsToken(RCurlyToken))
			return;
		lexNextToken();
	}
	lexNextToken();
}

// We expect semicolon since statement has run its course
void parseSemi() {
	if (!lexIsToken(SemiToken))
		errorMsgLex(ErrorNoSemi, "Expected semicolon - skipping forward to find it");
	parseSkipToNextStmt();
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
AstNode *parseFn(ParseState *parse, int16_t flags) {
	VarDclAstNode *fnnode;

	fnnode = newVarDclNode(NULL, VarNameDclNode, NULL, immPerm, NULL);
	fnnode->owner = parse->owner;

	// Skip past the 'fn'
	lexNextToken();

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		if (!(flags&ParseMayName))
			errorMsgLex(WarnName, "Unnecessary function name is ignored");
		fnnode->namesym = lex->val.ident;
		lexNextToken();
	}
	else {
		if (!(flags&ParseMayAnon))
			errorMsgLex(ErrorNoName, "Functions declarations must be named");
	}

	// Process the function's signature info. I
	fnnode->vtype = parseFnSig(parse);

	// Process statements block that implements function, if provided
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken)) {
		if (!(flags&ParseMayImpl))
			errorMsgLex(ErrorBadImpl, "Function implementation is not allowed here.");
		fnnode->value = parseBlock(parse);
	}
	else {
		if (!(flags&ParseMaySig))
			errorMsgLex(ErrorNoImpl, "Function must be implemented.");
		parseSemi();
	}

	return (AstNode*) fnnode;
}

// Parse source filename/path as identifier or string literal
char *parseFile() {
	char *filename;
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
		errorExit(ExitNF, "Invalid source file; expected identifier or string");
		filename = NULL;
	}
	return filename;
}

void parseStmts(ParseState *parse, ModuleAstNode *mod);

// Parse include statement
void parseInclude(ParseState *parse) {
	char *filename;
	lexNextToken();
	filename = parseFile();
	parseSemi();

	lexInjectFile(filename);
	parseStmts(parse, parse->mod);
	if (lex->toktype != EofToken) {
		errorMsgLex(ErrorNoEof, "Expected end-of-file");
	}
	lexPop();
}

// Parse function or variable, as it may be preceded by a qualifier
// Return NULL if not either
AstNode *parseFnOrVar(ParseState *parse) {
	AstNode *node;
	int16_t flags = 0;

	// extern keyword
	if (lexIsToken(ExternToken)) {
		flags |= FlagExtern;
		lexNextToken();
	}

	if (lexIsToken(FnToken)) {
		node = parseFn(parse, (flags&FlagExtern)? (ParseMayName | ParseMaySig) : (ParseMayName | ParseMayImpl));
	}

	// A global variable declaration, if it begins with a permission
	else if lexIsToken(PermToken) {
		node = (AstNode*)parseVarDcl(parse, immPerm, (flags&FlagExtern) ? ParseMaySig : ParseMayImpl);
		parseSemi();
	}
	else
		return NULL;

	node->flags |= flags;
	return node;
}

ModuleAstNode *parseModule(ParseState *parse);

void parseStmts(ParseState *parse, ModuleAstNode *mod) {
	Nodes **modnodes = &mod->nodes;
	AstNode *node;
	Name *alias;

	// Create and populate a Module node for the program
	while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken)) {
		node = NULL;
		alias = NULL;
		switch (lex->toktype) {

		case IncludeToken:
			parseInclude(parse);
			continue; // Does not generate a node

		case ModToken:
			node = (AstNode*)parseModule(parse);
			break;

		// 'struct' definition
		case StructToken:
		case AllocToken:
			node = parseStruct(parse);
			break;

		case ExternToken:
		case FnToken:
		case PermToken:
			if (!(node = parseFnOrVar(parse))) {
				errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
				parseSkipToNextStmt();
				continue; // restart loop for next statement
			}
			break;

		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
			parseSkipToNextStmt();
			continue; // restart loop for next statement
		}

		// Add parsed node to module's nodes, module's namednodes and global name table
		// Note: will generate an error if name is a duplicate
		if (node != NULL) {
			nodesAdd(modnodes, node);
			if (isNamedNode(node)) {
				modAddNamedNode(mod, (NamedAstNode*)node, alias);
			}
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
	char *filename, *modname;

	// Parse enough to know what we are dealing with
	lexNextToken();
	filename = parseFile();
	modname = fileName(filename);
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();

	// This is a new module, build it
	mod = newModuleNode();
	mod->owner = svowner;
	parse->owner = (NamedAstNode *)mod;
	mod->namesym = nameFind(modname, strlen(modname));
	if (lexIsToken(LCurlyToken)) {
		lexNextToken();
		parseModuleBlk(parse, mod);
		parseRCurly();
	}
	else {
		parseSemi();
		lexInjectFile(filename);
		parseModuleBlk(parse, mod);
		lexPop();
	}
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
