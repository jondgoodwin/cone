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
Name *parSliceName = NULL;
Name *parSliceMutName = NULL;
Name *parBagName = NULL;
Name *parPieceName = NULL;
Name *parListName = NULL;
FnDclNode *parallelEachFn = NULL;

void parallelEachNames() {
    if (parLoName != NULL)
        return;
    parListName = nametblPrivate("list'", 5);
    parBagName = nametblPrivate("bag'", 4);
    parPieceName = nametblPrivate("piece'", 6);
    parLoName = nametblPrivate("lo'", 3);
    parHiName = nametblPrivate("hi'", 3);
    parKName = nametblPrivate("k'", 2);
    parFirstName = nametblPrivate("first'", 6);
    parLastName = nametblPrivate("last'", 5);
    parSliceName = nametblPrivate("s'", 2);
    parSliceMutName = nametblPrivate("sm'", 3);
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

// Is this type an instance of the struct of this name that core declares?
static int parIsCore(INode *type, char *name, size_t len) {
    if (type == NULL || type->tag != StructTag || ((StructNode*)type)->namesym != nametblFind(name, (uint32_t)len))
        return 0;
    ModuleNode *mod = dclInfoGetModule(type);
    return mod != NULL && mod->namesym == nametblFind("core", 4);
}

INode *parIndexedReceiver(INode *src) {
    if (src == NULL || src->tag != FnCallTag)
        return NULL;
    FnCallNode *call = (FnCallNode*)src;
    if (call->objfn == NULL || !isNameUseNode(call->objfn) || call->args == NULL || call->args->used != 1)
        return NULL;
    INode *dcl = ((NameUseNode*)call->objfn)->dclnode;
    if (dcl == NULL || dcl->tag != FnDclTag || ((FnDclNode*)dcl)->namesym != nametblFind("indexed", 7))
        return NULL;
    INode *owner = inodeGetOwner(dcl);
    if (!parIsCore(owner, "Array", 5) && !parIsCore(owner, "ArrayChunks", 11) && !parIsCore(owner, "ArrayMutChunks", 14))
        return NULL;
    return nodesGet(call->args, 0);
}

// A list's 'indexed' is Array's, called on the slice the list lends ('view'): the
// receiver given is that call. The loop walks the list itself, as 'parallel each
// x in list' does (a list in a local of a behaviour is fine, its block being on
// the heap), so this answers the list it was lent from; any other receiver as it is
static INode *parUnlend(INode *recv) {
    if (recv->tag != FnCallTag)
        return recv;
    FnCallNode *call = (FnCallNode*)recv;
    if (call->objfn == NULL || !isNameUseNode(call->objfn) || call->args == NULL || call->args->used != 1)
        return recv;
    INode *dcl = ((NameUseNode*)call->objfn)->dclnode;
    INode *arg = nodesGet(call->args, 0);
    if (dcl == NULL || dcl->tag != FnDclTag || !isExpNode(arg))
        return recv;
    INode *type = iexpGetTypeDcl(arg);
    INode *base = type->tag == RefTag || type->tag == VirtRefTag ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
    if (base == NULL || base->tag != StructTag)
        return recv;
    StructNode *lentbody = structLentBody((StructNode*)base);
    if (lentbody == NULL || !itypeIsArrayBody((INode*)lentbody) || structLentVia((StructNode*)base) != ((FnDclNode*)dcl)->namesym)
        return recv;
    return arg;
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

static int parHasSeam(BlockNode *outer);

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
    case OfEntryTag: case FillEntryTag: case PairEntryTag: case YieldEntryTag:
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
    // The loop's own lending of each item to its pass, '&mut s[k]', which is how
    // a pass may change its item: items are disjoint
    if (root->namesym == parSliceName || root->namesym == parSliceMutName)
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
    default:
        break;
    }
    return 1;
}

// ---- A loop that changes its items: what else may name them -------------------
//
// Each pass changing its own item is safe, since the items are disjoint
// (mutItems, or a mutable slice as the source). But '&mut' is shared mutable
// in Cone, so nothing else stops the body reading the same list through its own
// name while another pass changes an item of it. The body therefore may not
// name the place the items are lent from -- or a place inside it, or one it is
// inside -- the variable and the fields named from it.

#define ParPathMax 8

typedef struct {
    VarDclNode *root;
    Name *fields[ParPathMax];
    uint32_t nfields;
    int closed;         // An element was picked (an index): what is named after it is that element's
} ParPath;

