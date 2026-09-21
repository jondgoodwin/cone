/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <inttypes.h>

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

// Serialize an integer literal. The value is held unsigned whatever its type,
// so it is printed by the signedness of the type it was built with, at the
// full 64 bits: 'long' is 32 bits on Windows, and '%ld' printed 5000000000 as
// 705032704 and u64's maximum as -1. The type may still be a name use, so it
// is resolved before its width or signedness is read.
void ulitPrint(ULitNode *lit) {
    INode *type = itypeGetTypeDcl(lit->vtype);
    if ((type->tag == IntNbrTag || type->tag == UintNbrTag) && ((NbrNode*)type)->bits == 1)
        inodeFprint(lit->uintlit == 1 ? "true" : "false");
    else {
        if (type->tag == UintNbrTag)
            inodeFprint("%" PRIu64, lit->uintlit);
        else
            inodeFprint("%" PRId64, (int64_t)lit->uintlit);
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

// Give an untyped integer literal the number type it is wanted as, in place of
// converting it to that type. FlagUnkType says the i32 the literal was built
// with was newULitNode's default and never the source's choice, and iexpMatches
// lets such a literal stand for any number type -- but as a conversion, and a
// conversion builds the constant at the default width first. Every bit above
// the low 32 was dropped there and the result widened back: 'i64arg(5000000000)'
// passed 705032704, 'mut n i64 = 9223372036854775807' stored -1, and
// 'mut n f64 = 5000000000' held 705032704.0. Adopting the type builds the
// constant once, at the width it is stored at.
//
// An integer target retypes the node. A float target replaces it with a float
// literal built from the full 64-bit value, read as signed because that is how
// the i32 default read it and how parsePrefix folded a unary minus into it.
// Returns 1 when *nodep is now a literal of the wanted type, 0 when it was not
// an untyped integer literal or the type is not a number.
int litAdoptNumberType(INode **nodep, INode *totype) {
    INode *node = *nodep;
    if (node->tag != ULitTag || !(node->flags & FlagUnkType))
        return 0;
    INode *nbrtype = itypeGetTypeDcl(totype);
    switch (nbrtype->tag) {
    case IntNbrTag:
    case UintNbrTag:
        ((ULitNode*)node)->vtype = nbrtype;
        node->flags &= ~FlagUnkType;
        return 1;
    case FloatNbrTag: {
        int64_t value = (int64_t)((ULitNode*)node)->uintlit;
        FLitNode *flit;
        newNode(flit, FLitNode, FLitTag);
        // Rounded once, at the target's own precision: rounding to double first
        // and to float after can land a value above 2^53 on the wrong neighbour
        flit->floatlit = ((NbrNode*)nbrtype)->bits == 32 ? (double)(float)value : (double)value;
        flit->vtype = nbrtype;
        inodeLexCopy((INode*)flit, node);
        *nodep = (INode*)flit;
        return 1;
    }
    default:
        return 0;
    }
}

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, INode **nodep, INode *expectType) {
    itypeTypeCheck(pstate, &((IExpNode*)*nodep)->vtype);

    // An untyped integer literal takes the number type it is wanted as. One that
    // arrives with no expected type -- a call's argument, checked before its
    // callee is resolved -- is given it by iexpCoerce instead.
    if (expectType != NULL && expectType != unknownType && expectType != noCareType)
        litAdoptNumberType(nodep, expectType);
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
