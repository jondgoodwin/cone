/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <inttypes.h>
#include <string.h>
#include <assert.h>

// Return expression node's "declared" type
INode *iexpGetTypeDcl(INode *node) {
    if (isExpNode(node) || (node)->tag == VarDclTag || (node)->tag == FnDclTag || (node)->tag == FieldDclTag) {
        return itypeGetTypeDcl(((IExpNode *)node)->vtype);
    }
    else {
        //if (isTypeNode(node)) // caller should be using itypeGetTypeDcl()
        //    return node;
        errorUnreachable(node, "a request for the type of a node that is not an expression");
        return unknownType;
    }
}

// Return type (or de-referenced type if ptr/ref)
INode *iexpGetDerefTypeDcl(INode *node) {
    return itypeGetDerefTypeDcl(iexpGetTypeDcl(node));
}

// Type check node we expect to be an expression. Return 0 if not.
int iexpTypeCheckAny(TypeCheckState *pstate, INode **from) {
    inodeTypeCheckAny(pstate, from);
    // From should be a typed expression node
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 0;
    }
    return 1;
}

// Return whether it is okay for from expression to be coerced to to-type
TypeCompare iexpMatches(INode **from, INode *totype, SubtypeConstraint constraint) {
    // A 'null' not yet typed is whichever raw pointer type is wanted, and nothing else
    if (litIsUntypedNull(*from))
        return litNullMatches(*from, totype) ? EqMatch : NoMatch;

    INode *fromtype = iexpGetTypeDcl(*from);

    // Is totype a supertype of (or equivalent to) from's type?
    TypeCompare result = itypeMatches(totype, fromtype, constraint);
    if (result != NoMatch)
        return result;

    // Handle implicit coercion to boolean, if type supports "isTrue" method
    INode *foundnode;
    if (totype == (INode*)boolType) {
        if ((foundnode = aliasDclResolve(iTypeFindFnField(fromtype, istrueName)))
            && (foundnode->tag == FnDclTag || foundnode->tag == FnOverloadDclTag))
            return ConvByMeth;
        else
            return NoMatch;
    }

    // Handle implicit conversion of untyped literal integer to any number type
    INode *totyp = itypeGetTypeDcl(totype);
    if ((*from)->tag == ULitTag && ((*from)->flags & FlagUnkType) && totyp != (INode*)charType
        && (totyp->tag == UintNbrTag || totyp->tag == IntNbrTag || totyp->tag == FloatNbrTag)) {
        return ConvSubtype;  // For literals, we do not care if to a supertype (for user convenience)
    }

    // An ASCII character literal, as written, stands for a u8 where one is wanted
    if (litCharMatchesByte(*from, totyp))
        return ConvSubtype;

    // A string literal fills a byte array of its length, and is copied into an
    // owner of 'str'
    if (slitMatches(*from, totyp))
        return ConvSubtype;

    // A string literal is lent as a temporary where a read-only borrow of a
    // type that declares 'fromLiteral' is wanted, in a call that no candidate
    // takes without it (slitBorrowFallback); iexpCoerceIn makes the borrow
    if (slitBorrowOffered(*from, totyp))
        return ConvSubtype;

    // Can we auto-borrow to match on the expected type?
    if (borrowAutoMatches(*from, (RefNode*)totyp)) {
        return ConvBorrow;    // Auto-borrow
    }

    return NoMatch;
}

// Is this the type of a borrowed reference, whose scope is a lifetime? A key's
// invariant lifetime is no scope: it goes anywhere.
int iexpIsBorrowType(INode *type) {
    return (type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag)
        && ((RefNode*)type)->region == borrowRef && !lifeIsInvariant(((RefNode*)type)->lifename);
}

// A copy of the borrowed-reference type 'typedcl' carrying the lifetime 'scope'.
// A declared type is one node shared by everything written with it, so a
// lifetime known only where the type is used goes on a copy belonging to that
// use -- exactly as fnCallFinalizeArgs builds one for a call's result.
INode *iexpScopedBorrowType(INode *typedcl, INode *lexnode, uint16_t scope) {
    RefNode *ref = (RefNode*)typedcl;
    RefNode *scoped = newRefNodeFull(typedcl->tag, lexnode, borrowRef, ref->perm, ref->vtexp);
    scoped->scope = scope;
    scoped->lifename = ref->lifename;
    scoped->bound = ref->bound;
    scoped->marks = ref->marks;
    return (INode*)scoped;
}

