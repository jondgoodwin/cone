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
    if (refnode->perm == NULL || refnode->pvtype == unknownType)
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

// Will from region coerce to a to region
TypeCompare regionMatches(INode *to, INode *from, SubtypeConstraint constraint) {
    if (to == from)
        return EqMatch;
    if (to == borrowRef)
        return CastSubtype;
    return NoMatch;
}

// Will from-reference coerce to a to-reference (we know they are not the same)
TypeCompare refMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint) {

    // **Remove** A nullable reference may not coerce to a non-nullable reference
    if ((from->flags & FlagRefNull) && !(to->flags & FlagRefNull))
        return NoMatch;

    // Start with matching the references' regions
    TypeCompare result = regionMatches(from->region, to->region, constraint);
    if (result == NoMatch)
        return NoMatch;

    // Now their permissions
    switch (permMatches(to->perm, from->perm)) {
    case NoMatch: return NoMatch;
    case CastSubtype: result = CastSubtype;
    default: break;
    }

    // Now we get to value-type (which might include lifetime).
    // The variance of this match depends on the mutability/read permission of the reference
    TypeCompare match;
    switch (permGetFlags(to->perm) & (MayWrite | MayRead)) {
    case 0:
    case MayRead:
        match = itypeMatches(to->pvtype, from->pvtype, Regref); // covariant
        break;
    case MayWrite:
        match = itypeMatches(from->pvtype, to->pvtype, Regref); // contravariant
        break;
    case MayRead | MayWrite:
        return itypeIsSame(to->pvtype, from->pvtype) ? result : NoMatch; // invariant
    }
    switch (match) {
    case EqMatch:
        return result;
    case CastSubtype:
        return CastSubtype;
    case ConvSubtype:
        return constraint == Monomorph ? ConvSubtype : NoMatch;
    default:
        return NoMatch;
    }
}

// Will from reference coerce to a virtual reference (we know they are not the same)
TypeCompare refvirtMatchesRef(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    // Given this performs a runtime conversion to a completely different type, 
    // it does not make sense for monomorphization
    if (constraint == Monomorph)
        return NoMatch;

    // Start with matching the references' regions
    TypeCompare result = regionMatches(from->region, to->region, constraint);
    if (result == NoMatch)
        return NoMatch;

    // Now their permissions
    switch (permMatches(to->perm, from->perm)) {
    case NoMatch: return NoMatch;
    case CastSubtype: result = CastSubtype;
    default: break;
    }

    // Handle value type without worrying about mutability-triggered variance
    // This is because a virtual reference "supertype" can never change the value's underlying type
    // Virtual references are based on structs/traits
    StructNode *tovtypedcl = (StructNode*)itypeGetTypeDcl(to->pvtype);
    StructNode *fromvtypedcl = (StructNode*)itypeGetTypeDcl(from->pvtype);
    if (tovtypedcl->tag != StructTag || fromvtypedcl->tag != StructTag)
        return NoMatch;

    // When value types are equivalent, ensure it is a closed, tagged trait.
    // The tag is needed to runtime select the vtable for the created virtual reference
    if (tovtypedcl == fromvtypedcl)
        return (fromvtypedcl->flags & HasTagField) ? ConvSubtype : NoMatch;

    // Use special structural subtyping logic to not only check compatibility,
    // but also to build vtable information
    switch (structVirtRefMatches((StructNode*)tovtypedcl, (StructNode*)fromvtypedcl)) {
    case EqMatch:
    case CastSubtype:
    case ConvSubtype:
        return ConvSubtype;  // Creating a virtual reference is always a conversion
    default:
        return NoMatch;
    }
}

// Will from virtual reference coerce to a virtual reference (we know they are not the same)
TypeCompare refvirtMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    // For now, there is no supported way to convert a virtual ref from one value type to another
    // This could change later, maybe for monomorphization or maybe for runtime coercion
    if (!itypeIsSame(to->pvtype, from->pvtype))
        return NoMatch;

    // However, region, permission and lifetime can be supertyped
    // Note: mutability variance on value type should be invariant, since underlying subtype won't change
    return refMatches(to, from, constraint);
}

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *refFindSuper(INode *type1, INode *type2) {
    RefNode *typ1 = (RefNode *)itypeGetTypeDcl(type1);
    RefNode *typ2 = (RefNode *)itypeGetTypeDcl(type2);

    if (itypeGetTypeDcl(typ1->region) != itypeGetTypeDcl(typ2->region)
        || itypeGetTypeDcl(typ1->perm) != itypeGetTypeDcl(typ2->perm))
        return NULL;

    INode *pvtype = structRefFindSuper(typ1->pvtype, typ2->pvtype);
    if (pvtype == NULL)
        return NULL;

    return (INode*)newRefNodeFull(typ1->region, typ1->perm, pvtype);
}
