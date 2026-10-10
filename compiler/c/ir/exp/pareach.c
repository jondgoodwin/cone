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
Name *parPairName = NULL;
Name *parPieceName = NULL;
Name *parListName = NULL;
FnDclNode *parallelEachFn = NULL;

void parallelEachNames() {
    if (parLoName != NULL)
        return;
    parListName = nametblPrivate("list'", 5);
    parBagName = nametblPrivate("bag'", 4);
    parPairName = nametblPrivate("pair'", 5);
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

// The cursors core gives that a parallel each walks by their 'len' and 'at' (the
// items are the passes; a cursor 'indexed' gives has a tuple of the position and
// the item for each): which this type is, and whether it lends '&mut' and takes
// two variables. What each lends is what the cursor's own 'next' lends.
typedef struct {
    int cursor;     // one of them
    int mut;        // lends '&mut' (a chunk, or an item), in either source of a zip
    int vars;       // how many variables it gives: one, or the elements of the tuple 'at' answers
    int chunks;     // an item is a run, not an element
} ParCursor;

// The type of a field of a struct type, or NULL
static INode *parFieldType(INode *type, char *name) {
    if (!isMethodType(type))
        return NULL;
    INode *field = iNsTypeFindFnField((INsTypeNode*)type, nametblFind(name, (uint32_t)strlen(name)));
    if (field == NULL || field->tag != FieldDclTag)
        return NULL;
    return itypeGetTypeDcl(((FieldDclNode*)field)->vtype);
}

static ParCursor parCursorOf(INode *type) {
    ParCursor c = { 0, 0, 1, 0 };
    if (parIsCore(type, "ArrayChunks", 11)) { c.cursor = 1; c.chunks = 1; }
    else if (parIsCore(type, "ArrayMutChunks", 14)) { c.cursor = 1; c.chunks = 1; c.mut = 1; }
    else if (parIsCore(type, "ChunksIndexed", 13)) { c.cursor = 1; c.chunks = 1; c.vars = 2; }
    else if (parIsCore(type, "MutChunksIndexed", 16)) { c.cursor = 1; c.chunks = 1; c.mut = 1; c.vars = 2; }
    else if (parIsCore(type, "ArrayMutItems", 13)) { c.cursor = 1; c.mut = 1; }
    else if (parIsCore(type, "MutItemsIndexed", 15)) { c.cursor = 1; c.mut = 1; c.vars = 2; }
    else if (parIsCore(type, "ArrayIndexed", 12)) { c.cursor = 1; c.vars = 2; }
    else if (parIsCore(type, "Zip", 3)) {
        // a pair, one variable from each source, when it can be walked by position at
        // all (both sources can: else it has no 'len' and 'at'); it lends '&mut' if
        // either source does
        c.cursor = 1;
        c.vars = 2;
        c.mut = parCursorOf(parFieldType(type, "first")).mut || parCursorOf(parFieldType(type, "second")).mut;
    }
    else if (parIsCore(type, "ZipIndexed", 10)) {
        c.cursor = 1;
        c.vars = 3;
        c.mut = parCursorOf(parFieldType(type, "zip")).mut;
    }
    else if (parIsCore(type, "Zip3", 4)) {
        // three, a flat triple: one variable from each source
        c.cursor = 1;
        c.vars = 3;
        c.mut = parCursorOf(parFieldType(type, "first")).mut || parCursorOf(parFieldType(type, "second")).mut
            || parCursorOf(parFieldType(type, "third")).mut;
    }
    else if (parIsCore(type, "Zip3Indexed", 11)) {
        c.cursor = 1;
        c.vars = 4;
        c.mut = parCursorOf(parFieldType(type, "zip")).mut;
    }
    return c;
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

#define ParLentMax 8

typedef struct {
    ParPath lent[ParLentMax];   // The places the items are lent from: those of each source of a zip
    uint32_t nlent;
    ParSet reported;
} ParAlias;

// Can this expression name or lend from a place of the caller's: a name, a field, an
// element, a dereference, a call made of such, or a borrow of one? Not a literal, nor
// an argument the compiler defaulted ('srcFile()', the borrow of a file name)
static int parNamesAPlace(INode *node) {
    if (node == NULL)
        return 0;
    switch (node->tag) {
    case BorrowTag:
    case ArrayBorrowTag:
        return parNamesAPlace(((RefNode*)node)->vtexp);
    case CastTag:
        return parNamesAPlace(((CastNode*)node)->exp);
    case DerefTag:
        return parNamesAPlace(((StarNode*)node)->vtexp);
    case FldAccessTag:
    case ArrIndexTag:
        return 1;
    case FnCallTag:
        return ((FnCallNode*)node)->args != NULL && ((FnCallNode*)node)->args->used > 0;
    default:
        return isNameUseNode(node);
    }
}

// Does this argument of a call hold a borrow of a place, which the call's result may lend from?
static int parArgLends(INode *arg) {
    if (arg == NULL || !isExpNode(arg) || !parNamesAPlace(arg))
        return 0;
    INode *type = iexpGetTypeDcl(arg);
    return type != NULL && itypeCarriesBorrow(type);
}

// The places a source lends from: for a call, those of every argument that holds a
// borrow (a zip is given two cursors, not one), else the path of the place named.
// Nothing here reads a function's name: what a call lends from is what it is given
static void parLentPaths(INode *node, ParAlias *alias) {
    while (node != NULL && (node->tag == BorrowTag || node->tag == ArrayBorrowTag))
        node = ((RefNode*)node)->vtexp;
    if (node == NULL)
        return;
    if (node->tag == FnCallTag) {
        INode *fn = ((FnCallNode*)node)->objfn;
        Nodes *args = ((FnCallNode*)node)->args;
        if (fn != NULL && isNameUseNode(fn) && ((NameUseNode*)fn)->dclnode != NULL
            && ((NameUseNode*)fn)->dclnode->tag == FnDclTag && args != NULL) {
            for (uint32_t i = 0; i < args->used; ++i) {
                if (parArgLends(nodesGet(args, i)))
                    parLentPaths(nodesGet(args, i), alias);
            }
        }
        return;
    }
    ParPath path;
    if (parPathOf(node, &path) && alias->nlent < ParLentMax)
        alias->lent[alias->nlent++] = path;
}

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
    int overlaps = 0;
    for (uint32_t i = 0; i < ctx->nlent; ++i) {
        if (path.root == ctx->lent[i].root && parPathsOverlap(&path, &ctx->lent[i]))
            overlaps = 1;
    }
    if (overlaps && !parSetHas(&ctx->reported, node)) {
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
// seen: a function it calls that copies a borrow it was handed is kept out by
// the reach rule below, which refuses the body naming what it could copy.
// A traced reference the pass makes itself is refused too, below.)

// Does a copy of a reference of this kind write shared state without atomics?
// (reference.c: the share check's rule, which this is the first user of)
static int parRefCountsPlain(RefNode *ref) {
    return refCountsPlain(ref);
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
            "A 'parallel each' body may not copy a value of type %s, which is not the pass's own: its passes run at the same time, and a copy writes state that every copy shares without an atomic operation (an Rc's count, or the roots of the collector's traced references), which two passes cannot do together. Borrow its contents before the loop ('imm mesh = &*shared;') and use the borrow, or use an Arc, whose count is atomic.",
            itypeName(type));
    }
    parWalkIndexArgs(node, parCheckCopies, ctx);
    return 0;
}

// ---- What a body may reach: values safe to share a borrow of ---------------------
//
// A copy is not the only way a pass touches a count it shares. A helper handed a
// borrow of an Rc may copy it, and so may anything reached through a field, an
// element or a reference: the call is not seen into. So a value declared outside
// the loop whose type is not Shareable (the marker, Rust's Sync, the sibling of
// Sendable: a borrow of it may be held by several threads at once, inferred from
// the type's contents, itypeNotShareable) may not be named in the body at all:
// not read, not lent, not passed. It is not when it holds, through a field, a
// variant, a tuple or array element, a borrow or an owner's pointee, an
// aliasable owner of a region that does not declare ThreadSafe, a traced
// reference, or a reference that writes through a shared path (a '&mut': Cone's
// is shared mutable), and so does an instance of a generic type whose type
// argument does (List[Rc[T]], whatever pointer the list keeps its items behind).
// What the pass declares itself is its own, and so is nothing it was lent: the
// variable holding the item, which is judged without the '&mut' rule (no other
// pass reaches the item it is lent).

typedef struct {
    ParSet inside;      // The variables declared in the body, and the loop's own
    ParSet *copied;     // The places parCheckCopies already refused
    VarDclNode *item;   // The pass's item: a borrow of the source's element
} ParReach;

static ParSet parReachReported;

// Is this type a counted or traced owner itself, or a borrow of one, as against
// a value or borrow of a struct, collection or tuple that holds one?
static int parIsCountedItself(INode *type) {
    for (int depth = 0; type != NULL && depth < 8; ++depth) {
        if (type->tag == NameUseTag && isTypeNode(type))
            type = itypeGetTypeDcl(type);
        else if (type->tag == AliasDclTag)
            type = ((AliasDclNode *)type)->target;
        else if (type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag) {
            if (parRefCountsPlain((RefNode *)type))
                return 1;
            if (((RefNode *)type)->region != borrowRef)
                return 0;
            type = ((RefNode *)type)->vtexp;
        }
        else
            return 0;
    }
    return 0;
}

// A variable the lowering made for the loop, which no source can name
static int parHiddenVar(VarDclNode *var) {
    Name *name = var->namesym;
    return name == anonName || name == parListName || name == parBagName || name == parPieceName
        || name == parLoName || name == parHiName || name == parKName || name == parFirstName
        || name == parLastName || name == parSliceName || name == parSliceMutName
        || name == nametblFind("-recv", 5);
}

static int parCheckReach(INode *node, void *ctxp) {
    ParReach *ctx = (ParReach *)ctxp;
    if (parSetHas(ctx->copied, node))
        return 0;           // refused as a copy
    if (!isNameUseNode(node))
        return 1;
    INode *dcl = ((NameUseNode*)node)->dclnode;
    if (dcl == NULL || dcl->tag != VarDclTag)
        return 1;
    VarDclNode *var = (VarDclNode *)dcl;
    if (var->vtype == NULL || var->vtype == unknownType || parHiddenVar(var)
        || (parSetHas(&ctx->inside, dcl) && var != ctx->item))
        return 1;
    // Shareable: the item a pass is lent is its own, so it is judged without the
    // rule about references that write
    int mutrule = var != ctx->item;
    // A variable that is itself a borrow ('self', a '&mut' parameter) is read
    // through, as a '&': what it can reach is judged, not its own permission
    // (the body's writes through it are refused above, as writes to anything
    // outside). A '&mut' held inside what it reaches is another matter.
    INode *named = var->vtype;
    INode *namedcl = itypeGetTypeDcl(named);
    if ((namedcl->tag == RefTag || namedcl->tag == ArrayRefTag)
        && itypeGetTypeDcl(((RefNode *)namedcl)->region) == borrowRef)
        named = ((RefNode *)namedcl)->vtexp;
    char path[256];
    INode *culprit = itypeNotShareableWhy(named, mutrule, path, sizeof(path));
    if (culprit == NULL || parSetHas(&parReachReported, node))
        return 1;
    parSetAdd(&parReachReported, node);
    char typename[256] = "";
    itypeSpellCat(typename, sizeof(typename), var->vtype, 0);
    // Three kinds of culprit: a count or a root written without atomics, a
    // reference that writes through a shared path, and a reference to a trait or
    // a callable that does not say what is behind it
    int opaque = culprit->tag == VirtRefTag && !refCountsPlain((RefNode *)culprit) && !refWritesShared((RefNode *)culprit);
    int writes = !refCountsPlain((RefNode *)culprit) && !opaque;
    // A closure passed in: say which variable it holds or borrows, which is
    // where the fix is
    INode *held = itypeGetTypeDcl(var->vtype);
    while (held != NULL && (held->tag == RefTag || held->tag == ArrayRefTag || held->tag == VirtRefTag))
        held = itypeGetTypeDcl(((RefNode *)held)->vtexp);
    ClosureCap *cap = held != NULL && closureOfStruct(held) != NULL
        ? genericClosureNotShareableCap(held, mutrule) : NULL;
    if (cap != NULL) {
        char *capname = &cap->dcl->namesym->namestr;
        char sentence[600], reason[512] = "";
        genericCapSentence(cap, culprit, sentence, sizeof(sentence));
        genericNotShareableReason(culprit, 0, reason, sizeof(reason));
        if (opaque)
            errorMsgNode(node, ErrorParReach,
                "This 'parallel each' reaches '%s', the closure passed in, and %s: %s. The passes run at the same time, so each would call whatever it is.",
                &var->namesym->namestr, sentence, reason);
        else if (writes)
            errorMsgNode(node, ErrorParReach,
                "This 'parallel each' reaches '%s', the closure passed in, and %s: %s. The passes run at the same time, so each would hold that reference and could write through it. Give the closure a copy of what it needs by value in its list, as '[n = *%s]', instead of the reference.",
                &var->namesym->namestr, sentence, reason, capname);
        else
            errorMsgNode(node, ErrorParReach,
                "This 'parallel each' reaches '%s', the closure passed in, and %s: %s. The passes run at the same time, so each would copy it. Where '%s' is made, hold it in an Arc ('Arc[imm, ...]'), and give the closure the Arc in its list: '[%s]'.",
                &var->namesym->namestr, sentence, reason, capname, capname);
    }
    else if (opaque) {
        char reason[512] = "", culpritname[256] = "";
        itypeSpellCat(culpritname, sizeof(culpritname), culprit, 0);
        genericNotShareableReason(culprit, 1, reason, sizeof(reason));
        errorMsgNode(node, ErrorParReach,
            "This 'parallel each' reaches '%s', a %s declared outside the loop%s%s%s%s: %s.",
            &var->namesym->namestr, typename, path[0] ? ", which holds a " : "", path[0] ? culpritname : "",
            path[0] ? " in " : "", path, reason);
    }
    else if (writes) {
        char reason[512] = "", culpritname[256] = "";
        itypeSpellCat(culpritname, sizeof(culpritname), culprit, 0);
        genericNotShareableReason(culprit, 0, reason, sizeof(reason));
        errorMsgNode(node, ErrorParReach,
            "This 'parallel each' reaches '%s', a %s declared outside the loop, which holds a %s%s%s%s: %s. Its passes run at the same time, and naming it at all gives each of them a path to write through. Copy what the loop needs into a local before it ('imm n = %s.n;') and use the local in the body.",
            &var->namesym->namestr, typename, culpritname, path[0] ? " (" : "", path, path[0] ? ")" : "", reason,
            &var->namesym->namestr);
    }
    else if (var == ctx->item)
        errorMsgNode(node, ErrorParReach,
            "This 'parallel each' reaches its item '%s', a %s, and the source's items hold a counted owner whose count is not atomic (an Rc, for example) or a traced reference (a Gc, for example): its passes run at the same time, and a copy made anywhere they can reach it, in this body or in a function it hands the item to, would write a count or a root that every pass shares without an atomic operation. Walk a source whose items are safe to share (numbers, structs of them, Arcs), or its indexes and look the data up from a borrow taken before the loop.",
            &var->namesym->namestr, typename);
    else if (!parIsCountedItself(var->vtype))
        errorMsgNode(node, ErrorParReach,
            "This 'parallel each' reaches '%s', a %s declared outside the loop, which holds a counted owner whose count is not atomic (an Rc, for example) or a traced reference (a Gc, for example), in a field, an element or a type argument: its passes run at the same time, and a copy made anywhere they can reach it, in this body or in a function it hands it to, would write a count or a root that every pass shares without an atomic operation. Naming it at all counts, even for a number beside the owner. Copy what the loop needs into a local before it ('imm n = %s.n;') and use the local in the body, or hold the owner in an Arc.",
            &var->namesym->namestr, typename, &var->namesym->namestr);
    else
        errorMsgNode(node, ErrorParReach,
            "This 'parallel each' reaches '%s', a %s declared outside the loop, which is or holds a counted owner whose count is not atomic (an Rc, for example) or a traced reference (a Gc, for example): its passes run at the same time, and a copy made anywhere they can reach it, in this body or in a function it hands it to, would write a count or a root that every pass shares without an atomic operation. Reading through it, lending it and passing it count too. Borrow what the body needs before the loop ('imm mesh = &*%s;') and use 'mesh' in the body, or hold it in an Arc.",
            &var->namesym->namestr, typename, &var->namesym->namestr);
    return 1;
}

// ---- An outside reference that could point at what the loop writes --------------
//
// A loop that writes through its source (a cursor lending '&mut': mutChunks,
// mutItems, a zip of them, or a '&mut' slice) writes the elements one pass at a
// time. The body may not name the place they are lent from (parCheckAlias), but a
// borrow taken before the loop and passed in, a parameter or a field of a struct, can
// point at the same element, and reading it while a pass writes races (flow
// analysis sees only a local). So the body may not read, from an outside variable,
// a place that could be one of the written items, judged by the PATH read
// (parPlaceReaches): a struct reached through a reference that is a written type, an
// element of that type indexed behind a reference, a reference to one; the place's
// own type, a bare name and a call in the chain are judged whole (parReachesWritten,
// not through a raw pointer). A scalar field beside the buffer (self.scale), and a
// value copied out before the loop, reach none. What is written is read from the
// cursor types (parCursorOf), never from a method's name.

#define ParAliasMax 24

typedef struct {
    ParSet inside;              // The variables declared in the body, and the loop's own
    ParSet *aliased;            // The places parCheckAlias already refused
    INode *set[ParAliasMax];    // The written element, and what it holds inline
    uint32_t nset;
    INode *elset[ParAliasMax];  // The written element, and the elements of arrays it holds inline: what a collection's element can be
    uint32_t nelset;
    INode *elem;                // The first written element, for the message
    char *source;               // The place they are lent from, for the message
} ParAliasOuter;

// The element types a cursor lends '&mut', from the cursor's own types
static void parWrittenElems(INode *type, INode **elems, uint32_t *n) {
    type = type ? itypeGetTypeDcl(type) : NULL;
    if (type == NULL)
        return;
    if (type->tag == ArrayRefTag || type->tag == RefTag) {
        // A '&mut' slice as the source itself
        RefNode *ref = (RefNode *)type;
        if (permMatches((INode*)mutPerm, ref->perm) && *n < ParAliasMax) {
            INode *elem = itypeLenBodyElem(ref->vtexp);
            elems[(*n)++] = elem != NULL ? elem : ref->vtexp;
        }
        return;
    }
    if (parIsCore(type, "ArrayMutChunks", 14) || parIsCore(type, "ArrayMutItems", 13)
        || parIsCore(type, "MutItemsIndexed", 15)) {
        // The cursor is an instance of core's ['a, T]: the element is its one type
        // argument (the lifetime is not a type)
        Nodes *args = itypeInstanceTypeArgs(type);
        if (args != NULL && args->used == 1 && *n < ParAliasMax)
            elems[(*n)++] = nodesGet(args, 0);
    }
    else if (parIsCore(type, "MutChunksIndexed", 16))
        parWrittenElems(parFieldType(type, "chunks"), elems, n);
    else if (parIsCore(type, "Zip", 3) || parIsCore(type, "Zip3", 4)) {
        parWrittenElems(parFieldType(type, "first"), elems, n);
        parWrittenElems(parFieldType(type, "second"), elems, n);
        if (parIsCore(type, "Zip3", 4))
            parWrittenElems(parFieldType(type, "third"), elems, n);
    }
    else if (parIsCore(type, "ZipIndexed", 10) || parIsCore(type, "Zip3Indexed", 11))
        parWrittenElems(parFieldType(type, "zip"), elems, n);
}

// The type, and the types it holds inline (a field, a tuple's or array's element): a
// borrow of any of them can point into a written element
static void parAddElemType(INode *type, ParAliasOuter *ctx) {
    type = type ? itypeGetTypeDcl(type) : NULL;
    if (type == NULL || ctx->nelset >= ParAliasMax)
        return;
    for (uint32_t i = 0; i < ctx->nelset; ++i) {
        if (itypeIsSame(ctx->elset[i], type))
            return;
    }
    ctx->elset[ctx->nelset++] = type;
}

static void parInlineTypes(INode *type, ParAliasOuter *ctx, int depth) {
    type = type ? itypeGetTypeDcl(type) : NULL;
    if (type == NULL || depth > 6 || ctx->nset >= ParAliasMax)
        return;
    if (depth == 0)
        parAddElemType(type, ctx);      // (a collection of the written type holds the items themselves)
    for (uint32_t i = 0; i < ctx->nset; ++i) {
        if (itypeIsSame(ctx->set[i], type))
            return;
    }
    ctx->set[ctx->nset++] = type;
    INode **nodesp;
    uint32_t cnt;
    switch (type->tag) {
    case ArrayTag:
        parAddElemType(arrayElemType(type), ctx);   // (so does a slice of an array inline in it)
        parInlineTypes(arrayElemType(type), ctx, depth + 1);
        break;
    case TTupleTag:
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp))
            parInlineTypes(*nodesp, ctx, depth + 1);
        break;
    case StructTag:
        for (nodelistFor(&((StructNode *)type)->fields, cnt, nodesp))
            parInlineTypes(((IExpNode *)*nodesp)->vtype, ctx, depth + 1);
        break;
    default:
        break;
    }
}

