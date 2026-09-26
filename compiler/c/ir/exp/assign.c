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

// Name resolution for assignment node
void assignNameRes(NameResState *pstate, AssignNode *node) {
    inodeNameRes(pstate, &node->lval);
    inodeNameRes(pstate, &node->rval);
}

// Type check a single matched assignment between lval and rval
// - lval must be a lval
// - rval's type must coerce to lval's type
void assignSingleCheck(TypeCheckState *pstate, INode *lval, INode **rval) {
    // '_' named lval need not be checked. It is a placeholder that just swallows a value
    if (isNameUseNode(lval) && isExpNode(lval) && ((NameUseNode*)lval)->namesym == anonName)
        return;

    if (iexpIsLvalError(lval) == 0) {
        return;
    }
    if (iexpTypeCheckCoerce(pstate, ((IExpNode*)lval)->vtype, rval) == 0) {
        errorMsgNode(*rval, ErrorInvType, "Expression's type does not match lval's type");
        return;
    }
}

// Type check the rvals no lval receives, and give the rval tuple its type.
//
// More values than lvals is accepted: the extra values are still evaluated,
// and the assignment's value is the whole rval tuple. So each extra needs a
// type of its own, and the tuple's type has to list every value. Before this,
// nothing type checked an extra at all -- an ill-typed one crashed the
// compiler, and 'x = 1, 2' crashed it with a tuple that had no type.
// 'received' is how many leading values were already checked against lvals.
static void assignExtraRvalsCheck(TypeCheckState *pstate, TupleNode *rval, uint32_t received) {
    TupleNode *ttuple = newTupleNode(rval->elems->used);
    ttuple->tag = TTupleTag;
    INode **nodesp;
    uint32_t cnt;
    uint32_t index = 0;
    for (nodesFor(rval->elems, cnt, nodesp)) {
        if (index++ >= received && iexpTypeCheckAny(pstate, nodesp) == 0)
            continue;
        nodesAdd(&ttuple->elems, ((IExpNode *)*nodesp)->vtype);
    }
    rval->vtype = (INode *)ttuple;
}

// Handle parallel assignment (multiple values on both sides)
void assignParaCheck(TypeCheckState *pstate, TupleNode *lval, TupleNode *rval) {
    Nodes *lnodes = lval->elems;
    Nodes *rnodes = rval->elems;
    if (lnodes->used > rnodes->used) {
        errorMsgNode((INode*)rval, ErrorBadTerm, "Not enough tuple values given to lvals");
        return;
    }
    uint32_t lcnt;
    INode **lnodesp;
    INode **rnodesp = &nodesGet(rnodes, 0);
    uint32_t rcnt = rnodes->used;
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        assignSingleCheck(pstate, *lnodesp, rnodesp++);
        rcnt--;
    }
    // rcnt is now the number of values no lval receives
    if (rcnt > 0)
        assignExtraRvalsCheck(pstate, rval, lnodes->used);
    else
        rval->vtype = lval->vtype;
}

// Handle when single function/expression returns to multiple lval
void assignMultRetCheck(TypeCheckState *pstate, TupleNode *lval, INode **rval) {
    if (iexpTypeCheckAny(pstate, rval) == 0)
        return;;
    // Resolved, because the rval's type may be a name standing for the tuple
    // rather than the tuple itself
    INode *rtype = iexpGetTypeDcl(*rval);
    if (rtype->tag != TTupleTag) {
        errorMsgNode(*rval, ErrorBadTerm, "Not enough values for lvals");
        return;
    }
    Nodes *lnodes = lval->elems;
    Nodes *rtypes = ((TupleNode*)rtype)->elems;
    if (lnodes->used > rtypes->used) {
        errorMsgNode(*rval, ErrorBadTerm, "Not enough tuple values for lvals");
        return;
    }
    uint32_t lcnt;
    INode **lnodesp;
    INode **rtypep = &nodesGet(rtypes, 0);
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        if (iexpIsLvalError(*lnodesp) == 0)
            continue;
        if (itypeIsSame(((IExpNode *)*lnodesp)->vtype, *rtypep++) == 0)
            errorMsgNode(*lnodesp, ErrorInvType, "Return value's type does not match lval's type");
    }
}

