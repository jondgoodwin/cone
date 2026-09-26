/** Handling for gennode declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new generic info block
GenericInfo *newGenericInfo() {
    GenericInfo *geninfo = (GenericInfo*)memAllocBlk(sizeof(GenericInfo));
    geninfo->parms = NULL;
    geninfo->memonodes = NULL;
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
// Capture it, and return 0 if it does not match what we already thought it was
int genericCaptureType(FnCallNode *gencall, Nodes *genparms, INode *parmtype, INode *argtype) {
    INode **genvarp;
    uint32_t genvarcnt;
    INode **genargp = &nodesGet(gencall->args, 0);
    for (nodesFor(genparms, genvarcnt, genvarp)) {
        Name *genvarname = ((GenVarDclNode *)(*genvarp))->namesym;
        // Found parameter with corresponding name? Capture/check type
        if (genvarname == ((NameUseNode*)parmtype)->namesym) {
            if (*genargp == NULL)
                *genargp = argtype;
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
    if (typeLitStructReorder(srcgencall, genstruct, (INode*)genstruct == pstate->typenode) == 0)
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
        if (nameUseNames(parmtype, GenVarDclTag)
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
// and 'List[T]' matches an instance of List, type argument by type argument.
// In a template a type parameter is not yet
// a type, so '*T' is held as a dereference and '&T' or '&[]T' as a borrow
// (cloneStarNode, cloneRefNode), and both spellings are accepted here. Region
// and permission take no part: the instance's own check of the call judges
// them. Any other shape infers nothing, and returns 1 as a non-match does.
// Returns 0 only when a type parameter is given two different types.
static int genericInferType(FnCallNode *inferredgencall, Nodes *genparms, INode *parmtype, INode *argtype) {
    if (parmtype == NULL || argtype == NULL)
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
    case FnCallTag: {
        // An instance of a generic type, 'List[T]', matches an argument that is an
        // instance of the same generic, each type argument against the instance's
        FnCallNode *parmcall = (FnCallNode *)parmtype;
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
static int genericInferFnParms(TypeCheckState *pstate, Nodes *genparms, FnSigNode *genfnsig,
        Nodes *args, uint32_t firstparm, INode *errnode, FnCallNode *inferredgencall) {

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
        // Capture the type of each generic variable the parameter's type names
        if (genericInferType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic function");
            retcode = 0;
        }
        ++parmp;
    }
    return retcode;
}

// How deeply expansion is currently nested. See generic.h.
static uint32_t instantiateDepth = 0;

int genericInstantiateEnter(INode *errnode) {
    if (instantiateDepth >= TypeCheckLoopMax) {
        errorMsgNode(errnode, ErrorInstDepth,
            "Generic or macro expansion nested more than %d deep. It likely expands itself endlessly.",
            TypeCheckLoopMax);
        return 0;
    }
    ++instantiateDepth;
    return 1;
}

void genericInstantiateExit() {
    --instantiateDepth;
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
static INode *genericClone(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone,
        GenericInfo *genericinfo, INode *shell) {
    CloneState cstate;
    clonePushState(&cstate, (INode*)srcgencall, NULL, pstate->scope, genericinfo->parms, srcgencall->args);
    cstate.structshell = shell;
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

    // A generic function's own name is not mapped: written bare in its body, it
    // is a call whose type arguments are inferred, as it is anywhere else
    uint32_t dclpos = cloneDclPush();
    INode *shell = nodetoclone->tag == StructTag ? genericReserve(nodetoclone) : NULL;
    INode *instance = genericClone(pstate, srcgencall, nodetoclone, genericinfo, shell);
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
        inodeTypeCheckAny(&tstate, &instance);
    }
    else
        inodeTypeCheckAny(pstate, &instance);

    return instance;
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
        for (nodesFor(fncallprior->args, priorcnt, priornodesp)) {
            if (!itypeIsSame(*priornodesp, *nownodesp)) {
                match = 0;
                break;
            }
            nownodesp++;
        }
        if (match) {
            // Return a namenode pointing to dcl instance
            return newNameUseFromDclNode(*nodesp, (INode*)srcgencall);
        }
    }

    // No match found, instantiate the dcl generic.
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
        uint32_t dclpos = cloneDclPush();
        INode *traitshell = genericReserve((INode*)basetrait);
        Nodes *shells = newNodes(basetrait->derived->used);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&shells, genericReserve(*nodesp));
        INode *instrait = genericClone(pstate, srcgencall, (INode*)basetrait, basetrait->genericinfo, traitshell);
        if (basetrait == (StructNode*)nodetoclone)
            retinstance = instrait;

        // A variant's body may also name a static function, static or overload
        // name of the enum bare, which name resolution bound to the generic's
        // member; it is the instance's that has a symbol, so the variants are
        // cloned with each such member mapped to the instance's.
        structCloneMapMembers(basetrait, (StructNode*)instrait);
        Nodes *variants = newNodes(basetrait->derived->used);
        INode **shellp = &nodesGet(shells, 0);
        for (nodesFor(basetrait->derived, cnt, nodesp))
            nodesAdd(&variants, genericClone(pstate, srcgencall, *nodesp, ((StructNode*)*nodesp)->genericinfo, *shellp++));
        cloneDclPop(dclpos);
        // The instance's 'derived' lists its own variants, and lists all of them
        // before the enum or any variant is type checked: a variant's method body
        // may match a value of the enum, and its match is exhaustive only against
        // the whole set -- including a variant reached first from the enum's own
        // check, as a static function building it reaches it.
        Nodes **instraitderived = &((StructNode*)instrait)->derived;
        for (nodesFor(variants, cnt, nodesp))
            nodesAdd(instraitderived, *nodesp);
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
    genericInstantiateExit();

    return newNameUseFromDclNode(retinstance, (INode*)srcgencall);
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
            (INode*)srcgencall, inferredgencall) == 0)
            return 1;
        break;
    }
    case StructTag:
        // Infer based on type constructor arguments
        if (genericInferStructParms(pstate, genericinfo->parms, (StructNode*)itypeGetTypeDcl(nodetoclone), srcgencall, inferredgencall) == 0)
            return 1;
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
            callnode->args, 1, (INode*)callnode, gencall) == 0)
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