// The type of a value that may be any of several -- an 'if' or 'match' arm, a
// block's last value or one of its breaks -- given 'scope', the narrowest
// lifetime among them (the highest number). Whichever value is given, it lives
// only as long as the shortest-lived could, so a borrowed-reference type carries
// that lifetime.
INode *iexpNarrowestType(INode *type, INode *lexnode, uint16_t scope) {
    INode *typedcl = itypeGetTypeDcl(type);
    if (!iexpIsBorrowType(typedcl) || scope <= ((RefNode*)typedcl)->scope)
        return type;
    return iexpScopedBorrowType(typedcl, lexnode, scope);
}

// Widen 'scope', the narrowest lifetime seen so far, by the value 'exp'
uint16_t iexpNarrowerScope(uint16_t scope, INode *exp) {
    if (!isExpNode(exp))
        return scope;
    INode *type = iexpGetTypeDcl(exp);
    if (iexpIsBorrowType(type) && ((RefNode*)type)->scope > scope)
        return ((RefNode*)type)->scope;
    return scope;
}

// The type a coercion's result carries. A borrowed reference keeps the lifetime
// it was borrowed with when it is widened to a base trait's reference or turned
// into a virtual reference: the value still points at what it was borrowed from.
// An owning reference held in a place and wanted as a borrowed one is borrowed
// from ('&*owner'), so the result has the lifetime of a borrow through the
// owner: the place's, a by-value parameter's or a local's never reaching the
// caller. The type coerced to is a declared node, so the scope goes on a copy
// belonging to this coercion. varDclTypeCheck gives a local declared with a
// borrowed-reference type the same copy, so the local keeps its initializer's
// lifetime.
INode *iexpCoerceType(INode *from, INode *totypedcl) {
    if (!iexpIsBorrowType(totypedcl))
        return totypedcl;
    INode *fromtype = iexpGetTypeDcl(from);
    uint16_t scope = 0;
    if (iexpIsBorrowType(fromtype))
        scope = ((RefNode*)fromtype)->scope;
    else if ((fromtype->tag == RefTag || fromtype->tag == VirtRefTag) && iexpIsLval(from)) {
        INode *lvalperm;
        iexpGetLvalInfo(from, &lvalperm, &scope);
    }
    if (scope == 0)
        return totypedcl;
    return iexpScopedBorrowType(totypedcl, from, scope);
}

static int iexpCoerceShape(INode **from, INode *totype);

// Is this a reference to text: '&str', which a literal is?
static int iexpIsTextRef(INode *type) {
    return type->tag == RefTag && itypeGetTypeDcl(((RefNode*)type)->vtexp) == (INode*)strTypeDcl;
}

// Refuse, saying what is meant, a crossing between a slice, text, a raw
// pointer and a 'cstr' that the C boundary asks to be explicit. Answers 1
// having reported it. The caller builds the conversion anyway, so what uses the
// value says nothing more.
static int iexpCPtrMismatch(INode *from, INode *totypedcl) {
    INode *fromtype = iexpGetTypeDcl(from);
    int toptr = totypedcl->tag == PtrTag;
    int tocstr = totypedcl == (INode*)cstrTypeDcl;
    if (toptr && fromtype->tag == ArrayRefTag
        && itypeIsSame(((RefNode*)fromtype)->vtexp, ((StarNode*)totypedcl)->vtexp)) {
        errorMsgNode(from, ErrorCPtrConv,
            "A slice is an address and a count, not a pointer. Take its address explicitly, 'xs as *%s'; a C function that takes a string is declared with 'cstr'.",
            itypeName(((StarNode*)totypedcl)->vtexp));
        return 1;
    }
    if (toptr && iexpIsTextRef(fromtype)) {
        errorMsgNode(from, ErrorCPtrConv,
            "Text is not a pointer. A C function that takes a string is declared with 'cstr', which a literal or a String's 'cstr()' is; for the bytes alone, 's as &Array[u8]' and then 'as *u8'.");
        return 1;
    }
    if (toptr && fromtype == (INode*)cstrTypeDcl) {
        errorMsgNode(from, ErrorCPtrConv,
            "A cstr is not a raw pointer. Its pointer is 'c.ptr()'.");
        return 1;
    }
    if (tocstr && fromtype->tag == PtrTag) {
        errorMsgNode(from, ErrorCPtrConv,
            "A raw pointer is no cstr, which promises a NUL after its bytes. 'cstr.fromPtr(p)' says that this pointer's bytes end in one.");
        return 1;
    }
    if (tocstr && (iexpIsTextRef(fromtype) || fromtype->tag == ArrayRefTag)) {
        errorMsgNode(from, ErrorCPtrConv,
            "Text or a slice promises no NUL after its bytes, so it is no cstr. A literal is one, and so is a String's 'cstr()': 'String.from(s).cstr()' copies this.");
        return 1;
    }
    return 0;
}