// The place a node names, from the variable at its root: fields named after it,
// through references, borrows and a method's receiver. 0 where it names none
static int parPathOf(INode *node, ParPath *path) {
    memset(path, 0, sizeof(*path));
    while (node != NULL) {
        if (isNameUseNode(node)) {
            INode *dcl = ((NameUseNode*)node)->dclnode;
            if (dcl == NULL || dcl->tag != VarDclTag)
                return 0;
            path->root = (VarDclNode*)dcl;
            // (the fields were met outermost first: put them in order)
            for (uint32_t i = 0; i < path->nfields / 2; ++i) {
                Name *swap = path->fields[i];
                path->fields[i] = path->fields[path->nfields - 1 - i];
                path->fields[path->nfields - 1 - i] = swap;
            }
            return 1;
        }
        switch (node->tag) {
        case FldAccessTag: {
            INode *field = ((FnCallNode*)node)->methfld;
            // A field after an index names an element's, which the index picked
            if (field != NULL && isNameUseNode(field) && path->nfields < ParPathMax)
                path->fields[path->nfields++] = ((NameUseNode*)field)->namesym;
            else
                path->closed = 1;
            node = ((FnCallNode*)node)->objfn;
            break;
        }
        case ArrIndexTag:
            // What was named outside the index is an element's: forget it
            path->nfields = 0;
            path->closed = 1;
            node = ((FnCallNode*)node)->objfn;
            break;
        case DerefTag:
            node = ((StarNode*)node)->vtexp;
            break;
        case BorrowTag:
        case ArrayBorrowTag:
            node = ((RefNode*)node)->vtexp;
            break;
        case CastTag:
            node = ((CastNode*)node)->exp;
            break;
        case FnCallTag: {
            // A method called on a place (the lending method's own receiver)
            INode *fn = ((FnCallNode*)node)->objfn;
            Nodes *args = ((FnCallNode*)node)->args;
            if (fn == NULL || !isNameUseNode(fn) || ((NameUseNode*)fn)->dclnode == NULL
                || ((NameUseNode*)fn)->dclnode->tag != FnDclTag || args == NULL || args->used < 1)
                return 0;
            node = nodesGet(args, 0);
            break;
        }
        default:
            return 0;
        }
    }
    return 0;
}

// Do two places overlap: one is the other, or inside it?
static int parPathsOverlap(ParPath *a, ParPath *b) {
    if (a->root != b->root)
        return 0;
    uint32_t n = a->nfields < b->nfields ? a->nfields : b->nfields;
    for (uint32_t i = 0; i < n; ++i) {
        if (a->fields[i] != b->fields[i])
            return 0;
    }
    return 1;
}

typedef struct {
    ParPath lent;           // The place the items are lent from
    ParSet reported;
} ParAlias;

// Walk the arguments of the indexes a place goes through: they are uses too
static void parWalkIndexArgs(INode *node, ParVisit visit, void *ctx) {
    while (node != NULL) {
        switch (node->tag) {
        case ArrIndexTag:
            parWalkNodes(((FnCallNode*)node)->args, visit, ctx);
            node = ((FnCallNode*)node)->objfn;
            break;
        case FldAccessTag:
            node = ((FnCallNode*)node)->objfn;
            break;
        case DerefTag:
            node = ((StarNode*)node)->vtexp;
            break;
        case BorrowTag:
        case ArrayBorrowTag:
            node = ((RefNode*)node)->vtexp;
            break;
        default:
            return;
        }
    }
}

static int parCheckAlias(INode *node, void *ctxp) {
    ParAlias *ctx = (ParAlias *)ctxp;
    int place = node->tag == FldAccessTag || node->tag == ArrIndexTag || isNameUseNode(node);
    if (!place)
        return 1;
    ParPath path;
    if (!parPathOf(node, &path))
        return 1;
    if (path.root == ctx->lent.root && parPathsOverlap(&path, &ctx->lent) && !parSetHas(&ctx->reported, node)) {
        parSetAdd(&ctx->reported, node);
        char *name = path.root->namesym == anonName ? "a value made outside the loop" : &path.root->namesym->namestr;
        errorMsgNode(node, ErrorParWrite,
            "This 'parallel each' changes the items of %s one pass at a time, so its body may not name %s, or what is inside it, as well: another pass may be changing the item it would read. Read the item through the pass's own variable.",
            name, name);
    }
    // The place is taken whole: only the indexes inside it are uses of their own
    parWalkIndexArgs(node, parCheckAlias, ctx);
    return 0;
}

