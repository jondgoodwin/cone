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

// Type check an array literal
void arrayLitTypeCheckDimExp(TypeCheckState *pstate, ArrayNode *arrlit) {

    // Handle array literal "fill" format: [dimen, fill-value]
    if (arrlit->dimens->used > 0) {

        // Ensure only one constant integer dimension
        if (arrlit->dimens->used > 1) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal may only specify one dimension");
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
            return;
        }
        INode **elemnodep = &nodesGet(arrlit->elems, 0);
        size_t dimsize = 0;
        if (dimnode->tag != ULitTag) {
            while (dimnode->tag == VarNameUseTag) {
                INode *dclnode = ((NameUseNode*)dimnode)->dclnode;
                if (dclnode->tag == ConstDclTag)
                    dimnode = ((ConstDclNode*)dclnode)->value;
                else
                    break;
            }
        }
        if (dimnode->tag == ULitTag)
            dimsize = (size_t)((ULitNode*)dimnode)->uintlit;
        if (iexpTypeCheckAny(pstate, elemnodep)) {
            arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit,
                dimsize, ((IExpNode*)*elemnodep)->vtype);
        }
        return;
    }

    // Otherwise handle multi-value array literal
    if (arrlit->elems->used == 0) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal list may not be empty");
        return;
    }

    // Ensure all elements are consistently typed (matching first element's type)
    INode *matchtype = unknownType;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elems, cnt, nodesp)) {
        if (iexpTypeCheckAny(pstate, nodesp) == 0)
            continue;
        if (matchtype == unknownType) {
            // Get element type from first element
            // Type of array literal is: array of elements whose type matches first value
            matchtype = ((IExpNode*)*nodesp)->vtype;
        }
        else if (!itypeIsSame(((IExpNode*)*nodesp)->vtype, matchtype))
            errorMsgNode((INode*)*nodesp, ErrorBadArray, "Inconsistent type of array literal value");
    }
    arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit, arrlit->elems->used, matchtype);
}

// The default type check
void arrayLitTypeCheck(TypeCheckState *pstate, ArrayNode *arrlit) {

    // In the default scenario (not as part of region allocation),
    // we must insist that array literal's dimension is a constant unsigned integer
    if (arrlit->dimens->used > 0 && !litIsLiteral(nodesGet(arrlit->dimens, 0))) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal dimension value must be a constant");
    }
    arrayLitTypeCheckDimExp(pstate, arrlit);
}

// Return a fill literal's element count, or -1 when it is not known until run time.
// The dimension may be a named constant, which code generation resolves the same way.
// A count past what an alias amount can hold is clamped to just past it, so the
// caller refuses it rather than wrapping into a plausible-looking number.
static int64_t arrayLitFillCount(ArrayNode *arrlit) {
    INode *dimnode = nodesGet(arrlit->dimens, 0);
    while (dimnode->tag == VarNameUseTag) {
        INode *dclnode = ((NameUseNode*)dimnode)->dclnode;
        if (dclnode->tag != ConstDclTag)
            return -1;
        dimnode = ((ConstDclNode*)dclnode)->value;
    }
    if (dimnode->tag != ULitTag)
        return -1;
    uint64_t nbrelems = ((ULitNode*)dimnode)->uintlit;
    return nbrelems > (uint64_t)INT16_MAX ? (int64_t)INT16_MAX + 1 : (int64_t)nbrelems;
}

// Perform data flow analysis on an array literal's element values.
//
// The list form '[a, b]' gives every element a holder of its own, so each value
// is moved or copied exactly as a function call's argument is.
//
// The fill form '[n; value]' evaluates one expression and stores it into every
// element, which the two ownership rules answer differently:
//
// - A move value has exactly one owner and cannot have n of them, so filling
//   with one is refused however small n is. Proving n is 1 would buy a construct
//   nobody writes at the cost of a rule that is harder to state.
// - A counted reference may legitimately be repeated, but n holders appear
//   rather than one, so the count rises by n -- or by n-1 when the value is a
//   temporary, which hands over the one reference it was born holding. That
//   amount is a constant in the alias node, so a count known only at run time
//   cannot be expressed and is refused rather than counted wrongly.
//
// The refusals carry two codes because they have two lifetimes. ErrorBadFill is a
// language rule -- a move value has one owner, so it may not be repeated -- and
// outlives this implementation. ErrorFillCount says only that the count cannot be
// carried by a constant alias amount, and should disappear when a fill is rewritten
// into a loop that builds the elements one at a time, before generation. That
// general answer is recorded in the Types. Array work item.
void arrayLitFlow(FlowState *fstate, ArrayNode **nodep) {
    ArrayNode *arrlit = *nodep;
    INode **elemsp;
    uint32_t cnt;

    // List form: each element is its own holder
    if (arrlit->dimens->used == 0) {
        for (nodesFor(arrlit->elems, cnt, elemsp)) {
            flowLoadValue(fstate, elemsp);
            flowHandleMoveOrCopy(elemsp);
        }
        return;
    }

    // Fill form: one value, repeated
    INode **valp = &nodesGet(arrlit->elems, 0);
    flowLoadValue(fstate, valp);
    if (iexpIsMove(*valp)) {
        errorMsgNode(*valp, ErrorBadFill,
            "An array fill literal may not repeat a move value, which may have only one owner.");
        return;
    }
    RefNode *reftype = (RefNode *)iexpGetTypeDcl(*valp);
    if (reftype->tag != RefTag || !isRegion(reftype->region, rcName))
        return;   // Any other value copies freely, needing no count

    int64_t nbrelems = arrayLitFillCount(arrlit);
    if (nbrelems < 0) {
        errorMsgNode(*valp, ErrorFillCount,
            "An array fill literal whose value is a counted reference needs an element count known at compile time.");
        return;
    }
    if (nbrelems > INT16_MAX) {
        errorMsgNode(*valp, ErrorFillCount,
            "An array fill literal has too many elements to count a reference into.");
        return;
    }
    int16_t amt = flowIsLvalRead(*valp) ? (int16_t)nbrelems : (int16_t)(nbrelems - 1);
    if (amt != 0)
        flowInjectAliasAmt(valp, amt);
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
