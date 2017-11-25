/** Parser
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef parser_h
#define parser_h

#include "../shared/ast.h"
#include "../types/type.h"

// parser.c
AstNode *parse();
void parseSemi();
void parseRCurly();

// parsestmt.c
void parseStmtBlock(Nodes **nodes);

// parseexp.c
AstNode *parseExp();

// parsetype.c
void parseFnType(TypeAndName *typnam);
AstNode *parseType();
QuadTypeAstNode *parseQuadType();

#endif
