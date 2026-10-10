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
    anode->repeats = NULL;
    anode->nsizes = 0;
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

// The old array type's spelling, '[n; T]': '[n; x]' given a type for x. An
// array type is written 'Array[T, n]', so this is refused; the node stays the
// array type it spells, so nothing downstream reports it again.
static void arrayRefuseFillSpelling(INode *node) {
    errorMsgNode(node, ErrorArrayTypeOld,
        "An array type is written 'Array[T, n]', its element type first, then its size, not '[n; T]'.");
}

// The fill literal, '[n; x]' with x a value, is retired: an array of n copies
// is constructed with its contents, as any collection's are
static void arrayRefuseFillLiteral(INode *node) {
    errorMsgNode(node, ErrorFillLiteral,
        "'[n; x]' is no longer written: an array of n values is constructed with its contents, 'new Array[T, n] <- fill x' (or '<- n of x').");
}

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node) {
    ArrayNode *newnode = memAllocBlk(sizeof(ArrayNode));
    memcpy(newnode, node, sizeof(ArrayNode));
    newnode->elems = cloneNodes(cstate, node->elems);
    // A size is a use of a value parameter in a generic's template, which the
    // instance holds as the number it was given
    if (node->dimens)
        newnode->dimens = cloneNodes(cstate, node->dimens);
    // arrayNameRes decided array type or array literal by asking whether the
    // element is a type, and in a template '[2; T]' asked that of a generic
    // parameter, which is not one -- so the template holds a literal. Cloning
    // stands in for name resolution on an instance, so decide again now that
    // the element is the type argument (see cloneRefNode): a type there is the
    // old spelling of an array type, refused as arrayNameRes refuses it.
    if (newnode->tag == ArrayLitTag && newnode->elems->used > 0
        && !isTypeNode(nodesGet(node->elems, 0)) && isTypeNode(nodesGet(newnode->elems, 0))) {
        newnode->tag = ArrayTag;
        if (newnode->dimens->used > 0)
            arrayRefuseFillSpelling((INode*)newnode);
    }
    // A value there is the retired fill literal, refused as arrayNameRes refuses it
    else if (newnode->tag == ArrayLitTag && newnode->dimens->used > 0 && newnode->elems->used > 0
        && inodeIsProvisionalType(nodesGet(node->elems, 0)))
        arrayRefuseFillLiteral((INode*)newnode);
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
        // An element an array's contents repeat in a loop, as they spell it
        uint32_t repeat = node->repeats ? node->repeats[node->elems->used - cnt] : 1;
        if (repeat > 1)
            inodeFprint("%u of ", (unsigned)repeat);
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint("]");
}

// Name resolution of an array literal. The parser makes this node for every
// bracketed list. Written '[n; x]' it is refused either way: with a type for x
// it is the old array type, '[3; i32]', and with a value the retired fill
// literal. A generic's parameter is neither until substituted, so that one is
// decided by the clone.
void arrayNameRes(NameResState *pstate, ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->elems, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
    if (node->elems->used > 0 && !isTypeNode(nodesGet(node->elems, 0))) {
        node->tag = ArrayLitTag; // We have an array literal, not array type
        if (node->dimens->used > 0 && !inodeIsProvisionalType(nodesGet(node->elems, 0)))
            arrayRefuseFillLiteral((INode*)node);
    }
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
// The outermost node records how many sizes were written (nsizes), since a
// construction's contents differ by spelling: the scalars, row-major, for
// several sizes, and the rows for the nested spelling (contentsArrayLit).
// Nothing else reads it, so the two spellings stay one type.
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
    // A value parameter is a size as itself, 'Array[T, N]'. Arithmetic over one,
    // 'N + 1', would need two such sizes compared and solved, which is not built.
    for (uint32_t i = 1; i < args->used; ++i) {
        INode *size = nodesGet(args, i);
        if (size->tag != ULitTag && !isNameUseNode(size) && genericMentionsValueParm(size)) {
            errorMsgNode(size, ErrorGenValueArith,
                "An array's size is a number or a value parameter alone, 'Array[T, N]'. Arithmetic over a value parameter in a type, like this one, is not supported.");
            *((INode**)nodep) = newErrorNode(where);
            return;
        }
    }
    INode *type = elemtype;
    for (uint32_t i = args->used - 1; i >= 1; --i) {
        ArrayNode *array = newArrayNode();
        inodeLexCopy((INode*)array, where);
        nodesAdd(&array->dimens, nodesGet(args, i));
        nodesAdd(&array->elems, type);
        type = (INode*)array;
    }
    if (args->used > 2)
        ((ArrayNode*)type)->nsizes = args->used - 1;
    *((INode**)nodep) = type;
}

// A type a generic's parameter is substituted with, as the instance sees it:
// an array type without the shape it was written in. Instances are shared
// between type arguments that are the same type, so 'Array[i32, 2, 3]' and
// 'Array[Array[i32, 3], 2]' reach one instance, and a shape kept there would
// be whichever argument instantiated it first. Without it, an array's
// contents through a parameter are its rows, as the nested spelling's are.
// Answers the type itself where there is no shape to remove.
INode *arrayTypeUnshaped(CloneState *cstate, INode *type) {
    if (type->tag == ArrayTag && ((ArrayNode*)type)->nsizes > 1) {
        ((ArrayNode*)type)->nsizes = 0;     // the substitution's own copy
        return type;
    }
    if (!isNameUseNode(type) || ((NameUseNode*)type)->dclnode == NULL || !isTypeNode(type))
        return type;
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != ArrayTag || ((ArrayNode*)dcl)->nsizes <= 1)
        return type;
    // An alias of a shaped array: the instance gets the array it names
    ArrayNode *unshaped = (ArrayNode*)cloneArrayNode(cstate, (ArrayNode*)dcl);
    inodeLexCopy((INode*)unshaped, type);
    unshaped->nsizes = 0;
    return (INode*)unshaped;
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
    // A reference's target does not wait on its layout, and neither does an
    // array that is one: its element's size and flags are read once the
    // element is laid out, or when a by-value use needs them first
    if (structTargetDeferring()) {
        structArrayWait(pstate, (INode*)node);
        return;
    }
    arrayTypeFinish(node);
}

void arrayTypeFinish(ArrayNode *node) {
    INode **elemtypep = &nodesGet(node->elems, 0);
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
