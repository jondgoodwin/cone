/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>

// Is this expression a borrowed reference -- one that does not own what it
// points at? Its permission does not matter: a '&uni' is the only path to its
// value while it lives, but the value still belongs to the place it borrows.
static int flowIsBorrowedRef(INode *exp) {
    RefNode *reftype = (RefNode *)iexpGetTypeDcl(exp);
    return (reftype->tag == RefTag || reftype->tag == ArrayRefTag || reftype->tag == VirtRefTag)
        && itypeGetTypeDcl(reftype->region) == borrowRef;
}

// Is this expression a shared owner -- an owning reference that other holders
// may be sharing? An owning reference that may be aliased ('+rc-mut', '+rc-imm',
// '+rc-ro', and every 'rc' form but '+rc-uni') is one of possibly many holders
// counting the same value, so it does not solely own it. An owning reference
// that is a move type -- a 'uni' one, or any in the '@move' region 'so' -- is
// the only holder, and may give the value up.
static int flowIsSharedOwner(INode *exp) {
    INode *reftype = iexpGetTypeDcl(exp);
    return (reftype->tag == RefTag || reftype->tag == ArrayRefTag || reftype->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)reftype)->region) != borrowRef
        && !itypeIsMove(reftype);
}

// Refuse a move out through a reference that does not solely own what it points
// at. Returns 1 when it refused.
static int flowRefuseMoveThrough(INode *node, INode *ref) {
    if (flowIsBorrowedRef(ref)) {
        errorMsgNode(node, ErrorMoveOut, "May not move a value out through a borrowed reference, which does not own it.");
        return 1;
    }
    if (flowIsSharedOwner(ref)) {
        errorMsgNode(node, ErrorMoveOut, "May not move a value out through a shared owning reference, which does not solely own it.");
        return 1;
    }
    return 0;
}

// Add a variable to a list of the variables a move leaves without their value
static void flowAddMoved(Nodes **moved, INode *vardcl) {
    INode **nodesp;
    uint32_t cnt;
    if (*moved == NULL)
        *moved = newNodes(4);
    for (nodesFor(*moved, cnt, nodesp)) {
        if (*nodesp == vardcl)
            return;
    }
    nodesAdd(moved, vardcl);
}

static void flowMoveSource(INode *node, Nodes **moved);

// Walk one of the values a block or an 'if' may hand back, narrowing 'common'
// to the variables that every value walked so far moves out of ('first' says
// none has been walked yet). With 'moved' NULL the walk only checks.
static void flowMoveExit(INode *exp, Nodes **moved, Nodes **common, int *first) {
    Nodes *these = NULL;
    if (iexpIsMove(exp))
        flowMoveSource(exp, moved ? &these : NULL);
    if (moved == NULL)
        return;
    if (*first) {
        *first = 0;
        *common = these;
        return;
    }
    Nodes *kept = NULL;
    if (*common != NULL && these != NULL) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(*common, cnt, nodesp)) {
            INode **thesep;
            uint32_t thesecnt;
            for (nodesFor(these, thesecnt, thesep)) {
                if (*thesep == *nodesp)
                    flowAddMoved(&kept, *nodesp);
            }
        }
    }
    *common = kept;
}

