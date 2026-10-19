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
// nothing says which raw pointer type it is ('let p = null'), it is refused, and
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
            "Nothing here says which raw pointer type 'null' is. Give it one where it is wanted, as in 'let p: *u8 = null'.");
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

// Create a new float literal node. With no suffix its type is unknownType: it is
// built an f64, and FlagUnkType lets it take the float type it is wanted as
// (litAdoptNumberType), as an integer literal does
FLitNode *newFLitNode(double nbr, INode *type) {
    FLitNode *lit;
    newNode(lit, FLitNode, FLitTag);
    lit->floatlit = nbr;
    if (type == unknownType) {
        lit->flags |= FlagUnkType;
        type = (INode*)f64Type;
    }
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
// passed 705032704, 'var n: i64 = 9223372036854775807' stored -1, and
// 'var n: f64 = 5000000000' held 705032704.0. Adopting the type builds the
// constant once, at the width it is stored at.
//
// An integer target retypes the node, and refuses a value it cannot hold. A
// float target replaces it with a float literal built from the full 64-bit
// magnitude written, negated when parsePrefix folded a unary minus into it.
// Returns 1 when *nodep is now a literal of the wanted type, 0 when it was not
// an untyped integer literal or the type is not a number.
//
// bool is a number type by tag -- a 1-bit unsigned -- but it is not a width to
// adopt: its only values are true and false, and a literal reaches it the way
// any other number does, through isTrue. Adopting it masked the constant to its
// low bit, so 'var b: bool = 2', 'b = 2', a bool return of 2, 'not 2' and
// '2 && 3' all read as false, while 'boolarg(2)' and 'Gauge[2]', which reach
// bool through coercion instead, read as true.
int litAdoptNumberType(INode **nodep, INode *totype) {
    INode *node = *nodep;
    // A float literal with no suffix is an f64 until a float type is wanted of
    // it, whose precision it is then rounded to; no other type is its to adopt
    if (node->tag == FLitTag && (node->flags & FlagUnkType)) {
        INode *floattype = itypeGetTypeDcl(totype);
        if (floattype->tag != FloatNbrTag)
            return 0;
        FLitNode *flit = (FLitNode*)node;
        if (((NbrNode*)floattype)->bits == 32) {
            if (isinf((float)flit->floatlit)) {
                errorMsgNode(node, ErrorFloatRange,
                    "Float literal %g does not fit f32, the type wanted here.", flit->floatlit);
                flit->floatlit = 0.0;
            }
            else
                flit->floatlit = (double)(float)flit->floatlit;
        }
        flit->vtype = floattype;
        node->flags &= ~FlagUnkType;
        return 1;
    }
    if (node->tag != ULitTag || !(node->flags & FlagUnkType))
        return 0;
    INode *nbrtype = itypeGetTypeDcl(totype);
    // Nor is a char one: an integer is no code point until it is converted,
    // 'char.from(65)'
    if (nbrtype == (INode*)boolType || nbrtype == (INode*)charType)
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

// A character literal as written is a char, a 32-bit code point, and takes the
// type u8 where a u8 is wanted, as an integer literal takes the number type its
// context wants, but only when it is ASCII: 'a' is the byte 97, and 'é' is the
// code point U+00E9, which as a single byte would be Latin-1 and not the two
// bytes UTF-8 writes it with. So comparing a byte with 'a' keeps working, and
// comparing it with 'é' is refused. Nothing else about a char reaches a number implicitly
// (nbrMatches), and a named constant holding one is a char: it is the literal as
// written that is flexible.
int litCharMatchesByte(INode *node, INode *totypedcl) {
    return node->tag == ULitTag && (node->flags & FlagCharLit) && totypedcl == (INode*)u8Type
        && ((ULitNode*)node)->uintlit < 128;
}

int litAdoptCharAsByte(INode **nodep, INode *totype) {
    INode *totypedcl = itypeGetTypeDcl(totype);
    if (!litCharMatchesByte(*nodep, totypedcl))
        return 0;
    ((ULitNode*)*nodep)->vtype = totypedcl;
    (*nodep)->flags &= ~FlagCharLit;
    return 1;
}

// An ASCII character literal that is the receiver of a binary operator, beside
// a u8, is that byte: '0' + digit, as the same literal on the other side
// ('digit + '0'') is by the argument's wanting a u8. Whichever side it is on,
// the byte decides. Returns 1 when the literal was retyped.
int litAdoptCharBesideByte(INode **objp, Nodes *args) {
    if (args == NULL || args->used != 1 || !litCharMatchesByte(*objp, (INode*)u8Type))
        return 0;
    INode *arg = nodesGet(args, 0);
    if (!isExpNode(arg) || iexpGetTypeDcl(arg) != (INode*)u8Type)
        return 0;
    return litAdoptCharAsByte(objp, (INode*)u8Type);
}

// Is this a number literal written with no suffix, whose type is still its default
// (i32 for an integer, f64 for a float) and may become any number type that fits?
int litIsUntypedNumber(INode *node) {
    return (node->tag == ULitTag || node->tag == FLitTag) && (node->flags & FlagUnkType);
}

// A number literal beside a binary operator's other operand takes that operand's
// type when it is known, so '0.5 - s' is an f64's subtraction when 's' is an f64,
// and '1 - n' an i64's: the literal on the left is the receiver, which would
// otherwise decide the type by its default. 'othertype' is the type of the
// other operand. Two literals together keep their defaults, an operand whose
// type is no number is none to take, and a float literal takes only a float
// type. Returns 1 when the literal was retyped.
int litAdoptBesideOperand(INode **objp, INode *othertype) {
    if (!litIsUntypedNumber(*objp) || othertype == NULL || othertype == unknownType || othertype == noCareType)
        return 0;
    INode *nbrtype = itypeGetTypeDcl(othertype);
    if (nbrtype->tag != IntNbrTag && nbrtype->tag != UintNbrTag && nbrtype->tag != FloatNbrTag)
        return 0;
    return litAdoptNumberType(objp, nbrtype);
}

int litCharRefusedAsByte(INode *node, INode *totypedcl) {
    return node->tag == ULitTag && (node->flags & FlagCharLit) && totypedcl == (INode*)u8Type
        && ((ULitNode*)node)->uintlit >= 128;
}

// A number literal's value as a signed 64-bit integer. 0 when its magnitude is
// beyond what a signed 64 bits holds, which is none this folds.
static int litUntypedInt(INode *node, int64_t *v) {
    ULitNode *lit = (ULitNode*)node;
    int negated = (lit->flags & FlagLitNeg) != 0;
    uint64_t magnitude = negated ? 0 - lit->uintlit : lit->uintlit;
    if (negated ? magnitude > (uint64_t)INT64_MAX + 1 : magnitude > (uint64_t)INT64_MAX)
        return 0;
    *v = negated ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    return 1;
}

static double litUntypedDouble(INode *node) {
    if (node->tag == FLitTag)
        return ((FLitNode*)node)->floatlit;
    int64_t v;
    if (!litUntypedInt(node, &v)) {
        ULitNode *lit = (ULitNode*)node;
        double magnitude = (double)((lit->flags & FlagLitNeg) ? 0 - lit->uintlit : lit->uintlit);
        return (lit->flags & FlagLitNeg) ? -magnitude : magnitude;
    }
    return (double)v;
}

// An expression made only of number literals with no suffix -- '2.0 * 3.0', '200 + 100',
// '-(1 + 2)' -- takes the type its place gives it, as a single such literal does,
// and the default of its kind where nothing does. So it is folded here, as its operands
// are known, into one untyped literal whose value is what the expression comes to;
// where a place names the type the literal then adopts it, and a value that type
// cannot hold is refused at the literal (litCheckRange). A sum, difference, product,
// quotient or remainder, and a negation. Integers are computed exactly, in 64 bits,
// division truncating toward zero; a float is computed as an f64, the result
// rounded once to the f32 a place may ask for. An integer operand beside a float one
// is that number as a float, the operand's own kind deciding the subexpression it
// is in ('7 / 2 + 0.5' is 3.5). What would not be a finite literal or fits no 64 bits
// is not folded, nor a division by zero: the operation is left for type check and
// generation to judge as it has been. The node is made the literal in place: both are
// expression nodes with the same header, and nothing yet points into the call's
// operands. Returns 1 when it was folded.
int litFoldUntyped(FnCallNode *call) {
    if (!(call->flags & FlagOperator) || call->methfld == NULL || !isNameUseNode(call->methfld) || call->objfn == NULL)
        return 0;
    Name *op = ((NameUseNode*)call->methfld)->namesym;
    int unary = call->args == NULL || call->args->used == 0;
    if (!unary && call->args->used != 1)
        return 0;
    if (op != plusName && op != minusName && op != multName && op != divName && op != remName)
        return 0;
    if (unary && op != minusName)
        return 0;
    INode *a = call->objfn;
    INode *b = unary ? NULL : nodesGet(call->args, 0);
    if (!litIsUntypedNumber(a) || (a->flags & FlagCharLit) || (b && (!isExpNode(b) || !litIsUntypedNumber(b) || (b->flags & FlagCharLit))))
        return 0;
    // (a ULit's FlagMatchAny shares a bit with other nodes' flags: a literal 'case' pattern is no operand)
    if ((a->tag == ULitTag && (a->flags & FlagMatchAny)) || (b && b->tag == ULitTag && (b->flags & FlagMatchAny)))
        return 0;

    int isfloat = a->tag == FLitTag || (b && b->tag == FLitTag);
    uint64_t ibits = 0;
    int ineg = 0;
    double fval = 0.0;
    if (!isfloat) {
        int64_t x, y, r;
        if (!litUntypedInt(a, &x) || (b && !litUntypedInt(b, &y)))
            return 0;
        if (unary) {
            if (x == INT64_MIN) return 0;
            r = -x;
        }
        else if (op == plusName) {
            if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) return 0;
            r = x + y;
        }
        else if (op == minusName) {
            if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) return 0;
            r = x - y;
        }
        else if (op == multName) {
            if (x != 0 && y != 0) {
                if ((x == -1 && y == INT64_MIN) || (y == -1 && x == INT64_MIN)) return 0;
                r = (int64_t)((uint64_t)x * (uint64_t)y);
                if (r / y != x) return 0;
            }
            else
                r = 0;
        }
        else {
            if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
            r = op == divName ? x / y : x % y;
        }
        ineg = r < 0;
        ibits = (uint64_t)r;
    }
    else {
        double x = litUntypedDouble(a);
        double y = b ? litUntypedDouble(b) : 0.0;
        double r;
        if (unary) r = -x;
        else if (op == plusName) r = x + y;
        else if (op == minusName) r = x - y;
        else if (op == multName) r = x * y;
        else if (op == divName) r = x / y;
        else r = fmod(x, y);
        if (!isfinite(r))
            return 0;
        fval = r;
    }

    // The call's memory becomes the literal's
    INode *deftype = isfloat ? (INode*)f64Type : (INode*)i32Type;
    NameUseNode *typename = newNameUseNode(((NbrNode*)deftype)->namesym);
    typename->dclnode = deftype;
    if (isfloat) {
        FLitNode *lit = (FLitNode*)call;
        lit->tag = FLitTag;
        lit->flags = FlagUnkType;
        lit->vtype = (INode*)typename;
        lit->floatlit = fval;
    }
    else {
        ULitNode *lit = (ULitNode*)call;
        lit->tag = ULitTag;
        lit->flags = FlagUnkType | FlagLitFolded | (ineg ? FlagLitNeg : 0);
        lit->vtype = (INode*)typename;
        lit->uintlit = ibits;
    }
    return 1;
}

