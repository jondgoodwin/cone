/** Handling for array reference (slice) type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Serialize an array reference type
void arrayRefPrint(RefNode *node) {
    inodeFprint("&[](");
    inodePrintNode(node->region);
    inodeFprint(" ");
    inodePrintNode((INode*)node->perm);
    inodeFprint(" ");
    inodePrintNode(node->vtexp);
    inodeFprint(")");
}

// Name resolution of an array reference node
void arrayRefNameRes(NameResState *pstate, RefNode *node) {
    inodeNameRes(pstate, &node->region);
    inodeNameRes(pstate, (INode**)&node->perm);
    inodeNameRes(pstate, &node->vtexp);

    // If this is not a reference type, turn it into a borrow/allocate constructor
    if (!isTypeNode(node->vtexp)) {
        node->tag = node->region == (INode*)borrowRef ? ArrayBorrowTag : ArrayAllocTag;
    }
}

// Type check an array reference node
void arrayRefTypeCheck(TypeCheckState *pstate, RefNode *node) {
    if (node->perm == unknownType)
        node->perm = newPermUseNode(node->region == borrowRef ? roPerm : uniPerm);
    itypeTypeCheck(pstate, &node->region);
    itypeTypeCheck(pstate, (INode**)&node->perm);
    if (node->vtexp)
        itypeTypeCheck(pstate, &node->vtexp);

    // Normalize reference type and point to its metadata
    node->typeinfo = typetblFind((INode*)node, refTypeInfoAlloc);
}

// Compare two reference signatures to see if they are equivalent
int arrayRefIsSame(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->vtexp, node2->vtexp)
        && permIsSame(node1->perm, node2->perm)
        && itypeIsSame(node1->region, node2->region);
}

// Calculate hash for a structural reference type
size_t arrayRefHash(RefNode *node) {
    size_t hash = 5381 + node->tag;
    hash = ((hash << 5) + hash) ^ itypeHash(node->region);
    hash = ((hash << 5) + hash) ^ itypeHash(node->perm);
    return ((hash << 5) + hash) ^ itypeHash(node->vtype);
}

// Compare two reference signatures to see if they are equivalent at runtime
int arrayRefIsRunSame(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->vtexp, node2->vtexp)
        && itypeIsRunSame(node1->perm, node2->perm)
        && itypeIsRunSame(node1->region, node2->region);
}

// Will from reference coerce to a to reference (we know they are not the same)
TypeCompare arrayRefMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    return refMatches(to, from, constraint);
}

// Will from reference coerce to a to arrayref (we know they are not the same)
// At best, we get ConvSubtype, because a conversion is needed to convert ref to arrayref
TypeCompare arrayRefMatchesRef(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    // From type must be a reference to 
    ArrayNode *arraytype = (ArrayNode*)from->vtexp;
    if (arraytype->tag != ArrayTag)
        return NoMatch;

    // Start with matching the references' regions
    if (regionMatches(from->region, to->region, constraint) == NoMatch)
        return NoMatch;

    // Now their permissions
    if (permMatches(to->perm, from->perm) == NoMatch)
        return NoMatch;

    // Now we get to value-type (which might include lifetime).
    // The variance of this match depends on the mutability/read permission of the reference
    TypeCompare match;
    switch (permGetFlags(to->perm) & (MayWrite | MayRead)) {
    case 0:
    case MayRead:
        match = itypeMatches(to->vtexp, arrayElemType((INode*)arraytype), constraint); // covariant
        break;
    case MayWrite:
        match = itypeMatches(arrayElemType((INode*)arraytype), to->vtexp, constraint); // contravariant
        break;
    case MayRead | MayWrite:
        return itypeIsSame(to->vtexp, arrayElemType((INode*)arraytype)) ? ConvSubtype : NoMatch; // invariant
    }
    switch (match) {
    case EqMatch:
    case CastSubtype:
        return ConvSubtype;
    case ConvSubtype:
        return constraint == Monomorph ? ConvSubtype : NoMatch;
    default:
        return NoMatch;
    }
}
