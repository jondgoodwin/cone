/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <inttypes.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// Create a new null literal node, its pointer type not known yet
NullLitNode *newNullLitNode() {
    NullLitNode *null;
    newNode(null, NullLitNode, NullLitTag);
    null->vtype = nullLitType;
    return null;
}

// Clone null node. A template's 'null' has no type yet, and its instance's
// finds one the same way, from where it is wanted.
INode *cloneNullLitNode(CloneState *cstate, NullLitNode *lit) {
    NullLitNode *newlit;
    newlit = memAllocBlk(sizeof(NullLitNode));
    memcpy(newlit, lit, sizeof(NullLitNode));
    if (lit->vtype != nullLitType)
        newlit->vtype = cloneNode(cstate, lit->vtype);
    return (INode *)newlit;
}

// Serialize a null node
void nullLitPrint(NullLitNode *lit) {
    inodeFprint("null");
}

// Is this a 'null' whose pointer type is not known yet?
int litIsUntypedNull(INode *node) {
    return node->tag == NullLitTag && ((NullLitNode*)node)->vtype == nullLitType;
}

// Will an untyped 'null' coerce to this type? Only a raw pointer may be null:
// a reference never is, and an Option's absence is 'None'.
int litNullMatches(INode *node, INode *totype) {
    return litIsUntypedNull(node) && itypeGetTypeDcl(totype)->tag == PtrTag;
}

// Give an untyped 'null' the raw pointer type it is wanted as. It is a literal
// of that type from then on, generated as the pointer type's null constant, so
// it may be anything a constant may be: a global's value, a field's or a
// parameter's default. Wanted as anything other than a raw pointer, or where
// nothing says which raw pointer type it is ('imm p = null'), it is refused, and
// marked an error so that what uses it says nothing more.
int litAdoptNullType(INode **nodep, INode *totype) {
    if (!litIsUntypedNull(*nodep))
        return 0;
    NullLitNode *node = (NullLitNode*)*nodep;
    if (totype == errorType) {
        node->vtype = errorType;
        return 1;
    }
    if (totype == unknownType || totype == noCareType) {
        errorMsgNode((INode*)node, ErrorNullNotPtr,
            "Nothing here says which raw pointer type 'null' is. Give it one where it is wanted, as in 'imm p *u8 = null'.");
        node->vtype = errorType;
        return 1;
    }
    INode *totypedcl = itypeGetTypeDcl(totype);
    if (totypedcl->tag == PtrTag) {
        node->vtype = totype;
        return 1;
    }
    if (totypedcl != errorType)
        errorMsgNode((INode*)node, ErrorNullNotPtr,
            "'null' is a raw pointer, and %s is wanted here. Only a raw pointer type ('*T') has a null.",
            itypeName(totype));
    node->vtype = errorType;
    return 1;
}

