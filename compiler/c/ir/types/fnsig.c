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
    sig->retwritten = 0;
    sig->ccall = 0;
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
    inodeFprint(sig->ccall ? "fn @c(" : "fn(");
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
        // An init's 'self: &new' is the one place that permission is written
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

// Compare two function signatures to see if they are equivalent, the calling
// convention they are marked for too if 'checkc'
static int fnSigEqualC(FnSigNode *node1, FnSigNode *node2, int checkc) {
    INode **nodes1p, **nodes2p;
    uint32_t cnt;

    // A Cone function and a C-named one are called differently: never one type
    if (checkc && node1->ccall != node2->ccall)
        return 0;

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

// Compare two function signatures to see if they are equivalent
int fnSigEqual(FnSigNode *node1, FnSigNode *node2) {
    return fnSigEqualC(node1, node2, 1);
}

// Is an implementation's type 'impl' the type a trait requirement's 'req' names?
// 'Self' in a requirement stands for the type meeting it, 'selftype', as it does
// in a default cloned into that type (cloneNode repoints it there). Only the
// name 'Self' is read that way, through a reference, a pointer or a slice to it:
// a requirement naming its trait outright means the trait, and so does a virtual
// reference to Self, '&<Self', which is a reference to the trait. With no
// 'selftype' the match is exact.
//
// In an enum's own method 'Self' is bound to the enum, so the requirement's 'Self'
// would equal the enum by itself; but the method is each variant's copy, where 'Self'
// is the variant, and a variant's own method meeting it with the enum for 'Self' would
// be a conversion. So with a 'selftype' that name is never read as the enum.
static int fnSigReqNamesEnumSelf(INode *req) {
    for (;;) {
        if (isNameUseNode(req) && ((NameUseNode*)req)->namesym == selfTypeName)
            return (itypeGetTypeDcl(req)->flags & EnumType) != 0;
        INode *dcl = itypeGetTypeDcl(req);
        switch (dcl->tag) {
        case RefTag:
        case ArrayRefTag:
            req = ((RefNode*)dcl)->vtexp;
            break;
        case PtrTag:
            req = ((StarNode*)dcl)->vtexp;
            break;
        default:
            return 0;
        }
    }
}

static int fnSigReqTypeSame(INode *req, INode *impl, INode *selftype) {
    if (itypeIsSame(req, impl) && !(selftype && fnSigReqNamesEnumSelf(req)))
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
            && (permIsSame(reqref->perm, implref->perm)
                // A reference written with no permission to a type declaring Immutable is
                // 'imm' (refImmutableDefaultPerm), but a trait's '&Self' is 'ro' as the
                // trait is written. Where Self is such a type, they are the same reference
                || (isNameUseNode(reqref->vtexp) && ((NameUseNode*)reqref->vtexp)->namesym == selfTypeName
                    && itypeIsImmutable(selftype)
                    && itypeGetTypeDcl(reqref->perm) == (INode*)roPerm && itypeGetTypeDcl(implref->perm) == (INode*)immPerm))
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

// Does a requirement's type name 'Self' outright, or through the references and
// pointers that lead to it? Such a type is read as the implementer
// (fnSigReqTypeSame), so it has no fixed shape to be compared for variance.
static int fnSigReqNamesSelf(INode *req) {
    for (;;) {
        if (isNameUseNode(req) && ((NameUseNode*)req)->namesym == selfTypeName)
            return 1;
        INode *dcl = itypeGetTypeDcl(req);
        switch (dcl->tag) {
        case RefTag:
        case ArrayRefTag:
            req = ((RefNode*)dcl)->vtexp;
            break;
        case PtrTag:
            req = ((StarNode*)dcl)->vtexp;
            break;
        default:
            return 0;
        }
    }
}

// Two distinct types that itypeMatches called a CastSubtype of each other, in the
// slot of a signature ('req' the type the signature names, 'impl' the one that
// stands in it): are they one layout, so the value passes as it is? A value held
// by value is passed and returned as its own type's aggregate, and only an
// enrichment and its base are one. A variant is a different aggregate from its
// enum, so a variant for its enum (or the reverse) would be read as another value
// and needs a conversion, which no recast makes. A reference to either is one
// pointer, and an array of recast elements is those elements in a row.
static int fnSigRecastSameLayout(INode *req, INode *impl) {
    if (itypeIsSame(req, impl))
        return 1;
    INode *reqdcl = itypeGetTypeDcl(req);
    switch (reqdcl->tag) {
    case RefTag:
    case VirtRefTag:
    case ArrayRefTag:
    case PtrTag:
        return 1;
    case ArrayTag:
        return itypeGetTypeDcl(impl)->tag == ArrayTag
            && fnSigRecastSameLayout(arrayElemType(req), arrayElemType(impl));
    default:
        return structExtendsEquiv(req, impl);
    }
}

// How an implementation's type 'impl' stands where a requirement's type 'req' is
typedef enum {
    PosRefused,     // not the requirement's, and not a recast of it
    PosSame,        // the requirement's own type
    PosRecast,      // differs by a recast of one layout alone
    PosLayout,      // differs by a recast that is not one layout (a variant held for its enum)
} FnSigPos;

// Does an implementation's type 'impl' stand where a requirement's type 'req'
// is, or differ from it only by a recast? 'flowin' is true for a parameter, a
// value that flows from the caller (the requirement's) into the implementation,
// and false for a result, which flows the other way. This is the one test every
// route asks (fnSigMeetKind): itypeMatches in Regref, where only EqMatch and
// CastSubtype count, since a difference that needs a run-time conversion (a wider
// number, a value made an Option) is no recast and a call passes the
// requirement's values as they are, and a CastSubtype counts only if it is one
// layout (fnSigRecastSameLayout).
static FnSigPos fnSigPosMeets(INode *req, INode *impl, INode *selftype, int flowin) {
    if (fnSigReqTypeSame(req, impl, selftype))
        return PosSame;
    if (fnSigReqNamesSelf(req))
        return PosRefused;
    TypeCompare match = flowin ? itypeMatches(impl, req, Regref) : itypeMatches(req, impl, Regref);
    if (match == EqMatch)
        return PosSame;
    if (match != CastSubtype)
        return PosRefused;
    return fnSigRecastSameLayout(req, impl) ? PosRecast : PosLayout;
}

// How an implementation's signature stands against a requirement's
typedef enum {
    FnSigMeetNo,        // does not meet it
    FnSigMeetLayout,    // meets it in every position but a value held by value that is not one layout
    FnSigMeetSame,      // meets it, every type and lifetime the requirement's own
    FnSigMeetRecast,    // meets it, differing by a recast
} FnSigMeet;

// Name the position 'at' (1-based, 0 for the result) of the signature 'req'
static void fnSigPosSpell(char *buf, size_t size, FnSigNode *req, uint32_t at) {
    if (at == 0)
        snprintf(buf, size, "result");
    else {
        Name *parmname = ((VarDclNode*)nodesGet(req->parms, at - 1))->namesym;
        if (parmname && parmname != anonName)
            snprintf(buf, size, "parameter %u (`%s`)", at, &parmname->namestr);
        else
            snprintf(buf, size, "parameter %u", at);
    }
}

// Does an implementation's signature 'impl' meet a requirement's 'req' as a
// subtype of it does: each parameter may be a supertype of the requirement's
// (contravariant) and the result a subtype of its (covariant), where the
// difference is a recast alone and no run-time conversion (fnSigPosMeets), and
// the lifetimes it promises are the requirement's or more (lifeSigMeets: a
// result may hold fewer borrows). THIS IS THE ONE RULE for a callable meeting a
// signature: a function reference's, a callable's, a module trait's, a trait's
// method, 'Uni[fn(sig)]' and a generic's 'fn(sig)' condition each ask it, none
// spelling it again. An equal signature meets it as the special case where every
// type is the same, and is the more specific: a requirement an exact candidate
// meets is not left to the looser reading (iNsTypeFindVrefMethod). 'receiver'
// says what is the first parameter, the receiver, which has its own rule:
// ReceiverNone, none; ReceiverBoth, both signatures have one and it is not
// compared; ReceiverImpl, only the implementation (a method) has one, standing
// where a signature written without one is wanted, and the lifetimes, which are
// counted by position, are not asked. 'checkc' compares the calling convention
// each is marked for; 'selftype' is as for fnSigVrefEqual. Where the one reason
// it is refused is a value held by value that is not one layout, that is
// FnSigMeetLayout and 'diff' (if given) says where; the lifetimes are not asked
// then.
static FnSigMeet fnSigMeetKind(FnSigNode *impl, FnSigNode *req, INode *selftype, FnSigReceiver receiver,
        int checkc, FnSigLayoutDiff *diff) {
    INode **implp, **reqp;
    uint32_t cnt;

    // A Cone function and a C-named one pass their values differently, and no
    // conversion or thunk makes one the other (fnSigConvRefusal says so)
    if (checkc && impl->ccall != req->ccall)
        return FnSigMeetNo;
    uint32_t implextra = receiver == ReceiverImpl ? 1 : 0;
    if (impl->parms->used != req->parms->used + implextra)
        return FnSigMeetNo;
    int recast = 0;
    INode *layoutreq = NULL, *layoutimpl = NULL;
    uint32_t layoutat = 0;
    reqp = &nodesGet(req->parms, 0);
    for (nodesFor(impl->parms, cnt, implp)) {
        if (receiver != ReceiverNone && cnt == impl->parms->used) {
            if (receiver == ReceiverBoth)
                reqp++;
            continue;
        }
        INode *reqtype = ((IExpNode*)*reqp)->vtype;
        INode *impltype = ((IExpNode*)*implp)->vtype;
        switch (fnSigPosMeets(reqtype, impltype, selftype, 1)) {
        case PosRefused:
            return FnSigMeetNo;
        case PosRecast:
            recast = 1;
            break;
        case PosLayout:
            if (layoutreq == NULL) {
                layoutreq = itypeGetTypeDcl(reqtype);
                layoutimpl = itypeGetTypeDcl(impltype);
                layoutat = impl->parms->used - cnt + 1 - implextra;
            }
            break;
        default:
            break;
        }
        reqp++;
    }
    switch (fnSigPosMeets(req->rettype, impl->rettype, selftype, 0)) {
    case PosRefused:
        return FnSigMeetNo;
    case PosRecast:
        recast = 1;
        break;
    case PosLayout:
        if (layoutreq == NULL) {
            layoutreq = itypeGetTypeDcl(req->rettype);
            layoutimpl = itypeGetTypeDcl(impl->rettype);
            layoutat = 0;
        }
        break;
    default:
        break;
    }
    if (layoutreq) {
        if (diff) {
            fnSigPosSpell(diff->position, sizeof(diff->position), req, layoutat);
            diff->req = layoutreq;
            diff->impl = layoutimpl;
        }
        return FnSigMeetLayout;
    }
    if (receiver == ReceiverImpl)
        return recast ? FnSigMeetRecast : FnSigMeetSame;
    if (!lifeSigMeets(impl, req))
        return FnSigMeetNo;
    return recast || !lifeSigsAgree(impl, req) ? FnSigMeetRecast : FnSigMeetSame;
}

static int fnSigMeetsC(FnSigNode *impl, FnSigNode *req, INode *selftype, FnSigReceiver receiver, int checkc) {
    FnSigMeet kind = fnSigMeetKind(impl, req, selftype, receiver, checkc, NULL);
    return kind == FnSigMeetSame || kind == FnSigMeetRecast;
}

// A method's signature meets a requirement's, ignoring the receiver
int fnSigVrefMeets(FnSigNode *impl, FnSigNode *req, INode *selftype) {
    return fnSigMeetsC(impl, req, selftype, ReceiverBoth, 0);
}

// A method's signature meets one written without a receiver, as a generic's
// 'fn(sig)' condition is met by a struct's '()'
int fnSigMeetsMethod(FnSigNode *impl, FnSigNode *req) {
    return fnSigMeetsC(impl, req, NULL, ReceiverImpl, 0);
}

// The same for two signatures that have no receiver (a function reference, a
// plain function standing where a callable's '()' is wanted, a module's function
// where a module trait asks for one). A Cone function and a C-named one are never
// each other's, as fnSigEqual has it.
int fnSigMeets(FnSigNode *impl, FnSigNode *req) {
    return fnSigMeetsC(impl, req, NULL, ReceiverNone, 1);
}

// Is the one reason 'impl' does not meet 'req' a parameter or the result held by
// value that is not one layout (a variant for its enum, or the reverse)? Then
// 'diff' says which, the first such position. Every other position meets it, so
// this is the cause to give, where a plain refusal would only say they differ.
int fnSigLayoutDiff(FnSigNode *impl, FnSigNode *req, INode *selftype, FnSigReceiver receiver, int checkc,
        FnSigLayoutDiff *diff) {
    return fnSigMeetKind(impl, req, selftype, receiver, checkc, diff) == FnSigMeetLayout;
}

// Say why a signature is refused for the position 'diff' names, as the sentence
// that follows the lead each route gives its own subject. 'wanted' names what the
// requirement has there ("the wanted one"), 'through' what a call goes through
// ("the reference").
void fnSigLayoutSpell(char *buf, size_t size, const FnSigLayoutDiff *diff, const char *wanted, const char *through) {
    char reqspelled[160], implspelled[160];
    reqspelled[0] = implspelled[0] = '\0';
    itypeSpellCat(reqspelled, sizeof(reqspelled), diff->req, 0);
    itypeSpellCat(implspelled, sizeof(implspelled), diff->impl, 0);
    snprintf(buf, size,
        "its %s is `%s` held by value, where %s has `%s`. The two are not one layout (a variant and its enum are passed and returned as different aggregates), so a call through %s would read the value wrongly, and no cast makes one the other.",
        diff->position, implspelled, wanted, reqspelled, through);
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
// 'to': 'from' meets it (fnSigMeetKind, the one rule), EqMatch where it is the
// same and CastSubtype where it differs by a recast. A conversion never matches.
TypeCompare fnSigMatches(FnSigNode *to, FnSigNode *from) {
    switch (fnSigMeetKind(from, to, NULL, ReceiverNone, 1, NULL)) {
    case FnSigMeetSame:
        return EqMatch;
    case FnSigMeetRecast:
        return CastSubtype;
    default:
        return NoMatch;
    }
}

// The signature a type is a plain reference to, or NULL: '&fn(...)', or the
// type of '&f' for a function 'f'
static FnSigNode *fnSigRefTarget(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl->tag != RefTag)
        return NULL;
    INode *target = itypeGetTypeDcl(((RefNode*)dcl)->vtexp);
    return target->tag == FnSigTag ? (FnSigNode*)target : NULL;
}

// A signature as a function reference is written: '&fn(Vec3) f32', '&fn @c (Vec3) f32'
static void fnSigRefSpell(char *buf, size_t size, FnSigNode *sig) {
    char spelled[300] = "";
    genericFnSigCat(spelled, sizeof(spelled), sig);
    snprintf(buf, size, "&%s", spelled);
}

// Why the function reference signature 'fromsig' is refused where 'tosig' is
// wanted when a parameter or the result is held by value and differs in layout (a
// variant for its enum, or the reverse): the first such position, named with both
// types; NULL when no position is that. A call through 'tosig' passes and reads
// that value as 'tosig' has it. 'callable' is true where 'tosig' is the signature
// of a callable ('&<fn(...)'), not of a function reference.
static char *fnSigByValueRefusal(FnSigNode *tosig, FnSigNode *fromsig, int callable) {
    FnSigLayoutDiff diff;
    if (!fnSigLayoutDiff(fromsig, tosig, NULL, ReceiverNone, 0, &diff))
        return NULL;
    static char msg[1100];
    char wanted[320], have[320], why[700];
    fnSigRefSpell(have, sizeof(have), fromsig);
    fnSigRefSpell(wanted, sizeof(wanted), tosig);
    fnSigLayoutSpell(why, sizeof(why), &diff, callable ? "the callable" : "the wanted one",
        callable ? "the callable" : "the reference");
    if (callable)
        snprintf(msg, sizeof(msg),
            "`%s` is refused where the callable `&<%s` is wanted: %s Write a wrapper function that converts, and make that the callable.",
            have, wanted + 1, why);
    else
        snprintf(msg, sizeof(msg),
            "`%s` is refused where `%s` is wanted: %s Write a wrapper function that converts, and take a reference to that.",
            have, wanted, why);
    return msg;
}

// Why a value of type 'from' is refused where the type 'to' is wanted, when the
// two are function references that differ only in the calling convention they
// are marked for, or in a parameter or result held by value that is not one
// layout (fnSigByValueRefusal); NULL when it is not that
char *fnSigConvRefusal(INode *from, INode *to, int *code) {
    FnSigNode *fromsig = fnSigRefTarget(from);
    if (fromsig == NULL)
        return NULL;
    static char msg[1100];
    char have[320], wanted[320];
    INode *todcl = itypeGetTypeDcl(to);
    // A callable reference, '&<fn(...)', calls what it holds with Cone's convention
    if (todcl->tag == VirtRefTag) {
        StructNode *trait = (StructNode*)itypeGetTypeDcl(((RefNode*)todcl)->vtexp);
        if (trait->tag != StructTag || trait->callsig == NULL)
            return NULL;
        // A by-value parameter or result that is not one layout; the stub passes
        // the callable's values as they are
        char *bywhy = fnSigByValueRefusal(trait->callsig, fromsig, 1);
        if (bywhy) {
            *code = ErrorFnRefByValue;
            return bywhy;
        }
        if (!fromsig->ccall || !fnSigMeetsC(fromsig, trait->callsig, NULL, ReceiverNone, 0))
            return NULL;
        *code = ErrorFnRefConv;
        fnSigRefSpell(have, sizeof(have), fromsig);
        snprintf(msg, sizeof(msg),
            "`%s` is a reference to a C-named function, and a callable (`&<fn`, `Uni[fn]`) calls what it holds in Cone's convention, which passes a struct differently from C's. Neither converts to the other. Call the function from a Cone function, and make that the callable.",
            have);
        return msg;
    }
    FnSigNode *tosig = fnSigRefTarget(to);
    if (tosig == NULL)
        return NULL;
    if (tosig->ccall == fromsig->ccall || !fnSigMeetsC(fromsig, tosig, NULL, ReceiverNone, 0)) {
        char *bywhy = fnSigByValueRefusal(tosig, fromsig, 0);
        if (bywhy)
            *code = ErrorFnRefByValue;
        return bywhy;
    }
    *code = ErrorFnRefConv;
    fnSigRefSpell(have, sizeof(have), fromsig);
    fnSigRefSpell(wanted, sizeof(wanted), tosig);
    if (fromsig->ccall)
        snprintf(msg, sizeof(msg),
            "`%s` is a reference to a C-named function, and `%s`, a Cone function's, is wanted. C and Cone pass a struct differently, so a call through one would pass its values wrongly, and neither converts to the other. Where C is to call it, write the wanted type `&fn @c (...)`; where Cone is, call the C function from a Cone function and use that.",
            have, wanted);
    else
        snprintf(msg, sizeof(msg),
            "`%s` is a reference to a Cone function, and `%s`, a C-named function's, is wanted. C would call it in C's convention, which passes a struct differently from Cone's, so its values would arrive wrong, and neither converts to the other. Make the function C-named, `fn @c name(...)`, or put it in an `@c` module.",
            have, wanted);
    return msg;
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
// The signature as a trait: '&<fn(sig)', 'Uni[fn(sig)]'

// The callable traits made so far, one per signature and kind of 'self'
static Nodes *fnCallTraits = NULL;

// The trait standing for 'fn(sig)' behind a virtual reference: a trait with the
// one method '()' of that signature, which anything with a pub '()' of exactly
// those parameter and return types meets (a closure's hidden struct, a
// hand-written struct, a plain function through its stub). The reference's
// permission is the call's kind: a reference that may write ('&<mut', an owner)
// is to the trait whose '()' takes 'self: &mut', any other to the one taking
// 'self: &', so a state-changing '()' is not met behind a read-only reference.
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
// receiver the trait's kind allows: 'self: &' (a read-only borrow) for either
// kind, 'self: &mut' for the kind that may write
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
// borrow ('self Uni[T]', a by-value self, a static function)
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
// implementation may ask for no more of it: a trait's 'self: &' met by 'self: &mut'
// would change what the caller was only allowed to read. Anything but a borrow
// on both sides is not compared here.
int fnSigVrefSelfFits(FnDclNode *traitmeth, FnDclNode *implmeth) {
    INode *tperm = fnSigSelfBorrowPerm(traitmeth);
    INode *iperm = fnSigSelfBorrowPerm(implmeth);
    if (tperm == NULL || iperm == NULL || tperm->tag != PermTag || iperm->tag != PermTag)
        return 1;
    return permMatches(iperm, tperm) != NoMatch;
}

// A 'self' as it is written, for a message: 'self: &', 'self: &mut', 'self: &uni'
static char *fnSigSelfSpell(FnDclNode *meth, char *buf, size_t size) {
    INode *perm = fnSigSelfBorrowPerm(meth);
    char *name = perm && perm->tag == PermTag ? &inodeGetName(perm)->namestr : "";
    snprintf(buf, size, "self: &%s", strcmp(name, "ro") == 0 ? "" : name);
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

// Why 'impl' cannot be viewed as 'trait' behind a virtual reference that may write
// when a field the trait requires has a narrower type in 'impl' (structVirtRefFieldVariant);
// NULL when it is not that
char *fnSigVrefFieldRefusal(StructNode *trait, StructNode *impl) {
    if (trait->tag != StructTag || impl->tag != StructTag || trait->callsig || !(trait->flags & TraitType))
        return NULL;
    StructNode *offender = impl;
    FieldDclNode *fld = structVirtRefFieldVariant(trait, impl, &offender);
    if (fld == NULL)
        return NULL;
    INode *strfld = namespaceFind(&offender->namespace, fld->namesym);
    static char msg[900];
    char traittype[200], impltype[200];
    traittype[0] = impltype[0] = '\0';
    itypeSpellCat(traittype, sizeof(traittype), fld->vtype, 0);
    itypeSpellCat(impltype, sizeof(impltype), strfld->tag == FieldDclTag ? ((FieldDclNode*)strfld)->vtype : fld->vtype, 0);
    snprintf(msg, sizeof(msg),
        "%s's field `%s` is `%s`, but %s declares it `%s`: a virtual reference that may write (`&<mut`, `&<uni`, `Uni[Trait]`) stores a value of the trait's type, which a field of the narrower type cannot hold. A reference that writes sees a field at exactly the trait's type, so declare `%s` as `%s` in %s; a read-only `&<` may see the narrower field.",
        &offender->namesym->namestr, &fld->namesym->namestr, impltype,
        &trait->namesym->namestr, traittype,
        &fld->namesym->namestr, traittype, &offender->namesym->namestr);
    return msg;
}

// A callable reference or owner as it is written, for a message: '&<fn(i32) i32',
// '&<mut fn(i32) i32', 'Uni[imm, fn(i32) i32]'. 'ref' says the region and the
// permission it was written with, 'trait' the signature and the kind.
static void fnCallSpell(char *buf, size_t size, RefNode *ref, StructNode *trait) {
    char sig[300] = "";
    genericFnSigCat(sig, sizeof(sig), trait->callsig);
    INode *region = itypeGetTypeDcl(ref->region);
    if (region == (INode*)borrowRef) {
        snprintf(buf, size, "&<%s%s", trait->callmut ? "mut " : "", sig);
        return;
    }
    char *regname = region->tag == StructTag ? &((StructNode*)region)->namesym->namestr : "Uni";
    INode *perm = itypeGetTypeDcl(ref->perm);
    int permwrites = perm->tag == PermTag && (permGetFlags(perm) & MayWrite);
    if (trait->callmut && (!permwrites || perm == (INode*)uniPerm))
        snprintf(buf, size, "%s[%s]", regname, sig);
    else if (trait->callmut || permwrites)
        snprintf(buf, size, "%s[%s, %s]", regname, trait->callmut ? &inodeGetName(perm)->namestr : "imm", sig);
    else
        snprintf(buf, size, "%s[%s, %s]", regname, &inodeGetName(perm)->namestr, sig);
}

static int fnSigFieldThreadBound(INode *type) {
    return itypeThreadBound(type, NULL);
}

static int fnSigFieldNotShareable(INode *type) {
    return itypeNotShareable(type);
}

// Why a value of the struct 'target' is refused where a virtual reference marked
// '+ Sendable' or '+ Shareable' ('mark', the one it does not meet) is wanted: the
// variable a closure holds or borrows that binds it, or the part of a struct that
// does, and what to do about it
static char *fnSigMarkRefusal(RefNode *toref, StructNode *target, uint8_t mark) {
    static char msg[1400];
    char wanted[300] = "", why[512] = "", what[512] = "", advice[600] = "";
    itypeSpellCat(wanted, sizeof(wanted), (INode*)toref, 0);
    if (mark == RefMarkShapeKeeping) {
        char reason[900] = "";
        shapeKeepsVia(target, (StructNode*)itypeGetTypeDcl(toref->vtexp), reason, sizeof(reason));
        snprintf(msg, sizeof(msg),
            "`%s` says a call through it keeps the shape of what the caller has borrowed, and %s. Leave '+ ShapeKeeping' off, which ends the caller's borrows into a collection at the call, or don't reshape it there.",
            wanted, reason);
        return msg;
    }
    int sendable = mark == RefMarkSendable;
    int (*bad)(INode *) = sendable ? fnSigFieldThreadBound : fnSigFieldNotShareable;
    ClosureCap *cap = closureFirstCap((INode*)target, bad);
    if (cap != NULL) {
        char captype[256] = "";
        itypeSpellCat(captype, sizeof(captype), cap->field->vtype, 0);
        if (sendable)
            genericNotSendableWhy(cap->field->vtype, what, why, StaticOff);
        else {
            char path[128];
            INode *culprit = itypeNotShareableWhy(cap->field->vtype, 1, path, sizeof(path));
            if (culprit)
                genericNotShareableReason(culprit, 0, why, sizeof(why));
        }
        closureCapAdvice(cap, !bad(closureCapValueType(cap)), advice, sizeof(advice));
        snprintf(msg, sizeof(msg),
            "`%s` says the closure behind it is %s, and this closure is not: it %s '%s' (%s), which is %s. %s",
            wanted, refMarkSpell(mark), cap->state ? "holds" : "borrows", &cap->dcl->namesym->namestr, captype, why, advice);
        return msg;
    }
    if (sendable) {
        genericNotSendableWhy((INode*)target, what, why, StaticOff);
        snprintf(msg, sizeof(msg), "`%s` says the value behind it is Sendable, and %s is not: %s %s.",
            wanted, itypeName((INode*)target), what, why);
    }
    else {
        char path[128], culpritname[256] = "";
        INode *culprit = itypeNotShareableWhy((INode*)target, 1, path, sizeof(path));
        if (culprit) {
            genericNotShareableReason(culprit, 1, why, sizeof(why));
            itypeSpellCat(culpritname, sizeof(culpritname), culprit, 0);
        }
        snprintf(msg, sizeof(msg), "`%s` says the value behind it is Shareable, and %s is not: %s is %s, %s.",
            wanted, itypeName((INode*)target), path[0] ? path : "it", culpritname, why);
    }
    return msg;
}

// Why 'impl' cannot be viewed as 'trait' behind a virtual reference when a method
// it declares is the trait's but for a parameter or the result held by value that
// is not one layout (a variant for its enum, or the reverse): the method's slot is
// called with the trait's values; NULL when it is not that
static char *fnSigVrefLayoutRefusal(StructNode *trait, StructNode *impl) {
    if (trait->tag != StructTag || impl->tag != StructTag || trait->callsig || !(trait->flags & TraitType)
        || (impl->flags & TraitType))
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode*)*nodesp;
        INode *binding = namespaceFind(&impl->namespace, meth->namesym);
        if (binding == NULL || iNsTypeFindVrefMethod(binding, meth, NULL) != NULL)
            continue;
        FnSigLayoutDiff diff;
        if (!iNsTypeVrefLayoutDiff(binding, meth, NULL, &diff))
            continue;
        static char msg[1100];
        char why[700];
        fnSigLayoutSpell(why, sizeof(why), &diff, trait->namesym ? &trait->namesym->namestr : "the trait",
            trait->namesym ? &trait->namesym->namestr : "the trait");
        snprintf(msg, sizeof(msg),
            "%s's `%s` is refused for %s's: %s Write the method with the trait's types, and convert inside it.",
            &impl->namesym->namestr, &meth->namesym->namestr, &trait->namesym->namestr, why);
        return msg;
    }
    return NULL;
}

// Why a value of type 'from' is refused where the callable type 'to' is wanted,
// when that is the permission its '()' or the borrow needs; NULL when it is not
// that. The message leads with the cause in the author's words.
char *fnSigCallRefusal(INode *from, INode *to, int *code) {
    char *convwhy = fnSigConvRefusal(from, to, code);
    if (convwhy)
        return convwhy;
    *code = ErrorCallablePerm;
    INode *todcl = itypeGetTypeDcl(to);
    if (todcl->tag != VirtRefTag)
        return NULL;
    RefNode *toref = (RefNode*)todcl;
    StructNode *trait = (StructNode*)itypeGetTypeDcl(toref->vtexp);
    if (trait->tag != StructTag)
        return NULL;
    INode *fromdcl = itypeGetTypeDcl(from);
    // A value that is not what '+ Sendable', '+ Shareable' or '+ ShapeKeeping' promises of it
    if (toref->marks && fromdcl->tag == RefTag) {
        INode *target = itypeGetTypeDcl(((RefNode*)fromdcl)->vtexp);
        uint8_t unmet = target->tag == StructTag && !(target->flags & TraitType)
            ? refMarksUnmet(toref->marks, target, (INode*)trait) : 0;
        if (unmet) {
            *code = unmet == RefMarkSendable ? ErrorNotSendable
                : unmet == RefMarkShareable ? ErrorNotShareable : ErrorNotShapeKeeping;
            return fnSigMarkRefusal(toref, (StructNode*)target, unmet);
        }
    }
    // A struct (a closure's included) whose method asks for a stronger 'self' than
    // the trait's
    if (trait->callsig == NULL) {
        if (fromdcl->tag != RefTag)
            return NULL;
        INode *target = itypeGetTypeDcl(((RefNode*)fromdcl)->vtexp);
        char *selfwhy = target->tag == StructTag ? fnSigVrefSelfRefusal(trait, (StructNode*)target) : NULL;
        if (selfwhy)
            *code = ErrorVtableSelf;
        else if (target->tag == StructTag && (permGetFlags(toref->perm) & MayWrite)) {
            selfwhy = fnSigVrefFieldRefusal(trait, (StructNode*)target);
            if (selfwhy)
                *code = ErrorVtableField;
        }
        if (selfwhy == NULL && target->tag == StructTag) {
            selfwhy = fnSigVrefLayoutRefusal(trait, (StructNode*)target);
            if (selfwhy)
                *code = ErrorFnRefByValue;
        }
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
    if (meth == NULL) {
        // A '()' that is the callable's but for a value held by value that is not one layout
        FnSigLayoutDiff diff;
        if (want && binding && iNsTypeVrefLayoutDiff(binding, want, NULL, &diff)) {
            static char lmsg[1100];
            char lwanted[300], lwhy[700];
            fnCallSpell(lwanted, sizeof(lwanted), toref, trait);
            fnSigLayoutSpell(lwhy, sizeof(lwhy), &diff, "the callable", "the callable");
            snprintf(lmsg, sizeof(lmsg),
                "The `()` of %s is refused where `%s` is wanted: %s Write the `()` with the callable's types, and convert inside it.",
                itypeName(target), lwanted, lwhy);
            *code = ErrorFnRefByValue;
            return lmsg;
        }
        return NULL;
    }
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
                "`%s` may only call a callable that reads its state; %s changes it (its `()` takes `self: &mut`). To let it change, the type is `%s`.",
                wanted, isclosure ? "this closure" : what, other);
        }
        else
            snprintf(msg, sizeof(msg),
                "The `()` of %s takes `self` %s, and `%s` calls it without that: it lends `self: &%s`.",
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
