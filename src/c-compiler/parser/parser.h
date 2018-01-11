/** Parser
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef parser_h
#define parser_h

#include "../ast/ast.h"

// parser.c
PgmAstNode *parse();
void parseSemi();
void parseRCurly();
void parseLCurly();
void parseRParen();

// parseflow.c
AstNode *parseIf();
AstNode *parseBlock();

// parseexpr.c
AstNode *parseExpr();

// parsetype.c
PermAstNode *parsePerm(PermAstNode *defperm);
NameDclAstNode *parseVarDcl(PermAstNode *defperm);
AstNode *parseFnSig();
AstNode *parseVtype();

#endif
