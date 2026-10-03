/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>
#include <stdio.h>

// Is this expression a borrowed reference -- one that does not own what it
// points at? Its permission does not matter: a '&uni' is the only path to its
// value while it lives, but the value still belongs to the place it borrows.
static int flowIsBorrowedRef(INode *exp) {
    RefNode *reftype = (RefNode *)iexpGetTypeDcl(exp);
    return (reftype->tag == RefTag || reftype->tag == ArrayRefTag || reftype->tag == VirtRefTag)
        && itypeGetTypeDcl(reftype->region) == borrowRef;
}

// The value a match binds this variable to -- the matched value, which the
// match holds in a variable of its own -- or NULL for any other variable.
// 'case imm c Circle' binds 'c' to the matched value converted to its variant,
// which is the same value under the variant's name, not a second one: the
// matched value's variable owns it, and is what releases it as its scope ends,
// as the enum, so that whichever arm runs it is finalized once. So the binding
// is never released itself; moving it moves the matched value, and handing it
// back hands back the matched value.
INode *flowMatchBound(INode *var) {
    if (var->tag != VarDclTag)
        return NULL;
    INode *value = ((VarDclNode *)var)->value;
    if (value == NULL || value->tag != CastTag || !(value->flags & FlagMatchBind))
        return NULL;
    INode *matched = ((CastNode *)value)->exp;
    return isNameUseNode(matched) && isExpNode(matched) ? matched : NULL;
}

// Does a match's binding name the matched value's own storage? A binding by
// value (a variant struct of a matched enum) does: code generation gives it
// the matched value's variable's storage, read through the variant's layout
// as the conversion reads it, so a swap or a store into the binding changes
// the value that variable releases. A binding by reference holds a copy of
// the matched reference.
int flowMatchInPlace(VarDclNode *var) {
    INode *matched = flowMatchBound((INode *)var);
    if (matched == NULL)
        return 0;
    INode *matchdcl = ((NameUseNode *)matched)->dclnode;
    return matchdcl->tag == VarDclTag && !(matchdcl->flags & FlagStatic)
        && iexpGetTypeDcl(matched)->tag == StructTag
        && itypeGetTypeDcl(var->vtype)->tag == StructTag;
}

