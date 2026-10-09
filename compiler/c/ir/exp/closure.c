/** Handling for closure literals
 *
 * See closure.h. In the order a literal passes through the compiler:
 *
 * - The parser reads 'fn (u f32) [ribs, mut n = 0] f32 { ... }' and
 *   'x => x * 2' into a ClosureNode.
 * - Name resolution resolves the body in the code around it, with its own
 *   parameters and state names hooked over that, and notes each variable of the
 *   code around that the body names: the closure borrows it (closureNoteUse).
 * - Type check (closureTypeCheck) makes the hidden struct: a field for each
 *   state entry, a field holding a borrow for each variable the body names, and
 *   '()' holding the body. A name inside the body that names one of them is
 *   rewritten to the field's access when the body is checked (closureUse). The
 *   literal itself becomes 'new Hidden(state values, &variables)'.
 *
 * Whether a borrow is '&' or '&mut', and whether '()' takes 'self &' or 'self
 * &mut', is what the body does with it. It is found by trying: the body is
 * checked, its diagnostics unprinted, as a clone, with the weakest permissions;
 * if that fails it is checked with every borrow the variable allows '&mut', and
 * then each is taken back one at a time as long as the body still checks.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FnSigNode *closureHint = NULL;
Name *closureMethod = NULL;
int closureInferring = 0;

// What a literal given where the trait 'trait' is wanted must be: the signature
// of the trait's one method, without its receiver, and that method's name. NULL
// when the trait is not one a literal fills: '*count' says how many methods and
// fields it requires, and '*method' the name of the first method.
FnSigNode *closureTraitSig(StructNode *trait, Name **method, uint32_t *count) {
    *count = trait->fields.used;
    *method = NULL;
    FnDclNode *only = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        // A private method counts too: a generic bound requires it (a reference to the
        // trait has no slot for it, which its own use says)
        if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
            continue;
        if (only == NULL)
            only = (FnDclNode*)*nodesp;
        ++*count;
    }
    if (only)
        *method = only->namesym;
    if (*count != 1 || only == NULL || only->genericinfo)
        return NULL;
    FnSigNode *msig = (FnSigNode*)itypeGetTypeDcl(only->vtype);
    if (msig->tag != FnSigTag || msig->parms->used == 0)
        return NULL;
    FnSigNode *sig = newFnSigNode();
    inodeLexCopy((INode*)sig, (INode*)only);
    for (uint32_t i = 1; i < msig->parms->used; ++i)
        nodesAdd(&sig->parms, nodesGet(msig->parms, i));
    sig->rettype = msig->rettype;
    return sig;
}

// Whether the method the closure struct holds takes 'self &mut'
int closureMethodMutates(ClosureInfo *info) {
    INode *meth = info->method ? namespaceFind(&info->strct->namespace, info->method) : NULL;
    if (meth == NULL || meth->tag != FnDclTag)
        return 0;
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(((FnDclNode*)meth)->vtype);
    INode *selftype = sig->tag == FnSigTag && sig->parms->used ? iexpGetTypeDcl(nodesGet(sig->parms, 0)) : NULL;
    return selftype && selftype->tag == RefTag && itypeGetTypeDcl(((RefNode*)selftype)->perm) == (INode*)mutPerm;
}

ClosureNode *newClosureNode() {
    ClosureNode *node;
    newNode(node, ClosureNode, ClosureTag);
    node->vtype = unknownType;
    node->sig = newFnSigNode();
    node->state = newNodes(2);
    node->body = NULL;
    node->captures = newNodes(2);
    node->outerself = NULL;
    node->outer = NULL;
    node->outerscope = 0;
    node->isarrow = 0;
    return node;
}

// The copy of a closure literal, name resolved and not yet checked. Its state
// entries, parameters and body are copied, and each variable it borrows is
// pointed at the copy of its declaration where that was copied too.
INode *cloneClosureNode(CloneState *cstate, ClosureNode *node) {
    ClosureNode *newnode = memAllocBlk(sizeof(ClosureNode));
    memcpy(newnode, node, sizeof(ClosureNode));
    uint32_t dclpos = cloneDclPush();
    newnode->state = newNodes(node->state->used + 1);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->state, cnt, nodesp))
        nodesAdd(&newnode->state, cloneNode(cstate, *nodesp));
    newnode->sig = (FnSigNode*)cloneNode(cstate, (INode*)node->sig);
    newnode->body = cloneNode(cstate, node->body);
    newnode->captures = newNodes(node->captures->used + 1);
    for (nodesFor(node->captures, cnt, nodesp))
        nodesAdd(&newnode->captures, cloneDclFix(*nodesp));
    if (node->outerself)
        newnode->outerself = (VarDclNode*)cloneDclFix((INode*)node->outerself);
    newnode->outer = NULL;
    cloneDclPop(dclpos);
    return (INode*)newnode;
}

void closurePrint(ClosureNode *node) {
    inodeFprint("fn");
    inodePrintNode((INode*)node->sig);
    if (node->state->used) {
        inodeFprint(" [");
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(node->state, cnt, nodesp)) {
            inodePrintNode(*nodesp);
            if (cnt > 1)
                inodeFprint(", ");
        }
        inodeFprint("]");
    }
    inodeFprint(" {} ");
    inodePrintNode(node->body);
}

uint32_t closureParmCount(ClosureNode *node) {
    return node->sig->parms->used;
}

int closureNeedsSig(ClosureNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->sig->parms, cnt, nodesp))
        if (((VarDclNode*)*nodesp)->vtype == unknownType)
            return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Name resolution

static int closureHas(Nodes *list, INode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(list, cnt, nodesp))
        if (*nodesp == node)
            return 1;
    return 0;
}

// Is this variable the literal's own: a parameter, a state entry, or a local
// of its body? Anything else it can see is of the code around it.
static int closureOwns(ClosureNode *clo, VarDclNode *var) {
    return var->scope > clo->outerscope || closureHas(clo->sig->parms, (INode*)var)
        || closureHas(clo->state, (INode*)var);
}

void closureNameRes(NameResState *pstate, ClosureNode *node) {
    // The state's values and the parameters' types are read in the code around
    // the literal: the names the closure declares are not in scope in them
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->state, cnt, nodesp)) {
        VarDclNode *ent = (VarDclNode*)*nodesp;
        inodeNameRes(pstate, (INode**)&ent->perm);
        inodeNameRes(pstate, &ent->value);
    }
    inodeNameRes(pstate, (INode**)&node->sig);

    // A closure's names must differ from each other
    INode **otherp;
    uint32_t othercnt;
    for (nodesFor(node->sig->parms, cnt, nodesp)) {
        for (otherp = nodesp + 1, othercnt = cnt - 1; othercnt; --othercnt, ++otherp)
            if (((VarDclNode*)*nodesp)->namesym == ((VarDclNode*)*otherp)->namesym)
                errorMsgNode(*otherp, ErrorDupName, "Name is already defined. Only one allowed.");
    }
    for (nodesFor(node->state, cnt, nodesp)) {
        for (otherp = nodesp + 1, othercnt = cnt - 1; othercnt; --othercnt, ++otherp)
            if (((VarDclNode*)*nodesp)->namesym == ((VarDclNode*)*otherp)->namesym)
                errorMsgNode(*otherp, ErrorDupName, "Name is already defined. Only one allowed.");
        for (otherp = &nodesGet(node->sig->parms, 0), othercnt = node->sig->parms->used; othercnt; --othercnt, ++otherp)
            if (((VarDclNode*)*nodesp)->namesym == ((VarDclNode*)*otherp)->namesym)
                errorMsgNode(*nodesp, ErrorDupName, "Name is already defined. Only one allowed.");
    }

    nametblHookPush();
    for (nodesFor(node->sig->parms, cnt, nodesp))
        nametblHookNode(((VarDclNode*)*nodesp)->namesym, *nodesp);
    for (nodesFor(node->state, cnt, nodesp))
        nametblHookNode(((VarDclNode*)*nodesp)->namesym, *nodesp);

    node->outer = pstate->closure;
    node->outerscope = pstate->scope;
    pstate->closure = node;
    BlockNode *svloop = pstate->loopblock;
    BlockNode *svouter = pstate->outerloop;
    pstate->loopblock = NULL;
    pstate->outerloop = NULL;
    inodeNameRes(pstate, &node->body);
    pstate->loopblock = svloop;
    pstate->outerloop = svouter;
    pstate->closure = node->outer;
    nametblHookPop();
}

// A name was bound inside a closure literal's body. A variable of the code
// around, or the 'self' a bare member name is reached through, is one the
// closure borrows.
void closureNoteUse(NameResState *pstate, NameUseNode *name) {
    ClosureNode *clo = pstate->closure;
    INode *dcl = name->dclnode;
    if (clo == NULL || dcl == NULL)
        return;
    VarDclNode *var = NULL;
    int member = 0;
    if (dcl->tag == VarDclTag)
        var = (VarDclNode*)dcl;
    else if (!(name->flags & FlagQualified) && inodeIsMember(dcl)) {
        INode *self = selfName->node;
        if (self && self->tag == VarDclTag) {
            var = (VarDclNode*)self;
            member = 1;
        }
    }
    // A global, and a function's static, are reached by name from anywhere
    if (var == NULL || var->scope == 0 || (var->flags & FlagStatic))
        return;
    for (ClosureNode *c = clo; c && !closureOwns(c, var); c = c->outer) {
        if (!closureHas(c->captures, (INode*)var))
            nodesAdd(&c->captures, (INode*)var);
        if (member)
            c->outerself = var;
    }
}

// ---------------------------------------------------------------------------
// The hidden struct

// What a guess at the permissions is: whether '()' takes 'self &mut', and
// whether each variable the body names is borrowed '&mut'
typedef struct ClosurePlan {
    uint8_t selfmut;
    uint8_t *capmut;
    Name *method;           // The name of the one method: '()', or the trait's method a literal fills
} ClosurePlan;

static uint32_t closureSerial = 0;

// Does this variable allow a '&mut' borrow of it?
static int closureVarMut(VarDclNode *var) {
    INode *perm = itypeGetTypeDcl(var->perm);
    return perm->tag == PermTag && (permGetFlags(perm) & MayWrite);
}

// The access of a closure's field from inside its '()': 'self.name'
static INode *closureFieldAccess(ClosureInfo *info, FieldDclNode *field, INode *lexnode) {
    NameUseNode *selfuse = newNameUseNode(selfName);
    inodeLexCopy((INode*)selfuse, lexnode);
    selfuse->dclnode = (INode*)info->selfparm;
    selfuse->vtype = info->selfparm->vtype;
    FnCallNode *access = newFnCallNode((INode*)selfuse, 0);
    inodeLexCopy((INode*)access, lexnode);
    NameUseNode *member = newMemberUseNode(field->namesym);
    inodeLexCopy((INode*)member, lexnode);
    access->methfld = (INode*)member;
    return (INode*)access;
}

int closureUse(TypeCheckState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;
    FnDclNode *fn = pstate->fn;
    if (fn == NULL || fn->closure == NULL || name->dclnode == NULL || name->dclnode->tag != VarDclTag)
        return 0;
    ClosureInfo *info = fn->closure;
    for (uint32_t i = 0; i < info->ncaps; ++i) {
        ClosureCap *cap = &info->caps[i];
        if ((INode*)cap->dcl != name->dclnode)
            continue;
        INode *access = closureFieldAccess(info, cap->field, (INode*)name);
        *((INode**)namep) = access;
        inodeTypeCheckAny(pstate, (INode**)namep);
        // A borrowed variable is the place the borrow points at
        if (!cap->state && !inodeIsError(*(INode**)namep))
            derefInject((INode**)namep);
        return 1;
    }
    return 0;
}

ClosureInfo *closureOfStruct(INode *node) {
    if (node == NULL || node->tag != StructTag)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode*)node)->nodelist, cnt, nodesp))
        if ((*nodesp)->tag == FnDclTag && ((FnDclNode*)*nodesp)->closure)
            return ((FnDclNode*)*nodesp)->closure;
    return NULL;
}

ClosureInfo *closureOfDcl(INode *dcl) {
    return closureOfStruct(inodeGetOwner(dcl));
}

INode *closureSelfParm(FnDclNode *fn) {
    if (fn->closure)
        return (INode*)fn->closure->outerself;
    return nodesGet(((FnSigNode*)fn->vtype)->parms, 0);
}

// ---------------------------------------------------------------------------
// Closures in GPU code

// Why 'type' may not be held by a closure in GPU code, or NULL when it may. The
// closure is inlined into its kernel and dissolves there into locals, so what it
// holds must be what a GPU has: numbers, structs and arrays of them, and borrows
// of memory the GPU has (a local, a buffer's slice), which are as safe as the
// type they point at. A GPU has no allocator, so no owning reference; no code
// pointer, so no function reference and no virtual reference; and no address
// that means anything outside the kernel, so no raw pointer. 'depth' bounds the
// walk of a type that names itself.
static const char *closureGpuWhy(INode *type, int depth) {
    if (type == NULL || !isTypeNode(type) || depth > 8)
        return NULL;
    INode *dcl = itypeGetTypeDcl(type);
    switch (dcl->tag) {
    case ArrayTag:
        return closureGpuWhy(arrayElemType(dcl), depth + 1);
    case StructTag: {
        StructNode *strnode = (StructNode*)dcl;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode*)*nodesp;
            if (field->tag != FieldDclTag || field->namesym == NULL)
                continue;
            const char *why = closureGpuWhy(field->vtype, depth + 1);
            if (why)
                return why;
        }
        return NULL;
    }
    case RefTag:
    case ArrayRefTag: {
        RefNode *ref = (RefNode*)dcl;
        if (ref->vtexp && isTypeNode(ref->vtexp) && itypeGetTypeDcl(ref->vtexp)->tag == FnSigTag)
            return "a function reference, a pointer to code, which a GPU has none of";
        if (ref->region == NULL || itypeGetTypeDcl(ref->region) != borrowRef)
            return "an owning reference (So, Rc, Arc, Gc), which needs an allocator a GPU has none of";
        return closureGpuWhy(ref->vtexp, depth + 1);
    }
    case VirtRefTag:
        return "a virtual reference, which dispatches through a table of code pointers a GPU has none of";
    case PtrTag:
        return "a raw pointer, whose address means nothing on a GPU";
    default:
        return NULL;
    }
}

// Refuse a closure in GPU code that holds what a GPU has none of. Answers
// whether it holds only what a GPU has.
static int closureGpuCheck(ClosureNode *clo, INode **statetypes) {
    uint32_t k = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(clo->state, cnt, nodesp)) {
        VarDclNode *ent = (VarDclNode*)*nodesp;
        const char *why = closureGpuWhy(statetypes[k++], 0);
        if (why) {
            errorMsgNode((INode*)ent, ErrorGpuClosureData,
                "This closure, written in GPU code, holds %s as its own state, and its type %s is %s. In GPU code a closure holds numbers, structs and arrays of them, and borrows of memory the GPU has.",
                &ent->namesym->namestr, itypeName(statetypes[k - 1]), why);
            return 0;
        }
    }
    for (nodesFor(clo->captures, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode*)*nodesp;
        const char *why = closureGpuWhy(var->vtype, 0);
        if (why) {
            errorMsgNode((INode*)clo, ErrorGpuClosureData,
                "This closure, written in GPU code, borrows %s, and its type %s is %s. In GPU code a closure holds numbers, structs and arrays of them, and borrows of memory the GPU has.",
                &var->namesym->namestr, itypeName(var->vtype), why);
            return 0;
        }
    }
    return 1;
}

// Whether 'from' is a reference to a closure's hidden struct, and 'totype' a
// virtual reference (a '&<Trait', or an owner of a trait): the closure would be
// called through a table of code pointers. Refused in GPU code, where the
// closure is taken by a generic bound by a signature and inlined.
int closureGpuVirtRefused(INode *from, INode *totypedcl) {
    if (!flowGpu || totypedcl->tag != VirtRefTag || !isExpNode(from))
        return 0;
    INode *fromtype = iexpGetTypeDcl(from);
    if (fromtype->tag != RefTag || closureOfStruct(itypeGetTypeDcl(((RefNode*)fromtype)->vtexp)) == NULL)
        return 0;
    errorMsgNode(from, ErrorGpuClosureRef,
        "A closure in GPU code cannot be made a virtual reference: it would be called through a table of code pointers, which a GPU has none of. Give it to a function generic over its signature, '[F fn(...)]', which is inlined.");
    return 1;
}

// Make the hidden struct for a literal under a guess at its permissions, and
// check it. 'statetypes' are the state entries' types, from their values.
// Answers the struct, whose '()' is checked by the time it returns unless a
// layout is in flight. 'final' puts it among the module's nodes, to be
// generated.
static StructNode *closureBuild(TypeCheckState *pstate, ClosureNode *clo, ClosurePlan *plan,
        INode **statetypes, FnSigNode *exsig, int final) {
    ModuleNode *mod = dclInfoGetModule((INode*)pstate->fn);
    char buf[48];
    snprintf(buf, sizeof(buf), "closure#%u", ++closureSerial);
    StructNode *st = newStructNode(nametblFind(buf, strlen(buf)));
    inodeLexCopy((INode*)st, (INode*)clo);
    dclInfoJoin((INode*)st, (INode*)mod);

    ClosureInfo *info = memAllocBlk(sizeof(ClosureInfo));
    info->strct = st;
    info->ncaps = clo->state->used + clo->captures->used;
    info->caps = memAllocBlk((info->ncaps ? info->ncaps : 1) * sizeof(ClosureCap));
    info->outerself = clo->outerself;
    info->retinfer = 0;
    info->retset = 0;
    info->method = plan->method;
    info->errbase = errors;
    info->expanded = (pstate->fn->flags & FlagInline) || dclIsInstance((INode*)pstate->fn);
    info->lit = clo;

    // The state entries are fields holding their values, then a field for each
    // variable of the code around that the body names, holding a borrow of it
    uint32_t k = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(clo->state, cnt, nodesp)) {
        VarDclNode *ent = (VarDclNode*)*nodesp;
        FieldDclNode *fld = newFieldDclNode(ent->namesym, ent->perm);
        inodeLexCopy((INode*)fld, (INode*)ent);
        fld->vtype = statetypes[k];
        fld->flags |= FlagMethFld;
        fld->index = (uint16_t)k;
        structAddField(st, fld);
        info->caps[k].dcl = ent;
        info->caps[k].field = fld;
        info->caps[k].state = 1;
        ++k;
    }
    uint32_t ci = 0;
    for (nodesFor(clo->captures, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode*)*nodesp;
        Name *fname = var->namesym;
        if (namespaceFind(&st->namespace, fname)) {
            char dup[300];
            snprintf(dup, sizeof(dup), "%s'", &fname->namestr);
            fname = nametblFind(dup, strlen(dup));
        }
        FieldDclNode *fld = newFieldDclNode(fname, (INode*)newPermUseNode(immPerm));
        inodeLexCopy((INode*)fld, (INode*)clo);
        fld->vtype = (INode*)newRefNodeFull(RefTag, (INode*)clo, borrowRef,
            (INode*)newPermUseNode(plan->capmut[ci] ? mutPerm : roPerm), var->vtype);
        fld->flags |= FlagMethFld;
        fld->index = (uint16_t)k;
        structAddField(st, fld);
        info->caps[k].dcl = var;
        info->caps[k].field = fld;
        info->caps[k].state = 0;
        ++k;
        ++ci;
    }

    // '()': the user's parameters after a 'self' of the struct, the body
    FnSigNode *sig = clo->sig;
    if (sig->rettype == unknownType) {
        if (exsig)
            sig->rettype = exsig->rettype;
        else {
            // Read off the paths when the body is checked (closureImplicitReturn)
            sig->rettype = (INode*)newVoidNode();
            inodeLexCopy(sig->rettype, (INode*)clo);
            info->retinfer = 1;
        }
    }
    VarDclNode *self = newVarDclNode(selfName, VarDclTag, (INode*)immPerm);
    inodeLexCopy((INode*)self, (INode*)clo);
    self->vtype = (INode*)newRefNodeFull(RefTag, (INode*)clo, borrowRef,
        (INode*)newPermUseNode(plan->selfmut ? mutPerm : roPerm), newNameUseFromDclNode((INode*)st, (INode*)clo));
    self->scope = 1;
    self->index = 0;
    self->flowtempflags |= VarInitialized;
    info->selfparm = self;
    nodesInsert(&sig->parms, (INode*)self, 0);
    uint16_t parmnbr = 0;
    for (nodesFor(sig->parms, cnt, nodesp))
        ((VarDclNode*)*nodesp)->index = parmnbr++;
    FnDclNode *fn = newFnDclNode(plan->method, FlagMethFld | FlagPub, (INode*)sig, clo->body);
    inodeLexCopy((INode*)fn, (INode*)clo);
    fn->closure = info;
    iNsTypeAddFn((INsTypeNode*)st, fn);

    if (final)
        nodesAdd(&mod->nodes, (INode*)st);
    INode *stnode = (INode*)st;
    inodeTypeCheckAny(pstate, &stnode);
    return st;
}

// Does the closure, under this guess, check? Tried on a copy of the literal,
// its diagnostics unprinted and uncounted.
static int closureTry(TypeCheckState *pstate, ClosureNode *tmpl, ClosurePlan *plan, INode **statetypes,
        FnSigNode *exsig) {
    int svErrors = errors;
    int svWarnings = warnings;
    errorSilent++;
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
    ClosureNode *copy = (ClosureNode*)cloneNode(&cstate, (INode*)tmpl);
    clonePopState();
    cloneDclPop(dclpos);
    closureBuild(pstate, copy, plan, statetypes, exsig, 0);
    int ok = errors == svErrors;
    errorSilent--;
    errors = svErrors;
    warnings = svWarnings;
    return ok;
}

// ---------------------------------------------------------------------------
// The signature a closure is given to

// The signature the position a closure is written in expects, or NULL; '*isref'
// is set where it is a function reference's, '&fn(...)'
static FnSigNode *closureExpectedSig(INode *expected, int *isref) {
    *isref = 0;
    if (closureHint) {
        FnSigNode *hint = closureHint;
        closureHint = NULL;
        return hint;
    }
    if (expected == NULL || expected == unknownType || expected == noCareType || !isTypeNode(expected))
        return NULL;
    INode *dcl = itypeGetTypeDcl(expected);
    if (dcl->tag == RefTag) {
        INode *target = itypeGetTypeDcl(((RefNode*)dcl)->vtexp);
        if (target->tag == FnSigTag) {
            *isref = 1;
            return (FnSigNode*)target;
        }
        return NULL;
    }
    return dcl->tag == FnSigTag ? (FnSigNode*)dcl : NULL;
}

// ---------------------------------------------------------------------------
// Type check

void closureTypeCheck(TypeCheckState *pstate, ClosureNode **nodep, INode *expected) {
    ClosureNode *clo = *nodep;
    INode **nodesp;
    uint32_t cnt;
    if (pstate->fn == NULL) {
        errorMsgNode((INode*)clo, ErrorClosureForm,
            "A closure is written inside a function: it borrows the variables around it and holds state, neither of which a global has.");
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }
    int isref;
    // The method of a trait this literal fills, when it was given where one is wanted
    Name *method = closureHint ? closureMethod : NULL;
    closureMethod = NULL;
    FnSigNode *exsig = closureExpectedSig(expected, &isref);
    char methtext[300] = "";
    if (method && method != parensName)
        snprintf(methtext, sizeof(methtext), "the trait's `%s`", &method->namestr);

    // The parameters take their types from the signature wanted where they are not written
    if (exsig && exsig->parms->used != clo->sig->parms->used) {
        errorMsgNode((INode*)clo, ErrorClosureParm,
            "This closure takes %u parameter%s, and %s takes %u.",
            clo->sig->parms->used, clo->sig->parms->used == 1 ? "" : "s",
            methtext[0] ? methtext : "the signature it is given to", exsig->parms->used);
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }
    uint32_t pi = 0;
    for (nodesFor(clo->sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode*)*nodesp;
        // A type written for a trait's method is the method's, exactly
        if (parm->vtype != unknownType && methtext[0] && exsig
            && !itypeIsSame(parm->vtype, ((VarDclNode*)nodesGet(exsig->parms, pi))->vtype)) {
            errorMsgNode((INode*)parm, ErrorClosureParm,
                "%s takes %s there, and this closure writes another type for %s.",
                methtext, itypeName(((VarDclNode*)nodesGet(exsig->parms, pi))->vtype), &parm->namesym->namestr);
            *nodep = (ClosureNode*)newErrorNode((INode*)clo);
            return;
        }
        if (parm->vtype == unknownType) {
            if (exsig)
                parm->vtype = ((VarDclNode*)nodesGet(exsig->parms, pi))->vtype;
            else {
                errorMsgNode((INode*)parm, ErrorClosureParm,
                    "The type of %s is not written, and nothing here says what it is. Write it, as in 'fn (%s i32) { ... }', or give the closure to a parameter whose signature does.",
                    &parm->namesym->namestr, &parm->namesym->namestr);
                *nodep = (ClosureNode*)newErrorNode((INode*)clo);
                return;
            }
        }
        ++pi;
    }
    if (methtext[0] && exsig && clo->sig->rettype != unknownType && !itypeIsSame(clo->sig->rettype, exsig->rettype)) {
        errorMsgNode((INode*)clo, ErrorClosureParm,
            "%s returns %s, and this closure is written to return another type.", methtext, itypeName(exsig->rettype));
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }

    // On a GPU a function reference is a pointer to code, which it has none of;
    // the closure is taken by a generic bound by its signature instead
    if (isref && flowGpu) {
        errorMsgNode((INode*)clo, ErrorGpuClosureRef,
            "A closure in GPU code cannot be made a function reference, '&fn(...)': a GPU has no pointers to code. Give it to a function generic over its signature, '[F fn(...)]', which is inlined.");
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }

    // A closure that holds or borrows something is not a function
    if (isref && (clo->state->used || clo->captures->used)) {
        if (clo->captures->used)
            errorMsgNode((INode*)clo, ErrorClosureForm,
                "This closure borrows %s, so it is not a function and cannot be given where a function reference, '&fn(...)', is wanted: only a closure that holds and borrows nothing is one.",
                &((VarDclNode*)nodesGet(clo->captures, 0))->namesym->namestr);
        else
            errorMsgNode((INode*)clo, ErrorClosureForm,
                "This closure holds state of its own, so it is not a function and cannot be given where a function reference, '&fn(...)', is wanted: only a closure that holds and borrows nothing is one.");
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }

    // A closure that holds and borrows nothing is a function. Given where a
    // function reference is wanted, it is one.
    if (isref) {
        ModuleNode *mod = dclInfoGetModule((INode*)pstate->fn);
        FnSigNode *sig = clo->sig;
        if (sig->rettype == unknownType)
            sig->rettype = exsig->rettype;
        FnDclNode *fn = newFnDclNode(NULL, 0, (INode*)sig, clo->body);
        inodeLexCopy((INode*)fn, (INode*)clo);
        nodesAdd(&mod->nodes, (INode*)fn);
        dclInfoJoin((INode*)fn, (INode*)mod);
        INode *fnnode = (INode*)fn;
        inodeTypeCheckAny(pstate, &fnnode);
        NameUseNode *use = newNameUseNode(anonName);
        inodeLexCopy((INode*)use, (INode*)clo);
        use->dclnode = (INode*)fn;
        use->vtype = fn->vtype;
        RefNode *ref = newRefNodeFull(BorrowTag, (INode*)clo, borrowRef, unknownType, (INode*)use);
        INode *refnode = (INode*)ref;
        inodeTypeCheckAny(pstate, &refnode);
        *((INode**)nodep) = refnode;
        return;
    }

    // The state's values are checked where the closure is made, and give the
    // types of its fields. A copy of the literal as it stands is kept to try.
    ClosureNode *tmpl;
    {
        CloneState cstate;
        uint32_t dclpos = cloneDclPush();
        clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
        tmpl = (ClosureNode*)cloneNode(&cstate, (INode*)clo);
        clonePopState();
        cloneDclPop(dclpos);
    }
    INode **statetypes = memAllocBlk((clo->state->used ? clo->state->used : 1) * sizeof(INode*));
    uint32_t k = 0;
    for (nodesFor(clo->state, cnt, nodesp)) {
        VarDclNode *ent = (VarDclNode*)*nodesp;
        if (!iexpTypeCheckCoerce(pstate, unknownType, &ent->value) || !isExpNode(ent->value) || inodeIsError(ent->value)) {
            *nodep = (ClosureNode*)newErrorNode((INode*)clo);
            return;
        }
        statetypes[k++] = ((IExpNode*)ent->value)->vtype;
    }
    for (nodesFor(clo->captures, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode*)*nodesp;
        if (var->vtype == unknownType) {
            errorMsgNode((INode*)clo, ErrorClosureForm,
                "The closure names %s, whose type is not known at this point.", &var->namesym->namestr);
            *nodep = (ClosureNode*)newErrorNode((INode*)clo);
            return;
        }
    }

    // In GPU code a closure holds only what a GPU has. Refused here, and built
    // all the same: the closure's value is one the rest of the call can use
    if (flowGpu)
        closureGpuCheck(clo, statetypes);

    // What the body does with what it names decides the permissions
    uint32_t ncap = clo->captures->used;
    ClosurePlan plan;
    plan.method = method ? method : parensName;
    plan.selfmut = 0;
    plan.capmut = memAllocBlk((ncap ? ncap : 1) * sizeof(uint8_t));
    memset(plan.capmut, 0, ncap ? ncap : 1);
    if (!closureTry(pstate, tmpl, &plan, statetypes, exsig)) {
        // The most any borrow allows; if the body fails with it, the failure is its own
        plan.selfmut = 1;
        uint32_t i = 0;
        for (nodesFor(clo->captures, cnt, nodesp))
            plan.capmut[i++] = (uint8_t)closureVarMut((VarDclNode*)*nodesp);
        if (closureTry(pstate, tmpl, &plan, statetypes, exsig)) {
            // Take back each borrow the body does not need
            for (i = 0; i < ncap; ++i) {
                if (!plan.capmut[i])
                    continue;
                plan.capmut[i] = 0;
                if (!closureTry(pstate, tmpl, &plan, statetypes, exsig))
                    plan.capmut[i] = 1;
            }
            int anymut = 0;
            for (i = 0; i < ncap; ++i)
                anymut |= plan.capmut[i];
            if (!anymut) {
                plan.selfmut = 0;
                if (!closureTry(pstate, tmpl, &plan, statetypes, exsig))
                    plan.selfmut = 1;
            }
        }
    }

    int errorsBefore = errors;
    StructNode *st = closureBuild(pstate, clo, &plan, statetypes, exsig, 1);
    // A closure whose body is refused has no value: what uses it is not judged
    if (errors != errorsBefore) {
        *nodep = (ClosureNode*)newErrorNode((INode*)clo);
        return;
    }

    // The literal is the construction of the struct: its state's values, and a
    // borrow of each variable the body names
    FnCallNode *call = newFnCallNode(newNameUseFromDclNode((INode*)st, (INode*)clo), clo->state->used + ncap);
    inodeLexCopy((INode*)call, (INode*)clo);
    call->flags |= FlagNew;
    if (call->args == NULL)
        call->args = newNodes(1);
    for (nodesFor(clo->state, cnt, nodesp))
        nodesAdd(&call->args, ((VarDclNode*)*nodesp)->value);
    uint32_t i = 0;
    for (nodesFor(clo->captures, cnt, nodesp)) {
        VarDclNode *var = (VarDclNode*)*nodesp;
        NameUseNode *use = newNameUseNode(var->namesym);
        inodeLexCopy((INode*)use, (INode*)clo);
        use->dclnode = (INode*)var;
        INode *borrow = (INode*)newRefNodeFull(BorrowTag, (INode*)clo, borrowRef,
            (INode*)newPermUseNode(plan.capmut[i++] ? mutPerm : roPerm), (INode*)use);
        inodeTypeCheck(pstate, &borrow, unknownType);
        nodesAdd(&call->args, borrow);
    }
    INode *callnode = (INode*)call;
    typeLitNewArgsChecked(pstate, (FnCallNode**)&callnode);
    *((INode**)nodep) = callnode;
}

// ---------------------------------------------------------------------------
// Reading the return type off the paths

// A statement that changes something, as the last of a closure's body, is the
// change and not a value to hand back: 'total += x;', 'n++;', 'a = b;', 'a <=> b;'
static int closureIsChange(INode *stmt) {
    return stmt->tag == AssignTag || stmt->tag == SwapTag
        || (stmt->tag == FnCallTag && (stmt->flags & FlagLvalOp));
}

void closureImplicitReturn(FnDclNode *fn) {
    BlockNode *blk = (BlockNode*)fn->value;
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    INode *last = blk->stmts->used ? nodesLast(blk->stmts) : NULL;
    if (last && last->tag == ReturnTag)
        return;
    if (last && isExpOrMacroNode(last) && !closureIsChange(last)) {
        nodesLast(blk->stmts) = (INode*)newReturnNodeExp(last);
        return;
    }
    // Nothing to hand back: it returns nothing
    fn->closure->retset = 1;
    fnImplicitReturn(sig->rettype, blk);
}

void closureReturnTypeCheck(TypeCheckState *tstate, BreakRetNode *retnode) {
    FnDclNode *fn = tstate->fn;
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    ClosureInfo *info = fn->closure;
    if (!info->retset) {
        info->retset = 1;
        closureInferring = 1;
        int typed = iexpTypeCheckCoerce(tstate, unknownType, &retnode->exp);
        closureInferring = 0;
        if (!typed) {
            errorMsgNode((INode*)retnode, ErrorInvType, "The closure's return value does not have a type.");
            return;
        }
        if (isExpNode(retnode->exp) && !inodeIsError(retnode->exp)) {
            INode *type = ((IExpNode*)retnode->exp)->vtype;
            if (itypeGetTypeDcl(type)->tag != VoidTag)
                sig->rettype = type;
        }
    }
    else {
        closureInferring = 1;
        inodeTypeCheck(tstate, &retnode->exp, sig->rettype);
        closureInferring = 0;
        if (!fnCallIsNever(retnode->exp) && !iexpCheckedCoerce(sig->rettype, &retnode->exp))
            errorMsgNode((INode*)info->lit, ErrorClosureRet,
                "The paths of this closure give different types, so its return type cannot be read off them: write the closure's return type, as in 'fn (x i32) i32 { ... }'.");
    }
    returnJoinFn(tstate, retnode);
}
