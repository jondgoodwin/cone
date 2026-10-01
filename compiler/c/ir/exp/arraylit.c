/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <limits.h>

// Note:  Creation, serialization and name checking are done with array type logic,
// as we don't yet know whether [] is a type or an array literal

static void arrayLitSettle(ArrayNode *arrlit);

// Type check an array literal's dimension and elements, with no type expected
// of it: arrayLitTypeCheck has already insisted the dimension is a constant.
//
// Every early return here follows a diagnostic, so each one marks the literal
// with errorType before leaving. Without it the literal would carry no type at
// all into the rest of the pass, which reads a value's type without asking
// whether there is one.
static void arrayLitTypeCheckElems(TypeCheckState *pstate, ArrayNode *arrlit) {

    // Handle array literal "fill" format: [dimen, fill-value]
    if (arrlit->dimens->used > 0) {

        // Ensure only one constant integer dimension
        if (arrlit->dimens->used > 1) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal may only specify one dimension");
            arrlit->vtype = errorType;
            return;
        }
        INode **dimnodep = &nodesGet(arrlit->dimens, 0);
        INode *dimnode = *dimnodep;
        if (dimnode->tag == ULitTag)
            ((ULitNode*)dimnode)->vtype = (INode*)usizeType; // Force type
        // Ensure it coerces to usize
        if (iexpTypeCheckCoerce(pstate, (INode*)usizeType, dimnodep) != 1)
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal dimension must coerce to usize");

        // Handle and type the single fill value
        if (arrlit->elems->used != 1 || !isExpNode(nodesGet(arrlit->elems, 0))) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array fill value may only be one value");
            arrlit->vtype = errorType;
            return;
        }
        INode **elemnodep = &nodesGet(arrlit->elems, 0);
        size_t dimsize = 0;
        // A dimension may name a constant, whose value may name another
        while (nameUseNames(dimnode, ConstDclTag))
            dimnode = ((ConstDclNode*)((NameUseNode*)dimnode)->dclnode)->value;
        if (dimnode->tag == ULitTag)
            dimsize = (size_t)((ULitNode*)dimnode)->uintlit;
        if (iexpTypeCheckAny(pstate, elemnodep)) {
            arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit,
                dimsize, ((IExpNode*)*elemnodep)->vtype);
        }
        else
            arrlit->vtype = errorType;   // iexpTypeCheckAny reported it
        return;
    }

    // Otherwise handle multi-value array literal
    if (arrlit->elems->used == 0) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal list may not be empty");
        arrlit->vtype = errorType;
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elems, cnt, nodesp))
        iexpTypeCheckAny(pstate, nodesp);
    arrayLitSettle(arrlit);
}

// Type an array literal wanted as an array type holding as many elements as it
// lists, by coercing each element to that type's element type. That is what a
// struct literal's field value and a variable's initializer get, so a string
// literal borrows as a slice and an untyped number literal adopts the number
// type -- which also lets strings of different lengths share an '&[]u8'
// element, where settling the type among the elements alone finds no type in
// common. When some element does not coerce, the literal settles its type from
// its elements as it does when no type is expected, and the receiver reports
// the mismatch as before. Return 0, having checked nothing, when no such type
// is expected.
static int arrayLitTypeCheckExpected(TypeCheckState *pstate, ArrayNode *arrlit, INode *expectType) {
    if (expectType == NULL || expectType == unknownType || expectType == noCareType
        || arrlit->dimens->used > 0 || arrlit->elems->used == 0)
        return 0;
    INode *totype = itypeGetTypeDcl(expectType);
    if (totype->tag != ArrayTag || ((ArrayNode*)totype)->dimens->used != 1
        || arrayDim1(totype) != arrlit->elems->used)
        return 0;
    INode *elemtype = arrayElemType(totype);
    INode *elemtypedcl = itypeGetTypeDcl(elemtype);

    int allmatch = 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elems, cnt, nodesp)) {
        inodeTypeCheck(pstate, nodesp, elemtype);
        if (!isExpNode(*nodesp)) {
            errorMsgNode(*nodesp, ErrorNotTyped, "Expected a typed expression.");
            allmatch = 0;
        }
        else if (iexpGetTypeDcl(*nodesp) == errorType
            || iexpMatches(nodesp, elemtypedcl, Coercion) == NoMatch)
            allmatch = 0;
    }
    if (!allmatch) {
        arrayLitSettle(arrlit);
        return 1;
    }
    for (nodesFor(arrlit->elems, cnt, nodesp)) {
        if (!iexpCoerce(nodesp, elemtype))
            errorMsgNode(*nodesp, ErrorBadArray, "Array literal value's type does not match the array's element type");
    }
    arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit, arrlit->elems->used, elemtype);
    return 1;
}

// Is an array literal's dimension a constant whose count type check can read? A
// reinterpretation of a constant is a constant (litIsLiteral), but its count is
// known only once generated, and the array type needs it now
static int arrayLitDimIsConst(INode *dimnode) {
    if (!litIsLiteral(dimnode))
        return 0;
    while (nameUseNames(dimnode, ConstDclTag))
        dimnode = ((ConstDclNode*)((NameUseNode*)dimnode)->dclnode)->value;
    return dimnode->tag != CastTag;
}