int iexpCharNumberMismatch(INode *from, INode *totypedcl) {
    INode *fromtype = iexpGetTypeDcl(from);
    // A number here is any but bool, which has its own message, and char
    int tonumber = isNbr(totypedcl) && totypedcl != (INode*)boolType && totypedcl != (INode*)charType;
    int fromnumber = isNbr(fromtype) && fromtype != (INode*)boolType && fromtype != (INode*)charType;
    if (litCharRefusedAsByte(from, totypedcl)) {
        errorMsgNode(from, ErrorCharNotNbr,
            "This character literal is U+%04" PRIX64 ", beyond ASCII, so it is no u8: a byte of its UTF-8 is not the code point. It is a char; convert it explicitly, 'u8.from(c)', for its low 8 bits, or write its bytes in a string.",
            (uint64_t)((ULitNode*)from)->uintlit);
        return 1;
    }
    if (fromtype == (INode*)charType && tonumber) {
        errorMsgNode(from, ErrorCharNotNbr,
            "A char is not a number, and %s is wanted here. Convert it explicitly, '%s.from(c)'.",
            itypeName(totypedcl), itypeName(totypedcl));
        return 1;
    }
    if (totypedcl == (INode*)charType && fromnumber) {
        errorMsgNode(from, ErrorCharNotNbr,
            "%s is not a char, and a char is wanted here. Convert it explicitly, 'char.from(n)'.",
            itypeName(fromtype));
        return 1;
    }
    return 0;
}

// Coerce from-node's type to 'to' expected type, if needed
// Return 1 if type "matches", 0 otherwise. A value meeting a type also meets
// its invariant lifetimes, which must be the same brands (lifeBrandsCoerce):
// so a key never reaches where another arena's is wanted, nor loses its brand.
int iexpCoerce(INode **from, INode *totype) {
    INode *fromtype = isExpNode(*from) ? ((IExpNode *)*from)->vtype : NULL;
    INode *at = *from;
    if (!iexpCoerceShape(from, totype))
        return 0;
    // A brand mismatch is reported there; the shape matching keeps the errors
    // from multiplying
    if (lifeInvariantSeen && fromtype)
        lifeBrandsCoerce(fromtype, totype, at);
    return 1;
}