// Is this expression a shared owner -- an owning reference that other holders
// may be sharing? An owning reference that may be aliased ('Rc[mut, T]', 'Rc[imm, T]',
// 'Rc[ro, T]', and every 'Rc' form but 'Rc[uni, T]') is one of possibly many holders
// counting the same value, so it does not solely own it. An owning reference
// that is a move type -- a 'uni' one, or any in the 'Move' region 'So' -- is
// the only holder, and may give the value up. One into a region that is
// neither counted nor 'Move' -- a collector's -- is shared freely, and is
// refused too.
static int flowIsSharedOwner(INode *exp) {
    INode *reftype = iexpGetTypeDcl(exp);
    return (reftype->tag == RefTag || reftype->tag == VirtRefTag)
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

// *********************
// Variable state along each path, for the main walk
//
// The flags on a declaration (VarInitialized, VarMoved, VarHollow) are the walk's
// state as it goes. An 'if' walks each arm from the state its conditions leave
// and joins them after (ifFlow): a flag any live arm set is set, so a value moved
// on some arm counts as moved -- it may not be used again -- while an arm that
// jumped away contributes nothing. So every change goes through
// flowVarSetFlags, which logs the old flags while an 'if' is being walked and the
// variable was declared outside the innermost conditional part (a variable
// declared inside an arm leaves scope with it). Whether a variable holds its
// value at a scope's end on every path is the path walk's to say, when the flags
// could differ by path (flowDropNote); this state only says what may be used.
// *********************

uint16_t flowDepth = 0;
FlowState *flowCurrent = NULL;
static uint32_t flowForks = 0;     // how many 'if's are being walked

typedef struct {
    VarDclNode *var;
    Nodes *hollowed;
    uint16_t flags;
} FlowVarEnt;

struct FlowVarPath {
    struct FlowVarPath *next;
    uint32_t cnt;
    FlowVarEnt ents[];
};

static FlowVarEnt *flowVarLog = NULL;
static uint32_t flowVarLogN = 0;
static uint32_t flowVarLogCap = 0;

void flowVarSetFlags(VarDclNode *var, uint16_t flags, Nodes *hollowed) {
    if (var->flowtempflags == flags && var->hollowed == hollowed)
        return;
    if (flowForks > 0 && var->flowdepth < flowDepth) {
        if (flowVarLogN == flowVarLogCap) {
            uint32_t oldcap = flowVarLogCap;
            flowVarLogCap = oldcap ? oldcap << 1 : 64;
            FlowVarEnt *grown = (FlowVarEnt *)memAllocBlk(flowVarLogCap * sizeof(FlowVarEnt));
            if (oldcap)
                memcpy(grown, flowVarLog, oldcap * sizeof(FlowVarEnt));
            flowVarLog = grown;
        }
        FlowVarEnt *ent = &flowVarLog[flowVarLogN++];
        ent->var = var;
        ent->flags = var->flowtempflags;
        ent->hollowed = var->hollowed;
    }
    var->flowtempflags = flags;
    var->hollowed = hollowed;
}

void flowFnBegin(FlowState *fstate) {
    flowDepth = 0;
    flowCurrent = fstate;
    flowVarLogN = 0;
    flowForks = 0;
}

uint32_t flowVarLogPos() {
    return flowVarLogN;
}

uint32_t flowVarLogMark() {
    ++flowForks;
    return flowVarLogN;
}

FlowVarPath *flowVarPathTake(uint32_t mark, FlowVarPath *next) {
    uint32_t cnt = flowVarLogN - mark;
    FlowVarPath *path = (FlowVarPath *)memAllocBlk(sizeof(FlowVarPath) + cnt * sizeof(FlowVarEnt));
    path->next = next;
    path->cnt = 0;
    for (uint32_t i = mark; i < flowVarLogN; ++i) {
        VarDclNode *var = flowVarLog[i].var;
        uint32_t k;
        for (k = 0; k < path->cnt && path->ents[k].var != var; ++k)
            ;
        if (k < path->cnt)
            continue;
        FlowVarEnt *ent = &path->ents[path->cnt++];
        ent->var = var;
        ent->flags = var->flowtempflags;
        ent->hollowed = var->hollowed;
    }
    return path;
}

void flowVarRollback(uint32_t mark) {
    while (flowVarLogN > mark) {
        FlowVarEnt *ent = &flowVarLog[--flowVarLogN];
        ent->var->flowtempflags = ent->flags;
        ent->var->hollowed = ent->hollowed;
    }
}

static Nodes *flowNodesUnion(Nodes *a, Nodes *b);

void flowVarJoin(FlowVarPath *paths, uint32_t npaths) {
    --flowForks;
    for (FlowVarPath *path = paths; path; path = path->next) {
        for (uint32_t i = 0; i < path->cnt; ++i) {
            VarDclNode *var = path->ents[i].var;
            // Only once per variable: the first path that changed it gathers every path's
            int seen = 0;
            for (FlowVarPath *before = paths; before != path && !seen; before = before->next) {
                for (uint32_t k = 0; k < before->cnt; ++k) {
                    if (before->ents[k].var == var) {
                        seen = 1;
                        break;
                    }
                }
            }
            if (seen)
                continue;
            uint16_t flags = 0;
            Nodes *hollowed = NULL;
            uint32_t changed = 0;
            for (FlowVarPath *other = paths; other; other = other->next) {
                for (uint32_t k = 0; k < other->cnt; ++k) {
                    if (other->ents[k].var == var) {
                        flags |= other->ents[k].flags;
                        hollowed = flowNodesUnion(hollowed, other->ents[k].hollowed);
                        ++changed;
                        break;
                    }
                }
            }
            // A path that did not change it has it as it was at the fork
            if (changed < npaths) {
                flags |= var->flowtempflags;
                hollowed = flowNodesUnion(hollowed, var->hollowed);
            }
            // Moved or hollowed on some path, and the list of what hollowed it
            // is only for a variable still hollow
            if (!(flags & VarHollow))
                hollowed = NULL;
            flowVarSetFlags(var, flags, hollowed);
        }
    }
}

// The variable owning a variable's value: a match's binding stands for the
// matched value, whose variable owns it
VarDclNode *flowDropOwner(VarDclNode *var) {
    INode *matched;
    while ((matched = flowMatchBound((INode *)var)) != NULL) {
        INode *dcl = ((NameUseNode *)matched)->dclnode;
        if (dcl->tag != VarDclTag)
            break;
        var = (VarDclNode *)dcl;
    }
    return var;
}

int flowNoDeath(INode *type) {
    INode *dcl = flowGateNamed(type);
    switch (dcl->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
    case VoidTag:
        return 1;
    default:
        return 0;
    }
}

// Asked once per variable, and remembered: at every store in a loop or a
// branch the question would otherwise walk the type again
int flowDropTracked(VarDclNode *var) {
    if (var->flowtracked == 0) {
        var->flowtracked = var->scope > 0 && !(var->flags & FlagStatic) && var->vtype
            && !flowNoDeath(var->vtype)
            && (itypeIsMove(var->vtype) || itypeNeedsFinal(var->vtype)) ? 2 : 1;
    }
    return var->flowtracked == 2;
}

void flowDropNote(FlowState *fstate, VarDclNode *var) {
    if (fstate == NULL || fstate->dropgate)
        return;
    var = flowDropOwner(var);
    if (flowDepth > var->flowdepth && flowDropTracked(var))
        fstate->dropgate = 1;
}

VarDclNode *flowLvalRootVar(INode *lval) {
    INode *node = lval;
    while (1) {
        if (isNameUseNode(node) && isExpNode(node)) {
            if (node == lval)
                return NULL;
            INode *dcl = ((NameUseNode *)node)->dclnode;
            if (dcl->tag != VarDclTag || ((VarDclNode *)dcl)->scope == 0 || (dcl->flags & FlagStatic))
                return NULL;
            return (VarDclNode *)dcl;
        }
        switch (node->tag) {
        case FldAccessTag:
        {
            INode *objfn = ((FnCallNode *)node)->objfn;
            if (iexpGetTypeDcl(objfn)->tag != StructTag && iexpGetTypeDcl(objfn)->tag != TTupleTag)
                return NULL;
            node = objfn;
            break;
        }
        case ArrIndexTag:
        {
            INode *objfn = ((FnCallNode *)node)->objfn;
            if (iexpGetTypeDcl(objfn)->tag != ArrayTag)
                return NULL;
            node = objfn;
            break;
        }
        case CastTag:
            if (node->flags & FlagConvert)
                return NULL;
            node = ((CastNode *)node)->exp;
            break;
        default:
            return NULL;
        }
    }
}

// A copy of 'nodes' with 'node' added, so that a list an undo log remembers is
// never changed under it
static Nodes *flowNodesWith(Nodes *nodes, INode *node) {
    Nodes *copy = newNodes(nodes ? nodes->used + 1 : 2);
    INode **nodesp;
    uint32_t cnt;
    if (nodes) {
        for (nodesFor(nodes, cnt, nodesp))
            nodesAdd(&copy, *nodesp);
    }
    nodesAdd(&copy, node);
    return copy;
}

static int flowNodesHas(Nodes *nodes, INode *node);

static Nodes *flowNodesUnion(Nodes *a, Nodes *b) {
    if (b == NULL || a == b)
        return a;
    if (a == NULL)
        return b;
    INode **nodesp;
    uint32_t cnt;
    Nodes *both = a;
    for (nodesFor(b, cnt, nodesp)) {
        if (!flowNodesHas(both, *nodesp))
            both = flowNodesWith(both, *nodesp);
    }
    return both;
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

// What one move does to the variables it reads, beside the list of variables it
// leaves without their value: the ones whose whole value moved, and the moves
// that took what a local owning reference points at, or an element of it. Neither is
// narrowed to what every value of a block or an 'if' moves (flowMoveExit).
typedef struct {
    Nodes *wholes;    // variables whose own value moved
    Nodes *hollow;    // move sources, each reaching a local owning reference
    int nomark;       // walking a match binding's matched value: the binding's use is the move site
} MoveParts;

// The local variable holding an owning reference that 'ref' names, or NULL.
// Moving a value out through such a reference -- which flowRefuseMoveThrough
// has let through only for a sole owner -- empties the allocation, not the
// variable: the variable still owns the allocation, whose memory must go back.
VarDclNode *flowOwningLocal(INode *ref) {
    if (!(isNameUseNode(ref) && isExpNode(ref)))
        return NULL;
    VarDclNode *var = (VarDclNode *)((NameUseNode *)ref)->dclnode;
    if (var->tag != VarDclTag || var->scope == 0)
        return NULL;
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(var->vtype);
    if (reftype->tag != RefTag || !regionIsOwning(reftype->region))
        return NULL;
    return var;
}

static void flowMoveSource(INode *node, Nodes **moved, INode *top, MoveParts *parts);

// Refuse a move out of a field, 'fld' being the field access on the chain
// walked inwards from 'top', the value moving. Whether the value is the
// field's own or one reached through it -- an element of an array field, what
// an owning reference field points at -- the struct or tuple holding the field
// would be left with a hole in it, which it could neither be used with nor
// finalized with. So nothing moves out of a field: '<=>' swaps a value in and
// out of it, and moving the whole struct takes every field with it.
static void flowRefuseMoveField(FnCallNode *fld, INode *top) {
    INode *methfld = fld->methfld;
    INode *whole = top;
    while (whole->tag == CastTag)
        whole = ((CastNode *)whole)->exp;
    if (methfld->tag == ULitTag) {
        errorMsgNode(top, ErrorMoveField,
            (INode *)fld == whole ? "May not move element %d out of the tuple that holds it. Swap a value in with '<=>', or move the whole tuple."
                : "May not move a value out through element %d of a tuple, which would be left with a hole in it. Swap a value in with '<=>', or move the whole tuple.",
            (int)((ULitNode *)methfld)->uintlit);
        return;
    }
    char *name = isNameUseNode(methfld) ? &((NameUseNode *)methfld)->namesym->namestr : "?";
    errorMsgNode(top, ErrorMoveField,
        (INode *)fld == whole ? "May not move field '%s' out of the struct that holds it. Swap a value in with '<=>', or move the whole struct."
            : "May not move a value out through field '%s', which would leave a hole in the struct that holds it. Swap a value in with '<=>', or move the whole struct.",
        name);
}

// A move out through a local sole owner: the variable is in 'moved', so that a
// block's or an 'if''s values still narrow it as any moved variable, and the
// move itself is noted, to become the variable's hollow release.
static void flowMoveHollow(VarDclNode *owner, INode *owneruse, Nodes **moved, INode *top, MoveParts *parts) {
    if (moved == NULL)
        return;
    // Where it happens, for a drop flag
    owneruse->flags |= FlagHollowOut;
    flowAddMoved(moved, (INode *)owner);
    if (parts->hollow == NULL)
        parts->hollow = newNodes(2);
    nodesAdd(&parts->hollow, top);
}

// Walk one of the values a block or an 'if' may hand back, narrowing 'common'
// to the variables that every value walked so far moves out of ('first' says
// none has been walked yet). With 'moved' NULL the walk only checks.
static void flowMoveExit(INode *exp, Nodes **moved, Nodes **common, int *first, MoveParts *parts) {
    Nodes *these = NULL;
    if (iexpIsMove(exp))
        flowMoveSource(exp, moved ? &these : NULL, exp, parts);
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

// A move out through a temporary sole owner, '*make()': the temporary still
// owns the allocation, whose memory goes back at the end of its statement
// without what moved, as a hollowed variable's does (genlTempRelease)
static void flowTempHollow(TempNode *temp, Nodes **moved, INode *top) {
    if (moved == NULL)
        return;
    if (temp->moved == NULL)
        temp->moved = newNodes(2);
    if (!flowNodesHas(temp->moved, top))
        nodesAdd(&temp->moved, top);
}

// Walk inwards from a moved value to its source: refuse a move out of a place
// that does not own the value, and, when 'moved' is given, add to it each
// source variable the move leaves without its value. A value reached through a
// borrowed reference still belongs to what was borrowed, and one reached through
// a shared owning reference still belongs to its other holders, so moving it out
// would leave two owners of one value. A chain that passes through a field is
// refused whatever it reaches (flowRefuseMoveField).
//
// 'top' is the outermost node of the chain of elements, dereferences and
// recasts being walked -- the expression that names the value moving. A chain
// that reaches a local owning reference through it (a dereference, or an
// element of the array it points at) moves the referent, or an element of it, out and leaves the
// variable owning the allocation: 'parts' notes the move as hollowing the
// variable rather than as moving it.
static void flowMoveSource(INode *node, Nodes **moved, INode *top, MoveParts *parts) {
    // For a variable, its value is what moves. A variable a match binds is the
    // matched value under its variant's name, which owns it (flowMatchBound):
    // moving the one moves the other.
    if (isNameUseNode(node) && isExpNode(node)) {
        VarDclNode *vardclnode = (VarDclNode *)((NameUseNode*)node)->dclnode;
        if (moved) {
            flowAddMoved(moved, (INode *)vardclnode);
            flowAddMoved(&parts->wholes, (INode *)vardclnode);
            // Where the value leaves, for a drop flag. A match's binding is
            // marked, not the matched value's name its declaration reads: the
            // move happens here, and the flag is the matched value's variable's
            // (flowDropOwner).
            // The match's own variable ('_') is named by one name use every
            // pattern and binding shares, so no one site can be marked on it.
            if (!parts->nomark && vardclnode->namesym != anonName)
                node->flags |= FlagMoveOut;
        }
        if (vardclnode->scope == 0) {
            errorMsgNode(node, ErrorInvType, "May not move a value out of a global variable.");
        }
        INode *matched = flowMatchBound((INode *)vardclnode);
        if (matched) {
            int nomark = parts ? parts->nomark : 0;
            if (parts)
                parts->nomark = 1;
            flowMoveSource(matched, moved, top, parts);
            if (parts)
                parts->nomark = nomark;
        }
        return;
    }
    switch (node->tag) {
    case FldAccessTag:
        flowRefuseMoveField((FnCallNode *)node, top);
        return;

    // Go inwards to find the variable to mark it as moved. An element read
    // straight through a reference -- a slice, which takes no injected
    // dereference -- is read through that reference.
    case ArrIndexTag:
    {
        INode *objfn = ((FnCallNode*)node)->objfn;
        if (flowRefuseMoveThrough(node, objfn))
            return;
        VarDclNode *owner = flowOwningLocal(objfn);
        if (owner) {
            flowMoveHollow(owner, objfn, moved, top, parts);
            return;
        }
        if (objfn->tag == TempTag && iexpGetTypeDcl(objfn)->tag == RefTag) {
            flowTempHollow((TempNode *)objfn, moved, top);
            return;
        }
        flowMoveSource(objfn, moved, top, parts);
        break;
    }
    case DerefTag:
    {
        INode *ref = ((StarNode*)node)->vtexp;
        if (flowRefuseMoveThrough(node, ref))
            return;
        VarDclNode *owner = flowOwningLocal(ref);
        if (owner) {
            flowMoveHollow(owner, ref, moved, top, parts);
            return;
        }
        if (ref->tag == TempTag) {
            flowTempHollow((TempNode *)ref, moved, top);
            return;
        }
        flowMoveSource(ref, moved, top, parts);
        break;
    }

    // A value moved out of a temporary by value -- an element of the array
    // it is -- leaves it with a hole: like a local array an element moved out
    // of, it is not finalized, and what else it held leaks
    case TempTag:
        if (moved)
            ((TempNode *)node)->kept = 1;
        break;

    // A recast is its operand under another type name -- an enrichment and its
    // base, which share one representation -- so moving it moves the operand,
    // and so does a conversion that carries its owner (flowCastCarries)
    case CastTag:
        if (flowCastCarries(node))
            flowMoveSource(((CastNode*)node)->exp, moved, top, parts);
        break;

    // A tuple literal has no storage of its own: its sources are its elements,
    // and only a move-typed element is moved out of, the rest are copied
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node)->elems, cnt, nodesp)) {
            if (iexpIsMove(*nodesp))
                flowMoveSource(*nodesp, moved, *nodesp, parts);
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
                flowMoveExit(((BreakRetNode *)last)->exp, moved, &common, &first, parts);
        }
        if (blk->breaks) {
            for (nodesFor(blk->breaks, cnt, nodesp)) {
                if ((*nodesp)->tag == BreakTag)
                    flowMoveExit(((BreakRetNode *)*nodesp)->exp, moved, &common, &first, parts);
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
            flowMoveExit(*nodesp, moved, &common, &first, parts);
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

static int flowNodesHas(Nodes *nodes, INode *node) {
    INode **nodesp;
    uint32_t cnt;
    if (nodes == NULL)
        return 0;
    for (nodesFor(nodes, cnt, nodesp)) {
        if (*nodesp == node)
            return 1;
    }
    return 0;
}

// The local owning reference a hollowing move's chain reaches, walked inwards
// exactly as flowMoveSource walked it
static VarDclNode *flowHollowOwner(INode *exp) {
    while (1) {
        INode *inner;
        switch (exp->tag) {
        case ArrIndexTag:
            inner = ((FnCallNode *)exp)->objfn; break;
        case DerefTag:
            inner = ((StarNode *)exp)->vtexp; break;
        case CastTag:
            exp = ((CastNode *)exp)->exp;
            continue;
        default:
            return NULL;
        }
        VarDclNode *owner = flowOwningLocal(inner);
        if (owner)
            return owner;
        exp = inner;
    }
}

// Deactivate source of a moved value (or say move is illegal)
//
// A move out through a local sole owner hollows the variable instead: it may
// not be used again, as if moved, but it still owns the allocation, which its
// release gives back without finalizing what moved (HollowNode). A variable
// whose whole value also moved is left to that move.
//
// A variable moved or hollowed by only some of the values a block or an 'if'
// hands back is moved or hollowed as far as its use goes: it may not be used
// again, since on some path it holds nothing. Whether it is released at its
// scope's end is a drop flag's to say, set where each value moves (the name
// use flowMoveSource marked), so the function's drops go to the path walk.
void flowHandleMove(INode *node) {
    Nodes *moved = NULL;
    MoveParts parts = { NULL, NULL, 0 };
    INode **nodesp;
    uint32_t cnt;
    flowMoveSource(node, &moved, node, &parts);
    if (parts.wholes) {
        for (nodesFor(parts.wholes, cnt, nodesp)) {
            VarDclNode *var = (VarDclNode *)*nodesp;
            flowVarSetFlags(var, var->flowtempflags | VarMoved, var->hollowed);
            flowDropNote(flowCurrent, var);
            if (!flowNodesHas(moved, *nodesp) && flowCurrent && flowDropTracked(flowDropOwner(var)))
                flowCurrent->dropgate = 1;
        }
    }
    if (parts.hollow) {
        for (nodesFor(parts.hollow, cnt, nodesp)) {
            VarDclNode *owner = flowHollowOwner(*nodesp);
            if (flowNodesHas(parts.wholes, (INode *)owner))
                continue;
            flowVarSetFlags(owner, owner->flowtempflags | VarHollow, flowNodesWith(owner->hollowed, *nodesp));
            if (!flowNodesHas(owner->hollowall, *nodesp))
                owner->hollowall = flowNodesWith(owner->hollowall, *nodesp);
            flowDropNote(flowCurrent, owner);
            if (!flowNodesHas(moved, (INode *)owner) && flowCurrent)
                flowCurrent->dropgate = 1;
        }
    }
}

// Refuse a move-typed value a scope hands back -- a return's or a block's
// result -- when its source does not own it. The source is not deactivated
// here: a local handed back is exempted from the scope's release by
// flowScopeDealias instead.
void flowResultMove(INode *node) {
    if (iexpIsMove(node))
        flowMoveSource(node, NULL, node, NULL);
}

// Is this type a counted reference: one into a region whose 'aliasRef' is called
// for each copy that becomes another owner? A virtual one ('Rc[Trait]') is
// counted exactly as a single reference is.
int flowIsRcRef(INode *type) {
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(type);
    return (reftype->tag == RefTag || reftype->tag == VirtRefTag)
        && regionIsCounted(reftype->region);
}

// Is this type an owning reference into a region, single or virtual, or a
// tuple carrying one: what a store releases before it overwrites (genlStore)?
// What a scope's end does to a variable is itypeNeedsFinal's wider question.
int flowIsOwningType(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    if (typedcl->tag == RefTag || typedcl->tag == VirtRefTag) {
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

// Does a copy of a value of this type add a holder to a counted reference
// inside it, which the value's death releases? A struct's drop releases what
// its fields own, an enum's what its variant's fields own (genlStructDrop,
// genlEnumDrop), and a tuple's or an array's death each element's
// (genlFinalizeAt), so any of them holding a counted reference, directly or
// deeper, does; the count must then rise with each copy, or each copy's death
// would release the one holder again. A counted reference itself is
// flowIsRcRef's. A move type is never copied, so the answer is only ever asked
// of a copy type.
int flowHeldCounted(INode *type) {
    INode *typedcl = itypeGetTypeDcl(type);
    INode **nodesp;
    uint32_t cnt;
    switch (typedcl->tag) {
    case TTupleTag:
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, nodesp)) {
            if (flowIsRcRef(*nodesp) || flowHeldCounted(*nodesp))
                return 1;
        }
        return 0;
    case ArrayTag:
    {
        INode *elemtype = arrayElemType(typedcl);
        return flowIsRcRef(elemtype) || flowHeldCounted(elemtype);
    }
    case StructTag:
        break;
    default:
        return 0;
    }
    StructNode *strnode = (StructNode *)typedcl;
    if (strnode->flags & EnumType) {
        if (strnode->dropfn == NULL || strnode->derived == NULL)
            return 0;
        for (nodesFor(strnode->derived, cnt, nodesp)) {
            if (flowVariantHeldCounted(*nodesp))
                return 1;
        }
        return 0;
    }
    if (strnode->flags & TraitType)
        return 0;
    return flowVariantHeldCounted((INode *)strnode);
}

// Does a struct -- a variant among them -- hold a counted reference its drop
// releases: in a field of its own, or inside one (flowHeldCounted)?
int flowVariantHeldCounted(INode *variant) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode *)variant)->fields, cnt, nodesp)) {
        INode *fldtype = ((FieldDclNode *)*nodesp)->vtype;
        if (flowIsRcRef(fldtype) || flowHeldCounted(fldtype))
            return 1;
    }
    return 0;
}

