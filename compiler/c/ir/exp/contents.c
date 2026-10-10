/** Contents after '<-': the entries of an append list, and its lowering
 * @file
 *
 * What follows '<-' is a list of entries, each a value appended in turn:
 *
 *   xs <- 1, 2, 3;                  // listed values
 *   xs <- 4 of 0;                   // n values, the value evaluated for each
 *   xs <- fill 0;                   // values until the collection is full
 *   dict <- "a": 1, "b": 2;         // pairs, given to a two-value append
 *   imm ys = new List[i32](8) <- fill 0;    // a construction's contents
 *
 * Every entry lowers to applications of the receiver's '<-' method, against
 * one borrow of the receiver. A construction's contents are appended to the
 * value it builds, which is then the expression's value; an array's are its
 * elements, since an array has no '<-' and its size is fixed (its scalars,
 * when its type is written with several sizes), and an allocation of an array
 * fills them in place in the region's memory.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <limits.h>
#include <string.h>

// Create an entry node: 'n of x', 'fill x', 'k: v', an each or a yield, its value filled in after
EntryNode *newEntryNode(uint16_t tag, INode *first) {
    EntryNode *node;
    newNode(node, EntryNode, tag);
    node->vtype = unknownType;
    node->first = first;
    node->val = NULL;
    node->recv = NULL;
    node->nest = 0;
    node->drain = 0;
    return node;
}

// Clone an entry. A yield's receiver is set by the '<-' that lowers its loop,
// after any cloning, so it is not followed.
INode *cloneEntryNode(CloneState *cstate, EntryNode *node) {
    EntryNode *newnode = memAllocBlk(sizeof(EntryNode));
    memcpy(newnode, node, sizeof(EntryNode));
    newnode->first = cloneNode(cstate, node->first);
    newnode->val = cloneNode(cstate, node->val);
    newnode->recv = NULL;
    return (INode *)newnode;
}

// Serialize an entry as it is written
void entryPrint(EntryNode *node) {
    switch (node->tag) {
    case OfEntryTag:
        inodePrintNode(node->first);
        inodeFprint(" of ");
        break;
    case FillEntryTag:
        inodeFprint("fill ");
        break;
    case PairEntryTag:
        inodePrintNode(node->first);
        inodeFprint(": ");
        break;
    case EachEntryTag:
        inodeFprint("(<- ");
        inodePrintNode(node->first);
        inodeFprint(")");
        return;
    case YieldEntryTag:
        inodeFprint("yield ");
        if (node->first) {
            inodePrintNode(node->first);
            inodeFprint(": ");
        }
        break;
    }
    inodePrintNode(node->val);
}

// Name resolution of an entry: a count or key, and the value. An each holds its
// loop, a block of the scopes the loop's variables are numbered in; lowering
// puts that block inside the blocks it makes to hold the receiver (nest of
// them), so it is numbered as if inside them too.
void entryNameRes(NameResState *pstate, EntryNode *node) {
    if (node->tag == EachEntryTag) {
        pstate->scope += node->nest;
        inodeNameRes(pstate, &node->first);
        pstate->scope -= node->nest;
        return;
    }
    if (node->first)
        inodeNameRes(pstate, &node->first);
    inodeNameRes(pstate, &node->val);
}

// Is this a construction, 'new T(...)' or 'trynew T(...)', not yet lowered?
static int contentsIsConstruction(INode *node) {
    return node->tag == FnCallTag && (node->flags & FlagNew)
        && !nameUseNames(((FnCallNode*)node)->objfn, FnDclTag);
}

// Is this '<-' one contentsLower takes apart: contents after a construction,
// several entries, or an entry that is not a plain value?
int contentsIsAppend(FnCallNode *node) {
    if (node->methfld == NULL || !isNameUseNode(node->methfld)
        || ((NameUseNode*)node->methfld)->namesym != lessDashName || !(node->flags & FlagOpAssgn)
        || node->args == NULL || node->args->used != 1)
        return 0;
    INode *arg = nodesGet(node->args, 0);
    return contentsIsConstruction(node->objfn) || arg->tag == VTupleTag || isEntryNode(arg);
}

static void yieldEntryLower(TypeCheckState *pstate, INode **nodep);

// The '<-' that owns an entry takes it apart before it is checked
// (contentsLower), so one reached here sits where no '<-' takes it: inside a
// tuple value that is itself appended, '(1, 2 of 0)'. A yield is the exception:
// it is reached as the last statement of its loop's body, and is lowered here.
void entryTypeCheck(TypeCheckState *pstate, INode **nodep) {
    EntryNode *node = (EntryNode*)*nodep;
    if (node->tag == YieldEntryTag) {
        yieldEntryLower(pstate, nodep);
        return;
    }
    errorMsgNode((INode*)node, ErrorEntryPlace,
        "%s is an entry of the list on the right of '<-', and is written only there: 'xs <- 3 of 0', 'xs <- fill 0', 'xs <- each ys', 'dict <- key: value'.",
        node->tag == OfEntryTag ? "'n of x'" : node->tag == FillEntryTag ? "'fill x'"
        : node->tag == EachEntryTag ? "An 'each' that drains a source or yields values" : "A pair 'k: v'");
    node->vtype = errorType;
}

// A use of a variable the lowering declared, positioned on lexnode
static INode *contentsVarUse(VarDclNode *var, INode *lexnode) {
    NameUseNode *use = newNameUseNode(var->namesym);
    inodeLexCopy((INode*)use, lexnode);
    use->dclnode = (INode*)var;
    return (INode*)use;
}

// A variable the lowering declares, in the block at 'scope'
static VarDclNode *contentsVar(Name *name, INode *type, PermNode *perm, INode *val, uint16_t scope, INode *lexnode) {
    VarDclNode *var = newVarDclFull(name, VarDclTag, type, (INode*)perm, val);
    inodeLexCopy((INode*)var, lexnode);
    var->scope = scope;
    return var;
}

// '*recv <- value': one application of the receiver's '<-', positioned on the value
static FnCallNode *contentsAppend(VarDclNode *recv, INode *value) {
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, value);
    deref->vtexp = contentsVarUse(recv, value);
    FnCallNode *append = newFnCallOpnameLower(value, (INode*)deref, lessDashName, 2);
    append->flags |= FlagOpAssgn | FlagLvalOp;
    nodesAdd(&append->args, value);
    return append;
}

// '(*recv).name()', a method the receiver's type declares, called with nothing
static FnCallNode *contentsAsk(VarDclNode *recv, Name *name, INode *lexnode) {
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, lexnode);
    deref->vtexp = contentsVarUse(recv, lexnode);
    FnCallNode *call = newFnCallLower(lexnode, (INode*)deref, 1);
    call->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(call->methfld, lexnode);
    return call;
}

// 'if cond {break}', leaving 'loop'. Built after name resolution, so the
// break is joined to its loop here rather than by breakNameRes.
static INode *contentsBreakIf(INode *cond, BlockNode *loop) {
    BreakRetNode *brk = newBreakNode();
    inodeLexCopy((INode*)brk, cond);
    brk->exp = (INode*)newNilLitNode();
    inodeLexCopy(brk->exp, cond);
    brk->block = loop;
    BlockNode *ifblk = newBlockNode();
    inodeLexCopy((INode*)ifblk, cond);
    nodesAdd(&ifblk->stmts, (INode*)brk);
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, cond);
    nodesAdd(&ifnode->condblk, cond);
    nodesAdd(&ifnode->condblk, (INode*)ifblk);
    return (INode*)ifnode;
}

// The literal a count is, through any named constants; NULL when it is not one
static ULitNode *contentsConstCount(INode *count) {
    while (nameUseNames(count, ConstDclTag))
        count = ((ConstDclNode*)((NameUseNode*)count)->dclnode)->value;
    return count->tag == ULitTag ? (ULitNode*)count : NULL;
}

// A constant count is checked as an array's size is: 0 through 4294967295.
// Answers 0 when it reported one out of that range.
static int contentsCountInRange(INode *count) {
    ULitNode *lit = contentsConstCount(count);
    if (lit == NULL)
        return 1;
    if ((lit->flags & FlagLitNeg) && lit->uintlit != 0) {
        errorMsgNode(count, ErrorRepeatCount, "The count before 'of' may not be negative.");
        return 0;
    }
    if (lit->uintlit > UINT32_MAX) {
        errorMsgNode(count, ErrorRepeatCount, "The count before 'of' may be at most 4294967295, as an array's size may.");
        return 0;
    }
    return 1;
}

// Does the type declare a method of this name?
static int contentsHasMethod(INode *colltype, Name *name) {
    return isMethodType(colltype) && iNsTypeFindFnField((INsTypeNode*)colltype, name) != NULL;
}

// Does the type declare a '<-' that takes two values, a key and its value?
static int contentsHasPairAppend(INode *colltype) {
    if (!isMethodType(colltype))
        return 0;
    INode *appends = iNsTypeFindFnField((INsTypeNode*)colltype, lessDashName);
    if (appends == NULL)
        return 0;
    INode **candp = &appends;
    uint32_t ncand = 1;
    if (appends->tag == FnOverloadDclTag) {
        candp = &nodesGet(((FnOverloadDclNode*)appends)->overloads, 0);
        ncand = ((FnOverloadDclNode*)appends)->overloads->used;
    }
    else if (appends->tag != FnDclTag)
        return 0;
    while (ncand--) {
        FnDclNode *cand = (FnDclNode*)*candp++;
        if (cand->vtype->tag == FnSigTag && ((FnSigNode*)cand->vtype)->parms->used == 3)
            return 1;
    }
    return 0;
}

// 'n of x':
//   { imm count usize = n; mut i = 0usize; loop { if i >= count {break}; *recv <- x; i++ } }
// n is evaluated once, x once for each value. The block is a statement of the
// block holding recv, so it opens the scope two past the one lowering it.
static INode *contentsRepeat(TypeCheckState *pstate, VarDclNode *recv, EntryNode *entry) {
    INode *lexnode = (INode*)entry;
    if (!contentsCountInRange(entry->first))
        return NULL;
    uint16_t scope = (uint16_t)(pstate->scope + 2);

    // The count is checked here, where a mismatch can be said in its own
    // terms, and its variable is then taken as checked
    inodeTypeCheck(pstate, &entry->first, (INode*)usizeType);
    if (!isExpNode(entry->first) || iexpGetTypeDcl(entry->first) == errorType)
        return NULL;
    if (!iexpCoerce(&entry->first, (INode*)usizeType)) {
        errorMsgNode(entry->first, ErrorInvType, "The count before 'of' is a usize.");
        return NULL;
    }
    VarDclNode *count = contentsVar(tempName, (INode*)usizeType, immPerm, entry->first, scope, lexnode);
    count->flags |= TypeChecked;
    VarDclNode *index = contentsVar(tempName, (INode*)usizeType, mutPerm,
        (INode*)newULitNodeTC(0, (INode*)usizeType), scope, lexnode);
    inodeLexCopy(index->value, lexnode);

    BlockNode *loop = newLoopBlockNode();
    inodeLexCopy((INode*)loop, lexnode);
    FnCallNode *done = newFnCallOpnameLower(lexnode, contentsVarUse(index, lexnode), geName, 1);
    nodesAdd(&done->args, contentsVarUse(count, lexnode));
    nodesAdd(&loop->stmts, contentsBreakIf((INode*)done, loop));
    nodesAdd(&loop->stmts, (INode*)contentsAppend(recv, entry->val));
    FnCallNode *step = newFnCallOpnameLower(lexnode, contentsVarUse(index, lexnode), incrPostName, 0);
    step->flags |= FlagLvalOp;
    nodesAdd(&loop->stmts, (INode*)step);

    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, lexnode);
    nodesAdd(&blk->stmts, (INode*)count);
    nodesAdd(&blk->stmts, (INode*)index);
    nodesAdd(&blk->stmts, (INode*)loop);
    return (INode*)blk;
}

// 'fill x': loop { if (*recv).len() >= (*recv).capacity() {break}; *recv <- x }
// A collection says how full it is through those two methods
static INode *contentsFill(VarDclNode *recv, INode *colltype, EntryNode *entry) {
    INode *lexnode = (INode*)entry;
    Name *missing = !contentsHasMethod(colltype, lenName) ? lenName
        : !contentsHasMethod(colltype, capacityName) ? capacityName : NULL;
    if (missing) {
        errorMsgNode(lexnode, ErrorFillSize,
            "'fill' appends a value until the collection is full, which it learns from the collection's 'len()' and 'capacity()' methods, and %s has no '%s' method.",
            itypeName(colltype), &missing->namestr);
        return NULL;
    }
    BlockNode *loop = newLoopBlockNode();
    inodeLexCopy((INode*)loop, lexnode);
    FnCallNode *full = newFnCallOpnameLower(lexnode, (INode*)contentsAsk(recv, lenName, lexnode), geName, 1);
    nodesAdd(&full->args, (INode*)contentsAsk(recv, capacityName, lexnode));
    nodesAdd(&loop->stmts, contentsBreakIf((INode*)full, loop));
    nodesAdd(&loop->stmts, (INode*)contentsAppend(recv, entry->val));
    return (INode*)loop;
}

// 'k: v': '*recv <- (k, v)', one application of a '<-' taking both
static INode *contentsPair(VarDclNode *recv, INode *colltype, EntryNode *entry) {
    if (!contentsHasPairAppend(colltype)) {
        errorMsgNode((INode*)entry, ErrorPairAppend,
            "A pair 'k: v' is appended by a '<-' method taking a key and a value, and %s declares none.",
            itypeName(colltype));
        return NULL;
    }
    FnCallNode *append = contentsAppend(recv, entry->first);
    nodesAdd(&append->args, entry->val);
    return (INode*)append;
}

// The yield that is the body's value in the loop of an each entry: the loop is
// the last statement of the block parseEach made, and the yield stands among its
// statements (a filter and the pass variables before it, a range's step after)
static EntryNode *contentsFindYield(BlockNode *outer) {
    if (outer->tag != BlockTag || outer->stmts->used == 0)
        return NULL;
    INode *loop = nodesLast(outer->stmts);
    if (loop->tag != BlockTag)
        return NULL;
    INode **stmtp;
    uint32_t cnt;
    for (nodesFor(((BlockNode*)loop)->stmts, cnt, stmtp)) {
        if ((*stmtp)->tag == YieldEntryTag)
            return (EntryNode*)*stmtp;
    }
    return NULL;
}

// 'each src' and 'each x in src ... yield v': the each's loop is a statement of
// the block holding recv, and its yield appends against that borrow as the
// other entries do; the yield lowers when the loop's body is checked
// (yieldEntryLower), where the pass variables are known. Answers NULL for a loop
// the parser could not build, which it reported.
static INode *contentsEach(VarDclNode *recv, EntryNode *entry) {
    EntryNode *yield = contentsFindYield((BlockNode*)entry->first);
    if (yield == NULL)
        return NULL;
    yield->recv = (INode*)recv;
    return entry->first;
}

// A yield, checked as the last statement of its loop's body: '*recv <- v' (or
// '*recv <- (k, v)' for 'yield k: v'), the one application of the receiver's '<-'
// that every pass makes. A drain ('<- each src') appends a copy of an item that
// comes borrowed, for 'each' gives a borrow of the items of a collection and a
// collection is not appended borrows.
static void yieldEntryLower(TypeCheckState *pstate, INode **nodep) {
    EntryNode *yield = (EntryNode*)*nodep;
    VarDclNode *recv = (VarDclNode*)yield->recv;
    if (recv == NULL || iexpGetTypeDcl((INode*)recv) == errorType || iexpGetTypeDcl((INode*)recv) == unknownType) {
        *nodep = newErrorNode((INode*)yield);
        return;
    }
    INode *recvtype = iexpGetTypeDcl((INode*)recv);
    INode *colltype = recvtype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)recvtype)->vtexp) : recvtype;
    INode *value = yield->val;
    if (yield->drain) {
        inodeTypeCheckAny(pstate, &yield->val);
        value = yield->val;
        INode *itemtype = isExpNode(value) ? iexpGetTypeDcl(value) : errorType;
        if (itemtype == errorType || itemtype == unknownType) {
            *nodep = newErrorNode((INode*)yield);
            return;
        }
        if (itemtype->tag == RefTag && itypeGetTypeDcl(((RefNode*)itemtype)->region) == borrowRef) {
            INode *pointee = itypeGetTypeDcl(((RefNode*)itemtype)->vtexp);
            if (itypeIsMove(pointee)) {
                errorMsgNode((INode*)yield, ErrorDrainItem,
                    "'each' gives a borrow of each item of this source, and %s, a type that moves, cannot be copied out of one: drain a source that hands its items over ('xs <- each other.drain()'), or append a value made from each item ('xs <- each x in other yield make(x)').",
                    itypeName(pointee));
                *nodep = newErrorNode((INode*)yield);
                return;
            }
            StarNode *deref = newStarNode(DerefTag);
            inodeLexCopy((INode*)deref, value);
            deref->vtexp = value;
            value = (INode*)deref;
        }
    }
    FnCallNode *append;
    if (yield->first != NULL) {
        if (!contentsHasPairAppend(colltype)) {
            errorMsgNode((INode*)yield, ErrorPairAppend,
                "A pair 'yield k: v' is appended by a '<-' method taking a key and a value, and %s declares none.",
                itypeName(colltype));
            *nodep = newErrorNode((INode*)yield);
            return;
        }
        append = contentsAppend(recv, yield->first);
        nodesAdd(&append->args, value);
    }
    else
        append = contentsAppend(recv, value);
    *nodep = (INode*)append;
    inodeTypeCheck(pstate, nodep, noCareType);
}

// A name only the lowering writes, for the receiver a list with an each holds
static Name *contentsRecvName() {
    static Name *name = NULL;
    if (name == NULL)
        name = nametblFind("-recv", 5);
    return name;
}

// The entries a '<-' holds, as one list: the elements of a tuple, or its one argument
static INode **contentsEntries(FnCallNode *node, uint32_t *nentries) {
    INode **argp = &nodesGet(node->args, 0);
    if ((*argp)->tag == VTupleTag) {
        Nodes *elems = ((TupleNode*)*argp)->elems;
        *nentries = elems->used;
        return &nodesGet(elems, 0);
    }
    *nentries = 1;
    return argp;
}

// The entries of a '<-' on a value that is not a construction become a block
// that borrows the receiver once and applies each entry against that borrow:
//   { imm recv = &mut xs; *recv <- 1; ... }
// A receiver that is already a reference is held as it is, so its permission,
// not the binding's, is what each application is checked against.
//
// This is lowering, so it belongs to type check: the borrow needs the
// receiver's type. See compiler/c/doc/phases/type-check.md, "Order of resolution".
static void contentsLowerEntries(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // A receiver with no type was reported where it was made
    inodeTypeCheckAny(pstate, &node->objfn);
    if (!isExpNode(node->objfn) || iexpGetTypeDcl(node->objfn) == errorType
        || iexpGetTypeDcl(node->objfn) == unknownType) {
        *((INode**)nodep) = newErrorNode((INode*)node);
        return;
    }
    INode *lval = node->objfn;
    INode *lvaltype = iexpGetTypeDcl(node->objfn);
    if (!(lvaltype->tag == RefTag || lvaltype->tag == VirtRefTag || lvaltype->tag == ArrayRefTag))
        borrowMutRef(&lval, lvaltype, (INode*)mutPerm);
    // The type whose methods the entries are applied through
    INode *colltype = lvaltype->tag == RefTag ? itypeGetTypeDcl(((RefNode*)lvaltype)->vtexp) : lvaltype;

    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, (INode*)node);
    blk->flags |= FlagKeepTemps;
    // An operator's temporary holds no borrow past the statement (flowpath.c), and
    // that is all the other entries need. A loop appends across its passes while
    // it reads its source, and the receiver's borrow must be held through them for
    // 'xs <- each xs' to be refused as 'each x in xs { xs.push(*x); }' is, so the
    // receiver of a list with an each is a variable like any other.
    uint32_t nentries;
    INode **entryp = contentsEntries(node, &nentries);
    int haseach = 0;
    for (uint32_t i = 0; i < nentries; ++i)
        haseach |= entryp[i]->tag == EachEntryTag;

    // An entry holding an 'await' is made before the receiver is borrowed, as is
    // every entry written before it, in the order written, each into a local of
    // the rewrite: the seam ends a borrow, and the receiver is borrowed (and the
    // values appended) only after the last of them. A repeated, filled or looped
    // entry makes its value more than once, past the seam, and is not hoisted
    // (the seam is then reported where it stands, pwSeamVar)
    uint32_t lastawait = 0;     // One past the last entry that holds a seam
    int hoistable = 1;
    for (uint32_t i = 0; i < nentries; ++i) {
        INode *entry = entryp[i];
        if (!awaitWithin(entry))
            continue;
        lastawait = i + 1;
        if (entry->tag == OfEntryTag || entry->tag == FillEntryTag || entry->tag == EachEntryTag)
            hoistable = 0;
    }
    VarDclNode **hoisted = NULL;
    uint32_t nhoisted = 0;
    if (lastawait && hoistable) {
        hoisted = (VarDclNode **)memAllocBlk(lastawait * 2 * sizeof(VarDclNode *));
        for (uint32_t i = 0; i < lastawait; ++i) {
            INode *entry = entryp[i];
            INode **partp[2] = { NULL, NULL };
            if (entry->tag == PairEntryTag) {
                partp[0] = &((EntryNode*)entry)->first;
                partp[1] = &((EntryNode*)entry)->val;
            }
            else if (entry->tag != OfEntryTag && entry->tag != FillEntryTag && entry->tag != EachEntryTag)
                partp[0] = &entryp[i];
            for (int p = 0; p < 2; ++p) {
                if (partp[p] == NULL || litIsLiteral(*partp[p]))
                    continue;
                VarDclNode *part = contentsVar(tempLocalName, unknownType, immPerm, *partp[p],
                    (uint16_t)(pstate->scope + 1), *partp[p]);
                *partp[p] = contentsVarUse(part, (INode*)part);
                hoisted[nhoisted++] = part;
            }
        }
    }
    for (uint32_t i = 0; i < nhoisted; ++i)
        nodesAdd(&blk->stmts, (INode*)hoisted[i]);
    VarDclNode *recv = contentsVar(haseach ? contentsRecvName() : tempName, unknownType, immPerm, lval,
        (uint16_t)(pstate->scope + 1), (INode*)node);
    nodesAdd(&blk->stmts, (INode*)recv);

    int bad = 0;
    while (nentries--) {
        INode *entry = *entryp++;
        INode *stmt;
        switch (entry->tag) {
        case OfEntryTag:
            stmt = contentsRepeat(pstate, recv, (EntryNode*)entry); break;
        case FillEntryTag:
            stmt = contentsFill(recv, colltype, (EntryNode*)entry); break;
        case PairEntryTag:
            stmt = contentsPair(recv, colltype, (EntryNode*)entry); break;
        case EachEntryTag:
            stmt = contentsEach(recv, (EntryNode*)entry); break;
        default:
            stmt = (INode*)contentsAppend(recv, entry); break;
        }
        if (stmt)
            nodesAdd(&blk->stmts, stmt);
        else
            bad = 1;
    }
    *((INode**)nodep) = (INode*)blk;
    inodeTypeCheckAny(pstate, (INode**)nodep);
    if (bad)
        ((IExpNode*)*nodep)->vtype = errorType;
}

// A name the construction's variable alone has, which nothing written can be:
// unlike the operators' temporaries, it holds the value it was given
static Name *contentsBuiltName() {
    static Name *name = NULL;
    if (name == NULL)
        name = nametblFind("-built", 6);
    return name;
}

// A construction's contents are appended to the value it builds, which is
// then the value of the whole:
//   { mut built = new T(...); built <- contents; built }
// The construction fills the variable in place; the block hands it on.
static void contentsLowerConstruction(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, (INode*)node);
    VarDclNode *built = contentsVar(contentsBuiltName(), unknownType, mutPerm, node->objfn,
        (uint16_t)(pstate->scope + 1), node->objfn);
    nodesAdd(&blk->stmts, (INode*)built);
    node->objfn = contentsVarUse(built, node->objfn);
    nodesAdd(&blk->stmts, (INode*)node);
    nodesAdd(&blk->stmts, contentsVarUse(built, (INode*)node));
    *((INode**)nodep) = (INode*)blk;
    inodeTypeCheckAny(pstate, (INode**)nodep);
    // A construction refused leaves the whole without a value, which a block
    // would otherwise take as no type at all rather than as an error
    if (iexpGetTypeDcl((INode*)built) == errorType)
        blk->vtype = errorType;
}

// Type check one of an array's element values against its element type, or,
// for an array written with several sizes, its scalar type
static int contentsArrayElem(TypeCheckState *pstate, INode **valp, INode *elemtype, int scalars) {
    inodeTypeCheck(pstate, valp, elemtype);
    if (!isExpNode(*valp)) {
        errorMsgNode(*valp, ErrorNotTyped, "Expected a typed expression.");
        return 0;
    }
    if (iexpGetTypeDcl(*valp) == errorType)
        return 0;
    if (!iexpCoerce(valp, elemtype)) {
        if (scalars && iexpGetTypeDcl(*valp)->tag == ArrayTag)
            errorMsgNode(*valp, ErrorInvType,
                "An array written with several sizes is filled with its scalars, %s, row by row, not with its rows: write each row's values in turn, or write the type nested, 'Array[Array[T, n], m]', to give rows.",
                itypeName(elemtype));
        else
            errorMsgNode(*valp, ErrorInvType, scalars
                ? "This value's type does not match the array's scalar type, the element type written first."
                : "This value's type does not match the array's element type.");
        return 0;
    }
    return 1;
}

// A copy of a value not yet checked, to be checked as another element
static INode *contentsCopy(TypeCheckState *pstate, INode *val) {
    CloneState cstate = {0};     // Every field the clone reads, the ones not set below NULL
    cstate.instnode = val->instnode;
    cstate.scope = (uint16_t)pstate->scope;
    return cloneNode(&cstate, val);
}

// How many scalars an array's contents give, and their type: for an array
// type written with several sizes, 'Array[u8, 2, 3, 4]', the product of the
// sizes and the type written first, row-major as C's 'u8 a[2][3][4]'; for one
// written with one size, the nested spelling 'Array[Array[u8, 4], 3]'
// included, its size and its element type, so the nested spelling's contents
// are its rows. Answers 0 when it reported why it could not, or when a size
// is not one that arrayTypeCheck accepted (reported there).
static int contentsArrayShape(ArrayNode *arraydcl, INode *lexnode, uint32_t *levels, uint64_t *size, INode **scalartype) {
    *levels = arraydcl->nsizes > 1 ? arraydcl->nsizes : 1;
    *size = 1;
    INode *level = (INode*)arraydcl;
    for (uint32_t i = 0; i < *levels; ++i) {
        INode *dimnode = nodesGet(((ArrayNode*)level)->dimens, 0);
        if (dimnode->tag != ULitTag || ((ULitNode*)dimnode)->uintlit > UINT32_MAX
            || ((dimnode->flags & FlagLitNeg) && ((ULitNode*)dimnode)->uintlit != 0))
            return 0;
        uint64_t dim = ((ULitNode*)dimnode)->uintlit;
        // Each factor is at most 4294967295, so the product is checked before
        // it can overflow 64 bits
        *size = *size > UINT32_MAX ? *size : *size * dim;
        if (dim == 0)
            *size = 0;
        level = arrayElemType(level);
    }
    *scalartype = level;
    if (*levels > 1 && *size > UINT32_MAX) {
        errorMsgNode(lexnode, ErrorArrayContents,
            "An array written with several sizes is filled with its scalars, and %s has more than 4294967295 of them, the most an array's size may be.",
            itypeName((INode*)arraydcl));
        return 0;
    }
    // 'Array[Array[u8, 4], 2, 3]' (or a type parameter or alias standing for
    // such an element) mixes the spellings: whether its contents are the u8s
    // or the 'Array[u8, 4]'s is not settled, so neither is taken
    if (*levels > 1 && itypeGetTypeDcl(level)->tag == ArrayTag) {
        errorMsgNode(lexnode, ErrorArrayContents,
            "An array written with several sizes whose element type is itself an array, %s, takes no contents yet: write it nested, 'Array[Array[T, n], m]', whose contents are its rows.",
            itypeName((INode*)arraydcl));
        return 0;
    }
    return 1;
}

// An array's contents are its elements: 'new Array[f32, 4] <- 1.0, fill 0.0'.
// An array has no '<-' and no 'init', and its size is fixed, so the contents
// must give exactly that many values, every count known at compile time. They
// lower to an array literal of the array's type, which fills the destination
// in place. An entry repeating a value has that value evaluated once for each
// element, so each element is its own copy of the expression, up to
// ArrayRepeatUnroll of them; beyond that, two copies, the second repeated
// (the literal's repeats). One entry repeating a constant is kept as one value
// stored into every element (the literal's fill form), since evaluating a
// constant again gives nothing new.
// An array written with several sizes, 'Array[f32, 2, 3]', is an array of
// arrays whose contents are its scalars, row-major (contentsArrayShape): the
// literal's elements are those scalars, its nsizes the levels they fill, and
// generation stores them into the array as one run of scalars.
// 'arraytype' is the array the contents fill: the construction's type, or the
// value type of the allocation the construction is (contentsLowerAlloc).
// Answers NULL when it reported why it could not.
static INode *contentsArrayLit(TypeCheckState *pstate, FnCallNode *node, INode *arraytype) {
    FnCallNode *ctor = (FnCallNode*)node->objfn;
    ArrayNode *arraydcl = (ArrayNode*)itypeGetTypeDcl(arraytype);
    if (ctor->args && ctor->args->used > 0) {
        errorMsgNode((INode*)ctor, ErrorArrayContents,
            "An array has no init to take arguments: its contents follow '<-', as 'new Array[f32, 4] <- fill 0.0'.");
        return NULL;
    }
    uint32_t levels;
    uint64_t size;
    INode *elemtype;
    if (!contentsArrayShape(arraydcl, (INode*)node, &levels, &size, &elemtype))
        return NULL;

    // How many values each entry gives, the one 'fill' given what is left
    uint32_t nentries;
    INode **entries = contentsEntries(node, &nentries);
    uint64_t given = 0;
    int fills = 0;
    int bad = 0;
    for (uint32_t i = 0; i < nentries; ++i) {
        INode *entry = entries[i];
        switch (entry->tag) {
        case OfEntryTag: {
            INode *count = ((EntryNode*)entry)->first;
            if (!contentsCountInRange(count))
                bad = 1;
            else if (contentsConstCount(count) == NULL) {
                errorMsgNode(count, ErrorRepeatCount,
                    "An array's size is fixed, so the count before 'of' in its contents is a constant: an integer literal, or a named constant holding one.");
                bad = 1;
            }
            else
                given += contentsConstCount(count)->uintlit;
            break;
        }
        case FillEntryTag:
            ++fills;
            break;
        case PairEntryTag:
            errorMsgNode(entry, ErrorPairAppend, "An array's contents are values, not pairs 'k: v'.");
            bad = 1;
            break;
        case EachEntryTag:
            errorMsgNode(entry, ErrorArrayContents,
                "An array's size is fixed, so its contents are a count known at compile time, and an 'each' gives as many values as its source holds: build a List with 'each', or fill the array in a loop.");
            bad = 1;
            break;
        default:
            ++given;
        }
    }
    if (bad)
        return NULL;
    uint64_t filled = fills && given < size ? size - given : 0;
    if (given + filled != size) {
        if (levels > 1)
            errorMsgNode((INode*)node, ErrorArrayContents,
                "These contents give %llu values, and the array holds %llu scalars of %s: an array written with several sizes is filled exactly, with its scalars, row by row.",
                (unsigned long long)(given + filled), (unsigned long long)size, itypeName(elemtype));
        else
            errorMsgNode((INode*)node, ErrorArrayContents,
                "These contents give %llu values, and the array holds %llu: an array's contents fill it exactly.",
                (unsigned long long)(given + filled), (unsigned long long)size);
        return NULL;
    }

    ArrayNode *lit = newArrayNode();
    lit->tag = ArrayLitTag;
    inodeLexCopy((INode*)lit, (INode*)node);
    lit->vtype = arraytype;
    lit->nsizes = levels > 1 ? levels : 0;
    uint32_t *runs = NULL;   // each run's element and how many more it fills
    uint32_t nruns = 0;
    for (uint32_t i = 0; i < nentries; ++i) {
        INode *entry = entries[i];
        INode **valp = &entry;
        uint64_t count = 1;
        if (entry->tag == OfEntryTag) {
            valp = &((EntryNode*)entry)->val;
            count = contentsConstCount(((EntryNode*)entry)->first)->uintlit;
        }
        else if (entry->tag == FillEntryTag) {
            valp = &((EntryNode*)entry)->val;
            count = filled;
            filled = 0;     // a second 'fill' finds the array full
        }
        if (count == 0)
            continue;
        INode *pristine = count > 1 ? contentsCopy(pstate, *valp) : NULL;
        if (!contentsArrayElem(pstate, valp, elemtype, levels > 1)) {
            bad = 1;
            continue;
        }
        if (nentries == 1 && count > 1 && litIsLiteral(*valp)) {
            nodesAdd(&lit->dimens, (INode*)newULitNodeTC(count, (INode*)usizeType));
            inodeLexCopy(nodesGet(lit->dimens, 0), (INode*)node);
            nodesAdd(&lit->elems, *valp);
            break;
        }
        nodesAdd(&lit->elems, *valp);
        // Past ArrayRepeatUnroll copies, the second stands for every element
        // after the first, which generation fills in a loop evaluating it once
        // for each (genlArrayLitInto). A later copy is checked and walked by
        // flow as the second is, so the second asks all a later one would: a
        // variable moved into the first is gone for it, a counted reference
        // gains the holder each pass of the loop adds.
        if (count > ArrayRepeatUnroll) {
            if (contentsArrayElem(pstate, &pristine, elemtype, levels > 1)) {
                if (runs == NULL)
                    runs = (uint32_t *)memAllocBlk(nentries * 2 * sizeof(uint32_t));
                runs[nruns * 2] = lit->elems->used;
                runs[nruns++ * 2 + 1] = (uint32_t)(count - 1);
                nodesAdd(&lit->elems, pristine);
            }
            else
                bad = 1;
            continue;
        }
        for (uint64_t copy = 1; copy < count; ++copy) {
            INode *val = copy == count - 1 ? pristine : contentsCopy(pstate, pristine);
            if (contentsArrayElem(pstate, &val, elemtype, levels > 1))
                nodesAdd(&lit->elems, val);
            else
                bad = 1;
        }
    }
    if (bad)
        return NULL;
    if (nruns) {
        lit->repeats = (uint32_t *)memAllocBlk(lit->elems->used * sizeof(uint32_t));
        for (uint32_t i = 0; i < lit->elems->used; ++i)
            lit->repeats[i] = 1;
        for (uint32_t run = 0; run < nruns; ++run)
            lit->repeats[runs[run * 2]] = runs[run * 2 + 1];
    }
    return (INode*)lit;
}

// An array's construction becomes the array literal of its contents, or, when
// they were refused, an error node, so nothing left unchecked stays in the tree
static void contentsLowerArray(TypeCheckState *pstate, FnCallNode **nodep) {
    INode *lit = contentsArrayLit(pstate, *nodep, ((FnCallNode*)(*nodep)->objfn)->objfn);
    *((INode**)nodep) = lit ? lit : newErrorNode((INode*)*nodep);
}

// An allocation of an array with its contents, 'new Rc[mut, Array[i32, 4]] <-
// fill 0', is the allocation whose value is the literal of those contents,
// which generation fills in place in the region's memory once it is had, an
// element at a time (genlallocref): no array is built elsewhere and copied in.
// 'trynew' fills it only when the memory was had, since the literal is
// generated only there.
static void contentsLowerAlloc(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype) {
    FnCallNode *ctor = (FnCallNode*)(*nodep)->objfn;
    INode *lit = contentsArrayLit(pstate, *nodep, reftype->vtexp);
    INode *alloc = lit ? typeLitNewFilled(pstate, &ctor, reftype, lit) : NULL;
    *((INode**)nodep) = alloc ? alloc : newErrorNode((INode*)*nodep);
}

// The type a construction builds, checked, when its contents may need it
// before the construction itself is: whether it is an array, or an allocation
// of one, decides how the contents lower. NULL, unchecked, for what the
// construction alone can check: a generic struct named without its type
// arguments, which its arguments infer, and what is not a type. errorType
// where the check failed, reported.
static INode *contentsBuiltType(TypeCheckState *pstate, FnCallNode *ctor) {
    if (!isTypeNode(ctor->objfn))
        return NULL;
    if (isNameUseNode(ctor->objfn) && nameUseNames(ctor->objfn, StructTag)
        && genericGetInfo(nameUseGetDcl((NameUseNode*)ctor->objfn)) != NULL)
        return NULL;
    if (!itypeTypeCheck(pstate, &ctor->objfn))
        return errorType;
    return itypeGetTypeDcl(ctor->objfn);
}

// Contents after a 'trynew' that is not an array's allocation are refused:
// they would be appended through the Option it gives, and are built only for
// an array, filled inside its allocation (contentsLowerAlloc). The
// construction is checked first, so one it refuses itself (a 'trynew' of a
// value type, ErrorTryNewValue) is reported once, by it; the whole then
// stands as an error node, so nothing reports the '<-' again.
static void contentsRefuseTryNew(TypeCheckState *pstate, FnCallNode **nodep, INode *built) {
    FnCallNode *node = *nodep;
    inodeTypeCheckAny(pstate, &node->objfn);
    if (isExpNode(node->objfn) && !inodeIsError(node->objfn) && iexpGetTypeDcl(node->objfn) != errorType)
        errorMsgNode((INode*)node, ErrorTryNewContents,
            "Contents after '<-' on a 'trynew' are built only for an array's allocation, and not yet for %s. Allocate it with 'trynew' alone, then append on Some: 'imm maybe = trynew R[perm, T](...); match maybe { case imm s Some[R[perm, T]] { s <- ...; } else {...} }'.",
            built && built->tag == RefTag ? itypeName(((RefNode*)built)->vtexp) : "this value");
    *((INode**)nodep) = newErrorNode((INode*)node);
}

// Lower a '<-' that contentsIsAppend accepts into the appends it stands for,
// and type check them
void contentsLower(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    if (contentsIsConstruction(node->objfn)) {
        FnCallNode *ctor = (FnCallNode*)node->objfn;
        int trynew = (ctor->flags & FlagTryNew) != 0;
        INode *built = contentsBuiltType(pstate, ctor);
        if (built == errorType)
            *((INode**)nodep) = newErrorNode((INode*)node);
        else if (built && built->tag == RefTag && itypeGetTypeDcl(((RefNode*)built)->vtexp)->tag == ArrayTag)
            contentsLowerAlloc(pstate, nodep, (RefNode*)built);
        else if (trynew)
            contentsRefuseTryNew(pstate, nodep, built);
        else if (built && built->tag == ArrayTag)
            contentsLowerArray(pstate, nodep);
        else
            contentsLowerConstruction(pstate, nodep);
        return;
    }
    contentsLowerEntries(pstate, nodep);
}
