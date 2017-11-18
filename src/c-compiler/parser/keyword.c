/** Keyword Initialization
 * @file
 *
 * The lexer divides up the source program into tokens, producing each for the parser on demand.
 * The lexer assumes UTF-8 encoding for the source program.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "keyword.h"
#include "lexer.h"
#include "../shared/ast.h"
#include "../shared/symbol.h"
#include "../shared/memory.h"

#include <string.h>

void keyAdd(char *keyword, uint16_t toktype) {
	Symbol *sym;
	AstNode *node;
	sym = symFind(keyword, strlen(keyword));
	sym->node = node = (AstNode*)memAllocBlk(sizeof(AstNode));
	node->asttype = KeywordNode;
	node->flags = toktype;
}

void keywordInit() {
	keyAdd("fn", FnToken);
}