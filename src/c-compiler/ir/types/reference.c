/** Handling for references
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <memory.h>

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode() {
    RefNode *refnode;
    newNode(refnode, RefNode, RefTag);
    return refnode;
}

// Clone reference
INode *cloneRefNode(CloneState *cstate, RefNode *node) {
    RefNode *newnode = memAllocBlk(sizeof(RefNode));
    memcpy(newnode, node, sizeof(RefNode));
    newnode->region = cloneNode(cstate, node->region);
    newnode->perm = cloneNode(cstate, node->perm);
    newnode->pvtype = cloneNode(cstate, node->pvtype);
    return (INode *)newnode;
}

// Set type infection flags based on the reference's type parameters
void refAdoptInfections(RefNode *refnode) {
    if (refnode->perm == NULL || refnode->pvtype == NULL)
        return;  // Wait until we have this info
    if (!(permGetFlags(refnode->perm) & MayAlias) || refnode->region == (INode*)soRegion)
        refnode->flags |= MoveType;
    if (refnode->perm == (INode*)mutPerm || refnode->perm == (INode*)constPerm 
        || (refnode->pvtype->flags & ThreadBound))
        refnode->flags |= ThreadBound;
}

// Create a reference node based on fully-known type parameters
RefNode *newRefNodeFull(INode *region, INode *perm, INode *vtype) {
    RefNode *refnode = newRefNode();
    refnode->region = region;
    refnode->perm = perm;
    refnode->pvtype = vtype;
    refAdoptInfections(refnode);
    return refnode;
}

// Set the inferred value type of a reference
void refSetPermVtype(RefNode *refnode, INode *perm, INode *vtype) {
    refnode->perm = perm;
    refnode->pvtype = vtype;
    refAdoptInfections(refnode);
}

// Create a new ArrayDerefNode from an ArrayRefNode
RefNode *newArrayDerefNodeFrom(RefNode *refnode) {
    RefNode *dereftype = newRefNode();
    dereftype->tag = ArrayDerefTag;
    dereftype->region = refnode->region;
    dereftype->perm = refnode->perm;
    dereftype->pvtype = refnode->pvtype;
    return dereftype;
}

// Is type a nullable reference?
int refIsNullable(INode *typenode) {
    RefNode *ref = (RefNode*)typenode;
    return ref->tag == RefTag && (ref->flags & FlagRefNull);
}

// Serialize a pointer type
void refPrint(RefNode *node) {
    inodeFprint("&(");
    inodePrintNode(node->region);
    inodeFprint(" ");
    inodePrintNode((INode*)node->perm);
    inodeFprint(" ");
    inodePrintNode(node->pvtype);
    inodeFprint(")");
}

// Name resolution of a reference node
void refNameRes(NameResState *pstate, RefNode *node) {
    inodeNameRes(pstate, &node->region);
    inodeNameRes(pstate, (INode**)&node->perm);
    inodeNameRes(pstate, &node->pvtype);
}

// Type check a reference node
void refTypeCheck(TypeCheckState *pstate, RefNode *node) {
    itypeTypeCheck(pstate, &node->region);
    itypeTypeCheck(pstate, (INode**)&node->perm);
    if (itypeTypeCheck(pstate, &node->pvtype) == 0)
        return;
    refAdoptInfections(node);
}

// Type check a virtual reference node
void refvirtTypeCheck(TypeCheckState *pstate, RefNode *node) {
    itypeTypeCheck(pstate, &node->region);
    itypeTypeCheck(pstate, (INode**)&node->perm);
    if (itypeTypeCheck(pstate, &node->pvtype) == 0)
        return;
    refAdoptInfections(node);

    StructNode *trait = (StructNode*)itypeGetTypeDcl(node->pvtype);
    if (trait->tag != StructTag) {
        errorMsgNode((INode*)node, ErrorInvType, "A virtual reference must be to a struct or trait.");
        return;
    }

    // Build the Vtable info
    structMakeVtable(trait);
}

// Compare two reference signatures to see if they are equivalent
int refEqual(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->pvtype,node2->pvtype) 
        && permIsSame(node1->perm, node2->perm)
        && node1->region == node2->region
        && (node1->flags & FlagRefNull) == (node2->flags & FlagRefNull);
}

// Will from reference coerce to a to reference (we know they are not the same)
int refMatches(RefNode *to, RefNode *from) {
    if (NoMatch == permMatches(to->perm, from->perm)
        || (to->region != from->region && to->region != borrowRef)
        || ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull)))
        return NoMatch;

    // Match on ref's vtype as well.
    // If vtype is a struct, use a more relaxed subtype match appropriate to refs
    INode *tovtypedcl = itypeGetTypeDcl(to->pvtype);
    INode *fromvtypedcl = itypeGetTypeDcl(from->pvtype);
    if (tovtypedcl->tag == StructTag && fromvtypedcl->tag == StructTag) {
        if (tovtypedcl == fromvtypedcl)
            return EqMatch;
        return structRefMatches((StructNode*)tovtypedcl, (StructNode*)fromvtypedcl);
    }

    switch (itypeMatches(tovtypedcl, fromvtypedcl)) {
    case NoMatch:
        return NoMatch;
    case EqMatch:
        return EqMatch;
    case CoerceMatch:
        return CoerceMatch;
    default:
        return NoMatch;
    }
}

// Will from reference coerce to a virtual reference (we know they are not the same)
int refvirtMatches(RefNode *to, RefNode *from) {
    if (NoMatch == permMatches(to->perm, from->perm)
        || (to->region != from->region && to->region != borrowRef)
        || ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull)))
        return NoMatch;

    // Match on ref's vtype as well.
    // If vtype is a struct, use a more relaxed subtype match appropriate to refs
    INode *tovtypedcl = itypeGetTypeDcl(to->pvtype);
    INode *fromvtypedcl = itypeGetTypeDcl(from->pvtype);
    if (tovtypedcl->tag == StructTag && fromvtypedcl->tag == StructTag) {
        if (tovtypedcl == fromvtypedcl)
            return EqMatch;
        return structVirtRefMatches((StructNode*)tovtypedcl, (StructNode*)fromvtypedcl);
    }

    switch (itypeMatches(tovtypedcl, fromvtypedcl)) {
    case NoMatch:
        return NoMatch;
    case EqMatch:
        return EqMatch;
    case CoerceMatch:
        return CoerceMatch;
    default:
        return NoMatch;
    }
}
