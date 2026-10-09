/** Handling for variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Create a new name declaraction node
VarDclNode *newVarDclNode(Name *namesym, uint16_t tag, INode *perm) {
    VarDclNode *name;
    newNode(name, VarDclNode, tag);
    name->vtype = unknownType;
    name->namesym = namesym;
    name->perm = perm;
    name->value = NULL;
    name->scope = 0;
    name->index = 0;
    name->llvmvar = NULL;
    name->fold = NULL;
    dclInfoInit(&name->dclinfo);
    name->flowtempflags = 0;
    name->flowindex = 0;
    name->flowdepth = 0;
    name->flowtracked = 0;
    name->flowlend = 0;
    name->hollowed = NULL;
    name->hollowall = NULL;
    name->llvmflag = NULL;
    return name;
}

// Create a new name declaraction node
VarDclNode *newVarDclFull(Name *namesym, uint16_t tag, INode *type, INode *perm, INode *val) {
    VarDclNode *name;
    newNode(name, VarDclNode, tag);
    name->vtype = type;
    name->namesym = namesym;
    name->perm = perm;
    name->value = val;
    name->scope = 0;
    name->index = 0;
    name->llvmvar = NULL;
    name->fold = NULL;
    dclInfoInit(&name->dclinfo);
    name->flowtempflags = 0;
    name->flowindex = 0;
    name->flowdepth = 0;
    name->flowtracked = 0;
    name->flowlend = 0;
    name->hollowed = NULL;
    name->hollowall = NULL;
    name->llvmflag = NULL;
    return name;
}

// The copy of a variable declaration, before its type and value are copied: they
// still point at the original's. Split from filling it in so a generic type's
// clone can bind a static's copy before any body that may name it is copied
// (cloneStructNode).
VarDclNode *cloneVarDclShell(VarDclNode *node) {
    VarDclNode *newnode = memAllocBlk(sizeof(VarDclNode));
    memcpy(newnode, node, sizeof(VarDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    // What flow finds of the copy is the copy's own (VarLendSeen)
    newnode->flowlend = 0;
    return newnode;
}

// Copy the original's type and value into its shell, and re-point every later
// use of the original at the copy
void cloneVarDclFill(CloneState *cstate, VarDclNode *newnode, VarDclNode *node) {
    newnode->vtype = cloneNode(cstate, node->vtype);
    newnode->value = cloneNode(cstate, node->value);
    cloneDclSetMap((INode*)node, (INode*)newnode);
}

// Create a new variable dcl node that is a copy of an existing one
INode *cloneVarDclNode(CloneState *cstate, VarDclNode *node) {
    VarDclNode *newnode = cloneVarDclShell(node);
    cloneVarDclFill(cstate, newnode, node);
    return (INode*)newnode;
}

// Serialize a variable node
void varDclPrint(VarDclNode *name) {
    if (name->flags & FlagStatic)
        inodeFprint("static ");
    inodePrintNode((INode*)name->perm);
    inodeFprint(" %s", &name->namesym->namestr);
    dclInfoPrint((INode*)name);
    inodeFprint(" ");
    inodePrintNode(name->vtype);
    if (name->value) {
        inodeFprint(" = ");
        if (name->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(name->value);
    }
}

// Enable name resolution of local variables
void varDclNameRes(NameResState *pstate, VarDclNode *name) {
    inodeNameRes(pstate, (INode**)&name->perm);
    if (name->vtype)
        inodeNameRes(pstate, &name->vtype);

    // Name resolve value before hooking the variable name (so it cannot point to itself)
    if (name->value)
        inodeNameRes(pstate, &name->value);

    // Variable declaration within a block is a local variable
    if (pstate->scope > 0) {
        if (name->namesym->node && pstate->scope == ((VarDclNode*)name->namesym->node)->scope) {
            errorMsgNode((INode *)name, ErrorDupName, "Name is already defined. Only one allowed.");
            errorMsgNode((INode*)name->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
        else {
            name->scope = pstate->scope;
            // Add name to global name table (containing block will unhook it later)
            nametblHookNode(name->namesym, (INode*)name);
        }
    }
}

// *********************
// Temporaries an initializer extends
//
// A temporary dies at the end of the statement that made it (flow.c,
// "Temporaries"), except where Rust's rule extends it to the end of the
// enclosing block: what a borrow in an extending position of a local's
// initializer borrows ('imm r = &make();', '&make().x', '&*makeOwner()'), and
// the owner such an initializer lends where a borrow is wanted ('imm r &R =
// makeOwner();', read as Rust's '&*'). The initializer is an extending
// position, and so, recursively, are the operand of an extending borrow or
// recast and each element of an extending tuple, array or variant literal
// ('Some[..]'), and the final expression of a block in one, and so of an
// 'if''s or a 'match''s arm, as Rust 2024 extends them; a place an extending
// borrow reaches extends the value it is part of, or that its owner is
// reached through. A call's arguments, a method's receiver, a block's other
// statements and a construction's arguments ('new H(..)') are not extending:
// a temporary there ends with its statement, and a borrow of it held past
// that is refused (flowpath.c).
//
// An extended temporary becomes a hidden local of the block, declared just
// before the statement and initialized with the temporary's expression, so it
// has all a local has: its end at the block's end, its drop flag, the loans
// rooted in it, the lifetime a borrow of it has. One extended from a block's
// final expression is declared there holding nothing, and given its value
// where the temporary ran, just before that final expression: on the paths
// through that arm only, which its drop flag follows. A borrow of a place rooted in
// a temporary is made a borrow of such a local as it is type checked
// (varDclExtendTemp, from borrowTypeCheck), before anything shows whether it
// is extending; the walk here keeps it if it is, and the statement's end puts
// it back where it was if not ('id(&make())', 'id(&*makeOwner())'): a
// temporary of the statement, which a borrow may point at until its end.
//
// A hidden local runs where its declaration is, before the statement: so an
// element of a literal that runs before an extended temporary, and is not a
// constant, becomes a hidden local too, ahead of it, keeping the order the
// statement runs in.
// *********************

void varDclExtendBegin(TypeCheckState *pstate, VarDclExtend *ext, VarDclNode *var) {
    memset(ext, 0, sizeof(VarDclExtend));
    ext->outer = pstate->extend;
    ext->var = var;
    pstate->extend = ext;
}

// The temporary at 'slot' made a hidden local of the block: its declaration,
// which takes the expression, and a name use of it in its place. Nothing but
// the borrow reaches it, so it is 'uni', and lends whatever the borrow asks
// ('&mut make()', '&uni make()').
static VarDclNode *varDclHide(VarDclExtend *ext, INode **slot) {
    INode *exp = *slot;
    VarDclNode *var = newVarDclFull(tempLocalName, VarDclTag, ((IExpNode *)exp)->vtype,
        (INode *)uniPerm, exp);
    inodeLexCopy((INode *)var, exp);
    var->scope = ext->var->scope;
    // Its value is checked already, and a second check would lower it again
    var->flags |= TypeChecked;
    *slot = newNameUseFromDclNode((INode *)var, exp);
    return var;
}

int varDclExtendTemp(TypeCheckState *pstate, INode **slot) {
    VarDclExtend *ext = pstate->extend;
    if (ext == NULL)
        return 0;
    if (ext->ntemps == ext->tempcap) {
        uint32_t cap = ext->tempcap ? ext->tempcap << 1 : 4;
        VarDclTemp *temps = (VarDclTemp *)memAllocBlk(cap * sizeof(VarDclTemp));
        if (ext->ntemps)
            memcpy(temps, ext->temps, ext->ntemps * sizeof(VarDclTemp));
        ext->temps = temps;
        ext->tempcap = cap;
    }
    VarDclTemp *temp = &ext->temps[ext->ntemps++];
    temp->slot = slot;
    temp->kept = 0;
    temp->var = varDclHide(ext, slot);
    return 1;
}

// The hidden local 'node' names, made by a borrow in this statement, or NULL
static VarDclTemp *varDclTempOf(VarDclExtend *ext, INode *node) {
    if (!isNameUseNode(node))
        return NULL;
    INode *dcl = ((NameUseNode *)node)->dclnode;
    for (uint32_t i = 0; i < ext->ntemps; ++i) {
        if ((INode *)ext->temps[i].var == dcl)
            return &ext->temps[i];
    }
    return NULL;
}

// An element of a literal has run, and is still in place: if a hidden local
// is declared after it, it becomes one too
static void varDclExtendPend(VarDclExtend *ext, INode **elemp) {
    if (litIsLiteral(*elemp))
        return;
    if (ext->npending == ext->pendcap) {
        uint32_t cap = ext->pendcap ? ext->pendcap << 1 : 4;
        INode ***pending = (INode ***)memAllocBlk(cap * sizeof(INode **));
        if (ext->npending)
            memcpy(pending, ext->pending, ext->npending * sizeof(INode **));
        ext->pending = pending;
        ext->pendcap = cap;
    }
    ext->pending[ext->npending++] = elemp;
}

// A hidden local is declared: after what ran before it. Within a block's
// final expression, which runs on some paths only, or after the block's other
// statements, it is declared before the statement holding nothing, and given
// its value where the temporary ran: just before that final expression, after
// what ran there before it. Where a path does not give it one, its drop flag
// says so, as for any local given its value on some paths.
static void varDclExtendEmit(VarDclExtend *ext, VarDclNode *var) {
    if (ext->hoisted == NULL)
        ext->hoisted = newNodes(4);
    Nodes **runs = &ext->hoisted;
    if (ext->inblock) {
        if (ext->tail == NULL)
            ext->tail = newNodes(4);
        runs = &ext->tail;
    }
    if (ext->npending) {
        for (uint32_t i = 0; i < ext->npending; ++i)
            nodesAdd(runs, (INode *)varDclHide(ext, ext->pending[i]));
        ext->ran += ext->npending;
        ext->npending = 0;
        ++ext->flushes;
    }
    ++ext->ran;
    if (!ext->inblock) {
        nodesAdd(&ext->hoisted, (INode *)var);
        return;
    }
    INode *value = var->value;
    var->value = NULL;
    nodesAdd(&ext->hoisted, (INode *)var);
    AssignNode *init = newAssignNode(NormalAssign, newNameUseFromDclNode((INode *)var, value), value);
    inodeLexCopy((INode *)init, value);
    init->vtype = ((IExpNode *)value)->vtype;
    nodesAdd(&ext->tail, (INode *)init);
}

// The expression a hidden local was made of, when 'node' names one in this
// statement; else 'node'. For a use of the temporary that takes its value as it
// stands, in place of a borrow of the local: the hidden local is then never
// declared, as for any the statement's end puts back.
INode *varDclTempValue(TypeCheckState *pstate, INode *node) {
    VarDclExtend *ext = pstate->extend;
    VarDclTemp *temp = ext ? varDclTempOf(ext, node) : NULL;
    return temp ? temp->var->value : node;
}

static void varDclExtendExp(VarDclExtend *ext, INode **nodep);

// A hidden local extended: first what its own value extends. What its value
// ran after the last of those stays in it, run as it is declared.
static void varDclExtendKeep(VarDclExtend *ext, VarDclNode *var) {
    uint32_t mark = ext->npending;
    uint32_t flushes = ext->flushes;
    varDclExtendExp(ext, &var->value);
    ext->npending = ext->flushes == flushes ? mark : 0;
    varDclExtendEmit(ext, var);
}

static void varDclExtendArgs(VarDclExtend *ext, Nodes *args) {
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argsp))
        varDclExtendPend(ext, argsp);
}

// The place an extending borrow reaches, or part of one: the temporary it is
// rooted in is extended
static void varDclExtendPlace(VarDclExtend *ext, INode **nodep) {
    INode *node = *nodep;
    VarDclTemp *temp = varDclTempOf(ext, node);
    if (temp) {
        if (!temp->kept) {
            temp->kept = 1;
            varDclExtendKeep(ext, temp->var);
        }
        return;
    }
    switch (node->tag) {
    case FldAccessTag:
        varDclExtendPlace(ext, &((FnCallNode *)node)->objfn);
        return;
    case ArrIndexTag:
        varDclExtendPlace(ext, &((FnCallNode *)node)->objfn);
        varDclExtendArgs(ext, ((FnCallNode *)node)->args);
        return;
    case DerefTag:
        varDclExtendPlace(ext, &((StarNode *)node)->vtexp);
        return;
    case CastTag:
        if (!(node->flags & FlagConvert))
            varDclExtendPlace(ext, &((CastNode *)node)->exp);
        return;
    // Read through: '&*&make()'
    case BorrowTag:
    case ArrayBorrowTag:
        varDclExtendExp(ext, nodep);
        return;
    default:
        return;
    }
}

// Does this recast lend an owning reference as a borrowed one ('imm r &R =
// owner'), a borrow of what it owns?
static int varDclLendsOwner(CastNode *cast) {
    if (cast->flags & FlagConvert)
        return 0;
    RefNode *to = (RefNode *)iexpGetTypeDcl((INode *)cast);
    RefNode *from = (RefNode *)iexpGetTypeDcl(cast->exp);
    return (to->tag == RefTag || to->tag == VirtRefTag) && from->tag == to->tag
        && itypeGetTypeDcl(to->region) == borrowRef && itypeGetTypeDcl(from->region) != borrowRef;
}

// An extending position of the initializer
static void varDclExtendExp(VarDclExtend *ext, INode **nodep) {
    INode *node = *nodep;
    switch (node->tag) {
    case BorrowTag:
    case ArrayBorrowTag:
        varDclExtendPlace(ext, &((RefNode *)node)->vtexp);
        return;
    // '&v[i]', checked as an index through the borrow '&v' (borrowReassocIndex)
    case ArrIndexTag:
        if (node->flags & FlagBorrow) {
            varDclExtendExp(ext, &((FnCallNode *)node)->objfn);
            varDclExtendArgs(ext, ((FnCallNode *)node)->args);
        }
        return;
    case CastTag:
    {
        CastNode *cast = (CastNode *)node;
        if (varDclLendsOwner(cast)) {
            INode **temp = borrowTempRoot(&cast->exp);
            if (temp) {
                varDclExtendKeep(ext, varDclHide(ext, temp));
                // A borrow of a local now: the lifetime is the block's
                cast->vtype = iexpCoerceType(cast->exp, iexpGetTypeDcl((INode *)cast));
            }
        }
        else if (!(node->flags & FlagConvert))
            varDclExtendExp(ext, &cast->exp);
        return;
    }
    case VTupleTag:
    case ArrayLitTag:
    case TypeLitTag:
    {
        // A variant's literal ('Some[x]'), not a construction ('new H(x)')
        if (node->tag == TypeLitTag && !(node->flags & FlagIndex))
            return;
        Nodes *elems = node->tag == VTupleTag ? ((TupleNode *)node)->elems
            : node->tag == ArrayLitTag ? ((ArrayNode *)node)->elems : ((FnCallNode *)node)->args;
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(elems, cnt, elemp)) {
            INode **valp = (*elemp)->tag == NamedValTag ? &((NamedValNode *)*elemp)->val : elemp;
            uint32_t ran = ext->ran;
            varDclExtendExp(ext, valp);
            if (ext->ran == ran)
                varDclExtendPend(ext, valp);
        }
        return;
    }
    // A block's final expression, and so each arm of an 'if' or a 'match' (a
    // block whose final expression is an 'if'), as Rust 2024 extends them.
    // What it extends is given its value there (varDclExtendEmit).
    case BlockTag:
    {
        BlockNode *blk = (BlockNode *)node;
        if ((blk->flags & FlagLoop) || blk->stmts->used == 0 || !isExpNode(nodesLast(blk->stmts)))
            return;
        // Its code stays where it is, so what ran before it here is no
        // reason to move what runs after it ('ran')
        uint8_t svinblock = ext->inblock;
        uint32_t svran = ext->ran;
        Nodes *svtail = ext->tail;
        INode ***svpending = ext->pending;
        uint32_t svnpending = ext->npending;
        uint32_t svpendcap = ext->pendcap;
        ext->inblock = 1;
        ext->tail = NULL;
        ext->pending = NULL;
        ext->npending = 0;
        ext->pendcap = 0;
        varDclExtendExp(ext, &nodesLast(blk->stmts));
        if (ext->tail) {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(ext->tail, cnt, nodesp))
                nodesInsert(&blk->stmts, *nodesp, blk->stmts->used - 1);
        }
        ext->inblock = svinblock;
        ext->ran = svran;
        ext->tail = svtail;
        ext->pending = svpending;
        ext->npending = svnpending;
        ext->pendcap = svpendcap;
        return;
    }
    case IfTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((IfNode *)node)->condblk, cnt, nodesp)) {
            nodesp++; cnt--;
            varDclExtendExp(ext, nodesp);
        }
        return;
    }
    default:
        return;
    }
}

// Walk the initializer's extending positions, in the order they run
static void varDclExtend(VarDclExtend *ext, INode **valuep) {
    varDclExtendExp(ext, valuep);
    ext->npending = 0;
}

Nodes *varDclExtendEnd(TypeCheckState *pstate, VarDclExtend *ext) {
    pstate->extend = ext->outer;
    for (uint32_t i = 0; i < ext->ntemps; ++i) {
        VarDclTemp *temp = &ext->temps[i];
        if (!temp->kept)
            *temp->slot = temp->var->value;
    }
    return ext->hoisted;
}

// A variable holds its type by value: rule 4's report site
static void varDclSizeCheck(TypeCheckState *pstate, INode *node, void *extra) {
    VarDclNode *name = (VarDclNode *)node;
    INode *nosizeroot;
    char *nosize = itypeNoSizeCause(name->vtype, &nosizeroot);
    if (nosize) {
        errorMsgNode((INode*)name, ErrorNoSize, "Variable %s cannot be held by value: %s %s.",
            &name->namesym->namestr, itypeName(nosizeroot), nosize);
        itypeNoSizeExplain(name->vtype);
    }
}

// A '@workgroup' global is GPU memory its workgroup's invocations share, as a
// kernel's buffer is memory a dispatch's share: it holds what a buffer may
// (fnDclComputeData), on every target, so that the CPU and the GPU agree on one
// source. Nothing finalizes a workgroup's copy, which none of that needs.
static void varDclWorkgroupCheck(TypeCheckState *pstate, INode *node, void *extra) {
    VarDclNode *name = (VarDclNode *)node;
    char path[256];
    snprintf(path, sizeof(path), "%s", &name->namesym->namestr);
    const char *why = fnDclComputeData(name->vtype, path, sizeof(path));
    if (why)
        errorMsgNode((INode *)name, ErrorWorkgroupData,
            "'%s' is %s. A '@workgroup' global holds what a GPU's invocations share: 32-bit numbers (i32, u32, f32), Atomic[u32] and Atomic[i32], and structs and fixed arrays of them.",
            path, why);
    else if (itypeNeedsFinal(name->vtype))
        errorMsgNode((INode *)name, ErrorWorkgroupData,
            "'@workgroup' global %s's type needs finalizing, and nothing finalizes a workgroup's copy as its workgroup ends.",
            &name->namesym->namestr);
}

// Type check variable against its initial value
void varDclTypeCheck(TypeCheckState *pstate, VarDclNode *name) {
    itypeTypeCheck(pstate, (INode**)&name->perm);
    if (itypeTypeCheck(pstate, &name->vtype) == 0)
        return;

    // A function's static is owned by the function, which is what its symbol is
    // spelled after and what keeps two functions' statics of one name apart. An
    // inline function's body is copied into every caller, so a static in it
    // would be one copy per call site rather than one for all: refused.
    if ((name->flags & FlagStatic) && name->scope > 0 && pstate->fn) {
        if (pstate->fn->flags & FlagInline)
            errorMsgNode((INode*)name, ErrorBadStatic,
                "A static in an inline function would be one copy per call site, not one shared copy.");
        if (name->dclinfo.owner == NULL)
            dclInfoJoin((INode*)name, (INode*)pstate->fn);
    }

    // A parameter holding a borrowed reference holds one of the caller's, whose
    // lifetime is the caller band (1). Its declared type is shared, so it takes
    // a copy scoped so: what it points at is then the caller's however it is
    // reached, and a return or a store reads that from the type. A borrow of the
    // parameter itself is the function's (iexpGetLvalInfo). One whose type
    // names ''static' holds a global borrow, and keeps the global lifetime.
    if (name->scope == 1) {
        INode *vtypedcl = itypeGetTypeDcl(name->vtype);
        if (iexpIsBorrowType(vtypedcl) && !lifeIsStatic(vtypedcl))
            name->vtype = iexpScopedBorrowType(vtypedcl, name->vtype, 1);
    }

    // A local's declared type names only invariant lifetimes its function's
    // signature does
    if (name->scope >= 2 && name->vtype != unknownType)
        lifeBrandKnown(name->vtype, (INode*)name);

    // An initializer need not be specified, but if not, it must have a declared type
    if (name->value == NULL) {
        if (name->vtype == unknownType) {
            errorMsgNode((INode*)name, ErrorNoType, "Declared name must specify a type or value");
            return;
        }
    }
    // Type check the initialization value
    else {
        // Verify that declared type and initial value type match
        // A parameter's default that is a string literal for a type declaring
        // 'fromLiteral' ('&Path', or 'Path') stays the literal: each call that
        // takes the default makes the temporary, or the value, itself, as it does
        // for a literal written as the argument (fnCallFinalizeArgs). A default
        // is a constant, and a temporary has no statement of its own here.
        int matches;
        int deferred = 0;
        if (name->scope == 1 && !(name->flags & FlagStatic) && name->value->tag == StringLitTag
            && name->vtype != unknownType) {
            inodeTypeCheck(pstate, &name->value, name->vtype);
            deferred = slitDefaultDeferred(name->value, name->vtype);
            matches = deferred ? 1 : iexpCheckedCoerceIn(pstate, name->vtype, &name->value);
        }
        else
            matches = iexpTypeCheckCoerce(pstate, name->vtype, &name->value);
        // A temporary the initializer extends becomes a hidden local of the block
        if (matches && pstate->extend && pstate->extend->var == name && !(name->flags & FlagStatic))
            varDclExtend(pstate->extend, &name->value);
        if (!matches)
            errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
        else if (name->vtype == unknownType) {
            // A local of the type its initializer has, a '&uni' reference held
            // in a place, borrows from it as a local of the declared type does
            if (name->scope >= 2 && !(name->flags & FlagStatic) && isExpNode(name->value)
                && borrowUniReborrows(name->value, iexpGetTypeDcl(name->value)))
                borrowUniReborrow(&name->value, iexpGetTypeDcl(name->value));
            name->vtype = ((IExpNode *)name->value)->vtype;
        }
        // A declared borrowed-reference type is a shared node carrying no
        // lifetime, so the variable takes a copy of it scoped as its initializer
        // is, as an undeclared one takes the initializer's own scoped type:
        // returning or storing it is then judged by what it was borrowed from
        else if (!deferred && isExpNode(name->value)) {
            INode *vtypedcl = itypeGetTypeDcl(name->vtype);
            INode *scoped = iexpCoerceType(name->value, vtypedcl);
            if (scoped != vtypedcl)
                name->vtype = scoped;
        }
        // Global variables, function parameters and statics require literal
        // initializers: the value is the storage's initializer, written once
        // before anything runs. A parameter may default to 'srcFile()' or
        // 'srcLine()' too, each a constant where each call taking it is
        // (fnCallFinalizeArgs). An expression of constants is folded into the
        // one it computes (litFoldConst).
        if ((name->scope <= 1 || (name->flags & FlagStatic)) && !litFoldConst(&name->value)
            && !(name->scope == 1 && !(name->flags & FlagStatic) && intrinsicIsSrcDefault(name->value)))
            errorMsgNode((INode*)name, ErrorNotLit, "Variable may only be initialized with a literal value.");
    }

    // A global or a static is found by no collector, so it may not hold a
    // traced reference (judged once every type is laid out)
    if (name->scope == 0 || (name->flags & FlagStatic))
        regionTracedGlobalNote(name);

    // A variable holds its type by value, so that type has to be able to say how
    // large it is. A parameter of a function reference's signature, checked as
    // a reference's target while a layout is in flight, asks once that type is
    // laid out: a reference does not demand what it points at.
    if (structTargetDeferring())
        structDeferCheck(pstate, varDclSizeCheck, (INode*)name, NULL);
    else
        varDclSizeCheck(pstate, (INode*)name, NULL);
    if (name->dclinfo.facts & DclWorkgroup) {
        if (structTargetDeferring())
            structDeferCheck(pstate, varDclWorkgroupCheck, (INode*)name, NULL);
        else
            varDclWorkgroupCheck(pstate, (INode*)name, NULL);
    }
}

// Perform data flow analysis
void varDclFlow(FlowState *fstate, VarDclNode **vardclnode) {
    // A static is not the block's to release: its storage outlives every call,
    // and its literal initializer moves nothing. It holds a value from the
    // start, as a global does, which the parser already recorded.
    if ((*vardclnode)->flags & FlagStatic)
        return;
    flowAddVar(*vardclnode);
    // The temporary an operator changing its operand in place borrows it
    // through ('x += 1', 'v <- (a, b)') is the operator's own, as a method's
    // receiver is, and holds nothing past it
    if ((*vardclnode)->namesym != tempName)
        flowGateHolder(fstate, (*vardclnode)->vtype);
    if ((*vardclnode)->value) {
        flowLoadValue(fstate, &((*vardclnode)->value));
        flowHandleMoveOrCopy(&((*vardclnode)->value));  // initialization copies/moves value
        (*vardclnode)->flowtempflags |= VarInitialized;
    }
}