// If needed, inject a reference-count node for Rc/own references, adjusting the count by amt.
// A struct or an enum is one holder of each counted reference its drop releases
// (flowHeldCounted).
void flowInjectRefCountAmt(INode **nodep, int16_t amt) {
    INode *vtype = ((IExpNode*)*nodep)->vtype;
    INode *typedcl = itypeGetTypeDcl(vtype);
    int16_t *counts = NULL;
    if (typedcl->tag == TTupleTag) {
        // A tuple value is one holder of each counted reference it carries, so
        // every Rc element, and every element holding one, gets the
        // adjustment and every other element none.
        Nodes *elems = ((TupleNode *)typedcl)->elems;
        counts = (int16_t *)memAllocBlk(elems->used * sizeof(int16_t));
        int16_t *countp = counts;
        int anycounted = 0;
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(elems, cnt, elemp)) {
            *countp = flowIsRcRef(*elemp) || flowHeldCounted(*elemp) ? amt : 0;
            anycounted |= *countp++;
        }
        if (!anycounted)
            return;
        amt = (int16_t)elems->used;
    }
    // No need for injected node if we are not dealing with Rc references
    else if (!flowIsRcRef(vtype) && !flowHeldCounted(vtype))
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

// If needed, inject a reference-count node for Rc/own references, adding one holder
void flowInjectRefCount(INode **nodep) {
    flowInjectRefCountAmt(nodep, 1);
}