static int iexpCoerceShape(INode **from, INode *totype) {
    // From should be a typed expression node
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorInvType, "An expression value is expected here.");
        return 0;
    }
    IExpNode *fromnode = (IExpNode *)*from;

    // A 'null' takes the raw pointer type it is wanted as, and is refused
    // wanted as anything else, or as nothing in particular
    if (litAdoptNullType(from, totype))
        return 1;

    // No need to do coercion, if no expected type
    if (totype == unknownType || totype == noCareType)
        return 1;

    INode *totypedcl = itypeGetTypeDcl(totype);

    // An untyped integer literal still on its i32 default -- an argument to an
    // overload set, a generic or an operator is type checked before its callee
    // is chosen, so litTypeCheck had no expected type to give it -- takes the
    // number type it is wanted as rather
    // than being converted to it, which would build the constant at 32 bits
    // first. bool is refused by litAdoptNumberType itself, so a literal meets it
    // as any number does, through isTrue.
    if (litAdoptNumberType(from, totypedcl))
        return 1;

    // An ASCII character literal, as written, wanted as a u8 is that byte
    if (litAdoptCharAsByte(from, totypedcl))
        return 1;

    // A string literal fills a byte array of its length, or is copied into an
    // owner of 'str'
    if (slitCoerce(from, totypedcl))
        return 1;

    // A string literal lent as a temporary is made by iexpCoerceIn, which has
    // the type check state its borrow needs. Reached without it, it is not met.
    if (slitBorrowMatches(*from, totypedcl))
        return 0;

    // Are types equivalent, or is 'to' a subtype of fromtypedcl?
    switch (iexpMatches(from, totypedcl, Coercion)) {
    case NoMatch:
        // An array literal is typed from its elements, so it may still reach
        // the wanted array type by coercing each element to its element type
        if ((*from)->tag == ArrayLitTag)
            return arrayLitCoerce((ArrayNode*)*from, totypedcl);
        // So may a value tuple, each value to its element's type
        if ((*from)->tag == VTupleTag)
            return vtupleCoerce((TupleNode*)*from, totypedcl);
        // So may a borrowed constant one, and the borrow then match as it is
        if (borrowConstLitCoerce(*from, totypedcl))
            return iexpMatches(from, totypedcl, Coercion) != NoMatch && iexpCoerce(from, totype);
        // A bool wanted as a number is refused here, naming the conversion that
        // would say what it means. The conversion is then built anyway, so what
        // uses the value sees the type it wanted and says nothing more.
        if (iexpGetTypeDcl(*from) == (INode*)boolType && isNbr(totypedcl)) {
            errorMsgNode(*from, ErrorBoolNotNbr,
                "A bool is not a number, and %s is wanted here. Convert it explicitly, '%s.from(b)', which gives 0 or 1.",
                itypeName(totypedcl), itypeName(totypedcl));
            INode *conv = (INode*)newConvCastNode(*from, totypedcl);
            inodeLexCopy(conv, *from);
            *from = conv;
            return 1;
        }
        // A char is no number, nor a number a char (a non-ASCII character
        // literal wanted as a u8 included): the conversion is asked for. It is
        // then built anyway, as for a bool, so what uses the value says nothing more.
        if (iexpCharNumberMismatch(*from, totypedcl) || iexpCPtrMismatch(*from, totypedcl)
            || slitBorrowRefused(*from, totypedcl)) {
            INode *conv = (INode*)newConvCastNode(*from, totypedcl);
            inodeLexCopy(conv, *from);
            *from = conv;
            return 1;
        }
        return 0;
    case EqMatch:
        // A '&uni' wanted as a borrowed reference, a '&uni' too, is borrowed
        // from (a fresh, shorter loan), not handed over
        if (borrowUniReborrows(*from, totypedcl))
            borrowUniReborrow(from, totypedcl);
        return 1;
    case CastSubtype: {
        if ((totypedcl->tag == RefTag || totypedcl->tag == VirtRefTag) && ((RefNode*)totypedcl)->region == borrowRef)
            borrowOwnerLendRefused(*from, ((RefNode*)totypedcl)->perm);
        // A sole owner wanted as a '&uni' borrowed reference is borrowed from,
        // as it is when wanted as a '&' or '&mut', not moved into the borrow
        if (borrowOwnerLendsUni(*from, totypedcl)) {
            borrowOwnerLend(from, totypedcl);
            return iexpCoerce(from, totype);
        }
        INode *newfrom = (INode*)newRecastNode(*from, iexpCoerceType(*from, totypedcl));
        inodeLexCopy(newfrom, *from);
        *from = newfrom;
        return 1;
    }
    case ConvSubtype: {
        if (totypedcl->tag == VirtRefTag && ((RefNode*)totypedcl)->region == borrowRef)
            borrowOwnerLendRefused(*from, ((RefNode*)totypedcl)->perm);
        // A float literal widened stays a literal, which the positions that
        // require one ask for, and so does a named constant's use
        if (litWidenFloat(from, totypedcl) || litWidenConst(from, totypedcl))
            return 1;
        // Refused in GPU code; the conversion is built anyway, so what uses the
        // value says nothing more
        if (!closureGpuVirtRefused(*from, totypedcl) && flowGpu && totypedcl->tag == VirtRefTag
            && isExpNode(*from) && iexpGetTypeDcl(*from)->tag == RefTag)
            errorMsgNode(*from, ErrorGpuUnavailable,
                "In GPU code a reference to %s cannot be made a virtual reference to %s: it would be called through a table of code pointers, which a GPU has none of. Give the value to a function generic over a trait, '[S Shape]', which is inlined.",
                itypeName(((RefNode*)iexpGetTypeDcl(*from))->vtexp), itypeName(((RefNode*)totypedcl)->vtexp));
        INode *newfrom = (INode*)newConvCastNode(*from, iexpCoerceType(*from, totypedcl));
        inodeLexCopy(newfrom, *from);
        *from = newfrom;
        return 1;
    }
    case ConvByMeth: 
    {
        // The injected call takes the operand's position before it is lowered,
        // because lowering is what reports a private or unmatched isTrue
        FnCallNode *istrue = newFnCallLower(*from, *from, 1);
        istrue->methfld = (INode *)newMemberUseNode(istrueName);
        inodeLexCopy(istrue->methfld, *from);
        int success;
        if (iexpGetTypeDcl(*from)->tag == PtrTag)
            success = fnCallLowerPtrMethod(istrue, ptrType);
        else
            success = fnCallLowerMethod(NULL, istrue) == 1;
        if (success) {
            *from = (INode*)istrue;
            return 1;
        }
        else
            return 0;
    }
    case ConvBorrow:
    {
        borrowAuto(from, totypedcl);
        return 1;
    }
    default: {
        return 0;
    }
    }
}

