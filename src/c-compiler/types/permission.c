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

#include <assert.h>

// Declare built-in permission types and their names
void permDclNames() {
	newNameDclNodeStr("mut", PermNameDclNode, (AstNode*)(mutPerm = newPermNode(MutPerm, MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("mmut", PermNameDclNode, (AstNode*)(mmutPerm = newPermNode(MmutPerm, MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless, NULL)));
	newNameDclNodeStr("imm", PermNameDclNode, (AstNode*)(immPerm = newPermNode(ImmPerm, MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("const", PermNameDclNode, (AstNode*)(constPerm = newPermNode(ConstPerm, MayRead | MayAlias | IsLockless, NULL)));
	newNameDclNodeStr("mutx", PermNameDclNode, (AstNode*)(mutxPerm = newPermNode(MutxPerm, MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("id", PermNameDclNode, (AstNode*)(idPerm = newPermNode(IdPerm, MayAlias | RaceSafe | IsLockless, NULL)));
}

// Create a new permission node
PermAstNode *newPermNode(char ptyp, uint16_t flags, AstNode *locker) {
	PermAstNode *node;
	newAstNode(node, PermAstNode, PermType);
	node->instnames = newInodes(1);	// May not need members for static types
	node->typenames = newInodes(1); // May not need members for static types
	node->traits = newNodes(8);	// build appropriate list using the permission's flags
	node->flags = flags;
	node->ptype = ptyp;
	node->locker = locker;
	return node;
}

// Serialize the AST for a permission
void permPrint(PermAstNode *node) {
	switch (node->ptype) {
	case MutPerm: astFprint("mut "); break;
	case MmutPerm: astFprint("mmut "); break;
	case ImmPerm: astFprint("imm "); break;
	case ConstPerm: astFprint("const "); break;
	case MutxPerm: astFprint("mutx "); break;
	case IdPerm: astFprint("id "); break;
	default: astFprint("lock "); break;
	}
}

// Retrieve the permission flags for the node
uint16_t permGetFlags(AstNode *node) {
	switch (node->asttype) {
	case VarNameUseNode:
		return ((NameUseAstNode*)node)->dclnode->perm->flags;
	case VarNameDclNode:
		return ((NameDclAstNode*)node)->perm->flags;
	case DerefNode:
	{
		PtrTypeAstNode *vtype = (PtrTypeAstNode*)typeGetVtype(((DerefAstNode *)node)->exp);
		assert(vtype->asttype == PtrType);
		return vtype->perm->flags;
	}
	default:
		return 0;
	}
}