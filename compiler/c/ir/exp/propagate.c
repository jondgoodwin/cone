/** '?': a Result's or an Option's value, or an early return of its failure
 * @file
 *
 * See propagate.h for the block the parser builds and what this lowers it to.
 *
 * The lowering is built after name resolution, so every name it uses is bound
 * here, as each.c binds its own: a hidden variable's uses point at its
 * declaration, the variants the function returns are named by their
 * declarations, and a pattern's variant is looked up among the matched value's
 * when its test is checked (castPatternMark).
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>

// What '?' was given, or what the function returns
enum PropagateKind {
    PropagateNone,      // neither
    PropagateResult,    // core's Result
    PropagateOption,    // core's Option
};

// A use of a variable the lowering declared, positioned on lexnode
static INode *propagateUse(VarDclNode *var, INode *lexnode) {
    NameUseNode *use = newNameUseNode(var->namesym);
    inodeLexCopy((INode*)use, lexnode);
    use->dclnode = (INode*)var;
    return (INode*)use;
}

// Is this type an instance of core's enum of this name?
static int propagateIsCore(INode *type, Name *name) {
    if (type->tag != StructTag || !(type->flags & EnumType) || ((StructNode*)type)->namesym != name)
        return 0;
    ModuleNode *mod = dclInfoGetModule(type);
    return mod != NULL && mod->namesym == nametblFind("core", 4);
}

// Which of the two this type is
static enum PropagateKind propagateKindOf(INode *type) {
    if (propagateIsCore(type, resultName))
        return PropagateResult;
    if (propagateIsCore(type, optionName))
        return PropagateOption;
    return PropagateNone;
}

// The variant of this name among an enum's variants, or NULL
static StructNode *propagateVariant(INode *enumdcl, Name *name) {
    if (enumdcl->tag != StructTag || ((StructNode*)enumdcl)->derived == NULL)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((StructNode*)enumdcl)->derived, cnt, nodesp)) {
        if (((StructNode*)*nodesp)->namesym == name)
            return (StructNode*)*nodesp;
    }
    return NULL;
}

// What a variant declared as a type holds, 'Error(E)' holds E, or NULL
static INode *propagateHeld(StructNode *variant) {
    FieldDclNode *field = variant ? structTypeVariantField(variant) : NULL;
    return field ? field->vtype : NULL;
}

// Append a type as the reader writes it, an instance with its type arguments,
// 'Result[i32, ConfigError]'
static void propagateSpellCat(char *buf, size_t size, INode *type, int depth) {
    itypeSpellCat(buf, size, type, 0);
    INode *dcl = isTypeNode(type) ? itypeGetTypeDcl(type) : NULL;
    Nodes *args = dcl != NULL && dcl->tag == StructTag && depth < 4 ? itypeInstanceTypeArgs(dcl) : NULL;
    if (args == NULL)
        return;
    INode **argsp;
    uint32_t cnt;
    char *sep = "[";
    for (nodesFor(args, cnt, argsp)) {
        size_t used = strlen(buf);
        snprintf(buf + used, size - used, "%s", sep);
        propagateSpellCat(buf, size, *argsp, depth + 1);
        sep = ", ";
    }
    size_t used = strlen(buf);
    snprintf(buf + used, size - used, "]");
}

// The type as the reader writes it, for a message
static char *propagateSpell(char *buf, size_t size, INode *type) {
    buf[0] = '\0';
    propagateSpellCat(buf, size, type, 0);
    return buf;
}

// Leave the block giving no value: what is wrong has been said. Its value is
// typed as what the success holds, where that is known ('type' is a Result or
// an Option, or a reference to one), so that what uses it says nothing more;
// else as the error type. Nothing generates it: the function has an error.
static void propagateFail(BlockNode *blk, INode *type) {
    INode *payload = NULL;
    if (type != NULL && type->tag == RefTag)
        type = itypeGetTypeDcl(((RefNode*)type)->vtexp);
    if (type != NULL) {
        enum PropagateKind kind = propagateKindOf(type);
        if (kind != PropagateNone)
            payload = propagateHeld(propagateVariant(type, kind == PropagateResult ? okName : someName));
    }
    INode *fail = newErrorNode((INode*)blk);
    if (payload != NULL)
        ((IExpNode*)fail)->vtype = payload;
    nodesAdd(&blk->stmts, fail);
}

// A variable the lowering declares in an arm: a name the reader cannot write
static VarDclNode *propagateVar(char *name, INode *val, uint16_t scope, INode *lexnode) {
    VarDclNode *var = newVarDclFull(nametblFind(name, strlen(name)), VarDclTag, unknownType, (INode*)immPerm, val);
    inodeLexCopy((INode*)var, lexnode);
    var->scope = scope;
    return var;
}

// 'held is Variant' and the binding of what it holds, '[Variant]held', which
// share one type node, looked up among the matched enum's variants
static void propagateTest(VarDclNode *held, Name *variant, INode *lexnode, CastNode **test, CastNode **bind) {
    NameUseNode *pattern = newNameUseFromLex(variant, lexnode);
    castPatternMark((INode*)pattern);
    *test = newIsNode(propagateUse(held, lexnode), (INode*)pattern);
    inodeLexCopy((INode*)*test, lexnode);
    if (bind) {
        *bind = newConvCastNode(propagateUse(held, lexnode), (INode*)pattern);
        inodeLexCopy((INode*)*bind, lexnode);
        (*bind)->flags |= FlagMatchBind;
    }
}

// The variant's construction, 'Error(value)', or 'None' with no value: a call of
// the declaration, which type check makes the variant's literal (fnCallVariantCall)
static INode *propagateConstruct(StructNode *variant, INode *value, INode *lexnode) {
    FnCallNode *lit = newFnCallNode(newNameUseFromDclNode((INode*)variant, lexnode), 1);
    inodeLexCopy((INode*)lit, lexnode);
    if (value != NULL)
        nodesAdd(&lit->args, value);
    else
        lit->flags |= FlagIndex | FlagCtorParen;
    return (INode*)lit;
}

// How the error '?' was given becomes the error the function returns
enum PropagateConv {
    ConvAsIs,       // the same type, or one that coerces to it
    ConvFrom,       // the destination's 'from' takes it
    ConvWrap,       // the one variant of the destination that holds it
    ConvRefused,    // reported
};

// Decide how 'from' becomes 'to' (propagate.h), reporting where it cannot.
// '*wrap' is the variant that wraps it, for ConvWrap.
static enum PropagateConv propagateConvKind(INode *from, INode *to, StructNode **wrap, INode *lexnode, INode *rettype) {
    *wrap = NULL;
    // (a coercion that converts by a method or borrows is no error's own: a
    // borrow of it would not outlive the function)
    TypeCompare coerce = itypeMatches(to, from, Coercion);
    if (itypeIsSame(from, to) || coerce == EqMatch || coerce == CastSubtype || coerce == ConvSubtype)
        return ConvAsIs;
    INode *todcl = itypeGetTypeDcl(to);

    // An 'overload from' of the destination that takes it, asked as a call of it
    // asks, with nothing changed. A selection that is ambiguous is the call's to
    // report, so it is built as the call.
    if (isMethodType(todcl)) {
        INode *binding = iNsTypeFindFnField((INsTypeNode*)todcl, fromName);
        if (binding != NULL) {
            VarDclNode *probevar = newVarDclFull(anonName, VarDclTag, from, (INode*)immPerm, NULL);
            inodeLexCopy((INode*)probevar, lexnode);
            INode *probe = newNameUseFromDclNode((INode*)probevar, lexnode);
            Nodes *args = newNodes(1);
            nodesAdd(&args, probe);
            enum OverloadMatch status;
            iNsTypeFindMethod(binding, NULL, args, &status);
            if (status != OverloadNone)
                return ConvFrom;
        }
    }

    // The one variant holding exactly that type
    StructNode *found[2] = { NULL, NULL };
    int count = 0;
    if (todcl->tag == StructTag && (todcl->flags & EnumType) && ((StructNode*)todcl)->derived) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((StructNode*)todcl)->derived, cnt, nodesp)) {
            INode *held = propagateHeld((StructNode*)*nodesp);
            if (held != NULL && itypeIsSame(held, from)) {
                if (count < 2)
                    found[count] = (StructNode*)*nodesp;
                ++count;
            }
        }
    }
    char fromname[200], toname[200];
    if (count == 1) {
        *wrap = found[0];
        return ConvWrap;
    }
    if (count > 1) {
        errorMsgNode(lexnode, ErrorQuesAmbiguous,
            "'?' would wrap the error %s in %s, which has more than one variant holding it (%s and %s). Say which with an 'overload from' on %s that takes %s.",
            propagateSpell(fromname, sizeof(fromname), from), propagateSpell(toname, sizeof(toname), to),
            &found[0]->namesym->namestr, &found[1]->namesym->namestr, toname, fromname);
        return ConvRefused;
    }
    char retname[200];
    errorMsgNode(lexnode, ErrorQuesConvert,
        "'?' returns this error, %s, from a function returning %s, whose error %s cannot take it: declare an 'overload from' on %s that takes %s, or a variant of %s that holds it.",
        propagateSpell(fromname, sizeof(fromname), from), propagateSpell(retname, sizeof(retname), rettype),
        propagateSpell(toname, sizeof(toname), to), toname, fromname, toname);
    return ConvRefused;
}

// The error converted (propagateConvKind): 'value' as it is, 'To.from(value)'
// or 'Wrap(value)'
static INode *propagateConvert(enum PropagateConv kind, INode *value, INode *to, StructNode *wrap, INode *lexnode) {
    switch (kind) {
    case ConvFrom:
    {
        FnCallNode *call = newFnCallLower(lexnode, newNameUseFromDclNode(itypeGetTypeDcl(to), lexnode), 1);
        call->methfld = (INode*)newMemberUseNode(fromName);
        inodeLexCopy(call->methfld, lexnode);
        nodesAdd(&call->args, value);
        return (INode*)call;
    }
    case ConvWrap:
        return propagateConstruct(wrap, value, lexnode);
    default:
        return value;
    }
}

void propagateLower(TypeCheckState *pstate, BlockNode *blk) {
    blk->flags &= 0xFFFF - FlagPropagate;
    INode *lexnode = (INode*)blk;
    VarDclNode *held = (VarDclNode*)nodesGet(blk->stmts, 0);
    uint16_t armscope = (uint16_t)(pstate->scope + 1);

    // What '?' was given: reported already when it has no type
    INode *value = held->value;
    if (value == NULL || inodeIsError(value) || !isExpNode(value)) {
        propagateFail(blk, NULL);
        return;
    }
    INode *type = iexpGetTypeDcl(value);
    if (type == errorType || type == unknownType) {
        propagateFail(blk, NULL);
        return;
    }
    // A value typed as one of the variants, 'Ok[i32, E](5)', is a Result all the
    // same: held as its enum, which the tests and bindings are made against
    if (type->tag == StructTag && !(type->flags & EnumType) && (type->flags & HasTagField)) {
        StructNode *base = structBaseTraitDcl((StructNode*)type);
        if (base != NULL && propagateKindOf((INode*)base) != PropagateNone
            && iexpCheckedCoerceIn(pstate, (INode*)base, &held->value)) {
            held->vtype = (INode*)base;
            type = (INode*)base;
        }
    }
    char tname[200], rname[200];
    enum PropagateKind kind = propagateKindOf(type);
    if (kind == PropagateNone) {
        errorMsgNode(lexnode, ErrorQuesOperand,
            "'?' takes a Result or an Option, and this is %s.%s", propagateSpell(tname, sizeof(tname), type),
            (type->tag == RefTag && propagateKindOf(itypeGetTypeDcl(((RefNode*)type)->vtexp)) != PropagateNone)
                ? " A reference to one is not: '?' takes the value, so match on the reference instead." : "");
        propagateFail(blk, type);
        return;
    }
    char *kindname = kind == PropagateResult ? "Result" : "Option";

    // Where the failure goes: the function this is in returns one of the same kind
    FnDclNode *fn = pstate->fn;
    if (fn == NULL || fn->vtype == NULL || fn->vtype->tag != FnSigTag) {
        errorMsgNode(lexnode, ErrorQuesReturn,
            "'?' returns a failure from the function it is in, and this is in none.");
        propagateFail(blk, type);
        return;
    }
    if (fn->closure && fn->closure->retinfer) {
        errorMsgNode(lexnode, ErrorQuesReturn,
            "'?' returns a failure from this closure, whose return type is read off its returns: write its return type, %s %s, for the failure to go to.",
            kind == PropagateResult ? "a" : "an", kindname);
        propagateFail(blk, type);
        return;
    }
    INode *rettype = ((FnSigNode*)fn->vtype)->rettype;
    INode *retdcl = itypeGetTypeDcl(rettype);
    if (propagateKindOf(retdcl) != kind) {
        errorMsgNode(lexnode, ErrorQuesReturn,
            "'?' returns this %s's failure from %s, which returns %s, not a%s %s. Handle it here (match, let-else), or trust it with 'unwrap()' or 'expect(\"why\")'%s.",
            kindname, fn->namesym ? &fn->namesym->namestr : "the function",
            propagateSpell(rname, sizeof(rname), rettype), kind == PropagateResult ? "" : "n", kindname,
            kind == PropagateOption && propagateKindOf(retdcl) == PropagateResult ? ", or make it a Result with 'okOr(error)'"
            : kind == PropagateResult && propagateKindOf(retdcl) == PropagateOption ? ", or make it an Option with 'ok()'" : "");
        propagateFail(blk, type);
        return;
    }

    // The failure's arm, and how its error is converted
    Name *okname = kind == PropagateResult ? okName : someName;
    Name *failname = kind == PropagateResult ? errorVariantName : noneName;
    StructNode *retfail = propagateVariant(retdcl, failname);
    if (retfail == NULL) {
        propagateFail(blk, type);
        return;
    }
    BlockNode *failarm = newBlockNode();
    inodeLexCopy((INode*)failarm, lexnode);
    CastNode *failtest;
    INode *failvalue = NULL;
    if (kind == PropagateResult) {
        INode *from = propagateHeld(propagateVariant(type, failname));
        INode *to = propagateHeld(retfail);
        if (from == NULL || to == NULL) {
            propagateFail(blk, type);
            return;
        }
        StructNode *wrap;
        enum PropagateConv conv = propagateConvKind(from, to, &wrap, lexnode, rettype);
        if (conv == ConvRefused) {
            propagateFail(blk, type);
            return;
        }
        CastNode *failbind;
        propagateTest(held, failname, lexnode, &failtest, &failbind);
        VarDclNode *err = propagateVar("-err", (INode*)failbind, armscope, lexnode);
        nodesAdd(&failarm->stmts, (INode*)err);
        failvalue = propagateConvert(conv, propagateUse(err, lexnode), to, wrap, lexnode);
    }
    else
        propagateTest(held, failname, lexnode, &failtest, NULL);
    BreakRetNode *ret = newReturnNodeExp(propagateConstruct(retfail, failvalue, lexnode));
    inodeLexCopy((INode*)ret, lexnode);
    nodesAdd(&failarm->stmts, (INode*)ret);

    // The value's arm: what the success holds, which is the block's value
    CastNode *oktest, *okbind;
    propagateTest(held, okname, lexnode, &oktest, &okbind);
    VarDclNode *ok = propagateVar(kind == PropagateResult ? "-ok" : "-some", (INode*)okbind, armscope, lexnode);
    BlockNode *okarm = newBlockNode();
    inodeLexCopy((INode*)okarm, lexnode);
    nodesAdd(&okarm->stmts, (INode*)ok);
    nodesAdd(&okarm->stmts, propagateUse(ok, lexnode));

    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, lexnode);
    nodesAdd(&ifnode->condblk, (INode*)oktest);
    nodesAdd(&ifnode->condblk, (INode*)okarm);
    nodesAdd(&ifnode->condblk, (INode*)failtest);
    nodesAdd(&ifnode->condblk, (INode*)failarm);
    nodesAdd(&blk->stmts, (INode*)ifnode);
}

// ---- 'main' returning a Result ----------------------------------------------

static Name *mainResultName = NULL;     // "-mainresult", the entry's stand-in for 'main'
static Name *mainErrorName = NULL;      // "-mainError", conestd's cone_mainError

static void propagateMainNames() {
    if (mainResultName != NULL)
        return;
    mainResultName = nametblFind("-mainresult", 11);
    mainErrorName = nametblFind("-mainError", 10);
}

// '&str'
static INode *propagateStrRef(INode *lexnode) {
    return (INode*)newRefNodeFull(RefTag, lexnode, borrowRef, (INode*)newPermUseNode(roPerm),
        newNameUseFromDclNode((INode*)strTypeDcl, lexnode));
}

// Is this type '&str'?
static int propagateIsStrRef(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    return dcl->tag == RefTag && itypeGetTypeDcl(((RefNode*)dcl)->vtexp) == (INode*)strTypeDcl;
}

// 'recv.name()'
static FnCallNode *propagateMethodCall(INode *recv, Name *name, INode *lexnode) {
    FnCallNode *call = newFnCallLower(lexnode, recv, 1);
    call->methfld = (INode*)newMemberUseNode(name);
    inodeLexCopy(call->methfld, lexnode);
    return call;
}

// How the error's message is had as '&str': 0 refused (reported), 1 'message()'
// gives it, 2 it gives a String, whose 'view()' is lent
static int propagateMainMessage(INode *errtype, INode *errnode) {
    static Name *viewName = NULL;
    if (viewName == NULL)
        viewName = nametblFind("view", 4);
    char ename[200];
    INode *edcl = itypeGetTypeDcl(errtype);
    INode *msgfn = isMethodType(edcl) ? iNsTypeFindFnField((INsTypeNode*)edcl, messageName) : NULL;
    if (msgfn == NULL || msgfn->tag != FnDclTag || !(msgfn->flags & FlagMethFld)
        || ((FnDclNode*)msgfn)->vtype->tag != FnSigTag || ((FnSigNode*)((FnDclNode*)msgfn)->vtype)->parms->used != 1) {
        errorMsgNode(errnode, ErrorMainResult,
            "A 'main' returning a Result prints its error when it fails, with the error's 'message()', and %s declares no 'message(self: &)'.",
            propagateSpell(ename, sizeof(ename), errtype));
        return 0;
    }
    INode *gives = ((FnSigNode*)((FnDclNode*)msgfn)->vtype)->rettype;
    if (propagateIsStrRef(gives))
        return 1;
    INode *gdcl = itypeGetTypeDcl(gives);
    INode *view = isMethodType(gdcl) ? iNsTypeFindFnField((INsTypeNode*)gdcl, viewName) : NULL;
    if (view != NULL && view->tag == FnDclTag && ((FnDclNode*)view)->vtype->tag == FnSigTag
        && propagateIsStrRef(((FnSigNode*)((FnDclNode*)view)->vtype)->rettype))
        return 2;
    char gname[200];
    errorMsgNode(errnode, ErrorMainResult,
        "A 'main' returning a Result prints its error when it fails, with the error's 'message()', and the 'message()' of %s gives %s, which is neither a String nor a '&str'.",
        propagateSpell(ename, sizeof(ename), errtype), propagateSpell(gname, sizeof(gname), gives));
    return 0;
}

// Is this a module's 'main' that returns a Result: a function of the module, not
// generic, with a body, taking nothing or C's two arguments
static int propagateIsResultMain(INode *node) {
    if (node->tag != FnDclTag)
        return 0;
    FnDclNode *fn = (FnDclNode*)node;
    if (fn->namesym != nametblFind("main", 4) || (fn->flags & FlagMethFld) || fn->genericinfo
        || fn->value == NULL || fn->value->tag != BlockTag || fn->vtype->tag != FnSigTag)
        return 0;
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    if (sig->parms->used != 0 && sig->parms->used != 2)
        return 0;
    return propagateKindOf(itypeGetTypeDcl(sig->rettype)) == PropagateResult;
}

void propagateMainResult(TypeCheckState *pstate, ModuleNode *mod) {
    FnDclNode *mainfn = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->nodes, cnt, nodesp)) {
        if (propagateIsResultMain(*nodesp)) {
            mainfn = (FnDclNode*)*nodesp;
            break;
        }
    }
    if (mainfn == NULL)
        return;
    propagateMainNames();
    if (namespaceFind(&mod->namespace, mainResultName) != NULL)
        return;
    FnSigNode *mainsig = (FnSigNode*)mainfn->vtype;
    INode *lexnode = mainsig->rettype;
    INode *retdcl = itypeGetTypeDcl(mainsig->rettype);
    INode *errtype = propagateHeld(propagateVariant(retdcl, errorVariantName));
    if (errtype == NULL)
        return;
    int msgform = propagateMainMessage(errtype, mainsig->rettype);
    if (msgform == 0)
        return;

    // conestd's 'cone_mainError(msg: &str)', a C name of a C-named module
    FnSigNode *errsig = newFnSigNode();
    inodeLexCopy((INode*)errsig, lexnode);
    VarDclNode *msgparm = newVarDclFull(nametblFind("msg", 3), VarDclTag, propagateStrRef(lexnode), (INode*)immPerm, NULL);
    inodeLexCopy((INode*)msgparm, lexnode);
    msgparm->scope = 1;
    msgparm->index = 0;
    msgparm->flowtempflags |= VarInitialized;
    nodesAdd(&errsig->parms, (INode*)msgparm);
    errsig->rettype = (INode*)newVoidNode();
    inodeLexCopy(errsig->rettype, lexnode);
    FnDclNode *errfn = newFnDclNode(mainErrorName, FlagPub | FlagExtern, (INode*)errsig, NULL);
    inodeLexCopy((INode*)errfn, lexnode);
    errfn->dclinfo.facts |= DclCName;
    errfn->dclinfo.cname = "cone_mainError";
    dclInfoJoin((INode*)errfn, (INode*)mod);

    // The stand-in's parameters are main's
    FnSigNode *sig = newFnSigNode();
    inodeLexCopy((INode*)sig, lexnode);
    // (a list even when empty: a call, not a member's access)
    FnCallNode *callmain = newFnCallNode(newNameUseFromDclNode((INode*)mainfn, lexnode), mainsig->parms->used + 1);
    inodeLexCopy((INode*)callmain, lexnode);
    uint16_t parmnbr = 0;
    for (nodesFor(mainsig->parms, cnt, nodesp)) {
        VarDclNode *orig = (VarDclNode*)*nodesp;
        VarDclNode *parm = newVarDclNode(orig->namesym, VarDclTag, orig->perm);
        inodeLexCopy((INode*)parm, (INode*)orig);
        parm->vtype = orig->vtype;
        parm->scope = 1;
        parm->index = parmnbr++;
        parm->flowtempflags |= VarInitialized;
        nodesAdd(&sig->parms, (INode*)parm);
        nodesAdd(&callmain->args, propagateUse(parm, lexnode));
    }
    sig->rettype = (INode*)i32Type;

    // let -held = main(...)
    BlockNode *body = newBlockNode();
    inodeLexCopy((INode*)body, lexnode);
    VarDclNode *held = propagateVar("-held", (INode*)callmain, 2, lexnode);
    held->namesym = anonName;
    nodesAdd(&body->stmts, (INode*)held);

    // if -held is Error { let -err = [Error]-held; let -msg = -err.message(); -mainError(-msg.view()); return 1 }
    CastNode *failtest, *failbind;
    propagateTest(held, errorVariantName, lexnode, &failtest, &failbind);
    BlockNode *arm = newBlockNode();
    inodeLexCopy((INode*)arm, lexnode);
    VarDclNode *err = propagateVar("-err", (INode*)failbind, 3, lexnode);
    nodesAdd(&arm->stmts, (INode*)err);
    VarDclNode *msg = propagateVar("-msg", (INode*)propagateMethodCall(propagateUse(err, lexnode), messageName, lexnode), 3, lexnode);
    nodesAdd(&arm->stmts, (INode*)msg);
    INode *text = propagateUse(msg, lexnode);
    if (msgform == 2)
        text = (INode*)propagateMethodCall(text, nametblFind("view", 4), lexnode);
    FnCallNode *report = newFnCallNode(newNameUseFromDclNode((INode*)errfn, lexnode), 1);
    inodeLexCopy((INode*)report, lexnode);
    nodesAdd(&report->args, text);
    nodesAdd(&arm->stmts, (INode*)report);
    BreakRetNode *fail = newReturnNodeExp((INode*)newULitNodeTC(1, (INode*)i32Type));
    inodeLexCopy((INode*)fail, lexnode);
    inodeLexCopy(fail->exp, lexnode);
    nodesAdd(&arm->stmts, (INode*)fail);
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, lexnode);
    nodesAdd(&ifnode->condblk, (INode*)failtest);
    nodesAdd(&ifnode->condblk, (INode*)arm);
    nodesAdd(&body->stmts, (INode*)ifnode);

    // return 0
    BreakRetNode *ok = newReturnNodeExp((INode*)newULitNodeTC(0, (INode*)i32Type));
    inodeLexCopy((INode*)ok, lexnode);
    inodeLexCopy(ok->exp, lexnode);
    nodesAdd(&body->stmts, (INode*)ok);

    FnDclNode *entry = newFnDclNode(mainResultName, 0, (INode*)sig, (INode*)body);
    inodeLexCopy((INode*)entry, (INode*)mainfn);
    dclInfoJoin((INode*)entry, (INode*)mod);

    // Both join the module, so generation declares and defines them as it does
    // the module's own, and are checked now, the module's own being checked
    nodesAdd(&mod->nodes, (INode*)errfn);
    inodeTypeCheckAny(pstate, &nodesLast(mod->nodes));
    nodesAdd(&mod->nodes, (INode*)entry);
    namespaceAdd(&mod->namespace, mainResultName, (INode*)entry);
    inodeTypeCheckAny(pstate, &nodesLast(mod->nodes));
}

FnDclNode *propagateMainEntry(FnDclNode *mainfn) {
    if (mainResultName == NULL || !propagateIsResultMain((INode*)mainfn))
        return NULL;
    ModuleNode *mod = dclInfoGetModule((INode*)mainfn);
    INode *entry = mod ? namespaceFind(&mod->namespace, mainResultName) : NULL;
    return entry != NULL && entry->tag == FnDclTag ? (FnDclNode*)entry : NULL;
}