// Widen a float literal to a wider float type at compile time, in place of
// wrapping it in a conversion. A float literal is not context-typed: '0.5' is
// an f32 however it is wanted, and reaches an f64 by the implicit widening
// every f32 value has. Left as a conversion node, the literal stopped being a
// literal, so every position that requires one refused it -- 'let g: f64 = 0.5'
// as a global, a static, a parameter default and 'const K: f64 = 0.5' -- while
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
// 'let g: i64 = K' as a global, a static, a parameter default and
// 'const K3: i64 = K' -- while a use of the constant's own type was accepted.
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
// bool is a 1-bit unsigned by tag but is never a literal's own type
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
// bit above it: '300u8' and 'var n: u8 = 300' stored 44.
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

    // An expression of integer literals, computed (litFoldUntyped): no digits were
    // written for it, so the value it came to is what is quoted
    if (lit->flags & FlagLitFolded) {
        uint64_t magnitude = negated ? 0 - lit->uintlit : lit->uintlit;
        if (defaulted)
            errorMsgNode((INode*)lit, ErrorLitRange,
                "The value %s%" PRIu64 " of this expression of literals does not fit %s, the type it defaults to where nothing gives it one. Give it a declared type, or a suffix on one of its literals.",
                negated ? "-" : "", magnitude, &type->namesym->namestr);
        else
            errorMsgNode((INode*)lit, ErrorLitRange,
                "The value %s%" PRIu64 " of this expression of literals does not fit %s.",
                negated ? "-" : "", magnitude, &type->namesym->namestr);
        lit->uintlit = 0;
        lit->flags &= ~(FlagUnkType | FlagLitNeg | FlagLitFolded);
        return;
    }

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
    if ((*nodep)->tag == ULitTag && !((*nodep)->flags & FlagUnkType)) {
        litCheckRange((ULitNode*)*nodep, 0);
        // A character literal wanted as a byte is the byte, if it is ASCII
        if (expectType != NULL && expectType != unknownType && expectType != noCareType)
            litAdoptCharAsByte(nodep, expectType);
    }

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

