/** Handling for type literals and constructions: a struct's value built from
 * its fields, which 'Point(1., 2.)' lowers to when the struct's implicit
 * 'init' takes its arguments (typeLitNewCheck), or which an enum's variant
 * writes with parentheses, 'Some(x)', or by its name alone when it holds nothing,
 * 'None'; and an allocation's value in brackets, '+Rc-mut Node[1]' (refused); and a number's
 * conversion, 'u64.from(count)', which type check lowers into the same node
 * (fnCallNumberFrom)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <stdio.h>

INode *typeLitSelfFill = NULL;

// Is this a number type, the target of a conversion rather than a struct literal?
static int typeLitIsNbrType(INode *littype) {
    return littype->tag == IntNbrTag || littype->tag == UintNbrTag || littype->tag == FloatNbrTag;
}

// Is this struct a variant, built with its discriminant: an enum's, or a
// tagged trait's?
static int typeLitIsVariant(StructNode *strnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp))
        if ((*nodesp)->flags & IsTagField)
            return 1;
    return 0;
}

// Is this type-checked value one 'new' constructs: a construction, or a struct
// that is not a variant written in brackets?
int typeLitIsConstruction(INode *node) {
    if (node->flags & FlagNew)
        return 1;
    if (node->tag != TypeLitTag)
        return 0;
    INode *littype = itypeGetTypeDcl(((FnCallNode*)node)->vtype);
    return littype->tag == StructTag && !typeLitIsVariant((StructNode*)littype);
}

// Serialize a type literal: a construction, a variant's or an allocation's
// value, and a number's conversion, each as it was written
void typeLitPrint(FnCallNode *node) {
    int conversion = node->vtype && (typeLitIsNbrType(itypeGetTypeDcl(node->vtype))
        || itypeGetTypeDcl(node->vtype)->tag == VectorTag || itypeGetTypeDcl(node->vtype)->tag == ArrayTag);
    int paren = conversion || (node->flags & (FlagNew | FlagCtorParen));
    if (node->flags & FlagNew)
        inodeFprint("new ");
    if (node->objfn)
        inodePrintNode(node->objfn);
    // An empty variant is its name alone
    if ((node->flags & FlagCtorParen) && (node->args == NULL || node->args->used == 0))
        return;
    INode **nodesp;
    uint32_t cnt;
    inodeFprint(conversion ? ".from(" : paren ? "(" : "[");
    for (nodesFor(node->args, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt)
            inodeFprint(",");
    }
    inodeFprint(paren ? ")" : "]");
}

// Check the type literal node (actually done by fncall)
void typeLitNameRes(NameResState *pstate, FnCallNode *arrlit) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(arrlit->args, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
}

// Is the type literal actually a literal?
int typeLitIsLiteral(FnCallNode *node) {
    INode **nodesp;
    uint32_t cnt;
    // A vector, or the array a vector converts to, is built where it is used
    if (node->vtype && (itypeGetTypeDcl(node->vtype)->tag == VectorTag || itypeGetTypeDcl(node->vtype)->tag == ArrayTag))
        return 0;
    for (nodesFor(node->args, cnt, nodesp)) {
        INode *arg = *nodesp;
        if (arg->tag == NamedValTag)
            arg = ((NamedValNode*)arg)->val;
        if (!litIsLiteral(arg))
            return 0;
    }
    return 1;
}

// May a value of fromtype be converted to bool? A number may, and so may a
// reference or a pointer, which converts by asking whether it is non-null, as a
// condition asks
static int typeLitConvertsToBool(INode *fromtype) {
    switch (fromtype->tag) {
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
    case RefTag:
    case PtrTag:
        return 1;
    default:
        return 0;
    }
}

// Type check the value a number's 'from' converts, its one argument, already
// type checked. Returns 0 when it does not convert.
int typeLitNbrFromCheck(FnCallNode *conv, INode *type) {
    INode *first = nodesGet(conv->args, 0);
    INode *firsttype = itypeGetTypeDcl(((IExpNode*)first)->vtype);

    // 'bool.from(value)' accepts a reference or a pointer too. Every other
    // number type requires a number source: a pointer has no conversion to an
    // integer.
    if (type == (INode*)boolType) {
        if (!typeLitConvertsToBool(firsttype)) {
            errorMsgNode((INode*)first, ErrorNbrFrom, "bool.from converts a number, a reference or a pointer");
            return 0;
        }
        return 1;
    }

    if (!typeLitIsNbrType(firsttype)) {
        errorMsgNode((INode*)first, ErrorNbrFrom, "%s.from converts a number", &((NbrNode*)type)->namesym->namestr);
        return 0;
    }
    return 1;
}

// Return true if desired named field is found and swapped into place
int typeLitGetName(Nodes *args, uint32_t argi, Name *name) {
    uint32_t nargs = args->used;
    uint32_t i = argi;
    for (; i < nargs; i++) {
        NamedValNode *node = (NamedValNode*)nodesGet(args, i);
        if (node->tag == NamedValTag && ((NameUseNode*)node->name)->namesym == name) {
            nodesMove(args, argi, i);
            return 1;
        }
    }
    return 0;
}

// Reorder the literal's field values to the same order as the type's fields.
// Also refuse a value for a private field where 'private' says the literal is
// written outside the module that declares the type.
int typeLitStructReorder(FnCallNode *arrlit, StructNode *strnode, int private) {

    int retcode = 1;
    INode **nodesp;
    uint32_t cnt;
    uint32_t argi = 0;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;

        // If field represents a discriminated tag, inject struct's discriminant nbr.
        // A negative one's 64 bits are cut to the discriminant's width where the
        // literal is generated.
        if (field->flags & IsTagField) {
            ULitNode *tagnbrnode = newULitNodeTC((uint64_t)strnode->tagnbr, field->vtype);
            nodesInsert(&arrlit->args, (INode*)tagnbrnode, argi++);
            continue;
        }

        // A field value has been specified...
        if (argi < arrlit->args->used) {
            // If we have a named value, insert the proper named value here where it belongs
            INode **litval = &nodesGet(arrlit->args, argi);
            if ((*litval)->tag == NamedValTag && !typeLitGetName(arrlit->args, argi, field->namesym)) {
                // Use default value for unmatched field, if the type defined one.
                // The default is the type's own value, not one the literal gives,
                // so leaving a private field to it is allowed from anywhere.
                if (field->value) {
                    nodesInsert(&arrlit->args, field->value, argi);
                    ++argi;
                    continue;
                }
                else {
                    errorMsgNode((INode*)arrlit, ErrorBadArray, "Cannot find named value matching the field %s", &field->namesym->namestr);
                    ++argi;
                    retcode = 0;
                    continue;
                }
            }
            // A private field is given a value only by its type's module
            if (!private && inodeIsPrivate((INode*)field)) {
                errorMsgNode(*litval, ErrorNotPublic, "May not give a value to the private field %s from outside the module that declares %s.",
                    &field->namesym->namestr, &strnode->namesym->namestr);
                retcode = 0;
            }
        }
        // Append default value if no value specified
        else if (field->value) {
            nodesAdd(&arrlit->args, field->value);
        }
        else {
            errorMsgNode((INode*)arrlit, ErrorBadArray, "Not enough values specified on type literal");
            while (cnt--)
                nodesAdd(&arrlit->args, (INode*)newULitNodeTC(0,field->vtype));  // Put in fake nodes to pretend we are ok
            return 0;
        }
        ++argi;
    }
    if (argi < arrlit->args->used) {
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Too many values specified on type literal");
        retcode = 0;
    }
    return retcode;
}

// Type check a struct literal
void typeLitStructCheck(TypeCheckState *pstate, FnCallNode *arrlit, StructNode *strnode) {

    // Ensure type has been type-checked, in case any rewriting/semantic analysis was needed
    itypeTypeCheck(pstate, &arrlit->vtype);

    // Reorder the literal's arguments to match the type's field order. A private
    // field is given a value by any code of the module that declares the type
    // (structSeesPrivate).
    int private = (INode*)strnode == pstate->typenode || structSeesPrivate(pstate, (INode*)strnode);
    if (typeLitStructReorder(arrlit, strnode, private) == 0)
        return;

    // A struct naming invariant lifetimes binds each to the brand its fields'
    // values carry, one per name, and its value carries those brands; one no
    // field's value gives is minted here, as a call minting one would
    LifeBind *brands = lifeTypeHasBrands((INode*)strnode) ? lifeBindBegin(NULL) : NULL;
    if (brands)
        lifeBindUse(brands, (INode*)strnode, arrlit->vtype, (INode*)arrlit);

    uint32_t cnt;
    INode **nodesp;
    uint32_t argi = 0;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        INode **litval = &nodesGet(arrlit->args, argi);
        // A value given by name is coerced inside its NamedValNode: the coercion
        // must see the value itself, so a string literal borrows as an lval and
        // an untyped number literal adopts the field's type, as by position.
        NamedValNode *named = (*litval)->tag == NamedValTag ? (NamedValNode*)*litval : NULL;
        if (named)
            litval = &named->val;
        // Coerce the value to the field's type rather than demanding an exact
        // match: a field takes a value on the same terms a variable initializer
        // does, including a union variant standing in for the union.
        if (!iexpCoerce(litval, field->vtype)) {
            iexpMismatchMsg((INode*)*litval, field->vtype, ErrorBadArray, "Literal value's type does not match expected field's type");
        }
        if (named)
            named->vtype = ((IExpNode*)named->val)->vtype;
        ++argi;
    }
    if (brands) {
        lifeBindEnd(brands);
        INode *branded = lifeBrandSubst((INode*)strnode, brands, (INode*)arrlit);
        if (branded != (INode*)strnode)
            arrlit->vtype = branded;
    }
}

// Check the list node
// Note:  We get here from FnCallTypeCheck, which has already checked that all arguments are expressions
// Perform data flow analysis on a type literal's field values.
// A literal initializes its fields from expressions, so each value is moved or
// copied exactly as a function call's argument is. A value given by field name
// is wrapped in a NamedValNode, which is unwrapped here so the decision is made
// about the value itself.
void typeLitFlow(FlowState *fstate, FnCallNode **nodep) {
    INode **argsp;
    uint32_t cnt;
    uint16_t inflight = fstate->inflightcnt;
    uint8_t keep = flowGateOperandsKeep(fstate);
    uint32_t i = 0;
    for (nodesFor((*nodep)->args, cnt, argsp)) {
        INode **valp = (*argsp)->tag == NamedValTag ? &((NamedValNode *)*argsp)->val : argsp;
        flowGateOperandNext(fstate, *valp);
        flowLoadValue(fstate, valp);
        flowHandleMoveOrCopy(valp);
        flowGateOperand(fstate, *valp);
        flowGateStatic(fstate, (INode *)*nodep, i++, *valp);
    }
    flowGateOperandsEnd(fstate, inflight, keep);
}

void typeLitTypeCheck(TypeCheckState *pstate, FnCallNode *arrlit) {

    INode *littype = itypeGetTypeDcl(arrlit->vtype);
    if (!itypeIsConcrete(arrlit->vtype))
        errorMsgNode((INode*)arrlit, ErrorInvType, "Type must be concrete and instantiable.");
    else if (littype->tag == StructTag) {
        // A struct's value is constructed by its name with parentheses,
        // 'Point(1, 2)'. An enum's variant is constructed likewise, 'Some(x)', an
        // empty one by its name alone; the brackets it was once written in,
        // 'Some[x]', give type arguments only, and are refused where they are,
        // naming the new spelling.
        // A '+' allocation's value, '+Rc-mut Node[1]',
        // is refused with the allocation, by allocateTypeCheck (ErrorPlusAlloc).
        // Refused here, at type check, so that a struct reached through an
        // alias or a type parameter is refused as one named directly. The
        // literal is still built, so nothing after it reports again.
        if (typeLitIsVariant((StructNode*)littype)) {
            if (!(arrlit->flags & FlagCtorParen)) {
                Name *vname = ((StructNode*)littype)->namesym;
                if (arrlit->args == NULL || arrlit->args->used == 0)
                    errorMsgNode((INode*)arrlit, ErrorVariantBracket,
                        "An empty variant is constructed by its name alone: write %s, not %s[]. Square brackets after a variant give a generic one its type arguments and never values, as in None[i32].",
                        &vname->namestr, &vname->namestr);
                else
                    errorMsgNode((INode*)arrlit, ErrorVariantBracket,
                        "A variant is constructed with parentheses: write %s(...), not %s[...]. Square brackets after a variant give a generic one its type arguments and never values, written before them, as in Some[i32](5).",
                        &vname->namestr, &vname->namestr);
            }
        }
        else if (!(arrlit->flags & (FlagNew | FlagAllocValue))) {
            Name *written = isNameUseNode(arrlit->objfn) ? ((NameUseNode*)arrlit->objfn)->namesym : ((StructNode*)littype)->namesym;
            errorMsgNode((INode*)arrlit, ErrorStructBracket,
                "A struct's value is constructed by its name, its init's arguments in parentheses: '%s(...)'.", &written->namestr);
        }
        typeLitStructCheck(pstate, arrlit, (StructNode*)littype);
    }
    // A number is not built from brackets: its conversion is a method,
    // 'u64.from(count)'. Refused here, at type check, so that a number reached
    // through an alias or a type parameter is refused as one named directly.
    else if (typeLitIsNbrType(littype)) {
        Name *written = isNameUseNode(arrlit->objfn) ? ((NameUseNode*)arrlit->objfn)->namesym : ((NbrNode*)littype)->namesym;
        errorMsgNode((INode*)arrlit, ErrorNbrBracket,
            "A number type takes no '[...]': a conversion is its method, %s.from(value)", &written->namestr);
    }
    else  // ArrayTag is dispatched in a different way and should never get here
        errorMsgNode((INode*)arrlit, ErrorBadArray, "Unknown type literal type for type checking");
}

// Can a declared init take these arguments after its 'self'? Viability only,
// as fnSigViableCall decides it for a call: the count, the defaults, and each
// argument passable to its parameter.
static int typeLitInitViable(FnDclNode *init, Nodes *args, int carries) {
    if (init->genericinfo)
        return 0;
    FnSigNode *sig = (FnSigNode*)init->vtype;
    uint32_t nparms = sig->parms->used - 1;
    if (args->used > nparms)
        return 0;
    INode **parmp = &nodesGet(sig->parms, 1);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(args, cnt, argsp)) {
        if (iexpMatches(argsp, ((IExpNode *)*parmp)->vtype, Coercion) == NoMatch)
            return 0;
        ++parmp;
    }
    for (uint32_t i = args->used; i < nparms; ++i) {
        // A CarriesRegion type's last parameter is the region, which a
        // construction naming none is handed the heap's (typeLitRegionArg)
        if (((VarDclNode *)*parmp++)->value == NULL && !(carries && i == nparms - 1))
            return 0;
    }
    return 1;
}

// Can the struct's implicit init, which takes its fields in the order they are
// declared, take these values by position, a field left out taking its default?
static int typeLitImplicitViable(StructNode *strnode, Nodes *args) {
    uint32_t argi = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&strnode->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (argi < args->used) {
            if (iexpMatches(&nodesGet(args, argi), field->vtype, Coercion) == NoMatch)
                return 0;
        }
        else if (field->value == NULL)
            return 0;
        ++argi;
    }
    return argi >= args->used;
}

// The region a construction of a CarriesRegion type is handed when it names
// none: the region of the arena it is made in ('new a[T](...)': 'hereregion', the
// head's region field, typeLitNewInRegion), else the heap's, core's
// 'heapRegion()'. Checked as written at the construction.
static INode *typeLitRegionArg(TypeCheckState *pstate, FnCallNode *node, INode *hereregion) {
    INode *arg;
    if (hereregion)
        arg = hereregion;
    else {
        FnDclNode *heap = heapRegionFn();
        if (heap == NULL)
            errorExit(ExitNF, "A CarriesRegion type's construction calls the core package's 'heapRegion', and the core package found declares none.");
        FnCallNode *call = newFnCallNode(newNameUseFromDclNode((INode*)heap, (INode*)node), 1);
        inodeLexCopy((INode*)call, (INode*)node);
        arg = (INode*)call;
    }
    inodeTypeCheck(pstate, &arg, unknownType);
    return arg;
}

// Coerce a declared init's arguments to its parameters after 'self', and
// append the defaults of those left out, as fnCallFinalizeArgs does for a call
static void typeLitInitArgs(TypeCheckState *pstate, FnCallNode *node, FnSigNode *sig, int carries, INode *hereregion) {
    INode **parmp = &nodesGet(sig->parms, 1);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(node->args, cnt, argsp)) {
        // (A string literal wanted as a read-only borrow of a type declaring
        // 'fromLiteral' is lent as a temporary of it: iexpCoerceIn.)
        if (!iexpCoerceIn(pstate, argsp, ((IExpNode *)*parmp)->vtype))
            iexpMismatchMsg(*argsp, ((IExpNode *)*parmp)->vtype, ErrorInvType, "Expression's type does not match declared parameter");
        ++parmp;
    }
    for (uint32_t i = node->args->used + 1; i < sig->parms->used; ++i) {
        // 'srcFile()' and 'srcLine()' answer where the construction is
        INode *dflt = ((VarDclNode *)nodesGet(sig->parms, i))->value;
        // A CarriesRegion type's last parameter, the region, left out: the arena
        // it is made in, else the heap's
        if (dflt == NULL && carries && i == sig->parms->used - 1)
            dflt = typeLitRegionArg(pstate, node, hereregion);
        else if (intrinsicIsSrcDefault(dflt))
            dflt = intrinsicSrcCallAt(dflt, (INode*)node);
        nodesAdd(&node->args, dflt);
    }
}

// Is this type's value constructed by an init: a struct that is neither a
// trait, an enum, nor an enum's variant?
static int typeLitHasInits(INode *typedcl) {
    return typedcl->tag == StructTag && !(typedcl->flags & TraitType)
        && !typeLitIsVariant((StructNode*)typedcl);
}

// The inits a struct declares, NULL if none, their signatures demanded so the
// arguments can be matched to them
static INode *typeLitDeclaredInits(StructNode *strnode) {
    INode *inits = iNsTypeFindFnField((INsTypeNode*)strnode, initMethodName);
    if (inits && inits->tag != FnDclTag && inits->tag != FnOverloadDclTag)
        return NULL;
    if (inits)
        fnCallDemandCandidates(inits);
    return inits;
}

// Is this construction's one argument, already checked, a finished value of
// the type being constructed: its type that type exactly, an alias resolved,
// with no coercion?
static int typeLitArgIsFinished(Nodes *args, INode *type) {
    if (args == NULL || args->used != 1)
        return 0;
    INode *arg = nodesGet(args, 0);
    return arg->tag != NamedValTag && isExpNode(arg) && !inodeIsError(arg)
        && itypeIsSame(((IExpNode*)arg)->vtype, type);
}

static void typeLitNewChecked(TypeCheckState *pstate, FnCallNode **nodep, int argschecked, INode *expectType);

// The value an allocation stores, from its parentheses, or NULL when they give
// none. A struct's value is constructed by one of its inits ('Rc[mut,
// Node](1)' constructs 'Node(1)'), unless its one argument is already a
// value of that struct, exactly, which is moved in. Any other type's value is
// not constructed: the one argument is the value, coerced to the type, so a
// number literal adopts it and a variant becomes its enum.
static INode *typeLitAllocValue(TypeCheckState *pstate, FnCallNode *node, INode *vtype) {
    INode *typedcl = itypeGetTypeDcl(vtype);
    Nodes *args = node->args;
    INode **argp = args && args->used == 1 && nodesGet(args, 0)->tag != NamedValTag ? &nodesGet(args, 0) : NULL;

    // A body whose length a reference carries has no value to construct or
    // move in: it is allocated by copying a borrow of one, 'Uni[str](view)',
    // into the memory the region gives, as long as the view
    if (itypeLenBodyElem(vtype)) {
        if (argp == NULL) {
            errorMsgNode((INode*)node, ErrorAllocValue,
                "%s has no value to construct: its allocation copies a borrow of one, 'Uni[%s](view)'.",
                itypeName(vtype), itypeName(vtype));
            return NULL;
        }
        // The elements are copied bit for bit, so each must be one that copies:
        // an owner of moving elements is made by moving them in (a List's 'freeze')
        if (itypeIsArrayBody(vtype) && itypeIsMove(itypeLenBodyElem(vtype))) {
            errorMsgNode((INode*)node, ErrorAllocValue,
                "%s is allocated by copying a borrow of its elements, and an element of this type moves, so it cannot be copied. Move them in: 'list.freeze()' makes a Uni[%s] of a List's elements.",
                itypeName(vtype), itypeName(vtype));
            return NULL;
        }
        INode *viewtype = (INode*)newRefNodeFull(RefTag, (INode*)node, borrowRef, newPermUseNode(roPerm), vtype);
        if (!itypeTypeCheck(pstate, &viewtype))
            return NULL;
        if (!iexpTypeCheckCoerce(pstate, viewtype, argp)) {
            errorMsgNode(*argp, ErrorInvType, "The value allocated is copied from a borrow of %s, '&%s'.",
                itypeName(vtype), itypeName(vtype));
            return NULL;
        }
        return isExpNode(*argp) ? *argp : NULL;
    }

    if (typeLitHasInits(typedcl)) {
        // The one argument is checked as the construction would check it, so
        // one that is not the struct's own value reaches the inits unchanged
        int argschecked = 0;
        if (argp) {
            INode *expect = unknownType;
            if (typeLitDeclaredInits((StructNode*)typedcl) == NULL) {
                FieldDclNode *field = fnCallTypeLitField((StructNode*)typedcl, args, 0);
                if (field)
                    expect = field->vtype;
            }
            inodeTypeCheck(pstate, argp, expect);
            argschecked = 1;
            if (typeLitArgIsFinished(args, vtype))
                return *argp;
        }
        FnCallNode *construct = newFnCallNode(vtype, 0);
        inodeLexCopy((INode*)construct, (INode*)node);
        construct->flags |= FlagNew;
        construct->args = args;
        typeLitNewChecked(pstate, &construct, argschecked, unknownType);
        return (INode*)construct;
    }

    if (argp == NULL) {
        errorMsgNode((INode*)node, ErrorAllocValue,
            "%s is not constructed by an init, so its allocation takes one value of it in the parentheses.", itypeName(vtype));
        return NULL;
    }
    if (!iexpTypeCheckCoerce(pstate, vtype, argp)) {
        errorMsgNode(*argp, ErrorInvType, "The value allocated is not of the type the allocation names: %s.", itypeName(vtype));
        return NULL;
    }
    return isExpNode(*argp) ? *argp : NULL;
}

static void typeLitNewAllocate(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype, INode *option, INode *value);

// 'Uni[fn(i32) i32](c)': an owner of a callable, made from one value that has a
// pub '()' of that signature, a closure or a struct of the author's. The value
// is moved into an allocation of its own type, and the owner of that is viewed
// as the callable's, as an owner of a trait is made from one of an implementer.
static void typeLitNewCallable(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype, INode *option) {
    FnCallNode *node = *nodep;
    StructNode *trait = (StructNode*)itypeGetTypeDcl(reftype->vtexp);
    char sig[300] = "";
    FnSigNode *csig = trait->callsig;
    Name *method = NULL;
    if (csig)
        genericFnSigCat(sig, sizeof(sig), csig);
    else
        snprintf(sig, sizeof(sig), "%s", &trait->namesym->namestr);
    INode *region = itypeGetTypeDcl(reftype->region);
    char *regname = region->tag == StructTag ? &((StructNode*)region)->namesym->namestr : "Uni";
    if (node->args == NULL || node->args->used != 1 || nodesGet(node->args, 0)->tag == NamedValTag) {
        errorMsgNode((INode*)node, ErrorCallableUse,
            "%s[%s] is made from one callable value, a closure or a struct with a pub `()` of that signature: '%s[%s](fn (...) { ... })'.",
            regname, sig, regname, sig);
        return;
    }
    INode **argp = &nodesGet(node->args, 0);
    // A closure literal given for a trait with one method fills that method
    if (csig == NULL) {
        uint32_t count;
        csig = closureTraitSig(trait, &method, &count);
        if (csig == NULL) {
            fnCallClosureTraitRefused(*argp, trait);
            *((INode**)nodep) = newErrorNode((INode*)node);
            return;
        }
    }
    if ((*argp)->tag == ClosureTag) {
        closureHint = csig;
        closureMethod = method;
        closureTrait = method ? trait : NULL;
    }
    inodeTypeCheck(pstate, argp, unknownType);
    closureHint = NULL;
    closureMethod = NULL;
    closureTrait = NULL;
    if (!isExpNode(*argp) || inodeIsError(*argp))
        return;
    INode *vtype = ((IExpNode*)*argp)->vtype;
    INode *vdcl = itypeGetTypeDcl(vtype);
    // A plain function: the reference to it is held by a struct that calls it
    if (vdcl->tag == RefTag && itypeGetTypeDcl(((RefNode*)vdcl)->vtexp)->tag == FnSigTag
        && itypeGetTypeDcl(((RefNode*)vdcl)->region) == (INode*)borrowRef) {
        FnSigNode *fsig = (FnSigNode*)itypeGetTypeDcl(((RefNode*)vdcl)->vtexp);
        // The function's signature meets the callable's as any callable's meets a
        // signature (fnSigMeets): the holder's '()' calls it with the callable's
        // own values, so only a recast of one layout is accepted
        if (trait->callsig == NULL || !fnSigMeets(fsig, trait->callsig)) {
            FnSigLayoutDiff diff;
            if (trait->callsig && fnSigLayoutDiff(fsig, trait->callsig, NULL, ReceiverNone, 1, &diff)) {
                char why[700];
                fnSigLayoutSpell(why, sizeof(why), &diff, "the callable", "the callable");
                errorMsgNode(*argp, ErrorFnRefByValue,
                    "%s[%s] is made from a callable of that signature, and this function is refused: %s Write a wrapper function that converts, and make that the callable.",
                    regname, sig, why);
                return;
            }
            errorMsgNode(*argp, ErrorCallableUse,
                "%s[%s] is made from a callable of that signature, and this function is another.", regname, sig);
            return;
        }
        StructNode *holder = closureFnHolder(pstate, trait->callsig, vtype, *argp);
        FnCallNode *make = newFnCallNode(newNameUseFromDclNode((INode*)holder, *argp), 1);
        inodeLexCopy((INode*)make, *argp);
        make->flags |= FlagNew;
        nodesAdd(&make->args, *argp);
        INode *made = (INode*)make;
        typeLitNewArgsChecked(pstate, (FnCallNode**)&made);
        *argp = made;
        if (!isExpNode(*argp) || inodeIsError(*argp))
            return;
        vtype = ((IExpNode*)*argp)->vtype;
        vdcl = itypeGetTypeDcl(vtype);
    }
    if (vdcl->tag != StructTag || (vdcl->flags & TraitType)) {
        errorMsgNode(*argp, ErrorCallableUse,
            "%s[%s] is made from a closure, a function or a struct with a pub `()` of that signature, and %s is none of them.",
            regname, sig, itypeName(vtype));
        return;
    }
    // The owner of the value itself, then that viewed as the callable
    INode *owner =(INode*)newRefNodeFull(RefTag, (INode*)node, reftype->region, reftype->perm, vtype);
    refNewTypeWritten = 1;
    int ownerok = itypeTypeCheck(pstate, &owner);
    refNewTypeWritten = 0;
    if (!ownerok)
        return;
    // A callable that holds nothing has no size: its allocation is the smallest block
    allocateZeroSizeOk = 1;
    typeLitNewAllocate(pstate, nodep, (RefNode*)owner, option, *argp);
    allocateZeroSizeOk = 0;
    if ((*nodep)->tag != AllocateTag)
        return;
    INode *made = (INode*)*nodep;
    if (!iexpCoerce((INode**)nodep, (INode*)reftype)) {
        int whycode;
        char *why = fnSigCallRefusal((INode*)owner, (INode*)reftype, &whycode);
        errorMsgNode(made, why ? whycode : ErrorCallableUse, "%s",
            why ? why : "That value has no pub `()` of the signature the owner is of.");
        *((INode**)nodep) = newErrorNode(made);
    }
}

// 'Rc[mut, Node](1)': an allocation in the region a managed reference type
// names, written out or through an alias of it ('Node(1)' for 'alias Node
// = Gc[mut, NodeValue]'). The parentheses are the value's (typeLitAllocValue):
// the arguments of the construction 'NodeValue(1)', its init chosen as any
// value's is, or a finished value, 'Rc[i32](5)'. The allocation node holds
// that value, so that generation runs an allocation's order: the init's
// arguments or the value, 'alloc', the region's init, the permission's, the
// value's init in place or the value's store (or an array's contents filled
// in place, typeLitNewFilled), the destination (genlallocref).
// 'trynew' types the allocation as an Option of the reference, None when
// memory runs out.
// 'value' is the value already made when the construction's contents give it
// (typeLitNewFilled), and NULL when the parentheses do.
static void typeLitNewAllocate(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype, INode *option, INode *value) {
    FnCallNode *node = *nodep;
    if (reftype->region == borrowRef) {
        errorMsgNode((INode*)node, ErrorNewType,
            "An allocation is made in a region, and a borrowed reference has none: it is made by borrowing a value, '&x'.");
        return;
    }
    if (reftype->tag == VirtRefTag) {
        StructNode *calltrait = (StructNode*)itypeGetTypeDcl(reftype->vtexp);
        INode *only = node->args && node->args->used == 1 ? nodesGet(node->args, 0) : NULL;
        if (calltrait->tag == StructTag && value == NULL && (calltrait->callsig || (only && only->tag == ClosureTag))) {
            typeLitNewCallable(pstate, nodep, reftype, option);
            return;
        }
        errorMsgNode((INode*)node, ErrorNewType,
            "A virtual reference refers to a trait, which has no value to construct: allocate a type implementing it, as 'Rc[mut, Rect](...)', and the reference coerces where the virtual one is wanted.");
        return;
    }

    // A collector traces an object by its type record, which describes one
    // element of a body, so a body whose elements hold traced references would
    // have only its first one found
    if (itypeIsArrayBody(reftype->vtexp) && regionIsTraced(reftype->region)
        && itypeHoldsTraced(itypeLenBodyElem(reftype->vtexp))) {
        errorMsgNode((INode*)node, ErrorAllocValue,
            "%s is traced by the type record of one element, so a traced region cannot own an array of elements that hold traced references: only its first would be found.",
            itypeName(reftype->vtexp));
        return;
    }

    if (value == NULL)
        value = typeLitAllocValue(pstate, node, reftype->vtexp);
    if (value == NULL)
        return;

    RefNode *alloc = newRefNode(AllocateTag);
    inodeLexCopy((INode*)alloc, (INode*)node);
    alloc->region = reftype->region;
    alloc->perm = reftype->perm;
    alloc->vtexp = value;
    // Typed as written, so a rule about the reference type is judged once
    alloc->vtype = (INode*)reftype;
    if (option) {
        FnCallNode *opttype = newFnCallNode(option, 1);
        inodeLexCopy((INode*)opttype, (INode*)node);
        nodesAdd(&opttype->args, (INode*)reftype);
        alloc->flags |= FlagQues;
        alloc->vtype = (INode*)opttype;
    }
    *((INode**)nodep) = (INode*)alloc;
    allocateValueCheck(pstate, (RefNode**)nodep);
}

// 'Point(1, 2)': construct a struct's value by one of its inits, which the
// arguments select. Every struct has an implicit init taking its fields in
// declaration order (by name too, 'Point(y: 2, x: 1)', and a field left out
// taking its default), and it may declare others, 'fn init(self: &new, ...)',
// overloaded under the name 'init'. A declared init that takes the arguments
// is preferred to the implicit one, and exactly one declared init may take
// them; named ones are the implicit init's alone, since no call takes them,
// so the fields' names reach it whatever the struct declares. The implicit init is lowered to the struct's literal, whose fields are
// stored straight into wherever the value goes; a declared one to a call of it,
// which generation hands the memory the value goes into (genlNew).
void typeLitNewCheck(TypeCheckState *pstate, FnCallNode **nodep, INode *expectType) {
    typeLitNewChecked(pstate, nodep, 0, expectType);
}

// The same for a construction whose arguments are already checked (a closure's
// hidden struct is made from the values of its state list and borrows of the
// variables it names, closure.c)
void typeLitNewArgsChecked(TypeCheckState *pstate, FnCallNode **nodep) {
    typeLitNewChecked(pstate, nodep, 1, unknownType);
}

// 'Rc[mut, Array[i32, 4]]() <- fill 0': the allocation, of the reference
// type 'reftype' the construction names (already checked), whose value is the
// array literal of its contents, 'lit' (contentsLowerAlloc). The allocation
// fills it in place once the memory is had (FlagAllocFill, genlallocref), so
// with 'trynew' it is filled only when the memory was had. Answers the
// allocation, or NULL when it was refused, reported.
INode *typeLitNewFilled(TypeCheckState *pstate, FnCallNode **nodep, RefNode *reftype, INode *lit) {
    FnCallNode *node = *nodep;
    INode *option = NULL;
    if (node->flags & FlagTryNew) {
        option = node->methfld;
        node->methfld = NULL;
    }
    typeLitNewAllocate(pstate, nodep, reftype, option, lit);
    if ((*nodep)->tag != AllocateTag)
        return NULL;
    (*nodep)->flags |= FlagAllocFill;
    return (INode*)*nodep;
}

// Is this name-resolved head a VALUE: a variable, or a field reached through
// one ('a', 'self.arena')? A module's name, or a type's, is not.
static int typeLitIsValueHead(INode *head) {
    if (isNameUseNode(head)) {
        INode *dcl = nameUseGetDcl((NameUseNode*)head);
        return dcl != NULL && dcl->tag == VarDclTag;
    }
    if (head->tag == FnCallTag) {
        FnCallNode *path = (FnCallNode*)head;
        return path->methfld != NULL && path->args == NULL && !(path->flags & (FlagIndex | FlagOperator))
            && typeLitIsValueHead(path->objfn);
    }
    return 0;
}

// Does the type a 'new' is written before name a DYNAMIC region, a region whose
// state is a value: 'a[Node]', a variable or a path of fields followed by the
// brackets that name the type made in it? That is the one construction 'new'
// is written before. A static region is named by a type (an owner such as
// 'Rc[mut, Node]'), and a type's value is made by writing the type's name with
// parentheses. Name resolved; whether the value is a region is typeLitNewInRegion's
// to ask once it is type checked.
int typeLitNewNamesRegionValue(INode *type) {
    if (type == NULL || type->tag != FnCallTag)
        return 0;
    FnCallNode *index = (FnCallNode*)type;
    return (index->flags & FlagIndex) && index->methfld == NULL && index->args != NULL
        && index->args->used > 0 && typeLitIsValueHead(index->objfn);
}

// 'new a[Node](1)', 'new a[mut, Node](1)': a construction whose head is a region
// VALUE, an arena, then the type in brackets (WI type-expressions-gc-collections-
// constructors, 30 Sep). It means 'a.alloc(Node(1))': the value is made as
// any is and moved into the arena, and the construction is whatever that arena's
// 'alloc' answers -- a scoped '&mut Node' of an Arena, a key '&'=a mut Node' of a
// DynArena. The permission may be left out or written 'mut', which is what
// 'alloc' lends.
//
// The head must be a REGION VALUE (typeLitRegionField): a value whose type is a
// struct whose first field is a '*MemRegion', the region things made in it
// draw their memory from. A type declaring CarriesRegion that is made here is
// handed that region when its construction names none (the head's first field,
// read at the construction: 'hereregion' of typeLitNewChecked).
static int typeLitNewInRegion(TypeCheckState *pstate, FnCallNode **nodep, int hasoption, INode *expectType);

// The first field of this value type's struct when that is a '*MemRegion' and
// public, else NULL: what makes the value a region. A reference to one is the
// region it refers to.
static FieldDclNode *typeLitRegionField(INode *vtype) {
    if (vtype == NULL || inodeIsError(vtype))
        return NULL;
    INode *dcl = itypeGetDerefTypeDcl(vtype);
    if (dcl == NULL || dcl->tag != StructTag || (dcl->flags & (TraitType | EnumType)))
        return NULL;
    StructNode *strnode = (StructNode*)dcl;
    if (strnode->fields.used == 0)
        return NULL;
    FieldDclNode *field = (FieldDclNode*)nodelistGet(&strnode->fields, 0);
    INode *type = field->vtype;
    if (type == NULL || type->tag != PtrTag || !(field->flags & FlagPub))
        return NULL;
    INode *pointee = ((StarNode*)type)->vtexp;
    INode *pdcl = pointee && isNameUseNode(pointee) ? nameUseGetDcl((NameUseNode*)pointee) : pointee;
    return coreIsRegion(pdcl) ? field : NULL;
}

// Does this value's type, a reference to it too, lead to a region: declare an
// 'allocRegion' method, giving the region value the allocation is made in?
// 'new x[T](...)' with x such a value is 'x.allocRegion()' as the head, and so
// 'x.allocRegion().alloc(T(...))'. An actor's state declares one, which the
// compiler generates in a module that imports the actors' collector, so that
// 'new self[T](...)' allocates in the actor's own region.
static int typeLitLeadsToRegion(INode *vtype) {
    if (vtype == NULL || inodeIsError(vtype))
        return 0;
    INode *dcl = itypeGetDerefTypeDcl(vtype);
    if (dcl == NULL || dcl->tag != StructTag || (dcl->flags & (TraitType | EnumType)))
        return 0;
    INode *member = iNsTypeFindFnField((INsTypeNode*)dcl, allocRegionMethodName);
    return member != NULL && member->tag == FnDclTag;
}

static int typeLitNewInRegion(TypeCheckState *pstate, FnCallNode **nodep, int hasoption, INode *expectType) {
    FnCallNode *node = *nodep;
    if (!typeLitNewNamesRegionValue(node->objfn))
        return 0;
    FnCallNode *index = (FnCallNode*)node->objfn;
    if (hasoption) {
        errorMsgNode((INode*)node, ErrorRegionNew,
            "'trynew' is refused in a region: whether an allocation there can fail is not settled. Write 'new a[T](...)'.");
        return 1;
    }
    // The head, checked on a copy so the real one is checked once, in the call
    // this lowers to: is its type a region?
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
    INode *probe = cloneNode(&cstate, index->objfn);
    clonePopState();
    cloneDclPop(dclpos);
    ErrorTrial trial;
    errorTrialBegin(&trial, closureSerialNow());
    inodeTypeCheck(pstate, &probe, unknownType);
    int probeok = errorTrialEnd(&trial) && isExpNode(probe) && !inodeIsError(probe);
    genericTrialPurge();
    FieldDclNode *regionfld = probeok ? typeLitRegionField(((IExpNode*)probe)->vtype) : NULL;
    // A value that is no region but leads to one ('allocRegion', as an actor's
    // state has) is made the head's region: 'x.allocRegion()' is the head from
    // here on, and is asked the same
    if (probeok && regionfld == NULL && typeLitLeadsToRegion(((IExpNode*)probe)->vtype)) {
        FnCallNode *lead = newFnCallLower((INode*)node, index->objfn, 1);
        lead->methfld = (INode*)newMemberUseNode(allocRegionMethodName);
        inodeLexCopy(lead->methfld, (INode*)node);
        index->objfn = (INode*)lead;
        dclpos = cloneDclPush();
        clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
        probe = cloneNode(&cstate, index->objfn);
        clonePopState();
        cloneDclPop(dclpos);
        errorTrialBegin(&trial, closureSerialNow());
        inodeTypeCheck(pstate, &probe, unknownType);
        probeok = errorTrialEnd(&trial) && isExpNode(probe) && !inodeIsError(probe);
        genericTrialPurge();
        regionfld = probeok ? typeLitRegionField(((IExpNode*)probe)->vtype) : NULL;
    }
    if (probeok && regionfld == NULL) {
        errorMsgNode(index->objfn, ErrorRegionHead,
            "'new a[T](...)' allocates in a region, and this is a %s, which is not one. A region is a struct whose first field is a public '*MemRegion', as an arena's is; or a value whose type has an 'allocRegion' method giving one (every actor's state has one).",
            itypeName(((IExpNode*)probe)->vtype));
        return 1;
    }
    INode *perm = NULL;
    INode *type = nodesGet(index->args, 0);
    if (index->args->used == 2) {
        perm = type;
        type = nodesGet(index->args, 1);
    }
    else if (index->args->used > 2) {
        errorMsgNode((INode*)index, ErrorRegionNew,
            "An allocation in an arena names the type it makes in brackets after the arena, with a permission before it if one is written: 'new a[Node](...)', 'new a[mut, Node](...)'.");
        return 1;
    }
    if (perm && !(nameUseNames(perm, PermTag) && nameUseGetDcl((NameUseNode*)perm) == (INode*)mutPerm)) {
        errorMsgNode(perm, ErrorRegionNew,
            "An arena lends what it allocates mutably, so the permission of 'new a[perm, T](...)' is 'mut', or left out.");
        return 1;
    }
    if (nameUseNames(type, PermTag)) {
        errorMsgNode(type, ErrorRegionNew,
            "An allocation in an arena names the type it makes after the arena: 'new a[Node](...)'.");
        return 1;
    }
    // 'a.alloc(T(args))'
    FnCallNode *ctor = newFnCallLower((INode*)node, type, 0);
    ctor->flags |= FlagNew;
    ctor->args = node->args;
    // What a CarriesRegion type made here is handed when it names none: the
    // head's region field, 'a.region' (read from a copy of the head: the head
    // is a variable or a path of fields, so reading it twice runs nothing)
    if (regionfld) {
        dclpos = cloneDclPush();
        clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
        INode *headcopy = cloneNode(&cstate, index->objfn);
        clonePopState();
        cloneDclPop(dclpos);
        FnCallNode *regionof = newFnCallLower((INode*)node, headcopy, 0);
        regionof->methfld = (INode*)newMemberUseNode(regionfld->namesym);
        inodeLexCopy(regionof->methfld, (INode*)node);
        ctor->methfld = (INode*)regionof;
    }
    FnCallNode *call = newFnCallLower((INode*)node, index->objfn, 1);
    call->methfld = (INode*)newMemberUseNode(allocMethodName);
    inodeLexCopy(call->methfld, (INode*)node);
    nodesAdd(&call->args, (INode*)ctor);
    *((INode**)nodep) = (INode*)call;
    inodeTypeCheck(pstate, (INode**)nodep, expectType);
    return 1;
}

// typeLitNewCheck, its arguments already checked when 'argschecked' (an
// allocation's one argument, checked to see whether it is a finished value)
static void typeLitNewChecked(TypeCheckState *pstate, FnCallNode **nodep, int argschecked, INode *expectType) {
    FnCallNode *node = *nodep;
    // Already lowered to its declared init's call, and reached again
    if (nameUseNames(node->objfn, FnDclTag))
        return;
    node->vtype = errorType;    // until a value is known to come of it
    // 'trynew': the bound 'Option' name resolution left in methfld
    INode *option = NULL;
    // 'new a[T](...)' made this construction with the region of 'a' in methfld
    // (typeLitNewInRegion): what a CarriesRegion type is handed when it names none
    INode *hereregion = NULL;
    if (node->flags & FlagTryNew) {
        option = node->methfld;
        node->methfld = NULL;
    }
    else if (node->methfld) {
        hereregion = node->methfld;
        node->methfld = NULL;
    }

    // 'new a[Node](1)': the head is an arena, a value
    if (typeLitNewInRegion(pstate, nodep, option != NULL, expectType))
        return;

    if (!isTypeNode(node->objfn)) {
        // 'Plain[mut, i32](5)': a permission given to what is no region,
        // refused as it is in a type
        if (node->objfn->tag == FnCallTag && fnCallRefusePermArg((FnCallNode**)&node->objfn))
            return;
        Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : NULL;
        errorMsgNode(node->objfn, ErrorNewType, "A construction is of a type, and %s is not one.",
            written ? &written->namestr : "this");
        return;
    }

    INode **argsp;
    uint32_t cnt;
    // A generic struct named without its type arguments, 'Box(5i64)', has
    // them inferred from the values its fields are given, as its literal did:
    // the arguments are checked first, with no expectation, and the instance
    // they infer replaces the name. What they leave unsaid is read off the type
    // the construction is wanted as ('let b: Box[i64] = Box(5)')
    if (!argschecked && isNameUseNode(node->objfn) && nameUseNames(node->objfn, StructTag)
        && genericGetInfo(nameUseGetDcl((NameUseNode*)node->objfn)) != NULL) {
        for (nodesFor(node->args, cnt, argsp))
            inodeTypeCheck(pstate, argsp, unknownType);
        argschecked = 1;
        if (genericSubstitute(pstate, nodep, expectType))
            return;
        node = *nodep;
    }
    refNewTypeWritten = 1;
    int typeok = itypeTypeCheck(pstate, &node->objfn);
    refNewTypeWritten = 0;
    if (!typeok)
        return;

    INode *typedcl = itypeGetTypeDcl(node->objfn);
    Name *written = isNameUseNode(node->objfn) ? ((NameUseNode*)node->objfn)->namesym : NULL;
    if (typedcl->tag == RefTag || typedcl->tag == VirtRefTag) {
        typeLitNewAllocate(pstate, nodep, (RefNode*)typedcl, option, NULL);
        return;
    }
    if (option) {
        errorMsgNode((INode*)node, ErrorTryNewValue,
            "'trynew' is an allocation in a region, which may run out of memory, and %s is not a managed reference type: a value is constructed by its name, '%s(...)'.",
            written ? &written->namestr : "this type", written ? &written->namestr : "T");
        return;
    }
    if (typeLitIsNbrType(typedcl)) {
        errorMsgNode((INode*)node, ErrorNewType,
            "A number is not constructed: it is written as a literal, or converted with its method, %s.from(value).",
            written ? &written->namestr : &((NbrNode*)typedcl)->namesym->namestr);
        return;
    }
    // An array's contents are its construction (contentsLowerArray), which
    // gives them after '<-'
    if (typedcl->tag == ArrayTag) {
        errorMsgNode((INode*)node, ErrorArrayContents,
            "An array is constructed with the values that fill it, after '<-': 'Array[f32, 4]() <- fill 0.0'.");
        return;
    }
    if (typedcl->tag != StructTag || (typedcl->flags & TraitType)) {
        errorMsgNode((INode*)node, ErrorNewType, "Only a struct's value is constructed, and %s is not a struct.",
            written ? &written->namestr : "this type");
        return;
    }
    StructNode *strnode = (StructNode*)typedcl;
    if (typeLitIsVariant(strnode)) {
        errorMsgNode((INode*)node, ErrorNewType,
            "%s is an enum's variant, which is constructed by its name: '%s(...)', and an empty one by its name alone.",
            &strnode->namesym->namestr, &strnode->namesym->namestr);
        return;
    }

    // An actor's state exists only inside its actor: it is constructed only as
    // the actor is made, by its handle's initializer, which constructs it in the
    // actor's block (parseactor.c), and by an init the state declares filling
    // that same state, '*self = Self(field: value)' (typeLitSelfFill). Made
    // anywhere else -- 'Self(...)' in one of its methods, by its fields'
    // names or by an init -- it would be a state in no actor, from which
    // 'selfactor' and a send to itself make a handle to nothing
    ActorInfo *actor = actorOfState((INode*)strnode);
    int making = actor && pstate->fn && inodeGetOwner((INode*)pstate->fn) == (INode*)actor->handle;
    if (actor && !making && (INode*)node != typeLitSelfFill) {
        errorMsgNode((INode*)node, ErrorActorStateInit,
            "Actor %s's state exists only inside its actor, so it is constructed only as the actor is made ('%s(...)'), where its init fills it, '*self = Self(field: value)'. Constructed here, it would be a state in no actor, and a send it made to its actor would reach none. For a value of this shape, declare a plain struct.",
            &actor->handle->namesym->namestr, &actor->handle->namesym->namestr);
        return;
    }

    // The inits it declares, whose signatures are wanted before the arguments
    // are matched to them
    INode *inits = typeLitDeclaredInits(strnode);

    // Each argument is checked against the field it fills when the implicit
    // init is the only one, as a literal's values are; otherwise with no
    // expectation, as an overload set's arguments are
    uint32_t argi = 0;
    int named = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        INode *expect = unknownType;
        if (inits == NULL) {
            FieldDclNode *field = fnCallTypeLitField(strnode, node->args, argi);
            if (field)
                expect = field->vtype;
        }
        if (!argschecked)
            inodeTypeCheck(pstate, argsp, expect);
        if ((*argsp)->tag == NamedValTag)
            named = 1;
        ++argi;
    }
    int badarg = 0;
    for (nodesFor(node->args, cnt, argsp)) {
        if (!isExpNode(*argsp)) {
            errorMsgNode(*argsp, ErrorNotTyped, "Expected a typed expression.");
            badarg = 1;
        }
        else if (inodeIsError(*argsp))
            badarg = 1;
    }
    if (badarg)
        return;

    // A finished value of the struct is not constructed again: an allocation
    // takes it as its value ('Rc[Point](p)', typeLitAllocValue), and
    // anywhere else it is simply used
    if (typeLitArgIsFinished(node->args, node->objfn)) {
        errorMsgNode((INode*)node, ErrorNewFinished,
            "This is already a value of %s, which needs no construction: use it as it is, or allocate it with 'R[perm, %s](value)'.",
            &strnode->namesym->namestr, &strnode->namesym->namestr);
        return;
    }

    // A name none of the fields has is meant for an init the struct declares,
    // which takes its arguments by position, as every call does
    if (named && inits) {
        for (nodesFor(node->args, cnt, argsp)) {
            if ((*argsp)->tag != NamedValTag)
                continue;
            Name *name = ((NameUseNode*)((NamedValNode*)*argsp)->name)->namesym;
            int isfield = 0;
            INode **fieldp;
            uint32_t fcnt;
            for (nodelistFor(&strnode->fields, fcnt, fieldp))
                if (((FieldDclNode*)*fieldp)->namesym == name)
                    isfield = 1;
            if (!isfield) {
                namedValRefuseArgs(node->args, "a call of an init a struct declares");
                return;
            }
        }
    }

    // A type declaring CarriesRegion takes the region its memory comes from as
    // its inits' last argument, which a construction that names none leaves out
    int carries = structDeclaresTrait(strnode, carriesRegionTrait);

    // Select the one init that takes the arguments
    FnDclNode *selected = NULL;
    uint32_t viable = 0;
    if (inits && !named) {
        INode **candp;
        uint32_t ncand;
        if (inits->tag == FnDclTag) {
            candp = &inits;
            ncand = 1;
        }
        else {
            candp = &nodesGet(((FnOverloadDclNode*)inits)->overloads, 0);
            ncand = ((FnOverloadDclNode*)inits)->overloads->used;
        }
        INode **firstp = candp;
        uint32_t allcand = ncand;
        while (ncand--) {
            FnDclNode *cand = (FnDclNode*)*candp++;
            if (fnDclIsInit(cand) && typeLitInitViable(cand, node->args, carries)) {
                selected = cand;
                ++viable;
            }
        }
        // Neither a declared init nor the implicit one takes the arguments,
        // and one is a string literal: ask the declared inits again with a
        // literal also lent as a temporary to a '&T' ('File("x")' for
        // 'init(self: &new, p: &Path)'), as a call's selection falls back to it
        if (viable == 0 && !typeLitImplicitViable(strnode, node->args) && slitAnyText(NULL, node->args)) {
            slitBorrowFallback(1);
            candp = firstp;
            ncand = allcand;
            while (ncand--) {
                FnDclNode *cand = (FnDclNode*)*candp++;
                if (fnDclIsInit(cand) && typeLitInitViable(cand, node->args, carries)) {
                    selected = cand;
                    ++viable;
                }
            }
            slitBorrowFallback(0);
        }
    }
    // A declared init the arguments select is preferred to the implicit one,
    // which is reached by its fields' names when both would take them
    int implicit = viable == 0 && (inits == NULL || named || typeLitImplicitViable(strnode, node->args));
    viable += implicit;
    if (viable != 1) {
        errorMsgNode((INode*)node, ErrorInitNone, viable == 0
            ? "No init of %s takes these arguments: neither its fields, in the order they are declared, nor an init it declares."
            : "More than one init %s declares takes these arguments, so the construction cannot choose between them.",
            &strnode->namesym->namestr);
        return;
    }

    // The implicit init: the struct's literal
    if (implicit) {
        node->tag = TypeLitTag;
        node->vtype = node->objfn;
        typeLitTypeCheck(pstate, node);
        return;
    }

    // A declared init, called with the memory to fill. One not declared 'pub'
    // is its module's.
    if (inodeIsPrivate(inits) && !structSeesPrivate(pstate, (INode*)strnode)) {
        errorMsgNode((INode*)node, ErrorNotPublic,
            "May not construct %s with its private init: only code in the module that declares it may, unless it is declared 'pub fn init'.",
            &strnode->namesym->namestr);
        return;
    }
    INode *type = node->objfn;
    node->objfn = newNameUseFromDclNode((INode*)selected, (INode*)node);
    typeLitInitArgs(pstate, node, (FnSigNode*)selected->vtype, carries, hereregion);
    node->vtype = type;
}
