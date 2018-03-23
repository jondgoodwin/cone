/** Standard library initialization
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../ast/nametbl.h"
#include "../parser/lexer.h"

#include <string.h>

Name *keyAdd(char *keyword, uint16_t toktype) {
	Name *sym;
	NamedAstNode *node;
	sym = nameFind(keyword, strlen(keyword));
	sym->node = node = (NamedAstNode*)memAllocBlk(sizeof(AstNode));
	node->asttype = KeywordNode;
	node->flags = toktype;
	return sym;
}

void keywordInit() {
	keyAdd("include", IncludeToken);
	keyAdd("mod", ModToken);
	keyAdd("extern", ExternToken);
	keyAdd("fn", FnToken);
	keyAdd("struct", StructToken);
	keyAdd("alloc", AllocToken);
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
	keyAdd("as", AsToken);

	keyAdd("true", trueToken);
	keyAdd("false", falseToken);
}

// Declare built-in permission types and their names
void stdPermInit() {
	newNameDclNodeStr("uni", PermNameDclNode, (AstNode*)(uniPerm = newPermNode(UniPerm, MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("mut", PermNameDclNode, (AstNode*)(mutPerm = newPermNode(MutPerm, MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless, NULL)));
	newNameDclNodeStr("imm", PermNameDclNode, (AstNode*)(immPerm = newPermNode(ImmPerm, MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("const", PermNameDclNode, (AstNode*)(constPerm = newPermNode(ConstPerm, MayRead | MayAlias | IsLockless, NULL)));
	newNameDclNodeStr("mutx", PermNameDclNode, (AstNode*)(mutxPerm = newPermNode(MutxPerm, MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("id", PermNameDclNode, (AstNode*)(idPerm = newPermNode(IdPerm, MayAlias | RaceSafe | IsLockless, NULL)));
}

// Set up the standard library, whose names are always shared by all modules
void stdlibInit() {
	lexInject("std", "");

	keywordInit();
	stdPermInit();
	stdNbrInit();

	voidType = (AstNode*)newVoidNode();
}