// Type check string literal node. A literal is a borrow of its text that lasts
// for the whole program, '&imm str' (global scope, the default of newRefNodeFull).
// One already taken as the bytes it holds, an array (slitAsArray), stays so.
void slitTypeCheck(TypeCheckState *pstate, SLitNode *node) {
    if (node->vtype != unknownType && node->vtype != NULL && itypeGetTypeDcl(node->vtype)->tag == ArrayTag)
        return;
    // 'str' is named, as a written '&str' names it, so that a generic given
    // this type as an argument (inferred from a literal) copies a name, not
    // the struct
    node->vtype = (INode*)newRefNodeFull(RefTag, (INode*)node, borrowRef, newPermUseNode(immPerm),
        newNameUseFromDclNode((INode*)strTypeDcl, (INode*)node));
}

// Take a literal as the bytes it holds, an array of its length with no NUL
// counted (the NUL after them stays in the constant, for C). An array is the
// literal's place in memory: what a written borrow of it, '&"text"', is a
// reference to, and what fills a byte array (slitCoerce).
void slitAsArray(SLitNode *node) {
    node->vtype = (INode*)newArrayNodeTyped((INode*)node, node->strlen, (INode*)u8Type);
}

// Is this a string literal still typed as the '&imm str' it is?
static int slitIsText(INode *node) {
    return node->tag == StringLitTag && ((SLitNode*)node)->vtype != unknownType
        && itypeGetTypeDcl(((SLitNode*)node)->vtype)->tag == RefTag;
}