// Perform full type check on from-node and ensure it is an expression.
// Then coerce from-node's type to 'to' expected type, if needed
// Return 1 if type "matches", 0 otherwise
int iexpTypeCheckCoerce(TypeCheckState *pstate, INode *totype, INode **from) {
    inodeTypeCheck(pstate, from, totype);
    return iexpCheckedCoerceIn(pstate, totype, from);
}

// iexpCoerce where the type check state is at hand. A string literal wanted as
// a read-only borrow of a type that declares 'fromLiteral' is lent as a
// temporary of it (slitBorrowCoerce), and that is made only in a function's
// body (scope 2 and up): a temporary lasts to the end of its statement, which a
// global's initializer, a constant, a field's or a parameter's default has no
// statement to give. Anything else is iexpCoerce.
int iexpCoerceIn(TypeCheckState *pstate, INode **from, INode *totype) {
    if (pstate && pstate->scope >= 2 && totype != unknownType && totype != noCareType
        && slitBorrowMatches(*from, itypeGetTypeDcl(totype)))
        return slitBorrowCoerce(pstate, from, totype);
    return iexpCoerce(from, totype);
}

// iexpTypeCheckCoerce for a node already type checked: ensure it is an
// expression, then coerce it to the expected type, if needed
int iexpCheckedCoerceIn(TypeCheckState *pstate, INode *totype, INode **from) {
    if (totype == noCareType)
        return 1;
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 1; // pretend we match to not provoke additional errors
    }
    return iexpCoerceIn(pstate, from, totype);
}

int iexpCheckedCoerce(INode *totype, INode **from) {
    return iexpCheckedCoerceIn(NULL, totype, from);
}

// Used by 'if' and 'loop'/break to infer the type in common across all branches,
// one branch at a time. Errors on bad type match and returns Match condition.
// - expectType is the final type expected by receiver
// - maybeType is the inferred type in common
// - from is the current branch whose type is being examined
int iexpMultiInfer(INode *expectType, INode **maybeType, INode **from) {
    INode *fromType = iexpGetTypeDcl(*from);
    // If the type does not matter, we are good with anything
    if (expectType == noCareType)
        return EqMatch;

    // A branch already reported as bad contributes nothing to the type in
    // common, and must not be compared against the branches that are still good
    if (fromType == errorType)
        return EqMatch;

    // when we need a specific type, but don't care which one,
    // we need to find the type in common between this and previous branches
    if (expectType == unknownType) {
        // A type carrying brands is kept as the use that names them: the
        // declaration names none, and the branches must agree on them
        INode *fromUse = ((IExpNode *)*from)->vtype;
        if ((*maybeType) == unknownType) {
            *maybeType = lifeTypeHasBrands(fromUse) ? fromUse : fromType;  // First branch
            return EqMatch;
        }
        else {
            if (itypeIsSame(*maybeType, fromType)) {
                if (lifeInvariantSeen && !lifeBrandsCoerce(fromUse, *maybeType, *from))
                    return NoMatch;
                return EqMatch;
            }
            // Try to find some supertype exists between the two types
            INode *superType = itypeFindSuper(*maybeType, fromType);
            if (superType) {
                *maybeType = superType;
                return ConvSubtype;
            }
            else {
                if (closureInferring)
                    errorMsgNode(*from, ErrorClosureRet,
                        "The paths of this closure give different types, so its return type cannot be read off them: write the closure's return type, as in 'fn (x i32) i32 { ... }'.");
                else
                    errorMsgNode(*from, ErrorInvType, "Branch's expression type inconsistent with other branches.");
                return NoMatch;
            }
        }
    }

    // When we have an expected type, ensure this branch matches
    TypeCompare match = iexpMatches(from, expectType, Coercion);
    if (match == NoMatch) {
        errorMsgNode(*from, ErrorInvType, "Expression type does not match expected type.");
        return NoMatch;
    }
    // Still figure out the type in common among branches
    if ((*maybeType) == unknownType) {
        *maybeType = fromType;  // First branch
        return match;
    }
    else {
        if (!itypeIsSame(*maybeType, fromType))
            *maybeType = expectType; // Set type-in-common as expected supertype
        return match;
    }
}

