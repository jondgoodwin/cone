/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <inttypes.h>
#include <ctype.h>

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

static int litFitsType(ULitNode *lit);
static void litCheckRange(ULitNode *lit, int defaulted);

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
// An integer target retypes the node, and refuses a value it cannot hold. A
// float target replaces it with a float literal built from the full 64-bit
// magnitude written, negated when parsePrefix folded a unary minus into it.
// Returns 1 when *nodep is now a literal of the wanted type, 0 when it was not
// an untyped integer literal or the type is not a number.
//
// Bool is a number type by tag -- a 1-bit unsigned -- but it is not a width to
// adopt: its only values are true and false, and a literal reaches it the way
// any other number does, through isTrue. Adopting it masked the constant to its
// low bit, so 'mut b Bool = 2', 'b = 2', a Bool return of 2, 'not 2' and
// '2 and 3' all read as false, while 'boolarg(2)' and 'Gauge[2]', which reach
// Bool through coercion instead, read as true.
int litAdoptNumberType(INode **nodep, INode *totype) {
    INode *node = *nodep;
    if (node->tag != ULitTag || !(node->flags & FlagUnkType))
        return 0;
    INode *nbrtype = itypeGetTypeDcl(totype);
    if (nbrtype == (INode*)boolType)
        return 0;
    switch (nbrtype->tag) {
    case IntNbrTag:
    case UintNbrTag:
        ((ULitNode*)node)->vtype = nbrtype;
        node->flags &= ~FlagUnkType;
        litCheckRange((ULitNode*)node, 0);
        return 1;
    case FloatNbrTag: {
        // The magnitude written, with its sign: read as a signed value,
        // '18446744073709551615' was -1
        int negated = (node->flags & FlagLitNeg) != 0;
        uint64_t magnitude = negated ? 0 - ((ULitNode*)node)->uintlit : ((ULitNode*)node)->uintlit;
        FLitNode *flit;
        newNode(flit, FLitNode, FLitTag);
        // Rounded once, at the target's own precision: rounding to double first
        // and to float after can land a value above 2^53 on the wrong neighbour
        double value = ((NbrNode*)nbrtype)->bits == 32 ? (double)(float)magnitude : (double)magnitude;
        flit->floatlit = negated ? -value : value;
        flit->vtype = nbrtype;
        inodeLexCopy((INode*)flit, node);
        *nodep = (INode*)flit;
        return 1;
    }
    default:
        return 0;
    }
}

// Widen a float literal to a wider float type at compile time, in place of
// wrapping it in a conversion. A float literal is not context-typed: '0.5' is
// an f32 however it is wanted, and reaches an f64 by the implicit widening
// every f32 value has. Left as a conversion node, the literal stopped being a
// literal, so every position that requires one refused it -- 'imm g f64 = 0.5'
// as a global, a static, a parameter default and 'const K f64 = 0.5' -- while
// the same initializer was accepted in a function body and as a field default.
// The value is the f32 widened, exactly what the conversion generated, so no
// position's value changes. Returns 1 when *nodep is now a literal of the wider
// type, 0 when it was not a float literal or the target is not a wider float.
int litWidenFloat(INode **nodep, INode *totype) {
    INode *node = *nodep;
    if (node->tag != FLitTag)
        return 0;
    NbrNode *fromtype = (NbrNode*)itypeGetTypeDcl(((FLitNode*)node)->vtype);
    NbrNode *nbrtype = (NbrNode*)itypeGetTypeDcl(totype);
    if (fromtype->tag != FloatNbrTag || nbrtype->tag != FloatNbrTag || nbrtype->bits <= fromtype->bits)
        return 0;
    double value = ((FLitNode*)node)->floatlit;
    FLitNode *flit;
    newNode(flit, FLitNode, FLitTag);
    flit->floatlit = fromtype->bits == 32 ? (double)(float)value : value;
    flit->vtype = (INode*)nbrtype;
    inodeLexCopy((INode*)flit, node);
    *nodep = (INode*)flit;
    return 1;
}

