/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Note:  Creation, serialization and name checking are done with array type logic,
// as we don't yet know whether [] is a type or an array literal

// Type check an array literal
void arrayLitTypeCheck(TypeCheckState *pstate, ArrayNode *arrlit) {

    // Handle array literal "fill" format: [dimen, fill-value]
    if (arrlit->dimens->used > 0) {

        // Ensure only one constant integer dimension
        if (arrlit->dimens->used > 1) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal may only specify one dimension");
            return;
        }
        INode *dimnode = nodesGet(arrlit->dimens, 0);
        if (dimnode->tag != ULitTag) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal dimension value must be a constant integer");
            return;
        }

        // Handle and type the single fill value
        if (arrlit->elems->used != 1 || !isExpNode(nodesGet(arrlit->elems, 0))) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array fill value may only be one value");
            return;
        }
        INode **elemnodep = &nodesGet(arrlit->elems, 0);
        if (iexpTypeCheckAny(pstate, elemnodep)) {
            arrlit->vtype = (INode*)newArrayNodeTyped((INode*)arrlit,
                (size_t)((ULitNode*)dimnode)->uintlit, ((IExpNode*)*elemnodep)->vtype);
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
