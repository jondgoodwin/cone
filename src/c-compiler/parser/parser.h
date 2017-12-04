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

// parsestmt.c
void parseStmtBlock(Nodes **nodes);

// parseexp.c
AstNode *parseExp();

// parsetype.c
AstNode *parseFnSig();
AstNode *parseVtype();
AstNode* parsePerm();

#endif