// ---- Copies that write a count the passes share --------------------------------
//
// Outside variables are read-only for the loop, and copying a counted owner
// writes its count: 'aliasRef' adds one holder. An 'Arc' does it atomically; an
// 'Rc' (and any region that is shared and declares no ThreadSafe) does not, so
// two passes copying the same one would lose a count and free what is still held.
// A copy of such a value that is not the pass's own is therefore refused, as a
// write is. What is copied is the value of a place read as a value; a borrow of
// it, a field of it that is a number, a call that takes a borrow, are reads.
// A traced reference (Gc) is counted as one: it has no count, but a copy of it
// held in a local is a root the function links into the collector's one chain
// of frames (conestd's roots.cone, single threaded), written without atomics.
// A move owner (So) cannot be copied. (Only what the body itself copies is
// seen: a function it calls that copies a borrow it was handed is that
// function's; and a traced local the pass declares itself is not refused.)

// Does a copy of a reference of this kind write shared state without atomics?
static int parRefCountsPlain(RefNode *ref) {
    INode *region = ref->region && isTypeNode(ref->region) ? itypeGetTypeDcl(ref->region) : ref->region;
    if (region == NULL || region == borrowRef || permHeldKind(ref->perm))
        return 0;
    if (regionIsTraced(ref->region))
        return 1;
    if (regionIsMove(ref->region))
        return 0;
    INode *perm = ref->perm && isTypeNode(ref->perm) ? itypeGetTypeDcl(ref->perm) : NULL;
    if (perm == NULL || perm->tag != PermTag || !(permGetFlags(perm) & MayAlias))
        return 0;
    return !regionIsThreadSafe(ref->region);
}

