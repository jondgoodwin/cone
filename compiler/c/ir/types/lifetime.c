/** Named lifetimes on a function's signature
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

// Is this a borrowed reference to a function? A function is never a local, so
// such a borrow is global whatever is written on it (itypeCarriesBorrow).
static int lifeIsFnBorrow(RefNode *ref) {
    return ref->tag == RefTag && isTypeNode(ref->vtexp)
        && itypeGetTypeDcl(ref->vtexp)->tag == FnSigTag;
}

// The lifetime a borrowed reference is written with, or, 'anon', as if it
// were written with none: what the signature would promise unannotated
static Name *lifeName(RefNode *ref, int anon) {
    return anon ? NULL : ref->lifename;
}

static int lifeHoldsAs(INode *type, Name *life, int anon) {
    if (type == NULL || life == staticLifeName)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? lifeHoldsAs(itypeGetTypeDcl(type), life, anon) : 0;
    case AliasDclTag:
        return lifeHoldsAs(((AliasDclNode *)type)->target, life, anon);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)type;
        if (itypeGetTypeDcl(ref->region) == borrowRef) {
            if (lifeIsFnBorrow(ref))
                return 0;
            if (lifeName(ref, anon) == life)
                return 1;
        }
        return lifeHoldsAs(ref->vtexp, life, anon);
    }
    case PtrTag:
        return lifeHoldsAs(((StarNode *)type)->vtexp, life, anon);
    case ArrayTag:
        return lifeHoldsAs(arrayElemType(type), life, anon);
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (lifeHoldsAs(*nodesp, life, anon))
                return 1;
        }
        return 0;
    }
    // Any other type holding a borrow holds it unnamed: a name is written only
    // on a reference
    default:
        return life == NULL && itypeCarriesBorrow(type);
    }
}

static int lifeSharedAs(INode *a, INode *b, int anon) {
    if (a == NULL)
        return 0;
    switch (a->tag) {
    case NameUseTag:
        return isTypeNode(a) ? lifeSharedAs(itypeGetTypeDcl(a), b, anon) : 0;
    case AliasDclTag:
        return lifeSharedAs(((AliasDclNode *)a)->target, b, anon);
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *ref = (RefNode *)a;
        if (itypeGetTypeDcl(ref->region) == borrowRef) {
            if (lifeIsFnBorrow(ref))
                return 0;
            if (lifeHoldsAs(b, lifeName(ref, anon), anon))
                return 1;
        }
        return lifeSharedAs(ref->vtexp, b, anon);
    }
    case PtrTag:
        return lifeSharedAs(((StarNode *)a)->vtexp, b, anon);
    case ArrayTag:
        return lifeSharedAs(arrayElemType(a), b, anon);
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)a)->elems, cnt, nodesp)) {
            if (lifeSharedAs(*nodesp, b, anon))
                return 1;
        }
        return 0;
    }
    default:
        return itypeCarriesBorrow(a) && lifeHoldsAs(b, NULL, anon);
    }
}

int lifeHolds(INode *type, Name *life) {
    return lifeHoldsAs(type, life, 0);
}

int lifeShared(INode *a, INode *b) {
    return lifeSharedAs(a, b, 0);
}

INode *lifePointee(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if ((typedcl->tag == RefTag || typedcl->tag == ArrayRefTag)
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef)
        return ((RefNode *)typedcl)->vtexp;
    return type;
}

// Is ''static' named anywhere in this type?
static int lifeNamesStatic(INode *type) {
    if (type == NULL)
        return 0;
    switch (type->tag) {
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return ((RefNode *)type)->lifename == staticLifeName || lifeNamesStatic(((RefNode *)type)->vtexp);
    case PtrTag:
        return lifeNamesStatic(((StarNode *)type)->vtexp);
    case TTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (lifeNamesStatic(*nodesp))
                return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int lifeParmStaticInside(INode *parmtype) {
    INode *typedcl = itypeGetTypeDcl(parmtype);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        return lifeNamesStatic(((RefNode *)typedcl)->vtexp);
    return lifeNamesStatic(typedcl);
}

int lifeIsStatic(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && ((RefNode *)typedcl)->lifename == staticLifeName;
}

// May a call store a value through this parameter: is it a writable borrowed
// reference to something that can hold a borrow?
static int lifeParmStores(INode *parmtype) {
    RefNode *ref = (RefNode *)itypeGetTypeDcl(parmtype);
    return (ref->tag == RefTag || ref->tag == ArrayRefTag) && itypeGetTypeDcl(ref->region) == borrowRef
        && (permGetFlags(ref->perm) & MayWrite) && itypeCarriesBorrow(ref->vtexp);
}

// What a signature promises about lifetimes, as a string of digits: for each
// parameter, whether it shares a lifetime with the result (1), whether its
// reference is ''static' (2) and whether a call may store through it (4);
// after each that may, for every other parameter, whether that one may be
// stored there. 'anon' reads it as if no lifetime were written. A call is
// checked against nothing else, so two signatures promising the same agree.
static char *lifeSigPromises(FnSigNode *sig, int anon) {
    uint32_t nparms = sig->parms->used;
    char *promises = memAllocStr(NULL, nparms * (nparms + 1) + 1);
    char *p = promises;
    for (uint32_t i = 0; i < nparms; ++i) {
        INode *parmtype = ((IExpNode *)nodesGet(sig->parms, i))->vtype;
        int stores = lifeParmStores(parmtype);
        *p++ = (char)('0' + lifeSharedAs(parmtype, sig->rettype, anon)
            + (!anon && lifeIsStatic(parmtype) ? 2 : 0) + (stores ? 4 : 0));
        if (!stores)
            continue;
        for (uint32_t k = 0; k < nparms; ++k) {
            if (k != i)
                *p++ = (char)('0' + lifeSharedAs(((IExpNode *)nodesGet(sig->parms, k))->vtype,
                    lifePointee(parmtype), anon));
        }
    }
    *p = '\0';
    return promises;
}

int lifeSigsAgree(FnSigNode *a, FnSigNode *b) {
    if (!a->lifenamed && !b->lifenamed)
        return 1;
    if (a->parms->used != b->parms->used)
        return 0;
    return strcmp(lifeSigPromises(a, !a->lifenamed), lifeSigPromises(b, !b->lifenamed)) == 0;
}

char *lifeSigSpell(char *bufp, FnSigNode *sig) {
    if (!sig->lifenamed)
        return bufp;
    char *promises = lifeSigPromises(sig, 0);
    if (strcmp(promises, lifeSigPromises(sig, 1)) == 0)
        return bufp;
    *bufp++ = 'G';
    size_t len = strlen(promises);
    memcpy(bufp, promises, len);
    bufp += len;
    *bufp++ = '_';
    return bufp;
}