// Does this cast hand on what its operand holds? A recast is its operand under
// another type name. A conversion makes a new value, except one into an owning
// virtual reference ('So[App]' from a 'So[Spinner]', or from a virtual 'So' of
// a trait it extends): that adds a vtable to the operand's one owner and keeps
// it, so the operand is moved out of, or counted, as a recast's would be.
int flowCastCarries(INode *cast) {
    if (!(cast->flags & FlagConvert))
        return 1;
    RefNode *totype = (RefNode *)iexpGetTypeDcl(cast);
    return totype->tag == VirtRefTag && regionIsOwning(totype->region);
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
        return flowCastCarries(node) && flowIsLvalRead(((CastNode*)node)->exp);
    default:
        return 0;
    }
}

// Does a place still hold this expression's value once it is read: an lvalue
// (flowIsLvalRead), or an assignment, whose target keeps the value it stored
// ('a = b = make()' leaves the value in 'b' as well as in 'a')? The '_'
// placeholder keeps nothing.
static int flowIsKeptRead(INode *node) {
    if (node->tag == AssignTag) {
        INode *lval = ((AssignNode *)node)->lval;
        return !(isNameUseNode(lval) && isExpNode(lval) && ((NameUseNode *)lval)->namesym == anonName);
    }
    return flowIsLvalRead(node);
}

void flowHandleMoveOrCopy(INode **nodep) {
    // A tuple literal has no storage of its own: each element's value is moved
    // or copied into it on its own, as a struct literal's fields are, so a
    // counted reference read out of a variable gains a holder there
    if ((*nodep)->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)*nodep)->elems, cnt, elemp))
            flowHandleMoveOrCopy(elemp);
        return;
    }
    if (iexpIsMove(*nodep)) {
        // Moving needs to deactivate source variable use
        flowHandleMove(*nodep);
    }
    else {
        // A reference count is how many holders exist. Only a place that
        // still holds its reference afterwards adds a holder. A temporary --
        // an allocation, a call's result, a literal, a block's or an 'if''s
        // value -- hands over the reference it was born holding, and counting
        // that again would count one holder twice.
        if (flowIsKeptRead(*nodep))
            flowInjectRefCount(nodep);
    }
}

// *********************
// Temporaries
//
// A value an expression makes and nothing takes -- a call's result whose field
// is read, a sole owner read through, an owner lent to a call as a borrow, a
// value thrown away -- dies at the end of the statement that made it, newest
// first, as a scope's locals do at its end (doc/reference/refinitdrop.html).
// Flow finds each where it is read or thrown away and wraps it in a TempNode;
// generation keeps it and finalizes it there. A temporary in an 'if' or
// 'while' condition dies at the condition's end, and one in the right operand
// of 'and' or 'or' at that operand's end, since each runs on some paths only.
// *********************

uint32_t flowTempCount = 0;

