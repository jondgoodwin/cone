/** Permission Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Declare built-in permission types and their names
void permDclNames() {
	newNameDclNodeStr("mut", PermNameDclNode, (AstNode*)(mutPerm = newPermNode(MutPerm, MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("mmut", PermNameDclNode, (AstNode*)(mmutPerm = newPermNode(MmutPerm, MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless, NULL)));
	newNameDclNodeStr("imm", PermNameDclNode, (AstNode*)(immPerm = newPermNode(ImmPerm, MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("const", PermNameDclNode, (AstNode*)(constPerm = newPermNode(ConstPerm, MayRead | MayAlias | IsLockless, NULL)));
	newNameDclNodeStr("mutx", PermNameDclNode, (AstNode*)(mutxPerm = newPermNode(MutxPerm, MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("id", PermNameDclNode, (AstNode*)(idPerm = newPermNode(IdPerm, MayAlias | RaceSafe | IsLockless, NULL)));
}

// Create a new primitive number type node
PermAstNode *newPermNode(char ptyp, uint16_t flags, AstNode *locker) {
	PermAstNode *node;
	newAstNode(node, PermAstNode, PermType);
	node->flags = flags;
	node->ptype = ptyp;
	node->locker = locker;
	return node;
}

// Serialize the AST for a Unsigned literal
void permPrint(int indent, PermAstNode *node, char* prefix) {
	switch (node->ptype) {
	case MutPerm: astPrintLn(indent, "%s %s", prefix, "mut"); break;
	case MmutPerm: astPrintLn(indent, "%s %s", prefix, "mmut"); break;
	case ImmPerm: astPrintLn(indent, "%s %s", prefix, "imm"); break;
	case ConstPerm: astPrintLn(indent, "%s %s", prefix, "const"); break;
	case MutxPerm: astPrintLn(indent, "%s %s", prefix, "mutx"); break;
	case IdPerm: astPrintLn(indent, "%s %s", prefix, "id"); break;
	default: astPrintLn(indent, "%s %s", prefix, "lock"); break;
	}
}

// Retrieve the permission flags for the node
uint16_t permGetFlags(AstNode *node) {
	switch (node->asttype) {
	case VarNameUseNode:
	{
		return ((NameUseAstNode*)node)->dclnode->perm->flags;
	}
	default:
		return 0;
	}
}