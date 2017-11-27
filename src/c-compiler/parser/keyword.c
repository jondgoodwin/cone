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
	keyAdd("fn", FnToken);

	i8Type = (AstNode*) newNbrTypeNode(IntType, 1, keyAdd("i8", i8Token));
	i16Type = (AstNode*) newNbrTypeNode(IntType, 2, keyAdd("i16", i16Token));
	i32Type = (AstNode*) newNbrTypeNode(IntType, 4, keyAdd("i32", i32Token));
	i64Type = (AstNode*) newNbrTypeNode(IntType, 8, keyAdd("i64", i64Token));
	u8Type = (AstNode*) newNbrTypeNode(UintType, 1, keyAdd("u8", u8Token));
	u16Type = (AstNode*) newNbrTypeNode(UintType, 2, keyAdd("u16", u16Token));
	u32Type = (AstNode*) newNbrTypeNode(UintType, 4, keyAdd("u32", u32Token));
	u64Type = (AstNode*) newNbrTypeNode(UintType, 8, keyAdd("u64", u64Token));
	i8Type = (AstNode*) newNbrTypeNode(FloatType, 4, keyAdd("f32", f32Token));
	i8Type = (AstNode*) newNbrTypeNode(FloatType, 8, keyAdd("f64", f64Token));
}