// Type check an array literal. Its dimension is part of its type, so it must
// be a constant unsigned integer: a count chosen at run time belongs to a List.
void arrayLitTypeCheck(TypeCheckState *pstate, ArrayNode *arrlit, INode *expectType) {
    if (arrlit->dimens->used > 0 && !arrayLitDimIsConst(nodesGet(arrlit->dimens, 0))) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal dimension value must be a constant: an integer literal, or a named constant holding one");
    }
    if (arrayLitTypeCheckExpected(pstate, arrlit, expectType))
        return;
    arrayLitTypeCheckElems(pstate, arrlit);
}

// Settle a list literal's element type from its elements, already type checked:
// every element must agree, meeting at a common supertype where they differ.
// That is the same question 'if' asks across its branches, and itypeFindSuper
// is what answers it there too. Requiring each element to match the first one
// exactly made an array literal the one construct that refused a variant where
// its union was wanted -- a variable initializer, a struct literal's field and
// an 'if' all accept that.
static void arrayLitSettle(ArrayNode *arrlit) {
    INode *matchtype = unknownType;
    int metatsuper = 0;   // some element met at a supertype, so all must coerce
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elems, cnt, nodesp)) {
        // An element that is not an expression was reported by its check
        if (!isExpNode(*nodesp))
            continue;
        // An element already reported as bad contributes no type of its own,
        // and must not be compared against the ones that are still good
        if (iexpGetTypeDcl(*nodesp) == errorType)
            continue;
        if (matchtype == unknownType) {
            matchtype = ((IExpNode*)*nodesp)->vtype;
            continue;
        }
        if (itypeIsSame(((IExpNode*)*nodesp)->vtype, matchtype))
            continue;
        INode *super = itypeFindSuper(matchtype, ((IExpNode*)*nodesp)->vtype);
        if (super == NULL)
            errorMsgNode((INode*)*nodesp, ErrorBadArray, "Inconsistent type of array literal value");
        else {
            matchtype = super;
            metatsuper = 1;
        }
    }
    // No element typed successfully, so there is no element type to build on
    if (matchtype == unknownType) {
        arrlit->vtype = errorType;
        return;
    }
    // Where the elements met at a supertype rather than agreeing outright, each
    // one is coerced to it -- otherwise the element type the array claims and
    // the values stored in it would disagree about size. 'if' coerces its
    // branches to the type in common for the same reason.
    if (metatsuper) {
        for (nodesFor(arrlit->elems, cnt, nodesp))
            iexpCoerce(nodesp, matchtype);
    }
    arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit, arrlit->elems->used, matchtype);
}

// Coerce an array literal to the array type it is wanted as, when the element
// types differ but the sizes do not. Each element is coerced to the wanted
// element type on the same terms a struct literal's field value is coerced to
// its field's type, so a string literal borrows as a slice, an untyped number
// literal adopts the element's number type and a narrower value widens. It
// reaches the literals checked with no type expected of them -- an argument to
// an overload set, a generic or an operator is checked before its callee is
// chosen -- which settled their type from their elements alone. The
// element count is not coerced: a literal of another size still does not match.
int arrayLitCoerce(ArrayNode *arrlit, INode *totypedcl) {
    INode *littype = arrlit->vtype;
    if (totypedcl->tag != ArrayTag || littype->tag != ArrayTag
        || ((ArrayNode*)totypedcl)->dimens->used != 1 || ((ArrayNode*)littype)->dimens->used != 1
        || arrayDim1(totypedcl) != arrayDim1(littype))
        return 0;
    INode *elemtype = arrayElemType(totypedcl);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elems, cnt, nodesp)) {
        if (!iexpCoerce(nodesp, elemtype))
            return 0;
    }
    arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit, (size_t)arrayDim1(littype), elemtype);
    return 1;
}

// Perform data flow analysis on an array literal's element values.
//
// The list form '[a, b]' gives every element a holder of its own, so each value
// is moved or copied exactly as a function call's argument is. An array's
// contents that repeat a value, 'new Array[Handle, 2] <- fill new Handle(9)',
// are this form, the value's expression copied into each element
// (contentsLowerArray), so a move value is moved once per element, and a
// variable moved by one is gone for the next, by the ordinary move rule.
//
// The fill form, one value stored into every element, is built only for
// contents repeating a constant, which moves nothing and holds no counted
// reference: there is nothing to account for beyond reading it.
void arrayLitFlow(FlowState *fstate, ArrayNode **nodep) {
    ArrayNode *arrlit = *nodep;
    INode **elemsp;
    uint32_t cnt;

    // List form: each element is its own holder
    if (arrlit->dimens->used == 0) {
        uint16_t inflight = fstate->inflightcnt;
        for (nodesFor(arrlit->elems, cnt, elemsp)) {
            flowLoadValue(fstate, elemsp);
            flowHandleMoveOrCopy(elemsp);
            flowGateOperand(fstate, *elemsp);
        }
        flowGateOperandsEnd(fstate, inflight);
        return;
    }

    // Fill form: one constant, repeated
    flowLoadValue(fstate, &nodesGet(arrlit->elems, 0));
}

// Is the array actually a literal?
int arrayLitIsLiteral(ArrayNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->elems, cnt, nodesp)) {
        INode *elem = *nodesp;
        if (!litIsLiteral(elem))
            return 0;
    }
    return 1;
}
