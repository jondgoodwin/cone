/** Parser
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef parser_h
#define parser_h

#include "../ir/ir.h"
typedef struct ConeOptions ConeOptions;

typedef struct ParseState {
    ProgramNode *pgm;       // Program node
    ModuleNode *pgmmod;     // Root module for program
    ModuleNode *mod;        // Current module
    INsTypeNode *typenode;  // Current type
    char *gennamePrefix;    // Module or type prefix for unique linker names
} ParseState;

// When parsing a variable definition, what syntax is allowed?
enum ParseFlags {
    ParseMayName = 0x8000,        // The variable may be named
    ParseMayAnon = 0x4000,        // The variable may be anonymous
    ParseMaySig  = 0x2000,        // The variable may be signature only
    ParseMayImpl = 0x1000,        // The variable may implement a code block
    ParseEmbedded = 0x0800,       // Is embedded in expression (no semi)
    ParseMayConst = 0x0400        // const allowed for variable declaration
};

// parsemod.c
ProgramNode *parsePgm(ConeOptions *opt);
ModuleNode *parseModuleBlk(ParseState *parse, ModuleNode *mod);

// parsefnflow.c
INode *parseFn(ParseState *parse, uint16_t mayflags);
// Parse a macro declaration
MacroDclNode *parseMacro(ParseState *parse);
// Parse a list of generic variables and add to the genericnode
Nodes *parseGenericParms(ParseState *parse);
INode *parseIf(ParseState *parse);
INode *parseMatch(ParseState *parse);
INode *parseWhile(ParseState *parse, Name *lifesym, int stmtflag);
// Parse an expression block
INode *parseExprBlock(ParseState *parse, int isloop);
INode *parseLifetime(ParseState *parse, int stmtflag);

// parseexpr.c
INode *parseSimpleExpr(ParseState *parse);
INode *parseAnyExpr(ParseState *parse);
// Parse a name use, which may be qualified with module names
INode *parseNameUse(ParseState *parse);
// Parse a term: literal, identifier, etc.
INode *parseTerm(ParseState *parse);
// Parse a prefix operator
INode *parsePrefix(ParseState *parse, int noSuffix);

// parsetype.c
INode *parsePerm();
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags);
ConstDclNode *parseConstDcl(ParseState *parse);
INode *parseFnSig(ParseState *parse);
INode *parseStruct(ParseState *parse, uint16_t flags);
INode *parseVtype(ParseState *parse);
TypedefNode *parseTypedef(ParseState *parse);

// parsehelper.c for statement/block start/end processing
// Skip to next statement for error recovery
void parseSkipToNextStmt();
// Is this end-of-statement? if ';', '}', or end-of-file
int parseIsEndOfStatement();
// We expect optional semicolon since statement has run its course
void parseEndOfStatement();
// Return true on '{' or ':'
int parseHasBlock();
// Expect a block to start, consume its token and set lexer mode
void parseBlockStart();
// Are we at end of block yet? If so, consume token and reset lexer mode
int parseBlockEnd();
// Expect closing token (e.g., right parenthesis). If not found, search for it or '}' or ';'
void parseCloseTok(uint16_t closetok);

#endif