// Is this expression a temporary whose death does something: a value made
// here -- a call's result, a literal, an allocation, a block's or an 'if''s
// value, a conversion -- rather than read out of a place that keeps it
// (flowIsLvalRead)? An assignment's value is what it stored, which the target
// keeps.
static int flowIsTemp(INode *node) {
    if (!isExpNode(node) || flowIsLvalRead(node))
        return 0;
    switch (node->tag) {
    case AssignTag:
    case SwapTag:
    case VarDclTag:
    case TempTag:
    case RefCountTag:
    case HollowTag:
    case DropFlagTag:
    case NilLitTag:
    case NullLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
    case SizeofTag:
    case AbsenceTag:
    case UnknownTag:
        return 0;
    case FnCallTag:
        if (fnCallIsNever(node))
            return 0;
        break;
    default:
        break;
    }
    INode *vtype = ((IExpNode *)node)->vtype;
    return vtype != NULL && vtype != unknownType && itypeNeedsFinal(vtype);
}

static void flowTempWrap(INode **nodep) {
    TempNode *temp;
    newNode(temp, TempNode, TempTag);
    inodeLexCopy((INode *)temp, *nodep);
    temp->vtype = ((IExpNode *)*nodep)->vtype;
    temp->exp = *nodep;
    temp->moved = NULL;
    temp->walkvar = NULL;
    temp->kept = 0;
    *nodep = (INode *)temp;
    ++flowTempCount;
}

void flowTempRead(INode **nodep) {
    if (flowIsTemp(*nodep))
        flowTempWrap(nodep);
}

void flowTempBorrowed(INode **nodep) {
    INode *node = *nodep;
    if (flowIsTemp(node)) {
        flowTempWrap(nodep);
        return;
    }
    // A constant literal is kept in a constant global
    if (!isExpNode(node) || flowIsLvalRead(node) || node->tag == TempTag || borrowIsConstLit(node)
        || (node->tag == FnCallTag && fnCallIsNever(node)))
        return;
    flowTempWrap(nodep);
    ((TempNode *)*nodep)->kept = 1;
}

// Does this cast's value hold what its operand held, so that the operand is
// not a temporary of its own? A recast does, unless it lends an owner as a
// borrowed reference; a conversion does only into an owning virtual
// reference (flowCastCarries).
static int flowCastHandsOn(INode *cast) {
    if (!flowCastCarries(cast))
        return 0;
    INode *to = iexpGetTypeDcl(cast);
    if (to->tag == PtrTag)
        return 0;
    return !((to->tag == RefTag || to->tag == ArrayRefTag || to->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)to)->region) == borrowRef);
}

