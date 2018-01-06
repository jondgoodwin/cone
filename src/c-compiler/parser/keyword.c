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
#include "../ast/ast.h"
#include "../shared/symbol.h"
#include "../shared/memory.h"

#include <string.h>

Symbol *keyAdd(char *keyword, uint16_t toktype) {
	Symbol *sym;
	AstNode *node;
	sym = symFind(keyword, strlen(keyword));
	sym->node = node = (AstNode*)memAllocBlk(sizeof(AstNode));
	node->asttype = KeywordNode;
	node->flags = toktype;
	return sym;
}

void keywordInit() {
	// Declare built-in types and publish their names to the symbol table
	permDclNames();
	nbrDclNames();

	keyAdd("fn", FnToken);
	keyAdd("return", RetToken);
	keyAdd("if", IfToken);
	keyAdd("elif", ElifToken);
	keyAdd("else", ElseToken);
	keyAdd("while", WhileToken);
	keyAdd("break", BreakToken);
	keyAdd("continue", ContinueToken);
	keyAdd("not", NotToken);
	keyAdd("or", OrToken);
	keyAdd("and", AndToken);

	keyAdd("true", trueToken);
	keyAdd("false", falseToken);
}
