/** Parser
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef parser_h
#define parser_h

#include "../ast/ast.h"

typedef struct ParseState {
	NamedAstNode *owner;	// Current namespace owning named nodes
} ParseState;

// parser.c
ModuleAstNode *parseModule();
void parseSemi();
void parseRCurly();
void parseLCurly();
void parseRParen();

// parseflow.c
AstNode *parseIf(ParseState *parse);
AstNode *parseBlock(ParseState *parse);

// parseexpr.c
AstNode *parseExpr(ParseState *parse);

// parsetype.c
PermAstNode *parsePerm(PermAstNode *defperm);
void parseAllocPerm(PtrAstNode *refnode);
NameDclAstNode *parseVarDcl(ParseState *parse, PermAstNode *defperm);
AstNode *parseFn(ParseState *parse);
AstNode *parseFnSig(ParseState *parse);
AstNode *parseStruct(ParseState *parse);
AstNode *parseVtype(ParseState *parse);

#endif