// Does a value of this type hold a raw pointer, anywhere in it, through owning
// references too? A few levels are looked at; a type nested deeper is taken
// as holding none.
static int flowTempHoldsPtr(INode *type, int depth) {
    if (type == NULL || depth > 6)
        return 0;
    INode *typedcl = itypeGetTypeDcl(type);
    INode **nodesp;
    uint32_t cnt;
    switch (typedcl->tag) {
    case PtrTag:
        return 1;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return flowTempHoldsPtr(((RefNode *)typedcl)->vtexp, depth + 1);
    case ArrayTag:
        return flowTempHoldsPtr(arrayElemType(typedcl), depth + 1);
    case TTupleTag:
        for (nodesFor(((TupleNode *)typedcl)->elems, cnt, nodesp)) {
            if (flowTempHoldsPtr(*nodesp, depth + 1))
                return 1;
        }
        return 0;
    case StructTag:
    {
        StructNode *strnode = (StructNode *)typedcl;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            if (flowTempHoldsPtr(((IExpNode *)*nodesp)->vtype, depth + 1))
                return 1;
        }
        if (strnode->derived) {
            for (nodesFor(strnode->derived, cnt, nodesp)) {
                if (flowTempHoldsPtr(*nodesp, depth + 1))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

// How a value goes out of its statement (flowTempEscape's 'out'): not at all;
// through values that may hold a borrow, which the loan walk follows; or, at
// some step, through a value holding a raw pointer, which nothing follows
enum {
    TempOutNone = 0,
    TempOutBorrow = 1,
    TempOutPtr = 2,
};

// How a part of a value going out as 'out' goes out, the part of type 'type':
// not at all, if it can hold neither a borrow nor a pointer
static int flowTempOut(int out, INode *type) {
    if (out == TempOutNone || type == NULL)
        return TempOutNone;
    if (flowTempHoldsPtr(type, 0))
        return TempOutPtr;
    return itypeCarriesBorrow(type) ? out : TempOutNone;
}

// May this call keep what an argument points at beyond itself: is an argument
// a '&mut' reference, or a pointer, to something that can hold a borrow or a
// pointer? Returns how what it keeps goes out.
static int flowTempCallStores(FnCallNode *call) {
    INode **argsp;
    uint32_t cnt;
    int out = TempOutNone;
    for (nodesFor(call->args, cnt, argsp)) {
        INode *type = iexpGetTypeDcl(*argsp);
        int stores = TempOutNone;
        if (type->tag == PtrTag)
            stores = flowTempOut(TempOutBorrow, ((StarNode *)type)->vtexp);
        else if ((type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag)
            && itypeGetTypeDcl(((RefNode *)type)->region) == borrowRef
            && (permGetFlags(((RefNode *)type)->perm) & MayWrite))
            stores = flowTempOut(TempOutBorrow, ((RefNode *)type)->vtexp);
        if (stores > out)
            out = stores;
    }
    return out;
}

// A temporary is finalized at its statement's end. A borrow of it may outlive
// the statement only in what its value goes into -- a call's result that can
// hold one, an aggregate, a store, a value the statement hands back -- and the
// loan walk refuses any use of that after the temporary is gone (flowpath.c);
// where Rust's rule extends a temporary, it is a hidden local of the block
// already (varDclExtend). A raw pointer into it nothing follows: so the walk
// here carries 'out' down from where a value leaves the statement, through
// values that can hold a borrow or a pointer, and a temporary it reaches
// through a value holding a pointer is kept -- never finalized, which leaks
// it, as every temporary once was, rather than leave the pointer dangling.
// Not a lock's guard (FlagLockAcquire): kept, it would hold its lock forever,
// and every later borrow through the reference would wait for it. A guard
// gives its lock and its owner back at its statement's end whatever pointer
// into the value is made, which is unchecked, as a raw pointer is anywhere.
void flowTempEscape(INode *node, int out) {
    if (isNameUseNode(node))
        return;
    INode **nodesp;
    uint32_t cnt;
    switch (node->tag) {
    case TempTag:
    {
        TempNode *temp = (TempNode *)node;
        if (out == TempOutPtr
            && !(temp->exp->tag == CastTag && (temp->exp->flags & FlagLockAcquire)))
            temp->kept = 1;
        flowTempEscape(temp->exp, out);
        return;
    }
    case FnCallTag:
    {
        FnCallNode *call = (FnCallNode *)node;
        int argsout = flowTempOut(out, call->vtype);
        int stores = flowTempCallStores(call);
        if (stores > argsout)
            argsout = stores;
        if (!isNameUseNode(call->objfn))
            flowTempEscape(call->objfn, TempOutNone);
        for (nodesFor(call->args, cnt, nodesp))
            flowTempEscape(*nodesp, argsout);
        return;
    }
    case FldAccessTag:
    case ArrIndexTag:
    {
        FnCallNode *access = (FnCallNode *)node;
        flowTempEscape(access->objfn, flowTempOut(out, access->vtype));
        if (node->tag == ArrIndexTag) {
            for (nodesFor(access->args, cnt, nodesp))
                flowTempEscape(*nodesp, TempOutNone);
        }
        return;
    }
    case DerefTag:
        flowTempEscape(((StarNode *)node)->vtexp, flowTempOut(out, ((StarNode *)node)->vtype));
        return;
    case BorrowTag:
    case ArrayBorrowTag:
        flowTempEscape(((RefNode *)node)->vtexp, out);
        return;
    case CastTag:
        flowTempEscape(((CastNode *)node)->exp, flowTempOut(out, ((CastNode *)node)->vtype));
        return;
    case IsTag:
        flowTempEscape(((CastNode *)node)->exp, TempOutNone);
        return;
    case NotLogicTag:
        flowTempEscape(((LogicNode *)node)->lexp, TempOutNone);
        return;
    case OrLogicTag:
    case AndLogicTag:
        flowTempEscape(((LogicNode *)node)->lexp, TempOutNone);
        flowTempEscape(((LogicNode *)node)->rexp, TempOutNone);
        return;
    case VTupleTag:
    case ArrayLitTag:
    {
        int elemout = flowTempOut(out, ((IExpNode *)node)->vtype);
        Nodes *elems = node->tag == VTupleTag ? ((TupleNode *)node)->elems : ((ArrayNode *)node)->elems;
        for (nodesFor(elems, cnt, nodesp))
            flowTempEscape(*nodesp, elemout);
        return;
    }
    case TypeLitTag:
    {
        int elemout = flowTempOut(out, ((IExpNode *)node)->vtype);
        for (nodesFor(((FnCallNode *)node)->args, cnt, nodesp))
            flowTempEscape((*nodesp)->tag == NamedValTag ? ((NamedValNode *)*nodesp)->val : *nodesp, elemout);
        return;
    }
    case AllocateTag:
        flowTempEscape(((RefNode *)node)->vtexp, flowTempOut(out, ((RefNode *)node)->vtype));
        return;
    case AssignTag:
        flowTempEscape(((AssignNode *)node)->lval, TempOutNone);
        flowTempEscape(((AssignNode *)node)->rval, TempOutBorrow);
        return;
    case RefCountTag:
        flowTempEscape(((RefCountNode *)node)->exp, out);
        return;
    case HollowTag:
        if (((HollowNode *)node)->exp)
            flowTempEscape(((HollowNode *)node)->exp, out);
        return;
    // A branch's statements are walked as their block's; only the conditions
    // are this statement's
    case IfTag:
        for (nodesFor(((IfNode *)node)->condblk, cnt, nodesp)) {
            if (*nodesp != elseCond)
                flowTempEscape(*nodesp, TempOutNone);
            nodesp++; cnt--;
        }
        return;
    default:
        return;
    }
}


// Load a reference that a value is about to be read through, and refuse the
// read when the reference's permission grants none. The reference's own
// permission governs what may be done through it, whatever the permission of
// the binding that holds it -- the read-side twin of the MayWrite test in
// assignlvalrtype. A pointer carries no permission and is not checked here.
void flowLoadThroughRef(FlowState *fstate, INode **refp) {
    flowLoadValue(fstate, refp);
    // A temporary read through, or read a field or an element of, is
    // finalized once its statement is done with it: '*make()', 'make().x'
    flowTempRead(refp);
    RefNode *reftype = (RefNode *)iexpGetTypeDcl(*refp);
    if ((reftype->tag == RefTag || reftype->tag == ArrayRefTag || reftype->tag == VirtRefTag)
        && !(permGetFlags(reftype->perm) & MayRead)) {
        if (permIsLock(reftype->perm))
            permLockRefused(*refp, reftype->perm, "read");
        else
            errorMsgNode(*refp, ErrorNoRead, "This reference's permission does not allow reading the value it points to");
    }
}

// An initializer's 'self &new' is the one path to memory that holds no value
// until the init writes one, and that the construction running the init holds
// once it returns. So it is reached only through itself, and only once filled:
// '*self' read or written, a field of it, a method called on it, each refused
// before '*self = value' has filled it on every path (VarUnfilled, joined by
// union as every flag is). Any other use -- passed, stored, copied, returned --
// is refused, as it would let the reference outlive the init.
int flowThroughSelf = 0;

VarDclNode *flowNewSelf(INode *node) {
    while (node->tag == CastTag)
        node = ((CastNode *)node)->exp;
    if (!isNameUseNode(node) || !isExpNode(node))
        return NULL;
    VarDclNode *var = (VarDclNode *)((NameUseNode *)node)->dclnode;
    if (var == NULL || var->tag != VarDclTag || var->namesym != selfName)
        return NULL;
    RefNode *type = (RefNode *)itypeGetTypeDcl(var->vtype);
    return type->tag == RefTag && type->perm && itypeGetTypeDcl(type->perm) == (INode *)newPerm ? var : NULL;
}

void flowNewSelfThrough(FlowState *fstate, INode **selfp) {
    int svthrough = flowThroughSelf;
    flowThroughSelf = 1;
    flowLoadValue(fstate, selfp);
    flowThroughSelf = svthrough;
}

int flowNewSelfFill(INode *lval) {
    if (lval->tag != DerefTag)
        return 0;
    VarDclNode *self = flowNewSelf(((StarNode *)lval)->vtexp);
    if (self == NULL || !(self->flowtempflags & VarUnfilled))
        return 0;
    // What it held was never a value, so nothing is finalized as it is replaced
    lval->flags |= FlagFirstAssign;
    flowVarSetFlags(self, self->flowtempflags & (0xFFFF - VarUnfilled), self->hollowed);
    return 1;
}

// Reported at 'self', once: the return at the end of a body is one the
// compiler wrote, with no place in the source of its own
void flowNewSelfReturn(FlowState *fstate, INode *at) {
    Nodes *parms = fstate->fnsig->parms;
    VarDclNode *self = parms->used ? (VarDclNode *)nodesGet(parms, 0) : NULL;
    if (self == NULL || !(self->flowtempflags & VarUnfilled))
        return;
    errorMsgNode((INode *)self, ErrorInitSelf,
        "This init can return before '*self = value' fills self on every path, which would leave its construction holding no value.");
    self->flowtempflags &= 0xFFFF - VarUnfilled;
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
    case AllocateTag:
        allocateFlow(fstate, (RefNode **)nodep);
        break;
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        uint32_t index = 0;
        uint16_t inflight = fstate->inflightcnt;
        for (nodesFor(((TupleNode *)*nodep)->elems, cnt, nodesp)) {
            flowLoadValue(fstate, nodesp);
            flowGateOperand(fstate, *nodesp);
        }
        flowGateOperandsEnd(fstate, inflight);
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
        if ((*nodep)->tag == CastTag)
            flowGateBoxed(fstate, *nodep);
        // A lock's guard is a new owner of the value its operand points at:
        // the operand is copied in, counted, or a temporary moved in
        // (borrowLockPlace), and the guard is the temporary
        if ((*nodep)->tag == CastTag && ((*nodep)->flags & FlagLockAcquire)) {
            flowHandleMoveOrCopy(&((CastNode *)*nodep)->exp);
            break;
        }
        // An operand the cast does not hand on -- an owner lent as a borrowed
        // reference, a value tested or converted -- is a temporary
        if ((*nodep)->tag == IsTag || !flowCastHandsOn(*nodep))
            flowTempRead(&((CastNode *)*nodep)->exp);
        break;
    case TempTag:
        break;
    case NotLogicTag:
        flowLoadValue(fstate, &((LogicNode *)*nodep)->lexp);
        break;
    case OrLogicTag: case AndLogicTag:
    {
        LogicNode *lnode = (LogicNode*)*nodep;
        flowLoadValue(fstate, &lnode->lexp);
        // The right operand runs on one path only
        ++flowDepth;
        flowLoadValue(fstate, &lnode->rexp);
        --flowDepth;
        break;
    }

    case TypeLitTag:
        typeLitFlow(fstate, (FnCallNode**)nodep);
        break;

    case ArrayLitTag:
        arrayLitFlow(fstate, (ArrayNode**)nodep);
        break;

    // A 'null' reaches here typed by every position that wants a value. One
    // whose value nothing wanted was never told which pointer it is.
    case NullLitTag:
        litAdoptNullType(nodep, unknownType);
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
    varnode->flowdepth = flowDepth;
}

// Start a new scope
size_t flowScopePush() {
    return gVarFlowStackPos;
}

// Is this variable where a part handed back is taken from? The same walk
// inwards, through elements and owning dereferences, that flowMoveSource
// takes to the variable it deactivates; a field is never on it, since nothing
// moves out of a field. 1 says the part is of the variable's own value; 2 says
// it was taken out through the variable, a local owning reference, which is
// then hollowed rather than handed back.
static int flowIsScopeResultOwner(INode *exp, VarDclNode *varnode) {
    INode *inner;
    switch (exp->tag) {
    case ArrIndexTag:
        inner = ((FnCallNode *)exp)->objfn; break;
    case DerefTag:
        inner = ((StarNode *)exp)->vtexp; break;
    case CastTag:
        return flowCastCarries(exp) ? flowIsScopeResultOwner(((CastNode *)exp)->exp, varnode) : 0;
    default:
        return isNameUseNode(exp) && isExpNode(exp) && ((NameUseNode *)exp)->dclnode == (INode *)varnode;
    }
    if (flowOwningLocal(inner) == varnode)
        return 2;
    return flowIsScopeResultOwner(inner, varnode);
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
// A part handed back out of what a local owning reference points at leaves the
// variable owning the allocation: it is not exempt, and the part is added to
// 'hollow', so that its release gives the memory back without the part.
static int flowIsScopeResult(INode *retexp, VarDclNode *varnode, Nodes **hollow) {
    if (retexp == NULL)
        return 0;
    if (retexp->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)retexp)->elems, cnt, elemp)) {
            if (flowIsScopeResult(*elemp, varnode, hollow))
                return 1;
        }
        return 0;
    }
    // A recast hands back its operand: a local returned as its enrichment or
    // base, and an owner returned as an owning virtual reference
    if (retexp->tag == CastTag && flowCastCarries(retexp))
        return flowIsScopeResult(((CastNode *)retexp)->exp, varnode, hollow);
    // A move-typed element handed back moves out of the variable that holds
    // it, which as for any move out of an element no longer owns the whole:
    // releasing it would finalize the element a second time, in the caller. A
    // copied element leaves the variable owning everything it held. A field
    // handed back was refused (flowRefuseMoveField), so it exempts nothing.
    if ((retexp->tag == ArrIndexTag || retexp->tag == DerefTag) && iexpIsMove(retexp)) {
        int owner = flowIsScopeResultOwner(retexp, varnode);
        if (owner == 2) {
            if (*hollow == NULL)
                *hollow = newNodes(2);
            nodesAdd(hollow, retexp);
            return 0;
        }
        // A dereference handed back is exempt only as a hollowing: what a
        // value's own element points at still belongs to that value's release
        return retexp->tag == DerefTag ? 0 : owner;
    }
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
                && flowIsScopeResult(((BreakRetNode *)last)->exp, varnode, hollow))
                return 1;
            if (blk->breaks) {
                for (nodesFor(blk->breaks, cnt, nodesp)) {
                    if ((*nodesp)->tag == BreakTag
                        && flowIsScopeResult(((BreakRetNode *)*nodesp)->exp, varnode, hollow))
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
                if (flowIsScopeResult(*nodesp, varnode, hollow))
                    return 1;
            }
            return 0;
        }
        default:
            break;
        }
    }
    if (!(isNameUseNode(retexp) && isExpNode(retexp)))
        return 0;
    INode *named = ((NameUseNode *)retexp)->dclnode;
    if (named == (INode *)varnode)
        return 1;
    // A match's binding handed back hands back the matched value
    INode *matched = flowMatchBound(named);
    return matched ? flowIsScopeResult(matched, varnode, hollow) : 0;
}