// Is this type a byte array exactly as long as the literal's text?
static int slitFitsArray(SLitNode *node, INode *totypedcl) {
    return totypedcl->tag == ArrayTag && itypeGetTypeDcl(arrayElemType(totypedcl)) == (INode*)u8Type
        && arrayDim1(totypedcl) == node->strlen;
}

// Is this type an owner of 'str', in any region?
static int slitIsStrOwner(INode *totypedcl) {
    return totypedcl->tag == RefTag && itypeGetTypeDcl(((RefNode*)totypedcl)->region) != borrowRef
        && refIsFat((RefNode*)totypedcl);
}

// The static function 'fromLiteral(text &str) Self' a struct declares to say a
// string literal may stand where a value of it is wanted, the text copied in
// (collections' String does); NULL for any other type.
static FnDclNode *slitFromLiteralFn(INode *totypedcl) {
    if (totypedcl->tag != StructTag || ((StructNode*)totypedcl)->genericinfo
        || (totypedcl->flags & (TraitType | EnumType)))
        return NULL;
    INode *found = iNsTypeFindFnField((INsTypeNode*)totypedcl, nametblFind("fromLiteral", 11));
    if (found && found->tag == AliasDclTag)
        found = aliasDclResolve(found);
    if (found == NULL || found->tag != FnDclTag || (found->flags & FlagMethFld) || inodeIsPrivate(found))
        return NULL;
    return (FnDclNode*)found;
}