// Handle when multiple expressions assigned to single lval
// - the lval receives the first; the rest are evaluated and stored nowhere
void assignToOneCheck(TypeCheckState *pstate, INode *lval, TupleNode *rval) {
    assignSingleCheck(pstate, lval, &nodesGet(rval->elems, 0));
    assignExtraRvalsCheck(pstate, rval, 1);
}

// Type checking for assignment node
void assignTypeCheck(TypeCheckState *pstate, AssignNode *node) {
    if (iexpTypeCheckAny(pstate, &node->lval) == 0)
        return;

    // Handle tuple decomposition for parallel assignment
    INode *lval = node->lval;
    if (lval->tag == VTupleTag) {
        if (node->rval->tag == VTupleTag)
            assignParaCheck(pstate, (TupleNode*)node->lval, (TupleNode*)node->rval);
        else
            assignMultRetCheck(pstate, (TupleNode*)node->lval, &node->rval);
    }
    else {
        if (node->rval->tag == VTupleTag)
            assignToOneCheck(pstate, node->lval, (TupleNode*)node->rval);
        else
            assignSingleCheck(pstate, node->lval, &node->rval);
    }
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
    // '_' named lval is a placeholder that swallows (maybe drops) a value
    int lvalIsName = isNameUseNode(lval) && isExpNode(lval);
    if (lvalIsName && ((NameUseNode*)lval)->namesym == anonName) {
        // When lval = '_' and this is an own reference, we may have a problem
        // If this assignment is supposed to return a reference, it cannot
        /*
        if (flowAliasGet(0) > 0) {
            RefNode *reftype = (RefNode *)((IExpNode*)*rval)->vtype;
            if (reftype->tag == RefTag && reftype->region == (INode*)soRegion)
                errorMsgNode((INode*)lval, ErrorMove, "This frees reference. The reference is inaccessible for use.");
        }
        */
        return 1;
    }

    // Ensure lval is either mutable or var in need of initialization or mutable.
    uint16_t lvalscope;
    INode *lvalperm;
    INode *lvalvar = iexpGetLvalInfo(lval, &lvalperm, &lvalscope);
    if (!(MayWrite & permGetFlags(lvalperm)) &&
        (!lvalIsName || ((VarDclNode*)lvalvar)->flowtempflags & VarInitialized)) {
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
        if (!(flowflags & VarInitialized) || (flowflags & (VarMoved | VarHollow)))
            lval->flags |= FlagFirstAssign;
        if ((flowflags & VarHollow) && !(flowflags & VarMoved) && hollowrel)
            *hollowrel = flowNewHollow(var);
        var->flowtempflags |= VarInitialized;
        var->flowtempflags &= 0xFFFF - (VarMoved | VarHollow);
        var->hollowed = NULL;
    }

    assignBorrowLifetimeCheck(lval, lvalscope, rtype);
    return 0;
}

// Refuse storing a value of type 'rtype' into 'lval', whose storage lives at
// 'lvalscope', when the value is a borrowed reference the lval would outlive.
// Assignment stores one way; swap stores both ways and so calls this twice.
// A slice (ArrayRefTag) borrows exactly as a single reference does, and so
// does a virtual reference (VirtRefTag), which doc/reference/refvirtref.html
// describes as a borrowed reference carrying a vtable; all three tags carry
// the same scope and are subject to the same rule.
void assignBorrowLifetimeCheck(INode *lval, uint16_t lvalscope, INode *rtype) {
    RefNode* rvaltype = (RefNode *)rtype;
    RefNode* lvaltype = (RefNode *)((IExpNode*)lval)->vtype;
    if ((rvaltype->tag == RefTag || rvaltype->tag == ArrayRefTag || rvaltype->tag == VirtRefTag)
        && (lvaltype->tag == RefTag || lvaltype->tag == ArrayRefTag || lvaltype->tag == VirtRefTag)
        && lvaltype->region == borrowRef) {
        if (lvalscope < rvaltype->scope) {
            errorMsgNode(lval, ErrorInvType, "lval outlives the borrowed reference you are storing");
        }
    }
}

