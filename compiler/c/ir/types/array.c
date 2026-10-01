/** Handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new array type whose info will be filled in afterwards
ArrayNode *newArrayNode() {
    ArrayNode *anode;
    newNode(anode, ArrayNode, ArrayTag);
    // The node arrives as a type and may be re-tagged an array literal, whose
    // vtype the type check fills in. Until then it is unknown rather than
    // whatever the arena last held there.
    anode->vtype = unknownType;
    anode->llvmtype = NULL;
    anode->dimens = newNodes(1);
    anode->elems = newNodes(1);
    return anode;
}

// An array is a move type when its element type is. Whether the element
// moves is asked of itypeIsMove rather than read off its flags, since a tuple
// carries no flag of its own and moves when one of its elements does.
static void arrayInfectFlags(ArrayNode *node, INode *elemtype) {
    if (itypeIsMove(elemtype))
        node->flags |= MoveType;
}

// Create a new array type of a specified size and element type
// This only works for a single dimension array type
//
// The type is built already checked and never passes through arrayTypeCheck,
// so the element type must be one type check has settled. It takes the
// element's flags here; without them an array literal's inferred type would
// copy where the same type written out moves.
ArrayNode *newArrayNodeTyped(INode *lexnode, size_t size, INode *elemtype) {
    ArrayNode *anode = newArrayNode();
    inodeLexCopy((INode*)anode, lexnode);
    nodesAdd(&anode->dimens, (INode*)newULitNode(size, (INode*)u64Type));
    nodesAdd(&anode->elems, elemtype);
    arrayInfectFlags(anode, elemtype);
    return anode;
}

// Return the element type of the array type
INode *arrayElemType(INode *array) {
    return nodesGet(((ArrayNode *)array)->elems, 0);
}

// Return the size of the first dimension (assuming 1-dimensional array)
uint64_t arrayDim1(INode *array) {
    ULitNode *dim1 = (ULitNode *)nodesGet(((ArrayNode *)array)->dimens, 0);
    return dim1->uintlit;
}

// The fill literal's spelling, '[n; x]', given a type for x. An array type is
// written 'Array[T, n]', so this is refused; the node stays the array type it
// spells, so nothing downstream reports it again.
static void arrayRefuseFillSpelling(INode *node) {
    errorMsgNode(node, ErrorArrayTypeOld,
        "An array type is written 'Array[T, n]', its element type first, then its size. '[n; x]' is a fill literal, n copies of the value x, and x here is a type.");
}

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node) {
    ArrayNode *newnode = memAllocBlk(sizeof(ArrayNode));
    memcpy(newnode, node, sizeof(ArrayNode));
    newnode->elems = cloneNodes(cstate, node->elems);
    // arrayNameRes decided array type or array literal by asking whether the
    // element is a type, and in a template '[2; T]' asked that of a generic
    // parameter, which is not one -- so the template holds a literal. Cloning
    // stands in for name resolution on an instance, so decide again now that
    // the element is the type argument (see cloneRefNode): a type there is the
    // fill literal's spelling of an array type, refused as arrayNameRes refuses it.
    if (newnode->tag == ArrayLitTag && newnode->elems->used > 0
        && !isTypeNode(nodesGet(node->elems, 0)) && isTypeNode(nodesGet(newnode->elems, 0))) {
        newnode->tag = ArrayTag;
        if (newnode->dimens->used > 0)
            arrayRefuseFillSpelling((INode*)newnode);
    }
    return (INode *)newnode;
}

// Serialize an array type as it is written, 'Array[f32, 3]', a nested array
// with its sizes together, outermost first: 'Array[f32, 2, 3]'. An array
// literal is serialized as it is written, too.
void arrayPrint(ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    if (node->tag == ArrayTag && node->dimens->used == 1 && node->elems->used == 1) {
        INode *elem = (INode*)node;
        while (elem->tag == ArrayTag && ((ArrayNode*)elem)->dimens->used == 1 && ((ArrayNode*)elem)->elems->used == 1)
            elem = arrayElemType(elem);
        inodeFprint("Array[");
        inodePrintNode(elem);
        for (INode *dim = (INode*)node; dim != elem; dim = arrayElemType(dim)) {
            inodeFprint(", ");
            inodePrintNode(nodesGet(((ArrayNode*)dim)->dimens, 0));
        }
        inodeFprint("]");
        return;
    }
    inodeFprint("[");
    if (node->dimens->used > 0) {
        for (nodesFor(node->dimens, cnt, nodesp)) {
            inodePrintNode(*nodesp);
            if (cnt > 1)
                inodeFprint(", ");
        }
        inodeFprint("; ");
    }
    for (nodesFor(node->elems, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint("]");
}

// Name resolution of an array literal. The parser makes this node for every
// bracketed list; its first element being a type is what would make it the
// fill literal's spelling of an array type, '[3; i32]', which is refused.
void arrayNameRes(NameResState *pstate, ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
    if (node->elems->used > 0 && !isTypeNode(nodesGet(node->elems, 0)))
        node->tag = ArrayLitTag; // We have an array literal, not array type
    else if (node->dimens->used > 0)
        arrayRefuseFillSpelling((INode*)node);
    for (nodesFor(node->dimens, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Lower the array type 'Array[T, n]' (or 'Array[T, n, m, ...]') into the array
// type node it names. 'Array' is a name every module reaches unless it
// declares the name itself (arrayTypeDcl, stdlibInit); it looks like a generic
// type with number parameters, but nothing is instantiated: the call becomes
// the ArrayNode here, so from name resolution on nothing sees how it was
// written. Several sizes are row-major, the first the outermost: the node is
// built nested, 'Array[f32, 2, 3]' as 'Array[Array[f32, 3], 2]', so the layout,
// indexing ('a[i][j]') and type identity are the nested spelling's by
// construction. Each size is checked by arrayTypeCheck as any array's is.
// An element type that is a generic's parameter is a type once substituted,
// and the clone substitutes it in place.
void arrayTypeLower(NameResState *pstate, INode **nodep) {
    FnCallNode *node = (FnCallNode*)*nodep;
    Nodes *args = node->args;
    // Each node built, and each diagnostic, is placed where 'Array' is written
    INode *where = node->objfn;
    if ((node->flags & FlagRange) || args == NULL || args->used < 2) {
        errorMsgNode(where, ErrorArrayTypeArgs,
            "An array type names its element type, then its size: 'Array[T, n]', or 'Array[T, n, m]' with one size for each dimension, the first the outermost.");
        *((INode**)nodep) = newErrorNode(where);
        return;
    }
    INode *elemtype = nodesGet(args, 0);
    if (!isTypeNode(elemtype) && !inodeIsProvisionalType(elemtype)) {
        errorMsgNode(elemtype, ErrorArrayTypeElem,
            "An array type's first argument is its element type, and the sizes follow it: 'Array[T, n]'.");
        *((INode**)nodep) = newErrorNode(where);
        return;
    }
    INode *type = elemtype;
    for (uint32_t i = args->used - 1; i >= 1; --i) {
        ArrayNode *array = newArrayNode();
        inodeLexCopy((INode*)array, where);
        nodesAdd(&array->dimens, nodesGet(args, i));
        nodesAdd(&array->elems, type);
        type = (INode*)array;
    }
    *((INode**)nodep) = type;
}

// Type check an array type
void arrayTypeCheck(TypeCheckState *pstate, ArrayNode *node) {

    // Check out dimensions: must be literal numbers. Generation builds the
    // array with a 32-bit count, so a size is 0 through 4294967295: a negative
    // one (its two's complement read as a count) and a larger one (cut to its
    // low 32 bits) would each give a different array than the one written
    if (node->dimens->used == 1) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(node->dimens, cnt, nodesp)) {
            if ((*nodesp)->tag != ULitTag)
                errorMsgNode(*nodesp, ErrorBadArray, "An array type's size must be an integer literal");
            else if (((*nodesp)->flags & FlagLitNeg) && ((ULitNode*)*nodesp)->uintlit != 0)
                errorMsgNode(*nodesp, ErrorBadArray, "An array type's size may not be negative");
            else if (((ULitNode*)*nodesp)->uintlit > UINT32_MAX)
                errorMsgNode(*nodesp, ErrorBadArray, "An array type's size may be at most 4294967295");
        }
    }
    else
        errorMsgNode((INode*)node, ErrorBadArray, "Array type must have exactly one numeric array dimension");

    // Check out element type
    if (node->elems->used != 1) {
        errorMsgNode((INode*)node, ErrorBadArray, "Exactly one element type must be specified");
    }
    INode **elemtypep = &nodesGet(node->elems, 0);
    if (itypeTypeCheck(pstate, elemtypep) == 0) {
        errorMsgNode((INode*)node, ErrorBadArray, "Element type must be a known type");
        return;
    }
    // An array of a type that cannot say how large it is has no size either, so
    // it is not a type at all -- not even behind a reference, which is the one
    // place an opaque type is otherwise usable. This is the only site that
    // refuses that, since a reference to such an array asks nothing of it.
    INode *elemroot;
    char *elemnosize = itypeNoSizeCause(*elemtypep, &elemroot);
    if (elemnosize) {
        errorMsgNode((INode*)node, ErrorNoSize, "An array cannot be made of this element type: %s %s.",
            itypeName(elemroot), elemnosize);
        itypeNoSizeExplain(*elemtypep);
    }
    arrayInfectFlags(node, *elemtypep);
}

// Compare two array types to see if they are equivalent
int arrayEqual(ArrayNode *node1, ArrayNode *node2) {
    // Are element type and number of dimensions equivalent?
    if (!itypeIsSame(arrayElemType((INode*)node1), arrayElemType((INode*)node2))
        || node1->dimens->used != node2->dimens->used)
        return 0;

    // Now compare all the dimensions
    INode **nodes1p;
    uint32_t cnt;
    INode **nodes2p = &nodesGet(node2->dimens, 0);
    for (nodesFor(node1->dimens, cnt, nodes1p)) {
        ULitNode *dim1 = (ULitNode *)*nodes1p;
        ULitNode *dim2 = (ULitNode *)*nodes2p++;
        if (dim1->uintlit != dim2->uintlit)
            return 0;
    }
    return 1;
}

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare arrayMatches(ArrayNode *to, ArrayNode *from, SubtypeConstraint constraint) {
    // Must have same dimensions
    if (to->dimens->used != from->dimens->used)
        return NoMatch;
    INode **nodes1p;
    uint32_t cnt;
    INode **nodes2p = &nodesGet(to->dimens, 0);
    for (nodesFor(from->dimens, cnt, nodes1p)) {
        ULitNode *dim1 = (ULitNode *)*nodes1p;
        ULitNode *dim2 = (ULitNode *)*nodes2p++;
        if (dim1->uintlit != dim2->uintlit)
            return 0;
    }

    // We can subtype on element type sometimes
    TypeCompare result = itypeMatches(arrayElemType((INode*)to), arrayElemType((INode*)from), constraint);
    switch (result) {
    case ConvSubtype:
        return (constraint == Monomorph) ? result : NoMatch;
    default:
        return result;
    }
}
