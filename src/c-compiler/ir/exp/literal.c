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
    if (pstate->pass == TypeCheck) {
        // Ensure size is not zero
        // Ensure first element is a typed
        INode *firsttype = ((ITypedNode*)nodesGet(arrlit->elements, 0))->vtype;
        // Make type of array literal [size]firsttype
        // Ensure all elements are number literals and correctly typed
        for (nodesFor(arrlit->elements, cnt, nodesp)) {
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
	return (node->tag == FLitTag || node->tag == ULitTag);
}