// Perform data flow analysis between two single assignment nodes:
// - Lval is mutable
// - Borrowed reference lifetime is greater than its container
void assignSingleFlow(INode *lval, INode **rval) {
    // Handle lval-based data flow analysis
    HollowNode *hollowrel = NULL;
    if (assignlvalrtype(lval, ((IExpNode*)*rval)->vtype, &hollowrel))
        return;

    // Non-anonymous lval means assignment moves/copies rvalue
    // - Enforce move semantics
    // - Handle copy semantic aliasing
    flowHandleMoveOrCopy(rval);

    // The old value of a hollowed variable is released once the new one is
    // evaluated, where genlStore releases a whole one
    if (hollowrel) {
        hollowrel->exp = *rval;
        hollowrel->vtype = ((IExpNode*)*rval)->vtype;
        *rval = (INode *)hollowrel;
    }
}

// Handle parallel assignment (multiple values on both sides)
void assignParaFlow(TupleNode *lval, TupleNode *rval) {
    Nodes *lnodes = lval->elems;
    Nodes *rnodes = rval->elems;
    uint32_t lcnt;
    INode **lnodesp;
    INode **rnodesp = &nodesGet(rnodes, 0);
    uint32_t rcnt = rnodes->used;
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        assignSingleFlow(*lnodesp, rnodesp++);
        rcnt--;
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
    int anyanon = 0;
    for (nodesFor(lnodes, lcnt, lnodesp)) {
        // Need mutability check and borrowed lifetime check
        anyanon |= assignlvalrtype(*lnodesp, *rtypep++, NULL);
    }

    // The elements are moved or copied into their lvals as one value would be:
    // a move deactivates the source, a copy out of an lvalue adds a holder per
    // counted element (flowInjectRefCountAmt fills the per-element counts).
    flowHandleMoveOrCopy(rval);

    // An element swallowed by '_' is stored nowhere, so it gains no holder
    if (anyanon && (*rval)->tag == RefCountTag && ((RefCountNode *)*rval)->counts != NULL) {
        int16_t *countp = ((RefCountNode *)*rval)->counts;
        for (nodesFor(lnodes, lcnt, lnodesp)) {
            if (isNameUseNode(*lnodesp) && isExpNode(*lnodesp) && ((NameUseNode *)*lnodesp)->namesym == anonName)
                *countp = 0;
            ++countp;
        }
    }
}

// Handle when multiple expressions assigned to single lval
void assignToOneFlow(INode *lval, TupleNode *rval) {
    Nodes *rnodes = rval->elems;
    INode **rnodesp = &nodesGet(rnodes, 0);
    uint32_t rcnt = rnodes->used;
    assignSingleFlow(lval, rnodesp++);
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
            flowGateHolder(fstate, ((IExpNode*)*lvalp)->vtype);
        }
    }
    else {
        assignFlowLvalReads(fstate, &node->lval);
        flowGateHolder(fstate, ((IExpNode*)node->lval)->vtype);
    }

    // Handle tuple decomposition for parallel assignment
    INode *lval = node->lval;
    if (lval->tag == VTupleTag) {
        if (node->rval->tag == VTupleTag)
            assignParaFlow((TupleNode*)node->lval, (TupleNode*)node->rval);
        else
            assignMultRetFlow((TupleNode*)node->lval, &node->rval);
    }
    else {
        if (node->rval->tag == VTupleTag)
            assignToOneFlow(node->lval, (TupleNode*)node->rval);
        else {
            assignSingleFlow(node->lval, &node->rval);
        }
    }
}
