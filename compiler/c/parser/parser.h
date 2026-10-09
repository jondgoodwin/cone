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

// A BUILD DESCRIPTION is what Congo hands the compiler: which files make up
// each module of one package, where each module's imports are, and what to
// produce (parsebuild.c). These are its entries, read before anything is parsed.

// 'import name: "path"': where a described module's 'import name' is found; or,
// written at the top level, a PACKAGE LINE, where an include file's is found
typedef struct BuildImport {
    Name *name;
    char *path;             // Canonical; a relative path is relative to the description's folder
} BuildImport;

// 'name: { ... }': one module of the package, its files and its child modules
typedef struct BuildModule {
    Name *name;
    char **files;           // Canonical paths, in the order written; the first may declare the module
    struct BuildModule **children;
    BuildImport *imports;
    uint32_t nfiles, nchildren, nimports;
    uint32_t availfiles, availchildren, availimports;
    int isimport;           // Stands for the file an import line names, whose name is the import's,
                            // and whose imports are the description's package lines
} BuildModule;

typedef struct BuildDesc {
    BuildModule *root;      // The package's module
    BuildModule *packages;  // The package lines, as the import lines of an entry standing for no module
    int library;            // 'output: library': the root is named, and prefixes every symbol
} BuildDesc;

// A generator's signature as parseFnSig read it, 'fn walk(t &Tree) yields &Node':
// where its parameter list and the type after 'yields' are written, which the
// declarations it stands for are generated from (parsegen.c)
typedef struct GenSig {
    char *parms, *parmsend;     // The parameter list: its '(' to just past its ')'
    char *ytype, *ytypeend;     // The type after 'yields'
} GenSig;

// The generator whose body is being read: the names of the functions that make
// its results, which a 'yield' and a 'return' in the body call (parsegen.c)
typedef struct GenCtx {
    Name *some;             // Makes 'Some(value)', what a 'yield' hands the caller
    Name *none;             // Makes 'None', what the body's end and a 'return' hand it
    Name *name;             // The generator's own name, for a message
    int yields;             // How many 'yield's the body holds so far
} GenCtx;

// Where a field's or a parameter's type and default value are written, which
// an 'actor' copies into the declarations it generates (parseactor.c)
typedef struct DclText {
    INode *dcl;             // The field or parameter
    char *type, *typeend;   // Its type as written; NULL where none is
    char *value, *valueend; // Its default value, after the '='; NULL where none is
} DclText;

typedef struct DclTexts {
    DclText *items;
    uint32_t count, avail;
    uint32_t awaits;        // How many 'await's the actor's body holds so far
    Nodes *awaiting;        // Each function of the body whose own body holds an 'await' (parseFn)
    uint32_t selfactors;    // How many times the actor's body writes 'selfactor'
    Nodes *behaviours;      // Each function of the body declared 'async do', a behaviour (parseStruct)
} DclTexts;