// Walk inwards from a moved value to its source: refuse a move out of a place
// that does not own the value, and, when 'moved' is given, add to it each
// source variable the move leaves without its value. A value reached through a
// borrowed reference still belongs to what was borrowed, and one reached through
// a shared owning reference still belongs to its other holders, so moving it out
// would leave two owners of one value.
static void flowMoveSource(INode *node, Nodes **moved) {
    // For a variable, its value is what moves
    if (isNameUseNode(node) && isExpNode(node)) {
        VarDclNode *vardclnode = (VarDclNode *)((NameUseNode*)node)->dclnode;
        if (moved)
            flowAddMoved(moved, (INode *)vardclnode);
        if (vardclnode->scope == 0) {
            errorMsgNode(node, ErrorInvType, "May not move a value out of a global variable.");
        }
        return;
    }
    switch (node->tag) {
    // Go inwards to find the variable to mark it as moved. A field or element
    // read straight through a reference -- a slice or a virtual reference, which
    // take no injected dereference -- is read through that reference.
    case FldAccessTag:
    case ArrIndexTag:
    {
        INode *objfn = ((FnCallNode*)node)->objfn;
        if (flowRefuseMoveThrough(node, objfn))
            return;
        flowMoveSource(objfn, moved);
        break;
    }
    case DerefTag:
    {
        INode *ref = ((StarNode*)node)->vtexp;
        if (flowRefuseMoveThrough(node, ref))
            return;
        flowMoveSource(ref, moved);
        break;
    }

    // A recast is its operand under another type name -- an enrichment and its
    // base, which share one representation -- so moving it moves the operand
    case CastTag:
        if (!(node->flags & FlagConvert))
            flowMoveSource(((CastNode*)node)->exp, moved);
        break;

    // A tuple literal has no storage of its own: its sources are its elements,
    // and only a move-typed element is moved out of, the rest are copied
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node)->elems, cnt, nodesp)) {
            if (iexpIsMove(*nodesp))
                flowMoveSource(*nodesp, moved);
        }
        break;
    }

    // A block's value is what it hands back: its final expression and the value
    // of each break that leaves it. A 'return' leaves the function, not the block,
    // and blockFlow checks it there. A loop's final expression is not one of
    // them: it loops back. Where each value came from is checked, and a variable
    // is moved out of only when every value the block can hand back moves it.
    // One moved on only some of them is a conditional move, which is left as it
    // was: moved-ness is kept per function, not per path, so marking it would
    // leak it on the paths that leave it in place.
    case BlockTag:
    {
        BlockNode *blk = (BlockNode *)node;
        Nodes *common = NULL;
        int first = 1;
        INode **nodesp;
        uint32_t cnt;
        INode *last = blk->stmts->used > 0 ? nodesLast(blk->stmts) : NULL;
        if (last != NULL && last->tag == BlockRetTag) {
            if (blk->flags & FlagLoop)
                flowResultMove(((BreakRetNode *)last)->exp);
            else
                flowMoveExit(((BreakRetNode *)last)->exp, moved, &common, &first);
        }
        if (blk->breaks) {
            for (nodesFor(blk->breaks, cnt, nodesp)) {
                if ((*nodesp)->tag == BreakTag)
                    flowMoveExit(((BreakRetNode *)*nodesp)->exp, moved, &common, &first);
            }
        }
        if (moved && common) {
            for (nodesFor(common, cnt, nodesp))
                flowAddMoved(moved, *nodesp);
        }
        break;
    }
    // An 'if' is the value of whichever branch block runs, so it moves out of
    // what every branch moves out of, as a block does
    case IfTag:
    {
        Nodes *common = NULL;
        int first = 1;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((IfNode *)node)->condblk, cnt, nodesp)) {
            nodesp++; cnt--;
            flowMoveExit(*nodesp, moved, &common, &first);
        }
        if (moved && common) {
            for (nodesFor(common, cnt, nodesp))
                flowAddMoved(moved, *nodesp);
        }
        break;
    }

    // For any other node, no source variable to mark as moved
    default:
        break;
    }
}

// Deactivate source of a moved value (or say move is illegal)
void flowHandleMove(INode *node) {
    Nodes *moved = NULL;
    INode **nodesp;
    uint32_t cnt;
    flowMoveSource(node, &moved);
    if (moved) {
        for (nodesFor(moved, cnt, nodesp))
            ((VarDclNode *)*nodesp)->flowtempflags |= VarMoved;
    }
}

// Refuse a move-typed value a scope hands back -- a return's or a block's
// result -- when its source does not own it. The source is not deactivated
// here: a local handed back is exempted from the scope's release by
// flowScopeDealias instead.
void flowResultMove(INode *node) {
    if (iexpIsMove(node))
        flowMoveSource(node, NULL);
}

// Is this type a counted reference: one into a region whose 'alias' is called
// for each copy that becomes another owner? An owning slice (ArrayRefTag) is
// counted exactly as a single reference is.
int flowIsRcRef(INode *type) {
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(type);
    return (reftype->tag == RefTag || reftype->tag == ArrayRefTag) && regionIsCounted(reftype->region);
}

// Does a variable of this type hold something its scope must release: an
// owning reference into a region, single or slice, or a tuple carrying one?
int flowIsOwningType(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag) {
        RefNode *reftype = (RefNode *)typedcl;
        return regionIsOwning(reftype->region);
    }
    if (typedcl->tag == TTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, elemp)) {
            if (flowIsOwningType(*elemp))
                return 1;
        }
    }
    return 0;
}