// A hollow release of a variable, as it stands now: the moves that hollowed it
// so far are copied, since a later exit or reassignment sees a different set
HollowNode *flowNewHollow(VarDclNode *var) {
    HollowNode *hnode;
    newNode(hnode, HollowNode, HollowTag);
    hnode->vtype = (INode *)newVoidNode();
    hnode->exp = NULL;
    hnode->var = var;
    hnode->moved = newNodes(2);
    hnode->test = 0;
    if (var->flowtempflags & VarHollow) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(var->hollowed, cnt, nodesp))
            nodesAdd(&hnode->moved, *nodesp);
    }
    return hnode;
}

// A release that runs only when the variable's drop flag holds 'state'
static INode *flowDropFlagTest(VarDclNode *var, INode *release, uint8_t state) {
    DropFlagNode *test;
    newNode(test, DropFlagNode, DropFlagTag);
    test->vtype = (INode *)newVoidNode();
    test->release = release;
    test->var = var;
    test->state = state;
    return (INode *)test;
}

static void flowListAdd(Nodes **varlist, INode *node) {
    if (*varlist == NULL)
        *varlist = newNodes(4);
    nodesAdd(varlist, node);
}

void flowVarRelease(VarDclNode *var, INode *dropat, int whole, int hollow, Nodes *hollowed, Nodes *extra,
    int test, Nodes **varlist) {
    INode **nodesp;
    uint32_t cnt;
    if (hollow) {
        // A local owning reference out of whose referent a part was moved, or
        // is handed back here, is released without what moved
        HollowNode *hnode = flowNewHollow(var);
        hnode->moved = newNodes(2);
        if (hollowed) {
            for (nodesFor(hollowed, cnt, nodesp))
                nodesAdd(&hnode->moved, *nodesp);
        }
        if (extra) {
            for (nodesFor(extra, cnt, nodesp))
                nodesAdd(&hnode->moved, *nodesp);
        }
        flowListAdd(varlist, test ? flowDropFlagTest(var, (INode *)hnode, DropFlagHollow) : (INode *)hnode);
    }
    if (!whole)
        return;
    // A struct or an enum dies through its drop: a call to it is listed.
    // Anything else with anything to do as it dies -- an owning reference,
    // a tuple or an array of values that finalize or own -- is listed
    // itself, and generation finalizes it in place (genlFinalizeAt).
    INode *vartype = var->vtype;
    INode *dropfn = itypeGetDropFnDcl(vartype);
    INode *release = NULL;
    if (dropfn == NULL) {
        if (itypeNeedsFinal(vartype))
            release = (INode *)var;
    }
    else {
        FnCallNode *dropfncall = newFnCallLower(dropat, dropfn, 1);
        INode *dropnameuse = (INode*)newNameUseFromDclNode((INode*)var, dropat);
        INode *borrow = newBorrowMutRef(dropnameuse, ((IExpNode*)var)->vtype, (INode*)uniPerm);
        nodesAdd(&dropfncall->args, borrow);
        release = (INode *)dropfncall;
    }
    if (release)
        flowListAdd(varlist, test ? flowDropFlagTest(var, release, DropFlagWhole) : release);
}

int flowIsScopeResultOf(INode *retexp, VarDclNode *varnode, Nodes **hollow) {
    return flowIsScopeResult(retexp, varnode, hollow);
}

// Does the scope being left (the variables from 'startpos' up) hand one of its
// own variables back whole as 'retexp'? Such a variable is exempt from the
// scope's release (flowScopeDealias), so its holder goes to the receiver as it
// is; any other copy handed out of the scope is a new holder.
int flowScopeHandsBack(size_t startpos, INode *retexp) {
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        Nodes *hollow = NULL;
        if (flowIsScopeResult(retexp, avar->node, &hollow))
            return 1;
    }
    return 0;
}