// Used by 'if' and 'block/loop'/break to infer the type in common across all branches,
// one branch at a time. Errors on bad type match and returns Match condition.
// - expectType is the final type expected by receiver (or else NoCareType)
// - inferredType is the inferred supertype in common
// - fromexp is the current expression node whose type is being examined
// - oldMatch is the current match status on whether all branches match or not
int iexpMultiCoerceInfer(TypeCheckState *pstate, INode *expectType, INode **inferredType, INode **fromexp, int oldMatch) {
    inodeTypeCheck(pstate, fromexp, expectType);
    return iexpMultiCheckedCoerceInfer(expectType, inferredType, fromexp, oldMatch);
}

// iexpMultiCoerceInfer for an expression already type checked
int iexpMultiCheckedCoerceInfer(INode *expectType, INode **inferredType, INode **fromexp, int oldMatch) {
    if (!iexpCheckedCoerce(expectType, fromexp)) {
        errorMsgNode(*fromexp, ErrorInvType, "Expression does not match expected type.");
        return NoMatch;
    }
    else if (!isExpNode(*fromexp)) {
        return NoMatch;
    }
    else {
        switch (iexpMultiInfer(expectType, inferredType, fromexp)) {
        case NoMatch:
            return NoMatch;
        case CastSubtype:
            return oldMatch == NoMatch? NoMatch : CastSubtype;
        case ConvSubtype:
            return oldMatch == NoMatch? NoMatch : ConvSubtype;
        default:
            return oldMatch;
        }
    }
}

// Ensure it is a lval, return error and 0 if not.
int iexpIsLval(INode *lval) {
    if (isNameUseNode(lval) && isExpNode(lval))
        return 1;
    switch (lval->tag) {
    case StringLitTag:
    case DerefTag:
        return 1;
    case ArrIndexTag:
        return iexpIsLval(((FnCallNode *)lval)->objfn);
    case FldAccessTag:
        return iexpIsLval(((FnCallNode *)lval)->objfn);
    case FnCallTag:
        // A call already reported as bad is not reported again as no place
        return ((IExpNode *)lval)->vtype == errorType;
    case CastTag:
        // An owner of 'Array[T]' lent as the slice it is indexed through
        // (fnCallBodyAsSlice) is the owner's place
        if (!(lval->flags & FlagConvert) && iexpGetTypeDcl(lval)->tag == ArrayRefTag
            && iexpGetTypeDcl(((CastNode *)lval)->exp)->tag == RefTag)
            return iexpIsLval(((CastNode *)lval)->exp);
        return 0;
    default:
        return 0;
    }
}

// Ensure it is a lval, return error and 0 if not.
int iexpIsLvalError(INode *lval) {
    if (iexpIsLval(lval))
        return 1;
    errorMsgNode(lval, ErrorBadLval, "Expression must be lval");
    return 0;
}

// A place reached through the borrowed reference 'refexp' lives as long as the
// reference's lifetime says, not as long as whatever holds the reference: what a
// borrowed parameter points at is the caller's however it is reached. The
// lifetime is on the reference's type where the reference is held in no
// variable -- a borrow expression or a call's result, typed by borrowTypeCheck or
// fnCallFinalizeArgs -- and where it is the whole value of a variable whose type
// says what it holds: a parameter's type carries the caller band, and nothing
// shorter may be stored into it; an immutable local's carries its initializer's
// (varDclTypeCheck). A mutable local may since have been given a borrow its type
// does not record, so a store through one keeps the scope of the variable holding
// it, and so does a place reached through one held in a field or an element,
// which carries no lifetime of its own (the field's declared type is shared). A
// borrow of a place reached through a mutable local reads the lifetime its type
// records, as the local itself does when it is returned or handed on: a
// borrow it was later given is followed by the loan walk (flowloan.c), which
// refuses it returned past what it was borrowed from. A place reached through
// an owning reference lives as long as the owner's holder.
static void iexpScopeThroughRef(INode *refexp, INode *lvalvar, RefNode *reftype, uint16_t *scope, int stored) {
    if (reftype->region != borrowRef)
        return;
    if (lvalvar == NULL)
        *scope = reftype->scope;
    else if (isNameUseNode(refexp) && lvalvar->tag == VarDclTag) {
        VarDclNode *var = (VarDclNode *)lvalvar;
        if (var->scope == 1 || !(permGetFlags(var->perm) & MayWrite) || (!stored && reftype->scope > 0))
            *scope = reftype->scope;
    }
}

