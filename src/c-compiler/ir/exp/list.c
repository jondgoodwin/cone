/** Handling for list nodes (e.g., type literals)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new list node
ListNode *newListNode() {
    ListNode *lit;
    newNode(lit, ListNode, ArrLitTag);
    lit->type = NULL;
    lit->elements = newNodes(8);
    lit->vtype = NULL;
    return lit;
}

// Serialize a list node
void listPrint(ListNode *node) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    for (nodesFor(node->elements, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint("]");
}

// Check the list node
void listWalk(PassState *pstate, ListNode *arrlit) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->elements, cnt, nodesp))
        inodeWalk(pstate, nodesp);

    switch (pstate->pass) {
    case NameResolution:
        break;
    case TypeCheck:
    {
        if (arrlit->elements->used == 0) {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Array literal may not be empty");
            return;
        }

        // Get element type from first element
        INode *first = nodesGet(arrlit->elements, 0);
        if (!isExpNode(first)) {
            errorMsgNode((INode*)first, ErrorBadArray, "Array literal element must be a typed value");
            return;
        }
        INode *firsttype = ((ITypedNode*)first)->vtype;
        ((ArrayNode*)arrlit->vtype)->elemtype = firsttype;

        // Ensure all elements are number literals and consistently typed
        for (nodesFor(arrlit->elements, cnt, nodesp)) {
            if (!itypeIsSame(((ITypedNode*)*nodesp)->vtype, firsttype))
                errorMsgNode((INode*)*nodesp, ErrorBadArray, "Inconsistent type of array literal value");
            if (!litIsLiteral(*nodesp))
                errorMsgNode((INode*)*nodesp, ErrorBadArray, "Array literal element must be literal");
        }
    }
    }
}
