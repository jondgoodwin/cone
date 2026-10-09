/** 'parallel each': building its loop, and the rules its body keeps
 * @file
 *
 * See pareach.h for the shape this builds and what generation does with it
 * (genlpar.c). The counted loop is eachLower's (each.c) over an index range
 * [lo, hi) instead of the whole of the source, so that a piece can be run on
 * any part of it.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

Name *parLoName = NULL;
Name *parHiName = NULL;
Name *parKName = NULL;
Name *parFirstName = NULL;
Name *parLastName = NULL;
FnDclNode *parallelEachFn = NULL;

void parallelEachNames() {
    if (parLoName != NULL)
        return;
    parLoName = nametblPrivate("lo'", 3);
    parHiName = nametblPrivate("hi'", 3);
    parKName = nametblPrivate("k'", 2);
    parFirstName = nametblPrivate("first'", 6);
    parLastName = nametblPrivate("last'", 5);
}

// ---- Node builders (as each.c's, which are its own) --------------------------

static INode *parUse(VarDclNode *var, INode *lexnode) {
    NameUseNode *use = newNameUseNode(var->namesym);
    inodeLexCopy((INode*)use, lexnode);
    use->dclnode = (INode*)var;
    return (INode*)use;
}

static VarDclNode *parVar(Name *name, PermNode *perm, INode *val, uint16_t scope, INode *lexnode) {
    VarDclNode *var = newVarDclFull(name, VarDclTag, unknownType, (INode*)perm, val);
    inodeLexCopy((INode*)var, lexnode);
    var->scope = scope;
    return var;
}

static FnCallNode *parField(INode *recv, Name *name, INode *lexnode) {
    FnCallNode *access = newFnCallLower(lexnode, recv, 0);
    access->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(access->methfld, lexnode);
    return access;
}

static FnCallNode *parCall(INode *recv, Name *name, INode *lexnode) {
    FnCallNode *call = newFnCallLower(lexnode, recv, 1);
    call->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(call->methfld, lexnode);
    return call;
}

// 'if cond {break}', leaving 'loop'
static INode *parBreakIf(INode *cond, BlockNode *loop, INode *lexnode) {
    BreakRetNode *brk = newBreakNode();
    inodeLexCopy((INode*)brk, lexnode);
    brk->exp = (INode*)newNilLitNode();
    inodeLexCopy(brk->exp, lexnode);
    brk->block = loop;
    BlockNode *ifblk = newBlockNode();
    inodeLexCopy((INode*)ifblk, lexnode);
    nodesAdd(&ifblk->stmts, (INode*)brk);
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, lexnode);
    nodesAdd(&ifnode->condblk, cond);
    nodesAdd(&ifnode->condblk, (INode*)ifblk);
    return (INode*)ifnode;
}

static int parHasMethod(INode *type, Name *name) {
    return isMethodType(type) && iNsTypeFindFnField((INsTypeNode*)type, name) != NULL;
}

// A place that checking a second time leaves as it is (each.c, eachRecheckable)
static int parRecheckable(INode *node) {
    if (!iexpIsLval(node))
        return 0;
    if (isNameUseNode(node))
        return nameUseNames(node, VarDclTag);
    switch (node->tag) {
    case FldAccessTag:
    case ArrIndexTag:
        return 1;
    case DerefTag:
        return parRecheckable(((StarNode*)node)->vtexp);
    default:
        return 0;
    }
}

// How many variables the reader declared: the loop's leading declarations that
// have neither a value nor a type
static uint32_t parVarCount(BlockNode *loop) {
    uint32_t n = 0;
    while (n < loop->stmts->used) {
        INode *stmt = nodesGet(loop->stmts, n);
        if (stmt->tag != VarDclTag || ((VarDclNode*)stmt)->value != NULL
            || ((VarDclNode*)stmt)->vtype != unknownType)
            break;
        ++n;
    }
    return n;
}

// ---- A walk over a body ------------------------------------------------------

// Visit every node of a statement or expression, the node first. A visit that
// answers 0 does not have its node's parts visited.
typedef int (*ParVisit)(INode *node, void *ctx);

static void parWalk(INode *node, ParVisit visit, void *ctx);

static void parWalkNodes(Nodes *nodes, ParVisit visit, void *ctx) {
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp))
        parWalk(*nodesp, visit, ctx);
}

static void parWalk(INode *node, ParVisit visit, void *ctx) {
    if (node == NULL || !visit(node, ctx))
        return;
    switch (node->tag) {
    case VarDclTag:
        parWalk(((VarDclNode*)node)->value, visit, ctx); break;
    case BlockTag:
        parWalkNodes(((BlockNode*)node)->stmts, visit, ctx); break;
    case IfTag:
        parWalkNodes(((IfNode*)node)->condblk, visit, ctx); break;
    case BreakTag:
    case ContinueTag:
    case BlockRetTag:
    case ReturnTag:
        parWalk(((BreakRetNode*)node)->exp, visit, ctx); break;
    case AssignTag:
        parWalk(((AssignNode*)node)->lval, visit, ctx);
        parWalk(((AssignNode*)node)->rval, visit, ctx);
        break;
    case SwapTag:
        parWalk(((SwapNode*)node)->lval, visit, ctx);
        parWalk(((SwapNode*)node)->rval, visit, ctx);
        break;
    case VTupleTag:
        parWalkNodes(((TupleNode*)node)->elems, visit, ctx); break;
    case ArrayLitTag:
        parWalkNodes(((ArrayNode*)node)->dimens, visit, ctx);
        parWalkNodes(((ArrayNode*)node)->elems, visit, ctx);
        break;
    case FnCallTag:
    case ArrIndexTag:
    case FldAccessTag:
    case TypeLitTag:
        parWalk(((FnCallNode*)node)->objfn, visit, ctx);
        parWalkNodes(((FnCallNode*)node)->args, visit, ctx);
        break;
    case CastTag:
    case IsTag:
        parWalk(((CastNode*)node)->exp, visit, ctx); break;
    case DerefTag:
        parWalk(((StarNode*)node)->vtexp, visit, ctx); break;
    case BorrowTag:
    case ArrayBorrowTag:
    case AllocateTag:
        parWalk(((RefNode*)node)->vtexp, visit, ctx); break;
    case NotLogicTag:
        parWalk(((LogicNode*)node)->lexp, visit, ctx); break;
    case OrLogicTag:
    case AndLogicTag:
        parWalk(((LogicNode*)node)->lexp, visit, ctx);
        parWalk(((LogicNode*)node)->rexp, visit, ctx);
        break;
    case AwaitTag:
        parWalk(((AwaitNode*)node)->exp, visit, ctx); break;
    case NamedValTag:
        parWalk(((NamedValNode*)node)->val, visit, ctx); break;
    case OfEntryTag: case FillEntryTag: case PairEntryTag:
        parWalk(((EntryNode*)node)->first, visit, ctx);
        parWalk(((EntryNode*)node)->val, visit, ctx);
        break;
    case RefCountTag:
        parWalk(((RefCountNode*)node)->exp, visit, ctx); break;
    case HollowTag:
        parWalk(((HollowNode*)node)->exp, visit, ctx); break;
    case DropFlagTag:
        parWalk(((DropFlagNode*)node)->release, visit, ctx); break;
    case TempTag:
        parWalk(((TempNode*)node)->exp, visit, ctx); break;
    default:
        break;
    }
}

// A set of nodes, searched in a line: a body holds few
typedef struct {
    INode **items;
    uint32_t cnt;
    uint32_t max;
} ParSet;

static void parSetAdd(ParSet *set, INode *node) {
    if (set->cnt == set->max) {
        uint32_t newmax = set->max ? set->max * 2 : 16;
        INode **grown = (INode **)memAllocBlk(newmax * sizeof(INode *));
        if (set->cnt)
            memcpy(grown, set->items, set->cnt * sizeof(INode *));
        set->items = grown;
        set->max = newmax;
    }
    set->items[set->cnt++] = node;
}

static int parSetHas(ParSet *set, INode *node) {
    for (uint32_t i = 0; i < set->cnt; ++i) {
        if (set->items[i] == node)
            return 1;
    }
    return 0;
}

// ---- The rules about control, checked as written -----------------------------

typedef struct {
    BlockNode *loop;
    ParSet blocks;      // Every block inside the body
} ParControl;

static int parCollectBlocks(INode *node, void *ctx) {
    if (node->tag == BlockTag)
        parSetAdd(&((ParControl *)ctx)->blocks, node);
    return 1;
}

static int parCheckControl(INode *node, void *ctxp) {
    ParControl *ctx = (ParControl *)ctxp;
    switch (node->tag) {
    case AwaitTag:
        errorMsgNode(node, ErrorParControl,
            "'await' is not allowed in a 'parallel each' body: a piece runs to completion on a worker and has no mailbox to wait on. Await before the loop, or after it.");
        break;
    case ReturnTag:
        errorMsgNode(node, ErrorParControl,
            "'return' is not allowed in a 'parallel each' body: the passes run at the same time, so which of them returned first would vary from run to run. Finish the loop, then return.");
        break;
    case BreakTag: {
        BlockNode *target = ((BreakRetNode*)node)->block;
        if (target != NULL && !parSetHas(&ctx->blocks, (INode*)target))
            errorMsgNode(node, ErrorParControl,
                "'break' is not allowed in a 'parallel each' body: which items had already run when it was reached would vary from run to run. 'continue' skips the rest of an item and is allowed.");
        break;
    }
    case ContinueTag: {
        BlockNode *target = ((BreakRetNode*)node)->block;
        if (target != NULL && target != ctx->loop && !parSetHas(&ctx->blocks, (INode*)target))
            errorMsgNode(node, ErrorParControl,
                "'continue' in a 'parallel each' body may continue this loop, or a loop inside it, but not a loop around it.");
        break;
    }
    default:
        break;
    }
    return 1;
}

// ---- The rules about writing, checked once the body has its types ------------

typedef struct {
    ParSet inside;      // The variables declared in the body, and the loop's own
    ParSet reported;    // The nodes already refused
} ParWrites;

static int parCollectDcls(INode *node, void *ctx) {
    if (node->tag == VarDclTag)
        parSetAdd(&((ParWrites *)ctx)->inside, node);
    return 1;
}

// Is this a raw pointer? What is written through one is its owner's to answer for
static int parIsRawPtr(INode *exp) {
    if (!isExpNode(exp))
        return 0;
    INode *type = iexpGetTypeDcl(exp);
    return type != NULL && type->tag == PtrTag;
}

// The variable a written place belongs to: the name at its root, through
// fields, elements and references, or NULL where the place is no variable's
// (a computed address, a call's result) or is reached through a raw pointer
static VarDclNode *parWriteRoot(INode *node) {
    while (node != NULL) {
        if (isNameUseNode(node)) {
            INode *dcl = ((NameUseNode*)node)->dclnode;
            return dcl != NULL && dcl->tag == VarDclTag ? (VarDclNode*)dcl : NULL;
        }
        switch (node->tag) {
        case FldAccessTag:
        case ArrIndexTag:
            if (parIsRawPtr(((FnCallNode*)node)->objfn))
                return NULL;
            node = ((FnCallNode*)node)->objfn;
            break;
        case DerefTag:
            if (parIsRawPtr(((StarNode*)node)->vtexp))
                return NULL;
            node = ((StarNode*)node)->vtexp;
            break;
        case BorrowTag:
        case ArrayBorrowTag:
            node = ((RefNode*)node)->vtexp;
            break;
        case CastTag:
            node = ((CastNode*)node)->exp;
            break;
        default:
            return NULL;
        }
    }
    return NULL;
}

static void parRefuseWrite(ParWrites *ctx, INode *site, INode *place, const char *how) {
    VarDclNode *root = parWriteRoot(place);
    if (root == NULL || parSetHas(&ctx->inside, (INode*)root) || parSetHas(&ctx->reported, site))
        return;
    parSetAdd(&ctx->reported, site);
    char *name = root->namesym == anonName ? "a value made outside the loop" : &root->namesym->namestr;
    errorMsgNode(site, ErrorParWrite,
        "A 'parallel each' body may not %s %s, which is declared outside the loop: the passes run at the same time. Outside variables are read-only for the loop. Hand results back with the parallel builder, or 'mutItems()' on the items, or count with an Atomic.",
        how, name);
}

static int parCheckWrites(INode *node, void *ctxp) {
    ParWrites *ctx = (ParWrites *)ctxp;
    switch (node->tag) {
    case AssignTag:
        parRefuseWrite(ctx, node, ((AssignNode*)node)->lval, "write");
        break;
    case SwapTag:
        parRefuseWrite(ctx, node, ((SwapNode*)node)->lval, "swap");
        parRefuseWrite(ctx, node, ((SwapNode*)node)->rval, "swap");
        break;
    case BorrowTag:
    case ArrayBorrowTag: {
        INode *perm = ((RefNode*)node)->perm;
        if (perm != NULL && perm != (INode*)unknownType && (permGetFlags(perm) & MayWrite))
            parRefuseWrite(ctx, node, ((RefNode*)node)->vtexp, "lend for writing");
        break;
    }
    case FnCallTag: {
        // A message sent to an actor (a call of its handle's method)
        INode *fn = ((FnCallNode*)node)->objfn;
        if (fn != NULL && isNameUseNode(fn) && ((NameUseNode*)fn)->dclnode != NULL
            && ((NameUseNode*)fn)->dclnode->tag == FnDclTag) {
            ActorInfo *callee;
            if (actorMessageOfSend((FnDclNode *)((NameUseNode*)fn)->dclnode, &callee) != NULL)
                errorMsgNode(node, ErrorParSend,
                    "A 'parallel each' body may not send a message to an actor yet. The sends are to be gathered per piece and made in iteration order after the loop, which is not built; collect what is to be sent in a list, and send after the loop.");
        }
        break;
    }
    default:
        break;
    }
    return 1;
}

// The loop of a built parallel each, its last loop block
static BlockNode *parLoopOf(BlockNode *outer) {
    for (uint32_t i = outer->stmts->used; i > 0; --i) {
        INode *stmt = nodesGet(outer->stmts, i - 1);
        // (type check makes the loop, the block's last statement, its value)
        if (stmt->tag == BlockRetTag)
            stmt = ((BreakRetNode*)stmt)->exp;
        if (stmt->tag == BlockTag && (stmt->flags & FlagLoop))
            return (BlockNode *)stmt;
    }
    return NULL;
}

void parallelEachCheckBody(TypeCheckState *pstate, BlockNode *outer) {
    BlockNode *loop = parLoopOf(outer);
    if (loop == NULL)
        return;
    ParWrites ctx;
    memset(&ctx, 0, sizeof(ctx));
    // The loop's own variables are written by the loop: the index, and what the
    // body declares
    for (uint32_t i = 0; i < outer->stmts->used; ++i) {
        INode *stmt = nodesGet(outer->stmts, i);
        if (stmt->tag == VarDclTag)
            parSetAdd(&ctx.inside, stmt);
    }
    parWalk((INode*)loop, parCollectDcls, &ctx);
    parWalk((INode*)loop, parCheckWrites, &ctx);
}

// ---- The loop ------------------------------------------------------------------

uint32_t parallelEachBoundsFirst(BlockNode *outer) {
    if (outer->stmts->used != 3)
        return 0;
    INode *first = ((VarDclNode *)nodesGet(outer->stmts, 0))->value;
    INode *last = ((VarDclNode *)nodesGet(outer->stmts, 1))->value;
    int firstunk = first != NULL && first->tag == ULitTag && (first->flags & FlagUnkType);
    int lastunk = last != NULL && last->tag == ULitTag && (last->flags & FlagUnkType);
    return firstunk && !lastunk ? 1 : 0;
}

void parallelEachBoundType(BlockNode *outer, uint32_t first) {
    VarDclNode *known = (VarDclNode *)nodesGet(outer->stmts, first);
    VarDclNode *other = (VarDclNode *)nodesGet(outer->stmts, 1 - first);
    if (known->vtype != NULL && known->vtype != unknownType)
        other->vtype = known->vtype;
}

// The actors package a module imports, or NULL
static ModuleNode *parFindImport(Nodes *imports, char *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(imports, cnt, nodesp)) {
        ModuleNode *mod = ((ImportNode *)*nodesp)->module;
        if (mod && strcmp(&mod->namesym->namestr, name) == 0)
            return mod;
    }
    return NULL;
}

// Whether the loop can run where it is written, and the runtime function it is
// run by found: the module imports the actors package, and the function is not
// an actor's method (a loop there waits for D3's seam: not built)
static int parRuntime(TypeCheckState *pstate, INode *lexnode) {
    FnDclNode *fn = pstate->fn;
    ModuleNode *mod = fn ? dclInfoGetModule((INode*)fn) : NULL;
    if (mod == NULL) {
        errorMsgNode(lexnode, ErrorParRuntime, "A 'parallel each' may only be written in a function.");
        return 0;
    }
    ModuleNode *actorsmod = parFindImport(mod->imports, "actors");
    INode *run = actorsmod ? namespaceFind(&actorsmod->namespace, nametblFind("parallelEach", 12)) : NULL;
    if (run == NULL || run->tag != FnDclTag) {
        errorMsgNode(lexnode, ErrorParRuntime,
            "A 'parallel each' runs its passes on the actors package's workers, which module %s does not import: write 'import actors;' after the 'mod' line.",
            &mod->namesym->namestr);
        return 0;
    }
    if (actorOfState(inodeGetOwner((INode*)fn)) != NULL) {
        errorMsgNode(lexnode, ErrorParRuntime,
            "A 'parallel each' inside an actor's method is not built yet: the method must keep its actor from running again until the loop has finished, and the loop must borrow the actor's fields, which the language form does not do yet. Write it in a function outside actors for now.");
        return 0;
    }
    parallelEachFn = (FnDclNode *)run;
    return 1;
}

// The check that sends the loop's body through its control rules: before the
// loop is built, while the body is only what the reader wrote
static void parControlRules(BlockNode *loop) {
    ParControl ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;
    parWalkNodes(loop->stmts, parCollectBlocks, &ctx);
    parWalkNodes(loop->stmts, parCheckControl, &ctx);
}

static void parSourceError(INode *src, INode *type) {
    int cursor = isMethodType(type) && (parHasMethod(type, nextName) || parHasMethod(type, iterName));
    if (isMethodType(type) && parHasMethod(type, lenName) && parHasMethod(type, splitName))
        errorMsgNode(src, ErrorParSource,
            "A 'parallel each' over %s, which has 'len' and 'split' and so is a ParallelIterable, is not built yet: it walks arrays, slices, lists and number ranges.",
            itypeName(type));
    else if (cursor)
        errorMsgNode(src, ErrorParSource,
            "A 'parallel each' splits its source into pieces that run at the same time, so the source must report its size and split ('len' and 'split': a ParallelIterable). %s hands out its items one after another, with 'next', and cannot be split. Collect it into a list first, and walk the list.",
            itypeName(type));
    else
        errorMsgNode(src, ErrorParSource,
            "A 'parallel each' walks a number range ('parallel each i in 0 < n'), an array, a slice or a list, the sources that report their size and split into pieces, and %s is none of those. A generator, a file or a channel cannot be split: collect it into a list first.",
            itypeName(type));
}

void parallelEachLower(TypeCheckState *pstate, BlockNode *outer) {
    parallelEachNames();
    outer->flags &= 0xFFFF - FlagEach;
    int isrange = outer->stmts->used == 3;
    BlockNode *loop = (BlockNode*)nodesGet(outer->stmts, isrange ? 2 : 1);
    INode *lexnode = nodesGet(outer->stmts, 0);
    uint16_t scope = (uint16_t)pstate->scope;
    uint32_t nvars = parVarCount(loop);

    // Whatever stops the build leaves an empty block, and no parallel each
    VarDclNode *srcdcl = (VarDclNode*)nodesGet(outer->stmts, 0);
    VarDclNode *lastdcl = isrange ? (VarDclNode*)nodesGet(outer->stmts, 1) : NULL;
    outer->stmts->used = 0;
    outer->flags &= 0xFFFF - FlagParallel;

    if (!parRuntime(pstate, lexnode))
        return;
    parControlRules(loop);

    if (nvars != 1) {
        errorMsgNode(nodesGet(loop->stmts, 0), ErrorParSource,
            "A 'parallel each' gives one variable, a borrow of each element (or each number of a range).");
        return;
    }

    VarDclNode *lo, *hi, *k;
    INode *elem;

    if (isrange) {
        // The bounds were checked as the initializers of their variables
        INode *type = srcdcl->vtype;
        INode *typedcl = type != NULL ? itypeGetTypeDcl(type) : NULL;
        if (srcdcl->value == NULL || lastdcl->value == NULL || inodeIsError(srcdcl->value) || inodeIsError(lastdcl->value)
            || typedcl == NULL || typedcl == unknownType || typedcl == errorType)
            return;
        if (typedcl->tag != IntNbrTag && typedcl->tag != UintNbrTag) {
            errorMsgNode(srcdcl->value, ErrorParSource,
                "A 'parallel each' counts through a range of whole numbers, and %s is not one.", itypeName(type));
            return;
        }
        nodesAdd(&outer->stmts, (INode*)srcdcl);
        nodesAdd(&outer->stmts, (INode*)lastdcl);
        lo = parVar(parLoName, immPerm, (INode*)newULitNodeTC(0, (INode*)usizeType), scope, lexnode);
        // The count is worked out where the loop is generated, from the bounds
        hi = parVar(parHiName, immPerm, (INode*)newULitNodeTC(0, (INode*)usizeType), scope, lexnode);
        // imm x = first + (k as T)
        FnCallNode *sum = newFnCallOpnameLower(lexnode, parUse(srcdcl, lexnode), plusName, 1);
        k = parVar(parKName, mutPerm, parUse(lo, lexnode), scope, lexnode);
        // ... where k is converted by the number type's own 'from'
        FnCallNode *conv = newFnCallLower(lexnode, newNameUseFromDclNode(typedcl, lexnode), 1);
        conv->methfld = (INode*)newMemberUseNode(fromName);
        inodeLexCopy(conv->methfld, lexnode);
        nodesAdd(&conv->args, parUse(k, lexnode));
        nodesAdd(&sum->args, (INode*)conv);
        elem = (INode*)sum;
        outer->flags |= FlagParRange | FlagParallel;
        nodesAdd(&outer->stmts, (INode*)lo);
        nodesAdd(&outer->stmts, (INode*)hi);
        nodesAdd(&outer->stmts, (INode*)k);
    }
    else {
        // What kind of source: an array, a slice, a type that lends the array it holds
        INode *src = srcdcl->value;
        if (src == NULL || inodeIsError(src) || !isExpNode(src))
            return;
        INode *type = iexpGetTypeDcl(src);
        if (type == errorType || type == unknownType)
            return;
        int isref = type->tag == RefTag || type->tag == VirtRefTag;
        INode *base = isref ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
        int isslice = type->tag == ArrayRefTag;
        int isarray = base->tag == ArrayTag;
        Name *lentvia = NULL;
        if (!isslice && !isarray && base->tag == StructTag) {
            StructNode *lentbody = structLentBody((StructNode*)base);
            if (lentbody != NULL && itypeIsArrayBody((INode*)lentbody))
                lentvia = structLentVia((StructNode*)base);
        }
        int islent = lentvia != NULL;
        if (!isslice && !isarray && !islent) {
            parSourceError(src, type);
            return;
        }
        int place = parRecheckable(src);

        // imm s = [the slice]
        VarDclNode *slicedcl = srcdcl;
        if (islent) {
            // The slice the type lends: its place's, or a value held first
            INode *owner = src;
            if (!place) {
                nodesAdd(&outer->stmts, (INode*)srcdcl);
                owner = parUse(srcdcl, lexnode);
            }
            slicedcl = parVar(anonName, immPerm, (INode*)parCall(owner, lentvia, lexnode), scope, lexnode);
        }
        else if (isarray) {
            INode *array = src;
            if (!place) {
                nodesAdd(&outer->stmts, (INode*)srcdcl);
                array = parUse(srcdcl, lexnode);
            }
            if (isref) {
                StarNode *deref = newStarNode(DerefTag);
                inodeLexCopy((INode*)deref, lexnode);
                deref->vtexp = array;
                array = (INode*)deref;
            }
            slicedcl = parVar(anonName, immPerm, NULL, scope, lexnode);
            slicedcl->value = (INode*)newRefNodeFull(ArrayBorrowTag, lexnode, borrowRef, unknownType, array);
        }
        nodesAdd(&outer->stmts, (INode*)slicedcl);
        lo = parVar(parLoName, immPerm, (INode*)newULitNodeTC(0, (INode*)usizeType), scope, lexnode);
        hi = parVar(parHiName, immPerm, (INode*)parField(parUse(slicedcl, lexnode), lenName, lexnode), scope, lexnode);
        k = parVar(parKName, mutPerm, parUse(lo, lexnode), scope, lexnode);
        nodesAdd(&outer->stmts, (INode*)lo);
        nodesAdd(&outer->stmts, (INode*)hi);
        nodesAdd(&outer->stmts, (INode*)k);

        // imm x = &s[k]
        FnCallNode *at = newFnCallLower(lexnode, parUse(slicedcl, lexnode), 1);
        at->flags |= FlagIndex;
        nodesAdd(&at->args, parUse(k, lexnode));
        elem = (INode*)newRefNodeFull(BorrowTag, lexnode, borrowRef, unknownType, (INode*)at);
        outer->flags |= FlagParallel;
    }

    // loop { if k >= hi {break}; imm x = ...; k++; ...body... }
    ((VarDclNode*)nodesGet(loop->stmts, 0))->value = elem;
    FnCallNode *step = newFnCallOpnameLower(lexnode, parUse(k, lexnode), incrPostName, 0);
    step->flags |= FlagLvalOp;
    nodesInsert(&loop->stmts, (INode*)step, 1);
    FnCallNode *done = newFnCallOpnameLower(lexnode, parUse(k, lexnode), geName, 1);
    nodesAdd(&done->args, parUse(hi, lexnode));
    nodesInsert(&loop->stmts, parBreakIf((INode*)done, loop, lexnode), 0);
    nodesAdd(&outer->stmts, (INode*)loop);
}