// Type check a 'null'. Wanted as a raw pointer type, it takes that type now.
// With no expectation yet -- an argument to an overload set or an operator,
// checked before its callee is chosen -- the coercion that follows gives it
// one (iexpCoerce), and a 'null' whose value is not used has nothing to.
// Any other expectation is judged by that coercion too, which says what was
// wanted instead.
void nullLitTypeCheck(TypeCheckState *pstate, NullLitNode *node, INode *expectType) {
    if (node->vtype != nullLitType || expectType == NULL || expectType == unknownType)
        return;
    INode *nodep = (INode*)node;
    if (expectType == noCareType || itypeGetTypeDcl(expectType)->tag == PtrTag)
        litAdoptNullType(&nodep, expectType);
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
static ConstDclNode *litConstUnsettled(INode *use);

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
// or the value does not fit its default. A constant whose value is still being
// settled, one defined in terms of itself, is not followed: the fold reports it
// (litFoldConst).
int litWidenConst(INode **nodep, INode *totype) {
    INode *use = *nodep;
    if (!nameUseNames(use, ConstDclTag) || litConstUnsettled(use))
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
    // arrives with no expected type -- an argument to an overload set, a generic
    // or an operator, checked before its callee is chosen -- is given it by
    // iexpCoerce instead.
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

// A reinterpretation ('as') of a constant number to a number or pointer type is
// a constant: '4096usize as *u8' is a fixed address, known before anything runs.
// The operand is a number literal, a 'null', a named constant, or another such
// cast. This
// is answered from the tree as written, since a field's default is asked before
// it is type checked. The target is found through name resolution's binding and
// must be a number or a pointer: a struct target is reinterpreted through memory
// (genlRecast), which a global's initializer has none of. The same-size rule is
// type check's to apply (castTypeCheck), and generation folds the bitcast,
// 'inttoptr' or 'ptrtoint' of a constant operand into a constant. A number's
// conversion, 'u64.from(n)', is a type literal, asked by typeLitIsLiteral.
static int litIsConstCast(CastNode *node) {
    if ((node->flags & FlagConvert) || !isTypeNode(node->typ))
        return 0;
    switch (itypeGetTypeDcl(node->typ)->tag) {
    case UintNbrTag: case IntNbrTag: case FloatNbrTag: case PtrTag:
        break;
    default:
        return 0;
    }
    INode *exp = node->exp;
    return exp->tag == ULitTag || exp->tag == FLitTag || exp->tag == NullLitTag || nameUseNames(exp, ConstDclTag)
        || (exp->tag == CastTag && litIsConstCast((CastNode*)exp));
}

// A borrow of a string literal is a constant too: the text is a constant
// global, so its reference -- or, as a slice, its address and length -- is known
// before anything runs. That is the auto-borrow a string literal gets when a
// '&[]u8' wants it, as well as a written '&[]"text"' or '&"text"'. So is a
// borrow of an array literal whose elements are all constants, '&[1, 2, 3]',
// which generation places in a constant global the same way. A value tuple
// whose values are all constants, '1, 2', is a constant as a struct literal of
// constants is: generation folds its inserted values into a constant struct.
// A borrow of a named constant holding either literal is the borrow of that
// literal, '&K' for 'const K = [1, 2, 3]' being '&[1, 2, 3]'.
int litIsLiteral(INode* node) {
    return (node->tag == FLitTag || node->tag == ULitTag || node->tag == StringLitTag || node->tag == NilLitTag
        || node->tag == NullLitTag
        || ((node->tag == BorrowTag || node->tag == ArrayBorrowTag)
            && borrowIsConstLit(((RefNode*)node)->vtexp))
        || (node->tag == ArrayLitTag && arrayLitIsLiteral((ArrayNode*)node))
        || (node->tag == TypeLitTag && typeLitIsLiteral((FnCallNode*)node))
        || (node->tag == VTupleTag && vtupleIsLiteral((TupleNode*)node))
        || nameUseNames(node, ConstDclTag)
        || (node->tag == CastTag && litIsConstCast((CastNode*)node))
        );
}

// ---- Folding a constant expression ------------------------------------------
//
// Where a constant is required -- a named constant's value, a global's, a
// static's, a parameter's or a field's default -- an expression of constants
// is folded into the literal it computes, once it is type checked
// (litFoldConst). What folds is a built-in number operator, a comparison,
// 'not', 'and' and 'or', a number's conversion ('T.from(x)', and the widening a
// coercion injects) and a reinterpretation with 'as', each of number literals
// and named constants; inside an array, struct or tuple literal, and under a
// borrow of an array literal, each element folds on its own. The result is an
// ordinary literal node, so everything after type check sees a literal.
//
// The value is the one the operation has at run time on that type: integer
// division and remainder truncate toward zero, '>>' is arithmetic on a signed
// type and logical on an unsigned one, a float is computed at its own width,
// and a conversion does what genlConvert generates. What the run time does not
// give a value is refused rather than folded to something it never computes:
// a result its type cannot hold (an add, subtract, multiply, divide or
// negation past the integer type's range; a float past its range; a float
// converted to an integer that cannot hold its truncated value), a division or
// remainder by zero, and a shift by the width or more. The bitwise operators
// cannot overflow: '<<' discards the bits shifted out, as it does at run time,
// and '-' on an unsigned constant is negation in its own width, as it is on an
// unsigned literal ('-1u8' is 255).

// A number literal's value: an integer's bits extended to 64 from its type's
// width, signed or not as the type is, or a float's value
typedef struct {
    NbrNode *type;
    uint64_t bits;
    double f;
} LitNbr;

// The bits an integer type holds
static uint64_t litMask(NbrNode *type) {
    return type->bits >= 64 ? UINT64_MAX : ((uint64_t)1 << type->bits) - 1;
}

// An integer's low bits, as many as its type holds, extended to 64:
// sign-extended for a signed type, zero-extended for an unsigned one and Bool
static uint64_t litExtend(uint64_t v, NbrNode *type) {
    if (type->bits >= 64)
        return v;
    uint64_t mask = litMask(type);
    v &= mask;
    if (type->tag == IntNbrTag && (v >> (type->bits - 1)))
        v |= ~mask;
    return v;
}

// Does an exact result, extended to 64 bits as litExtend extends a value, lie
// in an integer type's range?
static int litIntFits(NbrNode *type, uint64_t v) {
    if (type->bits >= 64)
        return 1;
    if (type->tag == IntNbrTag) {
        int64_t half = (int64_t)1 << (type->bits - 1);
        return (int64_t)v >= -half && (int64_t)v < half;
    }
    return v <= litMask(type);
}

// The constant still under type check that a named constant's use reaches,
// through any constant whose value names another, or NULL. A use type checks
// the constant it names first, and a constant's value is folded before its
// check ends, so every constant a chain reaches is settled except one whose
// check is under way beneath this one: the chain has come back round, and that
// constant is defined in terms of itself.
static ConstDclNode *litConstUnsettled(INode *use) {
    while (use != NULL && nameUseNames(use, ConstDclTag)) {
        ConstDclNode *dcl = (ConstDclNode*)nameUseGetDcl((NameUseNode*)use);
        if ((dcl->flags & TypeChecking) && !(dcl->flags & TypeChecked))
            return dcl;
        use = dcl->value;
    }
    return NULL;
}

// Read a folded operand as a number: a number literal, or a named constant's
// use standing for one. 0 when it is neither. An untyped integer literal is
// the i32 it defaults to, and must fit it, as generation would require.
static int litNbrRead(INode *node, LitNbr *val) {
    while (nameUseNames(node, ConstDclTag))
        node = ((ConstDclNode*)nameUseGetDcl((NameUseNode*)node))->value;
    if (node->tag != ULitTag && node->tag != FLitTag)
        return 0;
    NbrNode *type = (NbrNode*)itypeGetTypeDcl(((IExpNode*)node)->vtype);
    if (type->tag != IntNbrTag && type->tag != UintNbrTag && type->tag != FloatNbrTag)
        return 0;
    val->type = type;
    if (node->tag == FLitTag) {
        // An f32 literal holds the double it was written as, and is rounded
        // to its type when it is generated: '0.1f32' is the f32 nearest 0.1
        double f = ((FLitNode*)node)->floatlit;
        val->f = type->bits == 32 ? (double)(float)f : f;
        val->bits = 0;
    }
    else {
        litCheckDefaultRange((ULitNode*)node);
        val->bits = litExtend(((ULitNode*)node)->uintlit, type);
        val->f = 0.0;
    }
    return 1;
}

// A literal of a folded value, placed where 'at' is
static INode *litNbrMake(LitNbr *val, INode *at) {
    INode *lit;
    if (val->type->tag == FloatNbrTag) {
        FLitNode *flit;
        newNode(flit, FLitNode, FLitTag);
        flit->floatlit = val->f;
        flit->vtype = (INode*)val->type;
        lit = (INode*)flit;
    }
    else {
        ULitNode *ulit = newULitNodeTC(val->bits, (INode*)val->type);
        // The value is the two's complement of its magnitude when negative,
        // which the range check reads through FlagLitNeg
        if (val->type->tag == IntNbrTag && (int64_t)val->bits < 0)
            ulit->flags |= FlagLitNeg;
        lit = (INode*)ulit;
    }
    inodeLexCopy(lit, at);
    return lit;
}

// The range of a number type, as a message gives it
static void litRangeText(NbrNode *type, char *buf, size_t size) {
    if (type->tag == FloatNbrTag)
        snprintf(buf, size, "whose finite values run from -%g to %g",
            type->bits == 32 ? 3.4028234663852886e38 : 1.7976931348623157e308,
            type->bits == 32 ? 3.4028234663852886e38 : 1.7976931348623157e308);
    else if (type->tag == IntNbrTag)
        snprintf(buf, size, "whose values run from %" PRId64 " to %" PRId64,
            (int64_t)(0 - ((uint64_t)1 << (type->bits - 1))), (int64_t)(((uint64_t)1 << (type->bits - 1)) - 1));
    else
        snprintf(buf, size, "whose values run from 0 to %" PRIu64, litMask(type));
}

// Refuse a folded result its type cannot hold
static void litFoldOverflow(INode *at, NbrNode *type, char *what) {
    char range[96];
    litRangeText(type, range, sizeof(range));
    errorMsgNode(at, ErrorConstOverflow,
        "This constant's %s does not fit %s, %s, so it has no value.",
        what, &type->namesym->namestr, range);
}

// Fold an integer operation. Returns 1 with the result, a Bool for a
// comparison, or 0 having reported why it has none.
static int litFoldInt(INode *at, int16_t op, LitNbr *a, LitNbr *b, LitNbr *r) {
    NbrNode *type = a->type;
    int issigned = type->tag == IntNbrTag;
    uint64_t x = a->bits;
    uint64_t y = b ? b->bits : 0;
    int64_t sx = (int64_t)x, sy = (int64_t)y;
    uint64_t v = 0;
    int ovf = 0;
    r->type = type;
    r->f = 0.0;
    switch (op) {
    case NegIntrinsic:
        if (issigned) {
            ovf = sx == INT64_MIN;
            v = 0 - x;
        }
        else
            v = litExtend(0 - x, type);
        break;
    case NotIntrinsic:
        v = litExtend(~x, type);
        break;
    case IsTrueIntrinsic:
        r->type = boolType;
        r->bits = x != 0;
        return 1;
    case AddIntrinsic:
        if (issigned)
            ovf = (sy > 0 && sx > INT64_MAX - sy) || (sy < 0 && sx < INT64_MIN - sy);
        else
            ovf = x + y < x;
        v = x + y;
        break;
    case SubIntrinsic:
        if (issigned)
            ovf = (sy < 0 && sx > INT64_MAX + sy) || (sy > 0 && sx < INT64_MIN + sy);
        else
            ovf = y > x;
        v = x - y;
        break;
    case MulIntrinsic:
        if (issigned) {
            if (sx > 0)
                ovf = sy > 0 ? sx > INT64_MAX / sy : sy < INT64_MIN / sx;
            else if (sx < 0)
                ovf = sy > 0 ? sx < INT64_MIN / sy : sy != 0 && sx < INT64_MAX / sy;
        }
        else
            ovf = x != 0 && y > UINT64_MAX / x;
        v = x * y;
        break;
    case SDivIntrinsic:
    case SRemIntrinsic:
    case DivIntrinsic:
    case RemIntrinsic: {
        int rem = op == SRemIntrinsic || op == RemIntrinsic;
        if (y == 0) {
            errorMsgNode(at, ErrorConstDivZero, rem
                ? "This constant is a remainder of a division by zero, which has no value."
                : "This constant is a division by zero, which has no value.");
            return 0;
        }
        if (issigned) {
            // The smallest value divided by -1 is one past the largest: no
            // value for the quotient, and LLVM gives none for the remainder
            if (sy == -1 && (int64_t)x == (int64_t)litExtend((uint64_t)1 << (type->bits - 1), type)) {
                if (rem)
                    errorMsgNode(at, ErrorConstOverflow,
                        "This constant is the remainder of %s's smallest value divided by -1, a division that overflows, so it has no value.",
                        &type->namesym->namestr);
                else
                    litFoldOverflow(at, type, "quotient");
                return 0;
            }
            v = (uint64_t)(rem ? sx % sy : sx / sy);
        }
        else
            v = rem ? x % y : x / y;
        break;
    }
    case AndIntrinsic: v = litExtend(x & y, type); break;
    case OrIntrinsic: v = litExtend(x | y, type); break;
    case XorIntrinsic: v = litExtend(x ^ y, type); break;
    case ShlIntrinsic:
    case ShrIntrinsic:
    case SShrIntrinsic:
        // The amount is the second operand, of the same type. A constant
        // shift by the width or more, a negative amount among them, is
        // refused, though at run time it is 0 or the sign (genlShift)
        if ((b->type->tag == IntNbrTag && sy < 0) || y >= type->bits) {
            errorMsgNode(at, ErrorConstShift,
                "This constant's shift is by %s%" PRIu64 ", so it has no value: a shift of %s is by 0 to %d.",
                (b->type->tag == IntNbrTag && sy < 0) ? "-" : "",
                (b->type->tag == IntNbrTag && sy < 0) ? 0 - y : y,
                &type->namesym->namestr, type->bits - 1);
            return 0;
        }
        if (op == ShlIntrinsic)
            v = litExtend(x << y, type);
        else if (op == ShrIntrinsic)
            v = (x & litMask(type)) >> y;
        else
            v = sx < 0 ? ~(~x >> y) : x >> y;
        break;
    case EqIntrinsic: v = x == y; r->type = boolType; break;
    case NeIntrinsic: v = x != y; r->type = boolType; break;
    case LtIntrinsic: v = x < y; r->type = boolType; break;
    case LeIntrinsic: v = x <= y; r->type = boolType; break;
    case GtIntrinsic: v = x > y; r->type = boolType; break;
    case GeIntrinsic: v = x >= y; r->type = boolType; break;
    case SLtIntrinsic: v = sx < sy; r->type = boolType; break;
    case SLeIntrinsic: v = sx <= sy; r->type = boolType; break;
    case SGtIntrinsic: v = sx > sy; r->type = boolType; break;
    case SGeIntrinsic: v = sx >= sy; r->type = boolType; break;
    default:
        return 0;
    }
    if (ovf || !litIntFits(r->type, v)) {
        litFoldOverflow(at, type, op == NegIntrinsic ? "negation" : "value");
        return 0;
    }
    r->bits = v;
    return 1;
}

// Fold a float operation, at the type's own width: an f32 is computed as an
// f32, as it is at run time, never as a double rounded afterward. Returns 1
// with the result, a Bool for a comparison, or 0 having reported why it has
// none. Neither operand is a NaN or an infinity: a float literal is finite, and
// a fold never yields either.
static int litFoldFloat(INode *at, int16_t op, LitNbr *a, LitNbr *b, LitNbr *r) {
    int f32 = a->type->bits == 32;
    double x = a->f;
    double y = b ? b->f : 0.0;
    double v;
    r->type = a->type;
    r->bits = 0;
    switch (op) {
    case NegIntrinsic: v = -x; break;
    case IsTrueIntrinsic:
        r->type = boolType;
        r->bits = x != 0.0;
        return 1;
    case AddIntrinsic: v = f32 ? (double)((float)x + (float)y) : x + y; break;
    case SubIntrinsic: v = f32 ? (double)((float)x - (float)y) : x - y; break;
    case MulIntrinsic: v = f32 ? (double)((float)x * (float)y) : x * y; break;
    case DivIntrinsic:
    case RemIntrinsic:
        if (y == 0.0) {
            errorMsgNode(at, ErrorConstDivZero, op == RemIntrinsic
                ? "This constant is a remainder of a division by zero, which has no value."
                : "This constant is a division by zero, which has no value.");
            return 0;
        }
        if (op == DivIntrinsic)
            v = f32 ? (double)((float)x / (float)y) : x / y;
        else
            v = f32 ? (double)fmodf((float)x, (float)y) : fmod(x, y);
        break;
    case EqIntrinsic: r->bits = x == y; r->type = boolType; return 1;
    case NeIntrinsic: r->bits = x != y; r->type = boolType; return 1;
    case LtIntrinsic: r->bits = x < y; r->type = boolType; return 1;
    case LeIntrinsic: r->bits = x <= y; r->type = boolType; return 1;
    case GtIntrinsic: r->bits = x > y; r->type = boolType; return 1;
    case GeIntrinsic: r->bits = x >= y; r->type = boolType; return 1;
    default:
        return 0;
    }
    if (!isfinite(v)) {
        litFoldOverflow(at, a->type, "value");
        return 0;
    }
    r->f = v;
    return 1;
}

// Convert a folded value to a number type, as 'T.from(x)' and a coercion's
// widening do (genlConvert): to Bool, whether it is non-zero; to a narrower
// integer, its low bits; to a wider one, sign-extended only from a signed type
// to a signed type and zero-extended from its own width otherwise; a float to
// an integer, truncated toward zero; to a float, the nearest. Returns 1 with the
// result, or 0 having reported a float whose truncated value the integer type
// cannot hold, or one past the range of the narrower float, neither of which
// the run time gives a value.
static int litFoldConvert(INode *at, LitNbr *a, NbrNode *to, LitNbr *r) {
    NbrNode *from = a->type;
    r->type = to;
    r->bits = 0;
    r->f = 0.0;
    if (to == boolType) {
        r->bits = from->tag == FloatNbrTag ? a->f != 0.0 : a->bits != 0;
        return 1;
    }
    if (to->tag == FloatNbrTag) {
        if (from->tag == FloatNbrTag)
            r->f = to->bits == 32 ? (double)(float)a->f : a->f;
        else if (from->tag == IntNbrTag)
            r->f = to->bits == 32 ? (double)(float)(int64_t)a->bits : (double)(int64_t)a->bits;
        else
            r->f = to->bits == 32 ? (double)(float)a->bits : (double)a->bits;
        if (!isfinite(r->f)) {
            litFoldOverflow(at, to, "value, converted,");
            return 0;
        }
        return 1;
    }
    if (from->tag == FloatNbrTag) {
        double t = trunc(a->f);
        double top = ldexp(1.0, to->tag == IntNbrTag ? to->bits - 1 : to->bits);
        double bottom = to->tag == IntNbrTag ? -top : 0.0;
        if (!(t >= bottom && t < top)) {
            litFoldOverflow(at, to, "value, converted from a float,");
            return 0;
        }
        r->bits = litExtend(to->tag == IntNbrTag ? (uint64_t)(int64_t)t : (uint64_t)t, to);
        return 1;
    }
    uint64_t v = a->bits;
    if (to->bits >= from->bits && !(to->tag == IntNbrTag && from->tag == IntNbrTag))
        v &= litMask(from);
    r->bits = litExtend(v, to);
    return 1;
}

// Reinterpret a folded value as a number type of the same size, as 'as' does:
// the same bits read as the other type. Returns 0 to leave the reinterpretation
// as it is: the sizes differ (type check has reported it), or the bits read as
// a float are a NaN or an infinity, which no float literal holds; generation
// still folds the reinterpretation of the constant into a constant.
static int litFoldRecast(LitNbr *a, NbrNode *to, LitNbr *r) {
    NbrNode *from = a->type;
    if (from->bits != to->bits || (from->bits != 32 && from->bits != 64 && to->tag == FloatNbrTag)
        || (from->tag == FloatNbrTag && from->bits != 32 && from->bits != 64))
        return 0;
    uint64_t bits = a->bits & litMask(from);
    if (from->tag == FloatNbrTag) {
        if (from->bits == 32) {
            float f = (float)a->f;
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            bits = u;
        }
        else
            memcpy(&bits, &a->f, sizeof(bits));
    }
    r->type = to;
    r->bits = 0;
    r->f = 0.0;
    if (to->tag == FloatNbrTag) {
        if (to->bits == 32) {
            uint32_t u = (uint32_t)bits;
            float f;
            memcpy(&f, &u, sizeof(f));
            r->f = f;
        }
        else
            memcpy(&r->f, &bits, sizeof(bits));
        return isfinite(r->f);
    }
    r->bits = litExtend(bits, to);
    return 1;
}

// The built-in number operation a type-checked call applies, or -1: the call
// of a number type's operator method, whose body is an intrinsic
static int litFoldOp(FnCallNode *call) {
    if (call->objfn == NULL || !nameUseNames(call->objfn, FnDclTag) || call->args == NULL)
        return -1;
    FnDclNode *fndcl = (FnDclNode*)nameUseGetDcl((NameUseNode*)call->objfn);
    if (fndcl->value == NULL || fndcl->value->tag != IntrinsicTag)
        return -1;
    int16_t op = ((IntrinsicNode*)fndcl->value)->intrinsicFn;
    switch (op) {
    case NegIntrinsic: case NotIntrinsic: case IsTrueIntrinsic:
        return call->args->used == 1 ? op : -1;
    case AddIntrinsic: case SubIntrinsic: case MulIntrinsic:
    case DivIntrinsic: case SDivIntrinsic: case RemIntrinsic: case SRemIntrinsic:
    case AndIntrinsic: case OrIntrinsic: case XorIntrinsic:
    case ShlIntrinsic: case ShrIntrinsic: case SShrIntrinsic:
    case EqIntrinsic: case NeIntrinsic:
    case LtIntrinsic: case LeIntrinsic: case GtIntrinsic: case GeIntrinsic:
    case SLtIntrinsic: case SLeIntrinsic: case SGtIntrinsic: case SGeIntrinsic:
        return call->args->used == 2 ? op : -1;
    default:
        return -1;
    }
}

// Replace a node that has no value with a zero of its number type, so that
// what folds from it reports nothing more, as a literal out of its range is
// left a zero (litCheckRange). Anything else becomes an error node.
static void litFoldNoValue(INode **nodep, INode *type) {
    NbrNode *nbr = (NbrNode*)itypeGetTypeDcl(type);
    if (nbr->tag == IntNbrTag || nbr->tag == UintNbrTag || nbr->tag == FloatNbrTag) {
        LitNbr zero = {nbr, 0, 0.0};
        *nodep = litNbrMake(&zero, *nodep);
    }
    else
        *nodep = newErrorNode(*nodep);
}

static void litFold(INode **nodep);

// Fold every node of a list
static void litFoldNodes(Nodes *nodes) {
    INode **nodesp;
    uint32_t cnt;
    if (nodes == NULL)
        return;
    for (nodesFor(nodes, cnt, nodesp))
        litFold(nodesp);
}

// Fold what of a type-checked expression is computed from constants alone,
// from its leaves up, replacing each such operation with the literal it
// computes. Anything else is left as it is.
static void litFold(INode **nodep) {
    INode *node = *nodep;
    LitNbr a, b, r;

    // A named constant's value is folded already, unless the chain from it
    // comes back round to a constant still being checked. A use with no type
    // was reported as circular by its type check (nameUseTypeCheck).
    if (nameUseNames(node, ConstDclTag)) {
        if (((IExpNode*)node)->vtype == errorType)
            return;
        ConstDclNode *unsettled = litConstUnsettled(node);
        if (unsettled) {
            errorMsgNode(node, ErrorCircular,
                "%s is defined in terms of itself, so it has no value to give.",
                &unsettled->namesym->namestr);
            litFoldNoValue(nodep, ((IExpNode*)node)->vtype);
        }
        return;
    }

    switch (node->tag) {
    case FnCallTag: {
        FnCallNode *call = (FnCallNode*)node;
        int op = litFoldOp(call);
        if (op < 0)
            return;
        litFoldNodes(call->args);
        if (!litNbrRead(nodesGet(call->args, 0), &a)
            || (call->args->used == 2 && !litNbrRead(nodesGet(call->args, 1), &b)))
            return;
        int ok = a.type->tag == FloatNbrTag
            ? litFoldFloat(node, (int16_t)op, &a, call->args->used == 2 ? &b : NULL, &r)
            : litFoldInt(node, (int16_t)op, &a, call->args->used == 2 ? &b : NULL, &r);
        if (ok)
            *nodep = litNbrMake(&r, node);
        else
            litFoldNoValue(nodep, call->vtype);
        return;
    }

    case NotLogicTag:
        litFold(&((LogicNode*)node)->lexp);
        if (litNbrRead(((LogicNode*)node)->lexp, &a) && a.type == boolType) {
            r.type = boolType;
            r.bits = !a.bits;
            *nodep = litNbrMake(&r, node);
        }
        return;

    case AndLogicTag:
    case OrLogicTag:
        litFold(&((LogicNode*)node)->lexp);
        litFold(&((LogicNode*)node)->rexp);
        if (litNbrRead(((LogicNode*)node)->lexp, &a) && a.type == boolType
            && litNbrRead(((LogicNode*)node)->rexp, &b) && b.type == boolType) {
            r.type = boolType;
            r.bits = node->tag == AndLogicTag ? a.bits && b.bits : a.bits || b.bits;
            *nodep = litNbrMake(&r, node);
        }
        return;

    case CastTag: {
        CastNode *cast = (CastNode*)node;
        litFold(&cast->exp);
        NbrNode *to = (NbrNode*)itypeGetTypeDcl(cast->vtype);
        if ((to->tag != IntNbrTag && to->tag != UintNbrTag && to->tag != FloatNbrTag)
            || !litNbrRead(cast->exp, &a))
            return;
        if (cast->flags & FlagConvert) {
            if (litFoldConvert(node, &a, to, &r))
                *nodep = litNbrMake(&r, node);
            else
                litFoldNoValue(nodep, cast->vtype);
        }
        else if (litFoldRecast(&a, to, &r))
            *nodep = litNbrMake(&r, node);
        return;
    }

    case TypeLitTag: {
        FnCallNode *lit = (FnCallNode*)node;
        litFoldNodes(lit->args);
        // A number's conversion, 'u64.from(n)', is a type literal of one value
        NbrNode *to = (NbrNode*)itypeGetTypeDcl(lit->vtype);
        if ((to->tag == IntNbrTag || to->tag == UintNbrTag || to->tag == FloatNbrTag)
            && lit->args->used == 1 && litNbrRead(nodesGet(lit->args, 0), &a)) {
            if (litFoldConvert(node, &a, to, &r))
                *nodep = litNbrMake(&r, node);
            else
                litFoldNoValue(nodep, lit->vtype);
        }
        return;
    }

    case NamedValTag:
        litFold(&((NamedValNode*)node)->val);
        return;

    case ArrayLitTag:
        litFoldNodes(((ArrayNode*)node)->elems);
        return;

    case VTupleTag:
        litFoldNodes(((TupleNode*)node)->elems);
        return;

    case BorrowTag:
    case ArrayBorrowTag:
        if (((RefNode*)node)->vtexp->tag == ArrayLitTag)
            litFold(&((RefNode*)node)->vtexp);
        return;

    default:
        return;
    }
}

// Fold a type-checked value where a constant is required into the literal it
// computes, and say whether it is now a constant (litIsLiteral). Answers 1
// too when folding it reported why part of it has no value, or its type check
// already reported why it has no type, so the caller does not report the value
// again as not constant.
int litFoldConst(INode **nodep) {
    int before = errors;
    litFold(nodep);
    return litIsLiteral(*nodep) || errors != before
        || (isExpNode(*nodep) && ((IExpNode*)*nodep)->vtype == errorType);
}
