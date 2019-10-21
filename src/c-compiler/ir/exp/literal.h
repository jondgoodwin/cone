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
    IExpNodeHdr;
    uint64_t uintlit;
} ULitNode;

// Float literal
typedef struct FLitNode {
    IExpNodeHdr;
    double floatlit;
} FLitNode;

// String literal
typedef struct SLitNode {
    IExpNodeHdr;
    char *strlit;
} SLitNode;

// The null literal
typedef struct NullNode {
    IExpNodeHdr;
} NullNode;

ULitNode *newULitNode(uint64_t nbr, INode *type);
void ulitPrint(ULitNode *node);

FLitNode *newFLitNode(double nbr, INode *type);
void flitPrint(FLitNode *node);

// Name resolution of lit node
void litNameRes(NameResState* pstate, IExpNode *node);

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, IExpNode *node);

NullNode *newNullNode();

SLitNode *newSLitNode(char *str, INode *type);
void slitPrint(SLitNode *node);

int litIsLiteral(INode* node);

#endif