// Does a value of this type reach, behind a reference, owner or type argument, a
// type of the written set? (A raw pointer is trusted: nothing is followed behind one.)
//
// 'behind' says the value was reached through a reference somewhere above, so a
// collection in it may be the one the loop writes; 'elemlike' says this type is
// itself an object that may be a written element: the pointee of a reference, or an
// element or type argument of a collection reached behind one. A field held inline
// is a part of its struct, not an element, so it is compared only when the struct is
// (and searched for the references and collections it holds). A type argument the
// struct also holds by value (an Atomic's, an Option's) is a value, not elements.
static int parInWritten(INode *type, ParAliasOuter *ctx) {
    for (uint32_t i = 0; i < ctx->nset; ++i) {
        if (itypeIsSame(type, ctx->set[i]))
            return 1;
    }
    return 0;
}

// ... and an element of a collection or array: only the written element itself, or
// the element of an array it holds inline, is one (a List of a part it holds is a
// separate buffer)
static int parInElems(INode *type, ParAliasOuter *ctx) {
    for (uint32_t i = 0; i < ctx->nelset; ++i) {
        if (itypeIsSame(type, ctx->elset[i]))
            return 1;
    }
    return 0;
}

// Does this struct (or one of its variants) hold a value of this type in a field?
static int parHoldsByValue(StructNode *strnode, INode *arg) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        INode *ftype = ((IExpNode *)*nodesp)->vtype;
        if (ftype != NULL && itypeIsSame(ftype, arg))
            return 1;
    }
    if (strnode->derived) {
        for (nodesFor(strnode->derived, cnt, nodesp)) {
            if ((*nodesp)->tag == StructTag && parHoldsByValue((StructNode *)*nodesp, arg))
                return 1;
        }
    }
    return 0;
}

