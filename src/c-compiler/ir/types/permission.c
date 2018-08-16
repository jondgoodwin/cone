/** Permission Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new permission node
PermNode *newPermNode(Name *namesym, char ptyp, uint16_t flags) {
	PermNode *node;
	newNode(node, PermNode, PermTag);
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

// Serialize a permission node
void permPrint(PermNode *node) {
	switch (node->ptype) {
	case UniPerm: inodeFprint("uni "); break;
	case MutPerm: inodeFprint("mut "); break;
	case ImmPerm: inodeFprint("imm "); break;
	case ConstPerm: inodeFprint("const "); break;
	case MutxPerm: inodeFprint("mutx "); break;
	case IdPerm: inodeFprint("id "); break;
	default: inodeFprint("lock "); break;
	}
}

// Retrieve the permission flags for the node
uint16_t permGetFlags(INode *node) {
	switch (node->tag) {
	case VarNameUseTag:
		return permGetFlags((INode*)((NameUseNode*)node)->dclnode);
	case VarDclTag:
		return ((VarDclNode*)node)->perm->flags;
    case FnDclTag:
        return immPerm->flags;
    case DerefTag:
	{
		PtrNode *vtype = (PtrNode*)typeGetVtype(((DerefNode *)node)->exp);
		assert(vtype->tag == RefTag || vtype->tag == PtrTag);
		return vtype->perm->flags;
	}
	default:
		return 0;
	}
}

// Is Lval mutable
int permIsMutable(INode *lval) {
	if (lval->tag == FnCallTag) {
		FnCallNode *fncall = (FnCallNode *)lval;
        if (fncall->methprop)
            return MayWrite & permGetFlags(fncall->objfn) & permGetFlags((INode*)fncall->methprop);
        else
            return 0;
	}
	else
		return MayWrite & permGetFlags(lval);
}

// Are the permissions the same?
int permIsSame(PermNode *node1, PermNode *node2) {
	return node1 == node2;
}

// Will 'from' permission coerce to the target?
int permMatches(PermNode *to, PermNode *from) {
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