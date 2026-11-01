/** Handling for gennode declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

// Hook every parameter, then resolve what follows each: a bound may name a
// parameter declared after it, '[S Stack[T], T]'
void genericParmsNameRes(NameResState *pstate, Nodes *parms) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(parms, cnt, nodesp)) {
        GenVarDclNode *parm = (GenVarDclNode *)*nodesp;
        nametblHookNode(parm->namesym, *nodesp);
    }
    for (nodesFor(parms, cnt, nodesp)) {
        GenVarDclNode *parm = (GenVarDclNode *)*nodesp;
        if (parm->annot == NULL)
            continue;
        INode **annotp;
        uint32_t annotcnt;
        for (nodesFor(parm->annot, annotcnt, annotp))
            inodeNameRes(pstate, annotp);
    }
}

// Create a new generic info block
GenericInfo *newGenericInfo() {
    GenericInfo *geninfo = (GenericInfo*)memAllocBlk(sizeof(GenericInfo));
    geninfo->parms = NULL;
    geninfo->memonodes = NULL;
    geninfo->where = NULL;
    return geninfo;
}

// Serialize
void genericInfoPrint(GenericInfo *info) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    for (nodesFor(info->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint("] ");
}

// Inference found an argtype that maps to a generic parmtype
// Capture it, and return 0 if it does not match what we already thought it was.
// An argument is held as a use of its type, as one written is. A value's type
// may be the struct's declaration itself -- a block's or an 'if''s value types
// as the declaration its branches agree on (iexpMultiInfer) -- and a declaration
// held as an argument would be cloned into the instance as a second copy of the
// type, methods and all, instead of named by it.
int genericCaptureType(FnCallNode *gencall, Nodes *genparms, INode *parmtype, INode *argtype) {
    INode **genvarp;
    uint32_t genvarcnt;
    INode **genargp = &nodesGet(gencall->args, 0);
    for (nodesFor(genparms, genvarcnt, genvarp)) {
        Name *genvarname = ((GenVarDclNode *)(*genvarp))->namesym;
        // Found parameter with corresponding name? Capture/check type. A value
        // parameter is no type: what it names is a number, captured from an
        // array's size (genericCaptureValue), never from a type
        if (genvarname == ((NameUseNode*)parmtype)->namesym) {
            if (((GenVarDclNode *)(*genvarp))->valtype)
                break;
            if (*genargp == NULL)
                *genargp = argtype->tag == StructTag ? newNameUseFromDclNode(argtype, (INode*)gencall) : argtype;
            else if (!itypeIsSame(*genargp, argtype))
                return 0;
            break;
        }
        ++genargp;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Value parameters, '[N usize]': a number the instance is made for. N stands
// for an integer literal wherever it is used, as a type parameter stands for a
// type, and only as itself: an array's size, 'Array[T, N]', or a value in the
// body. Nothing is computed from it in a type.

// The value parameter a node is a use of, or NULL
GenVarDclNode *genericValueParmOf(INode *node) {
    if (node == NULL || !isNameUseNode(node))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)node);
    if (dcl == NULL || dcl->tag != GenVarDclTag || ((GenVarDclNode*)dcl)->valtype == NULL)
        return NULL;
    return (GenVarDclNode*)dcl;
}

// Does this size, as written, compute something from a value parameter? It
// does when a parameter is used in it beside anything else
int genericMentionsValueParm(INode *node) {
    if (node == NULL)
        return 0;
    if (isNameUseNode(node))
        return genericValueParmOf(node) != NULL;
    INode **nodesp;
    uint32_t cnt;
    switch (node->tag) {
    case FnCallTag: {
        FnCallNode *call = (FnCallNode*)node;
        if (genericMentionsValueParm(call->objfn))
            return 1;
        if (call->args)
            for (nodesFor(call->args, cnt, nodesp))
                if (genericMentionsValueParm(*nodesp))
                    return 1;
        return 0;
    }
    case CastTag:
        return genericMentionsValueParm(((CastNode*)node)->exp);
    case VTupleTag:
        for (nodesFor(((TupleNode*)node)->elems, cnt, nodesp))
            if (genericMentionsValueParm(*nodesp))
                return 1;
        return 0;
    case OrLogicTag:
    case AndLogicTag:
        return genericMentionsValueParm(((LogicNode*)node)->lexp)
            || genericMentionsValueParm(((LogicNode*)node)->rexp);
    case NotLogicTag:
        return genericMentionsValueParm(((LogicNode*)node)->lexp);
    default:
        return 0;
    }
}

// Does the integer type hold this value?
static int genericValueFits(uint64_t value, INode *valtype) {
    NbrNode *nbr = (NbrNode*)itypeGetTypeDcl(valtype);
    if (nbr->bits >= 64)
        return nbr->tag == UintNbrTag || value <= (uint64_t)INT64_MAX;
    if (nbr->tag == IntNbrTag)
        return value <= (((uint64_t)1 << (nbr->bits - 1)) - 1);
    return value <= (((uint64_t)1 << nbr->bits) - 1);
}

// The argument for value parameter 'parm': a literal of the parameter's type,
// built whole (checked, with no suffix or sign left to read). NULL once
// reported, at 'errnode'.
static INode *genericValueArg(GenVarDclNode *parm, uint64_t value, INode *errnode) {
    if (!genericValueFits(value, parm->valtype)) {
        errorMsgNode(errnode, ErrorGenValueArg,
            "The value parameter %s is a %s, and %" PRIu64 " does not fit it.",
            &parm->namesym->namestr, &((NbrNode*)itypeGetTypeDcl(parm->valtype))->namesym->namestr, value);
        return NULL;
    }
    ULitNode *lit = newULitNodeTC(value, itypeGetTypeDcl(parm->valtype));
    inodeLexCopy((INode*)lit, errnode);
    return (INode*)lit;
}

// A written argument for a value parameter, 'dot[3]': an integer literal, with
// no minus, of the parameter's type or untyped. Answers the argument as the
// instance holds it, or NULL once reported.
static INode *genericWrittenValueArg(GenVarDclNode *parm, INode *arg) {
    char *tname = &((NbrNode*)itypeGetTypeDcl(parm->valtype))->namesym->namestr;
    if (arg->tag != ULitTag) {
        errorMsgNode(arg, ErrorGenValueArg,
            "The value parameter %s is a %s, so its argument is an integer written as a number, like 3. (A size is decided when the program is compiled, so it is no variable.)",
            &parm->namesym->namestr, tname);
        return NULL;
    }
    ULitNode *lit = (ULitNode*)arg;
    if (lit->flags & FlagLitNeg) {
        errorMsgNode(arg, ErrorGenValueArg,
            "The value parameter %s is a %s, and its argument may not be negative.",
            &parm->namesym->namestr, tname);
        return NULL;
    }
    if (!(lit->flags & FlagUnkType) && itypeGetTypeDcl(lit->vtype) != itypeGetTypeDcl(parm->valtype)) {
        errorMsgNode(arg, ErrorGenValueArg,
            "The value parameter %s is a %s, and this literal is a %s.",
            &parm->namesym->namestr, tname, &((NbrNode*)itypeGetTypeDcl(lit->vtype))->namesym->namestr);
        return NULL;
    }
    return genericValueArg(parm, lit->uintlit, arg);
}

// Is an argument of the instance the same as another? A type by its identity, a
// number by its value
static int genericArgSame(INode *a, INode *b) {
    if (a->tag == ULitTag || b->tag == ULitTag)
        return a->tag == b->tag && ((ULitNode*)a)->uintlit == ((ULitNode*)b)->uintlit
            && itypeGetTypeDcl(((ULitNode*)a)->vtype) == itypeGetTypeDcl(((ULitNode*)b)->vtype);
    return itypeIsSame(a, b);
}

// Set once a capture has reported its own diagnostic, so its caller does not
// add the general one
static int genericReported = 0;

// Set while a generic's result type is matched against the type expected of it
// (genericInferFromExpected). That match only fills type arguments nothing has
// said yet, so an argument it disagrees with is passed over, the rest still
// matched, and the call's own check reports the disagreement.
static int genericExpectMode = 0;

// A value parameter's number, found in an argument's type (an array's size)
// while inferring: its argument, if none yet, otherwise agreeing with it.
// Returns 0, reported, where it does not.
static int genericCaptureValue(FnCallNode *gencall, Nodes *genparms, GenVarDclNode *parm, uint64_t value) {
    INode **genvarp;
    uint32_t genvarcnt;
    INode **genargp = &nodesGet(gencall->args, 0);
    for (nodesFor(genparms, genvarcnt, genvarp)) {
        if (*genvarp != (INode*)parm) {
            ++genargp;
            continue;
        }
        if (*genargp == NULL) {
            *genargp = genericValueArg(parm, value, (INode*)gencall);
            if (*genargp == NULL) {
                genericReported = 1;
                return 0;
            }
        }
        else if (((ULitNode*)*genargp)->uintlit != value) {
            errorMsgNode((INode*)gencall, ErrorGenValueClash,
                "The value parameter %s is %" PRIu64 " by an earlier argument and %" PRIu64 " by another: the arguments must agree on it.",
                &parm->namesym->namestr, ((ULitNode*)*genargp)->uintlit, value);
            genericReported = 1;
            return 0;
        }
        break;
    }
    return 1;
}

static int genericArgIsAdaptable(INode *arg, INode *parmtype);
static INode *genericCapturedType(FnCallNode *gencall, Nodes *genparms, INode *parmtype);
static int genericInferType(FnCallNode *inferredgencall, Nodes *genparms, INode *parmtype, INode *argtype);

// The value a type literal gives a field: the value itself, or the one a name
// given it holds ('Pair[a: 1, b: 2]')
static INode **genericStructValue(INode **argp) {
    return (*argp)->tag == NamedValTag ? &((NamedValNode *)*argp)->val : argp;
}

// Infer generic type parameters (inferredgencall) from the type literal arguments (srcgencallp).
// An unsuffixed integer literal for a field that is a bare type parameter is left
// to genericInferStructLiterals, once the other values and the expected type
// have had their say, as a function's argument is (genericInferFnParms).
int genericInferStructParms(TypeCheckState *pstate, Nodes *genparms, StructNode *genstruct,
        FnCallNode *srcgencall, FnCallNode *inferredgencall) {

    // Reorder the literal's arguments to match the type's field order
    if (typeLitStructReorder(srcgencall, genstruct,
            (INode*)genstruct == pstate->typenode || structSeesPrivate(pstate, (INode*)genstruct)) == 0)
        return 0;

    // Iterate through arguments and expected parms
    int retcode = 1;
    uint32_t cnt;
    INode **argsp;
    INode **parmp = &nodelistGet(&genstruct->fields, 0);
    for (nodesFor(srcgencall->args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((FieldDclNode *)*argsp)->vtype;
        // If type of expected parm is a generic variable, capture type of corresponding argument
        // A 'null' says nothing of which pointer type it is (genericInferFnParms)
        if (nameUseNames(parmtype, GenVarDclTag) && !litIsUntypedNull(*argsp)
            && !genericArgIsAdaptable(*genericStructValue(argsp), parmtype)
            && genericCaptureType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic type");
            retcode = 0;
        }
        genericReported = 0;
        ++parmp;
    }
    return retcode;
}

// What is left to the type literal's integer literals, after genericInferStructParms:
// a literal names its field's type, i32, only if nothing did. Otherwise it is
// converted to that type, as an integer literal is wherever a number type is wanted.
static int genericInferStructLiterals(Nodes *genparms, StructNode *genstruct, FnCallNode *srcgencall,
        FnCallNode *inferredgencall) {
    int retcode = 1;
    uint32_t cnt;
    INode **argsp;
    INode **parmp = &nodelistGet(&genstruct->fields, 0);
    for (nodesFor(srcgencall->args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode **valp = genericStructValue(argsp);
        if (genericArgIsAdaptable(*valp, parmtype)) {
            INode *given = genericCapturedType(inferredgencall, genparms, parmtype);
            if (given == NULL)
                genericInferType(inferredgencall, genparms, parmtype, ((IExpNode *)*valp)->vtype);
            else if (!itypeIsSame(given, ((IExpNode *)*valp)->vtype) && iexpMatches(valp, given, Coercion) == NoMatch) {
                errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic type");
                retcode = 0;
            }
        }
        ++parmp;
    }
    return retcode;
}

// Match a parameter's declared type against its argument's type, capturing each
// type parameter it names. A type parameter matches the argument's type whole;
// a pointer, reference or array reference to one matches an argument of the
// same kind, the type parameter taking what that argument points at, so
// 'p *T' given a '*Fin' infers Fin; an array slice parameter also matches
// the fixed-size array, or reference to one, that a call converts to a slice;
// a function signature, 'f &fn(a: A) R', matches the referenced function's,
// each parameter type and then the return type, so '&triangle' infers both;
// and 'List[T]' matches an instance of List, type argument by type argument.
// A managed reference type, 'Rc[mut, T]', is a call until type check lowers
// it, and matches a reference, its last argument against what the reference
// points at. In a template a type parameter is not yet
// a type, so '*T' is held as a dereference and '&T' or '&Array[T]' as a borrow
// (cloneStarNode, cloneRefNode), and each spelling is accepted here. Region
// and permission take no part, but for a region that is itself a type
// parameter, 'R[mut, T]', which takes the argument's region: the instance's
// own check of the call judges them. Any other shape infers nothing, and
// returns 1 as a non-match does.
// Returns 0 only when a type parameter is given two different types.
static int genericInferType(FnCallNode *inferredgencall, Nodes *genparms, INode *parmtype, INode *argtype) {
    if (parmtype == NULL || argtype == NULL)
        return 1;
    // A reference to a generic function not instantiated, '&half', has a
    // signature naming half's own type parameters, which are not types
    if (nameUseNames(argtype, GenVarDclTag))
        return 1;
    if (nameUseNames(parmtype, GenVarDclTag))
        return genericCaptureType(inferredgencall, genparms, parmtype, argtype);
    if (isNameUseNode(argtype))
        argtype = itypeGetTypeDcl(argtype);
    switch (parmtype->tag) {
    case PtrTag:
    case DerefTag:
        if (argtype->tag != PtrTag)
            return 1;
        return genericInferType(inferredgencall, genparms,
            ((StarNode *)parmtype)->vtexp, ((StarNode *)argtype)->vtexp);
    case RefTag:
    case BorrowTag:
    case AllocateTag:
        if (argtype->tag != RefTag)
            return 1;
        return genericInferType(inferredgencall, genparms,
            ((RefNode *)parmtype)->vtexp, ((RefNode *)argtype)->vtexp);
    case ArrayRefTag:
    case ArrayBorrowTag: {
        // A fixed-size array, or a reference to one, is converted to the slice
        // a parameter expects, so its element type is what the slice's is
        INode *elemtype;
        if (argtype->tag == ArrayRefTag)
            elemtype = ((RefNode *)argtype)->vtexp;
        else if (argtype->tag == ArrayTag)
            elemtype = arrayElemType(argtype);
        else if (argtype->tag == RefTag && itypeGetTypeDcl(((RefNode *)argtype)->vtexp)->tag == ArrayTag)
            elemtype = arrayElemType(itypeGetTypeDcl(((RefNode *)argtype)->vtexp));
        // An owner of 'Array[T]' is lent as the slice a parameter expects
        else if (argtype->tag == RefTag && itypeIsArrayBody(((RefNode *)argtype)->vtexp))
            elemtype = itypeLenBodyElem(((RefNode *)argtype)->vtexp);
        else
            return 1;
        return genericInferType(inferredgencall, genparms, ((RefNode *)parmtype)->vtexp, elemtype);
    }
    case ArrayTag: {
        // An array type, 'Array[f32, N]', matches an array of the same shape: a
        // value parameter used as its size takes the argument's size, and its
        // element type is matched in turn
        if (argtype->tag != ArrayTag)
            return 1;
        ArrayNode *parmarray = (ArrayNode *)parmtype;
        ArrayNode *argarray = (ArrayNode *)argtype;
        if (parmarray->dimens->used == 1 && argarray->dimens->used == 1) {
            GenVarDclNode *sizeparm = genericValueParmOf(nodesGet(parmarray->dimens, 0));
            INode *size = nodesGet(argarray->dimens, 0);
            if (sizeparm && size->tag == ULitTag
                && genericCaptureValue(inferredgencall, genparms, sizeparm, ((ULitNode*)size)->uintlit) == 0)
                return 0;
        }
        return genericInferType(inferredgencall, genparms, arrayElemType(parmtype), arrayElemType(argtype));
    }
    case TTupleTag:
    case VTupleTag: {
        // A tuple type, '(P, P)', matches a tuple of as many elements, element by
        // element. In a template a tuple of type parameters is held as a value
        // tuple, since a parameter is not yet a type (cloneTupleNode)
        if (argtype->tag != TTupleTag)
            return 1;
        Nodes *parmelems = ((TupleNode *)parmtype)->elems;
        Nodes *argelems = ((TupleNode *)argtype)->elems;
        if (parmelems->used != argelems->used)
            return 1;
        INode **argelemp = &nodesGet(argelems, 0);
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(parmelems, cnt, elemp)) {
            if (genericInferType(inferredgencall, genparms, *elemp, *argelemp++) == 0)
                return 0;
        }
        return 1;
    }
    case FnSigTag: {
        // A function signature, reached through a function reference, matches
        // the signature of the function the argument references, parameter by
        // parameter, then the return type
        if (argtype->tag != FnSigTag)
            return 1;
        FnSigNode *parmsig = (FnSigNode *)parmtype;
        FnSigNode *argsig = (FnSigNode *)argtype;
        if (parmsig->parms->used != argsig->parms->used)
            return 1;
        INode **argparmp = &nodesGet(argsig->parms, 0);
        INode **parmp;
        uint32_t cnt;
        for (nodesFor(parmsig->parms, cnt, parmp)) {
            if (genericInferType(inferredgencall, genparms,
                ((VarDclNode *)*parmp)->vtype, ((VarDclNode *)*argparmp++)->vtype) == 0)
                return 0;
        }
        return genericInferType(inferredgencall, genparms, parmsig->rettype, argsig->rettype);
    }
    case FnCallTag: {
        // An instance of a generic type, 'List[T]', matches an argument that is an
        // instance of the same generic, each type argument against the instance's
        FnCallNode *parmcall = (FnCallNode *)parmtype;
        // A managed reference type matches a reference of either shape. The
        // value type is the last argument, whether or not a permission was written.
        if (parmcall->args != NULL && parmcall->args->used > 0
            && (itypeManagedRefRegion((INode*)parmcall) != NULL || nameUseNames(parmcall->objfn, GenVarDclTag))) {
            if (argtype->tag != RefTag && argtype->tag != VirtRefTag)
                return 1;
            RefNode *argref = (RefNode *)argtype;
            if (nameUseNames(parmcall->objfn, GenVarDclTag)
                && genericInferType(inferredgencall, genparms, parmcall->objfn, argref->region) == 0)
                return 0;
            return genericInferType(inferredgencall, genparms,
                nodesGet(parmcall->args, parmcall->args->used - 1), argref->vtexp);
        }
        if (!isNameUseNode(parmcall->objfn) || parmcall->args == NULL)
            return 1;
        GenericInfo *info = genericGetInfo(nameUseGetDcl((NameUseNode *)parmcall->objfn));
        if (info == NULL || info->memonodes == NULL)
            return 1;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(info->memonodes, cnt, nodesp)) {
            FnCallNode *instcall = (FnCallNode *)*nodesp;
            nodesp++; cnt--;
            if (*nodesp != argtype)
                continue;
            if (instcall->args->used != parmcall->args->used)
                return 1;
            INode **instargp = &nodesGet(instcall->args, 0);
            INode **parmargp;
            uint32_t parmcnt;
            for (nodesFor(parmcall->args, parmcnt, parmargp)) {
                if (genericInferType(inferredgencall, genparms, *parmargp, *instargp++) == 0 && !genericExpectMode)
                    return 0;
            }
            return 1;
        }
        // A variant is passed where its enum is expected: 'Some(7)' is an Option
        if (argtype->tag == StructTag) {
            StructNode *base = structBaseTraitDcl((StructNode *)argtype);
            if (base != NULL)
                return genericInferType(inferredgencall, genparms, parmtype, (INode *)base);
        }
        return 1;
    }
    default:
        return 1;
    }
}

// Infer generic type parameters from the function call arguments 'args', which
// match the signature's parameters from 'firstparm' on: 1 for a method called
// on a receiver, whose 'self' is not among the arguments yet, else 0. 'self' is
// that receiver where the call has one written (NULL for a method named bare in
// its type's braces), and is matched against 'self' first.
static void genericInferFromBounds(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall);
static FnSigNode *genericFnTypeSig(INode *type);

// An unsuffixed number literal passed for a parameter that is a bare type
// parameter, 'v T': it is whichever number type is wanted, and only defaults to
// i32 (an integer) or f64 (a float) if nothing else says (litAdoptNumberType)
static int genericArgIsAdaptable(INode *arg, INode *parmtype) {
    return litIsUntypedNumber(arg) && nameUseNames(parmtype, GenVarDclTag);
}

// The type a bare type parameter has been given so far, or NULL
static INode *genericCapturedType(FnCallNode *gencall, Nodes *genparms, INode *parmtype) {
    INode **genvarp;
    uint32_t genvarcnt;
    INode **genargp = &nodesGet(gencall->args, 0);
    for (nodesFor(genparms, genvarcnt, genvarp)) {
        if (((GenVarDclNode *)(*genvarp))->namesym == ((NameUseNode*)parmtype)->namesym)
            return *genargp;
        ++genargp;
    }
    return NULL;
}

static StructNode *genericNamedGenericTrait(INode *node);

// A slice, or a reference to a fixed-size array, passed for 'xs &C' where C is
// bounded by a generic trait ('C: Iterable[A]'), is C's argument as the body
// 'Array[T]' of its element: the struct whose methods an array and a slice call,
// so the bound is read off them as for any other type. (An array has no methods
// of its own, and a slice names no type for C to be.) Answers 1 once the
// argument is so taken up, 0 for any other parameter and argument, which are
// matched as ever (genericInferType). C's argument is the one 'T' of 'Array[T]'
// whatever the array's size, so a bound's cursor cannot depend on it.
static int genericInferArrayBound(TypeCheckState *pstate, Nodes *genparms, Nodes *where,
        FnCallNode *inferredgencall, INode *parmtype, INode *argtype) {
    if (where == NULL || parmtype == NULL || argtype == NULL)
        return 0;
    if (parmtype->tag != RefTag && parmtype->tag != BorrowTag)
        return 0;
    INode *target = ((RefNode*)parmtype)->vtexp;
    if (!nameUseNames(target, GenVarDclTag) || nameUseNames(argtype, GenVarDclTag))
        return 0;
    if (isNameUseNode(argtype))
        argtype = itypeGetTypeDcl(argtype);
    INode *elem;
    if (argtype->tag == ArrayRefTag)
        elem = ((RefNode*)argtype)->vtexp;
    else if (argtype->tag == RefTag && itypeGetTypeDcl(((RefNode*)argtype)->vtexp)->tag == ArrayTag)
        elem = arrayElemType(itypeGetTypeDcl(((RefNode*)argtype)->vtexp));
    else
        return 0;
    INode *parmdcl = ((NameUseNode*)target)->dclnode;
    INode **condp;
    uint32_t cnt;
    int bounded = 0;
    for (nodesFor(where, cnt, condp)) {
        if ((*condp)->tag != IsTag)
            continue;
        CastNode *clause = (CastNode*)*condp;
        if (isNameUseNode(clause->exp) && ((NameUseNode*)clause->exp)->dclnode == parmdcl
            && genericNamedGenericTrait(clause->typ) != NULL)
            bounded = 1;
    }
    if (!bounded)
        return 0;
    INode *body = fnCallArrayBody(pstate, (INode*)inferredgencall, elem);
    if (body == NULL)
        return 0;
    genericCaptureType(inferredgencall, genparms, target, body);
    return 1;
}

// A type parameter that nothing the call is given says may be said by where its
// value goes. 'result' is the type the generic makes, written in its type
// parameters: a function's return type, a generic type's own 'Box[T]', a
// variant's enum, 'Option[T]'. It is matched against 'expect', the type
// wanted of the call's value, as a parameter's type is against an argument's,
// but only the parameters still empty are filled (genericCaptureType never
// overwrites), so an argument or a written type argument always wins. An
// expectation the result does not match says nothing, and the call is then
// judged as it would be with none.
static int genericExpectUsable(INode *expect) {
    return expect != NULL && expect != unknownType && expect != noCareType
        && expect != errorType && isTypeNode(expect) && !inodeIsError(expect);
}

static void genericInferFromExpected(Nodes *genparms, INode *result, INode *expect, FnCallNode *inferredgencall) {
    if (result == NULL || !genericExpectUsable(expect))
        return;
    INode **argsp;
    uint32_t cnt;
    int unsaid = 0;
    for (nodesFor(inferredgencall->args, cnt, argsp)) {
        if (*argsp == NULL)
            unsaid = 1;
    }
    if (!unsaid)
        return;
    if (itypeGetTypeDcl(expect)->tag == VoidTag)
        return;
    ++genericExpectMode;
    genericInferType(inferredgencall, genparms, result, expect);
    --genericExpectMode;
}

// The type a generic type or variant makes, written in its type parameters,
// for genericInferFromExpected: a variant gives its enum ('Option[T]', which
// the variant keeps as its base), any other generic type itself, 'Box[T]'.
// NULL for a type with no parameters.
static INode *genericResultTemplate(StructNode *generic, GenericInfo *info) {
    if (generic->basetrait != NULL && generic->basetrait->tag == FnCallTag)
        return generic->basetrait;
    if (info->parms == NULL || info->parms->used == 0)
        return NULL;
    FnCallNode *template = newFnCallNode(newNameUseFromDclNode((INode*)generic, (INode*)generic), info->parms->used);
    template->flags |= FlagIndex;
    INode **parmp;
    uint32_t cnt;
    for (nodesFor(info->parms, cnt, parmp)) {
        NameUseNode *use = newNameUseFromLex(((GenVarDclNode*)*parmp)->namesym, (INode*)generic);
        use->dclnode = *parmp;
        nodesAdd(&template->args, (INode*)use);
    }
    return (INode*)template;
}

// Say which type parameters no argument and no expected type named. A macro's
// value goes nowhere that says a type (it has no return type), so for one,
// 'ismacro', only its arguments are said to have been asked.
static void genericMissingParms(FnCallNode *inferredgencall, Nodes *genparms, Name *name, INode *errnode, int ismacro) {
    char missing[200] = "";
    char all[200] = "";
    size_t mused = 0, aused = 0;
    uint32_t nmissing = 0;
    for (uint32_t j = 0; j < genparms->used && j < inferredgencall->args->used; ++j) {
        char *pname = &((GenVarDclNode*)nodesGet(genparms, j))->namesym->namestr;
        if (aused + 40 < sizeof(all))
            aused += snprintf(all + aused, sizeof(all) - aused, "%s%s", j ? ", " : "", pname);
        if (nodesGet(inferredgencall->args, j) == NULL) {
            if (mused + 40 < sizeof(missing))
                mused += snprintf(missing + mused, sizeof(missing) - mused, "%s%s", nmissing ? ", " : "", pname);
            ++nmissing;
        }
    }
    if (ismacro)
        errorMsgNode(errnode, ErrorInvType,
            "Could not infer the type parameter%s %s of the macro %s, since none of its arguments says %s. Write the type arguments out: %s[%s](...).",
            nmissing == 1 ? "" : "s", missing, &name->namestr, nmissing == 1 ? "what it is" : "what they are", &name->namestr, all);
    else
        errorMsgNode(errnode, ErrorInvType,
            "Could not infer the type parameter%s %s of %s, since neither its arguments nor where its value goes say %s. Write the type arguments out: %s[%s].",
            nmissing == 1 ? "" : "s", missing, &name->namestr, nmissing == 1 ? "what it is" : "what they are", &name->namestr, all);
}

// Match a receiver's type against its 'self' parameter's as the call will fit
// the one to the other: a value given to 'self: &Option[T]' is borrowed, so its
// type is matched against the reference's target; a reference given to a
// by-value 'self: Option[T]' is dereferenced, so what it points at is matched
static int genericInferReceiver(FnCallNode *inferredgencall, Nodes *genparms, INode *parmtype, INode *recvtype) {
    if (parmtype == NULL || recvtype == NULL || recvtype == unknownType)
        return 1;
    int parmref = parmtype->tag == RefTag || parmtype->tag == BorrowTag;
    INode *recvdcl = itypeGetTypeDcl(recvtype);
    if (parmref && recvdcl->tag != RefTag)
        parmtype = ((RefNode*)parmtype)->vtexp;
    while (!parmref && recvdcl->tag == RefTag) {
        recvtype = ((RefNode*)recvdcl)->vtexp;
        recvdcl = itypeGetTypeDcl(recvtype);
    }
    return genericInferType(inferredgencall, genparms, parmtype, recvtype);
}

// 'what' names what is inferred, for the message where two arguments disagree
// on a type parameter: "generic function", or a macro's type parameters.
static int genericInferFnParms(TypeCheckState *pstate, Nodes *genparms, FnSigNode *genfnsig,
        INode *self, Nodes *args, uint32_t firstparm, INode *errnode, FnCallNode *inferredgencall, Nodes *where,
        INode *expectType, const char *what) {

    genericReported = 0;
    if (args != NULL && args->used + firstparm > genfnsig->parms->used) {
        errorMsgNode(errnode, ErrorManyArgs, "Too many arguments provided for generic function.");
        return 0;
    }

    // Iterate through arguments and expected parms
    int retcode = 1;
    INode **argsp;
    uint32_t cnt;

    // A method called on a receiver that is not among the arguments yet: the
    // receiver takes part as any argument does, matched against 'self' as the
    // call will borrow or dereference it ('nested.flatten()' is
    // 'flatten(self: Option[Option[U]])' given an 'Option[Option[i32]]')
    if (self != NULL && firstparm == 1 && genfnsig->parms->used > 0 && isExpNode(self) && !inodeIsError(self)) {
        INode *parmtype = ((VarDclNode *)nodesGet(genfnsig->parms, 0))->vtype;
        if (genericInferReceiver(inferredgencall, genparms, parmtype, ((IExpNode *)self)->vtype) == 0) {
            if (!genericReported)
                errorMsgNode(self, ErrorInvType, "Inconsistent type for %s", what);
            retcode = 0;
        }
        genericReported = 0;
    }
    INode **parmp = &nodesGet(genfnsig->parms, firstparm);
    for (cnt = args ? args->used : 0, argsp = args ? &nodesGet(args, 0) : NULL; cnt; cnt--, argsp++) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((IExpNode *)*argsp)->vtype;
        // Capture the type of each generic variable the parameter's type names.
        // A 'null' is whichever pointer type is wanted, so says nothing of
        // which; another argument may, and the call's coercion then types it.
        // So does an unsuffixed integer literal for a bare type parameter,
        // which is taken up below, once the other arguments and the bounds
        // have had their say.
        if (!litIsUntypedNull(*argsp) && !genericArgIsAdaptable(*argsp, parmtype)
            && !genericInferArrayBound(pstate, genparms, where, inferredgencall, parmtype, argtype)
            && genericInferType(inferredgencall, genparms, parmtype, argtype) == 0) {
            if (!genericReported)
                errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for %s", what);
            retcode = 0;
        }
        genericReported = 0;
        ++parmp;
    }

    // A type parameter no argument names, only the bound of one that is named,
    // is read off that argument's methods, before a literal's default is
    // taken for it
    genericInferFromBounds(genparms, where, inferredgencall);

    // A type parameter still unsaid may be said by where the call's value goes:
    // the function's return type is matched against the type expected of it. This
    // comes before a literal takes a default, so 'let n: i64 = id(5)' is an i64's
    // identity, not an i32's widened.
    genericInferFromExpected(genparms, genfnsig->rettype, expectType, inferredgencall);

    // What is left to a literal: it names its parameter's type, i32, only if
    // nothing did. Otherwise it is converted to that type with the call's
    // other arguments, as an integer literal is wherever a number type is wanted.
    parmp = &nodesGet(genfnsig->parms, firstparm);
    for (cnt = args ? args->used : 0, argsp = args ? &nodesGet(args, 0) : NULL; cnt; cnt--, argsp++) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        if (genericArgIsAdaptable(*argsp, parmtype)) {
            INode *given = genericCapturedType(inferredgencall, genparms, parmtype);
            if (given == NULL)
                genericInferType(inferredgencall, genparms, parmtype, ((IExpNode *)*argsp)->vtype);
            else if (!itypeIsSame(given, ((IExpNode *)*argsp)->vtype) && iexpMatches(argsp, given, Coercion) == NoMatch) {
                errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for %s", what);
                retcode = 0;
            }
        }
        ++parmp;
    }
    return retcode;
}

// A macro's type parameters, inferred from its arguments by the rules a
// generic function's are from a call's (genericInferFnParms): each argument's
// type matched against its parameter's, a 'null' and an unsuffixed number
// literal left to the others, then a literal's own type. A macro has no
// return type and no bounds, so nothing else has a say. 'parmtypes' and
// 'args' are paired: a parameter's type as written, and its argument, type
// checked (macro.c); a pair is left out where either is NULL. Answers the type
// arguments, a type parameter nothing said left NULL for the caller to report
// (genericMacroMissingParms), or NULL once two arguments are reported to
// disagree. 'prior', where not NULL, is what earlier arguments said, which
// these must agree with.
Nodes *genericInferMacroParms(TypeCheckState *pstate, Nodes *typeparms, Nodes *parmtypes, Nodes *args,
        Nodes *prior, INode *errnode, Name *name) {
    FnSigNode *sig = newFnSigNode();
    Nodes *typedargs = newNodes(args->used ? args->used : 1);
    for (uint32_t i = 0; i < args->used; ++i) {
        INode *parmtype = nodesGet(parmtypes, i);
        INode *arg = nodesGet(args, i);
        if (parmtype == NULL || arg == NULL)
            continue;
        VarDclNode *parm = newVarDclNode(anonName, VarDclTag, (INode*)immPerm);
        inodeLexCopy((INode*)parm, arg);
        parm->vtype = parmtype;
        nodesAdd(&sig->parms, (INode*)parm);
        nodesAdd(&typedargs, arg);
    }
    uint32_t nparms = typeparms->used;
    FnCallNode *inferred = newFnCallNode(NULL, nparms);
    inodeLexCopy((INode*)inferred, errnode);
    for (uint32_t t = 0; t < nparms; ++t)
        nodesAdd(&inferred->args, prior ? nodesGet(prior, t) : (INode*)NULL);
    char what[200];
    snprintf(what, sizeof(what), "the macro %s's type parameters: its arguments must agree on each",
        &name->namestr);
    if (genericInferFnParms(pstate, typeparms, sig, NULL, typedargs, 0, errnode, inferred, NULL, NULL, what) == 0)
        return NULL;
    return inferred->args;
}

// Say which of a macro's type parameters its arguments left unsaid: the NULLs
// of 'typeargs', as genericInferMacroParms answered them
void genericMacroMissingParms(Nodes *typeargs, Nodes *typeparms, Name *name, INode *errnode) {
    FnCallNode *inferred = newFnCallNode(NULL, typeargs->used);
    inodeLexCopy((INode*)inferred, errnode);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(typeargs, cnt, argsp))
        nodesAdd(&inferred->args, *argsp);
    genericMissingParms(inferred, typeparms, name, errnode, 1);
}

// ---------------------------------------------------------------------------
// Constraints. See generic.h.

// The trait a constraint's name resolved to, or NULL if it names no trait a
// constraint can take. An instance of a generic trait is not one yet.
static StructNode *genericNamedTrait(INode *node) {
    if (!isNameUseNode(node))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)node);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType)
        || ((StructNode*)dcl)->genericinfo)
        return NULL;
    return (StructNode*)dcl;
}

// The generic trait a constraint's 'Stack[T]' names, or NULL if the node is not
// an instance of one written out: the trait, not an instance of it yet, since
// the type arguments are parameters of the generic being constrained.
static StructNode *genericNamedGenericTrait(INode *node) {
    if (node->tag != FnCallTag)
        return NULL;
    INode *objfn = ((FnCallNode*)node)->objfn;
    if (!isNameUseNode(objfn))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)objfn);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType)
        || ((StructNode*)dcl)->genericinfo == NULL)
        return NULL;
    return (StructNode*)dcl;
}

// A generic trait named with no type arguments, 'Stack': it names no trait
// until it is given them
static StructNode *genericNamedBareGenericTrait(INode *node) {
    if (!isNameUseNode(node))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)node);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType)
        || ((StructNode*)dcl)->genericinfo == NULL)
        return NULL;
    return (StructNode*)dcl;
}

// Vet the instance of a generic trait a constraint names, 'Stack[T]': it is
// given as many arguments as the trait has parameters. Reported, 0, if not.
static int genericTraitInstanceVet(INode *node) {
    StructNode *trait = genericNamedGenericTrait(node);
    FnCallNode *call = (FnCallNode*)node;
    uint32_t given = call->args ? call->args->used : 0;
    uint32_t wanted = genericWrittenCount(trait->genericinfo);
    // An associated type given as an argument would fix it in the constraint,
    // which only the type meeting the trait says
    if (given != wanted && genericHasAssoc(trait->genericinfo) && given == trait->genericinfo->parms->used) {
        errorMsgNode(node, ErrorArgCount,
            "%s takes %u type argument%s, and this constraint gives %u: its associated types are said by the type that meets it, not by a constraint. (Fixing one in a constraint, as Rust's 'Item = u8', is not built.)",
            &trait->namesym->namestr, wanted, wanted == 1 ? "" : "s", given);
        return 0;
    }
    if (given != wanted) {
        errorMsgNode(node, ErrorArgCount,
            "%s takes %u type argument%s, and this constraint gives %u.",
            &trait->namesym->namestr, wanted, wanted == 1 ? "" : "s", given);
        return 0;
    }
    return 1;
}

// The one candidate of this name, in the type that has it, taking as many
// parameters as the trait's method: the one a trait's signature is matched
// against. NULL where the type has none, or has several, which say nothing.
static FnDclNode *genericMatchMethodArity(INode *binding, uint32_t wanted);
static FnDclNode *genericMatchMethod(INode *binding, FnDclNode *traitmeth) {
    return genericMatchMethodArity(binding, ((FnSigNode*)itypeGetTypeDcl(traitmeth->vtype))->parms->used);
}

// The one candidate of this name taking this many parameters (a receiver among them)
static FnDclNode *genericMatchMethodArity(INode *binding, uint32_t wanted) {
    // A folded method's binding is an alias; the candidates are its target's
    // (a list's 'split', lent from its body Array)
    while (binding && binding->tag == AliasDclTag && !(binding->flags & FlagTypeAlias)
        && ((AliasDclNode*)binding)->target && isNameUseNode(((AliasDclNode*)binding)->target))
        binding = ((NameUseNode*)((AliasDclNode*)binding)->target)->dclnode;
    if (binding == NULL)
        return NULL;
    fnCallDemandCandidates(binding);
    INode **candp;
    uint32_t cnt;
    if (binding->tag == FnDclTag) {
        candp = &binding;
        cnt = 1;
    }
    else if (binding->tag == FnOverloadDclTag) {
        candp = &nodesGet(((FnOverloadDclNode*)binding)->overloads, 0);
        cnt = ((FnOverloadDclNode*)binding)->overloads->used;
    }
    else
        return NULL;
    FnDclNode *found = NULL;
    while (cnt--) {
        FnDclNode *cand = (FnDclNode*)*candp++;
        if (cand->vtype == NULL || ((FnSigNode*)itypeGetTypeDcl(cand->vtype))->parms->used != wanted)
            continue;
        if (found)
            return NULL;
        found = cand;
    }
    return found;
}

// A type parameter that a call's arguments leave unsaid, named only in the bound
// of another that they do, 'T' in '[T, S Stack[T]]': read off the methods the
// bounded argument has. The bound's trait is matched with that argument as a
// call's parameters are with its arguments (genericInferType): each parameter
// type and the return type of each method the trait requires, against the same
// method of the argument's type, capture what the trait's own parameters are
// there, and those are what the bound's arguments come to. It captures only an
// empty slot, and never contradicts one, so a parameter already inferred stays,
// and the bound is then decided as ever, by evaluation, on whatever was found.
static void genericAssocAliases(StructNode *trait, StructNode *type, FnCallNode *traitcall);
static void genericInferTraitMethods(StructNode *trait, StructNode *type, FnCallNode *traitcall);
static void genericInferFromBoundsDepth(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall, int depth);

static void genericInferFromBounds(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall) {
    genericInferFromBoundsDepth(genparms, where, inferredgencall, 0);
}

// A trait's associated type is one of its own parameters, read off the methods
// as the others are; what its bound says of the trait's other parameters is read
// in turn, off the associated type's own methods ('Cursor is Iterator[T]': T is
// what the cursor's 'next' gives). 'depth' stops a bound that names its own
// trait from reading without end.
static void genericInferFromBoundsDepth(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall, int depth) {
    if (where == NULL || genparms == NULL)
        return;
    INode **argsp;
    uint32_t cnt;
    int unsaid = 0;
    for (nodesFor(inferredgencall->args, cnt, argsp)) {
        if (*argsp == NULL)
            unsaid = 1;
    }
    if (!unsaid)
        return;
    INode **condp;
    for (nodesFor(where, cnt, condp)) {
        if ((*condp)->tag != IsTag)
            continue;
        CastNode *clause = (CastNode*)*condp;
        StructNode *trait = genericNamedGenericTrait(clause->typ);
        if ((trait == NULL && clause->typ->tag != FnSigTag) || !isNameUseNode(clause->exp))
            continue;
        // The bounded parameter must have its argument already
        INode *subjdcl = ((NameUseNode*)clause->exp)->dclnode;
        INode *sarg = NULL;
        for (uint32_t j = 0; j < genparms->used && j < inferredgencall->args->used; ++j) {
            if (nodesGet(genparms, j) == subjdcl)
                sarg = nodesGet(inferredgencall->args, j);
        }
        if (sarg == NULL || !isTypeNode(sarg))
            continue;
        INode *sdcl = itypeGetTypeDcl(sarg);
        // A signature: what its parameters and return are is what the argument's
        // function, or its '()', has them as
        if (clause->typ->tag == FnSigTag) {
            FnSigNode *bound = (FnSigNode*)clause->typ;
            if (genericFnTypeSig(sarg))
                genericInferType(inferredgencall, genparms, (INode*)bound, (INode*)genericFnTypeSig(sarg));
            else if (sdcl->tag == StructTag) {
                FnDclNode *meth = genericMatchMethodArity(
                    namespaceFind(&((StructNode*)sdcl)->namespace, parensName), bound->parms->used + 1);
                if (meth && meth->vtype) {
                    FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
                    for (uint32_t j = 0; j < bound->parms->used; ++j)
                        genericInferType(inferredgencall, genparms, ((VarDclNode*)nodesGet(bound->parms, j))->vtype,
                            ((VarDclNode*)nodesGet(msig->parms, j + 1))->vtype);
                    genericInferType(inferredgencall, genparms, bound->rettype, msig->rettype);
                }
            }
            continue;
        }
        FnCallNode *boundcall = (FnCallNode*)clause->typ;
        Nodes *tparms = trait->genericinfo->parms;
        uint32_t nwritten = genericWrittenCount(trait->genericinfo);
        uint32_t given = boundcall->args ? boundcall->args->used : 0;
        if (sdcl->tag != StructTag || tparms == NULL || given != nwritten)
            continue;
        // What each of the trait's own parameters is, as the argument's methods say:
        // an associated type, as the alias the argument declares for it says first
        FnCallNode *traitcall = newFnCallNode(boundcall->objfn, tparms->used);
        inodeLexCopy((INode*)traitcall, (INode*)boundcall);
        for (uint32_t j = 0; j < tparms->used; ++j)
            nodesAdd(&traitcall->args, (INode*)NULL);
        genericAssocAliases(trait, (StructNode*)sdcl, traitcall);
        genericInferTraitMethods(trait, (StructNode*)sdcl, traitcall);
        if (nwritten < tparms->used && depth < 8)
            genericInferFromBoundsDepth(tparms, trait->genericinfo->where, traitcall, depth + 1);
        for (uint32_t j = 0; j < nwritten; ++j) {
            if (nodesGet(traitcall->args, j))
                genericInferType(inferredgencall, genparms, nodesGet(boundcall->args, j), nodesGet(traitcall->args, j));
        }
    }
}

// Each method a trait requires, matched with the type's method of the name (the
// one candidate with as many parameters): every parameter after the receiver,
// then the return type, captures what the trait's own parameters are there, into
// the empty slots of 'traitcall', one for each of the trait's parameters
static void genericInferTraitMethods(StructNode *trait, StructNode *type, FnCallNode *traitcall) {
    Nodes *tparms = trait->genericinfo->parms;
    INode **nodesp;
    uint32_t methcnt;
    for (nodelistFor(&trait->nodelist, methcnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *traitmeth = (FnDclNode*)*nodesp;
        FnDclNode *meth = genericMatchMethod(namespaceFind(&type->namespace, traitmeth->namesym), traitmeth);
        if (meth == NULL)
            continue;
        FnSigNode *tsig = (FnSigNode*)itypeGetTypeDcl(traitmeth->vtype);
        FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
        // The receiver is the first parameter of each; the rest are matched
        for (uint32_t j = 1; j < tsig->parms->used; ++j)
            genericInferType(traitcall, tparms, ((VarDclNode*)nodesGet(tsig->parms, j))->vtype,
                ((VarDclNode*)nodesGet(msig->parms, j))->vtype);
        genericInferType(traitcall, tparms, tsig->rettype, msig->rettype);
    }
}

// ---------------------------------------------------------------------------
// Associated types (generic.h). A trait's 'type Cursor: Iterator[T];' is a
// parameter of the trait no use writes, after the written ones, so an instance
// of the trait is made at every argument, 'Iterable[i64, ListIter[i64]]', and a
// type meets it, as ever, by fitting that instance. What a use leaves out is
// read off the type meeting the trait, where the use has one: a bound's argument,
// an 'is' list's type. Nothing is built for it: it is a type the program declares
// already, the target of the type's alias or what its method gives.

int genericIsAssocParm(INode *parm) {
    return parm->tag == GenVarDclTag && (parm->flags & FlagAssocType);
}

uint32_t genericWrittenCount(GenericInfo *info) {
    if (info == NULL || info->parms == NULL)
        return 0;
    uint32_t written = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(info->parms, cnt, nodesp)) {
        if (!genericIsAssocParm(*nodesp))
            ++written;
    }
    return written;
}

int genericHasAssoc(GenericInfo *info) {
    return info != NULL && info->parms != NULL && genericWrittenCount(info) < info->parms->used;
}

// The trait a use names written out, 'Iterable[T]' (a call of its name), where
// the trait declares associated types: its template. NULL for anything else.
static StructNode *genericAssocTraitOf(INode *node) {
    if (node == NULL || node->tag != FnCallTag || ((FnCallNode*)node)->methfld != NULL)
        return NULL;
    INode *objfn = ((FnCallNode*)node)->objfn;
    if (!isNameUseNode(objfn))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)objfn);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType)
        || !genericHasAssoc(((StructNode*)dcl)->genericinfo))
        return NULL;
    return (StructNode*)dcl;
}

void genericAssocBareUse(INode **nodep) {
    INode *node = *nodep;
    if (!isNameUseNode(node))
        return;
    INode *dcl = nameUseGetDcl((NameUseNode*)node);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType)
        || !genericHasAssoc(((StructNode*)dcl)->genericinfo) || genericWrittenCount(((StructNode*)dcl)->genericinfo) != 0)
        return;
    FnCallNode *call = newFnCallNode(node, 0);
    inodeLexCopy((INode*)call, node);
    call->flags |= FlagIndex;
    *nodep = (INode*)call;
}

// The alias a type declares for each of a trait's associated types, into the
// empty slots of 'traitcall': what it says outright is what the type's methods
// are then held to
static void genericAssocAliases(StructNode *trait, StructNode *type, FnCallNode *traitcall) {
    Nodes *tparms = trait->genericinfo->parms;
    for (uint32_t j = 0; j < tparms->used; ++j) {
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(tparms, j);
        if (!genericIsAssocParm((INode*)parm) || nodesGet(traitcall->args, j) != NULL)
            continue;
        INode *alias = namespaceFind(&type->namespace, parm->namesym);
        if (alias == NULL || alias->tag != AliasDclTag || !(alias->flags & FlagTypeAlias))
            continue;
        // Checked where it is asked for, as a member a call reaches is
        if (!(alias->flags & (TypeChecked | TypeChecking))) {
            TypeCheckState tstate;
            tstate.typenode = (INode*)type;
            tstate.fn = NULL;
            tstate.scope = 0;
            tstate.extend = NULL;
            alias->flags |= TypeChecking;
            aliasDclTypeCheck(&tstate, (AliasDclNode*)alias);
            alias->flags = (alias->flags & ~TypeChecking) | TypeChecked;
        }
        INode *target = ((AliasDclNode*)alias)->target;
        if (target == NULL || !isTypeNode(target) || inodeIsError(target))
            continue;
        INode *dcl = itypeGetTypeDcl(target);
        nodesGet(traitcall->args, j) = dcl->tag == StructTag ? newNameUseFromDclNode(dcl, (INode*)traitcall) : target;
    }
}

static INode *genericUnmetCondition(INode *generic, Nodes *where, Nodes *parms, Nodes *args);
static StructNode *genericTemplateOf(StructNode *inst);
static void genericTypeNameCat(char *buf, size_t size, INode *type, int depth);
static void genericTemplateNameCat(char *buf, size_t size, INode *node, int depth);
static void genericConditionCat(char *buf, size_t size, INode *cond, int grouped);
static void genericBindingsCat(char *buf, size_t size, INode *cond, Nodes *parms, Nodes *args);
static INode *genericClauseCloneChecked(CastNode *clause, Nodes *parms, Nodes *args);

// Why the last reading of associated types came to nothing, for a refusal to say
static char genericAssocWhy[700];

// Every argument of 'trait' (a template declaring associated types), given the
// written ones, 'written', and the type that meets it, 'type': the written ones,
// then each associated type, as the type's alias says or else as its methods
// do. NULL, the reason in genericAssocWhy, where the type says one of them
// nowhere, or says what the trait's constraints refuse ('Cursor is Iterator[T]').
static Nodes *genericAssocRead(StructNode *trait, Nodes *written, INode *type) {
    GenericInfo *info = trait->genericinfo;
    Nodes *tparms = info->parms;
    uint32_t nwritten = genericWrittenCount(info);
    genericAssocWhy[0] = '\0';
    char typename[256] = "";
    genericTypeNameCat(typename, sizeof(typename), type, 0);
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != StructTag) {
        snprintf(genericAssocWhy, sizeof(genericAssocWhy),
            " %s is not a struct: only a struct says what %s's associated types are, by the methods it has.",
            typename, &trait->namesym->namestr);
        return NULL;
    }
    FnCallNode *traitcall = newFnCallNode(NULL, tparms->used);
    for (uint32_t j = 0; j < tparms->used; ++j)
        nodesAdd(&traitcall->args, (INode*)NULL);
    genericAssocAliases(trait, (StructNode*)dcl, traitcall);
    genericInferTraitMethods(trait, (StructNode*)dcl, traitcall);
    Nodes *full = newNodes(tparms->used);
    for (uint32_t j = 0; j < tparms->used; ++j) {
        if (j < nwritten) {
            nodesAdd(&full, written && j < written->used ? nodesGet(written, j) : NULL);
            continue;
        }
        INode *said = nodesGet(traitcall->args, j);
        if (said == NULL) {
            // The methods that would have said it, by name
            Name *assoc = ((GenVarDclNode*)nodesGet(tparms, j))->namesym;
            char meths[256] = "";
            INode **nodesp;
            uint32_t cnt;
            for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
                if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
                    continue;
                size_t used = strlen(meths);
                snprintf(meths + used, sizeof(meths) - used, "%s%s", meths[0] ? ", " : "", &((FnDclNode*)*nodesp)->namesym->namestr);
            }
            snprintf(genericAssocWhy, sizeof(genericAssocWhy),
                " %s does not say what %s's %s is: it is said by an alias, 'alias %s = ...;', or by a method %s requires (%s), and %s has neither with the trait's shape.",
                typename, &trait->namesym->namestr, &assoc->namestr, &assoc->namestr,
                &trait->namesym->namestr, meths[0] ? meths : "none", typename);
            return NULL;
        }
        nodesAdd(&full, said);
    }
    // The trait's own constraints, each associated type's bound among them
    INode *unmet = genericUnmetCondition((INode*)trait, info->where, tparms, full);
    if (unmet) {
        // The clause as it comes to here, 'Cursor is Iterator[i64]', where it can
        // be made; else as written, with what it turned on
        char cond[256] = "";
        INode *inst = unmet->tag == IsTag && isNameUseNode(((CastNode*)unmet)->exp)
            ? genericClauseCloneChecked((CastNode*)unmet, tparms, full) : NULL;
        if (inst) {
            snprintf(cond, sizeof(cond), "%s is ", &((NameUseNode*)((CastNode*)unmet)->exp)->namesym->namestr);
            genericTypeNameCat(cond, sizeof(cond), inst, 0);
        }
        else {
            genericConditionCat(cond, sizeof(cond), unmet, 0);
            size_t used = strlen(cond);
            snprintf(cond + used, sizeof(cond) - used, ", false for ");
            genericBindingsCat(cond, sizeof(cond), unmet, tparms, full);
        }
        char said[600] = "";
        for (uint32_t j = nwritten; j < tparms->used; ++j) {
            size_t used = strlen(said);
            snprintf(said + used, sizeof(said) - used, "%s%s is ", said[0] ? ", " : "",
                &((GenVarDclNode*)nodesGet(tparms, j))->namesym->namestr);
            genericTypeNameCat(said, sizeof(said), nodesGet(full, j), 0);
        }
        snprintf(genericAssocWhy, sizeof(genericAssocWhy),
            " For %s, %s's %s, and %s requires %s.",
            typename, &trait->namesym->namestr, said, &trait->namesym->namestr, cond);
        return NULL;
    }
    return full;
}

// The call node an instance of a trait with associated types is checked as,
// once completed: genericMemoize takes its full list of arguments, which a
// use may not write
static FnCallNode *genericAssocCompleted = NULL;
static int genericAssocCheckArgs(TypeCheckState *pstate, FnCallNode *call);

// Complete 'call', a trait with associated types written with its written
// arguments, 'Iterable[i64]', with what 'type' says they are, ready to be
// checked as the trait's instance. 0, with genericAssocWhy, where it does not
// say them.
static int genericAssocComplete(FnCallNode *call, INode *type) {
    StructNode *trait = genericAssocTraitOf((INode*)call);
    Nodes *full = genericAssocRead(trait, call->args, type);
    if (full == NULL)
        return 0;
    call->args = full;
    genericAssocCompleted = call;
    return 1;
}

// The registry of the bounds that may say an associated type a generic's code
// names through its parameter: each clause of a 'where' list, or of a parameter's
// annotation, naming a trait with associated types, kept by its subject
static INode **genericAssocBoundSubj = NULL;
static INode **genericAssocBoundTrait = NULL;
static uint32_t genericAssocBoundCnt = 0;
static uint32_t genericAssocBoundCap = 0;

static void genericAssocBoundNote(INode *cond) {
    if (cond->tag != IsTag)
        return;
    CastNode *clause = (CastNode*)cond;
    if (!isNameUseNode(clause->exp) || ((NameUseNode*)clause->exp)->dclnode == NULL
        || ((NameUseNode*)clause->exp)->dclnode->tag != GenVarDclTag || genericAssocTraitOf(clause->typ) == NULL)
        return;
    if (genericAssocBoundCnt == genericAssocBoundCap) {
        uint32_t cap = genericAssocBoundCap ? genericAssocBoundCap * 2 : 16;
        INode **subj = memAllocBlk(cap * sizeof(INode*));
        INode **trait = memAllocBlk(cap * sizeof(INode*));
        if (genericAssocBoundCnt) {
            memcpy(subj, genericAssocBoundSubj, genericAssocBoundCnt * sizeof(INode*));
            memcpy(trait, genericAssocBoundTrait, genericAssocBoundCnt * sizeof(INode*));
        }
        genericAssocBoundSubj = subj;
        genericAssocBoundTrait = trait;
        genericAssocBoundCap = cap;
    }
    genericAssocBoundSubj[genericAssocBoundCnt] = ((NameUseNode*)clause->exp)->dclnode;
    genericAssocBoundTrait[genericAssocBoundCnt++] = clause->typ;
}

// The associated type of this name a trait declares, or NULL
static GenVarDclNode *genericAssocNamed(StructNode *trait, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->genericinfo->parms, cnt, nodesp)) {
        if (genericIsAssocParm(*nodesp) && ((GenVarDclNode*)*nodesp)->namesym == name)
            return (GenVarDclNode*)*nodesp;
    }
    return NULL;
}

int genericAssocPathNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    if (node->methfld == NULL || !isNameUseNode(node->methfld) || node->args != NULL
        || (node->flags & (FlagIndex | FlagOperator)) || !isNameUseNode(node->objfn))
        return 0;
    INode *parm = ((NameUseNode*)node->objfn)->dclnode;
    if (parm == NULL || parm->tag != GenVarDclTag || ((GenVarDclNode*)parm)->valtype)
        return 0;
    Name *name = ((NameUseNode*)node->methfld)->namesym;
    INode *bound = NULL;
    INode *other = NULL;
    char declared[300] = "";
    for (uint32_t i = 0; i < genericAssocBoundCnt; ++i) {
        if (genericAssocBoundSubj[i] != parm)
            continue;
        StructNode *trait = genericAssocTraitOf(genericAssocBoundTrait[i]);
        if (genericAssocNamed(trait, name)) {
            // The same trait named twice, 'C: Iterable[T]' and 'where C is
            // Iterable[T]', says one Cursor; two traits each declaring one are two
            if (bound && genericAssocTraitOf(bound) != trait)
                other = genericAssocBoundTrait[i];
            else if (bound == NULL)
                bound = genericAssocBoundTrait[i];
        }
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(trait->genericinfo->parms, cnt, nodesp)) {
            if (!genericIsAssocParm(*nodesp))
                continue;
            size_t used = strlen(declared);
            snprintf(declared + used, sizeof(declared) - used, "%s%s's %s", declared[0] ? ", " : "",
                &trait->namesym->namestr, &((GenVarDclNode*)*nodesp)->namesym->namestr);
        }
    }
    if (declared[0] == '\0')
        return 0;   // no bound of the parameter declares one: a path as ever
    Name *pname = ((GenVarDclNode*)parm)->namesym;
    if (bound == NULL) {
        errorMsgNode(node->methfld, ErrorUnkName,
            "%s.%s names no associated type: the bounds of %s declare %s, and no %s.",
            &pname->namestr, &name->namestr, &pname->namestr, declared, &name->namestr);
        *(INode**)nodep = errorType;
        return 1;
    }
    if (other) {
        errorMsgNode(node->methfld, ErrorUnkName,
            "%s.%s names no one associated type: %s and %s, both bounds of %s, each declare %s.",
            &pname->namestr, &name->namestr, &genericAssocTraitOf(bound)->namesym->namestr,
            &genericAssocTraitOf(other)->namesym->namestr, &pname->namestr, &name->namestr);
        *(INode**)nodep = errorType;
        return 1;
    }
    // 'Iterable[T].Cursor(C)': the bound as written, its names resolved already,
    // copied as uses of what they name, with the parameter as its one argument
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, NULL, NULL, 0, NULL, NULL);
    INode *traitcopy = cloneNode(&cstate, bound);
    clonePopState();
    cloneDclPop(dclpos);
    FnCallNode *path = newFnCallNode(traitcopy, 1);
    inodeLexCopy((INode*)path, (INode*)node);
    path->methfld = node->methfld;
    nodesAdd(&path->args, node->objfn);
    *nodep = path;
    return 1;
}

// The parts of a path genericAssocPathNameRes made: its trait's template and
// the associated type it names
static int genericAssocPathParts(INode *node, StructNode **traitp, GenVarDclNode **assocp) {
    if (node->tag != FnCallTag)
        return 0;
    FnCallNode *call = (FnCallNode*)node;
    if (call->methfld == NULL || !isNameUseNode(call->methfld) || call->args == NULL || call->args->used != 1)
        return 0;
    StructNode *trait = genericAssocTraitOf(call->objfn);
    if (trait == NULL)
        return 0;
    GenVarDclNode *assoc = genericAssocNamed(trait, ((NameUseNode*)call->methfld)->namesym);
    if (assoc == NULL)
        return 0;
    *traitp = trait;
    *assocp = assoc;
    return 1;
}

int genericAssocPathIs(INode *node) {
    StructNode *trait;
    GenVarDclNode *assoc;
    return genericAssocPathParts(node, &trait, &assoc);
}

int genericAssocPathIsType(INode *node) {
    StructNode *trait;
    GenVarDclNode *assoc;
    return node->tag == FnCallTag && genericAssocPathParts(node, &trait, &assoc)
        && isTypeNode(nodesGet(((FnCallNode*)node)->args, 0));
}

int genericAssocPathTypeCheck(TypeCheckState *pstate, INode **nodep) {
    StructNode *trait;
    GenVarDclNode *assoc;
    if (!genericAssocPathParts(*nodep, &trait, &assoc))
        return 0;
    FnCallNode *path = (FnCallNode*)*nodep;
    INode **subjectp = &nodesGet(path->args, 0);
    // Still the template's parameter: the template is never checked, so this is
    // a path a template holds, reached through no instance
    if (nameUseNames(*subjectp, GenVarDclTag) || itypeTypeCheck(pstate, subjectp) == 0) {
        *nodep = errorType;
        return 1;
    }
    FnCallNode *traitcall = (FnCallNode*)path->objfn;
    if (!genericAssocCheckArgs(pstate, traitcall)) {
        *nodep = errorType;
        return 1;
    }
    Nodes *full = genericAssocRead(trait, traitcall->args, *subjectp);
    if (full == NULL) {
        // The bound was a requirement of the instance, decided where it was asked
        // for, so this is reached only where that was not
        char isname[256] = "";
        genericTemplateNameCat(isname, sizeof(isname), (INode*)traitcall, 0);
        errorMsgNode((INode*)path, ErrorWhereUnmet, "%s names %s's %s, and is not %s.%s",
            &assoc->namesym->namestr, isname, &assoc->namesym->namestr, isname, genericAssocWhy);
        *nodep = errorType;
        return 1;
    }
    uint32_t idx = 0;
    while (nodesGet(trait->genericinfo->parms, idx) != (INode*)assoc)
        ++idx;
    INode *said = nodesGet(full, idx);
    INode *dcl = itypeGetTypeDcl(said);
    *nodep = dcl->tag == StructTag ? newNameUseFromDclNode(dcl, (INode*)path) : said;
    itypeTypeCheck(pstate, nodep);
    return 1;
}

// The written arguments of a use of a trait with associated types, checked as
// types before they are matched with what a type's methods say. 0 if one is not
static int genericAssocCheckArgs(TypeCheckState *pstate, FnCallNode *call) {
    if (call->args == NULL)
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(call->args, cnt, nodesp)) {
        if (itypeTypeCheck(pstate, nodesp) == 0)
            return 0;
    }
    return 1;
}

int genericAssocCompleteIs(TypeCheckState *pstate, INode *entry, StructNode *type) {
    StructNode *trait = genericAssocTraitOf(entry);
    if (trait == NULL)
        return 1;
    FnCallNode *call = (FnCallNode*)entry;
    uint32_t given = call->args ? call->args->used : 0;
    if (given != genericWrittenCount(trait->genericinfo))
        return 1;   // the arity is refused where the instance is asked for
    if (!genericAssocCheckArgs(pstate, call))
        return 1;   // reported
    if (genericAssocComplete(call, (INode*)type))
        return 1;
    char isname[256] = "";
    genericTemplateNameCat(isname, sizeof(isname), entry, 0);
    errorMsgNode(entry, ErrorInvType, "%s declares it is %s, and so says what its associated types are.%s",
        &type->namesym->namestr, isname, genericAssocWhy);
    return 0;
}

// The type a 'where' clause's name resolved to, where it names a type rather
// than a trait: 'where T is bool' [Jon 27 Sep], met by that type alone. NULL
// for anything else; a generic type is not one until given its arguments.
static INode *genericNamedType(INode *node) {
    if (!isNameUseNode(node) || !isTypeNode(node))
        return NULL;
    INode *dcl = itypeGetTypeDcl(node);
    if (dcl->tag == StructTag && ((dcl->flags & TraitType) || ((StructNode*)dcl)->genericinfo))
        return NULL;
    return node;
}

// Does a bound written after a type parameter name an actor trait: its handle,
// or an instance of a generic one with its type arguments? An actor trait is
// not a trait in the type system, but a handle struct, so '[S: ChunkSink]' reads
// as a type met by itself alone; an actor that conforms meets it too
static int genericNamedActorTrait(INode *node) {
    INode *dcl = NULL;
    if (node->tag == FnCallTag && isNameUseNode(((FnCallNode*)node)->objfn))
        dcl = nameUseGetDcl((NameUseNode*)((FnCallNode*)node)->objfn);
    else if (isNameUseNode(node) && isTypeNode(node))
        dcl = itypeGetTypeDcl(node);
    return dcl != NULL && dcl->tag == StructTag && actorTraitOfHandle(dcl) != NULL;
}

// A type written out as an instance, where a 'where' clause names one:
// 'where K is Uni[str]', 'where T is Option[i64]'. A generic type given its
// arguments, or a reference type (a region's, 'Uni[str]', is one), whose parts
// may name the generic's own parameters. It is met by that type alone, as a
// named type is; it is made at the arguments when the clause is decided
// (genericConditionValue).
static int genericIsTypeInstance(INode *node) {
    if (node->tag == RefTag)
        return 1;
    if (node->tag != FnCallTag || !isNameUseNode(((FnCallNode*)node)->objfn))
        return 0;
    INode *dcl = nameUseGetDcl((NameUseNode*)((FnCallNode*)node)->objfn);
    if (dcl == NULL || dcl->tag != StructTag || (dcl->flags & TraitType))
        return 0;
    // (A region's own declaration need not be resolved yet: the clause is
    // vetted during name resolution, so ask what its 'is' list wrote.)
    return ((StructNode*)dcl)->genericinfo != NULL || regionStructWritesRegionRef((StructNode*)dcl);
}

// Append the name a clause's subject 'is' to 'buf': a trait's or a type's
static void genericTypeNameCat(char *buf, size_t size, INode *type, int depth);
static void genericTemplateNameCat(char *buf, size_t size, INode *node, int depth);
static void genericClauseNameCat(char *buf, size_t size, CastNode *clause, StructNode *inst) {
    StructNode *trait = genericNamedTrait(clause->typ);
    // A signature, as it was written
    if (clause->typ->tag == FnSigTag) {
        size_t used = strlen(buf);
        Name *spelled = ((FnSigNode*)clause->typ)->spelled;
        snprintf(buf + used, size - used, "%s", spelled ? &spelled->namestr : "fn(...)");
    }
    else if (trait) {
        size_t used = strlen(buf);
        snprintf(buf + used, size - used, "%s", &trait->namesym->namestr);
    }
    // An instance of a generic trait: the one made at the arguments where the
    // caller has it, else as written, 'Stack[T]'
    else if (genericNamedGenericTrait(clause->typ)) {
        if (inst)
            genericTypeNameCat(buf, size, (INode*)inst, 0);
        else
            genericTemplateNameCat(buf, size, clause->typ, 0);
    }
    else if (genericNamedType(clause->typ))
        genericTypeNameCat(buf, size, clause->typ, 0);
    else if (genericIsTypeInstance(clause->typ))
        genericTemplateNameCat(buf, size, clause->typ, 0);
    else {
        size_t used = strlen(buf);
        snprintf(buf + used, size - used, "?");
    }
}

// Append a condition to a where list
static void genericAddCondition(Nodes **wherep, INode *cond) {
    if (*wherep == NULL)
        *wherep = newNodes(4);
    nodesAdd(wherep, cond);
}

// Is this one of 'parms'?
static int genericIsParmOf(INode *dcl, Nodes *parms) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(parms, cnt, nodesp)) {
        if (*nodesp == dcl)
            return 1;
    }
    return 0;
}

// Resolve and vet a condition's clauses: each subject a type parameter in
// scope -- the generic's own, or the type's whose member this is -- that 'is'
// a trait. Every clause is resolved and reported; 0 if any is refused.
// 'ownparms' is NULL for a 'where' clause; for the condition on an 'is' entry
// it is the type's parameters, the only ones such a condition may ask about.
static int genericConditionNameRes(NameResState *pstate, INode *cond, Nodes *ownparms) {
    if (cond->tag == OrLogicTag || cond->tag == AndLogicTag) {
        int lok = genericConditionNameRes(pstate, ((LogicNode*)cond)->lexp, ownparms);
        int rok = genericConditionNameRes(pstate, ((LogicNode*)cond)->rexp, ownparms);
        return lok && rok;
    }
    CastNode *clause = (CastNode*)cond;
    inodeNameRes(pstate, &clause->exp);
    inodeNameRes(pstate, &clause->typ);
    genericAssocBareUse(&clause->typ);
    NameUseNode *subject = (NameUseNode*)clause->exp;
    int ok = 1;
    if (subject->dclnode == NULL)
        ok = 0;   // reported as unknown where it was resolved
    else if (ownparms && (subject->dclnode->tag != GenVarDclTag || !genericIsParmOf(subject->dclnode, ownparms))) {
        errorMsgNode(clause->exp, ErrorWhereSubject,
            "A condition on an 'is' entry asks about one of the type's own type parameters, since each instance is decided by its arguments, and %s is not one.",
            &subject->namesym->namestr);
        ok = 0;
    }
    else if (subject->dclnode->tag == GenVarDclTag && ((GenVarDclNode*)subject->dclnode)->valtype) {
        errorMsgNode(clause->exp, ErrorGenValueParm,
            "%s is a value parameter, a number, and a 'where' clause asks what a type is or fits.",
            &subject->namesym->namestr);
        ok = 0;
    }
    else if (subject->dclnode->tag != GenVarDclTag) {
        errorMsgNode(clause->exp, ErrorWhereSubject,
            "A 'where' clause constrains a type parameter of this generic, or of the generic type it is a member of, and %s is not one.",
            &subject->namesym->namestr);
        ok = 0;
    }
    // A function signature, 'F is fn(a: &T) i32', is a constraint's to name; in a
    // condition on an 'is' entry it is not built
    if (clause->typ->tag == FnSigTag) {
        if (ownparms) {
            errorMsgNode(clause->typ, ErrorUnbuiltIsCond,
                "A condition on an 'is' entry asks whether a type parameter is a trait or a type; a function signature, 'fn(...)', is not built there yet.");
            ok = 0;
        }
    }
    // A generic trait's instance, 'Stack[T]', is a constraint's to name; in a
    // condition on an 'is' entry it is not built
    else if (ownparms == NULL && genericNamedGenericTrait(clause->typ)) {
        if (!genericTraitInstanceVet(clause->typ))
            ok = 0;
    }
    // A type written as an instance is met by that type alone, in a 'where'
    // clause; in a condition on an 'is' entry it is not built
    else if (genericNamedTrait(clause->typ) == NULL && genericNamedType(clause->typ) == NULL
        && !(ownparms == NULL && genericIsTypeInstance(clause->typ))) {
        StructNode *bare = ownparms ? NULL : genericNamedBareGenericTrait(clause->typ);
        if (bare)
            errorMsgNode(clause->typ, ErrorWhereTrait,
                "%s is a generic trait, and a constraint names an instance of it, with its type arguments: %s[T]. (A bare generic trait names no trait.)",
                &bare->namesym->namestr, &bare->namesym->namestr);
        else if (!isNameUseNode(clause->typ) || ((NameUseNode*)clause->typ)->dclnode != NULL)
            errorMsgNode(clause->typ, ErrorWhereTrait, ownparms
                ? "What a type parameter 'is' in a condition on an 'is' entry is a trait or a type, and this is neither. (A generic trait's or type's instance is not built in a condition yet.)"
                : "What a type parameter 'is' in a 'where' clause is a trait or a type, and this is neither. (A generic type is named with its arguments: Option[i64].)");
        ok = 0;
    }
    return ok;
}

int genericIsConditionNameRes(NameResState *pstate, Nodes *parms, INode *cond) {
    return genericConditionNameRes(pstate, cond, parms);
}

void genericConstraintsNameRes(NameResState *pstate, Nodes *parms, Nodes **wherep) {
    INode **nodesp;
    uint32_t cnt;
    Nodes *clauses = NULL;

    // What follows a parameter's name, in the order written: '[T A + B]' says
    // what 'where T is A && T is B' says. The slot is one for everything a
    // parameter may be annotated with, and what the name resolves to is what
    // it means: a trait constrains a type parameter; a type would make a value
    // parameter and a kind another kind of parameter, and neither is built.
    if (parms) {
        for (nodesFor(parms, cnt, nodesp)) {
            GenVarDclNode *parm = (GenVarDclNode*)*nodesp;
            if (parm->annot == NULL)
                continue;
            // A number type makes a value parameter, '[N usize]': the type
            // alone, an integer one
            INode *first = nodesGet(parm->annot, 0);
            INode *firstdcl = isNameUseNode(first) && isTypeNode(first) ? itypeGetTypeDcl(first) : NULL;
            if (firstdcl && (firstdcl->tag == IntNbrTag || firstdcl->tag == UintNbrTag || firstdcl->tag == FloatNbrTag)) {
                if (firstdcl->tag == FloatNbrTag || firstdcl == (INode*)boolType || firstdcl == (INode*)charType)
                    errorMsgNode(first, ErrorGenValueParm,
                        "A value parameter is an integer: %s is not an integer type, so %s is not built as a value parameter.",
                        &((NbrNode*)firstdcl)->namesym->namestr, &parm->namesym->namestr);
                else if (parm->annot->used > 1)
                    errorMsgNode(nodesGet(parm->annot, 1), ErrorGenValueParm,
                        "A value parameter takes its integer type alone, '[%s %s]', and no constraint beside it.",
                        &parm->namesym->namestr, &((NbrNode*)firstdcl)->namesym->namestr);
                else
                    parm->valtype = firstdcl;
                continue;
            }
            INode **annotp;
            uint32_t annotcnt;
            for (nodesFor(parm->annot, annotcnt, annotp)) {
                genericAssocBareUse(annotp);
                if ((*annotp)->tag == FnSigTag) {
                    // A signature bound, decided by evaluation (genericSigMeets)
                }
                else if (genericNamedGenericTrait(*annotp)) {
                    if (!genericTraitInstanceVet(*annotp))
                        continue;
                }
                else if (genericNamedActorTrait(*annotp)) {
                    // An actor trait's handle, or an instance of a generic one: met by an
                    // actor that conforms, decided by evaluation (actorTraitMeets)
                }
                else if (genericNamedTrait(*annotp) == NULL) {
                    // A name that bound nothing was reported where it was resolved
                    StructNode *bare = genericNamedBareGenericTrait(*annotp);
                    if (bare)
                        errorMsgNode(*annotp, ErrorGenParmConstr,
                            "%s is a generic trait, and what follows the type parameter %s names an instance of it, with its type arguments: %s[T]. (A bare generic trait names no trait.)",
                            &bare->namesym->namestr, &parm->namesym->namestr, &bare->namesym->namestr);
                    else if (!isNameUseNode(*annotp) || ((NameUseNode*)*annotp)->dclnode != NULL)
                        errorMsgNode(*annotp, ErrorGenParmConstr,
                            "What follows the type parameter %s constrains it, so it names a trait, and this is not one. (A value parameter's annotation is an integer type, '[N usize]'; a parameter of another kind is not built yet.)",
                            &parm->namesym->namestr);
                    continue;
                }
                CastNode *clause = newIsNode(newNameUseFromDclNode((INode*)parm, *annotp), *annotp);
                inodeLexCopy((INode*)clause, *annotp);
                genericAddCondition(&clauses, (INode*)clause);
            }
        }
    }

    // Each written condition, kept whole or, where any of its clauses is
    // refused, dropped whole: a '||' missing one side would say something else
    if (*wherep) {
        for (nodesFor(*wherep, cnt, nodesp)) {
            if (genericConditionNameRes(pstate, *nodesp, NULL))
                genericAddCondition(&clauses, *nodesp);
        }
    }
    *wherep = clauses;
    // A bound naming a trait with associated types may say one the generic's
    // code names through the parameter, 'C.Cursor'
    if (clauses) {
        for (nodesFor(clauses, cnt, nodesp))
            genericAssocBoundNote(*nodesp);
    }
}

// A generic type takes type parameters only. A value parameter on one is
// refused: its instances, its literals, and the type-or-value reading of
// 'Name[3]' are not built for a number.
void genericRefuseValueParms(Nodes *parms, Name *typename) {
    INode **nodesp;
    uint32_t cnt;
    if (parms == NULL)
        return;
    for (nodesFor(parms, cnt, nodesp)) {
        GenVarDclNode *parm = (GenVarDclNode*)*nodesp;
        if (parm->valtype)
            errorMsgNode(parm->annot ? nodesGet(parm->annot, 0) : *nodesp, ErrorGenValueParm,
                "%s is a value parameter, and a generic type does not take one yet: only a generic function or method does. (%s takes type parameters.)",
                &parm->namesym->namestr, &typename->namestr);
    }
}

// The names of the generic functions and methods that take a value parameter.
// 's.asArrays[3]()' and 'a.items[3]' are the same shape until the receiver's
// type says whether the name is a generic method or a field; only a name in
// this list can be the first (fnCallMethodTypeArgs), so an index is not
// mistaken for type arguments anywhere else.
static Name **genericValueFnNames = NULL;
static uint32_t genericValueFnCount = 0;
static uint32_t genericValueFnCap = 0;

void genericValueFnNote(Nodes *parms, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    int hasvalue = 0;
    if (parms == NULL || name == NULL)
        return;
    for (nodesFor(parms, cnt, nodesp))
        if (((GenVarDclNode*)*nodesp)->valtype)
            hasvalue = 1;
    if (!hasvalue || genericValueFnNamed(name))
        return;
    if (genericValueFnCount == genericValueFnCap) {
        uint32_t newcap = genericValueFnCap ? genericValueFnCap * 2 : 8;
        Name **grown = memAllocBlk(newcap * sizeof(Name*));
        if (genericValueFnCount)
            memcpy(grown, genericValueFnNames, genericValueFnCount * sizeof(Name*));
        genericValueFnNames = grown;
        genericValueFnCap = newcap;
    }
    genericValueFnNames[genericValueFnCount++] = name;
}

int genericValueFnNamed(Name *name) {
    for (uint32_t i = 0; i < genericValueFnCount; ++i)
        if (genericValueFnNames[i] == name)
            return 1;
    return 0;
}

// Does this generic take a value parameter?
int genericHasValueParm(GenericInfo *info) {
    INode **nodesp;
    uint32_t cnt;
    if (info == NULL || info->parms == NULL)
        return 0;
    for (nodesFor(info->parms, cnt, nodesp))
        if (((GenVarDclNode*)*nodesp)->valtype)
            return 1;
    return 0;
}

// Does this trait require nothing of a value -- no method, no field? Such a
// trait is a marker: every type would fit it structurally, so fitting it says
// nothing, and only a declaration or the compiler's grant makes a type one.
// A static function says nothing about a value either, so it does not count.
static int genericTraitIsMarker(StructNode *trait) {
    if (corelibIsBuiltinTrait((INode*)trait))
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag && ((*nodesp)->flags & FlagMethFld))
            return 0;
    }
    for (nodelistFor(&trait->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsMixin))
            return 0;
    }
    return 1;
}

// Does this type's 'is' list name the trait, or name a trait that names it?
static int genericDeclares(StructNode *type, StructNode *trait, int depth) {
    if (type->traits == NULL || depth > 8)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(type->traits, cnt, nodesp)) {
        if (*nodesp == (INode*)trait
            || ((*nodesp)->tag == StructTag && genericDeclares((StructNode*)*nodesp, trait, depth + 1)))
            return 1;
    }
    return 0;
}

// Fitting a trait structurally compares signatures, which have their types
// only once checked. Each requirement of the trait, and each candidate of the
// type's for it, is analyzed first, as a call reaching them would.
static void genericDemandMatch(StructNode *trait, StructNode *type) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        // A generic default is no requirement: nothing is compared of it, and its
        // signature (a step naming 'Self') is made only where it is called. Nor is
        // an 'inline' one, which fills no slot of the trait either
        if (((FnDclNode*)*nodesp)->value
            && (((FnDclNode*)*nodesp)->genericinfo || (((FnDclNode*)*nodesp)->flags & FlagInline)))
            continue;
        if (!((*nodesp)->flags & (TypeChecked | TypeChecking))) {
            TypeCheckState tstate;
            tstate.typenode = (INode*)trait;
            tstate.fn = NULL;
            tstate.scope = 0;
            tstate.extend = NULL;
            inodeTypeCheckAny(&tstate, nodesp);
        }
        INode *binding = namespaceFind(&type->namespace, ((FnDclNode*)*nodesp)->namesym);
        if (binding)
            fnCallDemandCandidates(binding);
    }
}

// Does this number type fit the trait by its built-in methods? A marker trait
// is met only by the compiler's grants (decided by the caller), so it is not
// fitted. Each method the trait requires (not a default) must be met by the
// number's method of that name, or of the operator name the requirement
// overloads (iNsTypeNumberMeets). A trait with a field is not met.
static int genericNumberFits(StructNode *trait, NbrNode *nbr) {
    if (genericTraitIsMarker(trait))
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsMixin))
            return 0;
    }
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode*)*nodesp;
        if (meth->value)
            continue;
        if (!((*nodesp)->flags & (TypeChecked | TypeChecking))) {
            TypeCheckState tstate;
            tstate.typenode = (INode*)trait;
            tstate.fn = NULL;
            tstate.scope = 0;
            tstate.extend = NULL;
            inodeTypeCheckAny(&tstate, nodesp);
        }
        INode *binding = namespaceFind(&nbr->namespace, meth->namesym);
        int fits = binding && iNsTypeNumberMeets(binding, meth, (INode*)nbr, (INode*)trait);
        if (!fits && meth->overloadsym) {
            binding = namespaceFind(&nbr->namespace, meth->overloadsym);
            fits = binding && iNsTypeNumberMeets(binding, meth, (INode*)nbr, (INode*)trait);
        }
        if (!fits)
            return 0;
    }
    return 1;
}

// Is this reference an owner of core's 'str' (Uni[str], Rc[str]: any region but
// a borrow's)?
static int genericIsTextOwner(RefNode *ref) {
    return itypeGetTypeDcl(ref->region) != borrowRef && refIsFat(ref)
        && itypeGetTypeDcl(ref->vtexp) == (INode*)strTypeDcl;
}

// Is the type 'trait', a borrow that lives for the whole program let cross as
// 'how' says where the trait is Sendable?
static int genericTypeIsHow(INode *type, StructNode *trait, StaticBorrow how) {
    INode *dcl = itypeGetTypeDcl(type);
    // The compiler's grants: every type is exactly one of Move and Copy, the
    // integer types of 8 to 64 bits, signed and unsigned, are Integer, and the
    // raw pointer types, '*T', are Pointer
    if (trait == moveTrait)
        return itypeIsMove(dcl);
    if (trait == copyTrait)
        return !itypeIsMove(dcl);
    if (trait == integerTrait
        && (dcl->tag == IntNbrTag || (dcl->tag == UintNbrTag && dcl != (INode*)boolType && dcl != (INode*)charType)))
        return 1;
    if (trait == pointerTrait && dcl->tag == PtrTag)
        return 1;
    // core's Hash is granted to the integers and bool, which feed their bits
    // (nbrAddHashMethods). Not to a float: NaN is not equal to itself and -0 is
    // equal to 0, so no hash of a float's bits agrees with its '=='
    if ((dcl->tag == IntNbrTag || dcl->tag == UintNbrTag) && coreIsHashTrait((INode*)trait))
        return 1;
    // and to an owner of text, Uni[str] and Rc[str]: its '==' compares the
    // bytes, and the 'hash' a call on it reaches is str's, which feeds them
    if (dcl->tag == RefTag && coreIsHashTrait((INode*)trait) && genericIsTextOwner((RefNode*)dcl))
        return 1;
    // Sendable is the thread check's: granted to every type holding nothing
    // bound to its thread, and to a type declaring it, on its word, where its
    // type arguments are Sendable. A borrow of the whole program is let cross
    // only where the generic bounds its parameter by ''static', which each call
    // is checked to meet
    if (trait == sendableTrait)
        return !itypeThreadBoundHow(dcl, NULL, how);
    // Shareable is the share check's: granted to every type holding nothing a
    // borrow of it could not share between threads
    if (trait == shareableTrait)
        return !itypeNotShareable(dcl);
    // Sized and DynSized are the type's size: known at compile time, or known
    // at compile time or carried by a reference to it. A type cannot declare
    // either.
    if (trait == sizedTrait)
        return itypeIsSized(dcl);
    if (trait == dynSizedTrait)
        return itypeIsDynSized(dcl);
    // A number type fits a trait with methods by its built-in ones (the
    // three-way '<=>', '==', '+'), as a struct does by its declared ones.
    // INTERIM: a trait cannot hold 'Self' by value (Error 1069), so it writes
    // '&Self', and a number's methods take the number by value. This branch lets
    // the by-value method meet a '&Self' requirement, for number types only. The
    // general fix is a by-value Self in traits, which Digester.finish wants too;
    // then this branch can go. It is not widened to Copy structs.
    if (dcl->tag == IntNbrTag || dcl->tag == UintNbrTag || dcl->tag == FloatNbrTag)
        return genericNumberFits(trait, (NbrNode*)dcl);
    if (dcl->tag != StructTag)
        return 0;
    StructNode *strnode = (StructNode*)dcl;
    if (strnode == trait || genericDeclares(strnode, trait, 0))
        return 1;
    if (genericTraitIsMarker(trait))
        return 0;
    // Fitting it: 'structMatches' is the structural subtype test a trait's
    // uses are made by, under the constraint monomorphization asks for. What
    // it compares is the type's members, so an instance's are decided first
    if (strnode->latewhere)
        genericLateWhereNow(strnode);
    genericDemandMatch(trait, strnode);
    return structMatches(trait, dcl, Monomorph) != NoMatch;
}

int genericTypeIs(INode *type, StructNode *trait) {
    return genericTypeIsHow(type, trait, StaticOff);
}

// The generic function whose requirements are being decided: a parameter it
// bounds by ''static' is handed only borrows of the whole program, which
// Sendable then lets cross
static INode *genericWhereOwner = NULL;

static StaticBorrow genericStaticHow(StructNode *trait, GenVarDclNode *parm) {
    if (trait == sendableTrait && parm && genericWhereOwner
        && lifeParmStaticBounded(genericWhereOwner, parm->namesym))
        return StaticVouched;
    return StaticOff;
}

// What a condition comes to at an instance's arguments. Unknown is a
// condition turning on a parameter not bound there. Late is a clause that asks
// what a type is -- a trait it meets, a signature -- while the question is
// held back (genericLayoutUnasked): only genericMemberWhereNow sees it.
typedef enum {
    WhereFalse,
    WhereTrue,
    WhereUnknown,
    WhereLate
} WhereValue;

// Set while a member's clauses are decided as far as they can be without asking
// any type what it is (genericMemberWhereNow)
static int genericLayoutUnasked = 0;

// The use that asked for the instance a condition is being decided for, and the
// walk it was asked in, which an instance of a clause's generic trait is made
// under (genericClauseTrait). Both are NULL when the condition is decided
// where no use is being checked, to explain a refusal.
static INode *genericCondSite = NULL;
static TypeCheckState *genericCondState = NULL;

// What a clause names written out as an instance, 'Stack[T]' or 'Uni[str]', at
// these arguments: a clone of it with the parameters substituted, checked as
// the same spelling in a signature is, so that it is the instance the program's
// other uses of it name -- 'Stack[i64]' where T is i64. NULL if it could not be
// made, which was reported where it failed.
// The use of a trait with associated types whose subject did not say them, as
// the last clause asked made it at its arguments, for a refusal to name
static INode *genericAssocUnfitUse = NULL;

// A trait with associated types, 'Iterable[T]', is made at the arguments with
// what 'subject' says they are (genericAssocComplete). Where it does not say
// them, NULL, and '*unfit' set: the subject is not the trait.
static INode *genericClauseCloneFor(CastNode *clause, Nodes *parms, Nodes *args, INode *subject, int *unfit) {
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, genericCondSite ? genericCondSite : clause->typ, NULL, 0, parms, args);
    INode *copy = cloneNode(&cstate, clause->typ);
    clonePopState();
    cloneDclPop(dclpos);
    TypeCheckState fresh;
    TypeCheckState *tstate = genericCondState;
    if (tstate == NULL) {
        fresh.typenode = NULL;
        fresh.fn = NULL;
        fresh.scope = 0;
        fresh.extend = NULL;
        tstate = &fresh;
    }
    // Whatever the check instantiates decides its own conditions in full
    int svunasked = genericLayoutUnasked;
    genericLayoutUnasked = 0;
    StructNode *assoctrait = genericAssocTraitOf(copy);
    if (assoctrait && subject && (((FnCallNode*)copy)->args ? ((FnCallNode*)copy)->args->used : 0)
            == genericWrittenCount(assoctrait->genericinfo)) {
        if (!genericAssocCheckArgs(tstate, (FnCallNode*)copy)) {
            genericLayoutUnasked = svunasked;
            return NULL;
        }
        if (!genericAssocComplete((FnCallNode*)copy, subject)) {
            genericLayoutUnasked = svunasked;
            genericAssocUnfitUse = copy;
            if (unfit)
                *unfit = 1;
            return NULL;
        }
    }
    inodeTypeCheckAny(tstate, &copy);
    genericLayoutUnasked = svunasked;
    if (inodeIsError(copy) || !isTypeNode(copy))
        return NULL;
    return copy;
}

static INode *genericClauseCloneChecked(CastNode *clause, Nodes *parms, Nodes *args) {
    return genericClauseCloneFor(clause, parms, args, NULL, NULL);
}

// The trait a clause says its subject is, at these arguments: the trait it
// names, or, where it names an instance of a generic trait, that instance.
// NULL if the clause names no trait, or the instance could not be made. A trait
// with associated types is made with what 'subject' says they are, and where it
// does not say them, NULL with '*unfit' set.
static StructNode *genericClauseTraitFor(CastNode *clause, Nodes *parms, Nodes *args, INode *subject, int *unfit) {
    StructNode *trait = genericNamedTrait(clause->typ);
    if (trait || genericNamedGenericTrait(clause->typ) == NULL)
        return trait;
    INode *copy = genericClauseCloneFor(clause, parms, args, subject, unfit);
    if (copy == NULL)
        return NULL;
    INode *dcl = itypeGetTypeDcl(copy);
    if (dcl->tag != StructTag || !(dcl->flags & TraitType) || ((StructNode*)dcl)->genericinfo)
        return NULL;
    return (StructNode*)dcl;
}

// The type a clause asks about at these arguments: the argument, where its
// subject is one of 'parms' (and '*parmp' is set to it); the subject itself,
// where the clone that copied the clause substituted it; or NULL, where it is
// another parameter, bound elsewhere
static INode *genericClauseType(CastNode *clause, Nodes *parms, Nodes *args, GenVarDclNode **parmp) {
    INode *subject = clause->exp;
    *parmp = NULL;
    if (isNameUseNode(subject)) {
        INode *dcl = ((NameUseNode*)subject)->dclnode;
        if (dcl && dcl->tag == GenVarDclTag) {
            for (uint32_t j = 0; parms && args && j < parms->used && j < args->used; ++j) {
                if (nodesGet(parms, j) == dcl) {
                    *parmp = (GenVarDclNode*)dcl;
                    return nodesGet(args, j);
                }
            }
            return NULL;
        }
    }
    return isTypeNode(subject) ? subject : NULL;
}

// genericClauseTraitFor with the clause's own subject at these arguments
static StructNode *genericClauseTrait(CastNode *clause, Nodes *parms, Nodes *args) {
    GenVarDclNode *parm;
    return genericClauseTraitFor(clause, parms, args, genericClauseType(clause, parms, args, &parm), NULL);
}

// ---------------------------------------------------------------------------
// A function signature as a bound, 'F: fn(a: &T, b: &T) i32'. It names an ad hoc
// structural trait: a type meets it by having a pub '()' method whose
// parameters after the receiver, and whose return type, meet the signature's
// (parameter names do not matter), and a function type, as a plain function
// referenced is, by meeting that signature. Meeting is the one rule every
// callable meets a signature by (fnSigMeets): the signature's own types, or a
// recast's difference from them.

static void genericSigCat(char *buf, size_t size, FnDclNode *fn);

// Does this method meet the signature, after its receiver?
static int genericMethodTakesSig(FnDclNode *meth, FnSigNode *sig) {
    if (meth->vtype == NULL || meth->genericinfo || !(meth->flags & FlagMethFld))
        return 0;
    FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    return msig->tag == FnSigTag && fnSigMeetsMethod(msig, sig);
}

// The candidates for '()' a type has, one FnDclNode at a time: candidate 'i' of
// the binding, or NULL past the last
static FnDclNode *genericParensCandidate(INode *binding, uint32_t i) {
    if (binding->tag == FnDclTag)
        return i == 0 ? (FnDclNode*)binding : NULL;
    if (binding->tag == FnOverloadDclTag) {
        Nodes *overloads = ((FnOverloadDclNode*)binding)->overloads;
        return i < overloads->used ? (FnDclNode*)nodesGet(overloads, i) : NULL;
    }
    return NULL;
}

// The pub '()' method a struct has for this signature, or NULL
static FnDclNode *genericParensMethod(StructNode *strnode, FnSigNode *sig) {
    INode *binding = namespaceFind(&strnode->namespace, parensName);
    if (binding == NULL)
        return NULL;
    fnCallDemandCandidates(binding);
    FnDclNode *cand;
    for (uint32_t i = 0; (cand = genericParensCandidate(binding, i)) != NULL; ++i) {
        if ((cand->flags & FlagPub) && genericMethodTakesSig(cand, sig))
            return cand;
    }
    return NULL;
}

// The signature of a function type, or of a function reference type: what a
// plain function referenced as '&byName' is the argument as ('&F' reaching
// the function type) or, taken by value, the reference itself. NULL for any
// other type.
static FnSigNode *genericFnTypeSig(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag == RefTag) {
        INode *target = ((RefNode*)dcl)->vtexp;
        dcl = target && isTypeNode(target) ? itypeGetTypeDcl(target) : NULL;
    }
    return dcl && dcl->tag == FnSigTag ? (FnSigNode*)dcl : NULL;
}

// Does this type meet the signature, a checked one?
static int genericSigMeets(INode *type, FnSigNode *sig) {
    FnSigNode *fnsig = genericFnTypeSig(type);
    if (fnsig)
        return fnSigMeets(fnsig, sig);
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != StructTag)
        return 0;
    return genericParensMethod((StructNode*)dcl, sig) != NULL;
}

// A permission that is the default is not written in a signature the message
// names: '&ro Person' is '&Person'
static void genericDropRo(char *buf) {
    char *at;
    while ((at = strstr(buf, "&ro ")) != NULL)
        memmove(at + 1, at + 4, strlen(at + 4) + 1);
}

// Append a checked signature as it comes to: 'fn(&Person, &Person) i32'
void genericFnSigCat(char *buf, size_t size, FnSigNode *sig) {
    size_t used = strlen(buf);
    snprintf(buf + used, size - used, sig->ccall ? "fn @c (" : "fn(");
    for (uint32_t j = 0; j < sig->parms->used; ++j) {
        used = strlen(buf);
        snprintf(buf + used, size - used, j ? ", " : "");
        genericTypeNameCat(buf, size, iexpGetTypeDcl(nodesGet(sig->parms, j)), 0);
    }
    used = strlen(buf);
    snprintf(buf + used, size - used, ")");
    if (sig->rettype && sig->rettype->tag != VoidTag) {
        used = strlen(buf);
        snprintf(buf + used, size - used, " ");
        genericTypeNameCat(buf, size, sig->rettype, 0);
    }
    genericDropRo(buf);
}

// Why a type does not meet the signature, into 'buf': what it has instead.
// Answers true where the reason is a parameter or the result held by value that
// is not one layout (fnSigLayoutDiff), which is reported under its own code
static int genericSigMeetWhy(char *buf, size_t size, INode *type, FnSigNode *sig) {
    buf[0] = '\0';
    INode *dcl = itypeGetTypeDcl(type);
    char wanted[256] = "";
    genericFnSigCat(wanted, sizeof(wanted), sig);
    FnSigLayoutDiff diff;
    char layout[700];
    FnSigNode *fnsig = genericFnTypeSig(type);
    if (fnsig) {
        char have[256] = "";
        genericFnSigCat(have, sizeof(have), fnsig);
        if (fnSigLayoutDiff(fnsig, sig, NULL, ReceiverNone, 1, &diff)) {
            fnSigLayoutSpell(layout, sizeof(layout), &diff, "the bound", "the bound");
            snprintf(buf, size, " A function must meet %s; this one is %s, and %s Write a wrapper function that converts, and use that.",
                wanted, have, layout);
            return 1;
        }
        snprintf(buf, size, " A function must meet %s (its types, or a recast's difference from them); this one is %s.", wanted, have);
        return 0;
    }
    char typename[256] = "";
    genericTypeNameCat(typename, sizeof(typename), type, 0);
    if (dcl->tag != StructTag) {
        snprintf(buf, size, " %s is neither a function nor a struct with a '()' method.", typename);
        return 0;
    }
    INode *binding = namespaceFind(&((StructNode*)dcl)->namespace, parensName);
    FnDclNode *cand = binding ? genericParensCandidate(binding, 0) : NULL;
    if (cand == NULL) {
        snprintf(buf, size, " %s has no '()' method, and %s needs a pub one taking those parameters after its receiver.",
            typename, wanted);
        return 0;
    }
    fnCallDemandCandidates(binding);
    char found[256] = "";
    for (uint32_t i = 0; (cand = genericParensCandidate(binding, i)) != NULL; ++i) {
        if (cand->genericinfo || !(cand->flags & FlagMethFld))
            continue;
        if (genericMethodTakesSig(cand, sig) && !(cand->flags & FlagPub)) {
            snprintf(buf, size, " %s has a '()' method of that signature, but it is not pub.", typename);
            return 0;
        }
        FnSigNode *msig = cand->vtype ? (FnSigNode*)itypeGetTypeDcl(cand->vtype) : NULL;
        if ((cand->flags & FlagPub) && msig && msig->tag == FnSigTag && fnSigLayoutDiff(msig, sig, NULL, ReceiverImpl, 0, &diff)) {
            fnSigLayoutSpell(layout, sizeof(layout), &diff, "the bound", "the bound");
            snprintf(buf, size, " The `()` of %s is refused for %s: %s Write a wrapper function that converts, and use that.",
                typename, wanted, layout);
            return 1;
        }
        size_t used = strlen(found);
        if (found[0])
            snprintf(found + used, sizeof(found) - used, " and ");
        genericSigCat(found, sizeof(found), cand);
    }
    genericDropRo(found);
    if (found[0] == '\0')
        snprintf(buf, size, " %s has '()' only as a generic method, which does not meet a signature bound.", typename);
    else
        snprintf(buf, size, " %s has %s, but %s needs a pub '()' taking those parameters after its receiver and returning that type, or a recast's difference from it.",
            typename, found, wanted);
    return 0;
}

// A bound is met by what the generic's own code could name through the type: the
// body is what calls the member, and a private member counts for code inside its
// module only (structMemberHidden), so the asker while a bound is decided is the
// generic, not the use instantiating it. A type passing the bound from where the
// use sits would then fail in the body, on a member the body cannot name. This
// stands the generic in as the code asking, and puts the real one back.
typedef struct {
    TypeCheckState state;
    TypeCheckState *saved;
} GenericAsker;

static void genericAskerEnter(GenericAsker *asker, INode *generic) {
    asker->saved = inodeCheckState;
    asker->state.typenode = NULL;
    asker->state.fn = NULL;
    asker->state.scope = 0;
    asker->state.extend = NULL;
    if (generic && generic->tag == FnDclTag)
        asker->state.fn = (FnDclNode*)generic;
    else if (generic)
        asker->state.typenode = generic;
    if (asker->state.fn || asker->state.typenode)
        inodeCheckState = &asker->state;
}

static void genericAskerLeave(GenericAsker *asker) {
    inodeCheckState = asker->saved;
}

// Evaluate a condition at these arguments, '||' and '&&' as in an expression,
// the right side only where the left does not decide it. A lone clause is
// decided only where its subject is one of 'parms': a clause over a parameter
// bound already was decided then. Inside a '||' or a '&&', a subject the
// clone substituted is asked as it stands, since the whole is decided here.
static WhereValue genericConditionValue(INode *cond, Nodes *parms, Nodes *args, int nested) {
    if (cond->tag == OrLogicTag || cond->tag == AndLogicTag) {
        WhereValue decides = cond->tag == OrLogicTag ? WhereTrue : WhereFalse;
        WhereValue lval = genericConditionValue(((LogicNode*)cond)->lexp, parms, args, 1);
        if (lval == decides)
            return decides;
        WhereValue rval = genericConditionValue(((LogicNode*)cond)->rexp, parms, args, 1);
        if (rval == decides)
            return decides;
        if (lval == WhereLate || rval == WhereLate)
            return WhereLate;
        return lval == WhereUnknown || rval == WhereUnknown ? WhereUnknown : rval;
    }
    GenVarDclNode *parm;
    INode *type = genericClauseType((CastNode*)cond, parms, args, &parm);
    // A signature is made at these arguments, as an instance written out is,
    // and met by a function of it or a struct with a '()' of it
    if (((CastNode*)cond)->typ->tag == FnSigTag) {
        if (type == NULL || (parm == NULL && !nested))
            return WhereUnknown;
        if (genericLayoutUnasked)
            return WhereLate;
        INode *sig = genericClauseCloneChecked((CastNode*)cond, parms, args);
        if (sig == NULL)
            return WhereUnknown;
        return genericSigMeets(type, (FnSigNode*)itypeGetTypeDcl(sig)) ? WhereTrue : WhereFalse;
    }
    StructNode *trait = genericNamedTrait(((CastNode*)cond)->typ);
    // An instance of a generic trait is made at these arguments, once the
    // clause is known to be decided here
    if (trait == NULL && genericNamedGenericTrait(((CastNode*)cond)->typ)) {
        if (type == NULL || (parm == NULL && !nested))
            return WhereUnknown;
        if (genericLayoutUnasked)
            return WhereLate;
        // A type that does not say what the trait's associated types are is not it
        int unfit = 0;
        trait = genericClauseTraitFor((CastNode*)cond, parms, args, type, &unfit);
        if (trait == NULL)
            return unfit ? WhereFalse : WhereUnknown;
    }
    INode *named = trait ? NULL : genericNamedType(((CastNode*)cond)->typ);
    // A type written as an instance, 'Uni[str]', is made at these arguments and
    // met by itself alone
    if (trait == NULL && named == NULL && genericIsTypeInstance(((CastNode*)cond)->typ)) {
        if (type == NULL || (parm == NULL && !nested))
            return WhereUnknown;
        INode *inst = genericClauseCloneChecked((CastNode*)cond, parms, args);
        if (inst == NULL)
            return WhereUnknown;
        return itypeIsSame(type, inst) || actorTraitMeets(type, inst) ? WhereTrue : WhereFalse;
    }
    if (type == NULL || (trait == NULL && named == NULL) || (parm == NULL && !nested))
        return WhereUnknown;
    if (trait && genericLayoutUnasked)
        return WhereLate;
    if (trait)
        return genericTypeIsHow(type, trait, genericStaticHow(trait, parm)) ? WhereTrue : WhereFalse;
    // A type is met by itself alone, but for an actor trait's handle, which an
    // actor that conforms meets too
    return itypeIsSame(type, named) || actorTraitMeets(type, named) ? WhereTrue : WhereFalse;
}

// One condition of a generic's, decided as the generic's own code would ask it
static WhereValue genericEntryValue(INode *generic, INode *cond, Nodes *parms, Nodes *args) {
    GenericAsker asker;
    genericAskerEnter(&asker, generic);
    WhereValue value = genericConditionValue(cond, parms, args, 0);
    genericAskerLeave(&asker);
    return value;
}

// The first condition of 'where' these arguments make false, or NULL if none
// does. One left unknown is decided where the parameter it turns on is bound.
static INode *genericUnmetCondition(INode *generic, Nodes *where, Nodes *parms, Nodes *args) {
    if (where == NULL || parms == NULL || args == NULL)
        return NULL;
    GenericAsker asker;
    genericAskerEnter(&asker, generic);
    INode *unmet = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if (genericConditionValue(*nodesp, parms, args, 0) == WhereFalse) {
            unmet = *nodesp;
            break;
        }
    }
    genericAskerLeave(&asker);
    return unmet;
}

// A member's 'where' at arguments still being laid out, decided as far as it can
// be without asking any type what it is: a clause naming a type is answered by
// that type alone ('K is String', 'K is Uni[str]'), whatever is in flight; a
// clause naming a trait or a signature waits (WhereLate), unless the rest of a
// '||' or a '&&' decides it. WhereFalse if a clause is false, WhereLate if one
// waits, else WhereTrue.
static WhereValue genericMemberWhereNow(INode *generic, Nodes *where, Nodes *parms, Nodes *args) {
    GenericAsker asker;
    genericAskerEnter(&asker, generic);
    int svunasked = genericLayoutUnasked;
    genericLayoutUnasked = 1;
    WhereValue value = WhereTrue;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        WhereValue clause = genericConditionValue(*nodesp, parms, args, 0);
        if (clause == WhereFalse) {
            value = WhereFalse;
            break;
        }
        if (clause == WhereLate)
            value = WhereLate;
    }
    genericLayoutUnasked = svunasked;
    genericAskerLeave(&asker);
    return value;
}

// Append a type's name, with an instance's type arguments, to 'buf'
static void genericTypeNameCat(char *buf, size_t size, INode *type, int depth) {
    INode *dcl = itypeGetTypeDcl(type);
    size_t used = strlen(buf);
    // A raw pointer by what it points at, as it is written: '*Node'
    if (dcl->tag == PtrTag && depth < 4 && isTypeNode(((StarNode*)dcl)->vtexp)) {
        snprintf(buf + used, size - used, "*");
        genericTypeNameCat(buf, size, ((StarNode*)dcl)->vtexp, depth + 1);
        return;
    }
    // A reference or an array as it is written: '&mut Point', 'Rc[imm, Pt]',
    // 'Array[u8, 3]'
    if (dcl->tag == RefTag || dcl->tag == ArrayRefTag || dcl->tag == VirtRefTag || dcl->tag == ArrayTag) {
        itypeSpellCat(buf, size, dcl, depth);
        return;
    }
    // (the body of a run-time length is already named with its element)
    snprintf(buf + used, size - used, "%s", itypeIsArrayBody(dcl) ? "Array" : itypeName(dcl));
    Nodes *args = dcl->tag == StructTag && depth < 4 ? itypeInstanceTypeArgs(dcl) : NULL;
    if (args == NULL)
        return;
    // A trait's instance is named as a use writes it, without its associated types
    uint32_t shown = args->used;
    StructNode *template = genericTemplateOf((StructNode*)dcl);
    if (template && genericHasAssoc(template->genericinfo))
        shown = genericWrittenCount(template->genericinfo);
    if (shown == 0)
        return;
    for (uint32_t i = 0; i < shown; ++i) {
        used = strlen(buf);
        snprintf(buf + used, size - used, i == 0 ? "[" : ", ");
        genericTypeNameCat(buf, size, nodesGet(args, i), depth + 1);
    }
    used = strlen(buf);
    snprintf(buf + used, size - used, "]");
}

// Append a type as a constraint writes it, before its parameters have
// arguments: a parameter by its name, an instance with its arguments written
// as they are ('Stack[T]'), anything else as the type it is
static void genericTemplateNameCat(char *buf, size_t size, INode *node, int depth) {
    size_t used = strlen(buf);
    if (isNameUseNode(node) && ((NameUseNode*)node)->dclnode
        && ((NameUseNode*)node)->dclnode->tag == GenVarDclTag) {
        snprintf(buf + used, size - used, "%s", &((NameUseNode*)node)->namesym->namestr);
        return;
    }
    if (node->tag == FnCallTag && isNameUseNode(((FnCallNode*)node)->objfn) && depth < 4) {
        FnCallNode *call = (FnCallNode*)node;
        snprintf(buf + used, size - used, "%s", &((NameUseNode*)call->objfn)->namesym->namestr);
        INode **nodesp;
        uint32_t cnt;
        if (call->args == NULL)
            return;
        for (nodesFor(call->args, cnt, nodesp)) {
            used = strlen(buf);
            snprintf(buf + used, size - used, cnt == call->args->used ? "[" : ", ");
            genericTemplateNameCat(buf, size, *nodesp, depth + 1);
        }
        used = strlen(buf);
        snprintf(buf + used, size - used, "]");
        return;
    }
    if (isTypeNode(node))
        genericTypeNameCat(buf, size, node, 0);
    else
        snprintf(buf + used, size - used, "?");
}

// Append a condition to 'buf' as it reads: 'T is A || T is B', a '||' inside
// a '&&' in parentheses ('grouped'), a substituted subject by its type's name
static void genericConditionCat(char *buf, size_t size, INode *cond, int grouped) {
    size_t used = strlen(buf);
    if (cond->tag == OrLogicTag || cond->tag == AndLogicTag) {
        int paren = grouped && cond->tag == OrLogicTag;
        int inand = cond->tag == AndLogicTag;
        if (paren)
            snprintf(buf + used, size - used, "(");
        genericConditionCat(buf, size, ((LogicNode*)cond)->lexp, inand);
        used = strlen(buf);
        snprintf(buf + used, size - used, cond->tag == OrLogicTag ? " or " : " and ");
        genericConditionCat(buf, size, ((LogicNode*)cond)->rexp, inand);
        used = strlen(buf);
        if (paren)
            snprintf(buf + used, size - used, ")");
        return;
    }
    INode *subject = ((CastNode*)cond)->exp;
    if (isNameUseNode(subject) && ((NameUseNode*)subject)->dclnode
        && ((NameUseNode*)subject)->dclnode->tag == GenVarDclTag)
        snprintf(buf + used, size - used, "%s", &((NameUseNode*)subject)->namesym->namestr);
    else if (isTypeNode(subject))
        genericTypeNameCat(buf, size, subject, 0);
    else
        snprintf(buf + used, size - used, "?");
    used = strlen(buf);
    snprintf(buf + used, size - used, " is ");
    genericClauseNameCat(buf, size, (CastNode*)cond, NULL);
}

// Does the condition ask about parameter 'parm'?
static int genericConditionNames(INode *cond, INode *parm) {
    if (cond->tag == OrLogicTag || cond->tag == AndLogicTag)
        return genericConditionNames(((LogicNode*)cond)->lexp, parm)
            || genericConditionNames(((LogicNode*)cond)->rexp, parm);
    INode *subject = ((CastNode*)cond)->exp;
    return isNameUseNode(subject) && ((NameUseNode*)subject)->dclnode == parm;
}

// Append each of 'parms' the condition asks about, with its argument: 'T = f64'
static void genericBindingsCat(char *buf, size_t size, INode *cond, Nodes *parms, Nodes *args) {
    int first = 1;
    for (uint32_t j = 0; j < parms->used && j < args->used; ++j) {
        if (!genericConditionNames(cond, nodesGet(parms, j)) || nodesGet(args, j) == NULL)
            continue;
        size_t used = strlen(buf);
        snprintf(buf + used, size - used, "%s%s = ", first ? "" : ", ",
            &((GenVarDclNode*)nodesGet(parms, j))->namesym->namestr);
        genericTypeNameCat(buf, size, nodesGet(args, j), 0);
        first = 0;
    }
    if (first)
        snprintf(buf, size, "these arguments");
}

// Why a type is not Sendable: into 'what', where the culprit sits in it
// ('Job.data is Rc[mut, Log],', or 'it is'), and into 'reason', what kind of
// thing binds it to its thread. Returns whether the cause is one most often met
// through a local -- a borrow or a permission -- whose own 'var' is not what is
// checked. Each buffer holds 512 bytes.
int genericNotSendableWhy(INode *arg, char *what, char *reason, StaticBorrow how) {
    char path[256];
    INode *culprit = itypeThreadBoundWhyHow(arg, path, sizeof(path), how);
    const size_t whatsize = 512, reasonsize = 512;
    what[0] = '\0';
    reason[0] = '\0';
    if (path[0] != '\0' && culprit) {
        snprintf(what, whatsize, "%s is ", path);
        itypeSpellCat(what, whatsize, culprit, 0);
        strcat(what, ",");
    }
    else
        snprintf(what, whatsize, "it is");
    int local = 0;
    INode *culpritdcl = culprit ? itypeGetTypeDcl(culprit) : NULL;
    if (culpritdcl == NULL)
        snprintf(reason, reasonsize, "a type bound to its thread");
    else if (culpritdcl->tag == PtrTag)
        snprintf(reason, reasonsize,
            "a raw pointer, whose target the compiler cannot check. A type holding raw pointers it shares safely across threads says so by declaring 'is Sendable', a promise the compiler takes on trust");
    else if (culpritdcl->tag == StructTag)
        snprintf(reason, reasonsize,
            "a trait, whose implementers are not all known here, so what a reference to one points at cannot be checked");
    else {
        RefNode *ref = (RefNode *)culpritdcl;
        INode *region = ref->region && isTypeNode(ref->region) ? itypeGetTypeDcl(ref->region) : NULL;
        char *regname = region && region->tag == StructTag ? &((StructNode *)region)->namesym->namestr : "its region";
        INode *perm = ref->perm && isTypeNode(ref->perm) ? itypeGetTypeDcl(ref->perm) : NULL;
        Name *permname = perm ? inodeGetName(perm) : NULL;
        switch (refThreadBinds(ref)) {
        case RefBindsBorrow:
        {
            StaticVerdict verdict = refStaticCrosses(ref, how);
            if (verdict == StaticBadPerm) {
                local = 1;
                snprintf(reason, reasonsize,
                    "a borrowed reference of permission %s, and a borrow crosses threads only if it lives for the whole program and is imm or opaq (any number of threads may read what it reaches, and none writes) or uni (it moves, so one holder has it). %s",
                    permname ? &permname->namestr : "?",
                    perm == (INode *)roPerm
                        ? "'ro' only stops this holder writing: another holder could still change what it reaches"
                        : "This permission lets a holder write while others may read");
            }
            else if (verdict == StaticInvariant)
                snprintf(reason, reasonsize,
                    "a borrow of an invariant lifetime, an arena's brand and not the whole program: the arena dies when its owner drops it, and a key reaches nothing without its arena, so a key may not leave its thread");
            else if (verdict == StaticUnwritten)
                snprintf(reason, reasonsize,
                    "a borrowed reference not written 'static: only a borrow written &'static imm, &'static opaq or &'static uni, which lives for the whole program, may be sent to another thread");
            else {
                local = 1;
                snprintf(reason, reasonsize,
                    "a borrowed reference, and no borrow may leave its thread: its lifetime is checked in that thread alone");
                if (how == StaticOff && (perm == (INode *)immPerm || perm == (INode *)opaqPerm || perm == (INode *)uniPerm))
                    strcat(reason,
                        ". A borrow that lives for the whole program does cross where the generic bounds that type parameter 'Sendable + 'static', and a borrow written &'static does anywhere");
            }
            break;
        }
        case RefBindsTraced:
            if (regionIsActorLocal(ref->region))
                snprintf(reason, reasonsize,
                    "a reference into %s, an actor's own region, which the actor's collector finds and moves between its messages, so the reference may not leave its actor",
                    regname);
            else
                snprintf(reason, reasonsize,
                    "a reference the %s collector traces, and a collector is single threaded, so a traced reference may not leave its thread",
                    regname);
            break;
        case RefBindsPerm:
            local = 1;
            snprintf(reason, reasonsize,
                "an owner whose permission, %s, is not race-safe: only a uni, imm or opaq reference may be shared with or sent to another thread",
                permname ? &permname->namestr : "?");
            break;
        case RefBindsShared:
            snprintf(reason, reasonsize,
                "an owner that may be copied, and %s does not declare ThreadSafe: its copies could not be made and dropped on several threads at once. It may cross as a uni owner, which moves it, or as an owner of a region declaring ThreadSafe, such as Arc",
                regname);
            break;
        default:
            snprintf(reason, reasonsize, "a reference bound to its thread");
            break;
        }
    }
    return local;
}

// Refuse an instance whose argument 'arg', for parameter 'parm', is not
// Sendable, saying what binds it to its thread and where that sits in it. A
// borrow or a permission is the cause most often met through a local, and a
// local's own 'var' is not what is checked, so the message says so.
static int genericFieldThreadBound(INode *type) {
    return itypeThreadBound(type, NULL);
}

static void genericNotSendableMsg(INode *errnode, Name *name, GenVarDclNode *parm, INode *arg, StaticBorrow how) {
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    char what[512], reason[512];
    // A closure: the variable it holds or borrows that binds it, and what to do about it
    ClosureCap *cap = closureFirstCap(arg, genericFieldThreadBound);
    if (cap != NULL) {
        char why[512], advice[600], captype[256] = "";
        genericNotSendableWhy(cap->field->vtype, what, why, how);
        itypeSpellCat(captype, sizeof(captype), cap->field->vtype, 0);
        closureCapAdvice(cap, !itypeThreadBound(closureCapValueType(cap), NULL), advice, sizeof(advice));
        errorMsgNode(errnode, ErrorNotSendable, "%s requires %s is Sendable, and this closure is not: it %s '%s' (%s), which is %s. %s",
            &name->namestr, &parm->namesym->namestr, cap->state ? "holds" : "borrows",
            &cap->dcl->namesym->namestr, captype, why, advice);
        return;
    }
    int local = genericNotSendableWhy(arg, what, reason, how);
    errorMsgNode(errnode, ErrorNotSendable, "%s requires %s is Sendable, and %s is not Sendable: %s %s.%s",
        &name->namestr, &parm->namesym->namestr, argname, what, reason,
        local ? " What is checked is the types of the references a value holds, not how a variable was declared: a local declared 'var x = 5' holds a number, which is Sendable." : "");
}

// What makes a type not Shareable, in the words of the reference found
// (itypeNotShareableWhy): into 'reason', what kind of thing it is and why it may
// not be held by several threads at once
void genericNotShareableReason(INode *culprit, int fix, char *reason, size_t size) {
    RefNode *ref = (RefNode *)culprit;
    INode *region = ref->region && isTypeNode(ref->region) ? itypeGetTypeDcl(ref->region) : NULL;
    char *regname = region && region->tag == StructTag ? &((StructNode *)region)->namesym->namestr : "its region";
    INode *perm = ref->perm && isTypeNode(ref->perm) ? itypeGetTypeDcl(ref->perm) : NULL;
    Name *permname = perm ? inodeGetName(perm) : NULL;
    if (regionIsTraced(ref->region) && !regionIsActorLocal(ref->region))
        snprintf(reason, size,
            "a reference the %s collector traces: copying one writes a root into the collector's one chain, which is single threaded, so two threads cannot do it together",
            regname);
    else if (refCountsPlain(ref))
        snprintf(reason, size,
            "an owner that may be copied, and %s does not declare ThreadSafe: a copy writes its count without an atomic operation, which two threads cannot do together%s",
            regname, fix ? ". Hold it in an Arc, whose count is atomic" : "");
    else if (culprit->tag == VirtRefTag && !refWritesShared(ref))
        snprintf(reason, size,
            "a reference to a trait or a callable, whose implementers are not all known here, so nothing says a borrow of what it points at may be shared between threads%s",
            fix ? ". Say what the value behind it is where the reference is written: '+ Shareable' (or '+ Sendable'), as in '&<Shape + Shareable' or 'Uni[fn(i32) i32 + Sendable]'" : "");
    else
        snprintf(reason, size,
            "a reference of permission %s, which may write: Cone's %s is shared mutable, so whatever holds one writes through a borrow of it, and two threads holding that borrow could write at once%s",
            permname ? &permname->namestr : "?", perm == (INode *)mutPerm ? "'&mut'" : "reference of that permission",
            fix ? ". Copy the value into the holder, or hold it behind a lock" : "");
}

// A captured variable that keeps a closure from being Shareable, in a sentence
// the reason follows: "it borrows 'hf2' (Rc[imm, Grid])", or, where the variable
// holds the culprit without being it, "it holds 'cfg' (Config), which holds Rc[imm, Grid]"
void genericCapSentence(ClosureCap *cap, INode *culprit, char *buf, size_t size) {
    char var[256] = "", held[256] = "";
    itypeSpellCat(var, sizeof(var), closureCapValueType(cap), 0);
    itypeSpellCat(held, sizeof(held), culprit, 0);
    if (strcmp(var, held) == 0)
        snprintf(buf, size, "it %s '%s' (%s)", cap->state ? "holds" : "borrows", &cap->dcl->namesym->namestr, var);
    else
        snprintf(buf, size, "it %s '%s' (%s), which holds %s", cap->state ? "holds" : "borrows",
            &cap->dcl->namesym->namestr, var, held);
}

// A closure's captured variable whose type is not Shareable: the field of the
// hidden struct that holds it (a variable borrowed, or a list entry), the first
// in the order captured. NULL where 'type' is no closure's.
ClosureCap *genericClosureNotShareableCap(INode *type, int mutrule) {
    INode *dcl = type ? itypeGetTypeDcl(type) : NULL;
    ClosureInfo *closure = dcl ? closureOfStruct(dcl) : NULL;
    for (uint32_t c = 0; closure != NULL && c < closure->ncaps; ++c) {
        ClosureCap *cap = &closure->caps[c];
        if (cap->field == NULL || cap->dcl == NULL)
            continue;
        char path[64];
        if (itypeNotShareableWhy(cap->field->vtype, mutrule, path, sizeof(path)) != NULL)
            return cap;
    }
    return NULL;
}

// Refuse an instance whose argument 'arg', for parameter 'parm', is not Shareable
static void genericNotShareableMsg(INode *errnode, Name *name, GenVarDclNode *parm, INode *arg) {
    char path[256];
    INode *culprit = itypeNotShareableWhy(arg, 1, path, sizeof(path));
    ClosureCap *cap = genericClosureNotShareableCap(arg, 1);
    char reason[512] = "";
    if (culprit)
        genericNotShareableReason(culprit, cap == NULL, reason, sizeof(reason));
    char culpritname[256] = "";
    if (culprit)
        itypeSpellCat(culpritname, sizeof(culpritname), culprit, 0);
    if (cap != NULL) {
        char sentence[600], advice[600];
        genericCapSentence(cap, culprit, sentence, sizeof(sentence));
        closureCapAdvice(cap, !itypeNotShareable(closureCapValueType(cap)), advice, sizeof(advice));
        errorMsgNode(errnode, ErrorNotShareable,
            "%s requires %s is Shareable, and this closure is not: %s, %s. %s",
            &name->namestr, &parm->namesym->namestr, sentence, reason, advice);
        return;
    }
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    errorMsgNode(errnode, ErrorNotShareable, "%s requires %s is Shareable, and %s is not Shareable: %s is %s, %s.",
        &name->namestr, &parm->namesym->namestr, argname, path[0] ? path : "it",
        culpritname, reason);
}

// An instance whose 'T is Sendable' was met while a struct it reaches was not
// yet type checked, so not settled: judged again once type check has finished
// (genericSendableCheckAll), when every struct is laid out
typedef struct {
    INode *where;           // The use asking for the instance
    Name *name;             // The generic's name
    GenVarDclNode *parm;    // The parameter the clause asks about
    INode *arg;             // Its argument
    StaticBorrow how;       // Whether a borrow of the whole program crosses there
} SendableNote;

static SendableNote *genericSendableNotes = NULL;
static uint32_t genericSendableCnt = 0;
static uint32_t genericSendableMax = 0;

static void genericSendableNote(FnCallNode *srcgencall, Nodes *where, Nodes *parms, Name *name) {
    if (where == NULL || parms == NULL || srcgencall->args == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if ((*nodesp)->tag != IsTag || genericNamedTrait(((CastNode*)*nodesp)->typ) != sendableTrait)
            continue;
        GenVarDclNode *parm;
        INode *arg = genericClauseType((CastNode*)*nodesp, parms, srcgencall->args, &parm);
        if (arg == NULL || parm == NULL)
            continue;
        int settled;
        StaticBorrow how = genericStaticHow(sendableTrait, parm);
        if (itypeThreadBoundHow(arg, &settled, how) || settled)
            continue;
        if (genericSendableCnt == genericSendableMax) {
            uint32_t newmax = genericSendableMax ? genericSendableMax * 2 : 16;
            SendableNote *notes = (SendableNote *)memAllocBlk(newmax * sizeof(SendableNote));
            if (genericSendableCnt)
                memcpy(notes, genericSendableNotes, genericSendableCnt * sizeof(SendableNote));
            genericSendableNotes = notes;
            genericSendableMax = newmax;
        }
        SendableNote *note = &genericSendableNotes[genericSendableCnt++];
        note->where = (INode*)srcgencall;
        note->name = name;
        note->parm = parm;
        note->arg = arg;
        note->how = how;
    }
}

void genericSendableCheckAll() {
    uint32_t cnt = genericSendableCnt;
    genericSendableCnt = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        SendableNote *note = &genericSendableNotes[i];
        if (itypeThreadBoundHow(note->arg, NULL, note->how))
            genericNotSendableMsg(note->where, note->name, note->parm, note->arg, note->how);
    }
}

static StructNode *genericTemplateOf(StructNode *inst);

// Where 'arg' is an instance of a generic type naming 'trait' in its 'is' list
// with a condition, 'is Move if T is Move', why it is not 'trait': the
// condition is false at its arguments. Asked of Copy, why it moves: an
// 'is Move if' condition is true there. Writes the sentence into 'buf', left
// empty where no condition explains it.
static void genericCondIsWhy(char *buf, size_t size, INode *arg, StructNode *trait) {
    INode *dcl = itypeGetTypeDcl(arg);
    if (dcl->tag != StructTag)
        return;
    StructNode *generic = genericTemplateOf((StructNode*)dcl);
    Nodes *args = itypeInstanceTypeArgs(dcl);
    if (generic == NULL || generic->condis == NULL || args == NULL)
        return;
    StructNode *want = trait == copyTrait ? moveTrait : trait;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(generic->condis, cnt, nodesp)) {
        FieldDclNode *entry = (FieldDclNode*)*nodesp;
        if (genericNamedTrait(entry->vtype) != want)
            continue;
        int holds = genericEntryValue((INode*)generic, entry->value, generic->genericinfo->parms, args) == WhereTrue;
        if (holds != (trait == copyTrait))
            continue;
        char text[256] = "";
        genericConditionCat(text, sizeof(text), entry->value, 0);
        char bound[256] = "";
        genericBindingsCat(bound, sizeof(bound), entry->value, generic->genericinfo->parms, args);
        if (trait == copyTrait)
            snprintf(buf, size, " %s is Move where %s, and that is true for %s.",
                &generic->namesym->namestr, text, bound);
        else
            snprintf(buf, size, " %s is %s only where %s, and that is false for %s.",
                &generic->namesym->namestr, &trait->namesym->namestr, text, bound);
        return;
    }
}

// Append a method's signature as it is written in a trait: 'push(self: &mut, v: i64)',
// 'pop(self: &mut) Option[i64]'
static void genericSigCat(char *buf, size_t size, FnDclNode *fn) {
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    size_t used = strlen(buf);
    snprintf(buf + used, size - used, "%s(", &fn->namesym->namestr);
    INode **nodesp;
    uint32_t cnt;
    int first = 1;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode*)*nodesp;
        used = strlen(buf);
        snprintf(buf + used, size - used, "%s%s", first ? "" : ", ", &parm->namesym->namestr);
        INode *dcl = parm->vtype && isTypeNode(parm->vtype) ? itypeGetTypeDcl(parm->vtype) : NULL;
        // A receiver is written by its permission alone: 'self: &mut'
        INode *region = dcl && dcl->tag == RefTag && ((RefNode*)dcl)->region && isTypeNode(((RefNode*)dcl)->region)
            ? itypeGetTypeDcl(((RefNode*)dcl)->region) : NULL;
        if (first && (fn->flags & FlagMethFld) && region == borrowRef) {
            INode *perm = ((RefNode*)dcl)->perm && isTypeNode(((RefNode*)dcl)->perm)
                ? itypeGetTypeDcl(((RefNode*)dcl)->perm) : NULL;
            Name *permname = perm ? inodeGetName(perm) : NULL;
            used = strlen(buf);
            if (permname == NULL || perm == (INode*)roPerm)
                snprintf(buf + used, size - used, ": &");
            else
                snprintf(buf + used, size - used, ": &%s", &permname->namestr);
        }
        else if (parm->vtype) {
            used = strlen(buf);
            snprintf(buf + used, size - used, ": ");
            genericTypeNameCat(buf, size, parm->vtype, 0);
        }
        first = 0;
    }
    used = strlen(buf);
    snprintf(buf + used, size - used, ")");
    if (sig->rettype && sig->rettype->tag != VoidTag) {
        used = strlen(buf);
        snprintf(buf + used, size - used, " ");
        genericTypeNameCat(buf, size, sig->rettype, 0);
    }
}

// Where a type's method 'meth' of the trait's differs from the trait's in nothing
// but a parameter or the result held by value that is not one layout
// (fnSigLayoutDiff; 'binding' is the type's name for it): the sentence saying so,
// into 'buf'. It is reported under ErrorFnRefByValue
static int genericMethodLayoutWhy(char *buf, size_t size, FnDclNode *meth, INode *binding, INode *dcl,
        char *typename, char *traitname) {
    FnSigLayoutDiff diff;
    if (!iNsTypeVrefLayoutDiff(binding, meth, dcl, &diff))
        return 0;
    char expected[256] = "", spelled[700];
    genericSigCat(expected, sizeof(expected), meth);
    fnSigLayoutSpell(spelled, sizeof(spelled), &diff, traitname, traitname);
    snprintf(buf, size, " %s has %s, but %s requires %s, and %s Write the method with the trait's types, and convert inside it.",
        typename, &meth->namesym->namestr, traitname, expected, spelled);
    return 1;
}

// The same for the first method of 'trait' that is refused for that reason alone:
// why a type does not fit a trait whose methods all exist, into 'buf'
static int genericLayoutWhy(char *buf, size_t size, StructNode *trait, INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != StructTag)
        return 0;
    char typename[256] = "", traitname[256] = "";
    genericTypeNameCat(typename, sizeof(typename), type, 0);
    genericTypeNameCat(traitname, sizeof(traitname), (INode*)trait, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode*)*nodesp;
        INode *binding = namespaceFind(&((StructNode*)dcl)->namespace, meth->namesym);
        if (iNsTypeFindVrefMethod(binding, meth, dcl) == NULL
            && genericMethodLayoutWhy(buf, size, meth, binding, dcl, typename, traitname))
            return 1;
    }
    return 0;
}

// Why a type does not fit a generic trait's instance structurally, into 'buf'
// (empty if nothing is found to say): the first method the trait requires that
// the type lacks, or has only with another signature, each spelled as the
// instance's types come to, so 'pop(self: &mut) Option[i64]' where the trait
// is 'Stack[i64]'. '*layout' is set where the reason is a parameter or result held
// by value that is not one layout (fnSigLayoutDiff), which has its own code
static void genericFitWhy(char *buf, size_t size, StructNode *trait, INode *type, int *layout) {
    buf[0] = '\0';
    *layout = 0;
    char typename[256] = "";
    genericTypeNameCat(typename, sizeof(typename), type, 0);
    char traitname[256] = "";
    genericTypeNameCat(traitname, sizeof(traitname), (INode*)trait, 0);
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != StructTag) {
        snprintf(buf, size, " %s is not a struct: only a struct fits a trait by the methods it has.", typename);
        return;
    }
    StructNode *strnode = (StructNode*)dcl;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode*)*nodesp;
        INode *binding = namespaceFind(&strnode->namespace, meth->namesym);
        if (iNsTypeFindVrefMethod(binding, meth, dcl) != NULL)
            continue;
        char expected[256] = "";
        genericSigCat(expected, sizeof(expected), meth);
        if (genericMethodLayoutWhy(buf, size, meth, binding, dcl, typename, traitname)) {
            *layout = 1;
            return;
        }
        if (binding == NULL || (binding->tag != FnDclTag && binding->tag != FnOverloadDclTag)) {
            snprintf(buf, size, " %s has no method %s, which %s requires.", typename,
                &meth->namesym->namestr, traitname);
            return;
        }
        // A generic method does not meet the trait's, which is not generic, and
        // its signature, written in type parameters, is not spelled
        char found[256] = "";
        if (binding->tag == FnDclTag) {
            if (((FnDclNode*)binding)->genericinfo == NULL)
                genericSigCat(found, sizeof(found), (FnDclNode*)binding);
        }
        else {
            Nodes *overloads = ((FnOverloadDclNode*)binding)->overloads;
            for (uint32_t i = 0; i < overloads->used; ++i) {
                FnDclNode *cand = (FnDclNode*)nodesGet(overloads, i);
                if (cand->genericinfo)
                    continue;
                size_t used = strlen(found);
                if (found[0])
                    snprintf(found + used, sizeof(found) - used, " and ");
                genericSigCat(found, sizeof(found), cand);
            }
        }
        if (found[0] == '\0')
            snprintf(buf, size, " %s has %s only as a generic method, and a generic method does not meet %s's %s, which is not generic and requires %s.",
                typename, &meth->namesym->namestr, traitname, &meth->namesym->namestr, expected);
        else
            snprintf(buf, size, " %s has %s, but %s requires %s.", typename, found, traitname, expected);
        return;
    }
    for (nodelistFor(&trait->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsMixin)
            continue;
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        INode *have = namespaceFind(&strnode->namespace, field->namesym);
        char fieldtype[256] = "";
        if (field->vtype)
            genericTypeNameCat(fieldtype, sizeof(fieldtype), field->vtype, 0);
        if (have == NULL || have->tag != FieldDclTag || ((FieldDclNode*)have)->hop) {
            snprintf(buf, size, " %s has no field %s, which %s requires as %s.", typename,
                &field->namesym->namestr, traitname, fieldtype);
            return;
        }
        INode *havecopy = have;
        TypeCompare match = iexpMatches(&havecopy, field->vtype, Monomorph);
        if (match != EqMatch && match != ConvSubtype && match != CastSubtype) {
            char havetype[256] = "";
            if (((FieldDclNode*)have)->vtype)
                genericTypeNameCat(havetype, sizeof(havetype), ((FieldDclNode*)have)->vtype, 0);
            snprintf(buf, size, " %s has the field %s as %s, but %s requires %s.", typename,
                &field->namesym->namestr, havetype, traitname, fieldtype);
            return;
        }
    }
}

// A constraint on a generic function or type is a requirement: an instance
// whose arguments do not meet it is refused where it is asked for, naming the
// clause, and nothing of it is made -- so nothing inside the generic is checked
// against arguments it was never meant for. A variant answers to its enum's.
static int genericRequirementsMetIn(FnCallNode *srcgencall, INode *generic, GenericInfo *genericinfo, Name *name) {
    Nodes *where = NULL;
    Nodes *parms = genericinfo->parms;
    if (generic->tag == FnDclTag)
        where = ((FnDclNode*)generic)->where;
    else if (generic->tag == StructTag) {
        StructNode *owner = (StructNode*)generic;
        if (owner->flags & HasTagField)
            owner = structGetBaseTrait(owner);
        if (owner && owner->genericinfo) {
            where = owner->genericinfo->where;
            parms = owner->genericinfo->parms;
            name = owner->namesym;
        }
    }
    genericWhereOwner = generic;
    INode *cond = genericUnmetCondition(generic, where, parms, srcgencall->args);
    if (cond == NULL) {
        genericSendableNote(srcgencall, where, parms, name);
        genericWhereOwner = NULL;
        return 1;
    }
    genericWhereOwner = NULL;
    // A type argument already reported as bad (a value whose initializer was
    // refused, handed to the call) meets nothing, and has no name to say so
    // with: the report that made it bad is the one, so this one is not made
    if (srcgencall->args) {
        INode **argp;
        uint32_t argcnt;
        for (nodesFor(srcgencall->args, argcnt, argp)) {
            if (*argp == errorType)
                return 0;
        }
    }
    // A condition joined by '||' or '&&' is false as a whole, and named whole
    if (cond->tag != IsTag) {
        char text[256] = "";
        genericConditionCat(text, sizeof(text), cond, 0);
        char bound[256] = "";
        genericBindingsCat(bound, sizeof(bound), cond, parms, srcgencall->args);
        errorMsgNode((INode*)srcgencall, ErrorWhereUnmet, "%s requires %s, and it is false for %s.",
            &name->namestr, text, bound);
        return 0;
    }
    GenVarDclNode *parm;
    INode *arg = genericClauseType((CastNode*)cond, parms, srcgencall->args, &parm);
    // A signature: say what it comes to here, and what the argument has instead
    if (((CastNode*)cond)->typ->tag == FnSigTag) {
        INode *sigtype = genericClauseCloneChecked((CastNode*)cond, parms, srcgencall->args);
        FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(sigtype);
        char argname[256] = "";
        if (genericFnTypeSig(arg))
            genericFnSigCat(argname, sizeof(argname), genericFnTypeSig(arg));
        else
            genericTypeNameCat(argname, sizeof(argname), arg, 0);
        char isname[256] = "";
        genericClauseNameCat(isname, sizeof(isname), (CastNode*)cond, NULL);
        char here[256] = "";
        genericFnSigCat(here, sizeof(here), sig);
        char why[1000] = "";
        int layout = genericSigMeetWhy(why, sizeof(why), arg, sig);
        errorMsgNode((INode*)srcgencall, layout ? ErrorFnRefByValue : ErrorWhereUnmet,
            "%s requires %s is %s, here %s, and %s is not.%s",
            &name->namestr, &parm->namesym->namestr, isname, here, argname, why);
        return 0;
    }
    StructNode *trait = genericNamedTrait(((CastNode*)cond)->typ);
    if (trait == sendableTrait) {
        genericWhereOwner = generic;
        StaticBorrow how = genericStaticHow(trait, parm);
        genericWhereOwner = NULL;
        genericNotSendableMsg((INode*)srcgencall, name, parm, arg, how);
        return 0;
    }
    if (trait == shareableTrait) {
        genericNotShareableMsg((INode*)srcgencall, name, parm, arg);
        return 0;
    }
    // An instance of a generic trait is the one made at these arguments, and the
    // message says which method of it the argument lacks
    int unfit = 0;
    StructNode *inst = trait ? NULL : genericClauseTraitFor((CastNode*)cond, parms, srcgencall->args, arg, &unfit);
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    char isname[256] = "";
    genericClauseNameCat(isname, sizeof(isname), (CastNode*)cond, inst);
    char why[1000] = "";
    int layout = 0;
    // A trait with associated types the argument does not say: named at the
    // arguments, with what the argument says of them, or fails to
    if (unfit) {
        isname[0] = '\0';
        genericTemplateNameCat(isname, sizeof(isname), genericAssocUnfitUse, 0);
        errorMsgNode((INode*)srcgencall, ErrorWhereUnmet, "%s requires %s is %s, and %s is not %s.%s",
            &name->namestr, &parm->namesym->namestr, isname, argname, isname, genericAssocWhy);
        return 0;
    }
    // An instance of a generic enum is closed: a type is it only by being it or
    // one of its variants, never by the methods or fields it has, so the clause
    // is named but no fit is explained (its methods are not requirements, and
    // its tag is no field a type could declare)
    if (inst && (inst->flags & EnumType)) {
        errorMsgNode((INode*)srcgencall, ErrorWhereUnmet, "%s requires %s is %s, and %s is not %s.",
            &name->namestr, &parm->namesym->namestr, isname, argname, isname);
        return 0;
    }
    if (trait)
        genericCondIsWhy(why, sizeof(why), arg, trait);
    else if (inst && !genericTraitIsMarker(inst))
        genericFitWhy(why, sizeof(why), inst, arg, &layout);
    if (inst == NULL)
        inst = trait;
    // A type whose method is the trait's but for a value held by value that is not
    // one layout is refused for that, under its own code
    if (why[0] == '\0' && inst && !layout)
        layout = genericLayoutWhy(why, sizeof(why), inst, arg);
    // A type that has what the trait requires but keeps it private to its own
    // module does not meet it from another, and is told so under its own code
    int privacy = 0;
    if (why[0] == '\0' && inst && itypeGetTypeDcl(arg)->tag == StructTag)
        privacy = structPrivateWhy(why, sizeof(why), inst, (StructNode*)itypeGetTypeDcl(arg));
    int usermarker = why[0] == '\0' && inst && genericTraitIsMarker(inst) && !corelibIsBuiltinTrait((INode*)inst);
    errorMsgNode((INode*)srcgencall, layout ? ErrorFnRefByValue : privacy ? ErrorPrivateMatch : ErrorWhereUnmet,
        "%s requires %s is %s, and %s is not %s.%s%s",
        &name->namestr, &parm->namesym->namestr, isname, argname, isname, why,
        usermarker ? " A trait requiring nothing of a value is met only by a type declaring it with 'is'." : "");
    return 0;
}

// The requirements, decided under the use asking for the instance: a clause
// naming a generic trait makes its instance there, and what asks while that is
// made restores this one's on the way out
static int genericRequirementsMet(TypeCheckState *pstate, FnCallNode *srcgencall, INode *generic,
        GenericInfo *genericinfo, Name *name) {
    INode *svsite = genericCondSite;
    TypeCheckState *svstate = genericCondState;
    genericCondSite = (INode*)srcgencall;
    genericCondState = pstate;
    // The words for a refusal ask the same question the decision did
    GenericAsker asker;
    genericAskerEnter(&asker, generic);
    int met = genericRequirementsMetIn(srcgencall, generic, genericinfo, name);
    genericAskerLeave(&asker);
    genericCondSite = svsite;
    genericCondState = svstate;
    return met;
}

// Is one of these type arguments still being laid out, so that what it is --
// whether its values move, above all -- is not yet settled?
static int genericArgsUnsettled(Nodes *args) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if ((*nodesp)->tag != ULitTag && isTypeNode(*nodesp) && structLayoutUnsettled(*nodesp))
            return 1;
    }
    return 0;
}

// The members of generic type 'generic' that do not exist at 'args': each
// whose 'where' clause, over the type's parameters, they do not meet; and each
// entry of its 'is' list whose condition they do not make true, which the
// instance lacks (cloneStructNode)
//
// A member's clause is read from its arguments' layouts. While an argument is
// still being laid out -- 'struct Node { kids: List[Node]; }' asks for List[Node]
// inside Node's own layout -- what it comes to is not settled, and answering
// then would answer by the order the declarations happen to be laid out in
// ('Node' is Copy until its fields say it moves). So with 'late' given, such a
// member is decided here only as far as no type is asked what it is
// (genericMemberWhereNow): a clause naming a type is answered now. One that
// waits on a trait or a signature is listed in 'late', cloned, and decided once
// no layout is in flight, before any member is checked (genericLateWhere), or
// sooner, when something reads the instance's members (genericLateWhereNow). An
// 'is' entry's condition is not deferred: it is part of the layout.
static Nodes *genericAbsentMembersIn(StructNode *generic, Nodes *args, Nodes **late) {
    Nodes *absent = NULL;
    INode **nodesp;
    uint32_t cnt;
    int unsettled = late != NULL && args != NULL && genericArgsUnsettled(args);
    for (nodelistFor(&generic->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || ((FnDclNode*)*nodesp)->where == NULL)
            continue;
        if (unsettled) {
            WhereValue now = genericMemberWhereNow((INode*)generic, ((FnDclNode*)*nodesp)->where,
                generic->genericinfo->parms, args);
            if (now == WhereTrue)
                continue;
            if (now == WhereLate) {
                if (*late == NULL)
                    *late = newNodes(4);
                nodesAdd(late, *nodesp);
                continue;
            }
            if (absent == NULL)
                absent = newNodes(4);
            nodesAdd(&absent, *nodesp);
            continue;
        }
        if (genericUnmetCondition((INode*)generic, ((FnDclNode*)*nodesp)->where, generic->genericinfo->parms, args) == NULL)
            continue;
        if (absent == NULL)
            absent = newNodes(4);
        nodesAdd(&absent, *nodesp);
    }
    if (generic->condis) {
        for (nodesFor(generic->condis, cnt, nodesp)) {
            INode *cond = ((FieldDclNode*)*nodesp)->value;
            if (genericEntryValue((INode*)generic, cond, generic->genericinfo->parms, args) == WhereTrue)
                continue;
            if (absent == NULL)
                absent = newNodes(4);
            nodesAdd(&absent, *nodesp);
        }
    }
    return absent;
}

// The same, decided under the use asking for the instance, as the requirements are
static Nodes *genericAbsentMembers(TypeCheckState *pstate, FnCallNode *srcgencall, StructNode *generic, Nodes *args,
        Nodes **late) {
    INode *svsite = genericCondSite;
    TypeCheckState *svstate = genericCondState;
    genericCondSite = (INode*)srcgencall;
    genericCondState = pstate;
    Nodes *absent = genericAbsentMembersIn(generic, args, late);
    genericCondSite = svsite;
    genericCondState = svstate;
    return absent;
}

// The generic type 'inst' is an instance of: the one whose memo holds it. A
// generic enum's variants are instantiated with the enum by one call, so the
// call's generic may be the enum or any of its variants.
static StructNode *genericTemplateOf(StructNode *inst) {
    INode *instnode = inst->instnode;
    if (itypeInstanceTypeArgs((INode*)inst) == NULL || !isNameUseNode(((FnCallNode*)instnode)->objfn))
        return NULL;
    INode *generic = nameUseGetDcl((NameUseNode*)((FnCallNode*)instnode)->objfn);
    if (generic == NULL || generic->tag != StructTag || ((StructNode*)generic)->genericinfo == NULL)
        return NULL;
    StructNode *base = (generic->flags & HasTagField) ? structGetBaseTrait((StructNode*)generic) : (StructNode*)generic;
    if (base == NULL)
        return NULL;
    uint32_t nvariants = base->derived ? base->derived->used : 0;
    for (uint32_t i = 0; i <= nvariants; ++i) {
        StructNode *cand = i == 0 ? base : (StructNode*)nodesGet(base->derived, i - 1);
        if (cand->genericinfo == NULL || cand->genericinfo->memonodes == NULL)
            continue;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(cand->genericinfo->memonodes, cnt, nodesp)) {
            ++nodesp; --cnt;  // pairs: the call, then its instance
            if (*nodesp == (INode*)inst)
                return cand;
        }
    }
    return NULL;
}

StructNode *genericTypeTemplate(StructNode *inst) {
    if (inst == NULL || inst->tag != StructTag || inst->instnode == NULL)
        return NULL;
    return genericTemplateOf(inst);
}

// Report a member absent because its condition 'cond' is false at 'args'
static void genericAbsentMsg(INode *errnode, char *what, Name *name, char *typename,
        INode *cond, Nodes *parms, Nodes *args) {
    if (cond->tag != IsTag) {
        char text[256] = "";
        genericConditionCat(text, sizeof(text), cond, 0);
        char bound[256] = "";
        genericBindingsCat(bound, sizeof(bound), cond, parms, args);
        errorMsgNode(errnode, ErrorWhereAbsent,
            "No %s %s for %s: it exists only where %s, and that is false for %s.",
            what, &name->namestr, typename, text, bound);
        return;
    }
    GenVarDclNode *parm;
    INode *arg = genericClauseType((CastNode*)cond, parms, args, &parm);
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    char isname[256] = "";
    genericClauseNameCat(isname, sizeof(isname), (CastNode*)cond,
        genericClauseTrait((CastNode*)cond, parms, args));
    errorMsgNode(errnode, ErrorWhereAbsent,
        "No %s %s for %s: it exists only where %s is %s, and %s is not %s.",
        what, &name->namestr, typename, &parm->namesym->namestr, isname, argname, isname);
}

int genericReportAbsent(INode *errnode, INode *typedcl, Name *name) {
    if (typedcl == NULL || typedcl->tag != StructTag)
        return 0;
    StructNode *generic = genericTemplateOf((StructNode*)typedcl);
    if (generic == NULL)
        return 0;
    INode *binding = namespaceFind(&generic->namespace, name);
    if (binding == NULL)
        return 0;
    INode **candp;
    uint32_t cnt;
    if (binding->tag == FnDclTag) {
        candp = &binding;
        cnt = 1;
    }
    else if (binding->tag == FnOverloadDclTag) {
        candp = &nodesGet(((FnOverloadDclNode*)binding)->overloads, 0);
        cnt = ((FnOverloadDclNode*)binding)->overloads->used;
    }
    else
        return 0;
    Nodes *args = itypeInstanceTypeArgs(typedcl);
    while (cnt--) {
        FnDclNode *fn = (FnDclNode*)*candp++;
        INode *cond = genericUnmetCondition((INode*)generic, fn->where, generic->genericinfo->parms, args);
        if (cond == NULL)
            continue;
        char typename[256] = "";
        genericTypeNameCat(typename, sizeof(typename), typedcl, 0);
        genericAbsentMsg(errnode, (fn->flags & FlagMethFld) ? "method" : "function", name, typename,
            cond, generic->genericinfo->parms, args);
        return 1;
    }
    return 0;
}

// Does one condition of a generic's bound a parameter by a trait that requires
// 'member', the parameter's argument being 'type', which declares the trait? An
// '&&' holds if either side does; a '||' never does, since a body governed by
// it may rely only on what every alternative gives.
static int genericBoundCondReaches(INode *cond, Nodes *parms, Nodes *args, StructNode *type, Name *member) {
    if (cond->tag == AndLogicTag)
        return genericBoundCondReaches(((LogicNode*)cond)->lexp, parms, args, type, member)
            || genericBoundCondReaches(((LogicNode*)cond)->rexp, parms, args, type, member);
    if (cond->tag != IsTag)
        return 0;
    GenVarDclNode *parm;
    INode *arg = genericClauseType((CastNode*)cond, parms, args, &parm);
    if (arg == NULL || parm == NULL || itypeGetTypeDcl(arg) != (INode*)type)
        return 0;
    StructNode *trait = genericNamedTrait(((CastNode*)cond)->typ);
    if (trait == NULL || !structDeclaresTraitTree(type, trait, 0))
        return 0;
    INode *required = namespaceFind(&trait->namespace, member);
    return required && (required->flags & FlagMethFld);
}

int genericBoundReachesMember(INode *node, StructNode *type, Name *member) {
    if (node == NULL || type == NULL || (type->flags & TraitType))
        return 0;
    Nodes *args = itypeInstanceTypeArgs(node);
    if (args == NULL || !isNameUseNode(((FnCallNode*)node->instnode)->objfn))
        return 0;
    INode *generic = nameUseGetDcl((NameUseNode*)((FnCallNode*)node->instnode)->objfn);
    Nodes *where = NULL;
    Nodes *parms = NULL;
    if (generic == NULL)
        return 0;
    if (generic->tag == FnDclTag && ((FnDclNode*)generic)->genericinfo) {
        where = ((FnDclNode*)generic)->where;
        parms = ((FnDclNode*)generic)->genericinfo->parms;
    }
    else if (generic->tag == StructTag) {
        StructNode *owner = (StructNode*)generic;
        if (owner->flags & HasTagField)
            owner = structGetBaseTrait(owner);
        if (owner && owner->genericinfo) {
            where = owner->genericinfo->where;
            parms = owner->genericinfo->parms;
        }
    }
    if (where == NULL || parms == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if (genericBoundCondReaches(*nodesp, parms, args, type, member))
            return 1;
    }
    return 0;
}

int genericReportTemplateMember(INode *errnode, FnDclNode *fn) {
    if (fn->where == NULL)
        return 0;
    INode *owner = inodeGetOwner((INode*)fn);
    if (owner == NULL || owner->tag != StructTag || ((StructNode*)owner)->genericinfo == NULL)
        return 0;
    // Only a use copied into an instance of the same generic -- a use written
    // anywhere else names the generic's member as such, and is told to name an
    // instance instead
    INode *instnode = errnode->instnode;
    if (instnode == NULL || instnode->tag != FnCallTag || !isNameUseNode(((FnCallNode*)instnode)->objfn))
        return 0;
    INode *generic = nameUseGetDcl((NameUseNode*)((FnCallNode*)instnode)->objfn);
    if (generic == NULL || generic->tag != StructTag)
        return 0;
    if (generic != owner) {
        StructNode *base = structGetBaseTrait((StructNode*)generic);
        if (base == NULL || base != structGetBaseTrait((StructNode*)owner))
            return 0;
    }
    // Which instance's body names it is not known here, only that its arguments
    // failed a clause, so the whole condition is named; the instantiation trace
    // says which instance it was
    char clauses[256] = "";
    for (uint32_t i = 0; i < fn->where->used; ++i) {
        size_t used = strlen(clauses);
        snprintf(clauses + used, sizeof(clauses) - used, i ? " and " : "");
        genericConditionCat(clauses, sizeof(clauses), nodesGet(fn->where, i), fn->where->used > 1);
    }
    errorMsgNode(errnode, ErrorWhereAbsent,
        "%s exists only where %s, and the instance of %s naming it here does not meet that.",
        &fn->namesym->namestr, clauses, &((StructNode*)owner)->namesym->namestr);
    return 1;
}

// How deeply expansion is currently nested, and whether the outermost
// expansion now unwinding was refused at the limit. See generic.h.
static uint32_t instantiateDepth = 0;
static int instantiateRefused = 0;

int genericInstantiateEnter(INode *errnode) {
    if (instantiateDepth >= TypeCheckLoopMax) {
        errorMsgNode(errnode, ErrorInstDepth,
            "Generic or macro expansion nested more than %d deep. It likely expands itself endlessly.",
            TypeCheckLoopMax);
        instantiateRefused = 1;
        return 0;
    }
    // Every level the refusal unwinds through would otherwise start its next
    // expansion down to the limit again: one that expands twice a level is
    // exponential, reporting the limit without end. So the rest of that
    // outermost expansion is refused too, already reported.
    if (instantiateRefused)
        return 0;
    ++instantiateDepth;
    return 1;
}

void genericInstantiateExit() {
    if (--instantiateDepth == 0)
        instantiateRefused = 0;
}

uint32_t genericInstantiateDepth() {
    return instantiateDepth;
}

void genericInstantiateDepthSet(uint32_t depth) {
    instantiateDepth = depth;
    if (depth == 0)
        instantiateRefused = 0;
}

// Reserve the instance of a generic type before it is cloned, and map the
// generic to it for cloneDclFix. Inside its own braces a generic type's bare
// name means the instance being defined -- 'Box' inside 'struct Box[T]' is
// 'Box[T]' -- so every use of the name the clone copies while the map is in
// force names this instance, which is never a second instantiation. A use given
// type arguments keeps naming the generic (cloneFnCallNode): 'Box[i32]' is
// another instance, reached through the memo like any other.
// The caller pushes and pops the map.
static INode *genericReserve(INode *generic) {
    StructNode *shell = memAllocBlk(sizeof(StructNode));
    cloneDclSetMap(generic, (INode*)shell);
    return (INode*)shell;
}

// An instance's members whose 'where' waited on its arguments' layouts: each
// beside its copy in the instance, alternating, and the walk the instance was
// made in, which they are decided under
typedef struct GenericLateWhere {
    StructNode *generic;
    FnCallNode *srcgencall;
    Nodes *args;
    Nodes *pairs;
    TypeCheckState state;
} GenericLateWhere;

// Lay out each argument no layout has reached
static void genericLateSettle(TypeCheckState *pstate, Nodes *args) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        if ((*nodesp)->tag != ULitTag && isTypeNode(*nodesp))
            structTypeSettle(pstate, *nodesp);
    }
}

// Decide which of an instance's members its arguments' 'where' clauses leave
// out, and take those out of it. Decided once: the instance stops pointing at
// what waits before anything is asked, so a read of the instance while it is
// decided does not decide it again.
static void genericLateDecide(TypeCheckState *pstate, StructNode *instance, GenericLateWhere *late) {
    instance->latewhere = NULL;
    INode *svsite = genericCondSite;
    TypeCheckState *svstate = genericCondState;
    genericCondSite = (INode*)late->srcgencall;
    genericCondState = pstate;
    for (uint32_t i = 0; i + 1 < late->pairs->used; i += 2) {
        FnDclNode *original = (FnDclNode*)nodesGet(late->pairs, i);
        if (genericUnmetCondition((INode*)late->generic, original->where, late->generic->genericinfo->parms, late->args) != NULL)
            structRetireMember(instance, nodesGet(late->pairs, i + 1));
    }
    genericCondSite = svsite;
    genericCondState = svstate;
}

// Decide, once no layout is in flight, the members of an instance whose 'where'
// waited, before any member is checked, unless a read of the instance decided
// them already (genericLateWhereNow). An argument no layout has reached is laid
// out first.
static void genericLateWhere(TypeCheckState *pstate, INode *instance, void *extra) {
    GenericLateWhere *late = (GenericLateWhere *)extra;
    if (((StructNode*)instance)->latewhere != late)
        return;
    genericLateSettle(pstate, late->args);
    genericLateDecide(pstate, (StructNode*)instance, late);
}

// A read of an instance's members, while those whose 'where' waited are not yet
// decided: they are decided now, so that no use selects, or checks the body of,
// a member its arguments leave out. The queue decides them only once no layout
// is in flight, and a body can be checked inside one: a type fitting a trait
// structurally has its methods checked as the fit is decided, and that can
// happen inside a layout (a field's type, a global's declared type, a
// signature, each naming 'Host[Svc]' with 'S: Hosted').
//
// What waits is a clause asking what an argument is, which needs its layout. An
// argument no layout has reached is laid out here, as the queue would. One whose
// layout is in flight cannot be answered yet; nor can the instance's own
// members be changed while it is laid out (TypeChecking without TypeChecked).
// Either way they are left to the queue.
void genericLateWhereNow(StructNode *instance) {
    GenericLateWhere *late = instance->latewhere;
    if (late == NULL || (instance->flags & (TypeChecking | TypeChecked)) == TypeChecking)
        return;
    // Held as a layout in flight, so that nothing this settles works the members
    // queue before the instance is decided
    structLayoutEnter();
    uint32_t target = structTargetSuspend();
    genericLateSettle(&late->state, late->args);
    structTargetResume(target);
    if (instance->latewhere == late && !genericArgsUnsettled(late->args))
        genericLateDecide(&late->state, instance, late);
    structLayoutExit();
}

// The instances made inside a trial for a type the trial made, to be taken out
// of their generics' memonodes once no trial is open (genericTrialPurge)
typedef struct {
    GenericInfo *info;
    INode *instance;
} GenericTrialMemo;
static GenericTrialMemo *genericTrialMemos = NULL;
static uint32_t genericTrialMemoUsed = 0;
static uint32_t genericTrialMemoCap = 0;

static void genericTrialNote(GenericInfo *info, INode *instance) {
    if (genericTrialMemoUsed == genericTrialMemoCap) {
        genericTrialMemoCap = genericTrialMemoCap ? genericTrialMemoCap * 2 : 16;
        genericTrialMemos = (GenericTrialMemo*)realloc(genericTrialMemos, genericTrialMemoCap * sizeof(GenericTrialMemo));
    }
    genericTrialMemos[genericTrialMemoUsed].info = info;
    genericTrialMemos[genericTrialMemoUsed].instance = instance;
    ++genericTrialMemoUsed;
}

void genericTrialPurge(void) {
    if (errorSilent)
        return;
    for (uint32_t i = 0; i < genericTrialMemoUsed; ++i) {
        Nodes *memonodes = genericTrialMemos[i].info->memonodes;
        if (memonodes == NULL)
            continue;
        for (uint32_t pos = 0; pos + 1 < memonodes->used; pos += 2) {
            if (nodesGet(memonodes, pos + 1) == genericTrialMemos[i].instance) {
                nodesMakeSpace(&genericTrialMemos[i].info->memonodes, pos, -2);
                break;
            }
        }
    }
    genericTrialMemoUsed = 0;
}

// Clone the generic's instance for parms and remember it, without type checking it.
// 'shell' is the instance reserved for a generic type (genericReserve), else NULL.
// 'absent' lists a generic type's members whose 'where' clause the arguments do
// not meet (genericAbsentMembers), which its instance is cloned without.
// 'late' lists those whose clause waits on an argument's layout: it is cloned
// with them, and genericLateWhere decides them.
static INode *genericClone(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone,
        GenericInfo *genericinfo, INode *shell, Nodes *absent, Nodes *late) {
    CloneState cstate;
    clonePushState(&cstate, (INode*)srcgencall, NULL, pstate->scope, genericinfo->parms, srcgencall->args);
    cstate.structshell = shell;
    cstate.absent = absent;
    cstate.late = late;
    INode *instance;
    // A generic function's instance is not generic itself, so its type
    // parameters are the ones substituted. A clone keeps a generic method
    // nested in what it copies generic (cloneFnDclShell), so a generic type's
    // instance still has its generic methods.
    if (nodetoclone->tag == FnDclTag) {
        FnDclNode *fn = cloneFnDclShell((FnDclNode*)nodetoclone);
        fn->genericinfo = NULL;
        cloneFnDclFill(&cstate, fn, (FnDclNode*)nodetoclone);
        fn->instnode = cstate.instnode;
        instance = (INode*)fn;
    }
    else
        instance = cloneNode(&cstate, nodetoclone);
    clonePopState();
    if (cstate.latepairs) {
        GenericLateWhere *wait = memAllocBlk(sizeof(GenericLateWhere));
        wait->generic = (StructNode*)nodetoclone;
        wait->srcgencall = srcgencall;
        wait->args = srcgencall->args;
        wait->pairs = cstate.latepairs;
        wait->state = *pstate;
        ((StructNode*)instance)->latewhere = wait;
        structDeferCheck(pstate, genericLateWhere, instance, wait);
    }

    // Remember instantiation for the future
    if (!genericinfo->memonodes)
        genericinfo->memonodes = newNodes(2);
    nodesAdd(&genericinfo->memonodes, (INode*)srcgencall);
    nodesAdd(&genericinfo->memonodes, instance);
    if (errorSilent && closureTrialArgs(srcgencall->args))
        genericTrialNote(genericinfo, instance);
    return instance;
}

// Instantiate the generic based on parms and return
INode *genericInstantiate(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone,
        GenericInfo *genericinfo, Name *name) {
    // A generic module's instance is a module of its own, cloned declaration by
    // declaration, registered and checked by the module (modInstantiate)
    if (nodetoclone->tag == ModuleTag)
        return (INode*)modInstantiate(pstate, srcgencall, (ModuleNode*)nodetoclone);

    // Which members the instance lacks is settled before anything is cloned or
    // mapped: evaluating a clause may analyze the argument's own methods.
    Nodes *late = NULL;
    Nodes *absent = nodetoclone->tag == StructTag
        ? genericAbsentMembers(pstate, srcgencall, (StructNode*)nodetoclone, srcgencall->args, &late) : NULL;

    // A generic function's own name is not mapped: written bare in its body, it
    // is a call whose type arguments are inferred, as it is anywhere else
    uint32_t dclpos = cloneDclPush();
    INode *shell = nodetoclone->tag == StructTag ? genericReserve(nodetoclone) : NULL;
    INode *instance = genericClone(pstate, srcgencall, nodetoclone, genericinfo, shell, absent, late);
    cloneDclPop(dclpos);

    // Type check the instanced declaration. A generic method's instance is
    // checked as a method of the type that owns it, whatever type the call
    // naming it is in, as any method reached by demand is (fnCallDemandCandidates).
    INode *owner = inodeGetOwner(instance);
    if (instance->tag == FnDclTag && (instance->flags & FlagMethFld) && owner && owner->tag == StructTag) {
        TypeCheckState tstate;
        tstate.typenode = owner;
        tstate.fn = NULL;
        tstate.scope = 0;
        tstate.extend = NULL;
        inodeTypeCheckAny(&tstate, &instance);
    }
    else
        inodeTypeCheckAny(pstate, &instance);

    return instance;
}

// What a type's use keeps of the type arguments it was written with
typedef struct {
    INode *genuse;
    Nodes *written;
} GenericUseArgs;

// Whether a type argument holds a borrow is read from its layout, so a use
// made as a reference's target, whose arguments' layouts may be waiting, is
// decided again once they are done
static void genericInstanceUseSettle(TypeCheckState *pstate, INode *use, void *extra) {
    GenericUseArgs *args = (GenericUseArgs *)extra;
    lifeUseInstance((NameUseNode*)use, args->genuse, args->written);
}

// The use of an instance, standing where 'srcgencall' named it. A type's use
// keeps the lifetimes the generic's name was given and the type arguments as
// 'written', for what lifetimes they name (lifeUseInstance).
static INode *genericInstanceUse(TypeCheckState *pstate, INode *instance, FnCallNode *srcgencall, Nodes *written) {
    INode *use = newNameUseFromDclNode(instance, (INode*)srcgencall);
    // A function's use keeps them too where they carry brands: a call binds
    // the instance's brands, named by place, to those (fnCallFinalizeArgs)
    if (instance->tag == StructTag || (lifeInvariantSeen && instance->tag == FnDclTag))
        lifeUseInstance((NameUseNode*)use, instance->tag == StructTag ? srcgencall->objfn : NULL, written);
    if (instance->tag == StructTag && written && structTargetDeferring()) {
        GenericUseArgs *args = memAllocBlk(sizeof(GenericUseArgs));
        args->genuse = srcgencall->objfn;
        args->written = written;
        structDeferCheck(pstate, genericInstanceUseSettle, use, args);
    }
    return use;
}

// Does a 'where' list read its arguments' layouts as it is decided? Each clause
// does but a 'Sendable' one standing alone: whether a type may cross threads is
// answered provisionally of a type not yet laid out, noted, and judged again
// once every type is (genericSendableNote), so an actor's handle, whose
// Mailbox's message type holds the handle back by value, is no cycle.
static int genericWhereReadsLayout(Nodes *where) {
    if (where == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if ((*nodesp)->tag != IsTag || genericNamedTrait(((CastNode*)*nodesp)->typ) != sendableTrait)
            return 1;
    }
    return 0;
}

// Does a condition decide anything about this generic's instances: a 'where'
// on the generic, on its enum, or on a member of either or of a variant?
//
// 'typelevel' asks only for those that cannot wait: a member's clause can, when
// an argument is unsettled (genericAbsentMembers, 'late'); an 'is' entry's and
// the generic's own cannot.
static int genericMembersConditioned(StructNode *type, int typelevel) {
    if (type->condis)
        return 1;
    if (typelevel)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&type->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag && ((FnDclNode*)*nodesp)->where != NULL)
            return 1;
    }
    return 0;
}

static int genericConditioned(INode *generic, GenericInfo *genericinfo, int typelevel) {
    if (generic->tag == FnDclTag)
        return genericWhereReadsLayout(((FnDclNode*)generic)->where);
    if (generic->tag != StructTag)
        return 0;
    StructNode *type = (StructNode*)generic;
    if (genericWhereReadsLayout(genericinfo->where) || genericMembersConditioned(type, typelevel))
        return 1;
    StructNode *owner = (type->flags & HasTagField) ? structGetBaseTrait(type) : NULL;
    if (owner == NULL)
        return 0;
    if ((owner->genericinfo && genericWhereReadsLayout(owner->genericinfo->where))
        || genericMembersConditioned(owner, typelevel))
        return 1;
    if (owner->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(owner->derived, cnt, nodesp)) {
            if (genericMembersConditioned((StructNode*)*nodesp, typelevel))
                return 1;
        }
    }
    return 0;
}

// Does a 'where' list, a type parameter's bound among it, ask about argument 'i'?
// A clause not of the form 'T is Trait' is taken to ask about every argument.
static int genericWhereNamesArg(Nodes *where, Nodes *parms, Nodes *args, uint32_t i) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if ((*nodesp)->tag != IsTag)
            return 1;
        if (genericNamedTrait(((CastNode*)*nodesp)->typ) == sendableTrait)
            continue;
        GenVarDclNode *parm;
        genericClauseType((CastNode*)*nodesp, parms, args, &parm);
        if (parm == NULL || parm == (GenVarDclNode*)nodesGet(parms, i))
            return 1;
    }
    return 0;
}

// Does a condition the instance cannot wait for -- its own 'where', its
// enum's, or an 'is' entry's -- read argument 'i' of the generic's instance?
static int genericTypeReadsArg(INode *generic, GenericInfo *genericinfo, Nodes *args, uint32_t i) {
    if (generic->tag != StructTag)
        return 1;
    StructNode *type = (StructNode*)generic;
    StructNode *owner = (type->flags & HasTagField) ? structGetBaseTrait(type) : NULL;
    if (genericMembersConditioned(type, 1) || (owner && genericMembersConditioned(owner, 1)))
        return 1;
    if (owner && owner->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(owner->derived, cnt, nodesp))
            if (genericMembersConditioned((StructNode*)*nodesp, 1))
                return 1;
    }
    if (genericWhereReadsLayout(genericinfo->where)
        && genericWhereNamesArg(genericinfo->where, genericinfo->parms, args, i))
        return 1;
    if (owner && owner->genericinfo && genericWhereReadsLayout(owner->genericinfo->where)
        && genericWhereNamesArg(owner->genericinfo->where, owner->genericinfo->parms, args, i))
        return 1;
    return 0;
}

// An instance of a generic made as a reference's target, whose layout waits
// (structTargetWait): 'Option[Node]' named inside Node's own layout. Laying it
// out first to decide a condition would lay Node out inside itself. So does a
// plain struct or enum named as the argument and not laid out yet:
// 'List[V]' inside the struct a variant of V holds, where V's layout asks that
// struct's, which is in flight.
static int genericArgWaiting(INode *arg) {
    if (arg->tag == ULitTag || !isTypeNode(arg))
        return 0;
    INode *dcl = itypeGetTypeDcl(arg);
    return dcl->tag == StructTag && !(dcl->flags & (TypeChecking | TypeChecked));
}

// An instance of a generic enum made as a reference's target: laid out with
// its variants, and its discriminant's width settled, once no layout is in
// flight -- unless a use holding it by value laid it out first
typedef struct {
    Nodes *variants;
    int firstinstance;
} GenericEnumWait;

static void genericEnumInstanceLayout(TypeCheckState *pstate, INode *instrait, void *extra) {
    GenericEnumWait *wait = (GenericEnumWait *)extra;
    if (instrait->flags & (TypeChecking | TypeChecked))
        return;
    structTypeCheckEnumInstance(pstate, (StructNode*)instrait);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(wait->variants, cnt, nodesp))
        inodeTypeCheckAny(pstate, nodesp);
    if (wait->firstinstance)
        structSetTagWidth((StructNode*)instrait);
}

// The type argument at 'i' as an instance is made from it: with no lifetime
// named in it, or, for a parameter the generic function bounds ('[T + 'a]'),
// with every lifetime it names renamed the bound's ''+T' (lifetime.h,
// "Lifetime bounds"), which the function's order holds outlasts ''a'. The
// renaming is the parameter's, not the use's, so it multiplies no instance.
static INode *genericInstanceArg(INode *generic, GenericInfo *info, uint32_t i, INode *arg) {
    // A number has no lifetime
    if (arg->tag == ULitTag)
        return arg;
    if (info->parms && i < info->parms->used && isTypeNode(arg)) {
        Name *tparm = ((GenVarDclNode*)nodesGet(info->parms, i))->namesym;
        if (lifeParmBounded(generic, tparm))
            return lifeRenamed(arg, lifeBoundName(tparm));
    }
    return lifeErased(arg);
}

// Verify arguments are types, check if instantiated, instantiate if needed and return ptr to it
//
// A type argument list the generic cannot be instantiated from yields a node
// already marked as bad rather than nothing at all. The caller substitutes it
// for the call and carries on type checking the rest of the function, which is
// where the next real diagnostic is.
INode *genericMemoize(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone,
        GenericInfo *genericinfo, Name *name) {

    // A trait with associated types is made with them only where a type meeting
    // it said them (genericAssocComplete): a use writes the other arguments alone,
    // and that names no one trait
    int completed = srcgencall == genericAssocCompleted;
    if (completed)
        genericAssocCompleted = NULL;
    if (!completed && nodetoclone->tag == StructTag && genericHasAssoc(genericinfo)) {
        uint32_t written = genericWrittenCount(genericinfo);
        StructNode *trait = (StructNode*)nodetoclone;
        char assocs[256] = "";
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(genericinfo->parms, parmcnt, parmp)) {
            if (!genericIsAssocParm(*parmp))
                continue;
            size_t used = strlen(assocs);
            snprintf(assocs + used, sizeof(assocs) - used, "%s%s", assocs[0] ? ", " : "", &((GenVarDclNode*)*parmp)->namesym->namestr);
        }
        uint32_t given = srcgencall->args ? srcgencall->args->used : 0;
        if (given == written)
            errorMsgNode((INode*)srcgencall, ErrorAssocUnsaid,
                "%s has an associated type (%s), which the type meeting it says, so it is named where a type meets it: a bound, '[C: %s[...]]', or an 'is' entry. A virtual reference to such a trait is not built yet.",
                &trait->namesym->namestr, assocs, &trait->namesym->namestr);
        else
            errorMsgNode((INode*)srcgencall, ErrorArgCount,
                "%s takes %u type argument%s, and this gives %u: its associated types (%s) are said by the type that meets it. (Fixing one in its arguments, as Rust's 'Item = u8', is not built.)",
                &trait->namesym->namestr, written, written == 1 ? "" : "s", given, assocs);
        return newErrorNode((INode*)srcgencall);
    }

    // Verify expected number of generic parameters
    uint32_t expected = genericinfo->parms ? genericinfo->parms->used : 0;
    if (srcgencall->args->used != expected) {
        errorMsgNode((INode*)srcgencall, ErrorArgCount, "Incorrect number of arguments vs. parameters expected");
        return newErrorNode((INode*)srcgencall);
    }

    // Verify all arguments are types
    INode **nodesp;
    uint32_t cnt;
    int badargs = 0;
    uint32_t argi = 0;
    for (nodesFor(srcgencall->args, cnt, nodesp)) {
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(genericinfo->parms, argi++);
        // A value parameter takes a number, written or inferred, and is held
        // as a literal of its type
        if (parm->valtype) {
            INode *valarg = genericWrittenValueArg(parm, *nodesp);
            if (valarg)
                *nodesp = valarg;
            else
                badargs = 1;
        }
        else if (!isTypeNode(*nodesp)) {
            errorMsgNode((INode*)*nodesp, ErrorNotType, "Expected a type for a generic parameter");
            badargs = 1;
        }
        // A bare generic, or a module's or module trait's name, refused here
        // rather than wherever the instance uses its parameter, which would
        // report it once per use
        else if (itypeRefuseBareGeneric(*nodesp) || itypeRefuseModule(*nodesp))
            badargs = 1;
    }
    if (badargs)
        return newErrorNode((INode*)srcgencall);

    if (!genericinfo->memonodes)
        genericinfo->memonodes = newNodes(2);

    // The arguments as an instance is made from them (below), for their
    // brands, which an instance keeps by their order (lifeCanonBrands)
    Nodes *erased = NULL;
    if (lifeInvariantSeen) {
        erased = newNodes(srcgencall->args->used);
        for (nodesFor(srcgencall->args, cnt, nodesp))
            nodesAdd(&erased, genericInstanceArg(nodetoclone, genericinfo, erased->used, *nodesp));
        lifeCanonBrands(erased);
    }

    // Check whether these types have already been instantiated for this generic
    // memonodes holds pairs of nodes: an FnCallNode and what it instantiated
    // A match is the first FnCallNode whose types match what we want
    for (nodesFor(genericinfo->memonodes, cnt, nodesp)) {
        FnCallNode *fncallprior = (FnCallNode *)*nodesp;
        nodesp++; cnt--; // skip to instance srcgencallp
        int match = 1;
        INode **priornodesp;
        uint32_t priorcnt;
        INode **nownodesp = &nodesGet(srcgencall->args, 0);
        INode **erasednodesp = erased ? &nodesGet(erased, 0) : NULL;
        for (nodesFor(fncallprior->args, priorcnt, priornodesp)) {
            if (!genericArgSame(*priornodesp, *nownodesp)
                || (erasednodesp && (*erasednodesp)->tag != ULitTag && !lifeBrandsEqual(*priornodesp, *erasednodesp))) {
                match = 0;
                break;
            }
            nownodesp++;
            if (erasednodesp)
                erasednodesp++;
        }
        if (match) {
            // Return a namenode pointing to dcl instance
            return genericInstanceUse(pstate, *nodesp, srcgencall, srcgencall->args);
        }
    }

    // No match found. The instance is made from its arguments with no
    // lifetime named in them: a lifetime is never instanced, so one instance
    // serves every use, whatever lifetimes each names, and its use keeps the
    // arguments as written (genericInstanceUse). An invariant lifetime is the
    // exception: a key stays a key, and which of its arguments' brands are one
    // and which apart is kept, each named by its order (lifeCanonBrands). So is
    // a parameter the generic bounds, '[T + 'a]': its argument's lifetimes are
    // all renamed the bound's one name (genericInstanceArg).
    Nodes *written = srcgencall->args;
    if (erased)
        srcgencall->args = erased;
    else {
        srcgencall->args = newNodes(written->used);
        for (nodesFor(written, cnt, nodesp))
            nodesAdd(&srcgencall->args, genericInstanceArg(nodetoclone, genericinfo, srcgencall->args->used, *nodesp));
    }

    // A condition asks what its arguments are -- whether one moves, holds a
    // borrow, may cross threads, fits a trait -- which is read from their
    // layouts. Named as a reference's target, an argument's layout may be
    // waiting (structTargetWait), so where a condition decides anything about
    // the instance, the arguments are laid out first.
    //
    // An argument that is an instance still waiting is left so when only a
    // member's clause reads it: that member is decided once no layout is in
    // flight (genericLateWhere), as it is for an argument in flight. A
    // condition the instance cannot wait for lays it out wherever the instance
    // is made, since an argument written as an instance waits whatever the
    // instance is made as (fnCallTypeCheckNode).
    int typecond = genericConditioned(nodetoclone, genericinfo, 1);
    if ((structTargetDeferring() && genericConditioned(nodetoclone, genericinfo, 0)) || typecond) {
        int deferring = structTargetDeferring();
        uint32_t target = structTargetSuspend();
        uint32_t argi = 0;
        for (nodesFor(written, cnt, nodesp)) {
            uint32_t i = argi++;
            if ((*nodesp)->tag == ULitTag)
                continue;
            if (genericArgWaiting(*nodesp) ? (typecond && genericTypeReadsArg(nodetoclone, genericinfo, written, i)) : deferring)
                structTypeSettle(pstate, *nodesp);
        }
        structTargetResume(target);
    }

    // A constraint the arguments do not meet refuses the instance here, before
    // anything of it is made.
    if (!genericRequirementsMet(pstate, srcgencall, nodetoclone, genericinfo, name))
        return newErrorNode((INode*)srcgencall);

    // Instantiate the dcl generic.
    // Instantiating analyzes the new instance, which may instantiate this same
    // generic again at larger type arguments, so this is where depth is counted.
    if (!genericInstantiateEnter((INode*)srcgencall))
        return newErrorNode((INode*)srcgencall);

    INode *retinstance;
    // If node is not a tagged-field trait/struct, we can just instantiate it and be done
    if (nodetoclone->tag != StructTag || !(nodetoclone->flags & HasTagField)) {
        retinstance = genericInstantiate(pstate, srcgencall, nodetoclone, genericinfo, name);
    }
    else {
        // For tag-based trait/struct, instantiate the base trait and all its variants.
        // The base trait and every variant are cloned and remembered before any of
        // them is type checked: a variant's body may name a later sibling at these
        // same arguments, and so may the enum's own static function, checked with
        // the enum. A variant not yet remembered would be a miss that instantiates
        // the whole enum again, endlessly.
        //
        // Every instance is reserved before any is cloned, and the generic enum
        // and each generic variant mapped to its own: inside the enum's braces --
        // its own methods, and each variant's body -- a bare 'Mb', 'No' or 'Mb.No'
        // names the instance at these arguments, and a sibling named bare may
        // come later than the body naming it.
        StructNode *basetrait = structGetBaseTrait((StructNode*)nodetoclone);
        Nodes *basememo = basetrait->genericinfo->memonodes;
        int firstinstance = basememo == NULL || basememo->used == 0;
        // What each of them lacks at these arguments, settled before any is
        // cloned or mapped
        Nodes *traitlate = NULL;
        Nodes *traitabsent = genericAbsentMembers(pstate, srcgencall, basetrait, srcgencall->args, &traitlate);
        Nodes *absents = newNodes(basetrait->derived->used);
        Nodes *lates = newNodes(basetrait->derived->used);
        for (nodesFor(basetrait->derived, cnt, nodesp)) {
            Nodes *varlate = NULL;
            nodesAdd(&absents, (INode*)genericAbsentMembers(pstate, srcgencall, (StructNode*)*nodesp, srcgencall->args, &varlate));
            nodesAdd(&lates, (INode*)varlate);
        }
        uint32_t dclpos = cloneDclPush();
        INode *traitshell = genericReserve((INode*)basetrait);
        Nodes *shells = newNodes(basetrait->derived->used);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&shells, genericReserve(*nodesp));
        INode *instrait = genericClone(pstate, srcgencall, (INode*)basetrait, basetrait->genericinfo, traitshell, traitabsent, traitlate);
        if (basetrait == (StructNode*)nodetoclone)
            retinstance = instrait;

        // A variant's body may also name a static function, static or overload
        // name of the enum bare, which name resolution bound to the generic's
        // member; it is the instance's that has a symbol, so the variants are
        // cloned with each such member mapped to the instance's.
        structCloneMapMembers(basetrait, (StructNode*)instrait);
        Nodes *variants = newNodes(basetrait->derived->used);
        INode **shellp = &nodesGet(shells, 0);
        INode **absentp = &nodesGet(absents, 0);
        INode **latep = &nodesGet(lates, 0);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&variants, genericClone(pstate, srcgencall, *nodesp, ((StructNode*)*nodesp)->genericinfo,
                *shellp++, (Nodes*)*absentp++, (Nodes*)*latep++));
        cloneDclPop(dclpos);
        // The instance's 'derived' lists its own variants, and lists all of them
        // before the enum or any variant is type checked: a variant's method body
        // may match a value of the enum, and its match is exhaustive only against
        // the whole set -- including a variant reached first from the enum's own
        // check, as a static function building it reaches it.
        Nodes **instraitderived = &((StructNode*)instrait)->derived;
        for (nodesFor(variants, cnt, nodesp))
            nodesAdd(instraitderived, *nodesp);
        // Made as a reference's target while a layout is in flight, the
        // instance is laid out, with its variants, once none is
        if (structTargetDeferring()) {
            GenericEnumWait *wait = memAllocBlk(sizeof(GenericEnumWait));
            wait->variants = variants;
            wait->firstinstance = firstinstance;
            structDeferLayout(pstate, genericEnumInstanceLayout, instrait, wait);
            INode **instp = &nodesGet(variants, 0);
            for (nodesFor(basetrait->derived, cnt, nodesp)) {
                if (*nodesp == (INode*)nodetoclone)
                    retinstance = *instp;
                ++instp;
            }
        }
        else {
            structTypeCheckEnumInstance(pstate, (StructNode*)instrait);
            INode **instp = &nodesGet(variants, 0);
            for (nodesFor(basetrait->derived, cnt, nodesp)) {
                inodeTypeCheckAny(pstate, instp);
                if (*nodesp == (INode*)nodetoclone)
                    retinstance = *instp;
                ++instp;
            }

            // The discriminant's width follows the largest tag value, and the instance's
            // own type check left it here (structTypeCheckEnumInstance). It is settled
            // once per generic: the discriminant node is shared by the template and
            // every instance, and their tag values are the template's, so a later
            // instance could only report a declared integer type's overflow again.
            if (firstinstance)
                structSetTagWidth((StructNode*)instrait);
        }
    }
    genericInstantiateExit();

    return genericInstanceUse(pstate, retinstance, srcgencall, written);
}

// Obtain GenericInfo from node, if it exists
GenericInfo *genericGetInfo(INode *node) {
    switch (node->tag) {
    case FnDclTag:
        return ((FnDclNode *)node)->genericinfo;
    case StructTag:
        return ((StructNode *)node)->genericinfo;
    case ModuleTag:
        return ((ModuleNode *)node)->genericinfo;
    default:
        return NULL;
    }
}

// The name use of an instance stands where the generic's name was written, so
// it keeps whether that was reached through a namespace. 'Holder.pick(&h, 6)'
// names a method with its receiver passed, and must not be taken for a bare
// method name lowered to 'self.pick'.
static void genericKeepQualified(INode *instance, INode *generic) {
    if (isNameUseNode(instance) && !inodeIsError(instance))
        instance->flags |= generic->flags & FlagQualified;
}

// Perform generic substitution, if this is a correctly set up generic "srcgencall"
// Return 1 if generic subsituted or error. Return 0 if not generic or it leaves behind a lit/srcgencall that needs processing.
int genericSubstitute(TypeCheckState *pstate, FnCallNode **srcgencallp, INode *expectType) {
    // Return if not generic, otherwise gather data needed to substitute
    FnCallNode *srcgencall = *srcgencallp;
    // Only a name that names a value or a type can name a generic
    INode *objfn = srcgencall->objfn;
    if (!isNameUseNode(objfn) || !(isExpNode(objfn) || isTypeNode(objfn)))
        return 0;
    INode *nodetoclone = nameUseGetDcl((NameUseNode*)objfn);
    GenericInfo *genericinfo = genericGetInfo(nodetoclone);
    if (!genericinfo)
        return 0;
    Name *name = inodeGetName(nodetoclone);

    // Decide whether generic requires inference of type parameters
    // For now, we decide based on whether any parameter is a type
    int usesTypeArgs = 0;
    INode **argsp;
    uint32_t cnt;
    if (srcgencall->args) {
        for (nodesFor(srcgencall->args, cnt, argsp)) {
            if (isTypeNode(*argsp))
                usesTypeArgs = 1;
        }
    }
    // A function taking a value parameter, called with brackets, 'dot[3]': there
    // is nothing else an index of a function can mean, so what is in them are
    // its generic arguments (genericMemoize refuses one that is no number)
    if (nodetoclone->tag == FnDclTag && (srcgencall->flags & FlagIndex) && srcgencall->args != NULL
        && genericHasValueParm(genericinfo))
        usesTypeArgs = 1;

    // Since the arguments are types, no inference is needed
    // Replace gennnone with instantiated generic, substituting parameters
    // Then type check the substituted, instantiated srcgencallp
    // A generic module is never inferred: nothing is passed to it to infer from,
    // so what it is given is checked as type arguments, and refused if it is not
    if (usesTypeArgs || nodetoclone->tag == ModuleTag) {
        *((INode**)srcgencallp) = genericMemoize(pstate, srcgencall, nodetoclone, genericinfo, name);
        genericKeepQualified(*((INode**)srcgencallp), objfn);
        inodeTypeCheckAny(pstate, (INode **)srcgencallp);
        return 1;
    }

    // Before we can get a generic instantiation, we have to first infer the type parameters from genfncall arguments.
    // We know arguments have been type checked
    // If successful, we are left with an instantiated function call/type that FnCall still needs to type-check.

    // Inject empty generic call as the place to fill in and hold the inferred type parameters
    // A node built here takes the lexer's position, which by type check is the
    // end of the source, so it is given the position of the call it stands for.
    uint32_t nparms = genericinfo->parms->used;
    FnCallNode *inferredgencall = newFnCallNode(srcgencall->objfn, nparms);
    inodeLexCopy((INode*)inferredgencall, (INode*)srcgencall);
    srcgencall->objfn = (INode*)inferredgencall;
    while (nparms--)
        nodesAdd(&inferredgencall->args, (INode*)NULL);

    // Inference varies depending on the kind of generic
    switch (nodetoclone->tag) {
    case FnDclTag: {
        // Infer based on function call arguments. A method named bare inside
        // its type's braces is called on an implicit 'self' that is not among
        // the arguments; named through its type, 'Holder.pick(&h, 6)', its
        // receiver is the first argument.
        uint32_t firstparm = (nodetoclone->flags & FlagMethFld) && !(objfn->flags & FlagQualified) ? 1 : 0;
        if (genericInferFnParms(pstate, genericinfo->parms,
            (FnSigNode*)itypeGetTypeDcl(((FnDclNode *)nodetoclone)->vtype), NULL, srcgencall->args, firstparm,
            (INode*)srcgencall, inferredgencall, ((FnDclNode *)nodetoclone)->where, expectType, "generic function") == 0)
            return 1;
        break;
    }
    case StructTag: {
        // Infer based on type constructor arguments, then the bounds, then
        // where the value goes (a variant's enum, or the type itself), and last
        // what a literal's own type says
        StructNode *genstruct = (StructNode*)itypeGetTypeDcl(nodetoclone);
        if (genericInferStructParms(pstate, genericinfo->parms, genstruct, srcgencall, inferredgencall) == 0)
            return 1;
        genericInferFromBounds(genericinfo->parms, genericinfo->where, inferredgencall);
        if (genericExpectUsable(expectType))
            genericInferFromExpected(genericinfo->parms, genericResultTemplate(genstruct, genericinfo),
                expectType, inferredgencall);
        if (genericInferStructLiterals(genericinfo->parms, genstruct, srcgencall, inferredgencall) == 0)
            return 1;
        break;
    }
    default:
        // genericGetInfo answers for a function, a struct and a module, it was
        // asked about this very node, and a module took the explicit path above
        errorUnreachable((INode*)srcgencall, "a generic that is neither a function nor a struct");
        return 1;
    }

    // Be sure all expected generic parameters were inferred
    for (nodesFor(inferredgencall->args, cnt, argsp)) {
        if (*argsp == NULL) {
            genericMissingParms(inferredgencall, genericinfo->parms, name, (INode*)srcgencall, 0);
            *((INode**)srcgencallp) = newErrorNode((INode*)srcgencall);
            return 1;
        }
    }

    // A callable taken as '&F' may only be read: refused here, at the argument
    if (nodetoclone->tag == FnDclTag) {
        uint32_t firstparm = (nodetoclone->flags & FlagMethFld) && !(objfn->flags & FlagQualified) ? 1 : 0;
        if (!genericCallablePermCheck((FnDclNode*)nodetoclone, srcgencall->args, firstparm)) {
            *((INode**)srcgencallp) = newErrorNode((INode*)srcgencall);
            return 1;
        }
    }

    // Now let's instantiate generic "call", substituting instantiated srcgencallp in objfn
    INode *instance = genericMemoize(pstate, (FnCallNode*)srcgencall->objfn, nodetoclone, genericinfo, name);
    if (inodeIsError(instance)) {
        // Nothing was instantiated, so there is nothing left for the call to
        // call. Replace the call itself and tell the caller it is handled.
        *((INode**)srcgencallp) = instance;
        return 1;
    }
    genericKeepQualified(instance, objfn);
    *((INode**)&srcgencall->objfn) = instance;

    return 0;
}

// The instance of generic method 'genmeth' that a call on a receiver names:
// 'h.pick(6)', or 'h.pick[i32](6)' with 'typeargs' written. Unwritten, the type
// arguments are inferred from the receiver, matched against 'self', and the
// call's arguments, which do not yet hold the receiver, so they match the
// method's parameters after 'self'. Returns NULL
// once an error is reported.
FnDclNode *genericMethodInstance(TypeCheckState *pstate, FnCallNode *callnode, FnDclNode *genmeth, Nodes *typeargs,
        INode *expectType) {
    GenericInfo *genericinfo = genmeth->genericinfo;
    uint32_t nparms = genericinfo->parms->used;
    FnCallNode *gencall = newFnCallNode(newNameUseFromDclNode((INode*)genmeth, callnode->methfld), nparms);
    inodeLexCopy((INode*)gencall, (INode*)callnode);
    INode **argsp;
    uint32_t cnt;
    if (typeargs) {
        for (nodesFor(typeargs, cnt, argsp))
            nodesAdd(&gencall->args, *argsp);
    }
    else {
        while (nparms--)
            nodesAdd(&gencall->args, (INode*)NULL);
        if (genericInferFnParms(pstate, genericinfo->parms, (FnSigNode*)itypeGetTypeDcl(genmeth->vtype),
            callnode->objfn, callnode->args, 1, (INode*)callnode, gencall, genmeth->where, expectType, "generic function") == 0)
            return NULL;
        for (nodesFor(gencall->args, cnt, argsp)) {
            if (*argsp == NULL) {
                genericMissingParms(gencall, genericinfo->parms, genmeth->namesym, (INode*)callnode, 0);
                return NULL;
            }
        }
    }
    if (!genericCallablePermCheck(genmeth, callnode->args, 1))
        return NULL;
    INode *instance = genericMemoize(pstate, gencall, (INode*)genmeth, genericinfo, genmeth->namesym);
    if (inodeIsError(instance))
        return NULL;
    return (FnDclNode*)nameUseGetDcl((NameUseNode*)instance);
}

// Does every type parameter this type names have its argument?
static int genericTypeArgsKnown(INode *type, Nodes *parms, Nodes *args) {
    if (type == NULL)
        return 1;
    if (isNameUseNode(type)) {
        INode *dcl = ((NameUseNode*)type)->dclnode;
        if (dcl && dcl->tag == GenVarDclTag) {
            for (uint32_t j = 0; j < parms->used; ++j)
                if (nodesGet(parms, j) == dcl)
                    return nodesGet(args, j) != NULL;
        }
        return 1;
    }
    INode **nodesp;
    uint32_t cnt;
    switch (type->tag) {
    case RefTag: case BorrowTag: case ArrayRefTag: case ArrayBorrowTag: case VirtRefTag: case AllocateTag:
        return genericTypeArgsKnown(((RefNode*)type)->vtexp, parms, args);
    case PtrTag: case DerefTag:
        return genericTypeArgsKnown(((StarNode*)type)->vtexp, parms, args);
    case FnSigTag:
        for (nodesFor(((FnSigNode*)type)->parms, cnt, nodesp))
            if (!genericTypeArgsKnown(((VarDclNode*)*nodesp)->vtype, parms, args))
                return 0;
        return genericTypeArgsKnown(((FnSigNode*)type)->rettype, parms, args);
    case FnCallTag:
        if (!genericTypeArgsKnown(((FnCallNode*)type)->objfn, parms, args))
            return 0;
        if (((FnCallNode*)type)->args)
            for (nodesFor(((FnCallNode*)type)->args, cnt, nodesp))
                if (!genericTypeArgsKnown(*nodesp, parms, args))
                    return 0;
        return 1;
    default:
        return 1;
    }
}

// The signature bound of the type parameter that parameter 'pos' of a generic
// function (a method's self counted) is, or is a reference to, and the
// permission of that reference ('*refperm', NULL for a parameter taken by
// value); NULL when it is no type parameter or has no signature bound. The
// bound is as written, in terms of the generic's parameters.
FnSigNode *genericParmBound(FnDclNode *generic, uint32_t pos, INode **refperm) {
    *refperm = NULL;
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    if (generic->genericinfo == NULL || pos >= gsig->parms->used)
        return NULL;
    INode *ptype = ((VarDclNode*)nodesGet(gsig->parms, pos))->vtype;
    // A borrowed callable, 'f &<fn(a: &T) i32': the signature is written in the parameter
    if (ptype && ptype->tag == VirtRefTag && ((RefNode*)ptype)->region == (INode*)borrowRef
        && ((RefNode*)ptype)->vtexp->tag == FnSigTag) {
        RefNode *vref = (RefNode*)ptype;
        *refperm = vref->perm == unknownType ? (INode*)newPermUseNode(roPerm) : vref->perm;
        return (FnSigNode*)vref->vtexp;
    }
    while (ptype && (ptype->tag == RefTag || ptype->tag == BorrowTag)) {
        if (*refperm == NULL)
            *refperm = ((RefNode*)ptype)->perm;
        ptype = ((RefNode*)ptype)->vtexp;
    }
    if (!nameUseNames(ptype, GenVarDclTag))
        return NULL;
    INode *parmdcl = nameUseGetDcl((NameUseNode*)ptype);
    FnSigNode *bound = NULL;
    if (generic->where) {
        INode **condp;
        uint32_t cnt;
        for (nodesFor(generic->where, cnt, condp)) {
            if ((*condp)->tag != IsTag)
                continue;
            CastNode *clause = (CastNode*)*condp;
            if (clause->typ->tag == FnSigTag && isNameUseNode(clause->exp)
                && ((NameUseNode*)clause->exp)->dclnode == parmdcl)
                bound = (FnSigNode*)clause->typ;
        }
    }
    return bound;
}

// The trait that bounds the type parameter parameter 'pos' of a generic function
// is, or is a reference to (the permission of that reference in '*refperm', NULL
// by value), when the bound is a plain trait: where a closure literal given to
// that parameter fills the trait's one method. NULL otherwise.
StructNode *genericParmTraitBound(FnDclNode *generic, uint32_t pos, INode **refperm) {
    *refperm = NULL;
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    if (generic->genericinfo == NULL || pos >= gsig->parms->used || generic->where == NULL)
        return NULL;
    INode *ptype = ((VarDclNode*)nodesGet(gsig->parms, pos))->vtype;
    while (ptype && (ptype->tag == RefTag || ptype->tag == BorrowTag)) {
        if (*refperm == NULL)
            *refperm = ((RefNode*)ptype)->perm;
        ptype = ((RefNode*)ptype)->vtexp;
    }
    if (!nameUseNames(ptype, GenVarDclTag))
        return NULL;
    INode *parmdcl = nameUseGetDcl((NameUseNode*)ptype);
    INode **condp;
    uint32_t cnt;
    for (nodesFor(generic->where, cnt, condp)) {
        if ((*condp)->tag != IsTag)
            continue;
        CastNode *clause = (CastNode*)*condp;
        if (!isNameUseNode(clause->exp) || ((NameUseNode*)clause->exp)->dclnode != parmdcl || !isNameUseNode(clause->typ))
            continue;
        INode *dcl = nameUseGetDcl((NameUseNode*)clause->typ);
        if (dcl && dcl->tag == StructTag && (dcl->flags & TraitType) && !(dcl->flags & EnumType)
            && ((StructNode*)dcl)->genericinfo == NULL)
            return (StructNode*)dcl;
    }
    return NULL;
}

// A callable given to a generic parameter taken as '&F' may only be read:
// the instance calls it through a borrow that grants no change. One whose '()'
// takes 'self: &mut' is refused here, at the caller's argument and in the
// author's terms, before an instance is made whose body would be refused at
// the call, far from what was written. Answers 0 once reported.
int genericCallablePermCheck(FnDclNode *generic, Nodes *valueargs, uint32_t firstparm) {
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    for (uint32_t j = 0; valueargs && j < valueargs->used; ++j) {
        uint32_t pos = firstparm + j;
        if (pos >= gsig->parms->used)
            break;
        INode *refperm;
        // Bound by a trait with a method, only a closure literal's struct is judged here:
        // a struct of the author's meets the trait as it always did
        int bytrait = 0;
        StructNode *boundtrait = NULL;
        if (genericParmBound(generic, pos, &refperm) == NULL) {
            boundtrait = genericParmTraitBound(generic, pos, &refperm);
            if (boundtrait == NULL)
                continue;
            bytrait = 1;
        }
        // A closure literal whose method needs a stronger 'self' than the trait's
        // does not meet the trait, however the parameter holds it
        if (bytrait && isExpNode(nodesGet(valueargs, j)) && !inodeIsError(nodesGet(valueargs, j))) {
            INode *carg = nodesGet(valueargs, j);
            INode *ctype = iexpGetTypeDcl(carg);
            if (ctype->tag == RefTag)
                ctype = itypeGetTypeDcl(((RefNode*)ctype)->vtexp);
            ClosureInfo *cinfo = closureOfStruct(ctype);
            char *selfwhy = cinfo && closureMethodMutates(cinfo) ? fnSigVrefSelfRefusal(boundtrait, (StructNode*)ctype) : NULL;
            if (selfwhy) {
                errorMsgNode(carg, ErrorVtableSelf, "%s", selfwhy);
                return 0;
            }
        }
        if (refperm == NULL)
            continue;
        // A '&<fn(...)' parameter is judged by its own conversion, in its own words
        if (((VarDclNode*)nodesGet(gsig->parms, pos))->vtype->tag == VirtRefTag)
            continue;
        INode *permdcl = refperm == unknownType ? (INode*)roPerm : itypeGetTypeDcl(refperm);
        if (permdcl->tag != PermTag || (permGetFlags(permdcl) & MayWrite))
            continue;
        INode *arg = nodesGet(valueargs, j);
        if (!isExpNode(arg) || inodeIsError(arg))
            continue;
        INode *argtype = iexpGetTypeDcl(arg);
        if (argtype->tag == RefTag)
            argtype = itypeGetTypeDcl(((RefNode*)argtype)->vtexp);
        if (argtype->tag != StructTag)
            continue;
        ClosureInfo *clinfo = closureOfStruct(argtype);
        if (bytrait && clinfo == NULL)
            continue;
        Name *methname = clinfo && clinfo->method ? clinfo->method : parensName;
        INode *binding = namespaceFind(&((StructNode*)argtype)->namespace, methname);
        if (binding == NULL || binding->tag != FnDclTag)
            continue;
        FnSigNode *csig = (FnSigNode*)itypeGetTypeDcl(((FnDclNode*)binding)->vtype);
        INode *selftype = csig->tag == FnSigTag && csig->parms->used ? iexpGetTypeDcl(nodesGet(csig->parms, 0)) : NULL;
        if (selftype == NULL || selftype->tag != RefTag || itypeGetTypeDcl(((RefNode*)selftype)->perm) != (INode*)mutPerm)
            continue;
        // The parameter as the generic wrote it: '&F'
        VarDclNode *parm = (VarDclNode*)nodesGet(gsig->parms, pos);
        INode *written = parm->vtype;
        while (written && (written->tag == RefTag || written->tag == BorrowTag))
            written = ((RefNode*)written)->vtexp;
        char *fname = isNameUseNode(written) ? &((NameUseNode*)written)->namesym->namestr : "F";
        errorMsgNode(arg, ErrorCallablePerm,
            "%s takes `%s` as `&%s`, so the callable may only read its state; %s changes it (its `%s` takes `self: &mut`). Take `%s` as `&mut %s` to let it change, or pass a callable that only reads.",
            &generic->namesym->namestr, &parm->namesym->namestr, fname,
            clinfo ? "this closure" : "this one", &methname->namestr, &parm->namesym->namestr, fname);
        return 0;
    }
    return 1;
}

// The signature a closure literal written as argument 'argi' of a call of
// generic function or method 'generic' is to fit: the signature bound of the
// type parameter the argument's parameter is (or is a reference to), with the
// type parameters the call's other arguments name read off them. 'args' are
// the call's arguments, the others checked; 'firstparm' is 1 for a method
// called on a receiver. NULL when the parameter has no signature bound, or
// the types the signature names are not known yet. '*refperm' is the
// permission of the reference the parameter takes the closure through, or NULL
// when it takes it by value.
FnSigNode *genericClosureSig(TypeCheckState *pstate, FnDclNode *generic, Nodes *args, uint32_t firstparm,
        uint32_t argi, INode **refperm) {
    GenericInfo *info = generic->genericinfo;
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    INode *bound = (INode*)genericParmBound(generic, firstparm + argi, refperm);
    if (bound == NULL)
        return NULL;

    // What the other arguments say of the type parameters
    FnCallNode *known = newFnCallNode((INode*)newNameUseNode(anonName), info->parms->used);
    for (uint32_t j = 0; j < info->parms->used; ++j)
        nodesAdd(&known->args, (INode*)NULL);
    for (uint32_t j = 0; j < args->used; ++j) {
        INode *arg = nodesGet(args, j);
        if (j == argi || arg->tag == ClosureTag || !isExpNode(arg) || firstparm + j >= gsig->parms->used)
            continue;
        INode *argtype = ((IExpNode*)arg)->vtype;
        if (argtype == unknownType || argtype == NULL)
            continue;
        genericInferType(known, info->parms, ((VarDclNode*)nodesGet(gsig->parms, firstparm + j))->vtype, argtype);
    }
    // A parameter named only in the bound of another the arguments settled, 'T' in
    // '[T, V: RandomAccess[T], F: fn(x: &T) bool]', is read off that argument's methods
    // as the call's own inference will read it
    genericInferFromBounds(info->parms, generic->where, known);
    if (!genericTypeArgsKnown(bound, info->parms, known->args)) {
        // A return type that names a parameter nothing else has settled ('U' in
        // 'F: fn(x: T) U', which a map's closure gives) waits for the closure's body:
        // the hint then has the parameters alone, and no return type
        FnSigNode *full = (FnSigNode*)bound;
        if (full->tag != FnSigTag)
            return NULL;
        INode **parmp;
        uint32_t pcnt;
        for (nodesFor(full->parms, pcnt, parmp))
            if (!genericTypeArgsKnown(((VarDclNode*)*parmp)->vtype, info->parms, known->args))
                return NULL;
        FnSigNode *open = newFnSigNode();
        inodeLexCopy((INode*)open, (INode*)full);
        open->parms = full->parms;
        open->rettype = unknownType;
        open->spelled = full->spelled;
        bound = (INode*)open;
    }

    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, bound, NULL, 0, info->parms, known->args);
    INode *copy = cloneNode(&cstate, bound);
    clonePopState();
    cloneDclPop(dclpos);
    inodeTypeCheckAny(pstate, &copy);
    if (inodeIsError(copy) || itypeGetTypeDcl(copy)->tag != FnSigTag)
        return NULL;
    return (FnSigNode*)itypeGetTypeDcl(copy);
}

// The type argument 'argi' of a call of a generic is wanted as, when the other
// arguments and the type expected of the call's value say every type parameter
// that parameter names. 'ptypes' are the generic's parameter (or field) types,
// as written in its type parameters, one for each written argument (NULL where
// none is certain). The others are read as the call's own inference will read
// them, quietly, into a scratch call; an argument not yet checked, or this one,
// says nothing. This is what lets 'Some(None)' and 'id2(None, 5)' give their
// 'None' the type its place needs: the parameter's type with those parameters
// replaced. NULL when some parameter is still unsaid, or the copy is refused.
static INode *genericArgExpect(TypeCheckState *pstate, GenericInfo *info, Nodes *where, INode *result,
        INode **ptypes, Nodes *args, uint32_t argi, INode *expectType) {
    if (ptypes[argi] == NULL)
        return NULL;
    FnCallNode *known = newFnCallNode((INode*)newNameUseNode(anonName), info->parms->used);
    uint32_t j;
    for (j = 0; j < info->parms->used; ++j)
        nodesAdd(&known->args, (INode*)NULL);
    ++genericExpectMode;
    for (j = 0; j < args->used; ++j) {
        INode *arg = nodesGet(args, j);
        INode *val = arg->tag == NamedValTag ? ((NamedValNode*)arg)->val : arg;
        if (j == argi || ptypes[j] == NULL || val->tag == ClosureTag || !isExpNode(val) || inodeIsError(val))
            continue;
        INode *argtype = ((IExpNode*)val)->vtype;
        if (argtype == NULL || argtype == unknownType || litIsUntypedNull(val) || genericArgIsAdaptable(val, ptypes[j]))
            continue;
        genericInferType(known, info->parms, ptypes[j], argtype);
    }
    genericInferFromBounds(info->parms, where, known);
    genericInferFromExpected(info->parms, result, expectType, known);
    for (j = 0; j < args->used; ++j) {
        INode *arg = nodesGet(args, j);
        INode *val = arg->tag == NamedValTag ? ((NamedValNode*)arg)->val : arg;
        if (j == argi || ptypes[j] == NULL || !isExpNode(val) || !genericArgIsAdaptable(val, ptypes[j]))
            continue;
        if (genericCapturedType(known, info->parms, ptypes[j]) == NULL && ((IExpNode*)val)->vtype != unknownType)
            genericInferType(known, info->parms, ptypes[j], ((IExpNode*)val)->vtype);
    }
    --genericExpectMode;
    if (!genericTypeArgsKnown(ptypes[argi], info->parms, known->args))
        return NULL;
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, ptypes[argi], NULL, 0, info->parms, known->args);
    INode *copy = cloneNode(&cstate, ptypes[argi]);
    clonePopState();
    cloneDclPop(dclpos);
    inodeTypeCheckAny(pstate, &copy);
    if (inodeIsError(copy) || !isTypeNode(copy))
        return NULL;
    return copy;
}

// What argument 'argi' of a call of generic function or method 'generic' is
// wanted as, from the other arguments (checked) and 'expectType', the type
// wanted of the call's value; 'firstparm' is 1 for a method called on a receiver.
INode *genericFnArgExpect(TypeCheckState *pstate, FnDclNode *generic, Nodes *args, uint32_t argi,
        uint32_t firstparm, INode *expectType) {
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    if (gsig == NULL || gsig->tag != FnSigTag || firstparm + args->used > gsig->parms->used)
        return NULL;
    INode *ptypes[16];
    if (args->used > 16)
        return NULL;
    for (uint32_t j = 0; j < args->used; ++j)
        ptypes[j] = ((VarDclNode*)nodesGet(gsig->parms, firstparm + j))->vtype;
    return genericArgExpect(pstate, generic->genericinfo, generic->where, gsig->rettype, ptypes, args, argi, expectType);
}

// The same for argument 'argi' of a generic variant's (or struct's) literal,
// 'Some(None)': the field it fills is the parameter, the enum it makes the result.
INode *genericStructArgExpect(TypeCheckState *pstate, StructNode *generic, Nodes *args, uint32_t argi,
        INode *expectType) {
    GenericInfo *info = generic->genericinfo;
    INode *result = info ? genericResultTemplate(generic, info) : NULL;
    INode *ptypes[16];
    if (info == NULL || result == NULL || args->used > 16)
        return NULL;
    for (uint32_t j = 0; j < args->used; ++j) {
        FieldDclNode *field = fnCallTypeLitField(generic, args, j);
        ptypes[j] = field ? field->vtype : NULL;
    }
    return genericArgExpect(pstate, info, info->where, result, ptypes, args, argi, expectType);
}

// Does a generic variant have a type parameter that none of its fields names:
// 'None' of 'Option[T]'? Such a construction can only be typed from where its
// value goes.
int genericVariantParmUnnamed(StructNode *variant) {
    GenericInfo *info = variant->genericinfo;
    if (info == NULL || info->parms == NULL)
        return 0;
    Nodes *blank = newNodes(info->parms->used);
    for (uint32_t k = 0; k < info->parms->used; ++k)
        nodesAdd(&blank, (INode*)unknownType);
    for (uint32_t k = 0; k < info->parms->used; ++k) {
        if (((GenVarDclNode*)nodesGet(info->parms, k))->valtype)
            continue;
        nodesGet(blank, k) = NULL;
        int named = 0;
        INode **fldp;
        uint32_t cnt;
        for (nodelistFor(&variant->fields, cnt, fldp)) {
            FieldDclNode *field = (FieldDclNode*)*fldp;
            if (!(field->flags & (IsTagField | IsMixin)) && field->vtype
                && !genericTypeArgsKnown(field->vtype, info->parms, blank))
                named = 1;
        }
        nodesGet(blank, k) = unknownType;
        if (!named)
            return 1;
    }
    return 0;
}

// Can a call passing 'self' (NULL if none) and 'args', all of them checked, call
// generic function or method 'generic', a candidate of an overload set? Only a
// generic whose every type parameter is bound to a function signature joins a
// set (parseGenericOverloadVet), and a parameter of its signature is either one
// of those type parameters ('f F', 'f &F', 'f &mut F') or names none. The first
// kind is viable when its argument has a type to give the parameter, read off
// as an inference reads it, and the signature bound is met by it, as the
// generic's own instance would require. The second is judged as a plain
// function's parameter is, by a copy of its written type, checked. Nothing is
// inserted into the call and no instance is made: that waits for the one
// candidate the call selects.
int genericOverloadViable(FnDclNode *generic, INode **self, Nodes *args, uint32_t firstparm) {
    GenericInfo *info = generic->genericinfo;
    FnSigNode *gsig = (FnSigNode*)itypeGetTypeDcl(generic->vtype);
    uint32_t nargs = args ? args->used : 0;
    uint32_t first = self ? 0 : firstparm;
    uint32_t end = first + nargs + (self ? 1 : 0);
    if (end > gsig->parms->used)
        return 0;

    // What the arguments say of each type parameter, and a list that says
    // nothing, to ask which parameters a type names
    FnCallNode *known = newFnCallNode((INode*)newNameUseNode(anonName), info->parms->used);
    Nodes *blank = newNodes(info->parms->used);
    for (uint32_t j = 0; j < info->parms->used; ++j) {
        nodesAdd(&known->args, (INode*)NULL);
        nodesAdd(&blank, (INode*)NULL);
    }

    // The type parameters first: each is the type of the argument it takes.
    // A closure literal not yet checked has no type: the parameter it fills is
    // left for its signature to settle, which the bound's clause leaves unknown
    uint32_t j;
    INode *literalfor[8];
    uint32_t nliteral = 0;
    for (j = first; j < end; ++j) {
        INode *ptype = ((VarDclNode*)nodesGet(gsig->parms, j))->vtype;
        if (genericTypeArgsKnown(ptype, info->parms, blank))
            continue;
        INode *bare = ptype;
        while (bare && (bare->tag == RefTag || bare->tag == BorrowTag))
            bare = ((RefNode*)bare)->vtexp;
        if (!nameUseNames(bare, GenVarDclTag))
            return 0;
        INode *arg = self && j == 0 ? *self : nodesGet(args, j - first - (self ? 1 : 0));
        if (arg->tag == ClosureTag) {
            if (nliteral < 8)
                literalfor[nliteral++] = nameUseGetDcl((NameUseNode*)bare);
            continue;
        }
        if (!isExpNode(arg) || inodeIsError(arg))
            return 0;
        INode *argtype = ((IExpNode*)arg)->vtype;
        if (argtype == NULL || argtype == unknownType || !genericInferType(known, info->parms, ptype, argtype))
            return 0;
    }
    for (j = 0; j < known->args->used; ++j) {
        if (nodesGet(known->args, j) != NULL)
            continue;
        int literal = 0;
        for (uint32_t i = 0; i < nliteral; ++i)
            if (literalfor[i] == nodesGet(info->parms, j))
                literal = 1;
        if (!literal)
            return 0;
    }
    if (genericUnmetCondition((INode*)generic, generic->where, info->parms, known->args) != NULL)
        return 0;

    // The other parameters are a plain function's
    INode *owner = inodeGetOwner((INode*)generic);
    TypeCheckState tstate;
    tstate.typenode = owner && owner->tag == StructTag ? owner : NULL;
    tstate.fn = NULL;
    tstate.scope = 0;
    tstate.extend = NULL;
    for (j = first; j < end; ++j) {
        INode *ptype = ((VarDclNode*)nodesGet(gsig->parms, j))->vtype;
        if (!genericTypeArgsKnown(ptype, info->parms, blank))
            continue;
        INode **argp = self && j == 0 ? self : &nodesGet(args, j - first - (self ? 1 : 0));
        if ((*argp)->tag == ClosureTag)
            continue;
        // A virtual reference receiver is judged as a plain method's is, by
        // fnSigViableCall, and a generic has no such receiver
        if (j == 0 && self && iexpGetTypeDcl(*self)->tag == VirtRefTag)
            return 0;
        CloneState cstate;
        uint32_t dclpos = cloneDclPush();
        clonePushState(&cstate, ptype, NULL, 0, info->parms, known->args);
        INode *copy = cloneNode(&cstate, ptype);
        clonePopState();
        cloneDclPop(dclpos);
        inodeTypeCheckAny(&tstate, &copy);
        if (inodeIsError(copy) || iexpMatches(argp, copy, Coercion) == NoMatch)
            return 0;
    }

    // Every parameter the call did not supply must declare a default value
    for (j = end; j < gsig->parms->used; ++j)
        if (((VarDclNode*)nodesGet(gsig->parms, j))->value == NULL)
            return 0;
    return 1;
}

// Is 'fn' an instance of generic function or method 'generic'?
int genericIsInstanceOf(INode *fn, FnDclNode *generic) {
    Nodes *memonodes = generic->genericinfo->memonodes;
    if (fn == NULL || memonodes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(memonodes, cnt, nodesp)) {
        ++nodesp; --cnt;  // memonodes holds pairs: the call, then what it instantiated
        if (*nodesp == fn)
            return 1;
    }
    return 0;
}
