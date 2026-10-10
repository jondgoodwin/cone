/** 'each' over something that is not a numeric range, and its lowering
 * @file
 *
 * See each.h for the shape the parser builds and the loops this lowers it to.
 *
 * The lowering is built after name resolution, so every name it uses is bound
 * here (a hidden variable's uses point at its declaration, and a break at its
 * loop), and every variable it declares is given the scope its place in the
 * block tree has: the block this lowers (S), the loop in it (S + 1), the block
 * of a pass variable's initializer (S + 2) and the arm in that (S + 3).
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// A use of a variable the lowering declared, positioned on lexnode
static INode *eachUse(VarDclNode *var, INode *lexnode) {
    NameUseNode *use = newNameUseNode(var->namesym);
    inodeLexCopy((INode*)use, lexnode);
    use->dclnode = (INode*)var;
    return (INode*)use;
}

// A variable the lowering declares: no name the reader can write
static VarDclNode *eachVar(PermNode *perm, INode *val, uint16_t scope, INode *lexnode) {
    VarDclNode *var = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)perm, val);
    inodeLexCopy((INode*)var, lexnode);
    var->scope = scope;
    return var;
}

// 'recv.name()'
static FnCallNode *eachCall(INode *recv, Name *name, INode *lexnode) {
    FnCallNode *call = newFnCallLower(lexnode, recv, 1);
    call->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(call->methfld, lexnode);
    return call;
}

// 'recv.name', a field
static FnCallNode *eachField(INode *recv, Name *name, INode *lexnode) {
    FnCallNode *access = newFnCallLower(lexnode, recv, 0);
    access->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(access->methfld, lexnode);
    return access;
}

// 'recv.k', a tuple's element
static FnCallNode *eachTupleElem(INode *recv, uint64_t k, INode *lexnode) {
    FnCallNode *access = newFnCallLower(lexnode, recv, 0);
    access->methfld = (INode*)newULitNode(k, (INode*)usizeType);
    inodeLexCopy(access->methfld, lexnode);
    return access;
}

// '*ref'
static INode *eachDeref(INode *ref, INode *lexnode) {
    StarNode *deref = newStarNode(DerefTag);
    inodeLexCopy((INode*)deref, lexnode);
    deref->vtexp = ref;
    return (INode*)deref;
}

// 'break', leaving 'loop'
static INode *eachBreak(BlockNode *loop, INode *lexnode) {
    BreakRetNode *brk = newBreakNode();
    inodeLexCopy((INode*)brk, lexnode);
    brk->exp = (INode*)newNilLitNode();
    inodeLexCopy(brk->exp, lexnode);
    brk->block = loop;
    return (INode*)brk;
}

// The block the loop leaves through when it has run out: its 'else', which ends
// in the break that carries its value out, or a block holding just that break
static BlockNode *eachLeave(BlockNode *loop, BlockNode *elseblk, INode *lexnode) {
    if (elseblk != NULL)
        return elseblk;
    BlockNode *leave = newBlockNode();
    inodeLexCopy((INode*)leave, lexnode);
    nodesAdd(&leave->stmts, eachBreak(loop, lexnode));
    return leave;
}

// 'if cond {break}', leaving 'loop'; or, given the loop's 'else', 'if cond {...}'
static INode *eachBreakIf(INode *cond, BlockNode *loop, BlockNode *elseblk, INode *lexnode) {
    BlockNode *ifblk = eachLeave(loop, elseblk, lexnode);
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, lexnode);
    nodesAdd(&ifnode->condblk, cond);
    nodesAdd(&ifnode->condblk, (INode*)ifblk);
    return (INode*)ifnode;
}

// The value of the cursor's next(), taken out of its Option, or a break out of
// the loop when there is none: what 'match' de-sugars to (parseMatch,
// parseBoundMatch), with the matched value held in a variable of the block that
// is the initializer:
//   { imm m = cursor.next();
//     if m is Some { imm s = [Some]m; s.value } elif m is None { break } }
// 'scope' is that block's.
static INode *eachNextItem(INode *cursor, BlockNode *loop, BlockNode *elseblk, uint16_t scope, INode *lexnode) {
    static Name *valueName = NULL;
    if (valueName == NULL)
        valueName = nametblFind("value", 5);
    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, lexnode);
    VarDclNode *matched = eachVar(immPerm, (INode*)eachCall(cursor, nextName, lexnode), scope, lexnode);
    nodesAdd(&blk->stmts, (INode*)matched);

    // The 'is' test and the conversion binding the variable share one type node,
    // whose name is looked up among the matched Option's variants
    NameUseNode *some = newNameUseFromLex(someName, lexnode);
    castPatternMark((INode*)some);
    CastNode *issome = newIsNode(eachUse(matched, lexnode), (INode*)some);
    inodeLexCopy((INode*)issome, lexnode);
    CastNode *cast = newConvCastNode(eachUse(matched, lexnode), (INode*)some);
    inodeLexCopy((INode*)cast, lexnode);
    cast->flags |= FlagMatchBind;
    // The binding has a name of its own (one the reader cannot write), unlike the
    // matched value's variable: a move out of it is marked where it happens, as a
    // named binding's is, and that mark is what tells the matched value's drop flag
    static Name *boundName = NULL;
    if (boundName == NULL)
        boundName = nametblFind("-some", 5);
    VarDclNode *bound = eachVar(immPerm, (INode*)cast, (uint16_t)(scope + 1), lexnode);
    bound->namesym = boundName;
    BlockNode *arm = newBlockNode();
    inodeLexCopy((INode*)arm, lexnode);
    nodesAdd(&arm->stmts, (INode*)bound);
    nodesAdd(&arm->stmts, (INode*)eachField(eachUse(bound, lexnode), valueName, lexnode));

    NameUseNode *none = newNameUseFromLex(noneName, lexnode);
    castPatternMark((INode*)none);
    CastNode *isnone = newIsNode(eachUse(matched, lexnode), (INode*)none);
    inodeLexCopy((INode*)isnone, lexnode);
    BlockNode *leave = eachLeave(loop, elseblk, lexnode);

    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, lexnode);
    nodesAdd(&ifnode->condblk, (INode*)issome);
    nodesAdd(&ifnode->condblk, (INode*)arm);
    nodesAdd(&ifnode->condblk, (INode*)isnone);
    nodesAdd(&ifnode->condblk, (INode*)leave);
    nodesAdd(&blk->stmts, (INode*)ifnode);
    return (INode*)blk;
}

// Is this a place that can be named again and again, evaluating nothing but
// the names on its way? A cursor held in one is advanced where it is.
static int eachStablePlace(INode *node) {
    if (isNameUseNode(node))
        return nameUseNames(node, VarDclTag);
    switch (node->tag) {
    case FldAccessTag:
        return eachStablePlace(((FnCallNode*)node)->objfn);
    case DerefTag:
        return eachStablePlace(((StarNode*)node)->vtexp);
    default:
        return 0;
    }
}

// Is this a place that checking a second time leaves as it is? A checked field
// or element is complete, and a name or a dereference of one is checked again
// without being lowered again. Any other expression (a call, say) is lowered by
// its check, so the lowering that follows the source's must use the node the
// check left once, in the variable that holds it.
static int eachRecheckable(INode *node) {
    if (!iexpIsLval(node))
        return 0;
    if (isNameUseNode(node))
        return nameUseNames(node, VarDclTag);
    switch (node->tag) {
    case FldAccessTag:
    case ArrIndexTag:
        return 1;
    case DerefTag:
        return eachRecheckable(((StarNode*)node)->vtexp);
    default:
        return 0;
    }
}

// Is this type an instance of the struct of this name that core declares?
static int eachIsCore(INode *type, char *name, size_t len) {
    if (type->tag != StructTag || ((StructNode*)type)->namesym != nametblFind(name, (uint32_t)len))
        return 0;
    ModuleNode *mod = dclInfoGetModule(type);
    return mod != NULL && mod->namesym == nametblFind("core", 4);
}

// Does the type declare a method of this name?
static int eachHasMethod(INode *type, Name *name) {
    return isMethodType(type) && iNsTypeFindFnField((INsTypeNode*)type, name) != NULL;
}

// The one method of this name a type declares, or NULL for none or several
static FnDclNode *eachMethod(INode *type, Name *name) {
    if (!isMethodType(type))
        return NULL;
    INode *fn = iNsTypeFindFnField((INsTypeNode*)type, name);
    return fn != NULL && fn->tag == FnDclTag ? (FnDclNode*)fn : NULL;
}

// The type a method answers, its declaration, or NULL where the signature is
// not settled enough to say
static INode *eachAnswer(FnDclNode *fn) {
    if (fn->vtype == NULL || fn->vtype->tag != FnSigTag)
        return NULL;
    INode *ret = ((FnSigNode*)fn->vtype)->rettype;
    if (ret == NULL || ret->tag == FnCallTag || ret == unknownType)
        return NULL;
    return itypeGetTypeDcl(ret);
}

// The variant of this name among an enum's variants, or NULL
static INode *eachVariant(INode *enumdcl, Name *name) {
    if (enumdcl->tag != StructTag || !(enumdcl->flags & EnumType) || ((StructNode*)enumdcl)->derived == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((StructNode*)enumdcl)->derived, cnt, nodesp)) {
        if (((StructNode*)*nodesp)->namesym == name)
            return *nodesp;
    }
    return NULL;
}

// What the cursor's 'next' gives, and whether the loop can take it: the payload
// of the Option it answers, which 'nvars' variables are to hold, one as it is
// and several as the elements of a tuple. Answers 0 having said why not. Sets
// *itemp to the payload's type where it could be read, and leaves it NULL where
// the signature is not settled enough to say (the loop is then built, and what
// is wrong shows where it is checked).
static int eachCheckItem(INode *cursortype, uint32_t nvars, INode *lexnode, INode *varnode, INode **itemp) {
    static Name *valueName = NULL;
    if (valueName == NULL)
        valueName = nametblFind("value", 5);
    *itemp = NULL;
    FnDclNode *nextfn = eachMethod(cursortype, nextName);
    INode *answer = nextfn ? eachAnswer(nextfn) : NULL;
    if (answer == NULL)
        return 1;
    INode *some = eachVariant(answer, someName);
    if (some == NULL || eachVariant(answer, noneName) == NULL) {
        errorMsgNode(lexnode, ErrorEachItem,
            "A cursor's 'next' answers an Option, 'Some' item or 'None' when there are no more, and the 'next' of %s answers %s.",
            itypeName(cursortype), itypeName(answer));
        return 0;
    }
    INode *field = isMethodType(some) ? iNsTypeFindFnField((INsTypeNode*)some, valueName) : NULL;
    if (field == NULL || field->tag != FieldDclTag)
        return 1;
    INode *item = itypeGetTypeDcl(((FieldDclNode*)field)->vtype);
    if (item == NULL || item == unknownType || item == errorType || item->tag == FnCallTag)
        return 1;
    *itemp = item;
    if (nvars > 1 && (item->tag != TTupleTag || ((TupleNode*)item)->elems->used != nvars)) {
        errorMsgNode(varnode, ErrorEachItem,
            "With %d variables each item must be a tuple of %d, and the items %s gives are %s.",
            (int)nvars, (int)nvars, itypeName(cursortype), itypeName(item));
        return 0;
    }
    if (nvars > 1 && itypeIsMove(item)) {
        errorMsgNode(lexnode, ErrorEachItem,
            "The items of %s are %s, a type that moves, and several 'each' variables cannot take its elements out one by one. Use one variable to take the item whole.",
            itypeName(cursortype), itypeName(item));
        return 0;
    }
    return 1;
}

// How many variables the reader declared: the loop's leading declarations that
// have neither a value nor a type, which a declaration in the body cannot be
static uint32_t eachVarCount(BlockNode *loop) {
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

// The pass's variables take the pass's item: one variable is the item, and
// several unpack a tuple item through a variable of the pass's own.
// 'item' builds the item's expression. 'scope' is the loop's.
static void eachBindVars(BlockNode *loop, uint32_t nvars, INode *item, uint16_t scope, INode *lexnode) {
    if (nvars == 1) {
        ((VarDclNode*)nodesGet(loop->stmts, 0))->value = item;
        return;
    }
    VarDclNode *whole = eachVar(immPerm, item, scope, lexnode);
    nodesInsert(&loop->stmts, (INode*)whole, 0);
    for (uint32_t k = 0; k < nvars; ++k) {
        VarDclNode *var = (VarDclNode*)nodesGet(loop->stmts, k + 1);
        var->value = (INode*)eachTupleElem(eachUse(whole, lexnode), k, (INode*)var);
    }
}

// The loop and the statements that make ready its source, once type check knows
// what the source is. The source has been checked as the initializer of its
// hidden variable (the first statement), which is how every other expression
// is: it is lowered there once, and the node it left is the one to use. Where
// the source is a place, the variable is dropped and the place used in its
// stead; where it is a value, the variable holds it for the loop.
void eachLower(TypeCheckState *pstate, BlockNode *outer) {
    outer->flags &= 0xFFFF - FlagEach;
    VarDclNode *srcdcl = (VarDclNode*)nodesGet(outer->stmts, 0);
    BlockNode *loop = (BlockNode*)nodesGet(outer->stmts, 1);
    INode *lexnode = (INode*)srcdcl;
    uint16_t scope = (uint16_t)pstate->scope;

    // The loop's 'else', when it has one, stands ahead of the reader's variables
    // (so that it cannot name them) as the block the loop leaves through; taken
    // out of the loop here to be put where the loop is left
    BlockNode *elseblk = NULL;
    INode *first = loop->stmts->used > 0 ? nodesGet(loop->stmts, 0) : NULL;
    if (first != NULL && first->tag == BlockTag && (first->flags & FlagLoopElse)) {
        elseblk = (BlockNode*)first;
        for (uint32_t i = 1; i < loop->stmts->used; ++i)
            nodesGet(loop->stmts, i - 1) = nodesGet(loop->stmts, i);
        --loop->stmts->used;
    }
    uint32_t nvars = eachVarCount(loop);

    // A source that has no value or no type ends the loop here: what the body
    // says of its variables could only repeat the one report
    INode *src = srcdcl->value;
    outer->stmts->used = 0;
    // (A source that is no expression, a type's name, was refused with the
    // variable that would hold it)
    if (src == NULL || inodeIsError(src) || !isExpNode(src))
        return;
    INode *type = iexpGetTypeDcl(src);
    if (type == errorType || type == unknownType)
        return;

    // What kind of source: the type itself, or what a reference to it points at
    int isref = type->tag == RefTag || type->tag == VirtRefTag;
    INode *base = isref ? itypeGetTypeDcl(((RefNode*)type)->vtexp) : type;
    int isslice = type->tag == ArrayRefTag;
    int isarray = base->tag == ArrayTag;
    int hasnext = !isslice && !isarray && eachHasMethod(base, nextName);
    // A type that lends the array it holds (a list) is walked as that array, in
    // the counted loop of any slice: the cursor its 'iter' gives is for code that
    // is generic over cursors, and would walk it through an Option each pass
    Name *lentvia = NULL;
    if (!isslice && !isarray && !hasnext && base->tag == StructTag) {
        StructNode *lentbody = structLentBody((StructNode*)base);
        if (lentbody != NULL && itypeIsArrayBody((INode*)lentbody))
            lentvia = structLentVia((StructNode*)base);
    }
    int islent = lentvia != NULL;
    int hasiter = !isslice && !isarray && !hasnext && !islent && eachHasMethod(base, iterName);
    if (!isslice && !isarray && !islent && !hasnext && !hasiter) {
        errorMsgNode(src, ErrorNotIterable,
            "'each' walks a numeric range ('each i in 0 < n'), an array or a slice, something with a 'next' method, or something with an 'iter' method that gives one, and %s is none of those.",
            itypeName(type));
        return;
    }
    if ((isslice || isarray || islent) && nvars != 1) {
        errorMsgNode(nodesGet(loop->stmts, 0), ErrorNotIterable,
            "An array, a slice or a list gives one variable, a borrow of each element.");
        return;
    }
    int place = eachRecheckable(src);

    // The cursor a slice gives (core's ArrayIter, ArrayIndexed, and ArrayMutItems,
    // MutItemsIndexed which lend '&mut'), made for this loop, is the slice and a count
    // it starts from: walked as the slice is, by a counted loop, rather than through
    // its Option each pass. What is lent is what the cursor's own 'next' lends: '&'
    // for the first two, '&mut' for the last two
    int itemskind = 0;
    if (hasnext && !isref && !place) {
        if (nvars == 1 && eachIsCore(base, "ArrayIter", 9))
            itemskind = 1;
        else if (nvars == 2 && eachIsCore(base, "ArrayIndexed", 12))
            itemskind = 2;
        else if (nvars == 1 && eachIsCore(base, "ArrayMutItems", 13))
            itemskind = 3;
        else if (nvars == 2 && eachIsCore(base, "MutItemsIndexed", 15))
            itemskind = 4;
    }

    // The cursor the loop walks, and what it gives, are checked before the loop
    // is built: what is wrong with either is said here, in the loop's terms,
    // rather than by the statements built from it
    if ((hasnext || hasiter) && !itemskind) {
        INode *cursortype = base;
        if (hasiter) {
            FnDclNode *iterfn = eachMethod(base, iterName);
            INode *given = iterfn ? eachAnswer(iterfn) : NULL;
            if (given != NULL && (given->tag == RefTag || given->tag == VirtRefTag))
                given = itypeGetTypeDcl(((RefNode*)given)->vtexp);
            if (given != NULL && !eachHasMethod(given, nextName)) {
                errorMsgNode(src, ErrorEachItem,
                    "The 'iter' of %s gives %s, which has no 'next' method to take the items from.",
                    itypeName(base), itypeName(given));
                return;
            }
            cursortype = given;
        }
        INode *item;
        if (cursortype != NULL && !eachCheckItem(cursortype, nvars, src, nodesGet(loop->stmts, 0), &item))
            return;
    }

    if (isslice || isarray || islent || itemskind) {
        // imm s = [the slice]; mut i = 0; loop { if i >= s.len {break}; imm x = &s[i]; i++; ... }
        VarDclNode *slicedcl = srcdcl;
        INode *start = (INode*)newULitNodeTC(0, (INode*)usizeType);
        if (itemskind) {
            // The cursor's own slice and position, read once
            static Name *itemsName = NULL, *posName = NULL;
            if (itemsName == NULL) {
                itemsName = nametblFind("items", 5);
                posName = nametblFind("pos", 3);
            }
            nodesAdd(&outer->stmts, (INode*)srcdcl);
            slicedcl = eachVar(immPerm, (INode*)eachField(eachUse(srcdcl, lexnode), itemsName, lexnode), scope, lexnode);
            start = (INode*)eachField(eachUse(srcdcl, lexnode), posName, lexnode);
        }
        else if (islent) {
            // The slice the type lends: its place's, or a value held first
            INode *owner = src;
            if (!place) {
                nodesAdd(&outer->stmts, (INode*)srcdcl);
                owner = eachUse(srcdcl, lexnode);
            }
            slicedcl = eachVar(immPerm, (INode*)eachCall(owner, lentvia, lexnode), scope, lexnode);
        }
        else if (isarray) {
            // The array's place, lent as the slice of it; an array that is no place is held first
            INode *array = src;
            if (!place) {
                nodesAdd(&outer->stmts, (INode*)srcdcl);
                array = eachUse(srcdcl, lexnode);
            }
            if (isref)
                array = eachDeref(array, lexnode);
            slicedcl = eachVar(immPerm, NULL, scope, lexnode);
            slicedcl->value = (INode*)newRefNodeFull(ArrayBorrowTag, lexnode, borrowRef, unknownType, array);
        }
        nodesAdd(&outer->stmts, (INode*)slicedcl);
        VarDclNode *index = eachVar(mutPerm, start, scope, lexnode);
        nodesAdd(&outer->stmts, (INode*)index);

        FnCallNode *done = newFnCallOpnameLower(lexnode, eachUse(index, lexnode), geName, 1);
        nodesAdd(&done->args, (INode*)eachField(eachUse(slicedcl, lexnode), lenName, lexnode));
        FnCallNode *elem = newFnCallLower(lexnode, eachUse(slicedcl, lexnode), 1);
        elem->flags |= FlagIndex;
        nodesAdd(&elem->args, eachUse(index, lexnode));
        // A slice that is mutable (what 'mutItems' gives) lends each element
        // mutably; any other lends it to be read
        INode *elemperm = unknownType;
        if ((isslice && permMatches((INode*)mutPerm, ((RefNode*)type)->perm)) || itemskind >= 3)
            elemperm = (INode*)mutPerm;
        RefNode *borrow = newRefNodeFull(BorrowTag, lexnode, borrowRef, elemperm, (INode*)elem);
        if (itemskind == 2 || itemskind == 4) {
            // The position is a copy of the count, the second variable the element's borrow
            ((VarDclNode*)nodesGet(loop->stmts, 0))->value = eachUse(index, lexnode);
            ((VarDclNode*)nodesGet(loop->stmts, 1))->value = (INode*)borrow;
        }
        else
            ((VarDclNode*)nodesGet(loop->stmts, 0))->value = (INode*)borrow;
        FnCallNode *step = newFnCallOpnameLower(lexnode, eachUse(index, lexnode), incrPostName, 0);
        step->flags |= FlagLvalOp;
        nodesInsert(&loop->stmts, (INode*)step, nvars);
        nodesInsert(&loop->stmts, eachBreakIf((INode*)done, loop, elseblk, lexnode), 0);
        nodesAdd(&outer->stmts, (INode*)loop);
        return;
    }

    // The cursor: what 'next' is called on
    INode *cursor;
    if (hasnext) {
        if (place && eachStablePlace(src))
            cursor = src;       // advanced where it is
        else if (place) {
            // A place that is not named again without evaluating something: borrowed once
            VarDclNode *lent = eachVar(immPerm, src, scope, lexnode);
            borrowMutRef(&lent->value, type, (INode*)mutPerm);
            nodesAdd(&outer->stmts, (INode*)lent);
            cursor = eachUse(lent, lexnode);
        }
        else {
            nodesAdd(&outer->stmts, (INode*)srcdcl);    // a value: held for the loop
            cursor = eachUse(srcdcl, lexnode);
        }
    }
    else {
        INode *owner = src;
        if (!place) {
            nodesAdd(&outer->stmts, (INode*)srcdcl);
            owner = eachUse(srcdcl, lexnode);
        }
        VarDclNode *cursordcl = eachVar(mutPerm, (INode*)eachCall(owner, iterName, lexnode), scope, lexnode);
        nodesAdd(&outer->stmts, (INode*)cursordcl);
        cursor = eachUse(cursordcl, lexnode);
    }
    eachBindVars(loop, nvars, eachNextItem(cursor, loop, elseblk, (uint16_t)(scope + 2), lexnode),
        (uint16_t)(scope + 1), lexnode);
    nodesAdd(&outer->stmts, (INode*)loop);
}

Name *eachRangeStepName = NULL;

void eachRangeNames() {
    if (eachRangeStepName == NULL)
        eachRangeStepName = nametblPrivate("rangestep'", 10);
}

// The step of a numeric range, which the parser could not build before the type of
// its counter was known (see each.h). The call is on the counter, with the range's
// first value and the count of steps so far, and the step after them when one was
// written. A float counter becomes 'x = first + n * step' ('first + n' with no step):
// computed from the first value, so a step of 0.1 taken ten times lands on 1.0
// and does not stop at the 0.9999999999999999 that adding it up reaches. Any other
// counter is stepped by the operator, 'x += step' or 'x++', which wraps at the
// type's extreme where the loop's own guard sees it (parseEachLoop). Returns 1 when
// the node has been replaced and checked, 0 when it is now that operator, to be checked
// as any other.
int eachRangeStepLower(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *node = *nodep;
    Nodes *args = node->args;
    INode *step = args->used > 4 ? nodesGet(args, 4) : NULL;

    // The counter's declaration is an earlier statement of the block, checked already
    INode *counter = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->dclnode : NULL;
    INode *type = NULL;
    if (counter != NULL && counter->tag == VarDclTag)
        type = itypeGetTypeDcl(((VarDclNode*)counter)->vtype);
    if (type != NULL && type->tag == FloatNbrTag) {
        INode *lexnode = (INode*)node;
        // The count as the counter's float type, 'f64.from(n)': the parser wrote the
        // conversion to each of the two, name resolved like any the reader writes
        INode *count = nodesGet(args, ((NbrNode*)type)->bits == 32 ? 2 : 3);
        if (step != NULL) {
            FnCallNode *scaled = newFnCallOpnameLower(lexnode, count, multName, 1);
            nodesAdd(&scaled->args, step);
            count = (INode*)scaled;
        }
        FnCallNode *sum = newFnCallOpnameLower(lexnode, nodesGet(args, 0), plusName, 1);
        nodesAdd(&sum->args, count);
        // (the target is a use of the counter under the counter's own name: an assignment
        // to the anonymous name discards its value)
        AssignNode *set = newAssignNode(NormalAssign, eachUse((VarDclNode*)counter, lexnode), (INode*)sum);
        inodeLexCopy((INode*)set, lexnode);
        *((INode**)nodep) = (INode*)set;
        inodeTypeCheckAny(pstate, (INode**)nodep);
        return 1;
    }

    node->methfld = (INode*)newMemberUseNode(step != NULL ? plusEqName : incrPostName);
    inodeLexCopy(node->methfld, (INode*)node);
    if (step != NULL) {
        node->args = newNodes(1);
        nodesAdd(&node->args, step);
    }
    else
        node->args = NULL;
    return 0;
}