// Is this place reached through a borrowed reference, a slice or a virtual
// reference: the value it holds is where a loan points, and what is lent of it
// is held to that loan's permission?
int iexpPathThroughBorrow(INode *lval) {
    for (;;) {
        INode *obj;
        if (lval->tag == FldAccessTag || lval->tag == ArrIndexTag)
            obj = ((FnCallNode *)lval)->objfn;
        else if (lval->tag == DerefTag)
            obj = ((StarNode *)lval)->vtexp;
        else
            return 0;
        if (!isExpNode(obj))
            return 0;
        INode *objtype = iexpGetTypeDcl(obj);
        if (((objtype->tag == RefTag || objtype->tag == ArrayRefTag) && itypeGetTypeDcl(((RefNode*)objtype)->region) == (INode*)borrowRef)
            || objtype->tag == VirtRefTag)
            return 1;
        lval = obj;
    }
}

// Extract lval variable, scope and overall permission from lval, for a borrow
// of the place ('stored' 0) or a store into it ('stored' 1). The two differ only
// at a parameter.
static INode *iexpLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope, int stored) {
    // A variable or named function node
    if (isNameUseNode(lval) && isExpNode(lval)) {
        INode *lvalvar = ((NameUseNode *)lval)->dclnode;
        if (lvalvar->tag == VarDclTag) {
            VarDclNode *var = (VarDclNode *)lvalvar;
            *lvalperm = var->perm;
            // A parameter (scope 1) is the function's own storage, gone at its
            // return like a local of its top block: a borrow of it, of 'self' by
            // value or of what an owner passed by value owns lives in that block
            // (2), usable anywhere in the function and never handed back. Only
            // what a borrowed parameter points at is the caller's, reached
            // through it (iexpScopeThroughRef). A store into a parameter, or
            // into a part of it held by value, is held to the caller band all
            // the same: what is stored can be read back out with the lifetime
            // its type gives -- a borrowed parameter's is the caller band, and
            // a field's carries none yet -- so nothing shorter-lived goes there.
            *scope = (var->scope == 1 && !stored) ? 2 : var->scope;
        }
        else {
            *lvalperm = (INode*)opaqPerm; // Function
            *scope = 0;
        }
        return lvalvar;
    }
    switch (lval->tag) {
    case DerefTag:
    {
        INode *refexp = ((StarNode *)lval)->vtexp;
        INode *lvalvar = iexpLvalInfo(refexp, lvalperm, scope, stored);
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(refexp);
        if (vtype->tag == RefTag || vtype->tag == ArrayRefTag) {
            // An owner held in a place reached through a borrow that does not
            // hold the place alone ('ro', 'imm', or 'mut', which others share)
            // lends no more than that borrow lets: its own permission is what
            // it grants a holder of the place, not one reading through a loan.
            // So an owner reached through a '&mut' lends '&mut' at most, never
            // '&uni' or '&imm': the shared path may be used to replace the
            // owner and end what was lent.
            int ownerShared = vtype->region != borrowRef && !permIsLock(*lvalperm) && !permIsLock(vtype->perm)
                && (permGetFlags(vtype->perm) & MayWrite)
                && itypeGetTypeDcl(*lvalperm) != (INode*)uniPerm && itypeGetTypeDcl(*lvalperm) != (INode*)newPerm
                && iexpPathThroughBorrow(refexp);
            if (!ownerShared)
                *lvalperm = vtype->perm;
            iexpScopeThroughRef(refexp, lvalvar, vtype, scope, stored);
        }
        else if (vtype->tag == PtrTag)
            *lvalperm = (INode*)mutPerm;
        return lvalvar;
    }

    // Array element (obj[2])
    case ArrIndexTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        // flowLoadValue(fstate, nodesFind(element->args, 0), 0);
        INode *lvalvar = iexpLvalInfo(element->objfn, lvalperm, scope, stored);
        INode *objtype = iexpGetTypeDcl(element->objfn);
        // Indexing through any reference takes the permission from the
        // reference, exactly as DerefTag does. RefTag belongs here because a
        // reference to a fixed-size array is indexed without an explicit
        // dereference; leaving it out took the permission of the variable
        // holding the reference instead, so 'v[0] = x' on a '&mut Array[i32, 3]'
        // parameter was refused while '(*v)[0] = x' was allowed.
        if (objtype->tag == ArrayRefTag || objtype->tag == RefTag) {
            *lvalperm = ((RefNode*)objtype)->perm;
            iexpScopeThroughRef(element->objfn, lvalvar, (RefNode*)objtype, scope, stored);
        }
        else if (objtype->tag == PtrTag)
            *lvalperm = (INode*)mutPerm;
        return lvalvar;
    }

    // Field access (obj.prop)
    case FldAccessTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        // A field of a place with no variable at its root -- reached through
        // the reference a call returned -- still takes the permission and the
        // lifetime the steps below give it; only the variable is absent.
        INode *lvalvar = iexpLvalInfo(element->objfn, lvalperm, scope, stored);
        // A field reached through a virtual reference takes the permission from
        // the reference, exactly as DerefTag does for a plain one. It has to be
        // done here because no dereference is injected for a virtual reference:
        // derefInject rewrites only RefTag and PtrTag receivers, so the field
        // access keeps the virtual reference as its objfn and the permission
        // would otherwise be that of the binding holding it.
        RefNode *objtype = (RefNode*)iexpGetTypeDcl(element->objfn);
        if (objtype->tag == VirtRefTag) {
            *lvalperm = objtype->perm;
            iexpScopeThroughRef(element->objfn, lvalvar, objtype, scope, stored);
        }
        // Downgrade overall static permission if the field may not be written.
        // Ask the permission for its flags rather than comparing node pointers:
        // a permission written in source is a name use wrapping the singleton,
        // so only a compiler-synthesized field would ever match by identity.
        // A tuple element is reached by index rather than by name, so it has no
        // field declaration to ask.
        if (isNameUseNode(element->methfld)) {
            INode *flddcl = ((NameUseNode *)element->methfld)->dclnode;
            if (flddcl->tag == FieldDclTag) {
                // A field whose declaration wrote no permission keeps
                // unknownType: parseFieldDcl takes a default permission and
                // never applies it. Unwrap and check for a real one rather than
                // handing that to permGetFlags, whose assert that it was given a
                // permission is nothing in a Release build -- it would read a
                // flag word out of an AbsenceNode. Which permissions a field may
                // carry, and what an unwritten one should mean, are open in
                // workitems/permissions.md.
                INode *fldperm = itypeGetTypeDcl(((FieldDclNode*)flddcl)->perm);
                if (fldperm->tag == PermTag && !(permGetFlags(fldperm) & MayWrite))
                    *lvalperm = (INode*)roPerm;
            }
        }
        return lvalvar;
    }

    // No other node is an lval
    default:
        return NULL;
    }
}

// Extract lval variable, scope and overall permission from lval. The scope is
// the lifetime a borrow of the place has.
INode *iexpGetLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope) {
    return iexpLvalInfo(lval, lvalperm, scope, 0);
}

// The same for a place stored into, whose scope is the lifetime a borrowed
// reference stored there must have at least
INode *iexpGetStoreLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope) {
    return iexpLvalInfo(lval, lvalperm, scope, 1);
}

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from) {
    if (!itypeIsSame(iexpGetTypeDcl(to), iexpGetTypeDcl(*from)))
        return 0;
    // The same type carries the same brands: a swap of two arenas would hand
    // each one's keys the other
    if (lifeInvariantSeen && isExpNode(to) && isExpNode(*from))
        lifeBrandsCoerce(((IExpNode*)*from)->vtype, ((IExpNode*)to)->vtype, *from);
    return 1;
}

// Return true if value uses move semantics
int iexpIsMove(INode *node) {
    return itypeIsMove(((IExpNode *)node)->vtype);
}