// If needed, inject a reference-count node for rc/own references, adjusting the count by amt.
// One value can become more than one holder at once: an array fill literal stores
// the reference it evaluates once into every one of its elements.
void flowInjectRefCountAmt(INode **nodep, int16_t amt) {
    INode *vtype = ((IExpNode*)*nodep)->vtype;
    INode *typedcl = itypeGetTypeDcl(vtype);
    int16_t *counts = NULL;
    if (typedcl->tag == TTupleTag) {
        // A tuple value is one holder of each counted reference it carries, so
        // every rc element gets the adjustment and every other element none.
        Nodes *elems = ((TupleNode *)typedcl)->elems;
        counts = (int16_t *)memAllocBlk(elems->used * sizeof(int16_t));
        int16_t *countp = counts;
        int anycounted = 0;
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(elems, cnt, elemp)) {
            *countp = flowIsRcRef(*elemp) ? amt : 0;
            anycounted |= *countp++;
        }
        if (!anycounted)
            return;
        amt = (int16_t)elems->used;
    }
    // No need for injected node if we are not dealing with rc references
    else if (!flowIsRcRef(vtype))
        return;

    // Inject the reference-count node
    RefCountNode *rcnode;
    newNode(rcnode, RefCountNode, RefCountTag);
    rcnode->exp = *nodep;
    rcnode->vtype = vtype;
    rcnode->amt = amt;
    rcnode->counts = counts;
    *nodep = (INode*)rcnode;
}

// If needed, inject a reference-count node for rc/own references, adding one holder
void flowInjectRefCount(INode **nodep) {
    flowInjectRefCountAmt(nodep, 1);
}

// Handle when we know we are either copying or moving a value
// (e.g., for assignment or function arguments).
// Does this expression still hold its value after it is read?
// An lvalue names storage that keeps it; anything else is a temporary.
int flowIsLvalRead(INode *node) {
    if (isNameUseNode(node) && isExpNode(node))
        return 1;
    switch (node->tag) {
    case DerefTag:
    case ArrIndexTag:
    case FldAccessTag:
        return 1;
    // A recast reads its operand, and holds only what the operand holds
    case CastTag:
        return !(node->flags & FlagConvert) && flowIsLvalRead(((CastNode*)node)->exp);
    default:
        return 0;
    }
}

void flowHandleMoveOrCopy(INode **nodep) {
    if (iexpIsMove(*nodep)) {
        // Moving needs to deactivate source variable use
        flowHandleMove(*nodep);
    }
    else {
        // A reference count is how many holders exist. Only an lvalue still
        // holds its reference afterwards, so only an lvalue adds a holder. A
        // temporary -- an allocation, a call's result, a literal -- hands over
        // the reference it was born holding, and counting that again would
        // count one holder twice.
        if (flowIsLvalRead(*nodep))
            flowInjectRefCount(nodep);
    }
}


// Load a reference that a value is about to be read through, and refuse the
// read when the reference's permission grants none. The reference's own
// permission governs what may be done through it, whatever the permission of
// the binding that holds it -- the read-side twin of the MayWrite test in
// assignlvalrtype. A pointer carries no permission and is not checked here.
void flowLoadThroughRef(FlowState *fstate, INode **refp) {
    flowLoadValue(fstate, refp);
    RefNode *reftype = (RefNode *)iexpGetTypeDcl(*refp);
    if ((reftype->tag == RefTag || reftype->tag == ArrayRefTag || reftype->tag == VirtRefTag)
        && !(permGetFlags(reftype->perm) & MayRead))
        errorMsgNode(*refp, ErrorNoRead, "This reference's permission does not allow reading the value it points to");
}

// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that any expression node holds an accessible, "readable" value
void flowLoadValue(FlowState *fstate, INode **nodep) {
    // Handle specific nodes here - lvals (read check) + literals + fncall
    // fncall + literals? do not need copy check - it can return
    if (isNameUseNode(*nodep) && isExpNode(*nodep)) {
        nameuseFlow(fstate, (NameUseNode**)nodep);
        return;
    }
    switch ((*nodep)->tag) {
    case BlockTag:
        blockFlow(fstate, (BlockNode **)nodep); break;
    case IfTag:
        ifFlow(fstate, (IfNode **)nodep); break;
    case AssignTag:
        assignFlow(fstate, (AssignNode **)nodep); break;
    case FnCallTag:
        fnCallFlow(fstate, (FnCallNode**)nodep);
        break;
    case ArrayBorrowTag:
    case BorrowTag:
        borrowFlow(fstate, (RefNode **)nodep);
        break;
    case ArrayAllocTag:
    case AllocateTag:
        allocateFlow(fstate, (RefNode **)nodep);
        break;
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        uint32_t index = 0;
        for (nodesFor(((TupleNode *)*nodep)->elems, cnt, nodesp)) {
            flowLoadValue(fstate, nodesp);
        }
        break;
    }
    case DerefTag:
        derefFlow(fstate, (StarNode**)nodep);
        break;
    case ArrIndexTag:
        fnCallArrIndexFlow(fstate, (FnCallNode**)nodep);
        break;
    case FldAccessTag:
        fnCallFldAccessFlow(fstate, (FnCallNode**)nodep);
        break;
    case CastTag: case IsTag:
        flowLoadValue(fstate, &((CastNode *)*nodep)->exp);
        break;
    case NotLogicTag:
        flowLoadValue(fstate, &((LogicNode *)*nodep)->lexp);
        break;
    case OrLogicTag: case AndLogicTag:
    {
        LogicNode *lnode = (LogicNode*)*nodep;
        flowLoadValue(fstate, &lnode->lexp);
        flowLoadValue(fstate, &lnode->rexp);
        break;
    }

    case TypeLitTag:
        typeLitFlow(fstate, (FnCallNode**)nodep);
        break;

    case ArrayLitTag:
        arrayLitFlow(fstate, (ArrayNode**)nodep);
        break;

    case SizeofTag:
    case NilLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
    case AbsenceTag:
    case UnknownTag:
        break;
    default:
        errorUnreachable(*nodep, "a value node data-flow analysis has no case for");
        break;
    }
}