// Fold a use of a named constant into a literal of the wider number type it
// reaches, in place of wrapping the use in a conversion. The manual has a
// constant's name substitute its literal value wherever it is used, but its use
// is typed as the constant is, so 'const K = 5' is an i32 and reaches an i64 by
// the implicit widening every i32 value has. Left as a conversion node, the use
// stopped being a literal, and every position that requires one refused it --
// 'imm g i64 = K' as a global, a static, a parameter default and
// 'const K3 i64 = K' -- while a use of the constant's own type was accepted.
// The constant's own literal is left alone, since other uses still read it; the
// value is the one the conversion generated, sign-extended from a signed type
// and zero-extended from an unsigned one, so no position's value changes. A
// widening holds every value of the narrower type, so there is no range to
// check at the wider one; the constant's value was held to its own type where
// it was declared -- except an untyped integer's i32 default, which only
// generation asks about (litCheckDefaultRange). A constant whose value does not
// fit that default is not folded: its use keeps the conversion, so generation
// reports it at the constant as it does every such literal, rather than a fold
// here carrying the whole value into the wider type. Returns 1 when *nodep is
// now a literal of the wider type, 0 when it does not name a constant whose
// value is a number literal, the target is not a wider number of the same kind,
// or the value does not fit its default.
int litWidenConst(INode **nodep, INode *totype) {
    INode *use = *nodep;
    if (!nameUseNames(use, ConstDclTag))
        return 0;
    // A constant's value may name another constant
    INode *lit = use;
    while (nameUseNames(lit, ConstDclTag))
        lit = ((ConstDclNode*)((NameUseNode*)lit)->dclnode)->value;

    if (lit->tag == FLitTag) {
        if (!litWidenFloat(&lit, totype))
            return 0;
        inodeLexCopy(lit, use);
        *nodep = lit;
        return 1;
    }
    if (lit->tag != ULitTag)
        return 0;
    ULitNode *ulit = (ULitNode*)lit;
    NbrNode *fromtype = (NbrNode*)itypeGetTypeDcl(ulit->vtype);
    NbrNode *nbrtype = (NbrNode*)itypeGetTypeDcl(totype);
    if ((fromtype->tag != IntNbrTag && fromtype->tag != UintNbrTag)
        || nbrtype->tag != fromtype->tag || nbrtype->bits <= fromtype->bits)
        return 0;
    if ((ulit->flags & FlagUnkType) && !litFitsType(ulit))
        return 0;
    uint64_t value = ulit->uintlit;
    if (fromtype->bits < 64) {
        uint64_t mask = ((uint64_t)1 << fromtype->bits) - 1;
        value &= mask;
        if (fromtype->tag == IntNbrTag && (value >> (fromtype->bits - 1)))
            value |= ~mask;
    }
    ULitNode *wide = newULitNodeTC(value, (INode*)nbrtype);
    if (fromtype->tag == IntNbrTag && (int64_t)value < 0)
        wide->flags |= FlagLitNeg;
    inodeLexCopy((INode*)wide, use);
    *nodep = (INode*)wide;
    return 1;
}

// Does an integer literal's value fit the integer type it now has? The digits
// written are the literal's magnitude, recovered from the two's complement
// value by FlagLitNeg. A signed type takes one more magnitude negated than not,
// so '-128i8' fits and '128i8' does not. An unsigned type takes any magnitude
// it can hold, negated or not: the manual has a minus on an unsigned literal
// leave it unsigned, so '-1u8' is 255, the value negation has in 8 bits.
// Bool is a 1-bit unsigned by tag but is never a literal's own type
// (litAdoptNumberType refuses it), so it is not asked.
static int litFitsType(ULitNode *lit) {
    NbrNode *type = (NbrNode*)itypeGetTypeDcl(lit->vtype);
    if ((type->tag != IntNbrTag && type->tag != UintNbrTag) || type->bits <= 1)
        return 1;
    int negated = (lit->flags & FlagLitNeg) != 0;
    uint64_t magnitude = negated ? 0 - lit->uintlit : lit->uintlit;
    uint64_t most;   // The largest magnitude the type holds, as written
    if (type->tag == IntNbrTag)
        most = ((uint64_t)1 << (type->bits - 1)) - (negated ? 0 : 1);
    else
        most = type->bits >= 64 ? UINT64_MAX : ((uint64_t)1 << type->bits) - 1;
    return magnitude <= most;
}