// Does a copy of a value of this type copy a counted owner whose count is not
// atomic: it is one, or holds one in a field, a variant, an element?
static int parHoldsPlainCount(INode *type, INode **seen, uint32_t *nseen) {
    if (type == NULL || *nseen > 60)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? parHoldsPlainCount(itypeGetTypeDcl(type), seen, nseen) : 0;
    case AliasDclTag:
        return parHoldsPlainCount(((AliasDclNode *)type)->target, seen, nseen);
    case RefTag:
        return parRefCountsPlain((RefNode *)type);
    case ArrayTag:
        return parHoldsPlainCount(arrayElemType(type), seen, nseen);
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (parHoldsPlainCount(*nodesp, seen, nseen))
                return 1;
        }
        return 0;
    }
    case StructTag: {
        for (uint32_t i = 0; i < *nseen; ++i) {
            if (seen[i] == type)
                return 0;       // (found where it was first asked)
        }
        seen[(*nseen)++] = type;
        StructNode *strnode = (StructNode *)type;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            if (parHoldsPlainCount(((IExpNode *)*nodesp)->vtype, seen, nseen))
                return 1;
        }
        if (strnode->derived) {
            for (nodesFor(strnode->derived, cnt, nodesp)) {
                if (parHoldsPlainCount(*nodesp, seen, nseen))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int parIsPlaceNode(INode *node) {
    return isNameUseNode(node) || node->tag == FldAccessTag || node->tag == ArrIndexTag || node->tag == DerefTag;
}

// Is this place a value of the pass's own: a variable it declared, or a part of
// one held inline (not through a reference or an index of something lent)?
static int parPlaceIsOwn(INode *node, ParSet *inside) {
    while (node != NULL) {
        if (isNameUseNode(node)) {
            INode *dcl = ((NameUseNode*)node)->dclnode;
            return dcl != NULL && dcl->tag == VarDclTag && parSetHas(inside, dcl);
        }
        if (node->tag == FldAccessTag || node->tag == ArrIndexTag) {
            INode *obj = ((FnCallNode*)node)->objfn;
            INode *objtype = isExpNode(obj) ? iexpGetTypeDcl(obj) : NULL;
            if (objtype == NULL || (objtype->tag != StructTag && objtype->tag != ArrayTag && objtype->tag != TTupleTag))
                return 0;       // reached through a reference, a slice or a pointer
            node = obj;
            continue;
        }
        return 0;
    }
    return 0;
}

typedef struct {
    ParSet inside;
    ParSet reported;
} ParCopies;

static int parCheckCopies(INode *node, void *ctxp) {
    ParCopies *ctx = (ParCopies *)ctxp;
    switch (node->tag) {
    case BorrowTag:
    case ArrayBorrowTag: {
        // The place is lent, not copied: only the indexes inside it are uses
        INode *place = ((RefNode*)node)->vtexp;
        if (!parIsPlaceNode(place))
            return 1;
        parWalkIndexArgs(place, parCheckCopies, ctx);
        return 0;
    }
    case AssignTag:
        if (parIsPlaceNode(((AssignNode*)node)->lval))
            parWalkIndexArgs(((AssignNode*)node)->lval, parCheckCopies, ctx);
        else
            parWalk(((AssignNode*)node)->lval, parCheckCopies, ctx);
        parWalk(((AssignNode*)node)->rval, parCheckCopies, ctx);
        return 0;
    case SwapTag:
        // (a swap moves what is in its places; writes are refused elsewhere)
        parWalkIndexArgs(((SwapNode*)node)->lval, parCheckCopies, ctx);
        parWalkIndexArgs(((SwapNode*)node)->rval, parCheckCopies, ctx);
        return 0;
    default:
        break;
    }
    if (!parIsPlaceNode(node) || !isExpNode(node))
        return 1;
    INode *type = iexpGetTypeDcl(node);
    INode *seen[64];
    uint32_t nseen = 0;
    if (type != NULL && parHoldsPlainCount(type, seen, &nseen) && !parPlaceIsOwn(node, &ctx->inside)
        && !parSetHas(&ctx->reported, node)) {
        parSetAdd(&ctx->reported, node);
        errorMsgNode(node, ErrorParCopy,
            "A 'parallel each' body may not copy a value of type %s, which is not the pass's own: its passes run at the same time, and a copy writes state that every copy shares without an atomic operation (an Rc's count, or the roots of the collector's traced references), which two passes cannot do together. Read it through a borrow ('&x', or a method that takes one), or use an Arc, whose count is atomic.",
            itypeName(type));
    }
    parWalkIndexArgs(node, parCheckCopies, ctx);
    return 0;
}

// ---- What a loop cut at its end reads from the behaviour's frame --------------
//
// The pieces read copies of the variables the body names, taken as the behaviour
// returns. A copy of a value is as good as the value; a copy of a borrow points
// into the behaviour's frame (or wherever the borrow did), which is gone. Only
// 'self', the actor's own state, is a borrow the pieces may read.

typedef struct {
    ParSet inside;
    ParSet reported;
} ParFrame;

static int parCheckFrame(INode *node, void *ctxp) {
    ParFrame *ctx = (ParFrame *)ctxp;
    if (!isNameUseNode(node))
        return 1;
    INode *dcl = ((NameUseNode*)node)->dclnode;
    if (dcl == NULL || dcl->tag != VarDclTag || parSetHas(&ctx->inside, dcl))
        return 1;
    VarDclNode *var = (VarDclNode *)dcl;
    if (var->namesym == selfName || var->scope == 0 || var->vtype == NULL)
        return 1;
    if (itypeCarriesBorrow(itypeGetTypeDcl(var->vtype)) && !parSetHas(&ctx->reported, dcl)) {
        parSetAdd(&ctx->reported, dcl);
        errorMsgNode(node, ErrorParFrame,
            "This 'parallel each' is in a behaviour, which returns to its actor's dispatcher while the pieces run, and %s holds a borrow, which points into the behaviour's own frame, gone by then. Read the actor's fields through 'self' instead, or copy the value the borrow reaches into a variable of the behaviour's.",
            &var->namesym->namestr);
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

    // Nor does it copy what counts its holders without atomic operations
    ParCopies copies;
    memset(&copies, 0, sizeof(copies));
    copies.inside = ctx.inside;
    parWalk((INode*)loop, parCheckCopies, &copies);

    // A loop cut at its end reads no borrow held in the behaviour's frame
    if (parHasSeam(outer)) {
        ParFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.inside = ctx.inside;
        parWalk((INode*)loop, parCheckFrame, &frame);
    }

    // A loop that lends its items to be changed: the body names none of the
    // place they are lent from
    for (uint32_t i = 0; i < outer->stmts->used; ++i) {
        INode *stmt = nodesGet(outer->stmts, i);
        if (stmt->tag == VarDclTag && ((VarDclNode*)stmt)->namesym == parSliceMutName) {
            ParAlias alias;
            memset(&alias, 0, sizeof(alias));
            if (((VarDclNode*)stmt)->value != NULL && parPathOf(((VarDclNode*)stmt)->value, &alias.lent))
                parWalk((INode*)loop, parCheckAlias, &alias);
            break;
        }
    }
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

// ---- A loop in an actor's behaviour: the seam ----------------------------------
//
// Written directly in a behaviour (not inside another parallel each's body, whose
// inner loop finishes inside its pass), a parallel each is cut where its loop
// ends, as an 'await' is: the behaviour returns to its actor's dispatcher while the
// pieces run, and the rest of it runs when the last has finished. A statement
// following the loop, an AwaitNode with 'par' set, is the cut. The pieces read
// what the loop uses from outside in copies of those variables' values made as the
// behaviour returns (genlpar.c), and the actor's fields through 'self', so a copy
// must not hold a pointer into the behaviour's own frame, which is gone by then.
// What may: values, owners (their blocks are on the heap), and 'self'. What
// may not: a borrow held in a variable, and an array in a local that the loop
// walks or lends. Nothing is lost by it: the pieces' own variables are their own.

// The functions whose parallel each bodies are being checked, outermost first (a
// function checked in the middle of one, called from its body, is another's)
#define ParBodyMax 64
static FnDclNode *parBodyFns[ParBodyMax];
static uint32_t parBodyDepth = 0;

void parallelEachEnter(TypeCheckState *pstate) {
    if (parBodyDepth < ParBodyMax)
        parBodyFns[parBodyDepth] = pstate->fn;
    ++parBodyDepth;
}

void parallelEachLeave() {
    --parBodyDepth;
}

// Is a parallel each's body being checked in this function?
static int parInBody(FnDclNode *fn) {
    for (uint32_t i = 0; i < parBodyDepth && i < ParBodyMax; ++i) {
        if (parBodyFns[i] == fn)
            return 1;
    }
    return 0;
}

// Is this the end-of-loop seam of a parallel each, the statement after its loop?
static int parIsSeam(INode *node) {
    return node->tag == AwaitTag && ((AwaitNode *)node)->par;
}

// Whether the block (a parallel each as built) is cut at its loop's end
static int parHasSeam(BlockNode *outer) {
    if (outer->stmts->used == 0)
        return 0;
    INode *last = nodesGet(outer->stmts, outer->stmts->used - 1);
    // (type check makes the last statement the block's value)
    if (last->tag == BlockRetTag)
        last = ((BreakRetNode*)last)->exp;
    return parIsSeam(last);
}

// Does this place, or the slice or array a call lends of one, lie where the
// method's frame is, or may it? The actor's own fields, reached through 'self',
// lie in the actor. A variable of the method holding its value inline lies in
// the frame; one holding a borrow or a pointer points where it points, which is
// not known here, so it counts as the frame. A value made for the loop does.
static int parMayBeFrame(INode *node) {
    while (node != NULL) {
        if (isNameUseNode(node)) {
            INode *dcl = ((NameUseNode*)node)->dclnode;
            if (dcl == NULL || dcl->tag != VarDclTag)
                return 0;       // a global's place is nobody's frame
            return 1;
        }
        switch (node->tag) {
        case FldAccessTag:
        case ArrIndexTag: {
            INode *obj = ((FnCallNode*)node)->objfn;
            INode *objtype = isExpNode(obj) ? iexpGetTypeDcl(obj) : NULL;
            // Through a reference: it lies where the reference points
            if (objtype != NULL && (objtype->tag == RefTag || objtype->tag == VirtRefTag
                || objtype->tag == ArrayRefTag || objtype->tag == PtrTag)) {
                if (isNameUseNode(obj) && ((NameUseNode*)obj)->namesym == selfName)
                    return 0;
                return parMayBeFrame(obj);
            }
            node = obj;
            break;
        }
        case DerefTag:
            node = ((StarNode*)node)->vtexp;
            if (node != NULL && isNameUseNode(node) && ((NameUseNode*)node)->namesym == selfName)
                return 0;
            break;
        case BorrowTag:
        case ArrayBorrowTag:
            node = ((RefNode*)node)->vtexp;
            break;
        case CastTag:
            node = ((CastNode*)node)->exp;
            break;
        case FnCallTag: {
            // A method lending a slice of its receiver: the receiver's place
            Nodes *args = ((FnCallNode*)node)->args;
            if (args == NULL || args->used < 1)
                return 1;
            node = nodesGet(args, 0);
            break;
        }
        default:
            return 1;
        }
    }
    return 1;
}

// Whether the loop can run where it is written, and the runtime function it is
// run by found: the module imports the actors package. '*seam' says the loop is
// cut at its end: it is written in a behaviour, and not in the body of another
// parallel each. (Any other actor method runs the loop where it stands, the
// worker running pieces meanwhile, so its actor is not run again until it returns.)
static int parRuntime(TypeCheckState *pstate, INode *lexnode, int *seam) {
    *seam = 0;
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
    if (yieldAny() && yieldGenOf(fn) != NULL) {
        errorMsgNode(lexnode, ErrorParRuntime,
            "A 'parallel each' inside a generator is not built: a generator's body is run a step at a time by whoever calls 'next', and its passes would have to finish within one step. Run the loop in a function the generator calls.");
        return 0;
    }
    parallelEachFn = (FnDclNode *)run;
    *seam = !parInBody(fn) && actorOfBehaviour(fn) != NULL;
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
            "A 'parallel each' over %s, which has 'len' and 'split' and so is a ParallelIterable, is not built yet: it walks arrays, slices, lists, number ranges and the runs of 'chunks' and 'mutChunks'.",
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

// ---- The parallel builder ------------------------------------------------------
//
// 'xs <- parallel each x in src [if c] yield v' is the entry of a '<-' whose loop
// is a parallel each (parseEachLoop) and whose last statement is the yield
// (contents.c), with the receiver the '<-' holds in a hidden variable. The
// pieces cannot append to that one list at the same time, so each gets a list of
// its own, and the lists are joined in the order of the passes after the loop:
//
//   mut bag = (*recv).pieceBag();                    // the caller's, before k
//   mut k = lo;                                      // from here, the piece's:
//   mut list = (*recv).emptyPiece();                 //   a list of its own
//   imm piece = &mut list;                           //   which the yield appends through
//   loop { if k >= hi { (*recv).depositPiece(&mut bag, lo, list); break }; ...; *piece <- v }
//   (*recv).joinPieces(&mut bag)                     // after the loop, the caller's
//
// The methods are the receiver's own (a List's: collections.cone), so the
// compiler needs to name no type. The list is a local of the piece (not a borrow
// a call returns): flow's shape-changing check cannot tell a borrow got from a
// call from the one the source is read through. It is moved into depositPiece at
// the loop's one exit, so nothing is left for the caller's block end to release.

// The yield of a builder's loop: the entry that is a statement of it
static EntryNode *parBuilderYield(BlockNode *loop) {
    INode **stmtp;
    uint32_t cnt;
    for (nodesFor(loop->stmts, cnt, stmtp)) {
        if ((*stmtp)->tag == YieldEntryTag && ((EntryNode*)*stmtp)->recv != NULL)
            return (EntryNode*)*stmtp;
    }
    return NULL;
}

// The type of the list a builder appends to, when it can be built in parallel
static int parBuilderCheck(TypeCheckState *pstate, EntryNode *yield, INode *lexnode) {
    VarDclNode *recv = (VarDclNode*)yield->recv;
    INode *recvtype = iexpGetTypeDcl((INode*)recv);
    if (recvtype == errorType || recvtype == unknownType)
        return 0;
    INode *colltype = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)recvtype)->vtexp) : recvtype;
    if (!parHasMethod(colltype, nametblFind("pieceBag", 8)) || !parHasMethod(colltype, nametblFind("emptyPiece", 10))
        || !parHasMethod(colltype, nametblFind("depositPiece", 12)) || !parHasMethod(colltype, nametblFind("joinPieces", 10))) {
        errorMsgNode(lexnode, ErrorParBuilder,
            "A parallel builder appends to a List, whose pieces it joins in the order of the passes, and %s is not one. Build a List with 'parallel each', and make the %s from it after the loop.",
            itypeName(colltype), yield->first != NULL ? "dictionary" : "collection");
        return 0;
    }
    if (actorOfState(inodeGetOwner((INode*)pstate->fn)) != NULL) {
        errorMsgNode(lexnode, ErrorParRuntime,
            "A parallel builder inside an actor's method is not built yet: it must join its pieces' lists after the loop, and the loop cannot yet be left and resumed in a method. Build the list in a function outside actors for now.");
        return 0;
    }
    return 1;
}

static INode *parRecvPlace(VarDclNode *recv, INode *lexnode) {
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, lexnode);
    deref->vtexp = parUse(recv, lexnode);
    return (INode*)deref;
}

// '&mut bag'
static INode *parBorrowMut(VarDclNode *bag, INode *lexnode) {
    return (INode*)newRefNodeFull(BorrowTag, lexnode, borrowRef, (INode*)mutPerm, parUse(bag, lexnode));
}

// The statements of the builder around the loop (see above): the bag before the
// index the pieces count with, the piece's list before the loop, the join after
static void parBuilderLower(BlockNode *outer, BlockNode *loop, EntryNode *yield, VarDclNode *lo, VarDclNode *k,
    uint16_t scope, INode *lexnode) {
    VarDclNode *recv = (VarDclNode*)yield->recv;
    VarDclNode *bag = parVar(parBagName, mutPerm, (INode*)parCall(parRecvPlace(recv, lexnode),
        nametblFind("pieceBag", 8), lexnode), scope, lexnode);
    uint32_t kat = 0;
    while (nodesGet(outer->stmts, kat) != (INode*)k)
        ++kat;
    nodesInsert(&outer->stmts, (INode*)bag, kat);

    // The piece's own list, a local of the piece, and the borrow the yield appends
    // through (as the receiver of an ordinary '<-' each is a borrow of the list)
    VarDclNode *list = parVar(parListName, mutPerm, (INode*)parCall(parRecvPlace(recv, lexnode),
        nametblFind("emptyPiece", 10), lexnode), scope, lexnode);
    VarDclNode *piece = parVar(parPieceName, immPerm, (INode*)newRefNodeFull(BorrowTag, lexnode, borrowRef,
        (INode*)mutPerm, parUse(list, lexnode)), scope, lexnode);
    yield->recv = (INode*)piece;
    nodesAdd(&outer->stmts, (INode*)list);
    nodesAdd(&outer->stmts, (INode*)piece);
    nodesAdd(&outer->stmts, (INode*)loop);

    // The piece gives its list up where it leaves the loop: the guard's 'break'
    // is ahead of 'break' in its block
    IfNode *guard = (IfNode*)nodesGet(loop->stmts, 0);
    BlockNode *leave = (BlockNode*)nodesGet(guard->condblk, 1);
    FnCallNode *deposit = parCall(parRecvPlace(recv, lexnode), nametblFind("depositPiece", 12), lexnode);
    nodesAdd(&deposit->args, parBorrowMut(bag, lexnode));
    nodesAdd(&deposit->args, parUse(lo, lexnode));
    nodesAdd(&deposit->args, parUse(list, lexnode));
    nodesInsert(&leave->stmts, (INode*)deposit, 0);

    FnCallNode *join = parCall(parRecvPlace(recv, lexnode), nametblFind("joinPieces", 10), lexnode);
    nodesAdd(&join->args, parBorrowMut(bag, lexnode));
    nodesAdd(&outer->stmts, (INode*)join);
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

    parControlRules(loop);
    int seam = 0;
    if (!parRuntime(pstate, lexnode, &seam))
        return;

    // (a source that is 'indexed()' gives two, checked with the source below)
    if (isrange && nvars != 1) {
        errorMsgNode(nodesGet(loop->stmts, 0), ErrorParSource,
            "A 'parallel each' gives one variable, a borrow of each element (or each number of a range); two come from indexed(), the position and the item.");
        return;
    }
    // A builder's loop holds the yield that appends to the receiver
    EntryNode *yield = parBuilderYield(loop);
    if (yield && !parBuilderCheck(pstate, yield, lexnode))
        return;

    VarDclNode *lo, *hi, *k;
    INode *elem;
    int elemindexed = 0;    // 'indexed()': the first variable is the position, the second the item

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
        // The passes are counted in a usize, so the numbers may be no wider
        if (((NbrNode*)typedcl)->bits > ((NbrNode*)usizeType)->bits) {
            errorMsgNode(srcdcl->value, ErrorParSource,
                "A 'parallel each' counts its passes in a usize, and %s is wider than one.", itypeName(type));
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

        // 'indexed()' on an array, slice, list or chunks gives the position and the
        // item: the loop walks what it was called on, and the position is the
        // pass's own index, counted in the whole source, so it is global
        int indexed = 0;
        INode *recv = parIndexedReceiver(src);
        if (recv != NULL && isExpNode(recv)) {
            recv = parUnlend(recv);
            INode *rtype = iexpGetTypeDcl(recv);
            if (rtype == errorType || rtype == unknownType)
                return;
            srcdcl->value = recv;
            srcdcl->vtype = ((IExpNode*)recv)->vtype;
            src = recv;
            type = rtype;
            indexed = 1;
        }
        if (nvars != (indexed ? 2u : 1u)) {
            errorMsgNode(nodesGet(loop->stmts, 0), ErrorParSource, indexed
                ? "A 'parallel each' over indexed() gives two variables, the position and the item."
                : "A 'parallel each' gives one variable, a borrow of each element (or each number of a range); two come from indexed(), the position and the item.");
            return;
        }

        int isref = type->tag == RefTag || type->tag == VirtRefTag;
        INode *base = isref ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
        int isslice = type->tag == ArrayRefTag;
        int isarray = base->tag == ArrayTag;
        // 'chunks(n)' and 'mutChunks(n)': the cursors core gives, cut between
        // chunks (each pass is given a chunk, a slice no other pass can reach)
        int ischunks = !isref && (parIsCore(type, "ArrayChunks", 11) || parIsCore(type, "ArrayMutChunks", 14));
        Name *lentvia = NULL;
        if (!isslice && !isarray && !ischunks && base->tag == StructTag) {
            StructNode *lentbody = structLentBody((StructNode*)base);
            if (lentbody != NULL && itypeIsArrayBody((INode*)lentbody))
                lentvia = structLentVia((StructNode*)base);
        }
        int islent = lentvia != NULL;
        if (!isslice && !isarray && !islent && !ischunks) {
            parSourceError(src, type);
            return;
        }
        int place = parRecheckable(src);

        // Cut at its end, the pieces walk the source after the behaviour has
        // returned: an array or slice must lie in the actor or on the heap, not in
        // the behaviour's own frame (a list's block is on the heap)
        if (seam && !islent && ((isarray && !isref && !place) || parMayBeFrame(src))) {
            errorMsgNode(src, ErrorParFrame,
                "This 'parallel each' is in a behaviour, which returns to its actor's dispatcher while the pieces run, and the %s it walks lies in the behaviour's own frame, which is gone by then. Walk a list, or an array or slice in one of the actor's fields (named through 'self'), or one lent from one.",
                ischunks ? "chunks" : isslice ? "slice" : "array");
            return;
        }

        // A slice that is mutable (what 'mutItems' gives, or a '&mut' slice) lends
        // each item mutably, as 'each' does; any other lends it to be read. Chunks
        // of a mutable slice (mutChunks) lend each chunk mutably
        int mutlend = ischunks ? parIsCore(type, "ArrayMutChunks", 14)
            : isslice && permMatches((INode*)mutPerm, ((RefNode*)type)->perm);
        Name *slicename = mutlend ? parSliceMutName : parSliceName;

        // imm s = [the slice]
        VarDclNode *slicedcl = srcdcl;
        if (isslice || ischunks)
            slicedcl->namesym = slicename;
        if (islent) {
            // The slice the type lends: its place's, or a value held first
            INode *owner = src;
            if (!place) {
                nodesAdd(&outer->stmts, (INode*)srcdcl);
                owner = parUse(srcdcl, lexnode);
            }
            slicedcl = parVar(slicename, immPerm, (INode*)parCall(owner, lentvia, lexnode), scope, lexnode);
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
            slicedcl = parVar(slicename, immPerm, NULL, scope, lexnode);
            slicedcl->value = (INode*)newRefNodeFull(ArrayBorrowTag, lexnode, borrowRef, unknownType, array);
        }
        nodesAdd(&outer->stmts, (INode*)slicedcl);
        lo = parVar(parLoName, immPerm, (INode*)newULitNodeTC(0, (INode*)usizeType), scope, lexnode);
        // (chunks: the number of chunks, 's.len()')
        hi = parVar(parHiName, immPerm, ischunks ? (INode*)parCall(parUse(slicedcl, lexnode), lenName, lexnode)
            : (INode*)parField(parUse(slicedcl, lexnode), lenName, lexnode), scope, lexnode);
        k = parVar(parKName, mutPerm, parUse(lo, lexnode), scope, lexnode);
        nodesAdd(&outer->stmts, (INode*)lo);
        nodesAdd(&outer->stmts, (INode*)hi);
        nodesAdd(&outer->stmts, (INode*)k);

        if (ischunks) {
            // imm x = s.at(k): chunk number k, a slice of its own
            FnCallNode *chunk = parCall(parUse(slicedcl, lexnode), nametblFind("at", 2), lexnode);
            nodesAdd(&chunk->args, parUse(k, lexnode));
            elem = (INode*)chunk;
        }
        else {
            // imm x = &s[k]
            FnCallNode *at = newFnCallLower(lexnode, parUse(slicedcl, lexnode), 1);
            at->flags |= FlagIndex;
            nodesAdd(&at->args, parUse(k, lexnode));
            elem = (INode*)newRefNodeFull(BorrowTag, lexnode, borrowRef, mutlend ? (INode*)mutPerm : unknownType, (INode*)at);
        }
        outer->flags |= FlagParallel;
        elemindexed = indexed;
    }

    // loop { if k >= hi {break}; imm x = ...; k++; ...body... }
    // (over indexed(): imm i = k; imm x = ...; k++)
    if (elemindexed) {
        ((VarDclNode*)nodesGet(loop->stmts, 0))->value = parUse(k, lexnode);
        ((VarDclNode*)nodesGet(loop->stmts, 1))->value = elem;
    }
    else
        ((VarDclNode*)nodesGet(loop->stmts, 0))->value = elem;
    FnCallNode *step = newFnCallOpnameLower(lexnode, parUse(k, lexnode), incrPostName, 0);
    step->flags |= FlagLvalOp;
    nodesInsert(&loop->stmts, (INode*)step, nvars);
    FnCallNode *done = newFnCallOpnameLower(lexnode, parUse(k, lexnode), geName, 1);
    nodesAdd(&done->args, parUse(hi, lexnode));
    nodesInsert(&loop->stmts, parBreakIf((INode*)done, loop, lexnode), 0);
    if (yield)
        parBuilderLower(outer, loop, yield, lo, k, scope, lexnode);
    else
        nodesAdd(&outer->stmts, (INode*)loop);

    // In a behaviour the behaviour is cut where the loop ends: the seam follows it
    // (the parallel builder is refused in an actor's method, parRuntime)
    if (seam) {
        AwaitNode *cut = awaitParNew(pstate, lexnode);
        if (cut == NULL) {
            outer->stmts->used = 0;
            outer->flags &= 0xFFFF - FlagParallel;
            return;
        }
        nodesAdd(&outer->stmts, (INode*)cut);
    }
}
