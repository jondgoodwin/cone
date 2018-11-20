/** Handling for list nodes (e.g., type literals)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Serialize a list node
void typeLitPrint(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    for (nodesFor(node->args, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint("]");
}

// Check the list node
void typeLitWalk(PassState *pstate, FnCallNode *arrlit) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->args, cnt, nodesp))
        inodeWalk(pstate, nodesp);

    switch (pstate->pass) {
    case NameResolution:
        break;
    case TypeCheck:
    {
        if (arrlit->args->used == 0) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal may not be empty");
            return;
        }

        // Get element type from first element
        INode *first = nodesGet(arrlit->args, 0);
        if (!isExpNode(first)) {
            errorMsgNode((INode*)first, ErrorBadArray, "Array literal element must be a typed value");
            return;
        }
        INode *firsttype = ((ITypedNode*)first)->vtype;
        ((ArrayNode*)arrlit->vtype)->elemtype = firsttype;

        // Ensure all elements are number literals and consistently typed
        for (nodesFor(arrlit->args, cnt, nodesp)) {
            if (!itypeIsSame(((ITypedNode*)*nodesp)->vtype, firsttype))
                errorMsgNode((INode*)*nodesp, ErrorBadArray, "Inconsistent type of array literal value");
            if (!litIsLiteral(*nodesp))
                errorMsgNode((INode*)*nodesp, ErrorBadArray, "Array literal element must be literal");
        }
    }
    }
}