// *********************
// Variable Info stack for data flow analysis
//
// As we traverse the IR nodes, this tracks what we know about a variable in each block:
// - Has it been initialized (and used)?
// - Has it been moved and has it not been moved?
// *********************

// An entry for a local declared name, in which we preserve its flow flags
typedef struct {
    VarDclNode *node;    // The variable declaration node
    int16_t flags;       // The preserved flow flags
} VarFlowInfo;

VarFlowInfo *gVarFlowStackp = NULL;
size_t gVarFlowStackSz = 0;
size_t gVarFlowStackPos = 0;

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode) {
    // Ensure we have room for another variable
    if (gVarFlowStackPos >= gVarFlowStackSz) {
        if (gVarFlowStackSz == 0) {
            gVarFlowStackSz = 1024;
            gVarFlowStackp = (VarFlowInfo*)memAllocBlk(gVarFlowStackSz * sizeof(VarFlowInfo));
            memset(gVarFlowStackp, 0, gVarFlowStackSz * sizeof(VarFlowInfo));
            gVarFlowStackPos = 0;
        }
        else {
            // Double table size, copying over old data
            VarFlowInfo *oldtable = gVarFlowStackp;
            size_t oldsize = gVarFlowStackSz;
            gVarFlowStackSz <<= 1;
            gVarFlowStackp = (VarFlowInfo*)memAllocBlk(gVarFlowStackSz * sizeof(VarFlowInfo));
            memset(gVarFlowStackp, 0, gVarFlowStackSz * sizeof(VarFlowInfo));
            memcpy(gVarFlowStackp, oldtable, oldsize * sizeof(VarFlowInfo));
        }
    }
    VarFlowInfo *stackp = &gVarFlowStackp[gVarFlowStackPos++];
    stackp->node = varnode;
    stackp->flags = 0;
}

// Start a new scope
size_t flowScopePush() {
    return gVarFlowStackPos;
}

// Is this variable where a part handed back is taken from? The same walk
// inwards, through fields, elements and owning dereferences, that
// flowMoveSource takes to the variable it deactivates.
static int flowIsScopeResultOwner(INode *exp, VarDclNode *varnode) {
    switch (exp->tag) {
    case FldAccessTag:
    case ArrIndexTag:
        return flowIsScopeResultOwner(((FnCallNode *)exp)->objfn, varnode);
    case DerefTag:
        return flowIsScopeResultOwner(((StarNode *)exp)->vtexp, varnode);
    case CastTag:
        return !(exp->flags & FlagConvert) && flowIsScopeResultOwner(((CastNode *)exp)->exp, varnode);
    default:
        return isNameUseNode(exp) && isExpNode(exp) && ((NameUseNode *)exp)->dclnode == (INode *)varnode;
    }
}

