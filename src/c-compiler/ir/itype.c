/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

// Return 1 if nominally (or structurally) identical, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
int itypeIsSame(INode *node1, INode *node2) {

    if (node1->tag == TypeNameUseTag)
        node1 = (INode*)((NameUseNode *)node1)->dclnode;
    if (node2->tag == TypeNameUseTag)
        node2 = (INode*)((NameUseNode *)node2)->dclnode;

    // If they are the same type name, types match
	if (node1 == node2)
		return 1;
	if (node1->tag != node2->tag)
		return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
	switch (node1->tag) {
	case FnSigTag:
		return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
	case RefTag: case PtrTag:
		return ptrTypeEqual((PtrNode*)node1, (PtrNode*)node2);
	default:
		return 0;
	}
}

// Is totype equivalent or a non-changing subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int itypeMatches(INode *totype, INode *fromtype) {
	// Convert, if needed, from names to the type declaration
	if (totype->tag == TypeNameUseTag)
		totype = (INode*)((NameUseNode *)totype)->dclnode;
	if (fromtype->tag == TypeNameUseTag)
		fromtype = (INode*)((NameUseNode *)fromtype)->dclnode;

	// If they are the same value type info, types match
	if (totype == fromtype)
		return 1;

	// Type-specific matching logic
	switch (totype->tag) {
	case RefTag: case PtrTag:
		if (fromtype->tag != RefTag && fromtype->tag != PtrTag)
			return 0;
		return ptrTypeMatches((PtrNode*)totype, (PtrNode*)fromtype);

	case ArrayTag:
		if (totype->tag != fromtype->tag)
			return 0;
		return arrayEqual((ArrayNode*)totype, (ArrayNode*)fromtype);

	//case FnSigTag:
	//	return fnSigMatches((FnSigNode*)totype, (FnSigNode*)fromtype);

	case UintNbrTag:
	case IntNbrTag:
	case FloatNbrTag:
		if (totype->tag != fromtype->tag)
			return isNbr(totype) && isNbr(fromtype) ? 4 : 0;
		return ((NbrNode *)totype)->bits > ((NbrNode *)fromtype)->bits ? 3 : 2;

	default:
		return itypeIsSame(totype, fromtype);
	}
}

// Return a CopyTrait indicating how to handle when a value is assigned to a variable or passed to a function.
int itypeCopyTrait(INode *typenode) {
    if (typenode->tag == TypeNameUseTag)
        typenode = (INode*)((NameUseNode *)typenode)->dclnode;

    // For an aggregate type, existence of a destructor or a non-CopyBitwise property is infectious
    // If it has a .copy method, it is CopyMethod, or else it is CopyMove.
    if (isMethodType(typenode)) {
        int copytrait = CopyBitwise;
        IMethNodes *nodes = &((IMethodNode *)typenode)->methprops;
        uint32_t cnt;
        INode **nodesp;
        for (imethnodesFor(nodes, cnt, nodesp)) {
            if (((*nodesp)->tag == VarDclTag && CopyBitwise != itypeCopyTrait(*nodesp))
                /* || *nodesp points to a destructor */)
                copytrait == CopyBitwise ? CopyMove : copytrait;
            // else if (nodesp points to the .copy method)
            //    return CopyMethod;
        }
        return copytrait;
    }
    // For references, a 'uni' reference is CopyMove and all others are CopyBitwise
    else if (typenode->tag == RefTag) {
        return (permGetFlags(((PtrNode *)typenode)->perm) & MayAlias) ? CopyBitwise : CopyMove;
    }
    // All pointers are CopyMove (potentially unsafe to copy)
    else if (typenode->tag == PtrTag)
        return CopyMove;
    // The default (e.g., numbers) is CopyBitwise
    return CopyBitwise;
}

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype) {
	switch (vtype->tag) {
	case TypeNameUseTag:
	{
		strcpy(bufp, &((NameUseNode *)vtype)->dclnode->namesym->namestr);
		break;
	}
	case RefTag: case PtrTag:
	{
		PtrNode *pvtype = (PtrNode *)vtype;
		*bufp++ = '&';
		if (permIsSame(pvtype->perm, (INode*) constPerm)) {
			bufp = itypeMangle(bufp, pvtype->perm);
			*bufp++ = ' ';
		}
		bufp = itypeMangle(bufp, pvtype->pvtype);
		break;
	}
	default:
		assert(0 && "unknown type for parameter type mangling");
	}
	return bufp + strlen(bufp);
}
