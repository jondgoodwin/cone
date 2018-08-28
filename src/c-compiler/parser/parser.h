/** Parser
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef parser_h
#define parser_h

#include "../ir/ir.h"

typedef struct ParseState {
	ModuleNode *pgmmod;	// Root module for program
	ModuleNode *mod;		// Current module
	INamedNode *owner;	// Current namespace owning named nodes
} ParseState;

// When parsing a variable definition, what syntax is allowed?
enum ParseFlags {
	ParseMayName = 0x8000,		// The variable may be named
	ParseMayAnon = 0x4000,		// The variable may be anonymous
	ParseMaySig  = 0x2000,		// The variable may be signature only
	ParseMayImpl = 0x1000		// The variable may implement a code block
};

// parser.c
ModuleNode *parsePgm();
ModuleNode *parseModuleBlk(ParseState *parse, ModuleNode *mod);
INode *parseFn(ParseState *parse, uint16_t nodeflags, uint16_t mayflags);
void parseSemi();
void parseRCurly();
void parseLCurly();
void parseRParen();

// parseflow.c
INode *parseIf(ParseState *parse);
INode *parseBlock(ParseState *parse);

// parseexpr.c
INode *parseExpr(ParseState *parse);

// parsetype.c
INode *parsePerm(PermNode *defperm);
void parseAllocPerm(PtrNode *refnode);
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags);
INode *parseFnSig(ParseState *parse);
INode *parseStruct(ParseState *parse);
INode *parseVtype(ParseState *parse);

#endif