// Is this variable's value the one being handed to the caller, and therefore
// not to be released as the scope ends?
// 'retexp' is the value being returned, or NULL where nothing is: NULL means
// nothing is exempt, not that nothing is released.
// A multi-value return hands back a value tuple, whose elements are exempt one
// by one -- the same walk returnFlowEscape does for the borrow check.
// The match is on the declaration the name resolves to, not on the name: a
// 'return' exempts from the whole function's stack, where an inner block's 'a'
// and an outer 'a' both sit, and only the one named is handed back.
static int flowIsScopeResult(INode *retexp, VarDclNode *varnode) {
    if (retexp == NULL)
        return 0;
    if (retexp->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)retexp)->elems, cnt, elemp)) {
            if (flowIsScopeResult(*elemp, varnode))
                return 1;
        }
        return 0;
    }
    // A recast hands back its operand: a local returned as its enrichment or base
    if (retexp->tag == CastTag && !(retexp->flags & FlagConvert))
        return flowIsScopeResult(((CastNode *)retexp)->exp, varnode);
    // A move-typed part handed back moves out of the variable that holds it,
    // which as for any move out of a part no longer owns the whole: releasing
    // it would finalize the part a second time, in the caller. A copied part
    // leaves the variable owning everything it held.
    if ((retexp->tag == FldAccessTag || retexp->tag == ArrIndexTag) && iexpIsMove(retexp))
        return flowIsScopeResultOwner(((FnCallNode *)retexp)->objfn, varnode);
    // A block or an 'if' used as a move value hands back what its final
    // expression, its breaks or its branches do, so what it hands back is matched
    // as if handed back directly. A local handed back on only some of those
    // paths is exempt on all of them: it leaks on the others rather than being
    // finalized twice on these, as a conditional move does.
    if (iexpIsMove(retexp)) {
        switch (retexp->tag) {
        case BlockTag:
        {
            BlockNode *blk = (BlockNode *)retexp;
            INode **nodesp;
            uint32_t cnt;
            INode *last = blk->stmts->used > 0 ? nodesLast(blk->stmts) : NULL;
            if (last != NULL && last->tag == BlockRetTag
                && flowIsScopeResult(((BreakRetNode *)last)->exp, varnode))
                return 1;
            if (blk->breaks) {
                for (nodesFor(blk->breaks, cnt, nodesp)) {
                    if ((*nodesp)->tag == BreakTag
                        && flowIsScopeResult(((BreakRetNode *)*nodesp)->exp, varnode))
                        return 1;
                }
            }
            return 0;
        }
        case IfTag:
        {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(((IfNode *)retexp)->condblk, cnt, nodesp)) {
                nodesp++; cnt--;
                if (flowIsScopeResult(*nodesp, varnode))
                    return 1;
            }
            return 0;
        }
        default:
            break;
        }
    }
    return isNameUseNode(retexp) && isExpNode(retexp) && ((NameUseNode *)retexp)->dclnode == (INode *)varnode;
}

// Create de-alias list of all own/rc reference variables (except the retexp name(s))
// A drop call built here is positioned on the result expression, and on 'lexnode'
// -- the jump that ends the scope -- where there is no result expression to take
// a position from. A 'continue' hands back no value, so it is the jump or nothing.
void flowScopeDealias(size_t startpos, Nodes **varlist, INode *retexp, INode *lexnode) {
    INode *dropat = retexp != NULL ? retexp : lexnode;
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        INode *vartype = avar->node->vtype;
        // A variable that was never given a value owns nothing, so there is
        // nothing to release or finalize: freeing its storage, or running a
        // drop fn over it, would act on garbage.
        if (!(avar->node->flowtempflags & VarInitialized))
            continue;
        // Stopgap: a variable whose value was moved out no longer owns it, so
        // releasing or finalizing it here would act on the new owner's value a
        // second time. VarMoved, like VarInitialized, is the state at scope exit
        // rather than at each program point, so a value moved on only one
        // branch is skipped on all of them -- that leaks rather than
        // double-frees -- and one assigned on only one branch is released on
        // all of them. Precise deactivation belongs to the region redesign.
        if (avar->node->flowtempflags & VarMoved)
            continue;
        // A variable the scope hands back is the caller's to release or finalize,
        // whether it owns a region reference or is a value the drop fn finalizes.
        if (flowIsScopeResult(retexp, avar->node))
            continue;
        if (flowIsOwningType(vartype)) {
            if (*varlist == NULL)
                *varlist = newNodes(4);
            nodesAdd(varlist, (INode*)avar->node);
        }
        else {
            // Add call to type's drop fn to dealias list, if there is one
            INode *dropfn = itypeGetDropFnDcl(vartype);
            if (dropfn != NULL) {
                FnCallNode *dropfncall = newFnCallLower(dropat, dropfn, 1);
                INode *dropnameuse = (INode*)newNameUseFromDclNode((INode*)avar->node, dropat);
                INode *borrow = newBorrowMutRef(dropnameuse, ((IExpNode*)avar->node)->vtype, (INode*)uniPerm);
                nodesAdd(&dropfncall->args, borrow);
                if (*varlist == NULL)
                    *varlist = newNodes(4);
                nodesAdd(varlist, (INode*)dropfncall);
            }
        }
    }
}

// Back out of current scope
void flowScopePop(size_t startpos) {
    gVarFlowStackPos = startpos;
}
