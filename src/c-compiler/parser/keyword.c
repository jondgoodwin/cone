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

// Add a compiler built-in type to the global symbol table as immutable, declared type name
// This gives a program's later NameUse something to point to
void newNameDclNodeStr(char *namestr, AstNode *type) {
	Symbol *sym;
	sym = symFind(namestr, strlen(namestr));
	sym->node = (AstNode*) newNameDclNode(sym, NULL, immPerm, type);
}

void keywordInit() {
	keyAdd("fn", FnToken);
	keyAdd("return", RetToken);

	newNameDclNodeStr("i8", (AstNode*)(i8Type = newNbrTypeNode(IntType, 1)));
	newNameDclNodeStr("i16", (AstNode*)(i16Type = newNbrTypeNode(IntType, 2)));
	newNameDclNodeStr("i32", (AstNode*)(i32Type = newNbrTypeNode(IntType, 4)));
	newNameDclNodeStr("i64", (AstNode*)(i64Type = newNbrTypeNode(IntType, 8)));
	newNameDclNodeStr("u8", (AstNode*)(u8Type = newNbrTypeNode(UintType, 1)));
	newNameDclNodeStr("u16", (AstNode*)(u16Type = newNbrTypeNode(UintType, 2)));
	newNameDclNodeStr("u32", (AstNode*)(u32Type = newNbrTypeNode(UintType, 4)));
	newNameDclNodeStr("u64", (AstNode*)(u64Type = newNbrTypeNode(UintType, 8)));
	newNameDclNodeStr("f32", (AstNode*)(f32Type = newNbrTypeNode(FloatType, 4)));
	newNameDclNodeStr("f64", (AstNode*)(f64Type = newNbrTypeNode(FloatType, 8)));

	newNameDclNodeStr("mut", (AstNode*) (mutPerm = newPermTypeNode(MutPerm, MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("mmut", (AstNode*) (mmutPerm = newPermTypeNode(MmutPerm, MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless, NULL)));
	newNameDclNodeStr("imm", (AstNode*) (immPerm = newPermTypeNode(ImmPerm, MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("const", (AstNode*) (constPerm = newPermTypeNode(ConstPerm, MayRead | MayAlias | IsLockless, NULL)));
	newNameDclNodeStr("mutx", (AstNode*) (mutxPerm = newPermTypeNode(MutxPerm, MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("id", (AstNode*) (idPerm = newPermTypeNode(IdPerm, MayAlias | RaceSafe | IsLockless, NULL)));
}
