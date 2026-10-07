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
        // Found parameter with corresponding name? Capture/check type
        if (genvarname == ((NameUseNode*)parmtype)->namesym) {
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

// Infer generic type parameters (inferredgencall) from the type literal arguments (srcgencallp)
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
            && genericCaptureType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic type");
            retcode = 0;
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
// a function signature, 'f &fn(a A) R', matches the referenced function's,
// each parameter type and then the return type, so '&triangle' infers both;
// and 'List[T]' matches an instance of List, type argument by type argument.
// A managed reference type, 'Rc[mut, T]', is a call until type check lowers
// it, and matches a reference, its last argument against what the reference
// points at. In a template a type parameter is not yet
// a type, so '*T' is held as a dereference and '&T' or '&[]T' as a borrow
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
        else
            return 1;
        return genericInferType(inferredgencall, genparms, ((RefNode *)parmtype)->vtexp, elemtype);
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
                if (genericInferType(inferredgencall, genparms, *parmargp, *instargp++) == 0)
                    return 0;
            }
            return 1;
        }
        // A variant is passed where its enum is expected: 'Some[7]' is an Option
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
// on a receiver, whose 'self' is not among the arguments yet, else 0.
static void genericInferFromBounds(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall);

// An unsuffixed integer literal passed for a parameter that is a bare type
// parameter, 'v T': it is whichever number type is wanted, and only defaults to
// i32 if nothing else says (litAdoptNumberType)
static int genericArgIsAdaptable(INode *arg, INode *parmtype) {
    return arg->tag == ULitTag && (arg->flags & FlagUnkType) && nameUseNames(parmtype, GenVarDclTag);
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

static int genericInferFnParms(TypeCheckState *pstate, Nodes *genparms, FnSigNode *genfnsig,
        Nodes *args, uint32_t firstparm, INode *errnode, FnCallNode *inferredgencall, Nodes *where) {

    if (args == NULL)
        return 1;
    if (args->used + firstparm > genfnsig->parms->used) {
        errorMsgNode(errnode, ErrorManyArgs, "Too many arguments provided for generic function.");
        return 0;
    }

    // Iterate through arguments and expected parms
    int retcode = 1;
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(genfnsig->parms, firstparm);
    for (nodesFor(args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((IExpNode *)*argsp)->vtype;
        // Capture the type of each generic variable the parameter's type names.
        // A 'null' is whichever pointer type is wanted, so says nothing of
        // which; another argument may, and the call's coercion then types it.
        // So does an unsuffixed integer literal for a bare type parameter,
        // which is taken up below, once the other arguments and the bounds
        // have had their say.
        if (!litIsUntypedNull(*argsp) && !genericArgIsAdaptable(*argsp, parmtype)
            && genericInferType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic function");
            retcode = 0;
        }
        ++parmp;
    }

    // A type parameter no argument names, only the bound of one that is named,
    // is read off that argument's methods, before a literal's default is
    // taken for it
    genericInferFromBounds(genparms, where, inferredgencall);

    // What is left to a literal: it names its parameter's type, i32, only if
    // nothing did. Otherwise it is converted to that type with the call's
    // other arguments, as an integer literal is wherever a number type is wanted.
    parmp = &nodesGet(genfnsig->parms, firstparm);
    for (nodesFor(args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        if (genericArgIsAdaptable(*argsp, parmtype)) {
            INode *given = genericCapturedType(inferredgencall, genparms, parmtype);
            if (given == NULL)
                genericInferType(inferredgencall, genparms, parmtype, ((IExpNode *)*argsp)->vtype);
            else if (!itypeIsSame(given, ((IExpNode *)*argsp)->vtype) && iexpMatches(argsp, given, Coercion) == NoMatch) {
                errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic function");
                retcode = 0;
            }
        }
        ++parmp;
    }
    return retcode;
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
    uint32_t wanted = trait->genericinfo->parms ? trait->genericinfo->parms->used : 0;
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
static FnDclNode *genericMatchMethod(INode *binding, FnDclNode *traitmeth) {
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
    uint32_t wanted = ((FnSigNode*)itypeGetTypeDcl(traitmeth->vtype))->parms->used;
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
static void genericInferFromBounds(Nodes *genparms, Nodes *where, FnCallNode *inferredgencall) {
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
        if (trait == NULL || !isNameUseNode(clause->exp))
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
        FnCallNode *boundcall = (FnCallNode*)clause->typ;
        Nodes *tparms = trait->genericinfo->parms;
        if (sdcl->tag != StructTag || tparms == NULL || boundcall->args == NULL
            || tparms->used != boundcall->args->used)
            continue;
        // What each of the trait's own parameters is, as the argument's methods say
        FnCallNode *traitcall = newFnCallNode(boundcall->objfn, tparms->used);
        inodeLexCopy((INode*)traitcall, (INode*)boundcall);
        for (uint32_t j = 0; j < tparms->used; ++j)
            nodesAdd(&traitcall->args, (INode*)NULL);
        INode **nodesp;
        uint32_t methcnt;
        for (nodelistFor(&trait->nodelist, methcnt, nodesp)) {
            if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
                continue;
            FnDclNode *traitmeth = (FnDclNode*)*nodesp;
            FnDclNode *meth = genericMatchMethod(namespaceFind(&((StructNode*)sdcl)->namespace, traitmeth->namesym), traitmeth);
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
        for (uint32_t j = 0; j < tparms->used; ++j) {
            if (nodesGet(traitcall->args, j))
                genericInferType(inferredgencall, genparms, nodesGet(boundcall->args, j), nodesGet(traitcall->args, j));
        }
    }
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

// Append the name a clause's subject 'is' to 'buf': a trait's or a type's
static void genericTypeNameCat(char *buf, size_t size, INode *type, int depth);
static void genericTemplateNameCat(char *buf, size_t size, INode *node, int depth);
static void genericClauseNameCat(char *buf, size_t size, CastNode *clause, StructNode *inst) {
    StructNode *trait = genericNamedTrait(clause->typ);
    if (trait) {
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
    else if (subject->dclnode->tag != GenVarDclTag) {
        errorMsgNode(clause->exp, ErrorWhereSubject,
            "A 'where' clause constrains a type parameter of this generic, or of the generic type it is a member of, and %s is not one.",
            &subject->namesym->namestr);
        ok = 0;
    }
    // A generic trait's instance, 'Stack[T]', is a constraint's to name; in a
    // condition on an 'is' entry it is not built
    if (ownparms == NULL && genericNamedGenericTrait(clause->typ)) {
        if (!genericTraitInstanceVet(clause->typ))
            ok = 0;
    }
    else if (genericNamedTrait(clause->typ) == NULL && genericNamedType(clause->typ) == NULL) {
        StructNode *bare = ownparms ? NULL : genericNamedBareGenericTrait(clause->typ);
        if (bare)
            errorMsgNode(clause->typ, ErrorWhereTrait,
                "%s is a generic trait, and a constraint names an instance of it, with its type arguments: %s[T]. (A bare generic trait names no trait.)",
                &bare->namesym->namestr, &bare->namesym->namestr);
        else if (!isNameUseNode(clause->typ) || ((NameUseNode*)clause->typ)->dclnode != NULL)
            errorMsgNode(clause->typ, ErrorWhereTrait, ownparms
                ? "What a type parameter 'is' in a condition on an 'is' entry is a trait or a type, and this is neither. (A generic trait's or type's instance is not built in a condition yet.)"
                : "What a type parameter 'is' in a 'where' clause is a trait or a type, and this is neither. (A generic type's instance is not built as a constraint yet.)");
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
    // what 'where T is A and T is B' says. The slot is one for everything a
    // parameter may be annotated with, and what the name resolves to is what
    // it means: a trait constrains a type parameter; a type would make a value
    // parameter and a kind another kind of parameter, and neither is built.
    if (parms) {
        for (nodesFor(parms, cnt, nodesp)) {
            GenVarDclNode *parm = (GenVarDclNode*)*nodesp;
            if (parm->annot == NULL)
                continue;
            INode **annotp;
            uint32_t annotcnt;
            for (nodesFor(parm->annot, annotcnt, annotp)) {
                if (genericNamedGenericTrait(*annotp)) {
                    if (!genericTraitInstanceVet(*annotp))
                        continue;
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
                            "What follows the type parameter %s constrains it, so it names a trait, and this is not one. A value parameter, typed, and a parameter of another kind are not built yet.",
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
    // refused, dropped whole: an 'or' missing one side would say something else
    if (*wherep) {
        for (nodesFor(*wherep, cnt, nodesp)) {
            if (genericConditionNameRes(pstate, *nodesp, NULL))
                genericAddCondition(&clauses, *nodesp);
        }
    }
    *wherep = clauses;
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
        && (dcl->tag == IntNbrTag || (dcl->tag == UintNbrTag && dcl != (INode*)boolType)))
        return 1;
    if (trait == pointerTrait && dcl->tag == PtrTag)
        return 1;
    // core's Hash is granted to the integers and bool, which feed their bits
    // (nbrAddHashMethods). Not to a float: NaN is not equal to itself and -0 is
    // equal to 0, so no hash of a float's bits agrees with its '=='
    if ((dcl->tag == IntNbrTag || dcl->tag == UintNbrTag) && coreIsHashTrait((INode*)trait))
        return 1;
    // Sendable is the thread check's: granted to every type holding nothing
    // bound to its thread, and to a type declaring it, on its word, where its
    // type arguments are Sendable. A borrow of the whole program is let cross
    // only where the generic bounds its parameter by ''static', which each call
    // is checked to meet
    if (trait == sendableTrait)
        return !itypeThreadBoundHow(dcl, NULL, how);
    // Sized and DynSized are the type's size: known at compile time, or known
    // at compile time or carried by a reference to it. A type cannot declare
    // either.
    if (trait == sizedTrait)
        return itypeIsSized(dcl);
    if (trait == dynSizedTrait)
        return itypeIsDynSized(dcl);
    if (dcl->tag != StructTag)
        return 0;
    StructNode *strnode = (StructNode*)dcl;
    if (strnode == trait || genericDeclares(strnode, trait, 0))
        return 1;
    if (genericTraitIsMarker(trait))
        return 0;
    // Fitting it: 'structMatches' is the structural subtype test a trait's
    // uses are made by, under the constraint monomorphization asks for
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
// condition turning on a parameter not bound there.
typedef enum {
    WhereFalse,
    WhereTrue,
    WhereUnknown
} WhereValue;

// The use that asked for the instance a condition is being decided for, and the
// walk it was asked in, which an instance of a clause's generic trait is made
// under (genericClauseTrait). Both are NULL when the condition is decided
// where no use is being checked, to explain a refusal.
static INode *genericCondSite = NULL;
static TypeCheckState *genericCondState = NULL;

// The trait a clause says its subject is, at these arguments: the trait it
// names, or, where it names an instance of a generic trait, 'Stack[T]', that
// instance made by giving the trait the arguments its own type arguments come
// to -- 'Stack[i64]' where T is i64. Made by cloning the clause's trait with
// the parameters substituted and checking it, as the same spelling in a
// signature is, so the instance is the one the program's other uses of it name.
// NULL if the clause names no trait, or the instance could not be made, which
// was reported where it failed.
static StructNode *genericClauseTrait(CastNode *clause, Nodes *parms, Nodes *args) {
    StructNode *trait = genericNamedTrait(clause->typ);
    if (trait || genericNamedGenericTrait(clause->typ) == NULL)
        return trait;
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
    inodeTypeCheckAny(tstate, &copy);
    if (inodeIsError(copy) || !isTypeNode(copy))
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

// Evaluate a condition at these arguments, 'or' and 'and' as in an expression,
// the right side only where the left does not decide it. A lone clause is
// decided only where its subject is one of 'parms': a clause over a parameter
// bound already was decided then. Inside an 'or' or an 'and', a subject the
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
        return lval == WhereUnknown || rval == WhereUnknown ? WhereUnknown : rval;
    }
    GenVarDclNode *parm;
    INode *type = genericClauseType((CastNode*)cond, parms, args, &parm);
    StructNode *trait = genericNamedTrait(((CastNode*)cond)->typ);
    // An instance of a generic trait is made at these arguments, once the
    // clause is known to be decided here
    if (trait == NULL && genericNamedGenericTrait(((CastNode*)cond)->typ)) {
        if (type == NULL || (parm == NULL && !nested))
            return WhereUnknown;
        trait = genericClauseTrait((CastNode*)cond, parms, args);
        if (trait == NULL)
            return WhereUnknown;
    }
    INode *named = trait ? NULL : genericNamedType(((CastNode*)cond)->typ);
    if (type == NULL || (trait == NULL && named == NULL) || (parm == NULL && !nested))
        return WhereUnknown;
    if (trait)
        return genericTypeIsHow(type, trait, genericStaticHow(trait, parm)) ? WhereTrue : WhereFalse;
    // A type is met by itself alone
    return itypeIsSame(type, named) ? WhereTrue : WhereFalse;
}

// The first condition of 'where' these arguments make false, or NULL if none
// does. One left unknown is decided where the parameter it turns on is bound.
static INode *genericUnmetCondition(Nodes *where, Nodes *parms, Nodes *args) {
    if (where == NULL || parms == NULL || args == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(where, cnt, nodesp)) {
        if (genericConditionValue(*nodesp, parms, args, 0) == WhereFalse)
            return *nodesp;
    }
    return NULL;
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
    snprintf(buf + used, size - used, "%s", itypeName(dcl));
    Nodes *args = dcl->tag == StructTag && depth < 4 ? itypeInstanceTypeArgs(dcl) : NULL;
    if (args == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(args, cnt, nodesp)) {
        used = strlen(buf);
        snprintf(buf + used, size - used, cnt == args->used ? "[" : ", ");
        genericTypeNameCat(buf, size, *nodesp, depth + 1);
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

// Append a condition to 'buf' as it reads: 'T is A or T is B', an 'or' inside
// an 'and' in parentheses ('grouped'), a substituted subject by its type's name
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
// through a local -- a borrow or a permission -- whose own 'mut' is not what is
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
                        ". A borrow that lives for the whole program does cross where the generic bounds that type parameter 'Sendable + 'static'");
            }
            break;
        }
        case RefBindsTraced:
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
// local's own 'mut' is not what is checked, so the message says so.
static void genericNotSendableMsg(INode *errnode, Name *name, GenVarDclNode *parm, INode *arg, StaticBorrow how) {
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    char what[512], reason[512];
    int local = genericNotSendableWhy(arg, what, reason, how);
    errorMsgNode(errnode, ErrorNotSendable, "%s requires %s is Sendable, and %s is not Sendable: %s %s.%s",
        &name->namestr, &parm->namesym->namestr, argname, what, reason,
        local ? " What is checked is the types of the references a value holds, not how a variable was declared: a local declared 'mut x = 5' holds a number, which is Sendable." : "");
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
        int holds = genericConditionValue(entry->value, generic->genericinfo->parms, args, 0) == WhereTrue;
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

// Append a method's signature as it is written in a trait: 'push(self &mut, v i64)',
// 'pop(self &mut) Option[i64]'
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
        // A receiver is written by its permission alone: 'self &mut'
        INode *region = dcl && dcl->tag == RefTag && ((RefNode*)dcl)->region && isTypeNode(((RefNode*)dcl)->region)
            ? itypeGetTypeDcl(((RefNode*)dcl)->region) : NULL;
        if (first && (fn->flags & FlagMethFld) && region == borrowRef) {
            INode *perm = ((RefNode*)dcl)->perm && isTypeNode(((RefNode*)dcl)->perm)
                ? itypeGetTypeDcl(((RefNode*)dcl)->perm) : NULL;
            Name *permname = perm ? inodeGetName(perm) : NULL;
            used = strlen(buf);
            if (permname == NULL || perm == (INode*)roPerm)
                snprintf(buf + used, size - used, " &");
            else
                snprintf(buf + used, size - used, " &%s", &permname->namestr);
        }
        else if (parm->vtype) {
            used = strlen(buf);
            snprintf(buf + used, size - used, " ");
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

// Why a type does not fit a generic trait's instance structurally, into 'buf'
// (empty if nothing is found to say): the first method the trait requires that
// the type lacks, or has only with another signature, each spelled as the
// instance's types come to, so 'pop(self &mut) Option[i64]' where the trait
// is 'Stack[i64]'
static void genericFitWhy(char *buf, size_t size, StructNode *trait, INode *type) {
    buf[0] = '\0';
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
        if (binding == NULL || (binding->tag != FnDclTag && binding->tag != FnOverloadDclTag)) {
            snprintf(buf, size, " %s has no method %s, which %s requires.", typename,
                &meth->namesym->namestr, traitname);
            return;
        }
        char found[256] = "";
        if (binding->tag == FnDclTag)
            genericSigCat(found, sizeof(found), (FnDclNode*)binding);
        else {
            Nodes *overloads = ((FnOverloadDclNode*)binding)->overloads;
            for (uint32_t i = 0; i < overloads->used; ++i) {
                size_t used = strlen(found);
                if (i)
                    snprintf(found + used, sizeof(found) - used, " and ");
                genericSigCat(found, sizeof(found), (FnDclNode*)nodesGet(overloads, i));
            }
        }
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
    INode *cond = genericUnmetCondition(where, parms, srcgencall->args);
    if (cond == NULL) {
        genericSendableNote(srcgencall, where, parms, name);
        genericWhereOwner = NULL;
        return 1;
    }
    genericWhereOwner = NULL;
    // A condition joined by 'or' or 'and' is false as a whole, and named whole
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
    StructNode *trait = genericNamedTrait(((CastNode*)cond)->typ);
    if (trait == sendableTrait) {
        genericWhereOwner = generic;
        StaticBorrow how = genericStaticHow(trait, parm);
        genericWhereOwner = NULL;
        genericNotSendableMsg((INode*)srcgencall, name, parm, arg, how);
        return 0;
    }
    // An instance of a generic trait is the one made at these arguments, and the
    // message says which method of it the argument lacks
    StructNode *inst = trait ? NULL : genericClauseTrait((CastNode*)cond, parms, srcgencall->args);
    char argname[256] = "";
    genericTypeNameCat(argname, sizeof(argname), arg, 0);
    char isname[256] = "";
    genericClauseNameCat(isname, sizeof(isname), (CastNode*)cond, inst);
    char why[640] = "";
    if (trait)
        genericCondIsWhy(why, sizeof(why), arg, trait);
    else if (inst && !genericTraitIsMarker(inst))
        genericFitWhy(why, sizeof(why), inst, arg);
    if (inst == NULL)
        inst = trait;
    int usermarker = why[0] == '\0' && inst && genericTraitIsMarker(inst) && !corelibIsBuiltinTrait((INode*)inst);
    errorMsgNode((INode*)srcgencall, ErrorWhereUnmet,
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
    int met = genericRequirementsMetIn(srcgencall, generic, genericinfo, name);
    genericCondSite = svsite;
    genericCondState = svstate;
    return met;
}

// The members of generic type 'generic' that do not exist at 'args': each
// whose 'where' clause, over the type's parameters, they do not meet; and each
// entry of its 'is' list whose condition they do not make true, which the
// instance lacks (cloneStructNode)
static Nodes *genericAbsentMembersIn(StructNode *generic, Nodes *args) {
    Nodes *absent = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&generic->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || ((FnDclNode*)*nodesp)->where == NULL)
            continue;
        if (genericUnmetCondition(((FnDclNode*)*nodesp)->where, generic->genericinfo->parms, args) == NULL)
            continue;
        if (absent == NULL)
            absent = newNodes(4);
        nodesAdd(&absent, *nodesp);
    }
    if (generic->condis) {
        for (nodesFor(generic->condis, cnt, nodesp)) {
            INode *cond = ((FieldDclNode*)*nodesp)->value;
            if (genericConditionValue(cond, generic->genericinfo->parms, args, 0) == WhereTrue)
                continue;
            if (absent == NULL)
                absent = newNodes(4);
            nodesAdd(&absent, *nodesp);
        }
    }
    return absent;
}

// The same, decided under the use asking for the instance, as the requirements are
static Nodes *genericAbsentMembers(TypeCheckState *pstate, FnCallNode *srcgencall, StructNode *generic, Nodes *args) {
    INode *svsite = genericCondSite;
    TypeCheckState *svstate = genericCondState;
    genericCondSite = (INode*)srcgencall;
    genericCondState = pstate;
    Nodes *absent = genericAbsentMembersIn(generic, args);
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
        INode *cond = genericUnmetCondition(fn->where, generic->genericinfo->parms, args);
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

// Clone the generic's instance for parms and remember it, without type checking it.
// 'shell' is the instance reserved for a generic type (genericReserve), else NULL.
// 'absent' lists a generic type's members whose 'where' clause the arguments do
// not meet (genericAbsentMembers), which its instance is cloned without.
static INode *genericClone(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone,
        GenericInfo *genericinfo, INode *shell, Nodes *absent) {
    CloneState cstate;
    clonePushState(&cstate, (INode*)srcgencall, NULL, pstate->scope, genericinfo->parms, srcgencall->args);
    cstate.structshell = shell;
    cstate.absent = absent;
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

    // Remember instantiation for the future
    if (!genericinfo->memonodes)
        genericinfo->memonodes = newNodes(2);
    nodesAdd(&genericinfo->memonodes, (INode*)srcgencall);
    nodesAdd(&genericinfo->memonodes, instance);
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
    Nodes *absent = nodetoclone->tag == StructTag
        ? genericAbsentMembers(pstate, srcgencall, (StructNode*)nodetoclone, srcgencall->args) : NULL;

    // A generic function's own name is not mapped: written bare in its body, it
    // is a call whose type arguments are inferred, as it is anywhere else
    uint32_t dclpos = cloneDclPush();
    INode *shell = nodetoclone->tag == StructTag ? genericReserve(nodetoclone) : NULL;
    INode *instance = genericClone(pstate, srcgencall, nodetoclone, genericinfo, shell, absent);
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
static int genericMembersConditioned(StructNode *type) {
    if (type->condis)
        return 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&type->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag && ((FnDclNode*)*nodesp)->where != NULL)
            return 1;
    }
    return 0;
}

static int genericConditioned(INode *generic, GenericInfo *genericinfo) {
    if (generic->tag == FnDclTag)
        return genericWhereReadsLayout(((FnDclNode*)generic)->where);
    if (generic->tag != StructTag)
        return 0;
    StructNode *type = (StructNode*)generic;
    if (genericWhereReadsLayout(genericinfo->where) || genericMembersConditioned(type))
        return 1;
    StructNode *owner = (type->flags & HasTagField) ? structGetBaseTrait(type) : NULL;
    if (owner == NULL)
        return 0;
    if ((owner->genericinfo && genericWhereReadsLayout(owner->genericinfo->where))
        || genericMembersConditioned(owner))
        return 1;
    if (owner->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(owner->derived, cnt, nodesp)) {
            if (genericMembersConditioned((StructNode*)*nodesp))
                return 1;
        }
    }
    return 0;
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
    for (nodesFor(srcgencall->args, cnt, nodesp)) {
        if (!isTypeNode(*nodesp)) {
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
            if (!itypeIsSame(*priornodesp, *nownodesp)
                || (erasednodesp && !lifeBrandsEqual(*priornodesp, *erasednodesp))) {
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
    if (structTargetDeferring() && genericConditioned(nodetoclone, genericinfo)) {
        uint32_t target = structTargetSuspend();
        for (nodesFor(written, cnt, nodesp))
            structTypeSettle(pstate, *nodesp);
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
        Nodes *traitabsent = genericAbsentMembers(pstate, srcgencall, basetrait, srcgencall->args);
        Nodes *absents = newNodes(basetrait->derived->used);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&absents, (INode*)genericAbsentMembers(pstate, srcgencall, (StructNode*)*nodesp, srcgencall->args));
        uint32_t dclpos = cloneDclPush();
        INode *traitshell = genericReserve((INode*)basetrait);
        Nodes *shells = newNodes(basetrait->derived->used);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&shells, genericReserve(*nodesp));
        INode *instrait = genericClone(pstate, srcgencall, (INode*)basetrait, basetrait->genericinfo, traitshell, traitabsent);
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
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&variants, genericClone(pstate, srcgencall, *nodesp, ((StructNode*)*nodesp)->genericinfo,
                *shellp++, (Nodes*)*absentp++));
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
int genericSubstitute(TypeCheckState *pstate, FnCallNode **srcgencallp) {
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
            (FnSigNode*)itypeGetTypeDcl(((FnDclNode *)nodetoclone)->vtype), srcgencall->args, firstparm,
            (INode*)srcgencall, inferredgencall, ((FnDclNode *)nodetoclone)->where) == 0)
            return 1;
        break;
    }
    case StructTag:
        // Infer based on type constructor arguments
        if (genericInferStructParms(pstate, genericinfo->parms, (StructNode*)itypeGetTypeDcl(nodetoclone), srcgencall, inferredgencall) == 0)
            return 1;
        genericInferFromBounds(genericinfo->parms, genericinfo->where, inferredgencall);
        break;
    default:
        // genericGetInfo answers for a function, a struct and a module, it was
        // asked about this very node, and a module took the explicit path above
        errorUnreachable((INode*)srcgencall, "a generic that is neither a function nor a struct");
        return 1;
    }

    // Be sure all expected generic parameters were inferred
    for (nodesFor(inferredgencall->args, cnt, argsp)) {
        if (*argsp == NULL) {
            errorMsgNode((INode*)srcgencall, ErrorInvType, "Could not infer all of generic's type parameters.");
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
// arguments are inferred from the call's arguments, which do not yet hold the
// receiver, so they match the method's parameters after 'self'. Returns NULL
// once an error is reported.
FnDclNode *genericMethodInstance(TypeCheckState *pstate, FnCallNode *callnode, FnDclNode *genmeth, Nodes *typeargs) {
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
            callnode->args, 1, (INode*)callnode, gencall, genmeth->where) == 0)
            return NULL;
        for (nodesFor(gencall->args, cnt, argsp)) {
            if (*argsp == NULL) {
                errorMsgNode((INode*)callnode, ErrorInvType, "Could not infer all of generic's type parameters.");
                return NULL;
            }
        }
    }
    INode *instance = genericMemoize(pstate, gencall, (INode*)genmeth, genericinfo, genmeth->namesym);
    if (inodeIsError(instance))
        return NULL;
    return (FnDclNode*)nameUseGetDcl((NameUseNode*)instance);
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
