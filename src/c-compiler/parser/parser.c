/** Parser
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
#include "../shared/fileio.h"
#include "../ir/nametbl.h"
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

// Expect closing token (e.g., right parenthesis). If not found, search for it or '}' or ';'
void parseCloseTok(uint16_t closetok) {
	if (!lexIsToken(closetok))
		errorMsgLex(ErrorNoRParen, "Expected right parenthesis - skipping forward to find it");
	while (!lexIsToken(closetok)) {
		if (lexIsToken(EofToken) || lexIsToken(SemiToken) || lexIsToken(RCurlyToken))
			return;
		lexNextToken();
	}
	lexNextToken();
}

// Parse a function block
INode *parseFn(ParseState *parse, uint16_t nodeflags, uint16_t mayflags) {
	FnDclNode *fnnode;

	fnnode = newFnDclNode(NULL, nodeflags, NULL, NULL);
	fnnode->owner = parse->owner;

	// Skip past the 'fn'
	lexNextToken();

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		if (!(mayflags&ParseMayName))
			errorMsgLex(WarnName, "Unnecessary function name is ignored");
		fnnode->namesym = lex->val.ident;
		lexNextToken();
	}
	else {
		if (!(mayflags&ParseMayAnon))
			errorMsgLex(ErrorNoName, "Function declarations must be named");
	}

	// Process the function's signature info.
	fnnode->vtype = parseFnSig(parse);

	// Process statements block that implements function, if provided
	if (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken))
		parseLCurly();
	if (lexIsToken(LCurlyToken)) {
		if (!(mayflags&ParseMayImpl))
			errorMsgLex(ErrorBadImpl, "Function implementation is not allowed here.");
		fnnode->value = parseBlock(parse);
	}
	else {
		if (!(mayflags&ParseMaySig))
			errorMsgLex(ErrorNoImpl, "Function must be implemented.");
		parseSemi();
	}

	return (INode*) fnnode;
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

void parseGlobalStmts(ParseState *parse, ModuleNode *mod);

// Parse include statement
void parseInclude(ParseState *parse) {
	char *filename;
	lexNextToken();
	filename = parseFile();
	parseSemi();

	lexInjectFile(filename);
	parseGlobalStmts(parse, parse->mod);
	if (lex->toktype != EofToken) {
		errorMsgLex(ErrorNoEof, "Expected end-of-file");
	}
	lexPop();
}

// Parse function or variable, as it may be preceded by a qualifier
// Return NULL if not either
INode *parseFnOrVar(ParseState *parse, uint16_t flags) {
	INode *node;

	if (lexIsToken(FnToken)) {
		node = parseFn(parse, 0, (flags&FlagExtern)? (ParseMayName | ParseMaySig) : (ParseMayName | ParseMayImpl));
	}

	// A global variable declaration, if it begins with a permission
	else if lexIsToken(PermToken) {
		node = (INode*)parseVarDcl(parse, immPerm, (flags&FlagExtern) ? ParseMaySig : ParseMayImpl | ParseMaySig);
		parseSemi();
	}
    else {
        errorMsgLex(ErrorBadGloStmt, "Expected function or variable declaration");
        parseSkipToNextStmt();
        return NULL;
    }

	node->flags |= flags;
	return node;
}

ModuleNode *parseModule(ParseState *parse);

// Parse a global area statement (within a module)
// modAddNode adds node to module, as needed, including error message for dupes
void parseGlobalStmts(ParseState *parse, ModuleNode *mod) {
    INode *node;

	// Create and populate a Module node for the program
	while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken)) {
		switch (lex->toktype) {

		case IncludeToken:
			parseInclude(parse);
			break;

		case ModToken:
            modAddNode(mod, (INode*)parseModule(parse));
			break;

		// 'struct'-style type definition
		case StructToken:
            modAddNode(mod, parseStruct(parse));
			break;

        // 'extern' qualifier in front of fn or var (block)
		case ExternToken:
        {
            lexNextToken();
            int16_t extflag = FlagExtern;
            if (lexIsToken(IdentToken)) {
                if (strcmp(&lex->val.ident->namestr, "system")==0)
                    extflag |= FlagSystem;
                lexNextToken();
            }
            if (lexIsToken(LCurlyToken)) {
                lexNextToken();
                while (lexIsToken(FnToken) || lexIsToken(PermToken)) {
                    if (node = parseFnOrVar(parse, extflag))
                        modAddNode(mod, node);
                }
                parseRCurly();
            }
            else
                if (node = parseFnOrVar(parse, extflag))
                    modAddNode(mod, node);
        }
            break;

        // Function or variable
        case FnToken:
		case PermToken:
            if (node = parseFnOrVar(parse, 0))
                modAddNode(mod, node);
			break;

		default:
			errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
			parseSkipToNextStmt();
			break;
		}
	}
}

// Parse a module's global statement block
ModuleNode *parseModuleBlk(ParseState *parse, ModuleNode *mod) {
	parse->mod = mod;
	modHook((ModuleNode*)mod->owner, mod);
	parseGlobalStmts(parse, mod);
	modHook(mod, (ModuleNode*)mod->owner);
	parse->mod = (ModuleNode*)mod->owner;
	return mod;
}

// Parse a submodule within a program
ModuleNode *parseModule(ParseState *parse) {
	INamedNode *svowner = parse->owner;
	ModuleNode *mod;
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
	parse->owner = (INamedNode *)mod;
	mod->namesym = nametblFind(modname, strlen(modname));
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
ModuleNode *parsePgm() {
	ParseState parse;
	ModuleNode *mod;
	mod = newModuleNode();
	parse.pgmmod = mod;
	parse.owner = (INamedNode *)mod;
	return parseModuleBlk(&parse, mod);
}
