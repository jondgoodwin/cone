/** Permission Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"

#include <assert.h>

// Create a new permission node
PermAstNode *newPermNode(Name *namesym, char ptyp, uint16_t flags) {
	PermAstNode *node;
	newAstNode(node, PermAstNode, PermType);
    node->vtype = NULL;
    node->owner = NULL;
    node->namesym = namesym;
    node->llvmtype = NULL;
    methnodesInit(&node->methprops, 1); // May not need members for static types
	node->subtypes = newNodes(8);	// build appropriate list using the permission's flags
	node->flags = flags;
	node->ptype = ptyp;
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
	case VarNameUseTag:
		return permGetFlags((AstNode*)((NameUseAstNode*)node)->dclnode);
	case VarDclTag:
		return ((VarDclAstNode*)node)->perm->flags;
    case FnDclTag:
        return immPerm->flags;
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
	if (lval->asttype == FnCallNode) {
		FnCallAstNode *fncall = (FnCallAstNode *)lval;
        if (fncall->methprop)
            return MayWrite & permGetFlags(fncall->objfn) & permGetFlags((AstNode*)fncall->methprop);
        else
            return 0;
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