static int parReachesWritten(INode *type, int behind, int elemlike, ParAliasOuter *ctx, INode **seen, uint32_t *nseen) {
    if (type == NULL || *nseen > 60)
        return 0;
    switch (type->tag) {
    case NameUseTag:
        return isTypeNode(type) ? parReachesWritten(itypeGetTypeDcl(type), behind, elemlike, ctx, seen, nseen) : 0;
    case AliasDclTag:
        return parReachesWritten(((AliasDclNode *)type)->target, behind, elemlike, ctx, seen, nseen);
    default:
        break;
    }
    // (1: the pointee of a reference, which may be a part of a written item; 2: an
    // element of a collection or array, which is the item or not)
    if ((elemlike == 1 && parInWritten(type, ctx)) || (elemlike == 2 && parInElems(type, ctx)))
        return 1;
    switch (type->tag) {
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return parReachesWritten(((RefNode *)type)->vtexp, 1, 1, ctx, seen, nseen);
    case ArrayTag:
        return parReachesWritten(arrayElemType(type), behind, behind ? 2 : 0, ctx, seen, nseen);
    case TTupleTag: {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode *)type)->elems, cnt, nodesp)) {
            if (parReachesWritten(*nodesp, behind, 0, ctx, seen, nseen))
                return 1;
        }
        return 0;
    }
    case StructTag: {
        for (uint32_t i = 0; i < *nseen; ++i) {
            if (seen[i] == type)
                return 0;
        }
        seen[(*nseen)++] = type;
        StructNode *strnode = (StructNode *)type;
        INode **nodesp;
        uint32_t cnt;
        Nodes *args = itypeInstanceTypeArgs(type);
        if (args != NULL) {
            for (nodesFor(args, cnt, nodesp)) {
                if (parReachesWritten(*nodesp, behind, behind && !parHoldsByValue(strnode, *nodesp) ? 2 : 0, ctx, seen, nseen))
                    return 1;
            }
        }
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            if (parReachesWritten(((IExpNode *)*nodesp)->vtype, behind, 0, ctx, seen, nseen))
                return 1;
        }
        if (strnode->derived) {
            for (nodesFor(strnode->derived, cnt, nodesp)) {
                if (parReachesWritten(*nodesp, behind, 0, ctx, seen, nseen))
                    return 1;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

// The place a node reads, from the node outward to the variable at its root, through
// fields, elements, dereferences and borrows (a borrow of a place reads it). 0 where
// the chain is not a plain place (a call, a cast): such a use is judged where its own
// variables are named, as a whole.
#define ParChainMax 16

static int parPlaceChain(INode *node, INode **chain, uint32_t *n) {
    *n = 0;
    while (node != NULL && *n < ParChainMax) {
        chain[(*n)++] = node;
        switch (node->tag) {
        case FldAccessTag:
        case ArrIndexTag:
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
            return isNameUseNode(node);
        }
    }
    return 0;
}

static INode *parPeelRef(INode *type, int *crossed) {
    type = type ? itypeGetTypeDcl(type) : NULL;
    if (type != NULL && (type->tag == RefTag || type->tag == ArrayRefTag || type->tag == VirtRefTag)) {
        *crossed = 1;
        return itypeGetTypeDcl(((RefNode *)type)->vtexp);
    }
    return type;
}

// Is what this place reads one a pass may be writing? Judged by the PATH read: each
// struct reached through a reference may itself be a written element, an element
// indexed behind a reference is one if its type is written, a pointer is trusted, and
// what the place finally holds is judged whole. A scalar field beside the buffer
// (self.scale) reaches none of these.
static int parPlaceReaches(INode **chain, uint32_t n, ParAliasOuter *ctx) {
    int behind = 0;
    for (uint32_t i = n - 1; i > 0; --i) {
        INode *node = chain[i - 1];
        if (node->tag == BorrowTag || node->tag == ArrayBorrowTag)
            continue;
        INode *obj = chain[i];
        while (obj->tag == BorrowTag || obj->tag == ArrayBorrowTag) {
            // (an object that is a borrow expression: its place is the one read)
            obj = ((RefNode*)obj)->vtexp;
        }
        INode *objtype = isExpNode(obj) ? iexpGetTypeDcl(obj) : NULL;
        if (objtype == NULL)
            return 0;
        if (objtype->tag == PtrTag)
            return 0;               // trusted: nothing is followed behind a raw pointer
        int crossed = 0;
        INode *pointee = parPeelRef(objtype, &crossed);
        if (node->tag == DerefTag && !crossed)
            return 0;
        if (crossed) {
            behind = 1;
            if (pointee != NULL && parInWritten(pointee, ctx))
                return 1;           // the struct, or the item, it points at may be a written element
        }
        if (node->tag == ArrIndexTag) {
            int ignore = 0;
            INode *elem = parPeelRef(isExpNode(node) ? iexpGetTypeDcl(node) : NULL, &ignore);
            if (behind && elem != NULL && parInElems(elem, ctx))
                return 1;
        }
    }
    uint32_t top = 0;
    while (top + 1 < n && (chain[top]->tag == BorrowTag || chain[top]->tag == ArrayBorrowTag))
        ++top;
    INode *finaltype = isExpNode(chain[top]) ? iexpGetTypeDcl(chain[top]) : NULL;
    if (chain[top]->tag == ArrIndexTag) {
        // (an index gives a borrow of the element: it is the element that is read)
        int ignore = 0;
        finaltype = parPeelRef(finaltype, &ignore);
    }
    INode *seen[64];
    uint32_t nseen = 0;
    return parReachesWritten(finaltype, behind, 0, ctx, seen, &nseen);
}

static ParSet parOuterReported;

static int parCheckOuterAlias(INode *node, void *ctxp) {
    ParAliasOuter *ctx = (ParAliasOuter *)ctxp;
    if (parSetHas(ctx->aliased, node))
        return 0;           // refused as naming the place the items are lent from
    // The outermost plain place rooted at a variable is judged by its path; a bare
    // name is a path of one (judged whole); a call in the chain is not a plain place,
    // and the variables named inside it are judged as bare names
    switch (node->tag) {
    case FldAccessTag:
    case ArrIndexTag:
    case DerefTag:
    case BorrowTag:
    case ArrayBorrowTag:
    case NameUseTag:
        break;
    default:
        return 1;
    }
    INode *chain[ParChainMax];
    uint32_t n;
    if (!parPlaceChain(node, chain, &n)) {
        // (the place's own variable is met further in, and the indexes in it are walked)
        return 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (parSetHas(ctx->aliased, chain[i]))
            return 0;       // a part of it was refused as naming the place the items are lent from
    }
    INode *root = chain[n - 1];
    INode *dcl = ((NameUseNode*)root)->dclnode;
    if (dcl == NULL || dcl->tag != VarDclTag)
        return 1;
    VarDclNode *var = (VarDclNode *)dcl;
    if (var->vtype == NULL || var->vtype == unknownType || parHiddenVar(var) || parSetHas(&ctx->inside, dcl))
        return 1;
    if (!parPlaceReaches(chain, n, ctx) || parSetHas(&parOuterReported, node)) {
        // The place is taken whole: only the indexes inside it are uses of their own
        parWalkIndexArgs(node, parCheckOuterAlias, ctx);
        return 0;
    }
    parSetAdd(&parOuterReported, node);
    char elemname[256] = "";
    itypeSpellCat(elemname, sizeof(elemname), ctx->elem, 0);
    char *name = &var->namesym->namestr;

    // A closure passed in (the loop is in a generic that calls it): say which variable
    // of the caller's the closure borrows, which is where the fix is
    int crossed = 0;
    INode *held = parPeelRef(var->vtype, &crossed);
    ClosureInfo *closure = held != NULL ? closureOfStruct(held) : NULL;
    for (uint32_t c = 0; closure != NULL && c < closure->ncaps; ++c) {
        ClosureCap *cap = &closure->caps[c];
        if (cap->state || cap->field == NULL || cap->dcl == NULL)
            continue;
        INode *seen[64];
        uint32_t nseen = 0;
        if (!parReachesWritten(cap->field->vtype, 0, 0, ctx, seen, &nseen))
            continue;
        char captype[256] = "";
        itypeSpellCat(captype, sizeof(captype), cap->field->vtype, 0);
        char *capname = &cap->dcl->namesym->namestr;
        // (borrowed, it is a reference to a value; copied in, would that value still reach?)
        INode *seen2[64];
        uint32_t nseen2 = 0;
        if (!parReachesWritten(cap->dcl->vtype, 0, 0, ctx, seen2, &nseen2))
            errorMsgNode(node, ErrorParAlias,
                "This 'parallel each' writes the items of %s (%s), and the closure passed as '%s' borrows '%s' (%s), which could point at one of them: reading it while a pass writes would race. Copy it into the closure, where it is a value of its own: write it in the state list, '[%s]', as in 'fn(x i32, y i32) [%s] T { ... }'.",
                ctx->source, elemname, name, capname, captype, capname, capname);
        else
            errorMsgNode(node, ErrorParAlias,
                "This 'parallel each' writes the items of %s (%s), and the closure passed as '%s' borrows '%s' (%s), which could be the very list this loop writes, or point into it: reading it while a pass writes would race. Hand the closure what it needs as a value copied before the loop instead of '%s'.",
                ctx->source, elemname, name, capname, captype, capname);
        parWalkIndexArgs(node, parCheckOuterAlias, ctx);
        return 0;
    }
    errorMsgNode(node, ErrorParAlias,
        "This 'parallel each' writes the items of %s (%s), and what this reads through '%s' could be one of them: reading it while a pass writes would race. Copy what you need before the loop ('imm f = *%s;', 'imm n = %s.n;') and use the copy in the body.",
        ctx->source, elemname, name, name, name);
    parWalkIndexArgs(node, parCheckOuterAlias, ctx);
    return 0;
}

// ---- A Gc in an actor or in a parallel each: refused, for now -------------------
//
// The collector keeps its heap and its one chain of roots in process globals,
// without locks, and a 'Gc' held in an actor's state is on no stack between
// messages, so a traced reference made, held or copied by an actor, or by the
// passes of a parallel each, corrupts it. Until the collector is per actor, they
// are refused. What counts is what itypeHoldsTraced does (a traced reference,
// or a tuple, array, struct or enum holding one inline; not what a borrow or a
// raw pointer reaches). A copy of a place is not a new Gc: one made outside the
// loop is parCheckCopies's refusal, one made inside is refused where it was made.

static ParSet gcReported;

// Refuse a node of a type holding a traced reference, once
static void gcRefuse(INode *node, INode *type, const char *what, const char *where) {
    if (parSetHas(&gcReported, node))
        return;
    parSetAdd(&gcReported, node);
    char typename[256] = "";
    itypeSpellCat(typename, sizeof(typename), type, 0);
    errorMsgNode(node, ErrorGcStopgap,
        "A traced reference (a Gc, for example) cannot be used inside an actor or a parallel each yet: this %s a %s, which a collector traces, inside %s. GC per actor is coming; until it lands, the collector's one chain of roots and its heap are not safe on the threads an actor or a parallel each runs on. Use Gc only in code that runs on a single thread.",
        what, typename, where);
}

static int gcRefuseVisit(INode *node, void *where) {
    switch (node->tag) {
    case AllocateTag:
    case FnCallTag:
    case TypeLitTag: {
        INode *type = isExpNode(node) ? ((IExpNode *)node)->vtype : NULL;
        if (type != NULL && itypeHoldsTraced(type)) {
            gcRefuse(node, type, node->tag == AllocateTag ? "makes" : "produces", (const char *)where);
            return 0;
        }
        break;
    }
    default:
        break;
    }
    return 1;
}

void gcStopgapCheckBody(INode *body, const char *where) {
    parWalk(body, gcRefuseVisit, (void *)where);
}

void gcStopgapCheckDcl(INode *node, INode *type, const char *where) {
    if (type != NULL && itypeHoldsTraced(type))
        gcRefuse(node, type, "holds", where);
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

    // Nor names anything outside that is not safe to share across the passes
    ParReach reach;
    memset(&reach, 0, sizeof(reach));
    reach.inside = ctx.inside;
    reach.copied = &copies.reported;
    if (loop->stmts->used > 1 && nodesGet(loop->stmts, 1)->tag == VarDclTag)
        reach.item = (VarDclNode *)nodesGet(loop->stmts, 1);
    parWalk((INode*)loop, parCheckReach, &reach);

    // Nor makes a Gc, in its header, its filter, its body or its yield (for now)
    parWalk((INode*)outer, gcRefuseVisit, (void *)"a 'parallel each' body");

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
            if (((VarDclNode*)stmt)->value != NULL)
                parLentPaths(((VarDclNode*)stmt)->value, &alias);
            if (alias.nlent > 0)
                parWalk((INode*)loop, parCheckAlias, &alias);

            // Nor an outside reference that could point at what the loop writes
            ParAliasOuter outer_alias;
            memset(&outer_alias, 0, sizeof(outer_alias));
            INode *written[ParAliasMax];
            uint32_t nwritten = 0;
            parWrittenElems(((VarDclNode*)stmt)->vtype, written, &nwritten);
            if (nwritten > 0) {
                for (uint32_t w = 0; w < nwritten; ++w)
                    parInlineTypes(written[w], &outer_alias, 0);
                outer_alias.inside = ctx.inside;
                outer_alias.aliased = &alias.reported;
                outer_alias.elem = written[0];
                outer_alias.source = "its sources";
                if (alias.nlent == 1 && alias.lent[0].root->namesym != anonName)
                    outer_alias.source = &alias.lent[0].root->namesym->namestr;
                parWalk((INode*)loop, parCheckOuterAlias, &outer_alias);
            }
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

// ---- Parallel reductions: 'xs.parallel().sum()' ---------------------------------
//
// `parallel()` is an ordinary method (core's Array and cursors give a view, a
// ParallelSlice, or the cursor itself). The three reductions of that view, 'sum', 'fold' and
// 'findFirst', run on the actors' workers, which core cannot reach, so they are
// functions of the actors package (reduce.cone) and the call is written as a call
// of them: 'xs.parallel().sum()' is 'actors.parSum(xs.parallel())'. That is done
// here, in name resolution, where the call is only what the reader wrote, by its
// shape: a method named sum, fold or findFirst called directly on a '.parallel()'
// with no arguments. Everything after is an ordinary call of a generic function.
// A number range is no value, so '(lo < hi).parallel()' is the call
// 'parallelNumbers(lo, hi)' of core's, and '<=' 'parallelNumbersThrough'.

static Name *parReduceName(FnCallNode *call, Name *a, Name *b, Name *c) {
    if (call->methfld == NULL || !isNameUseNode(call->methfld))
        return NULL;
    Name *name = ((NameUseNode*)call->methfld)->namesym;
    return name == a || name == b || name == c ? name : NULL;
}

// Is this call a method call written 'x.parallel()'?
static int parIsParallelCall(INode *node) {
    if (node == NULL || node->tag != FnCallTag)
        return 0;
    FnCallNode *call = (FnCallNode*)node;
    if (call->flags & (FlagOperator | FlagIndex | FlagNew))
        return 0;
    if (call->args != NULL && call->args->used != 0)
        return 0;
    return parReduceName(call, nametblFind("parallel", 8), NULL, NULL) != NULL;
}

// Is this call a reduction called on a '.parallel()'? Asked before the call is name
// resolved, of what was written
int parallelReduceIs(FnCallNode *node) {
    if (node->flags & (FlagOperator | FlagIndex | FlagNew))
        return 0;
    if (!parIsParallelCall(node->objfn))
        return 0;
    return parReduceName(node, nametblFind("sum", 3), nametblFind("fold", 4), nametblFind("findFirst", 9)) != NULL;
}

// The reduction, its parts name resolved: the call of the actors package's function
// with the view first, 'actors.parSum(view)'
void parallelReduceNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    Name *method = ((NameUseNode*)node->methfld)->namesym;
    char *fnname = method == nametblFind("sum", 3) ? "parSum"
        : method == nametblFind("fold", 4) ? "parFold" : "parFindFirst";
    ModuleNode *actorsmod = pstate->mod ? parFindImport(pstate->mod->imports, "actors") : NULL;
    INode *run = actorsmod ? namespaceFind(&actorsmod->namespace, nametblFind(fnname, (uint32_t)strlen(fnname))) : NULL;
    if (run == NULL || run->tag != FnDclTag) {
        errorMsgNode((INode*)node, ErrorParReduce,
            "'%s' on a parallel view runs on the actors package's workers, which module %s does not import: write 'import actors;' after the 'mod' line.",
            &method->namestr, pstate->mod ? &pstate->mod->namesym->namestr : "this");
        *((INode**)nodep) = newErrorNode((INode*)node);
        return;
    }
    NameUseNode *member = newNameUseNode(nametblFind(fnname, (uint32_t)strlen(fnname)));
    inodeLexCopy((INode*)member, node->methfld);
    member->dclnode = run;
    member->flags |= FlagQualified;
    nameUseMarkExpandReached(pstate, member);

    uint32_t nold = node->args ? node->args->used : 0;
    Nodes *args = newNodes(1 + nold);
    nodesAdd(&args, node->objfn);
    for (uint32_t i = 0; i < nold; ++i)
        nodesAdd(&args, nodesGet(node->args, i));
    node->objfn = (INode*)member;
    node->methfld = NULL;
    node->args = args;
}

// A method that is not found, asked of a type: if it is `parallel` on a source that
// cannot be cut, or a reduction on a parallel view held in a variable, say so (and
// answer 1), else answer 0 and leave the plain message to the caller
int parallelViewNotFound(FnCallNode *callnode, INode *objdereftype, Name *methsym) {
    char *tname = isMethodType(objdereftype) && inodeGetName(objdereftype)
        ? &inodeGetName(objdereftype)->namestr : "this";
    if (parIsCore(objdereftype, "IterMap", 7) || parIsCore(objdereftype, "IterFilter", 10)
        || parIsCore(objdereftype, "IterTake", 8) || parIsCore(objdereftype, "IterSkip", 8))
        tname = "A chain of iterator adapters ('map', 'filter', 'take', 'skip')";
    if (methsym == nametblFind("parallel", 8)) {
        errorMsgNode((INode*)callnode, ErrorParReduce,
            "`parallel()` makes a view of a source that can be cut into pieces: a list, an array or a slice, the runs of `chunks`, an `iter()` or `indexed()` of those, a zip of them, or a number range written `(0 < n).parallel()`. %s hands out its items one after another and cannot be cut. Collect it into a list first, or walk the list it starts from.",
            tname);
        return 1;
    }
    if ((parIsCore(objdereftype, "ParallelSlice", 13) || parIsCore(objdereftype, "ParallelNumbers", 15))
        && parReduceName(callnode, nametblFind("sum", 3), nametblFind("fold", 4), nametblFind("findFirst", 9)) != NULL) {
        errorMsgNode((INode*)callnode, ErrorParReduce,
            "`%s` is called directly on `parallel()`, as `xs.parallel().%s(...)`: a parallel view held in a variable has no methods of its own, because the reduction runs on the actors package's workers.",
            &methsym->namestr, &methsym->namestr);
        return 1;
    }
    return 0;
}

// '(lo < hi).parallel()' and '(lo <= hi).parallel()': a number range as a source
int parallelRangeIs(FnCallNode *node) {
    if (!parIsParallelCall((INode*)node) || node->objfn->tag != FnCallTag)
        return 0;
    FnCallNode *range = (FnCallNode*)node->objfn;
    if (!(range->flags & FlagOperator) || range->args == NULL || range->args->used != 1)
        return 0;
    return parReduceName(range, nametblFind("<", 1), nametblFind("<=", 2), NULL) != NULL;
}

void parallelRangeNameRes(NameResState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    FnCallNode *range = (FnCallNode*)node->objfn;
    int incl = ((NameUseNode*)range->methfld)->namesym == nametblFind("<=", 2);
    char *fnname = incl ? "parallelNumbersThrough" : "parallelNumbers";
    NameUseNode *callee = newNameUseNode(nametblFind(fnname, (uint32_t)strlen(fnname)));
    inodeLexCopy((INode*)callee, node->methfld);
    INode *calleep = (INode*)callee;
    inodeNameRes(pstate, &calleep);
    FnCallNode *call = newFnCallNode(calleep, 2);
    inodeLexCopy((INode*)call, (INode*)node);
    nodesAdd(&call->args, range->objfn);
    nodesAdd(&call->args, nodesGet(range->args, 0));
    *((INode**)nodep) = (INode*)call;
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
            // A call lends from each argument that holds a borrow (a zip is given
            // two cursors): if one may be the frame, so may the result
            int seen = 0;
            for (uint32_t i = 0; i < args->used; ++i) {
                if (parArgLends(nodesGet(args, i))) {
                    seen = 1;
                    if (parMayBeFrame(nodesGet(args, i)))
                        return 1;
                }
            }
            if (seen)
                return 0;
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
    // A GPU has no threads to run the passes on: its invocations are the parallelism
    if (flowGpu) {
        errorMsgNode(lexnode, ErrorGpuUnavailable,
            "In GPU code there is no 'parallel each': it runs its passes on worker threads, which a GPU has none of. A kernel is already run by thousands of invocations at once, each taking its part by its id.");
        return 0;
    }
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
    else if (parIsCore(type, "IterMap", 7) || parIsCore(type, "IterFilter", 10)
        || parIsCore(type, "IterTake", 8) || parIsCore(type, "IterSkip", 8))
        errorMsgNode(src, ErrorParSource,
            "A 'parallel each' splits its source into pieces that run at the same time, so the source must report its size and split ('len' and 'split': a ParallelIterable). A chain of iterator adapters ('map', 'filter', 'take', 'skip') hands out its items one after another, with 'next', and cannot be split. Walk the list the chain starts from, and put the adapter's work in the loop's own header and 'yield' ('parallel each x in xs if cond yield expr'), or collect the chain into a list first.");
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

        int isref = type->tag == RefTag || type->tag == VirtRefTag;
        INode *base = isref ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
        int isslice = type->tag == ArrayRefTag;
        int isarray = base->tag == ArrayTag;
        // The cursors core gives (parCursorOf): 'chunks(n)', 'mutChunks(n)', 'mutItems()'
        // and the ones 'indexed()' gives. The passes are their items, walked by 'len' and
        // 'at(k)', and what is lent is what the cursor's own types say. A chunk is a
        // slice no other pass can reach. An indexed one gives two variables, the
        // position (its own, so global) and the item
        ParCursor cursor = parCursorOf(isref ? NULL : type);
        int ischunks = cursor.cursor;
        // A zip is walked by position only when both its sources can be (they are
        // RandomAccess and ParallelIterable): else its 'len' and 'at' are not there
        if (ischunks && !(parHasMethod(type, lenName) && parHasMethod(type, nametblFind("at", 2)))) {
            errorMsgNode(src, ErrorParSource,
                "A 'parallel each' over %s needs every source of the zip to report its size and give its items by position, and one of them does not: a generator, a Deque's cursor, a file or a channel hands out its items one after another and cannot be split. Collect it into a list first, and zip the list. (The cursors of arrays, slices and lists, mutItems(), chunks(n) and mutChunks(n) can.)",
                itypeName(type));
            return;
        }
        if (nvars != (uint32_t)cursor.vars) {
            const char *why =
                "A 'parallel each' gives one variable, a borrow of each element (or each number of a range); two come from indexed(), the position and the item, or from a zip, one item from each source.";
            if (cursor.vars == 4)
                why = "A 'parallel each' over a three-way zip's indexed() gives four variables, the position and one item from each of the three sources.";
            else if (cursor.vars == 3 && parIsCore(type, "Zip3", 4))
                why = "A 'parallel each' over a three-way zip gives three variables, one item from each source (its indexed() adds the position first, four variables).";
            else if (cursor.vars == 3)
                why = "A 'parallel each' over a zip's indexed() gives three variables, the position and one item from each source.";
            else if (cursor.vars == 2 && parIsCore(type, "Zip", 3))
                why = "A 'parallel each' over a zip gives two variables, one item from each source (its indexed() adds the position first, three variables).";
            else if (cursor.vars == 2)
                why = "A 'parallel each' over indexed() gives two variables, the position and the item.";
            errorMsgNode(nodesGet(loop->stmts, 0), ErrorParSource, "%s", why);
            return;
        }
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
                ischunks ? (cursor.chunks ? "chunks" : "items") : isslice ? "slice" : "array");
            return;
        }

        // A mutable slice as the source itself lends each item mutably, as 'each'
        // does; any other lends it to be read. A cursor lends what its type says
        int mutlend = ischunks ? cursor.mut
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
            // imm x = s.at(k): item number k (a chunk is a slice of its own); over
            // an indexed cursor, the tuple of its position and the item
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
        elemindexed = cursor.vars >= 2;
    }

    // loop { if k >= hi {break}; imm x = ...; k++; ...body... }
    // (over an indexed cursor: imm -pair = s.at(k); imm i = -pair.0; imm x = -pair.1; k++)
    uint32_t stepat = nvars;
    if (elemindexed) {
        VarDclNode *pair = parVar(parPairName, immPerm, elem, (uint16_t)(scope + 1), lexnode);
        nodesInsert(&loop->stmts, (INode*)pair, 0);
        for (uint32_t i = 0; i < nvars; ++i) {
            VarDclNode *var = (VarDclNode*)nodesGet(loop->stmts, i + 1);
            FnCallNode *part = newFnCallLower((INode*)var, parUse(pair, lexnode), 0);
            part->methfld = (INode*)newULitNode(i, (INode*)usizeType);
            inodeLexCopy(part->methfld, lexnode);
            var->value = (INode*)part;
        }
        stepat = nvars + 1;
    }
    else
        ((VarDclNode*)nodesGet(loop->stmts, 0))->value = elem;
    FnCallNode *step = newFnCallOpnameLower(lexnode, parUse(k, lexnode), incrPostName, 0);
    step->flags |= FlagLvalOp;
    nodesInsert(&loop->stmts, (INode*)step, stepat);
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