// Would a string literal be taken where this type is wanted, beyond the
// '&str' it is (and the '&Array[u8]' that converts from it)? A byte array of its
// length, filled by it; an owner of 'str', which it is copied into; or a type
// that declares 'fromLiteral', which makes its value from it.
int slitMatches(INode *node, INode *totypedcl) {
    if (!slitIsText(node))
        return 0;
    return slitFitsArray((SLitNode*)node, totypedcl) || slitIsStrOwner(totypedcl)
        || slitFromLiteralFn(totypedcl) != NULL;
}

// A string literal made into a 'cstr' (by itself or lent as a temporary one): C
// reads up to the first NUL, so a NUL inside the literal would cut the text
// short and silently. Reported where it is known, at the literal. The
// conversion still goes ahead, so nothing else is said about the value.
static void slitCStrCheck(SLitNode *lit, INode *totypedcl) {
    if (totypedcl == (INode*)cstrTypeDcl && lit->strlen > 0 && memchr(lit->strlit, 0, lit->strlen))
        errorMsgNode((INode*)lit, ErrorCStrNul,
            "A string literal holding a NUL byte cannot be a cstr: C reads a string up to its first NUL, so it would see only the text before it. Pass the text another way, as a &str or a String.");
}

// Coerce a string literal to a byte array it fills or an owner of 'str' it is
// copied into [Jon 6 Oct: "Implicit copy is better."]. The copy is the
// allocation 'Uni[str](lit)' builds, region and permission as the wanted
// type has them. Only the literal itself is taken this way: any other '&str'
// needs the 'new'. Returns 1 when *nodep now has the wanted type, 0 when the
// literal is not wanted as either (nothing changed, nothing reported).
int slitCoerce(INode **nodep, INode *totypedcl) {
    if (!slitIsText(*nodep))
        return 0;
    SLitNode *lit = (SLitNode*)*nodep;
    if (slitFitsArray(lit, totypedcl)) {
        slitAsArray(lit);
        return 1;
    }
    // A type that declares 'fromLiteral' is made by calling it on the literal
    FnDclNode *fromlit = slitFromLiteralFn(totypedcl);
    if (fromlit) {
        slitCStrCheck(lit, totypedcl);
        fnCallDemandCandidates((INode*)fromlit);
        FnCallNode *call = newFnCallLower((INode*)lit, (INode*)newNameUseFromDclNode((INode*)fromlit, (INode*)lit), 1);
        nodesAdd(&call->args, (INode*)lit);
        fnCallFinalizeArgs(NULL, call);
        *nodep = (INode*)call;
        return 1;
    }
    if (!slitIsStrOwner(totypedcl))
        return 0;
    RefNode *reftype = (RefNode*)totypedcl;
    RefNode *alloc = newRefNode(AllocateTag);
    inodeLexCopy((INode*)alloc, (INode*)lit);
    alloc->region = reftype->region;
    alloc->perm = reftype->perm;
    alloc->vtexp = (INode*)lit;
    alloc->vtype = totypedcl;
    regionAllocTypeCheck(alloc->region);
    permInitTypeCheck(itypeGetTypeDcl(alloc->perm));
    *nodep = (INode*)alloc;
    return 1;
}

// A string literal wanted as a borrowed reference to a type that declares
// 'fromLiteral': is it one, and does the borrow write? Answers the wanted
// type's target, the type that declares it, or NULL for any other wanted type.
static INode *slitBorrowTarget(INode *node, INode *totypedcl) {
    if (!slitIsText(node) || totypedcl->tag != RefTag
        || itypeGetTypeDcl(((RefNode*)totypedcl)->region) != borrowRef)
        return NULL;
    INode *target = itypeGetTypeDcl(((RefNode*)totypedcl)->vtexp);
    return slitFromLiteralFn(target) ? target : NULL;
}

