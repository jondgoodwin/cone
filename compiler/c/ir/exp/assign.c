/** Handling for assignment nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>

// Create a new assignment node
AssignNode *newAssignNode(int16_t assigntype, INode *lval, INode *rval) {
    AssignNode *node;
    newNode(node, AssignNode, AssignTag);
    node->vtype = unknownType;  // Type checking sets it from the lval
    node->assignType = assigntype;
    node->lval = lval;
    node->rval = rval;
    return node;
}

// Clone assign
INode *cloneAssignNode(CloneState *cstate, AssignNode *node) {
    AssignNode *newnode;
    newnode = memAllocBlk(sizeof(AssignNode));
    memcpy(newnode, node, sizeof(AssignNode));
    newnode->lval = cloneNode(cstate, node->lval);
    newnode->rval = cloneNode(cstate, node->rval);
    return (INode *)newnode;
}

// Serialize assignment node
void assignPrint(AssignNode *node) {
    inodeFprint("(=, ");
    inodePrintNode(node->lval);
    inodeFprint(", ");
    inodePrintNode(node->rval);
    inodeFprint(")");
}

// Is this target '_', which takes its value and drops it at the end of the statement? It
// names no variable, so it stays unresolved and has no type of its own: it is the type of
// the value it takes.
int assignIsAnon(INode *node) {
    return isNameUseNode(node) && ((NameUseNode*)node)->namesym == anonName;
}

// Name resolution of an assignment's targets: each one, but a '_' names nothing to resolve
static void assignTargetNameRes(NameResState *pstate, INode **targetp) {
    if (assignIsAnon(*targetp))
        return;
    if ((*targetp)->tag == TupleTag) {
        // The targets are all places: a value tuple (ttupleNameRes decides this from its elements)
        TupleNode *targets = (TupleNode*)*targetp;
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(targets->elems, cnt, elemp))
            assignTargetNameRes(pstate, elemp);
        targets->tag = VTupleTag;
        targets->vtype = unknownType;
        return;
    }
    inodeNameRes(pstate, targetp);
}

// Name resolution for assignment node
void assignNameRes(NameResState *pstate, AssignNode *node) {
    // 'let a, b = values;' declares its names after the values are resolved, so a value
    // that names an outer 'a' means that one (varDclNameRes leaves the names unhooked)
    if (node->flags & FlagDestructAssign) {
        inodeNameRes(pstate, &node->rval);
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node->lval)->elems, cnt, elemp)) {
            INode *dcl = isNameUseNode(*elemp) ? ((NameUseNode*)*elemp)->dclnode : NULL;
            if (dcl && dcl->tag == VarDclTag && (dcl->flags & FlagDestructDcl))
                varDclHook(pstate, (VarDclNode*)dcl);
        }
        assignTargetNameRes(pstate, &node->lval);
        return;
    }
    assignTargetNameRes(pstate, &node->lval);
    inodeNameRes(pstate, &node->rval);
    // In a macro's body, a store to a plain parameter is refused here
    macroChangeNameRes(node->lval, node->lval, MacroChangePlace);
}

// Type check a single matched assignment between lval and rval
// - lval must be a lval
// - rval's type must coerce to lval's type
void assignSingleCheck(TypeCheckState *pstate, INode *lval, INode **rval) {
    // '_' takes the value whatever its type: the value is checked, and nothing else
    if (assignIsAnon(lval)) {
        if (iexpTypeCheckAny(pstate, rval))
            ((IExpNode*)lval)->vtype = ((IExpNode*)*rval)->vtype;
        return;
    }

    if (iexpIsLvalError(lval) == 0) {
        return;
    }
    if (iexpTypeCheckCoerce(pstate, ((IExpNode*)lval)->vtype, rval) == 0) {
        iexpMismatchMsg(*rval, ((IExpNode*)lval)->vtype, ErrorInvType, "Expression's type does not match lval's type");
        return;
    }
}

// The message for targets that are not as many as the values they take apart
static void assignCountMsg(INode *at, uint32_t targets, uint32_t values) {
    errorMsgNode(at, ErrorTargetCount,
        "%u targets are given %u value%s. A target takes exactly one value: write one target for each, and '_' for a value you do not want.",
        targets, values, values == 1 ? "" : "s");
}

// A target's type for the tuple type of the targets: the type of a '_' is that of the value it takes
static void assignAnonTypes(TupleNode *lval, Nodes *rtypes) {
    INode **lnodesp;
    uint32_t lcnt;
    uint32_t index = 0;
    for (nodesFor(lval->elems, lcnt, lnodesp)) {
        if (assignIsAnon(*lnodesp)) {
            ((IExpNode*)*lnodesp)->vtype = nodesGet(rtypes, index);
            if (lval->vtype && lval->vtype->tag == TTupleTag && ((TupleNode*)lval->vtype)->elems->used > index)
                nodesGet(((TupleNode*)lval->vtype)->elems, index) = nodesGet(rtypes, index);
        }
        ++index;
    }
}

// Handle parallel assignment (multiple values on both sides): each target takes exactly one
// value. 'prechecked', where given, marks the values already checked (and coerced to their
// names' types) when a declaration gave its names their types (assignDestructInfer)
void assignParaCheck(TypeCheckState *pstate, TupleNode *lval, TupleNode *rval, uint8_t *prechecked) {
    Nodes *lnodes = lval->elems;
    Nodes *rnodes = rval->elems;
    if (lnodes->used != rnodes->used) {
        for (uint32_t i = 0; i < rnodes->used; ++i) {     // (the values are still checked)
            if (!prechecked || !prechecked[i])
                iexpTypeCheckAny(pstate, &nodesGet(rnodes, i));
        }
        assignCountMsg((INode*)rval, lnodes->used, rnodes->used);
        rval->vtype = errorType;
        return;
    }
    uint32_t lcnt;
    INode **lnodesp;
    INode **rnodesp = &nodesGet(rnodes, 0);
    uint32_t index = 0;
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        if (!prechecked || !prechecked[index])
            assignSingleCheck(pstate, *lnodesp, rnodesp);
        ++rnodesp;
        ++index;
    }
    // The tuple of what is taken: its values' types, as coerced to the targets'
    TupleNode *ttuple = newTupleNode(rnodes->used);
    ttuple->tag = TTupleTag;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(rnodes, cnt, nodesp))
        nodesAdd(&ttuple->elems, ((IExpNode *)*nodesp)->vtype);
    rval->vtype = (INode *)ttuple;
    assignAnonTypes(lval, ttuple->elems);
}

// Handle when single function/expression returns to multiple lval
void assignMultRetCheck(TypeCheckState *pstate, TupleNode *lval, INode **rval, int rvalchecked) {
    if (!rvalchecked && iexpTypeCheckAny(pstate, rval) == 0)
        return;
    // Resolved, because the rval's type may be a name standing for the tuple
    // rather than the tuple itself
    INode *rtype = iexpGetTypeDcl(*rval);
    Nodes *lnodes = lval->elems;
    if (rtype->tag != TTupleTag) {
        assignCountMsg(*rval, lnodes->used, 1);
        return;
    }
    Nodes *rtypes = ((TupleNode*)rtype)->elems;
    if (lnodes->used != rtypes->used) {
        assignCountMsg(*rval, lnodes->used, rtypes->used);
        return;
    }
    uint32_t lcnt;
    INode **lnodesp;
    INode **rtypep = &nodesGet(rtypes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        INode *given = *rtypep++;
        if (assignIsAnon(*lnodesp))
            continue;
        if (iexpIsLvalError(*lnodesp) == 0)
            continue;
        if (itypeIsSame(((IExpNode *)*lnodesp)->vtype, given) == 0)
            errorMsgNode(*lnodesp, ErrorInvType, "Return value's type does not match lval's type");
    }
    assignAnonTypes(lval, rtypes);
}

// Is this one tuple-typed lval given a value tuple? It receives the tuple
// whole, as a declaration's initial value is received: 'p = 5, 6' (or
// 'p = (5, 6)', the same) gives a '(i64, i64)' variable both values, each
// coerced to its element's type. Where the counts differ the tuple does not
// match the lval's type and is refused. One target of any other type given a
// list of values is refused too (assignToOneCheck): a single target takes the
// whole of what it is given. The parser cannot tell the two apart; only the
// lval's type can, so every later phase asks this rather than the rval's tag alone.
int assignOneTakesTuple(AssignNode *node) {
    return node->lval->tag != VTupleTag && node->rval->tag == VTupleTag
        && !assignIsAnon(node->lval) && isExpNode(node->lval) && iexpGetTypeDcl(node->lval)->tag == TTupleTag;
}

// Handle when a list of values is given to a single target that is not a tuple: refused,
// since a target takes the whole of what it is given and the values are no one value of its type
static void assignToOneCheck(TypeCheckState *pstate, INode *lval, INode **rvalp) {
    if (iexpIsLvalError(lval) == 0)
        return;
    uint32_t count = ((TupleNode*)*rvalp)->elems->used;
    iexpTypeCheckAny(pstate, rvalp);
    errorMsgNode(*rvalp, ErrorOneTarget,
        "%u values are given to one target, which takes the whole of what it is given and is not a tuple of them. Write a target for each value, 'a, b = ...', with '_' for each you do not want.",
        count);
}

// The names of 'let a, b = values;' that were declared with no type take the types of the
// values they are given, as a name declared with a value does. A value given to a name that
// has a type is checked against that type as any assignment is. Done ahead of the targets
// being checked, since each name has its type by then. Where the values are a list they are
// checked here, one by one, and marked in 'prechecked'; a single value is checked here and
// its tuple type read. Returns 0 where the values cannot be taken apart (reported).
static int assignDestructInfer(TypeCheckState *pstate, AssignNode *node, uint8_t *prechecked, int *rvalchecked) {
    TupleNode *targets = (TupleNode*)node->lval;
    Nodes *lnodes = targets->elems;
    *rvalchecked = 0;
    for (uint32_t i = 0; i < lnodes->used; ++i) {
        INode *use = nodesGet(lnodes, i);
        if (!assignIsAnon(use))
            ((NameUseNode*)use)->dclnode->flags &= ~FlagDestructDcl;
    }
    if (node->rval->tag == VTupleTag) {
        Nodes *rnodes = ((TupleNode*)node->rval)->elems;
        if (lnodes->used != rnodes->used)
            return 1;       // (assignParaCheck reports it)
        for (uint32_t i = 0; i < lnodes->used; ++i) {
            INode *use = nodesGet(lnodes, i);
            VarDclNode *var = (VarDclNode*)((NameUseNode*)use)->dclnode;
            if (assignIsAnon(use) || var->vtype != unknownType)
                continue;
            // The name is declared with this value, as 'let name = value;' would
            var->value = nodesGet(rnodes, i);
            varDclTypeCheck(pstate, var);
            nodesGet(rnodes, i) = var->value;
            var->value = NULL;
            prechecked[i] = 1;
        }
        return 1;
    }
    // One value, taken apart: its tuple type gives each name its type
    if (iexpTypeCheckAny(pstate, &node->rval) == 0) {
        for (uint32_t i = 0; i < lnodes->used; ++i) {
            INode *use = nodesGet(lnodes, i);
            if (!assignIsAnon(use) && ((NameUseNode*)use)->dclnode->tag == VarDclTag
                && ((VarDclNode*)((NameUseNode*)use)->dclnode)->vtype == unknownType)
                ((VarDclNode*)((NameUseNode*)use)->dclnode)->vtype = errorType;
        }
        return 0;
    }
    *rvalchecked = 1;
    INode *rtype = iexpGetTypeDcl(node->rval);
    if (rtype->tag != TTupleTag || ((TupleNode*)rtype)->elems->used != lnodes->used) {
        assignCountMsg(node->rval, lnodes->used, rtype->tag == TTupleTag ? ((TupleNode*)rtype)->elems->used : 1);
        for (uint32_t i = 0; i < lnodes->used; ++i) {
            INode *use = nodesGet(lnodes, i);
            if (!assignIsAnon(use) && ((VarDclNode*)((NameUseNode*)use)->dclnode)->vtype == unknownType)
                ((VarDclNode*)((NameUseNode*)use)->dclnode)->vtype = errorType;
        }
        return 0;
    }
    Nodes *rtypes = ((TupleNode*)rtype)->elems;
    for (uint32_t i = 0; i < lnodes->used; ++i) {
        INode *use = nodesGet(lnodes, i);
        if (assignIsAnon(use))
            continue;
        VarDclNode *var = (VarDclNode*)((NameUseNode*)use)->dclnode;
        if (var->vtype != unknownType)
            continue;
        var->vtype = nodesGet(rtypes, i);
        varDclTypeCheck(pstate, var);
    }
    return 1;
}

// '*self = T(...)' in an init, where the construction selected that same
// init (typeLitNewCheck prefers a declared init to the implicit one): filling
// self would run the init again before it ever filled it. The field-wise fill
// names the fields, which only the implicit init takes.
static void assignInitRecurse(TypeCheckState *pstate, INode *lval, INode *rval) {
    if (pstate->fn == NULL || lval->tag != DerefTag || rval->tag != FnCallTag || !(rval->flags & FlagNew))
        return;
    INode *objfn = ((FnCallNode *)rval)->objfn;
    if (!isNameUseNode(objfn) || ((NameUseNode *)objfn)->dclnode != (INode *)pstate->fn)
        return;
    INode *self = ((StarNode *)lval)->vtexp;
    while (self->tag == CastTag)
        self = ((CastNode *)self)->exp;
    Nodes *parms = ((FnSigNode *)pstate->fn->vtype)->parms;
    if (!isNameUseNode(self) || parms->used == 0 || ((NameUseNode *)self)->dclnode != nodesGet(parms, 0))
        return;
    INode *owner = inodeGetOwner((INode *)pstate->fn);
    errorMsgNode(rval, ErrorInitRecurse,
        "Filling self with a construction that runs this same init again: an init fills its value by the fields' names, '%s(field: value)'.",
        owner && owner->tag == StructTag ? &((StructNode *)owner)->namesym->namestr : "T");
}

// Is this lval, type-checked, '*self' in an init: the value the init fills?
static int assignFillsInitSelf(TypeCheckState *pstate, INode *lval) {
    FnDclNode *fn = pstate->fn;
    if (fn == NULL || lval->tag != DerefTag || (fn->namesym != initName && fn->overloadsym != initName))
        return 0;
    INode *self = ((StarNode *)lval)->vtexp;
    while (self->tag == CastTag)
        self = ((CastNode *)self)->exp;
    Nodes *parms = ((FnSigNode *)fn->vtype)->parms;
    return isNameUseNode(self) && parms->used > 0 && ((NameUseNode *)self)->dclnode == nodesGet(parms, 0);
}

// Type check the targets of an assignment. A '_' has no type until its value is known.
// Returns 0 where a target could not be checked
static int assignTargetsTypeCheck(TypeCheckState *pstate, INode **lvalp) {
    if (assignIsAnon(*lvalp)) {
        ((IExpNode*)*lvalp)->vtype = unknownType;
        return 1;
    }
    if ((*lvalp)->tag != VTupleTag) {
        // 'x[i] = v', 'x[i].f = v' and 'a[g].b[i].f = v': on a type declaring '&[]',
        // each index of the place is the element's mutable borrow (fnCallTypeCheck),
        // and the whole place is stored through, as Rust's IndexMut is
        FnCallNode *setindex = fnCallSetIndexRoot(*lvalp);
        uint32_t setmark = fnCallSetPlace(*lvalp);
        int typed = iexpTypeCheckAny(pstate, lvalp);
        fnCallSetPlaceEnd(setmark);
        if (typed == 0)
            return 0;
        if (setindex && *lvalp == (INode*)setindex && (setindex->flags & FlagBorrow))
            derefInject(lvalp);
        return 1;
    }
    // The targets of a parallel assignment, each its own place; the tuple of their types
    TupleNode *targets = (TupleNode*)*lvalp;
    TupleNode *ttuple = newTupleNode(targets->elems->used);
    ttuple->tag = TTupleTag;
    targets->vtype = (INode*)ttuple;
    int allok = 1;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(targets->elems, cnt, nodesp)) {
        if (assignTargetsTypeCheck(pstate, nodesp) == 0) {
            allok = 0;
            nodesAdd(&ttuple->elems, errorType);
            continue;
        }
        nodesAdd(&ttuple->elems, ((IExpNode*)*nodesp)->vtype);
    }
    return allok;
}

// Type checking for assignment node
void assignTypeCheck(TypeCheckState *pstate, AssignNode *node) {
    // 'let a, b = values;': the names declared with no type take their values' types first
    uint8_t *prechecked = NULL;
    int rvalchecked = 0;
    if (node->flags & FlagDestructAssign) {
        prechecked = (uint8_t*)memAllocBlk(((TupleNode*)node->lval)->elems->used + 1);
        memset(prechecked, 0, ((TupleNode*)node->lval)->elems->used + 1);
        if (!assignDestructInfer(pstate, node, prechecked, &rvalchecked))
            return;
    }
    if (assignTargetsTypeCheck(pstate, &node->lval) == 0)
        return;

    // Handle tuple decomposition for parallel assignment
    INode *lval = node->lval;
    if (lval->tag == VTupleTag) {
        if (node->rval->tag == VTupleTag)
            assignParaCheck(pstate, (TupleNode*)node->lval, (TupleNode*)node->rval, prechecked);
        else
            assignMultRetCheck(pstate, (TupleNode*)node->lval, &node->rval, rvalchecked);
    }
    else {
        if (assignOneTakesTuple(node))
            assignSingleCheck(pstate, node->lval, &node->rval);
        else if (node->rval->tag == VTupleTag && !assignIsAnon(node->lval))
            assignToOneCheck(pstate, node->lval, &node->rval);
        else {
            // The construction filling an init's self is the one an actor's
            // state may be made by outside its handle's initializer
            INode *svfill = typeLitSelfFill;
            if (node->rval->tag == FnCallTag && (node->rval->flags & FlagNew) && assignFillsInitSelf(pstate, node->lval))
                typeLitSelfFill = node->rval;
            assignSingleCheck(pstate, node->lval, &node->rval);
            typeLitSelfFill = svfill;
            assignInitRecurse(pstate, node->lval, node->rval);
        }
    }
    // The values of 'let a, b = &f(), &g();' are one initializer: a temporary a borrow in it
    // is rooted in lives to the block's end, as it does for a single name's
    if ((node->flags & FlagDestructAssign) && node->rval->tag == VTupleTag
            && ((IExpNode*)node->rval)->vtype != errorType)
        varDclExtendInit(pstate, &node->rval);
    // What the names assigned hold now: the variant stored, where it is known (narrow.h)
    if (lval->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)lval)->elems, cnt, elemp)) {
            if (!assignIsAnon(*elemp))
                narrowAssigned(*elemp, NULL);
        }
    }
    else if (!assignIsAnon(lval))
        narrowAssigned(lval, assignOneTakesTuple(node) || node->rval->tag != VTupleTag ? node->rval : NULL);
    // A store through a reference or a field may change an enum another name reaches
    if (!assignIsAnon(lval) && !(isNameUseNode(lval) && isExpNode(lval)))
        narrowCallMade((INode*)node);
    node->vtype = ((IExpNode*)node->rval)->vtype;
}

// Handle data flow analysis related to single assignment rval
// Pass type of rval so we can determine what semantics apply
// Return true if lval is anonName
// 'hollowrel', where given, receives the hollow release of a hollowed variable
// being reassigned, for the caller to wrap around the value stored; where it is
// not given, such a variable's old allocation is left unreleased, as a moved
// one's value is.
int assignlvalrtype(INode *lval, INode *rtype, HollowNode **hollowrel) {
    // '_' is no variable: it takes the value and drops it at the end of the statement
    // (genlDiscard), so nothing about a variable changes
    if (assignIsAnon(lval))
        return 1;
    int lvalIsName = isNameUseNode(lval) && isExpNode(lval);

    // Ensure lval is either mutable or var in need of initialization or mutable.
    uint16_t lvalscope;
    INode *lvalperm;
    INode *lvalvar = iexpGetStoreLvalInfo(lval, &lvalperm, &lvalscope);
    if (!(MayWrite & permGetFlags(lvalperm)) &&
        (!lvalIsName || ((VarDclNode*)lvalvar)->flowtempflags & VarInitialized)) {
        if (permIsLock(lvalperm))
            permLockRefused(lval, lvalperm, "write");
        else if (lvalvar && lvalvar->tag == VarDclTag && (lvalvar->flags & FlagEachVar))
            errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval: the variable of an 'each' is a new one every pass that nothing can change. To change the items in place, walk src.mutItems()");
        else if (lvalvar && lvalvar->tag == VarDclTag && (lvalvar->flags & FlagMacroBind))
            macroParmAssignRefused(lval, (VarDclNode*)lvalvar);
        else
            errorMsgNode(lval, ErrorNoMut, "You do not have permission to modify lval");
        return 0;
    }

    // Mark that lval variable has valid initialized value.
    if (lvalIsName) {
        // Stopgap: record on this use that the variable held nothing -- never
        // initialized, or moved out -- so code generation does not release
        // uninitialized storage or a value another owner now holds. Flow state
        // is a running summary, so only the assignment site itself can carry this.
        // A hollowed variable holds an allocation but not all of its value:
        // genlStore must not release it whole, and the hollow release takes
        // its place.
        VarDclNode *var = (VarDclNode*)lvalvar;
        uint16_t flowflags = var->flowtempflags;
        // A match's binding by value names the matched value's own storage
        // (flowMatchInPlace), so a store over it releases the value it
        // replaces, and the matched value's variable releases the new one. A
        // binding by reference holds a copy of the matched reference, which the
        // matched value's variable releases: it has nothing of its own to release
        if (!(flowflags & VarInitialized) || (flowflags & (VarMoved | VarHollow))
            || (flowMatchBound((INode *)var) && !flowMatchInPlace(var)))
            lval->flags |= FlagFirstAssign;
        if ((flowflags & VarHollow) && !(flowflags & VarMoved) && hollowrel)
            *hollowrel = flowNewHollow(var);
        flowVarSetFlags(var, (flowflags | VarInitialized) & (0xFFFF - (VarMoved | VarHollow)), NULL);
        // The value stored into such a binding is the matched value's variable's
        // to release, so that variable holds a value again, as a moved plain
        // 'var' local does once assigned
        if (flowMatchInPlace(var)) {
            VarDclNode *owner = flowDropOwner(var);
            if (owner != var)
                flowVarSetFlags(owner, (owner->flowtempflags | VarInitialized) & (0xFFFF - (VarMoved | VarHollow)), NULL);
        }
        flowDropNote(flowCurrent, var);
    }
    // A part of a local's own value -- a field, an element -- is stored over:
    // its old value is released unless the variable holds none, whose part is
    // another owner's
    // (a variable never given a value, or whose whole value was moved out, has
    // no parts to store into: the whole of it is given first, so that no part
    // is left holding a value nothing releases. A hollowed one still owns its
    // allocation, and a store there has no old value to release)
    else if (lval->tag == FldAccessTag || lval->tag == ArrIndexTag) {
        VarDclNode *root = flowLvalRootVar(lval);
        if (root && !(root->flowtempflags & VarInitialized) && !flowNewSelfVar(root))
            errorMsgNode(lval, ErrorMove, "A field or element is stored only into a variable that holds a value. Give the whole variable its value first.");
        else if (root && (root->flowtempflags & VarMoved))
            errorMsgNode(lval, ErrorMove, "A field or element is not stored into a variable whose value has been moved out. Give the whole variable a new value first.");
        // VarHollow is a guard, not a path: a hollowed owner is refused first, as a use
        else if (!flowNoDeath(((IExpNode *)lval)->vtype) && root
                && ((root->flowtempflags & VarHollow)
                    || (flowMatchBound((INode *)root) && !flowMatchInPlace(root))))
            lval->flags |= FlagPartNoPrior;
    }

    assignBorrowLifetimeCheck(lval, lvalscope, rtype);
    return 0;
}

// Is 'lval' part of a variable's own storage -- the variable itself, or a
// field or element of its value, or of what a sole owner it holds owns -- the
// variable a local or a by-value parameter? A variable's lifetime follows what
// it holds now, so a shorter borrow may be stored there: the loan walk
// (flowpath.c) refuses it only where it is used after its source ends, or
// carried out of the function.
static int assignIsLocalPlace(INode *lval) {
    while (1) {
        if (isNameUseNode(lval) && isExpNode(lval)) {
            INode *dcl = ((NameUseNode *)lval)->dclnode;
            return dcl && dcl->tag == VarDclTag && ((VarDclNode *)dcl)->scope > 0
                && !(dcl->flags & FlagStatic);
        }
        switch (lval->tag) {
        case FldAccessTag:
        case ArrIndexTag:
        {
            // A virtual reference, a reference to an array, a slice and a
            // pointer are reached through with no dereference injected
            INode *obj = ((FnCallNode *)lval)->objfn;
            uint16_t objtag = iexpGetTypeDcl(obj)->tag;
            if (objtag == RefTag || objtag == ArrayRefTag || objtag == VirtRefTag || objtag == PtrTag)
                return 0;
            lval = obj;
            break;
        }
        case DerefTag:
        {
            // Only a sole owner's referent is its holder's own
            INode *refexp = ((StarNode *)lval)->vtexp;
            RefNode *reftype = (RefNode *)iexpGetTypeDcl(refexp);
            if (reftype->tag != RefTag || itypeGetTypeDcl(reftype->region) == borrowRef
                || (permGetFlags(reftype->perm) & MayAlias))
                return 0;
            lval = refexp;
            break;
        }
        default:
            return 0;
        }
    }
}

// Refuse storing a value of type 'rtype' into 'lval', whose storage lives at
// 'lvalscope', when the value is a borrowed reference the lval would outlive.
// Assignment stores one way; swap stores both ways and so calls this twice.
// A slice (ArrayRefTag) borrows exactly as a single reference does, and so
// does a virtual reference (VirtRefTag), which doc/manual/mod-substitution.html#virtref
// describes as a borrowed reference carrying a vtable; all three tags carry
// the same scope and are subject to the same rule. A store into a variable's
// own storage is the loan walk's (assignIsLocalPlace); this refuses a store
// into a global, or through a reference into what may outlive the borrow.
void assignBorrowLifetimeCheck(INode *lval, uint16_t lvalscope, INode *rtype) {
    RefNode* rvaltype = (RefNode *)rtype;
    RefNode* lvaltype = (RefNode *)((IExpNode*)lval)->vtype;
    if ((rvaltype->tag == RefTag || rvaltype->tag == ArrayRefTag || rvaltype->tag == VirtRefTag)
        && (lvaltype->tag == RefTag || lvaltype->tag == ArrayRefTag || lvaltype->tag == VirtRefTag)
        && lvaltype->region == borrowRef && !assignIsLocalPlace(lval)) {
        if (lvalscope < rvaltype->scope) {
            errorMsgNode(lval, ErrorInvType, "lval outlives the borrowed reference you are storing");
        }
    }
}

// The target of an assignment, once the value it is given has left its source
static void assignSingleTarget(INode *lval, INode **rval) {
    HollowNode *hollowrel = NULL;
    assignlvalrtype(lval, ((IExpNode*)*rval)->vtype, &hollowrel);
    // The old value of a hollowed variable is released once the new one is
    // evaluated, where genlStore releases a whole one
    if (hollowrel) {
        hollowrel->exp = *rval;
        hollowrel->vtype = ((IExpNode*)*rval)->vtype;
        *rval = (INode *)hollowrel;
    }
}

// The value is moved or copied out of its source first, then the target is
// stored over: the value leaves before the old one goes, as generation orders
// them. So 'p = p', or 'p = if c {p} else {q}', finds 'p' empty where it moved,
// and releases nothing the new value still is.
void assignSingleFlow(INode *lval, INode **rval) {
    // The assignment moves/copies the rvalue, '_' as well as any other target:
    // - Enforce move semantics
    // - Handle copy semantic aliasing
    // A '_' takes it and drops it at the statement's end (genlDiscard)
    flowHandleMoveOrCopy(rval);
    assignSingleTarget(lval, rval);
}

// Handle parallel assignment (multiple values on both sides): every value is
// taken before any target is stored over, as generation evaluates them all
// first, so 'a, b = b, a' swaps two values that move and releases neither
void assignParaFlow(TupleNode *lval, TupleNode *rval) {
    Nodes *lnodes = lval->elems;
    Nodes *rnodes = rval->elems;
    uint32_t lcnt;
    INode **lnodesp;
    INode **rnodesp = &nodesGet(rnodes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        flowHandleMoveOrCopy(rnodesp);
        ++rnodesp;
    }
    rnodesp = &nodesGet(rnodes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        assignSingleTarget(*lnodesp, rnodesp);
        ++rnodesp;
    }
}

// Handle when single function/expression returns to multiple lval
void assignMultRetFlow(TupleNode *lval, INode **rval) {
    Nodes *lnodes = lval->elems;
    // Resolved, because the rval's type may be a name standing for the tuple
    // rather than the tuple itself
    Nodes *rtypes = ((TupleNode*)iexpGetTypeDcl(*rval))->elems;
    uint32_t lcnt;
    INode **lnodesp;
    INode **rtypep = &nodesGet(rtypes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        // Need mutability check and borrowed lifetime check
        assignlvalrtype(*lnodesp, *rtypep++, NULL);
    }

    // The elements are moved or copied into their lvals as one value would be:
    // a move deactivates the source, a copy out of an lvalue adds a holder per
    // counted element (flowInjectRefCountAmt fills the per-element counts). An
    // element a '_' takes is moved or counted like the rest, and dropped at the
    // statement's end (genlDiscard)
    flowHandleMoveOrCopy(rval);
}

// Perform data flow analysis on assignment node
// - lval needs to be mutable.
// - borrowed reference lifetimes must exceed lifetime of lval
// Read the value sub-expressions of an assignment's target.
//
// Assignment reads its right side. Its left side is a target -- but parts of a
// target are values in their own right: the index an element is chosen by, and
// the reference a dereference writes through. Neither was read, so an
// uninitialized index went unnoticed where the identical expression on the
// right was caught, and '*p = 5' through an uninitialized reference compiled
// and emitted the store. swapFlow has always read both of its sides in full,
// so assignment and swap disagreed about the same expression.
//
// The *base* of a partial write is deliberately not read here. Whether
// 'a[i] = 5' reads 'a' is a question about read-modify-write against flow's
// whole-variable tracking, and it is not settled.
static void assignFlowLvalReads(FlowState *fstate, INode **lvalp) {
    switch ((*lvalp)->tag) {
    case ArrIndexTag: {
        FnCallNode *index = (FnCallNode *)*lvalp;
        assignFlowLvalReads(fstate, &index->objfn);
        flowLoadValue(fstate, &nodesGet(index->args, 0));
        break;
    }
    case DerefTag:
        // Through an init's 'self', once filled (flowNewSelf)
        if (flowNewSelf(((StarNode *)*lvalp)->vtexp))
            flowNewSelfThrough(fstate, &((StarNode *)*lvalp)->vtexp);
        else
            flowLoadValue(fstate, &((StarNode *)*lvalp)->vtexp);
        break;
    case FldAccessTag:
        assignFlowLvalReads(fstate, &((FnCallNode *)*lvalp)->objfn);
        break;
    default:
        break;
    }
}

void assignFlow(FlowState *fstate, AssignNode **nodep) {
    AssignNode *node = *nodep;

    flowLoadValue(fstate, &node->rval);

    if (node->lval->tag == VTupleTag) {
        INode **lvalp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node->lval)->elems, cnt, lvalp)) {
            assignFlowLvalReads(fstate, lvalp);
            flowGateAssigned(fstate, *lvalp);
            flowGateShapeStore(fstate, *lvalp);
        }
    }
    else {
        // '*self = value' in an init fills self: no value is there to read,
        // nor to finalize as it is replaced
        if (!flowNewSelfFill(node->lval))
            assignFlowLvalReads(fstate, &node->lval);
        flowGateAssigned(fstate, node->lval);
        flowGateShapeStore(fstate, node->lval);
    }

    // Handle tuple decomposition for parallel assignment
    INode *lval = node->lval;
    if (lval->tag == VTupleTag) {
        if (node->rval->tag == VTupleTag)
            assignParaFlow((TupleNode*)node->lval, (TupleNode*)node->rval);
        else
            assignMultRetFlow((TupleNode*)node->lval, &node->rval);
    }
    else
        assignSingleFlow(node->lval, &node->rval);
}