// Refuse an integer literal whose value does not fit the integer type it now
// has, in place of materializing it at that width and silently dropping every
// bit above it: '300u8' and 'mut n u8 = 300' stored 44.
//
// 'defaulted' says the type is the i32 newULitNode gave a literal nothing
// else typed, which the message says, since the reader wrote no type. Reported
// once: the literal becomes a zero of its type, so a literal checked again, or
// a constant generated at each use, does not repeat it.
static void litCheckRange(ULitNode *lit, int defaulted) {
    if (litFitsType(lit))
        return;
    NbrNode *type = (NbrNode*)itypeGetTypeDcl(lit->vtype);
    int negated = (lit->flags & FlagLitNeg) != 0;

    // Quoted as written, digits and suffix, with the minus that was folded in
    int len = 0;
    if (lit->srcp)
        while (isalnum((unsigned char)lit->srcp[len]) || lit->srcp[len] == '_')
            ++len;
    char *sign = negated ? "-" : "";
    if (defaulted)
        errorMsgNode((INode*)lit, ErrorLitRange,
            "Integer literal '%s%.*s' does not fit %s, the type it defaults to where nothing gives it one. Give it a suffix ('%s%.*si64') or a declared type.",
            sign, len, lit->srcp, &type->namesym->namestr, sign, len, lit->srcp);
    else if (type->tag == IntNbrTag)
        errorMsgNode((INode*)lit, ErrorLitRange,
            "Integer literal '%s%.*s' does not fit %s, whose values run from %" PRId64 " to %" PRId64 ".",
            sign, len, lit->srcp, &type->namesym->namestr,
            (int64_t)(0 - ((uint64_t)1 << (type->bits - 1))), (int64_t)(((uint64_t)1 << (type->bits - 1)) - 1));
    else
        errorMsgNode((INode*)lit, ErrorLitRange,
            "Integer literal '%s%.*s' does not fit %s, whose values run from 0 to %" PRIu64 ".",
            sign, len, lit->srcp, &type->namesym->namestr,
            type->bits >= 64 ? UINT64_MAX : ((uint64_t)1 << type->bits) - 1);
    lit->uintlit = 0;
    lit->flags &= ~(FlagUnkType | FlagLitNeg);
}

// Refuse an untyped integer literal whose value does not fit the i32 it
// defaulted to. A literal still carrying FlagUnkType when it is generated was
// given a type by nothing -- not litTypeCheck, whose expectType did not reach
// it, nor iexpCoerce -- so the i32 newULitNode built it with is final, and the
// constant would be materialized at 32 bits, silently dropping every bit above
// them: 'i64arg(if v {5000000000;} else {1;})' passed 705032704. Only
// generation sees every such literal, in a function body, a global's
// initializer and a constant's value alike, after everything that could type it
// has run.
void litCheckDefaultRange(ULitNode *lit) {
    if (lit->flags & FlagUnkType)
        litCheckRange(lit, 1);
}

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, INode **nodep, INode *expectType) {
    itypeTypeCheck(pstate, &((IExpNode*)*nodep)->vtype);

    // An integer literal with a suffix has its type, and must fit it
    if ((*nodep)->tag == ULitTag && !((*nodep)->flags & FlagUnkType))
        litCheckRange((ULitNode*)*nodep, 0);

    // An untyped integer literal takes the number type it is wanted as. One that
    // arrives with no expected type -- a call's argument, checked before its
    // callee is resolved -- is given it by iexpCoerce instead.
    else if (expectType != NULL && expectType != unknownType && expectType != noCareType)
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