typedef struct ParseState {
    ProgramNode *pgm;       // Program node
    ModuleNode *mod;        // Current module
    INsTypeNode *typenode;  // Current type
    int inrettype;          // Non-zero while parseFnSig reads a return type, where a '{' opens the declared function's body
    int inlist;             // Non-zero inside a parenthesised or bracketed list, where a comma continues the list, so a
                            // function signature read there ends its return type before it (parseFnSig)
    int intype;             // Non-zero while parseType reads a type, where '&new' is a permission, not a borrow of a construction
    FnSigNode *lifesig;     // The signature whose types are being read, where a type may name lifetimes ('&'a T', 'Cursor['a]'); NULL elsewhere
    StructNode *lifestruct; // The struct (or enum) whose field's type is being read, which may name its lifetimes; NULL elsewhere
    int entryparen;         // Non-zero while an entry after '<-' is begun: a '(' first is a parenthesized list of entries (parseEntry)
    ModuleNode *core;       // The core package, once loaded: every module loaded after it imports it
    BuildModule *build;     // The build description's entry for the current module; NULL where it is not described
    int generated;          // The file being parsed is a generated include file, which alone may
                            // hold a nested module block, 'mod sub { ... }' (parseModuleBlock)
    Nodes *blockmods;       // Where a nested block's module goes instead of the program's modules:
                            // the generator's self-check, which parses beside the program

    // Where the declaration parseFn or parseVarDcl last read has its body or
    // value, for the span its caller records (dclspan.h). Each is written as the
    // function returns, so a function nested in a body does not disturb it
    char *bodyp;            // A function's '{', a variable's '='; NULL where it has none
    char *bodyendp;         // Just past that body or value
    char *nameendp;         // A variable: just past its name
    int typed;              // A variable: its type is written
    char *typep;            // A variable: where its type is written, and just past it; NULL where none is
    char *typeendp;
    // Where a struct's header ends and where its 'is' list does, for the span
    // of the struct just parsed (dclspan.h); written as parseStruct returns
    char *hdrendp;
    char *isendp;

    // While an actor's body is read, where each field's and parameter's type
    // and default value are written (parseFieldDclBody, parseFnSig); else NULL
    DclTexts *dcltexts;

    // A generator (parsegen.c). parseFnSig writes 'isgen' and 'gensig' as it
    // returns, for the parseFn that called it; 'genctx' is the generator whose
    // body is being read, NULL elsewhere (a function nested in the body is no
    // generator)
    int isgen;
    GenSig gensig;
    GenCtx *genctx;
} ParseState;

// Record where a field's or a parameter's type and value are written, when an
// actor's body is being read
void parseDclText(ParseState *parse, INode *dcl, char *type, char *typeend, char *value, char *valueend);

// actor: 'actor Name { ... }', with the lexer on 'actor' (parseactor.c)
void parseActor(ParseState *parse, uint16_t pubflag);

// With the lexer on 'async' or 'do': read 'async do' up to the 'do', which
// parseFn then skips as it does 'fn'. Where the two are not written together,
// or 'where' is not an actor's body ('where' names what is being declared
// into, NULL in an actor's), each is reported, and the declaration is read as
// a function where one follows. Returns whether parseFn should read one
// (parseactor.c)
int parseBehaviourWords(char *where);

// parsegen.c: generators, 'fn walk(t &Tree) yields &Node { ... yield n; ... }'
//
// Called by parseFn, a generator's signature read: its body is read with the
// generator's context set (parseGenBegin), and then parseGenFinish makes the
// declarations the generator stands for, returning the one that takes its name,
// the function that makes the generator, in place of the one parseFn read
GenCtx *parseGenBegin(ParseState *parse, FnDclNode *fn);
FnDclNode *parseGenFinish(ParseState *parse, FnDclNode *fn, GenSig *sig, GenCtx *ctx, BlockNode *body);
// A 'yield' statement, with the lexer on 'yield': 'yield e;' or 'yield each src;'
INode *parseYield(ParseState *parse);
// The call giving a generator's 'None', which its 'return' and the end of its body hand the caller
INode *parseGenNone(ParseState *parse);

// When parsing a variable definition, what syntax is allowed?
enum ParseFlags {
    ParseMayName = 0x8000,        // The variable may be named
    ParseMayAnon = 0x4000,        // The variable may be anonymous
    ParseMaySig  = 0x2000,        // The variable may be signature only
    ParseMayImpl = 0x1000,        // The variable may implement a code block
    ParseEmbedded = 0x0800,       // Is embedded in expression (no semi)
    ParseMayFold = 0x0400,        // The variable may carry a fold clause: a module's global
    ParseInList = 0x0200,         // A comma ends the value: a parameter, whose list the comma continues
    ParseMayThreadLocal = 0x0100, // The variable may be '@threadlocal' or '@workgroup': a module's global
};

// parsebuild.c
// Does this source path name a build description, by its extension?
int parseIsBuildDesc(char *path);
// Read the build description at opt->srcpath, before generation is set up,
// since its 'build' line decides whether the output is optimised
BuildDesc *parseBuildDesc(ConeOptions *opt);
// The import line a described module writes for this name, or NULL
BuildImport *parseBuildFindImport(BuildModule *build, Name *name);
// The entry that stands for the module an import line's file draws: an include
// file, whose own imports the description's package lines answer
BuildModule *parseBuildImportModule(BuildImport *import);