// Does a borrow let what it points at be written (a lock permission included)?
static int slitBorrowWrites(RefNode *ref) {
    return !isTypeNode(ref->perm) || ref->perm == unknownType
        || permIsLock(ref->perm) || (permGetFlags(ref->perm) & MayWrite);
}

// Would a string literal be lent as a temporary where this type is wanted: a
// read-only borrowed reference to a type that declares 'fromLiteral'? The
// borrow is made by slitBorrowCoerce, which needs the type check state; a
// coercion without one (iexpCoerce) leaves it unmade and the literal unmatched.
int slitBorrowMatches(INode *node, INode *totypedcl) {
    return slitBorrowTarget(node, totypedcl) != NULL && !slitBorrowWrites((RefNode*)totypedcl);
}

// Does a call of overloaded functions count this conversion yet? Only while
// selection falls back to it (slitBorrowFallback): a literal that a candidate
// takes as it is -- as the '&str' it is -- is never lent as a temporary while
// another candidate would take it so, which would make 'd["a"]' ambiguous
// between a dictionary's '&K' and '&str' indexes.
static int slitBorrowSelecting = 0;

void slitBorrowFallback(int on) {
    slitBorrowSelecting = on;
}

int slitBorrowOffered(INode *node, INode *totypedcl) {
    return slitBorrowSelecting && slitBorrowMatches(node, totypedcl);
}

// Is any of these arguments a string literal, which selection could retry?
int slitAnyText(INode **self, Nodes *args) {
    INode **argp;
    uint32_t cnt;
    if (self && slitIsText(*self))
        return 1;
    if (args == NULL)
        return 0;
    for (nodesFor(args, cnt, argp))
        if (slitIsText(*argp))
            return 1;
    return 0;
}

// A string literal wanted as a borrow that writes, of a type that declares
// 'fromLiteral' ('&mut String'): refused, saying why. A write to the temporary
// would be lost with it. Answers 1 having reported it.
int slitBorrowRefused(INode *node, INode *totypedcl) {
    INode *target = slitBorrowTarget(node, totypedcl);
    if (target == NULL || !slitBorrowWrites((RefNode*)totypedcl))
        return 0;
    errorMsgNode(node, ErrorLitBorrowWrite,
        "A string literal is lent as a temporary %s only to a read-only borrow, '&%s'. This wants a borrow that writes, and a write to a temporary is lost when the statement ends. Make the %s first, '%s.fromLiteral(text)', and lend that.",
        itypeName(target), itypeName(target), itypeName(target), itypeName(target));
    return 1;
}

// A string literal where a read-only borrow of a type that declares
// 'fromLiteral' is wanted becomes the borrow of a temporary, '&T.fromLiteral(lit)',
// built and checked as that borrow is when written: the temporary lives to the
// end of the statement and is finalized there, a borrow of it used after that
// is refused, and a local's initializer extends it to the end of the block
// ('let r: &String = "x"'). The borrow is the one the wanted type names, so the
// permission is its own. Nothing here knows any type: 'T' is whatever declares
// the function. Answers whether the result meets the wanted type.
int slitBorrowCoerce(TypeCheckState *pstate, INode **nodep, INode *totype) {
    INode *totypedcl = itypeGetTypeDcl(totype);
    SLitNode *lit = (SLitNode*)*nodep;
    slitCStrCheck(lit, slitBorrowTarget(*nodep, totypedcl));
    FnDclNode *fromlit = slitFromLiteralFn(slitBorrowTarget(*nodep, totypedcl));
    fnCallDemandCandidates((INode*)fromlit);
    FnCallNode *call = newFnCallLower((INode*)lit, (INode*)newNameUseFromDclNode((INode*)fromlit, (INode*)lit), 1);
    nodesAdd(&call->args, (INode*)lit);
    RefNode *borrow = newRefNodeFull(BorrowTag, (INode*)lit, borrowRef,
        newPermUseNode((PermNode*)itypeGetTypeDcl(((RefNode*)totypedcl)->perm)), (INode*)call);
    *nodep = (INode*)borrow;
    borrowTypeCheck(pstate, (RefNode**)nodep);
    return iexpCoerce(nodep, totype);
}

