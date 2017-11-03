/** Lexer
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lexer_h
#define lexer_h

#include "../shared/ast.h"

typedef struct Lexer Lexer;

void lexGetNextToken();
uint16_t lexType();
AstNode *lexMatch(uint16_t nodetype);
Lexer *lexNew(char *url);

#endif
