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
	newNameDclNodeStr("uni", PermNameDclNode, (AstNode*)(uniPerm = newPermNode(UniPerm, MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("mut", PermNameDclNode, (AstNode*)(mutPerm = newPermNode(MutPerm, MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless, NULL)));
	newNameDclNodeStr("imm", PermNameDclNode, (AstNode*)(immPerm = newPermNode(ImmPerm, MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("const", PermNameDclNode, (AstNode*)(constPerm = newPermNode(ConstPerm, MayRead | MayAlias | IsLockless, NULL)));
	newNameDclNodeStr("mutx", PermNameDclNode, (AstNode*)(mutxPerm = newPermNode(MutxPerm, MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless, NULL)));
	newNameDclNodeStr("id", PermNameDclNode, (AstNode*)(idPerm = newPermNode(IdPerm, MayAlias | RaceSafe | IsLockless, NULL)));
}

// Create a new permission node
PermAstNode *newPermNode(char ptyp, uint16_t flags, AstNode *locker) {
	PermAstNode *node;
	newAstNode(node, PermAstNode, PermType);
	node->methods = newNodes(1);	// May not need members for static types
	node->subtypes = newNodes(8);	// build appropriate list using the permission's flags
	node->flags = flags;
	node->ptype = ptyp;
	node->locker = locker;
	return node;
}

// Serialize the AST for a permission
void permPrint(PermAstNode *node) {
	switch (node->ptype) {
	case UniPerm: astFprint("uni "); break;
	case MutPerm: astFprint("mut "); break;
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
	case NameUseNode:
		return ((NameUseAstNode*)node)->dclnode->perm->flags;
	case VarNameDclNode:
		return ((NameDclAstNode*)node)->perm->flags;
	case DerefNode:
	{
		PtrAstNode *vtype = (PtrAstNode*)typeGetVtype(((DerefAstNode *)node)->exp);
		assert(vtype->asttype == RefType || vtype->asttype == PtrType);
		return vtype->perm->flags;
	}
	default:
		return 0;
	}
}

// Is Lval mutable
int permIsMutable(AstNode *lval) {
	if (lval->asttype == ElementNode) {
		ElementAstNode *elem = (ElementAstNode *)lval;
		return MayWrite & permGetFlags(elem->owner) & permGetFlags(elem->element);
	}
	else
		return MayWrite & permGetFlags(lval);
}

// Are the permissions the same?
int permIsSame(PermAstNode *node1, PermAstNode *node2) {
	return node1 == node2;
}

// Will 'from' permission coerce to the target?
int permMatches(PermAstNode *to, PermAstNode *from) {
	if (permIsSame(to, from) || to==idPerm)
		return 1;
	if (from == uniPerm &&
		(to == constPerm || to == mutPerm || to == immPerm || to == mutxPerm))
		return 1;
	if (to == constPerm &&
		(from == mutPerm || from == immPerm || from == mutxPerm))
		return 1;
	return 0;
}