int slitDefaultDeferred(INode *node, INode *totype) {
    if (totype == unknownType || totype == noCareType || !slitIsText(node))
        return 0;
    INode *totypedcl = itypeGetTypeDcl(totype);
    return slitBorrowMatches(node, totypedcl) || slitFromLiteralFn(totypedcl) != NULL;
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

// Does this struct hold nothing but its enum's tag? An enum's tag field is the only
// one a variant of such an enum has: no field of its own, and none spliced in from
// the enum.
static int litIsTagOnly(StructNode *variant) {
    INode **fldp;
    uint32_t cnt;
    for (nodelistFor(&variant->fields, cnt, fldp))
        if (!((*fldp)->flags & IsTagField))
            return 0;
    return 1;
}

// A payload-less variant standing as its enum, 'Kind.Os' wanted as a 'Kind' or
// 'None' as an 'Option[i32]', is a constant: the tag it carries and nothing else.
// Type check makes it the recast of the variant's literal, which holds the tag
// alone, to the enum. The enum may have other variants that hold values: this one
// holds none, so the enum's value for it is the tag and padding, which generation
// builds at the enum's layout (genlRecast). A variant that holds a value is not,
// nor is any variant of an enum with a field in common (that field's value is no
// part of the tag).
static int litIsEnumVariant(CastNode *node) {
    if (node->flags & (FlagConvert | 0x00FF) || node->exp->tag != TypeLitTag
        || node->vtype == NULL || !isTypeNode(node->vtype))
        return 0;
    INode *enumdcl = itypeGetTypeDcl(node->vtype);
    if (enumdcl->tag != StructTag || !(enumdcl->flags & EnumType)
        || ((StructNode*)enumdcl)->derived == NULL || ((StructNode*)enumdcl)->derived->used == 0)
        return 0;
    FnCallNode *lit = (FnCallNode*)node->exp;
    if (lit->vtype == NULL || !isTypeNode(lit->vtype))
        return 0;
    INode *variant = itypeGetTypeDcl(lit->vtype);
    if (variant->tag != StructTag || variant == enumdcl || !litIsTagOnly((StructNode*)variant))
        return 0;
    return typeLitIsLiteral(lit);
}

// A string literal wanted as a '&Array[u8]' is the literal reinterpreted (a recast of
// the '&imm str' it is), and as constant as it is.
static int litIsTextAsBytes(CastNode *node) {
    return !(node->flags & FlagConvert) && node->exp->tag == StringLitTag;
}

// A borrow of a string literal is a constant too: the text is a constant
// global, so its reference -- or, as a slice, its address and length -- is known
// before anything runs. That is a written '&[]"text"' or '&"text"'. So is a
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
        || (node->tag == CastTag && litIsTextAsBytes((CastNode*)node))
        || (node->tag == CastTag && litIsEnumVariant((CastNode*)node))
        );
}

// ---- Folding a constant expression ------------------------------------------
//
// Where a constant is required -- a named constant's value, a global's, a
// static's, a parameter's or a field's default -- an expression of constants
// is folded into the literal it computes, once it is type checked
// (litFoldConst). What folds is a built-in number operator, a comparison,
// '!', '&&' and '||', a number's conversion ('T.from(x)', and the widening a
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
// sign-extended for a signed type, zero-extended for an unsigned one and bool
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

// Fold an integer operation. Returns 1 with the result, a bool for a
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
    case CmpIntrinsic: v = x > y ? 1 : x < y ? (uint64_t)-1 : 0; r->type = i32Type; break;
    case SCmpIntrinsic: v = sx > sy ? 1 : sx < sy ? (uint64_t)-1 : 0; r->type = i32Type; break;
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
// with the result, a bool for a comparison, or 0 having reported why it has
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
    // Two floats neither greater nor less than the other answer 0 (a NaN is never folded)
    case CmpIntrinsic: r->bits = x > y ? 1 : x < y ? (uint64_t)-1 : 0; r->type = i32Type; return 1;
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
// widening do (genlConvert): to bool, whether it is non-zero; to a narrower
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
    case CmpIntrinsic: case SCmpIntrinsic:
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
