/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new unsigned literal node
ULitNode *newULitNode(uint64_t nbr, INode *type) {
	ULitNode *lit;
	newNode(lit, ULitNode, ULitTag);
	lit->uintlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize an Unsigned literal
void ulitPrint(ULitNode *lit) {
	if (((NbrNode*)lit->vtype)->bits == 1)
		inodeFprint(lit->uintlit == 1 ? "true" : "false");
	else {
		inodeFprint("%ld", lit->uintlit);
		inodePrintNode(lit->vtype);
	}
}

// Create a new unsigned literal node
FLitNode *newFLitNode(double nbr, INode *type) {
	FLitNode *lit;
	newNode(lit, FLitNode, FLitTag);
	lit->floatlit = nbr;
	lit->vtype = type;
	return lit;
}

// Serialize a Float literal
void flitPrint(FLitNode *lit) {
	inodeFprint("%g", lit->floatlit);
	inodePrintNode(lit->vtype);
}

NullNode *newNullNode() {
    NullNode *node;
    newNode(node, NullNode, NullTag);
    node->vtype = NULL;
    return node;
}

// Create a new array literal
ArrLitNode *newArrLitNode() {
    ArrLitNode *lit;
    newNode(lit, ArrLitNode, ArrLitTag);
    lit->elements = newNodes(8);
    lit->vtype = NULL;
    return lit;
}

// Serialize an array literal
void arrLitPrint(ArrLitNode *node) {
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

// Check the array literal node
void arrLitWalk(PassState *pstate, ArrLitNode *arrlit) {
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

// Create a new string literal node
SLitNode *newSLitNode(char *str, INode *type) {
	SLitNode *lit;
	newNode(lit, SLitNode, StrLitTag);
	lit->strlit = str;
	lit->vtype = type;
	return lit;
}

// Serialize a string literal
void slitPrint(SLitNode *lit) {
	inodeFprint("\"%s\"", lit->strlit);
}

int litIsLiteral(INode* node) {
    return (node->tag == FLitTag || node->tag == ULitTag || node->tag == NullTag || node->tag == ArrLitTag || node->tag == StrLitTag);
}