// parsemod.c
// Set up the name table and the lexer, which a build description needs too
void parseInit(ConeOptions *opt);
ProgramNode *parsePgm(ConeOptions *opt, BuildDesc *desc);
// Parse a generated include file's text as the module it declares, beside the
// root it stands for, for the generator's self-check. Returns that module and
// then the modules its nested blocks declare, none added to the program's
Nodes *parseIncludeCheck(ProgramNode *pgm, BuildDesc *desc, char *text, char *url);
// folder + name, where folder carries its trailing slash
char *parsePathJoin(char *folder, char *name);
// Consume a 'pub' that precedes a declaration, returning FlagPub, or 0
uint16_t parsePub();
// Consume a 'static' that precedes a declaration, returning FlagStatic, or 0
uint16_t parseStatic();
// Report 'static' on a declaration that has no per-instance copies to share
void parseBadStatic(uint16_t staticflag);
// Parse a '@c' marker, if the lexer is on one, into dclinfo's facts and string:
// on a module ('onmod') the string is a prefix, on a fn the whole symbol
int parseCAttr(DclInfo *dclinfo, int onmod);
// Report 'extern' on an inline or generic fn, whose body its user must have
void parseExternFnCheck(FnDclNode *fn);
// A module's 'fn' or global, added to the module
INode *parseFnOrVar(ParseState *parse, uint16_t flags);
// Skip a declaration's '{ ... }' whole, or resync at the next ';'
void parseSkipDclBody();

// parsefnflow.c
INode *parseFn(ParseState *parse, uint16_t mayflags);
// Parse a macro declaration
MacroDclNode *parseMacro(ParseState *parse);
// Parse a list of generic variables and add to the genericnode; a struct's
// lifetimes among them go to '*lifes' (NULL refuses them), and a type
// parameter's lifetime bounds, '[T + 'a]', to '*bounds' (NULL refuses them)
Nodes *parseGenericParms(ParseState *parse, int annotate, LifeParms **lifes, LifeOrder **bounds);
// Parse a 'where' clause, with the lexer on 'where', into '*wherep', and its
// lifetime comparisons into '*orderp' (NULL refuses them), with its type
// parameters' lifetime bounds, 'T + 'a', where 'bounds' allows them
void parseWhere(ParseState *parse, Nodes **wherep, LifeOrder **orderp, int bounds);
// Parse the condition on one entry of a type's 'is' list, with the lexer on
// its 'if' ('is Move if T is Move'), read as a 'where' clause's is. NULL,
// reported, where it is refused.
INode *parseIsCondition(ParseState *parse);
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
// Consume a '@threadlocal' or '@workgroup', if the lexer is on one, returning
// the declaration fact it states (DclThreadLocal, DclWorkgroup) where
// 'allowed' -- a module's global -- else 0. Anywhere else it is reported
uint16_t parseStorageAttr(int allowed);
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
// Parse a function's signature; 'reftype' for the one after '&fn', which may
// be a function-reference type with unnamed parameters
INode *parseFnSig(ParseState *parse, int reftype);
// Settle a '&fn' signature's parameters once it is known whether it is a type
// ('istype': no body follows) or an anonymous function's
void parseFnSigSettle(ParseState *parse, FnSigNode *sig, int istype);
INode *parseStruct(ParseState *parse, uint16_t flags);
INode *parseType(ParseState *parse);
// Is the lexer on a token that may begin a type expression?
int parseIsTypeStart();
INode *parseTypeReq(ParseState *parse, char *after);
// A type's name: a name, a path through namespaces, or a generic's instance
INode *parseTypeName(ParseState *parse);
// 'alias Name = target;'
AliasDclNode *parseAlias(ParseState *parse);

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
// Record the span of the statement just parsed, from 'start' (its 'pub', where
// written) and 'kw' (the token after that) to the end of its last token, in the
// list '*listp' points to. A function's or variable's body is the one parseFn or
// parseVarDcl just read
DclSpan *parseSpan(ParseState *parse, DclSpans **listp, INode *node, char *start, char *kw, uint16_t kind);

#endif
