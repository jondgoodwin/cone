/** Handling for expression nodes: Literals, Variables, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <memory.h>
#include <stdio.h>
#include <string.h>

// Create a new function signature node
FnSigNode *newFnSigNode() {
    FnSigNode *sig;
    newNode(sig, FnSigNode, FnSigTag);
    sig->flags |= OpaqueType;
    sig->parms = newNodes(8);
    sig->rettype = unknownType;
    sig->lifenamed = 0;
    sig->lifeorder = NULL;
    sig->lifechecked = 0;
    sig->lifestatic = 0;
    sig->spelled = NULL;
    return sig;
}

// Clone function signature
INode *cloneFnSigNode(CloneState *cstate, FnSigNode *node) {
    FnSigNode *newnode = memAllocBlk(sizeof(FnSigNode));
    memcpy(newnode, node, sizeof(FnSigNode));
    newnode->parms = cloneNodes(cstate, node->parms);
    newnode->rettype = cloneNode(cstate, node->rettype);
    INode **origp = &nodesGet(node->parms, 0);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(newnode->parms, cnt, nodesp)) {
        cloneDclSetMap(*origp++, *nodesp);
    }
    return (INode *)newnode;
}

// Serialize a function signature node
void fnSigPrint(FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("fn(");
    for (nodesFor(sig->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint(") ");
    inodePrintNode(sig->rettype);
}

// Name resolution of the function signature.
//
// A parameter's default value is evaluated where the function is called, so
// an importer's object evaluates it from the include file's text: what it names
// is marked and recorded as an expanded body's reach is, against the function
// (NameResState.sigfn), so a library exports it and its include file carries
// it. The parameter itself is resolved first, at scope 0, so nothing is hooked
// that the value could see
void fnSigNameRes(NameResState *pstate, FnSigNode *sig) {
    uint16_t svscope = pstate->scope;
    pstate->scope = 0; // Make scope 0 to avoid parameter names being hooked.
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode*)*nodesp;
        INode *dflt = parm->tag == VarDclTag ? parm->value : NULL;
        if (dflt == NULL || pstate->sigfn == NULL || pstate->expander != NULL) {
            inodeNameRes(pstate, nodesp);
            continue;
        }
        parm->value = NULL;
        inodeNameRes(pstate, nodesp);
        parm = (VarDclNode*)*nodesp;
        parm->value = dflt;
        pstate->expander = pstate->sigfn;
        inodeNameRes(pstate, &parm->value);
        pstate->expander = NULL;
    }
    inodeNameRes(pstate, &sig->rettype);
    pstate->scope = svscope;
}

// The lifetimes a signature's types name and imply (lifeSigCheck)
static void fnSigLifeCheck(TypeCheckState *pstate, INode *node, void *extra) {
    FnSigNode *sig = (FnSigNode *)node;
    INode **nodesp;
    uint32_t cnt;
    lifeSigCheck(sig);
    // A parameter's own reference a type parameter's ''static' bound makes
    // global holds a global borrow, as one written ''static' does: the caller
    // band varDclTypeCheck gave its own copy of the type is undone
    if (sig->lifestatic) {
        for (nodesFor(sig->parms, cnt, nodesp)) {
            INode *parmtype = ((VarDclNode*)*nodesp)->vtype;
            if ((parmtype->tag == RefTag || parmtype->tag == ArrayRefTag) && ((RefNode*)parmtype)->scope == 1
                && lifeIsOwnBorrow(parmtype) && lifePartStatic(sig, parmtype, LifePartOwn))
                ((RefNode*)parmtype)->scope = 0;
        }
    }
}

// Type check the function signature
void fnSigTypeCheck(TypeCheckState *pstate, FnSigNode *sig) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        // An init's 'self &new' is the one place that permission is written
        // (refTypeCheck); whether this is an init is fnDclTypeCheck's to judge
        refAllowNewPerm = cnt == sig->parms->used && ((VarDclNode*)*nodesp)->namesym == selfName
            && ((VarDclNode*)*nodesp)->vtype->tag == RefTag;
        inodeTypeCheckAny(pstate, nodesp);
        refAllowNewPerm = 0;
        // A caller's argument is checked to be global only where the
        // parameter's own reference is written ''static'
        if (sig->lifenamed && lifeParmStaticInside(((VarDclNode*)*nodesp)->vtype))
            errorMsgNode(*nodesp, ErrorLifetimePlace,
                "''static' is named on a parameter's own reference ('p &'static T') or in the result, not inside a parameter's type: what a caller passes there is not checked to be global.");
    }
    itypeTypeCheck(pstate, &sig->rettype);
    // A function reference's signature, checked as a reference's target while a
    // layout is in flight, has its lifetimes read once its types are laid out:
    // what a type's values hold is read from its layout
    if (structTargetDeferring())
        structDeferCheck(pstate, fnSigLifeCheck, (INode*)sig, NULL);
    else
        fnSigLifeCheck(pstate, (INode*)sig, NULL);
}

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigNode *node1, FnSigNode *node2) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // Return types and number of parameters must match
    if (!itypeIsSame(node1->rettype, node2->rettype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match. A parameter is a VarDclNode, not a
    // type, so its declared type has to be extracted before the types are compared.
    // Comparing the declarations themselves compares node identity, which no two
    // separately written signatures can ever satisfy.
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (!itypeIsSame(iexpGetTypeDcl(*nodes1p), iexpGetTypeDcl(*nodes2p)))
            return 0;
        nodes2p++;
    }
    return lifeSigsAgree(node1, node2);
}

// Is an implementation's type 'impl' the type a trait requirement's 'req' names?
// 'Self' in a requirement stands for the type meeting it, 'selftype', as it does
// in a default cloned into that type (cloneNode repoints it there). Only the
// name 'Self' is read that way, through a reference, a pointer or a slice to it:
// a requirement naming its trait outright means the trait, and so does a virtual
// reference to Self, '&<Self', which is a reference to the trait. With no
// 'selftype' the match is exact.
static int fnSigReqTypeSame(INode *req, INode *impl, INode *selftype) {
    if (itypeIsSame(req, impl))
        return 1;
    if (selftype == NULL)
        return 0;
    if (isNameUseNode(req) && ((NameUseNode*)req)->namesym == selfTypeName)
        return itypeGetTypeDcl(impl) == selftype;
    INode *reqdcl = itypeGetTypeDcl(req);
    INode *impldcl = itypeGetTypeDcl(impl);
    if (reqdcl->tag != impldcl->tag)
        return 0;
    switch (reqdcl->tag) {
    case RefTag:
    case ArrayRefTag: {
        RefNode *reqref = (RefNode*)reqdcl;
        RefNode *implref = (RefNode*)impldcl;
        return fnSigReqTypeSame(reqref->vtexp, implref->vtexp, selftype)
            && permIsSame(reqref->perm, implref->perm)
            && itypeIsSame(reqref->region, implref->region);
    }
    case PtrTag:
        return fnSigReqTypeSame(((StarNode*)reqdcl)->vtexp, ((StarNode*)impldcl)->vtexp, selftype);
    default:
        return 0;
    }
}

// For virtual reference structural matches on two methods,
// compare two function signatures to see if they are equivalent,
// ignoring the first 'self' parameter (we know their types differ).
// 'node1' is the implementation, 'node2' the requirement; see fnSigReqTypeSame
// for 'selftype'.
int fnSigVrefEqual(FnSigNode *node1, FnSigNode *node2, INode *selftype) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // Return types and number of parameters must match
    if (!fnSigReqTypeSame(node2->rettype, node1->rettype, selftype)
        || node1->parms->used != node2->parms->used)
        return 0;

    // Every parameter's type must also match, exactly. A parameter is a
    // VarDclNode, not a type, so its declared type has to be extracted before the
    // types are compared; comparing the declarations themselves compares node
    // identity, which a trait's requirement and a type's implementation of it can
    // never satisfy, since they are separately written.
    //
    // The match is exact rather than by coercion. Coercion is a convenience at a
    // call site, and belongs to type comparison only where subtyping is in play,
    // which it is not between a trait requirement and an implementation of it.
    // A virtual reference dispatches through a typed vtable slot, so the machine
    // signatures have to line up. Reading 'Self' as the implementer is for a
    // match no slot is filled by, a constraint or a declared 'is'.
    //
    // A parameter's declared type is read off it as written, not through
    // iexpGetTypeDcl, so that a use of 'Self' is still one.
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (cnt < node1->parms->used
            && !fnSigReqTypeSame(((IExpNode*)*nodes2p)->vtype, ((IExpNode*)*nodes1p)->vtype, selftype))
            return 0;
        nodes2p++;
    }
    // A call through the requirement is checked against its lifetimes, so the
    // implementation must promise at least as much (lifeSigMeets)
    return lifeSigMeets(node1, node2);
}

// Do two signatures declare the same parameter types (ignoring return type)?
// Two overload candidates that compare equal would accept exactly the same
// arguments, so the overload name could never choose between them.
int fnSigParmsEqual(FnSigNode *node1, FnSigNode *node2) {
    if (node1->parms->used != node2->parms->used)
        return 0;

    INode **nodes1p, **nodes2p;
    uint32_t cnt;
    nodes2p = &nodesGet(node2->parms, 0);
    for (nodesFor(node1->parms, cnt, nodes1p)) {
        if (!itypeIsSame(iexpGetTypeDcl(*nodes1p), iexpGetTypeDcl(*nodes2p)))
            return 0;
        nodes2p++;
    }
    return 1;
}

// Return TypeCompare indicating whether from type matches the function signature
TypeCompare fnSigMatches(FnSigNode *to, FnSigNode *from, SubtypeConstraint constraint) {
    TypeCompare result = EqMatch;

    // Number of parameters must match
    if (to->parms->used != from->parms->used)
        return NoMatch;

    // Every parameter's type must also match
    INode **tonodesp, **fromnodesp;
    uint32_t cnt;
    fromnodesp = &nodesGet(from->parms, 0);
    for (nodesFor(to->parms, cnt, tonodesp)) {
        // Match for parameters is contravariant, switching order of to/from
        switch (itypeMatches(iexpGetTypeDcl(*fromnodesp), iexpGetTypeDcl(*tonodesp), constraint)) {
        case NoMatch:
            return NoMatch;
        case CastSubtype:
            result = result == ConvSubtype ? ConvSubtype : CastSubtype;
            break;
        case ConvSubtype:
            result = ConvSubtype;
            break;
        default:
            break;
        }
        fromnodesp++;
    }

    // A call through 'to' is checked against its lifetimes, so 'from' must
    // promise the same
    if (!lifeSigsAgree(to, from))
        return NoMatch;

    // Return type is covariant
    switch (itypeMatches(to->rettype, from->rettype, constraint)) {
    case NoMatch:
        return NoMatch;
    case CastSubtype:
        result = result == ConvSubtype ? ConvSubtype : CastSubtype;
        break;
    case ConvSubtype:
        result = ConvSubtype;
        break;
    default:
        break;
    }
    return result;
}

// Return true if type of from-exp matches totype
int fnSigCoerce(FnSigNode *totype, INode **fromexp) {
    return itypeMatches((INode*)totype, iexpGetTypeDcl(*fromexp), Coercion) == EqMatch;
}


// Can a call passing 'self' (NULL if none) and 'args' call this signature?
// Only viability is decided: arity, required versus defaulted parameters, receiver
// compatibility, and whether every explicit argument may be passed using a permitted
// implicit coercion. Nothing is inserted into the call, and no candidate is preferred
// over another for being an exact rather than a coercible match.
int fnSigViableCall(FnSigNode *to, INode **self, Nodes *args) {
    uint32_t argcnt = args ? args->used : 0;
    if (self)
        ++argcnt;

    // Too many arguments is not a match
    if (argcnt > to->parms->used)
        return 0;

    INode **parmp = &nodesGet(to->parms, 0);

    // A receiver, when there is one, must be passable as the first parameter
    if (self) {
        INode *selftype = iexpGetTypeDcl(*self);
        if (selftype->tag != VirtRefTag) {
            if (iexpMatches(self, iexpGetTypeDcl(*parmp), Coercion) == NoMatch)
                return 0;
        }
        // A virtual reference receiver is not type checked here as a whole, since
        // '&<Trait' never coerces to the '&Trait' the method declares. The
        // candidate must expect a reference it can be dispatched through, with a
        // permission the receiver's grants, as a plain reference's must
        else {
            RefNode *parmref = (RefNode*)iexpGetTypeDcl(*parmp);
            if (parmref->tag != RefTag
                || permMatches(parmref->perm, ((RefNode*)selftype)->perm) == NoMatch)
                return 0;
        }
        ++parmp;
    }

    // Every explicit argument must be passable to its corresponding parameter
    if (args) {
        INode **argsp;
        uint32_t cnt;
        for (nodesFor(args, cnt, argsp)) {
            if (iexpMatches(argsp, ((IExpNode *)*parmp)->vtype, Coercion) == NoMatch)
                return 0;
            ++parmp;
        }
    }

    // Every parameter the call did not supply must declare a default value
    uint32_t missing = to->parms->used - argcnt;
    while (missing--) {
        if (((VarDclNode *)*parmp++)->value == NULL)
            return 0;
    }

    return 1;
}

// ---------------------------------------------------------------------------
// The signature as a trait: '&<fn(sig)', 'So[fn(sig)]'

// The callable traits made so far, one per signature and kind of 'self'
static Nodes *fnCallTraits = NULL;

// The trait standing for 'fn(sig)' behind a virtual reference: a trait with the
// one method '()' of that signature, which anything with a pub '()' of exactly
// those parameter and return types meets (a closure's hidden struct, a
// hand-written struct, a plain function through its stub). The reference's
// permission is the call's kind: a reference that may write ('&<mut', an owner)
// is to the trait whose '()' takes 'self &mut', any other to the one taking
// 'self &', so a state-changing '()' is not met behind a read-only reference.
// One trait per signature and kind, so two spellings of one signature are one type.
StructNode *fnSigCallTrait(TypeCheckState *pstate, FnSigNode *sig, int mutself, INode *lexnode) {
    if (fnCallTraits == NULL)
        fnCallTraits = newNodes(8);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnCallTraits, cnt, nodesp)) {
        StructNode *found = (StructNode*)*nodesp;
        if (found->callmut == mutself && fnSigEqual(found->callsig, sig))
            return found;
    }

    // Named by its signature's symbol spelling, so that two compiles of one
    // signature name one vtable; a long one by its hash
    char spell[2048];
    char *end = nameType(spell, (INode*)sig);
    *end = '\0';
    char name[320];
    if (end - spell < 240)
        snprintf(name, sizeof(name), "fn%c%s", mutself ? 'M' : 'R', spell);
    else {
        uint64_t hash = 14695981039346656037ull;
        for (char *p = spell; *p; ++p)
            hash = (hash ^ (uint8_t)*p) * 1099511628211ull;
        snprintf(name, sizeof(name), "fn%c#%016llx", mutself ? 'M' : 'R', (unsigned long long)hash);
    }
    StructNode *st = newStructNode(nametblFind(name, strlen(name)));
    inodeLexCopy((INode*)st, lexnode);
    st->flags |= TraitType | FlagPub;
    st->callsig = sig;
    st->callmut = (uint8_t)mutself;
    dclInfoJoin((INode*)st, NULL);

    FnSigNode *msig = newFnSigNode();
    inodeLexCopy((INode*)msig, lexnode);
    VarDclNode *self = newVarDclNode(selfName, VarDclTag, (INode*)immPerm);
    inodeLexCopy((INode*)self, lexnode);
    self->vtype = (INode*)newRefNodeFull(RefTag, lexnode, borrowRef,
        (INode*)newPermUseNode(mutself ? mutPerm : roPerm), newNameUseFromDclNode((INode*)st, lexnode));
    self->scope = 1;
    self->flowtempflags |= VarInitialized;
    nodesAdd(&msig->parms, (INode*)self);
    uint16_t parmnbr = 1;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *orig = (VarDclNode*)*nodesp;
        VarDclNode *parm = newVarDclNode(orig->namesym, VarDclTag, orig->perm);
        inodeLexCopy((INode*)parm, (INode*)orig);
        parm->vtype = orig->vtype;
        parm->scope = 1;
        parm->index = parmnbr++;
        parm->flowtempflags |= VarInitialized;
        nodesAdd(&msig->parms, (INode*)parm);
    }
    msig->rettype = sig->rettype;
    msig->lifeorder = sig->lifeorder;
    msig->lifenamed = sig->lifenamed;
    msig->lifechecked = sig->lifechecked;
    msig->lifestatic = sig->lifestatic;
    FnDclNode *fn = newFnDclNode(parensName, FlagMethFld | FlagPub, (INode*)msig, NULL);
    inodeLexCopy((INode*)fn, lexnode);
    iNsTypeAddFn((INsTypeNode*)st, fn);

    nodesAdd(&fnCallTraits, (INode*)st);
    INode *stnode = (INode*)st;
    inodeTypeCheckAny(pstate, &stnode);
    return st;
}

// The signature a type is a callable trait for, or NULL
FnSigNode *fnSigOfCallTrait(INode *type) {
    if (type == NULL || !isTypeNode(type))
        return NULL;
    INode *dcl = itypeGetTypeDcl(type);
    return dcl->tag == StructTag ? ((StructNode*)dcl)->callsig : NULL;
}

// Whether a struct's '()' method, found for a callable trait, takes the
// receiver the trait's kind allows: 'self &' (a read-only borrow) for either
// kind, 'self &mut' for the kind that may write
int fnSigCallSelfFits(StructNode *trait, FnDclNode *meth) {
    FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    if (msig->tag != FnSigTag || msig->parms->used == 0)
        return 0;
    INode *selftype = iexpGetTypeDcl(nodesGet(msig->parms, 0));
    if (selftype->tag != RefTag || itypeGetTypeDcl(((RefNode*)selftype)->region) != (INode*)borrowRef)
        return 0;
    INode *perm = itypeGetTypeDcl(((RefNode*)selftype)->perm);
    if (perm == (INode*)mutPerm)
        return trait->callmut;
    return perm != (INode*)uniPerm && perm != (INode*)mut1Perm && perm->tag == PermTag
        && !(permGetFlags(perm) & MayWrite);
}

// The permission a method's 'self' borrows with, or NULL when 'self' is not a
// borrow ('self So[T]', a by-value self, a static function)
INode *fnSigSelfBorrowPerm(FnDclNode *meth) {
    FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
    if (msig->tag != FnSigTag || msig->parms->used == 0)
        return NULL;
    INode *selftype = iexpGetTypeDcl(nodesGet(msig->parms, 0));
    if (selftype->tag != RefTag || itypeGetTypeDcl(((RefNode*)selftype)->region) != (INode*)borrowRef)
        return NULL;
    return itypeGetTypeDcl(((RefNode*)selftype)->perm);
}

// Whether a type's method may fill the slot of a trait's method behind a
// virtual reference. A call through the trait lends the permission the trait's
// 'self' declares, and that borrow is all the caller holds, so the
// implementation may ask for no more of it: a trait's 'self &' met by 'self &mut'
// would change what the caller was only allowed to read. Anything but a borrow
// on both sides is not compared here.
int fnSigVrefSelfFits(FnDclNode *traitmeth, FnDclNode *implmeth) {
    INode *tperm = fnSigSelfBorrowPerm(traitmeth);
    INode *iperm = fnSigSelfBorrowPerm(implmeth);
    if (tperm == NULL || iperm == NULL || tperm->tag != PermTag || iperm->tag != PermTag)
        return 1;
    return permMatches(iperm, tperm) != NoMatch;
}

// A 'self' as it is written, for a message: 'self &', 'self &mut', 'self &uni'
static char *fnSigSelfSpell(FnDclNode *meth, char *buf, size_t size) {
    INode *perm = fnSigSelfBorrowPerm(meth);
    char *name = perm && perm->tag == PermTag ? &inodeGetName(perm)->namestr : "";
    snprintf(buf, size, "self &%s", strcmp(name, "ro") == 0 ? "" : name);
    return buf;
}

// Why the struct 'impl' cannot be viewed as the trait 'trait' behind a virtual
// reference when one of its methods asks for a stronger 'self' than the trait's
// method declares; NULL when it is not that
char *fnSigVrefSelfRefusal(StructNode *trait, StructNode *impl) {
    if (trait->tag != StructTag || impl->tag != StructTag || trait->callsig || !(trait->flags & TraitType)
        || (impl->flags & TraitType))
        return NULL;
    ClosureInfo *closure = closureOfStruct((INode*)impl);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode*)*nodesp;
        INode *binding = namespaceFind(&impl->namespace, meth->namesym);
        if (binding == NULL)
            continue;
        FnDclNode *implmeth = iNsTypeFindVrefMethod(binding, meth, NULL);
        if (implmeth == NULL || fnSigVrefSelfFits(meth, implmeth))
            continue;
        static char msg[900];
        char wanted[40], has[40];
        fnSigSelfSpell(meth, wanted, sizeof(wanted));
        fnSigSelfSpell(implmeth, has, sizeof(has));
        if (closure)
            snprintf(msg, sizeof(msg),
                "This closure's `%s` needs `%s` (it changes state it holds or borrows), but %s's `%s` takes `%s`: a call through the trait lends only what the trait declares, so the closure would have more of the value than the caller allowed. Declare the method `%s` in the trait, or don't change that state.",
                &meth->namesym->namestr, has, &trait->namesym->namestr, &meth->namesym->namestr, wanted, has);
        else
            snprintf(msg, sizeof(msg),
                "%s's `%s` takes `%s`, but %s's `%s` takes `%s`: a call through the trait lends only what the trait declares, so %s would have more of the value than the caller allowed. Declare `%s` with `%s`, or the trait's with `%s`.",
                &impl->namesym->namestr, &meth->namesym->namestr, has,
                &trait->namesym->namestr, &meth->namesym->namestr, wanted,
                &impl->namesym->namestr, &meth->namesym->namestr, wanted, has);
        return msg;
    }
    return NULL;
}

// A callable reference or owner as it is written, for a message: '&<fn(i32) i32',
// '&<mut fn(i32) i32', 'So[imm, fn(i32) i32]'. 'ref' says the region and the
// permission it was written with, 'trait' the signature and the kind.
static void fnCallSpell(char *buf, size_t size, RefNode *ref, StructNode *trait) {
    char sig[300] = "";
    genericFnSigCat(sig, sizeof(sig), trait->callsig);
    INode *region = itypeGetTypeDcl(ref->region);
    if (region == (INode*)borrowRef) {
        snprintf(buf, size, "&<%s%s", trait->callmut ? "mut " : "", sig);
        return;
    }
    char *regname = region->tag == StructTag ? &((StructNode*)region)->namesym->namestr : "So";
    INode *perm = itypeGetTypeDcl(ref->perm);
    int permwrites = perm->tag == PermTag && (permGetFlags(perm) & MayWrite);
    if (trait->callmut && (!permwrites || perm == (INode*)uniPerm))
        snprintf(buf, size, "%s[%s]", regname, sig);
    else if (trait->callmut || permwrites)
        snprintf(buf, size, "%s[%s, %s]", regname, trait->callmut ? &inodeGetName(perm)->namestr : "imm", sig);
    else
        snprintf(buf, size, "%s[%s, %s]", regname, &inodeGetName(perm)->namestr, sig);
}

// Why a value of type 'from' is refused where the callable type 'to' is wanted,
// when that is the permission its '()' or the borrow needs; NULL when it is not
// that. The message leads with the cause in the author's words.
char *fnSigCallRefusal(INode *from, INode *to, int *code) {
    *code = ErrorCallablePerm;
    INode *todcl = itypeGetTypeDcl(to);
    if (todcl->tag != VirtRefTag)
        return NULL;
    RefNode *toref = (RefNode*)todcl;
    StructNode *trait = (StructNode*)itypeGetTypeDcl(toref->vtexp);
    if (trait->tag != StructTag)
        return NULL;
    INode *fromdcl = itypeGetTypeDcl(from);
    // A struct (a closure's included) whose method asks for a stronger 'self' than
    // the trait's
    if (trait->callsig == NULL) {
        if (fromdcl->tag != RefTag)
            return NULL;
        INode *target = itypeGetTypeDcl(((RefNode*)fromdcl)->vtexp);
        char *selfwhy = target->tag == StructTag ? fnSigVrefSelfRefusal(trait, (StructNode*)target) : NULL;
        if (selfwhy)
            *code = ErrorVtableSelf;
        return selfwhy;
    }
    // A callable that may change, where one that only reads is wanted
    if (fromdcl->tag == VirtRefTag) {
        StructNode *fromtrait = (StructNode*)itypeGetTypeDcl(((RefNode*)fromdcl)->vtexp);
        if (fromtrait->tag != StructTag || fromtrait->callsig == NULL || fromtrait->callmut == trait->callmut
            || !fnSigEqual(fromtrait->callsig, trait->callsig))
            return NULL;
        static char vmsg[700];
        char wanted[300];
        fnCallSpell(wanted, sizeof(wanted), toref, trait);
        if (fromtrait->callmut)
            snprintf(vmsg, sizeof(vmsg),
                "`%s` may only call a callable that reads its state; this one may change it.", wanted);
        else
            snprintf(vmsg, sizeof(vmsg),
                "`%s` may change what it points at, and this one is a read-only reference to a callable: lend it as a `&<mut`, which needs a `&mut` borrow of the callable.",
                wanted);
        return vmsg;
    }
    if (fromdcl->tag != RefTag)
        return NULL;
    INode *target = itypeGetTypeDcl(((RefNode*)fromdcl)->vtexp);
    if (target->tag != StructTag || (target->flags & TraitType))
        return NULL;
    FnDclNode *want = (FnDclNode*)namespaceFind(&trait->namespace, parensName);
    INode *binding = namespaceFind(&((StructNode*)target)->namespace, parensName);
    FnDclNode *meth = want && binding ? iNsTypeFindVrefMethod(binding, want, NULL) : NULL;
    if (meth == NULL)
        return NULL;
    static char msg[900];
    int isclosure = closureOfStruct(target) != NULL;
    char *what = isclosure ? "this closure" : itypeName(target);
    char wanted[300], other[300];
    fnCallSpell(wanted, sizeof(wanted), toref, trait);
    if (!fnSigCallSelfFits(trait, meth)) {
        FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(meth->vtype);
        INode *selftype = msig->tag == FnSigTag && msig->parms->used ? iexpGetTypeDcl(nodesGet(msig->parms, 0)) : NULL;
        INode *perm = selftype && selftype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)selftype)->perm) : NULL;
        if (perm == (INode*)mutPerm && !trait->callmut) {
            StructNode flipped = *trait;
            flipped.callmut = 1;
            fnCallSpell(other, sizeof(other), toref, &flipped);
            snprintf(msg, sizeof(msg),
                "`%s` may only call a callable that reads its state; %s changes it (its `()` takes `self &mut`). To let it change, the type is `%s`.",
                wanted, isclosure ? "this closure" : what, other);
        }
        else
            snprintf(msg, sizeof(msg),
                "The `()` of %s takes `self` %s, and `%s` calls it without that: it lends `self &%s`.",
                what, perm == (INode*)uniPerm ? "uniquely, so it is called once" : "by value",
                wanted, trait->callmut ? "mut" : "");
        return msg;
    }
    // '()' fits; the borrow lent is what is too weak
    if (trait->callmut && !(permGetFlags(((RefNode*)fromdcl)->perm) & MayWrite)) {
        snprintf(msg, sizeof(msg),
            "`%s` may change what it points at, and this is a read-only borrow of %s. Lend it with `&mut`.",
            wanted, what);
        return msg;
    }
    return NULL;
}
