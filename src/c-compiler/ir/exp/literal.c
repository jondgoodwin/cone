/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new nil literal node
NilLitNode *newNilLitNode() {
    NilLitNode *nil;
    newNode(nil, NilLitNode, NilLitTag);
    nil->vtype = (INode*)newVoidNode();
    return nil;
}

// Clone nil node
INode *cloneNilLitNode(CloneState *cstate, NilLitNode *lit) {
    NilLitNode *newlit;
    newlit = memAllocBlk(sizeof(NilLitNode));
    memcpy(newlit, lit, sizeof(NilLitNode));
    newlit->vtype = cloneNode(cstate, lit->vtype);
    return (INode *)newlit;
}

// Serialize a nil node
void nilLitPrint(NilLitNode *lit) {
    inodeFprint("nil");
}

// Create a new unsigned literal node
ULitNode *newFakeULitNode(uint64_t nbr, INode *type) {
    ULitNode *lit;
    newNode(lit, ULitNode, ULitTag);
    lit->uintlit = nbr;
    lit->vtype = type;
    return lit;
}

// Create a new unsigned literal node
ULitNode *newULitNode(uint64_t nbr, INode *type) {
    ULitNode *lit;
    newNode(lit, ULitNode, ULitTag);
    lit->uintlit = nbr;
    if (type == unknownType) {
        lit->flags |= FlagUnkType;  // This flag allows us to convert it to another number type
        type = (INode*)i32Type;     // But otherwise, default is a 32-bit integer
    }
    lit->vtype = (INode*)newNameUseNode(((NbrNode*)type)->namesym);
    return lit;
}

// Create a new unsigned literal node (after name resolution)
ULitNode *newULitNodeTC(uint64_t nbr, INode *type) {
    ULitNode *lit;
    NameUseNode *typename = newNameUseNode(((NbrNode*)type)->namesym);
    typename->dclnode = type;
    newNode(lit, ULitNode, ULitTag);
    lit->uintlit = nbr;
    lit->vtype = (INode*)typename;
    return lit;
}

// Clone literal
INode *cloneULitNode(CloneState *cstate, ULitNode *lit) {
    ULitNode *newlit;
    newlit = memAllocBlk(sizeof(ULitNode));
    memcpy(newlit, lit, sizeof(ULitNode));
    newlit->vtype = cloneNode(cstate, lit->vtype);
    return (INode *)newlit;
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
    NameUseNode *typename = newNameUseNode(((NbrNode*)type)->namesym);
    lit->vtype = (INode*)typename;
    return lit;
}

// Clone literal
INode *cloneFLitNode(CloneState *cstate, FLitNode *lit) {
    FLitNode *newlit;
    newlit = memAllocBlk(sizeof(FLitNode));
    memcpy(newlit, lit, sizeof(FLitNode));
    newlit->vtype = cloneNode(cstate, lit->vtype);
    return (INode *)newlit;
}

// Serialize a Float literal
void flitPrint(FLitNode *lit) {
    inodeFprint("%g", lit->floatlit);
    inodePrintNode(lit->vtype);
}

// Name resolution of lit node
void litNameRes(NameResState* pstate, IExpNode *node) {
    inodeNameRes(pstate, &node->vtype);
}

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, IExpNode *node, INode *expectType) {
    itypeTypeCheck(pstate, &node->vtype);

    // An integer literal the source gave no suffix to becomes the integer type
    // it is wanted as, rather than being converted to it. FlagUnkType says the
    // i32 it was built with was newULitNode's default and never the source's
    // choice, and iexpMatches already lets such a literal stand for any number
    // type -- but as a conversion, and a conversion builds the constant at the
    // default width first. Every bit above the low 32 was dropped there and the
    // result widened back: 'mut n i64 = 9223372036854775807' stored -1, u64's
    // maximum stored 4294967295, and i64's minimum stored 0. Adopting the type
    // here builds the constant once, at the width it is stored at.
    //
    // Integer targets only. Coercion to a float is a real conversion, changing
    // representation rather than width, and stays on the conversion path.
    if (node->tag == ULitTag && (node->flags & FlagUnkType) && expectType != NULL
        && expectType != unknownType && expectType != noCareType) {
        INode *expect = itypeGetTypeDcl(expectType);
        if (expect->tag == IntNbrTag || expect->tag == UintNbrTag) {
            node->vtype = expect;
            node->flags &= ~FlagUnkType;
        }
    }
}

// Create a new string literal node
SLitNode *newSLitNode(char *str, uint32_t strlen) {
    SLitNode *lit;
    newNode(lit, SLitNode, StringLitTag);
    lit->strlit = str;
    lit->strlen = strlen;
    lit->vtype = unknownType;
    return lit;
}

// Clone literal
INode *cloneSLitNode(SLitNode *lit) {
    SLitNode *newlit;
    newlit = memAllocBlk(sizeof(SLitNode));
    memcpy(newlit, lit, sizeof(SLitNode));
    return (INode *)newlit;
}

// Serialize a string literal
void slitPrint(SLitNode *lit) {
    inodeFprint("\"%s\"", lit->strlit);
}

// Type check string literal node
void slitTypeCheck(TypeCheckState *pstate, SLitNode *node) {
    node->vtype = (INode*)newArrayNodeTyped((INode*)node, node->strlen, (INode*)u8Type);
}

int litIsLiteral(INode* node) {
    return (node->tag == FLitTag || node->tag == ULitTag || node->tag == StringLitTag || node->tag == NilLitTag
        || (node->tag == ArrayLitTag && arrayLitIsLiteral((ArrayNode*)node))
        || (node->tag == TypeLitTag && typeLitIsLiteral((FnCallNode*)node))
        || nameUseNames(node, ConstDclTag)
        );
}