// Create de-alias list of all own/Rc reference variables (except the retexp name(s))
// A drop call built here is positioned on the result expression, and on 'lexnode'
// -- the jump that ends the scope -- where there is no result expression to take
// a position from. A 'continue' hands back no value, so it is the jump or nothing.
void flowScopeDealias(size_t startpos, Nodes **varlist, INode *retexp, INode *lexnode) {
    INode *dropat = retexp != NULL ? retexp : lexnode;
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        // A variable that was never given a value owns nothing, so there is
        // nothing to release or finalize: freeing its storage, or running a
        // drop fn over it, would act on garbage.
        if (!(avar->node->flowtempflags & VarInitialized))
            continue;
        // A match's binding owns nothing: the matched value's variable does
        if (flowMatchBound((INode *)avar->node))
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
        Nodes *hollow = NULL;
        if (flowIsScopeResult(retexp, avar->node, &hollow))
            continue;
        int hollowed = (avar->node->flowtempflags & VarHollow) || hollow;
        flowVarRelease(avar->node, dropat, !hollowed, hollowed,
            (avar->node->flowtempflags & VarHollow) ? avar->node->hollowed : NULL, hollow, 0, varlist);
    }
}

// Back out of current scope
void flowScopePop(size_t startpos) {
    gVarFlowStackPos = startpos;
}

// *********************
// The gate: which functions hold a borrow in a way only a walk following each
// path could check. It is set as this walk goes, at O(1) per node (a type's
// answer is remembered, itypeCarriesBorrow), and nothing reads it yet.
// *********************

int flowGateCountAll = 0;
int flowGpu = 0;

// Functions walked, functions gated, and functions each trigger fired in
static uint32_t flowGateFns = 0;
static uint32_t flowGateGated = 0;
static uint32_t flowGateByTrigger[5] = { 0, 0, 0, 0, 0 };

// Is this type (a declaration) a bare borrowed reference?
static int flowGateIsBorrowRef(INode *typedcl) {
    return (typedcl->tag == RefTag || typedcl->tag == ArrayRefTag || typedcl->tag == VirtRefTag)
        && itypeGetTypeDcl(((RefNode *)typedcl)->region) == borrowRef;
}

// A name standing for a type, resolved to what it names; any other type node
// as it is
static INode *flowGateTypeDcl(INode *type) {
    return (type->tag == NameUseTag || type->tag == AliasDclTag) && isTypeNode(type)
        ? itypeGetTypeDcl(type) : type;
}

void flowStateInit(FlowState *fstate, FnSigNode *fnsig) {
    fstate->fnsig = fnsig;
    fstate->scope = 1;
    fstate->gate = 0;
    fstate->inflightcnt = 0;
    fstate->dropgate = 0;
    fstate->jumped = 0;
}

// A value handed out that carries a borrow: what it holds, and so how long it
// may live, only the walk knows -- a bare borrowed reference's scope number
// does not follow a borrow through a variable, a value holding it, or a call's
// by-value argument
void flowGateResultAsk(FlowState *fstate, INode *type) {
    if (itypeCarriesBorrow(flowGateTypeDcl(type)))
        fstate->gate |= FlowGateResult;
}

void flowGateCallAsk(FlowState *fstate, Nodes *args) {
    INode **argsp;
    uint32_t cnt;
    INode *storer = NULL;
    for (nodesFor(args, cnt, argsp)) {
        INode *type = ((IExpNode *)*argsp)->vtype;
        if (type->tag != RefTag && type->tag != NameUseTag && type->tag != AliasDclTag)
            continue;
        RefNode *argtype = (RefNode *)flowGateTypeDcl(type);
        if (argtype->tag == RefTag && itypeGetTypeDcl(argtype->region) == borrowRef
            && (permGetFlags(argtype->perm) & MayWrite) && itypeCarriesBorrow(argtype->vtexp)) {
            storer = *argsp;
            break;
        }
        // A value holding a writable borrow ('st(w, v)', 'w' a struct holding
        // a '&mut &R') may be stored through as well
        if (argtype->tag == StructTag && itypeWritableBorrowDepth((INode *)argtype, 1)) {
            storer = *argsp;
            break;
        }
    }
    if (storer == NULL)
        return;
    for (nodesFor(args, cnt, argsp)) {
        if (*argsp != storer && itypeCarriesBorrow(((IExpNode *)*argsp)->vtype)) {
            fstate->gate |= FlowGateStore;
            return;
        }
    }
}

// The variable at the root of a borrowed place, or NULL for one not rooted in
// a variable. A place reached through a reference is keyed by that reference.
static VarDclNode *flowGatePlaceRoot(INode *place) {
    while (1) {
        if (isNameUseNode(place) && isExpNode(place)) {
            INode *dcl = ((NameUseNode *)place)->dclnode;
            return dcl->tag == VarDclTag ? (VarDclNode *)dcl : NULL;
        }
        switch (place->tag) {
        case FldAccessTag:
        case ArrIndexTag:
            place = ((FnCallNode *)place)->objfn;
            break;
        case DerefTag:
            place = ((StarNode *)place)->vtexp;
            break;
        case CastTag:
            place = ((CastNode *)place)->exp;
            break;
        default:
            return NULL;
        }
    }
}

// An owning reference recast to a borrowed one of the same kind: the lend a
// coercion makes of an owner wanted as a '&' or '&mut' (or a '&<' or '&<mut'),
// which the loan walk reads as a borrow of what the owner owns (pwIsOwnedLent)
static int flowGateIsOwnedLent(CastNode *cast) {
    if (cast->flags & FlagConvert)
        return 0;
    INode *to = iexpGetTypeDcl((INode *)cast);
    INode *from = iexpGetTypeDcl(cast->exp);
    return (to->tag == RefTag || to->tag == VirtRefTag) && from->tag == to->tag
        && flowGateIsBorrowRef(to) && !flowGateIsBorrowRef(from);
}

void flowGateOperandAsk(FlowState *fstate, INode *operand) {
    // A borrow written or built as if written ('&mut *o'), or an owner lent
    // implicitly by a recast: either way a borrow waiting for its call
    INode *place = NULL;
    while (operand->tag == CastTag) {
        if (flowGateIsOwnedLent((CastNode *)operand)) {
            place = ((CastNode *)operand)->exp;
            break;
        }
        operand = ((CastNode *)operand)->exp;
    }
    if (place == NULL) {
        if (operand->tag != BorrowTag && operand->tag != ArrayBorrowTag)
            return;
        place = ((RefNode *)operand)->vtexp;
    }
    VarDclNode *root = flowGatePlaceRoot(place);
    if (root == NULL)
        return;
    if (fstate->inflightcnt == FlowInflightMax) {
        fstate->gate |= FlowGateInCall;
        return;
    }
    fstate->inflight[fstate->inflightcnt++] = root;
}

// A value whose type carries a borrow made an owning virtual reference: the
// loan walk checks that every borrow it holds is global (pwValue)
void flowGateBoxedAsk(FlowState *fstate, INode *cast) {
    if (flowCastCarries(cast) && itypeCarriesBorrow(((IExpNode *)((CastNode *)cast)->exp)->vtype))
        fstate->gate |= FlowGateBoxed;
}

void flowGateUse(FlowState *fstate, VarDclNode *var) {
    for (uint16_t i = 0; i < fstate->inflightcnt; ++i) {
        if (fstate->inflight[i] == var) {
            fstate->gate |= FlowGateInCall;
            return;
        }
    }
}

void flowGateCount(FlowState *fstate) {
    ++flowGateFns;
    if (fstate->gate)
        ++flowGateGated;
    for (int bit = 0; bit < 5; ++bit) {
        if (fstate->gate & (1 << bit))
            ++flowGateByTrigger[bit];
    }
}

void flowGatePrint() {
    printf("Flow gate: %u of %u functions (holder %u, result %u, store %u, in-call %u, boxed %u)\n\n",
        flowGateGated, flowGateFns, flowGateByTrigger[0], flowGateByTrigger[1],
        flowGateByTrigger[2], flowGateByTrigger[3], flowGateByTrigger[4]);
}
