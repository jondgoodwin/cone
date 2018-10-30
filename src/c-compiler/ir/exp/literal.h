/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef literal_h
#define literal_h

// Unsigned integer literal
typedef struct ULitNode {
	ITypedNodeHdr;
	uint64_t uintlit;
} ULitNode;

// Float literal
typedef struct FLitNode {
	ITypedNodeHdr;
	double floatlit;
} FLitNode;

// Array literal
typedef struct ArrLitNode {
    ITypedNodeHdr;
    Nodes *elements;
} ArrLitNode;

// String literal
typedef struct SLitNode {
	ITypedNodeHdr;
	char *strlit;
} SLitNode;

// The null literal
typedef struct NullNode {
    ITypedNodeHdr;
} NullNode;

ULitNode *newULitNode(uint64_t nbr, INode *type);
void ulitPrint(ULitNode *node);

FLitNode *newFLitNode(double nbr, INode *type);
void flitPrint(FLitNode *node);

NullNode *newNullNode();

// Create a new array literal
ArrLitNode *newArrLitNode();
// Serialize an array literal
void arrLitPrint(ArrLitNode *node);
// Check the array literal node
void arrLitWalk(PassState *pstate, ArrLitNode *blk);

SLitNode *newSLitNode(char *str, INode *type);
void slitPrint(SLitNode *node);

int litIsLiteral(INode* node);

#endif