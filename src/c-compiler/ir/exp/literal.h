/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef literal_h
#define literal_h

// Nil literal node: represents the absence of a value. Always has type "void"
typedef struct {
    IExpNodeHdr;
} NilLitNode;

// Unsigned integer literal
typedef struct {
    IExpNodeHdr;
    uint64_t uintlit;
} ULitNode;

// Float literal
typedef struct {
    IExpNodeHdr;
    double floatlit;
} FLitNode;

// String literal
typedef struct {
    IExpNodeHdr;
    char *strlit;
    uint32_t strlen;
} SLitNode;

NilLitNode *newNilLitNode();
INode *cloneNilLitNode(CloneState *cstate, NilLitNode *node);
void nilLitPrint(NilLitNode *node);

// Create a new fake unsigned literal node
ULitNode *newFakeULitNode(uint64_t nbr, INode *type);

ULitNode *newULitNode(uint64_t nbr, INode *type);

// Create a new unsigned literal node (after name resolution)
ULitNode *newULitNodeTC(uint64_t nbr, INode *type);

// Clone literal
INode *cloneULitNode(CloneState *cstate, ULitNode *lit);

void ulitPrint(ULitNode *node);

FLitNode *newFLitNode(double nbr, INode *type);
void flitPrint(FLitNode *node);

// Clone literal
INode *cloneFLitNode(CloneState *cstate, FLitNode *lit);

// Name resolution of lit node
void litNameRes(NameResState* pstate, IExpNode *node);

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, IExpNode *node, INode *expectType);

SLitNode *newSLitNode(char *str, uint32_t strlen);

// Clone literal
INode *cloneSLitNode(SLitNode *lit);

void slitPrint(SLitNode *node);

// Type check string literal node
void slitTypeCheck(TypeCheckState *pstate, SLitNode *node);

int litIsLiteral(INode* node);

#endif
