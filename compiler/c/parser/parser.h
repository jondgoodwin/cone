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
    ModuleNode *mod;        // Current module
    INsTypeNode *typenode;  // Current type
    int inrettype;          // Non-zero while parseFnSig reads a return type, where a '{' opens the declared function's body
    ModuleNode *core;       // The core package, once loaded: every module loaded after it imports it
} ParseState;

// When parsing a variable definition, what syntax is allowed?
enum ParseFlags {
    ParseMayName = 0x8000,        // The variable may be named
    ParseMayAnon = 0x4000,        // The variable may be anonymous
    ParseMaySig  = 0x2000,        // The variable may be signature only
    ParseMayImpl = 0x1000,        // The variable may implement a code block
    ParseEmbedded = 0x0800,       // Is embedded in expression (no semi)
    ParseMayFold = 0x0400,        // The variable may carry a fold clause: a module's global
};

// parsemod.c
ProgramNode *parsePgm(ConeOptions *opt);
// Consume a 'pub' that precedes a declaration, returning FlagPub, or 0
uint16_t parsePub();
// Consume a 'static' that precedes a declaration, returning FlagStatic, or 0
uint16_t parseStatic();
// Report 'static' on a declaration that has no per-instance copies to share
void parseBadStatic(uint16_t staticflag);

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
// Parse an operand of a comparison: everything that binds tighter than one
INode *parseOr(ParseState *parse);
// Finish a simple expression whose first operand parseOr has already parsed
INode *parseSimpleExprFrom(ParseState *parse, INode *lhnode);
// The method a comparison token names ("==", "<", ...), or NULL
char *parseCmpOp();
INode *parseAnyExpr(ParseState *parse);
// Parse a name use: one identifier
INode *parseNameUse(ParseState *parse);
// Parse a '.'-based member access or namespace hop, applied to an existing node
INode *parseDotCall(ParseState *parse, INode *node, uint16_t flags);
// Parse a term: literal, identifier, etc.
INode *parseTerm(ParseState *parse);
// Parse a prefix operator
INode *parsePrefix(ParseState *parse);

// parsetype.c
INode *parsePerm();

// Parse the permission a declaration carries, defaulting to 'defperm'
INode *parseDclPerm(PermNode *defperm);
VarDclNode *parseVarDcl(ParseState *parse, PermNode *defperm, uint16_t flags);
// What a fold clause's site does with a 'pub': gives the clause a visibility of
// its own (a module's global, an import), refuses it (a field), or reads it and
// drops it with a clause the site has refused whole already
enum FoldPub { FoldNoPub, FoldMayPub, FoldRecover };
// Is the lexer on a fold clause: its 'use', or the 'pub' written before it?
int parseIsFoldClause();
// Parse a fold clause, with the lexer on its 'use' or on the 'pub' before it
FoldClause *parseFoldClause(ParseState *parse, int maypub);
// Parse a module's standalone 'use' -- of an enum or of a submodule -- with the
// lexer on the 'use'. 'pubflag' is the 'pub' written before it.
ModUseNode *parseModUse(ParseState *parse, uint16_t pubflag);
ConstDclNode *parseConstDcl(ParseState *parse);
INode *parseFnSig(ParseState *parse);
INode *parseStruct(ParseState *parse, uint16_t flags);
INode *parseType(ParseState *parse);
AliasDclNode *parseTypedef(ParseState *parse);

// parsehelper.c for statement/block start/end processing
// Skip to next statement for error recovery
void parseSkipToNextStmt();
// Is this end-of-statement? if ';', '}', or end-of-file
int parseIsEndOfStatement();
// Require the ';' that ends every statement not ending in a block
void parseEndOfStatement();
// Return true if a block starts here: '{', or the ':' the language no longer has
int parseHasBlock();
// Expect '{' and consume it
void parseBlockStart();
// Are we at end of block yet? If so, consume '}'
int parseBlockEnd();
// Expect closing token (e.g., right parenthesis). If not found, search for it or '}' or ';'
void parseCloseTok(uint16_t closetok);

#endif
