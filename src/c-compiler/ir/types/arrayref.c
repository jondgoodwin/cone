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
    if (node->vtexp)
        inodeNameRes(pstate, &node->vtexp);
}

// Type check an array reference node
void arrayRefTypeCheck(TypeCheckState *pstate, RefNode *node) {
    if (node->perm == unknownType)
        node->perm = newPermUseNode(node->region == borrowRef ? constPerm : uniPerm);
    itypeTypeCheck(pstate, &node->region);
    itypeTypeCheck(pstate, (INode**)&node->perm);
    if (node->vtexp)
        itypeTypeCheck(pstate, &node->vtexp);
}

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(RefNode *node1, RefNode *node2) {
    return itypeIsSame(node1->vtexp,node2->vtexp) 
        && permIsSame(node1->perm, node2->perm)
        && node1->region == node2->region;
}

// Will from reference coerce to a to reference (we know they are not the same)
TypeCompare arrayRefMatches(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    return refMatches(to, from, constraint);
}

// Will from reference coerce to a to arrayref (we know they are not the same)
TypeCompare arrayRefMatchesRef(RefNode *to, RefNode *from, SubtypeConstraint constraint) {
    // From type must be a reference to 
    ArrayNode *arraytype = (ArrayNode*)from->vtexp;
    if (arraytype->tag != ArrayTag)
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

    if (result == CastSubtype)
        result = ConvSubtype;   // ref to arrayref is a conversion

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
        return itypeIsSame(to->vtexp, arrayElemType((INode*)arraytype)) ? result : NoMatch; // invariant
    }
    switch (match) {
    case EqMatch:
        return result;
    case CastSubtype:
        return ConvSubtype;
    case ConvSubtype:
        return constraint == Monomorph ? ConvSubtype : NoMatch;
    default:
        return NoMatch;
    }
}
