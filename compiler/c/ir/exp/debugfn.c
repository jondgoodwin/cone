/** The debug form of a value, generated on use
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

// 'x.toDebug(out)' writes x in a programmer's form: field names, quoted and
// escaped text, the variant's name. A type that declares a 'toDebug' (or folds
// one: a String's is str's) is called as it is. For any other struct or enum
// the compiler writes one, but only when a call asks, so a program that never
// asks carries none: fnCallDebugLower, run on a call before its receiver's
// members are looked up, makes a function of the module the call stands in,
//    fn toDebug#N(self: &T, out: &uni TextOut)
// once for each type and module, and turns the call into a call of it.
//
// The function is the module's, not the type's: the type may be another
// package's, which this compile cannot add to and which no importer would link
// against. It reaches the type's private fields through its 'self', as the
// type's own methods do. A field is written by its own 'toDebug', found or
// generated in the same way, so nesting builds no strings.
//
// What is not a struct or enum: a number or bool is written as its text (the
// call becomes 'toText'); a char through the writer's putDebugChar; a tuple
// element by element; a raw pointer, a function, a trait object and an actor's
// handle as a fixed mark (<Secret> for the handle); an array, a List and a Dict through a generic free function of core or
// collections (debugCollectionFn: a method would be checked for every instance). A
// str has 'toDebug' in core.

static struct DebugEntry {
    INode *type;
    ModuleNode *mod;
    FnDclNode *fn;
} *debugCache = NULL;
static size_t debugCount = 0;
static size_t debugCap = 0;
static unsigned debugSerial = 0;


static Name *debugMethodName(const char *name) {
    return nametblFind((char*)name, strlen(name));
}

// core's TextOut, found as the number types' toText was given it
static INode *debugTextOut(void) {
    INode *found = iNsTypeFindFnField((INsTypeNode*)i64Type, debugMethodName("toText"));
    if (found == NULL || found->tag != FnDclTag)
        return NULL;
    FnSigNode *sig = (FnSigNode*)((FnDclNode*)found)->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->parms->used < 2)
        return NULL;
    VarDclNode *oparm = (VarDclNode*)nodesGet(sig->parms, 1);
    INode *ref = itypeGetTypeDcl(oparm->vtype);
    return ref->tag == RefTag ? itypeGetTypeDcl(((RefNode*)ref)->vtexp) : NULL;
}

// What the body being built is made from
typedef struct {
    INode *lex;
    VarDclNode *self;
    VarDclNode *out;
} DebugBuild;

static INode *debugUse(DebugBuild *b, VarDclNode *parm) {
    return newNameUseFromDclNode((INode*)parm, b->lex);
}

// out.method(arg), arg optional
static INode *debugOutCall(DebugBuild *b, const char *method, INode *arg) {
    FnCallNode *call = newFnCallLower(b->lex, debugUse(b, b->out), 1);
    call->methfld = (INode*)newMemberUseNode(debugMethodName(method));
    inodeLexCopy(call->methfld, b->lex);
    if (arg)
        nodesAdd(&call->args, arg);
    return (INode*)call;
}

static INode *debugLit(DebugBuild *b, const char *text) {
    size_t len = strlen(text);
    SLitNode *lit = newSLitNode(memAllocStr((char*)text, len), (uint32_t)len);
    inodeLexCopy((INode*)lit, b->lex);
    return (INode*)lit;
}

static INode *debugWrite(DebugBuild *b, const char *text) {
    return debugOutCall(b, "writeStr", debugLit(b, text));
}

// self.member, a field by name or a tuple element by its number
static INode *debugMember(DebugBuild *b, INode *base, INode *member) {
    FnCallNode *access = newFnCallLower(b->lex, base, 0);
    access->methfld = member;
    inodeLexCopy(access->methfld, b->lex);
    return (INode*)access;
}

// base.toDebug(out)
static INode *debugValue(DebugBuild *b, INode *base) {
    FnCallNode *call = newFnCallLower(b->lex, base, 1);
    call->methfld = (INode*)newMemberUseNode(debugMethodName("toDebug"));
    inodeLexCopy(call->methfld, b->lex);
    nodesAdd(&call->args, debugUse(b, b->out));
    return (INode*)call;
}

static void debugStmt(BlockNode *blk, INode *stmt) {
    nodesAdd(&blk->stmts, stmt);
}

static BlockNode *debugBlock(DebugBuild *b) {
    BlockNode *blk = newBlockNode();
    inodeLexCopy((INode*)blk, b->lex);
    return blk;
}

// The fields a struct or variant writes, in order: not the discriminant, and not the
// placeholder for the fields its enum shares
static int debugIsWritten(INode *field) {
    return field->tag == FieldDclTag && !(field->flags & (IsMixin | IsTagField));
}

// "Name{a: <a>, b: <b>}", or "Name(<payload>)" for a variant declared as a type, or
// "Name" for one with nothing to show; 'base' is how the value is reached ('self')
static void debugFields(DebugBuild *b, BlockNode *blk, StructNode *st, Name *name, INode *(*mkbase)(DebugBuild*)) {
    char buf[300];
    INode **fldp;
    uint32_t cnt;
    int first = 1;
    FieldDclNode *payload = structTypeVariantField(st);
    if (payload) {
        snprintf(buf, sizeof(buf), "%s(", &name->namestr);
        debugStmt(blk, debugWrite(b, buf));
        debugStmt(blk, debugValue(b, debugMember(b, mkbase(b), (INode*)newMemberUseNode(payloadName))));
        debugStmt(blk, debugWrite(b, ")"));
        return;
    }
    for (nodelistFor(&st->fields, cnt, fldp)) {
        if (!debugIsWritten(*fldp))
            continue;
        FieldDclNode *field = (FieldDclNode*)*fldp;
        if (first)
            snprintf(buf, sizeof(buf), "%s{%s: ", &name->namestr, &field->namesym->namestr);
        else
            snprintf(buf, sizeof(buf), ", %s: ", &field->namesym->namestr);
        debugStmt(blk, debugWrite(b, buf));
        first = 0;
        debugStmt(blk, debugValue(b, debugMember(b, mkbase(b), (INode*)newMemberUseNode(field->namesym))));
    }
    if (first)
        debugStmt(blk, debugWrite(b, &name->namestr));
    else
        debugStmt(blk, debugWrite(b, "}"));
}

static INode *debugSelf(DebugBuild *b) {
    return debugUse(b, b->self);
}

// A struct, a variant, or an enum: the body inside the depth guard
static void debugStructBody(DebugBuild *b, BlockNode *blk, StructNode *st) {
    if (st->flags & EnumType) {
        // One arm per variant, tested 'self is V'; the arm narrows self
        IfNode *chain = newIfNode();
        inodeLexCopy((INode*)chain, b->lex);
        INode **varp;
        uint32_t cnt;
        if (st->derived) {
            for (nodesFor(st->derived, cnt, varp)) {
                StructNode *variant = (StructNode*)*varp;
                CastNode *test = newIsNode(debugSelf(b), newNameUseFromDclNode((INode*)variant, b->lex));
                inodeLexCopy((INode*)test, b->lex);
                nodesAdd(&chain->condblk, (INode*)test);
                BlockNode *arm = debugBlock(b);
                debugFields(b, arm, variant, variant->namesym, debugSelf);
                nodesAdd(&chain->condblk, (INode*)arm);
            }
            if (chain->condblk->used > 0)
                debugStmt(blk, (INode*)chain);
        }
    }
    else
        debugFields(b, blk, st, st->namesym, debugSelf);
}

static ProgramNode *debugProgram = NULL;

void debugSetProgram(INode *pgm) {
    debugProgram = (ProgramNode*)pgm;
}

// The function for a value type, or NULL. It joins the module the call stands in; when
// this compile does not generate that module (a call in core's wrapper, or any
// package's generic, reached from the program's own code) it joins the first
// module that it does generate, where the object that holds the call's instance
// can link to it.
static FnDclNode *debugMake(TypeCheckState *pstate, INode *valtype, INode *lex) {
    ModuleNode *mod = pstate->fn ? dclInfoGetModule((INode*)pstate->fn)
        : pstate->typenode ? dclInfoGetModule(pstate->typenode) : NULL;
    if (mod == NULL)
        return NULL;
    if (!(mod->flags & FlagGenMod) && debugProgram) {
        INode **modp;
        uint32_t mcnt;
        for (nodesFor(debugProgram->modules, mcnt, modp))
            if (((ModuleNode*)*modp)->flags & FlagGenMod && !modIsTemplate((ModuleNode*)*modp)) {
                mod = (ModuleNode*)*modp;
                break;
            }
    }
    for (size_t i = 0; i < debugCount; ++i)
        if (debugCache[i].type == valtype && debugCache[i].mod == mod)
            return debugCache[i].fn;
    INode *textout = debugTextOut();
    if (textout == NULL)
        return NULL;

    // fn toDebug#N(self: &T, out: &uni TextOut)
    char buf[48];
    snprintf(buf, sizeof(buf), "toDebug#%u", ++debugSerial);
    NameUseNode *outtype = newNameUseNode(((StructNode*)textout)->namesym);
    outtype->dclnode = textout;
    inodeLexCopy((INode*)outtype, lex);
    RefNode *outref = newRefNodeFull(RefTag, lex, borrowRef, newPermUseNode(uniPerm), (INode*)outtype);
    INode *selftype = valtype->tag == StructTag
        ? newNameUseFromDclNode(valtype, lex) : valtype;
    RefNode *selfref = newRefNodeFull(RefTag, lex, borrowRef, newPermUseNode(roPerm), selftype);
    VarDclNode *self = newVarDclFull(selfName, VarDclTag, (INode*)selfref, newPermUseNode(immPerm), NULL);
    VarDclNode *out = newVarDclFull(debugMethodName("out"), VarDclTag, (INode*)outref, newPermUseNode(immPerm), NULL);
    inodeLexCopy((INode*)self, lex);
    inodeLexCopy((INode*)out, lex);
    self->scope = 1;
    out->scope = 1;
    self->index = 0;
    out->index = 1;
    self->flowtempflags |= VarInitialized;
    out->flowtempflags |= VarInitialized;
    FnSigNode *sig = newFnSigNode();
    sig->rettype = (INode*)newVoidNode();
    inodeLexCopy(sig->rettype, lex);
    nodesAdd(&sig->parms, (INode*)self);
    nodesAdd(&sig->parms, (INode*)out);

    DebugBuild b = { lex, self, out };
    BlockNode *body = debugBlock(&b);
    if (valtype->tag == StructTag) {
        // if out.debugEnter() { <fields>; out.debugLeave(); }
        IfNode *guard = newIfNode();
        inodeLexCopy((INode*)guard, lex);
        nodesAdd(&guard->condblk, debugOutCall(&b, "debugEnter", NULL));
        BlockNode *inner = debugBlock(&b);
        debugStructBody(&b, inner, (StructNode*)valtype);
        debugStmt(inner, debugOutCall(&b, "debugLeave", NULL));
        nodesAdd(&guard->condblk, (INode*)inner);
        debugStmt(body, (INode*)guard);
    }
    else if (valtype->tag == TTupleTag) {
        Nodes *elems = ((TupleNode*)valtype)->elems;
        debugStmt(body, debugWrite(&b, "("));
        for (uint32_t i = 0; i < elems->used; ++i) {
            if (i > 0)
                debugStmt(body, debugWrite(&b, ", "));
            ULitNode *index = newULitNode(i, (INode*)usizeType);
            inodeLexCopy((INode*)index, lex);
            debugStmt(body, debugValue(&b, debugMember(&b, debugSelf(&b), (INode*)index)));
        }
        debugStmt(body, debugWrite(&b, ")"));
    }
    else
        return NULL;

    FnDclNode *fn = newFnDclNode(nametblFind(buf, strlen(buf)), 0, (INode*)sig, (INode*)body);
    inodeLexCopy((INode*)fn, lex);
    fn->generatedFor = valtype;     // part of the type: it sees what the type's own code sees
    nodesAdd(&mod->nodes, (INode*)fn);
    dclInfoJoin((INode*)fn, (INode*)mod);
    if (debugCount == debugCap) {
        debugCap = debugCap ? debugCap * 2 : 16;
        struct DebugEntry *grown = memAllocBlk(debugCap * sizeof(struct DebugEntry));
        if (debugCount)
            memcpy(grown, debugCache, debugCount * sizeof(struct DebugEntry));
        debugCache = grown;
    }
    debugCache[debugCount].type = valtype;
    debugCache[debugCount].mod = mod;
    debugCache[debugCount].fn = fn;
    ++debugCount;

    // Checked now, under the state a module's own walk gives a function of its own
    TypeCheckState tstate;
    tstate.typenode = NULL;
    tstate.fn = NULL;
    tstate.scope = 0;
    tstate.extend = NULL;
    INode *fnnode = (INode*)fn;
    inodeTypeCheckAny(&tstate, &fnnode);
    return fn;
}

// out.writeStr("mark") in place of the call
static void debugReplaceWithMark(TypeCheckState *pstate, FnCallNode **nodep, const char *mark) {
    FnCallNode *node = *nodep;
    INode *out = nodesGet(node->args, 0);
    size_t len = strlen(mark);
    SLitNode *lit = newSLitNode(memAllocStr((char*)mark, len), (uint32_t)len);
    inodeLexCopy((INode*)lit, (INode*)node);
    FnCallNode *call = newFnCallLower((INode*)node, out, 1);
    call->methfld = (INode*)newMemberUseNode(debugMethodName("writeStr"));
    inodeLexCopy(call->methfld, (INode*)node);
    nodesAdd(&call->args, (INode*)lit);
    *nodep = call;
    inodeTypeCheckAny(pstate, (INode**)nodep);
}

// An array, list or dictionary is written by a generic free function of its
// own module ('debugArray', 'debugList', 'debugDict'), which is checked only for
// the element types a call asks about. (A method would be checked for every
// instance, asked or not.) NULL for any other type.
static FnDclNode *debugCollectionFn(INode *valtype) {
    if (valtype->tag == ArrayTag) {
        INode *textout = debugTextOut();
        ModuleNode *core = textout ? (ModuleNode*)dclInfoGetModule(textout) : NULL;
        INode *arr = core ? namespaceFind(&core->namespace, debugMethodName("debugArray")) : NULL;
        return arr && arr->tag == FnDclTag ? (FnDclNode*)arr : NULL;
    }
    if (valtype->tag != StructTag)
        return NULL;
    const char *fname;
    const char *tname = &((StructNode*)valtype)->namesym->namestr;
    if (strcmp(tname, "Array") == 0)
        fname = "debugArray";
    else if (strcmp(tname, "List") == 0)
        fname = "debugList";
    else if (strcmp(tname, "Dict") == 0)
        fname = "debugDict";
    else
        return NULL;
    ModuleNode *mod = (ModuleNode*)dclInfoGetModule(valtype);
    if (mod == NULL)
        return NULL;
    INode *found = namespaceFind(&mod->namespace, debugMethodName(fname));
    return found && found->tag == FnDclTag ? (FnDclNode*)found : NULL;
}

int debugWillGenerate(INode *piece) {
    INode *valtype = iexpGetDerefTypeDcl(piece);
    if (valtype == NULL || valtype == errorType || valtype == unknownType)
        return 0;
    if (valtype->tag == ArrayTag || valtype->tag == TTupleTag)
        return 1;
    return valtype->tag == StructTag
        && iNsTypeFindFnField((INsTypeNode*)valtype, debugMethodName("toDebug")) == NULL;
}

int fnCallDebugLower(TypeCheckState *pstate, FnCallNode **nodep, int argschecked) {
    FnCallNode *node = *nodep;
    if (node->methfld == NULL || !isNameUseNode(node->methfld)
        || ((NameUseNode*)node->methfld)->namesym != debugMethodName("toDebug")
        || node->args == NULL || node->args->used != 1 || !isExpNode(node->objfn)
        || (node->flags & (FlagIndex | FlagLvalOp | FlagOperator | FlagBorrow | FlagRange)))
        return 0;
    INode *recvtype = iexpGetTypeDcl(node->objfn);
    if (recvtype == errorType || recvtype == unknownType)
        return 0;
    INode *valtype = iexpGetDerefTypeDcl(node->objfn);
    if (valtype == NULL || valtype == errorType || valtype == unknownType)
        return 0;
    // The writer is checked before the call is rewritten around it
    // (A lent writer made by a text send is checked once, with the call made here)
    if (!argschecked && nodesGet(node->args, 0)->tag != BorrowTag)
        inodeTypeCheckAny(pstate, &nodesGet(node->args, 0));
    if (!isExpNode(nodesGet(node->args, 0)) || iexpGetTypeDcl(nodesGet(node->args, 0)) == errorType)
        return 0;

    switch (valtype->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
        if (valtype == (INode*)charType) {
            // out.putDebugChar(x)
            INode *val = node->objfn;
            while (iexpGetTypeDcl(val)->tag == RefTag && derefInject(&val))
                ;
            INode *out = nodesGet(node->args, 0);
            node->objfn = out;
            node->methfld = (INode*)newMemberUseNode(debugMethodName("putDebugChar"));
            inodeLexCopy(node->methfld, (INode*)node);
            node->args = newNodes(1);
            nodesAdd(&node->args, val);
            return 0;
        }
        // Same text as display
        ((NameUseNode*)node->methfld)->namesym = debugMethodName("toText");
        return 0;
    case PtrTag:
        debugReplaceWithMark(pstate, nodep, "<pointer>");
        return 1;
    case FnSigTag:
        debugReplaceWithMark(pstate, nodep, "<fn>");
        return 1;
    case StructTag:
        break;
    case ArrayTag:
        break;
    case TTupleTag:
        break;
    default:
        return 0;
    }
    if (valtype->tag == StructTag) {
        StructNode *st = (StructNode*)valtype;
        if (iNsTypeFindFnField((INsTypeNode*)st, debugMethodName("toDebug")) != NULL)
            return 0;
        if (strncmp(&st->namesym->namestr, "closure#", 8) == 0) {
            debugReplaceWithMark(pstate, nodep, "<closure>");
            return 1;
        }
        // An actor's state is not the caller's to read: its handle is a fixed mark
        if (actorOfHandle((INode*)st) != NULL) {
            debugReplaceWithMark(pstate, nodep, "<Secret>");
            return 1;
        }
        if ((st->flags & TraitType) && !(st->flags & EnumType)) {
            char buf[200];
            snprintf(buf, sizeof(buf), "<%s>", &st->namesym->namestr);
            debugReplaceWithMark(pstate, nodep, buf);
            return 1;
        }
    }
    FnDclNode *fn = debugCollectionFn(valtype);
    if (fn == NULL)
        fn = debugMake(pstate, valtype, (INode*)node);
    if (fn == NULL)
        return 0;

    // fn(&x, out): x read through its references, then lent
    INode *recv = node->objfn;
    while (iexpGetTypeDcl(recv)->tag == RefTag && derefInject(&recv))
        ;
    INode *lent = (INode*)newRefNodeFull(BorrowTag, (INode*)node, borrowRef, newPermUseNode(roPerm), recv);
    inodeLexCopy(lent, (INode*)node);
    FnCallNode *call = newFnCallNode(newNameUseFromDclNode((INode*)fn, (INode*)node), 2);
    inodeLexCopy((INode*)call, (INode*)node);
    nodesAdd(&call->args, lent);
    nodesAdd(&call->args, nodesGet(node->args, 0));
    *nodep = call;
    inodeTypeCheckAny(pstate, (INode**)nodep);
    return 1;
}
