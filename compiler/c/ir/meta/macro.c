/** Handling for macro declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Create a new macro declaraction node
MacroDclNode *newMacroDclNode(Name *namesym) {
    MacroDclNode *gennode;
    newNode(gennode, MacroDclNode, MacroDclTag);
    gennode->vtype = NULL;
    gennode->namesym = namesym;
    gennode->typeparms = NULL;
    gennode->parms = newNodes(4);
    gennode->body = NULL;
    gennode->memonodes = newNodes(4);
    return gennode;
}

// Deep copy a macro declaration. A generic type's instance takes a copy of every
// member, and a macro declared in the type is one of them.
//
// The body's uses of the macro's own parameters must survive as uses of the
// copy's parameters: cloning a use substitutes whatever its name is hooked to,
// so each parameter is hooked to its own copy before the body is cloned, and
// cloneNode copies a use hooked that way as a use rather than substituting.
// The enclosing type's parameters are hooked to the instance's type arguments by
// the caller, so those are substituted, and the instance's macro body names the
// instance's types. So do its parameters' types: 'macro put[self, x: T]' in
// 'Cell[T]' takes an 'x: i64' in 'Cell[i64]'. The macro's own type parameters
// are hooked to their copies first, so a parameter's type naming one names the
// copy's.
static GenVarDclNode *cloneMacroParm(CloneState *cstate, GenVarDclNode *old) {
    GenVarDclNode *parm = newGVarDclNode(old->namesym);
    copyNodeLex(parm, old);
    parm->flags = old->flags;
    if (old->valtype)
        parm->valtype = cloneNode(cstate, old->valtype);
    nametblHookNode(parm->namesym, (INode*)parm);
    return parm;
}

INode *cloneMacroDclNode(CloneState *cstate, MacroDclNode *node) {
    MacroDclNode *newnode = memAllocBlk(sizeof(MacroDclNode));
    memcpy(newnode, node, sizeof(MacroDclNode));

    INode **nodesp;
    uint32_t cnt;
    nametblHookPush();
    if (node->typeparms) {
        newnode->typeparms = newNodes(node->typeparms->used);
        for (nodesFor(node->typeparms, cnt, nodesp))
            nodesAdd(&newnode->typeparms, (INode*)cloneMacroParm(cstate, (GenVarDclNode*)*nodesp));
    }
    newnode->parms = newNodes(node->parms->used);
    for (nodesFor(node->parms, cnt, nodesp))
        nodesAdd(&newnode->parms, (INode*)cloneMacroParm(cstate, (GenVarDclNode*)*nodesp));
    newnode->body = cloneNode(cstate, node->body);
    nametblHookPop();
    return (INode*)newnode;
}

// Serialize
void macroPrint(MacroDclNode *name) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("macro %s ", &name->namesym->namestr);
    if (name->typeparms) {
        inodeFprint("[");
        for (nodesFor(name->typeparms, cnt, nodesp)) {
            inodePrintNode(*nodesp);
            if (cnt > 1)
                inodeFprint(", ");
        }
        inodeFprint("]");
    }
    inodeFprint("(");
    for (nodesFor(name->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint(") ");
    inodePrintNode(name->body);
}

// The trait a macro parameter's resolved type names, 'a: Ordered' or an
// instance of a generic trait, 'a: Stack[i64]', or NULL. A trait is open: what
// a parameter typed by one would accept is a generic constraint's question,
// not built for a macro (Jon, 26 Oct 2026). An enum is a closed family, a type
// whose values a parameter holds, and a reference to a trait, 'a: &Shape', is a
// concrete type: neither is one.
static StructNode *macroParmTrait(INode *type) {
    if (type->tag == FnCallTag)
        type = ((FnCallNode*)type)->objfn;
    if (!isNameUseNode(type))
        return NULL;
    INode *dcl = nameUseGetDcl((NameUseNode*)type);
    if (dcl == NULL || dcl->tag != StructTag || !(dcl->flags & TraitType) || (dcl->flags & EnumType))
        return NULL;
    return (StructNode*)dcl;
}

// ---------------------------------------------------------------------------
// A plain parameter changed by its macro's body, refused where the macro is
// declared. A plain parameter is evaluated once, into a variable nothing may
// change (macroExpand), so a body that changes it wanted it 'lazy'. Name
// resolution sees which parameter a change names, and that is enough where no
// type is in question: an assignment, swap or '&mut' of the parameter itself,
// or of a part of it reached through no reference ('p.a = 0' for 'p: Pair'),
// is refused whatever the macro is given, so an unused macro is refused too.
// Where the answer waits on a type -- a step of the path that is a reference,
// a type parameter, an index into anything but an array value, a method --
// the change is judged where the macro is used, by the checks of a store, a
// swap or a borrow (macroParmAssignRefused), as before.

// The macro whose body is being resolved, or NULL
static MacroDclNode *macroResolving = NULL;

#define MacroParmAssignMsg "The macro's parameter %s is evaluated once, so its body cannot change it: an argument the macro assigns to must be lazy, '%s: lazy T'."

// The plain parameter of the macro being resolved that this name use names, or NULL
static GenVarDclNode *macroPlainParmOf(INode *node) {
    if (macroResolving == NULL || !isNameUseNode(node))
        return NULL;
    INode *dcl = ((NameUseNode*)node)->dclnode;
    if (dcl == NULL || dcl->tag != GenVarDclTag)
        return NULL;
    GenVarDclNode *parm = (GenVarDclNode*)dcl;
    if ((parm->flags & FlagLazy) || parm->valtype == NULL || parm->valtype == unknownType)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(macroResolving->parms, cnt, nodesp))
        if (*nodesp == dcl)
            return parm;
    return NULL;
}

// The declaration of a type, as written, that holds its value in place and is
// no reference: a number, a struct (not a trait, not a generic one, whose
// fields wait on its type arguments), or an array value. NULL for anything
// else, or a name not resolved yet.
static INode *macroValueTypeDcl(INode *type) {
    if (type == NULL)
        return NULL;
    if (type->tag == ArrayTag)
        return type;
    if (!isNameUseNode(type))
        return NULL;
    INode *dcl = ((NameUseNode*)type)->dclnode;
    if (dcl == NULL)
        return NULL;
    switch (dcl->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag:
        return dcl;
    case StructTag:
        if (((dcl->flags & TraitType) && !(dcl->flags & EnumType)) || ((StructNode*)dcl)->genericinfo)
            return NULL;
        return dcl;
    default:
        return NULL;
    }
}

// The plain parameter whose own storage 'place' is, reached through no
// reference: the parameter, a field of a struct value that is, or an element
// of an array value that is. '*typep' is the place's type as written. NULL
// where it is not, or where saying so waits on a type.
static GenVarDclNode *macroPlaceParm(INode *place, INode **typep, int depth) {
    if (place == NULL || depth > 32)
        return NULL;
    if (isNameUseNode(place)) {
        GenVarDclNode *parm = macroPlainParmOf(place);
        if (parm)
            *typep = parm->valtype;
        return parm;
    }
    if (place->tag != FnCallTag)
        return NULL;
    FnCallNode *call = (FnCallNode*)place;
    INode *outer = NULL;
    // A field, 'p.a'
    if (call->methfld && isNameUseNode(call->methfld) && call->args == NULL && !(call->flags & FlagIndex)) {
        GenVarDclNode *parm = macroPlaceParm(call->objfn, &outer, depth + 1);
        INode *dcl = macroValueTypeDcl(outer);
        if (parm == NULL || dcl == NULL || dcl->tag != StructTag)
            return NULL;
        INode *fld = iNsTypeFindFnField((INsTypeNode*)dcl, ((NameUseNode*)call->methfld)->namesym);
        if (fld == NULL || fld->tag != FieldDclTag)
            return NULL;
        *typep = ((FieldDclNode*)fld)->vtype;
        return parm;
    }
    // An element of an array value, 'a[i]'
    if (call->methfld == NULL && (call->flags & FlagIndex) && !(call->flags & FlagRange)
        && call->args && call->args->used == 1) {
        GenVarDclNode *parm = macroPlaceParm(call->objfn, &outer, depth + 1);
        if (parm == NULL || outer == NULL || outer->tag != ArrayTag)
            return NULL;
        *typep = arrayElemType(outer);
        return parm;
    }
    return NULL;
}

// 'place', in the body of the macro being resolved, is stored to, swapped or
// borrowed '&mut' (MacroChangePlace), or changed in place by an operator
// such as '+=', which reaches through a reference its operand is
// (MacroChangeInPlace). Refused at 'at' where it is a plain parameter's own
// storage, and so says nothing that waits on a type.
void macroChangeNameRes(INode *at, INode *place, MacroChange how) {
    if (macroResolving == NULL || place == NULL)
        return;
    if (place->tag == VTupleTag || place->tag == TupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)place)->elems, cnt, nodesp))
            macroChangeNameRes(*nodesp, *nodesp, how);
        return;
    }
    INode *type = NULL;
    GenVarDclNode *parm = macroPlaceParm(place, &type, 0);
    if (parm == NULL)
        return;
    if (how == MacroChangeInPlace && macroValueTypeDcl(type) == NULL)
        return;
    errorMsgNode(at, ErrorMacroParmAssign, MacroParmAssignMsg, &parm->namesym->namestr, &parm->namesym->namestr);
}

// Perform name resolution
void macroNameRes(NameResState *pstate, MacroDclNode *gennode) {
    uint16_t oldscope = pstate->scope;
    pstate->scope = 1;

    INode **nodesp;
    uint32_t cnt;

    // A macro method's body may name its type's members only through 'self';
    // nameUseNameRes refuses a bare one while this is set
    INode *svmacromethod = pstate->macromethod;
    pstate->macromethod = (gennode->flags & FlagMethFld) ? (INode*)gennode : NULL;
    // A macro's body is expanded wherever it is used, an importer included, so
    // what it names is marked for a library compile to export (nameUseNameRes)
    INode *svexpander = pstate->expander;
    pstate->expander = (INode*)gennode;

    // Hook gennode's parameters into global name table
    // so that when we walk the gennode's logic, parameter names are resolved.
    // Resolving a parameter hooks it, so this has to happen inside the push --
    // before it, the parameter would bind in the enclosing scope and the
    // matching pop would never remove it.
    nametblHookPush();
    // The type parameters first, so a value parameter's type may name one.
    // A parameter's type is resolved where the macro is declared, as its body
    // is, and inside the expander's window, so what it names is exported with
    // the macro (an importer's include file declares it).
    if (gennode->typeparms) {
        for (nodesFor(gennode->typeparms, cnt, nodesp))
            inodeNameRes(pstate, nodesp);
    }
    for (nodesFor(gennode->parms, cnt, nodesp)) {
        GenVarDclNode *parm = (GenVarDclNode*)*nodesp;
        if (parm->valtype) {
            inodeNameRes(pstate, &parm->valtype);
            // A parameter's type is a type, or what substituting a type
            // parameter makes one (a name nothing declares is reported already)
            if (!isTypeNode(parm->valtype) && !inodeIsProvisionalType(parm->valtype)
                && !inodeIsError(parm->valtype)
                && !(isNameUseNode(parm->valtype) && nameUseGetDcl((NameUseNode*)parm->valtype) == NULL))
                errorMsgNode(parm->valtype, ErrorNotType,
                    "A macro parameter's type must be a type: '%s: T', or '%s: lazy T'.",
                    &parm->namesym->namestr, &parm->namesym->namestr);
            else {
                // Pointed at the trait's name, in an instance too
                INode *named = parm->valtype->tag == FnCallTag ? ((FnCallNode*)parm->valtype)->objfn : parm->valtype;
                StructNode *trait = macroParmTrait(parm->valtype);
                if (trait)
                    errorMsgNode(named, ErrorGenParmConstr,
                        "The macro parameter %s is typed by a trait, %s: that is not built yet, as a constraint on a macro's type parameter is not, since what either means waits for generics.",
                        &parm->namesym->namestr, &trait->namesym->namestr);
            }
        }
        inodeNameRes(pstate, nodesp);
    }

    // A change to a plain parameter is refused as the body is resolved
    // (macroChangeNameRes), so that an unused macro is refused too
    MacroDclNode *svresolving = macroResolving;
    macroResolving = gennode;
    inodeNameRes(pstate, (INode**)&gennode->body);
    macroResolving = svresolving;

    nametblHookPop();
    pstate->macromethod = svmacromethod;
    pstate->expander = svexpander;
    pstate->scope = oldscope;
}

// Type check a generic declaration
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *gennode) {
}

// Is this a use of a macro: its name, bare or called, or a method call a macro
// method answered? Read off the use after it expanded, which leaves it intact
static int macroIsUse(INode *node) {
    if (node->tag == FnCallTag) {
        FnCallNode *call = (FnCallNode *)node;
        return call->methfld ? nameUseNames(call->methfld, MacroDclTag) : nameUseNames(call->objfn, MacroDclTag);
    }
    return nameUseNames(node, MacroDclTag);
}

// Where 'srcFile()' and 'srcLine()' in a macro's body answer: at the use, or,
// for a use another macro's body wrote, where that one is used, and so on out
// to the outermost. A node an expansion cloned from a body is marked as
// instantiated by the use it expanded (cloneNode), and an argument is not, so
// the chain climbs through bodies alone: a macro passed as an argument answers
// at its own place. So does one in a generic's instance, whose nodes are marked
// as instantiated by the generic's use, which is not a macro's
static INode *macroSrcSite(INode *use) {
    while (use->instnode && use->instnode != use && macroIsUse(use->instnode))
        use = use->instnode;
    return use;
}

// Do the type arguments a use wrote fit the macro's type parameters? Each is
// substituted for its parameter, as a value argument is, so it must be a type.
// Reported at the use, which becomes an error node, where they do not. Where
// none are written they are inferred from the arguments (macroInferTypeArgs).
static int macroTypeArgsFit(INode **nodep, MacroDclNode *macro, Nodes *typeargs) {
    if (macro->typeparms == NULL) {
        if (typeargs == NULL)
            return 1;
        errorMsgNode(*nodep, ErrorMacroTypeArgs,
            "%s declares no type parameters, so its use takes no type arguments: its arguments go in parentheses alone.",
            &macro->namesym->namestr);
        *nodep = newErrorNode(*nodep);
        return 0;
    }
    if (typeargs == NULL)
        return 1;
    if (typeargs->used != macro->typeparms->used) {
        errorMsgNode(*nodep, ErrorArgCount, "Incorrect number of type arguments vs. type parameters expected");
        *nodep = newErrorNode(*nodep);
        return 0;
    }
    INode **nodesp;
    uint32_t cnt;
    int bad = 0;
    for (nodesFor(typeargs, cnt, nodesp)) {
        if (!isTypeNode(*nodesp) && !inodeIsProvisionalType(*nodesp)) {
            errorMsgNode(*nodesp, ErrorNotType, "Expected a type for a macro's type parameter");
            bad = 1;
        }
    }
    if (bad)
        *nodep = newErrorNode(*nodep);
    return !bad;
}

// ---------------------------------------------------------------------------
// What the arguments are, asked before the body is expanded. Type checking
// lowers and replaces nodes, and an expression is checked once, so:
//
// - A plain argument is its binding's initializer, and the binding is a
//   declaration, checked once and marked so. Where the use leaves type
//   parameters to be inferred, each binding whose parameter's type names one
//   is checked first, typed by its argument as 'let a = arg' is, and the type
//   parameters are inferred from those types (macroInferTypeArgs); the body,
//   and the block the binding heads, come after, and pass it by as checked.
// - A 'lazy' argument is substituted, so it is checked where the body puts it.
//   Its type is asked of a copy, as a trial (error.h, "A trial"), once the type
//   parameters are known: its diagnostics counted and not kept, and what type
//   check knows of narrowed names (narrow.h) as it was before. Nothing of the
//   argument itself is touched. A declaration the copy reaches first -- a
//   function not yet checked, a generic's instance for the program's types --
//   is the program's, checked once and loud, and is what the argument reaches
//   too when it is checked for real. What checking the copy makes for itself
//   -- a closure's hidden struct, an instance for it -- is the trial's, and
//   reaches neither the module nor generation (closureBuild, genericTrialPurge).
//   A type parameter the plain arguments leave open may be fixed by a lazy
//   argument's copy, but never by a type the trial alone has (a closure's, or
//   an instance made with one): what such a copy is, is the copy's.

// Type check a copy of 'arg' as a trial, wanted as 'expect'. Answers the
// checked copy, or NULL where it does not check on its own (what it reports is
// the argument's, said where the argument is checked for real). With 'fits',
// also whether it coerces to 'expect'; with 'owned', whether its type is one
// the trial alone has (closureTrialType).
static INode *macroArgTrial(TypeCheckState *pstate, INode *arg, INode *expect, int *fits, int *owned) {
    CloneState cstate;
    uint32_t dclpos = cloneDclPush();
    clonePushState(&cstate, NULL, NULL, pstate->scope, NULL, NULL);
    INode *probe = cloneNode(&cstate, arg);
    clonePopState();
    cloneDclPop(dclpos);

    NarrowFnSave nsave;
    narrowFnBegin(&nsave);
    narrowRestore(nsave.env);
    ErrorTrial trial;
    errorTrialBegin(&trial, closureSerialNow());
    int before = errorsOwn();
    inodeTypeCheck(pstate, &probe, expect);
    int checked = errorsOwn() == before && isExpNode(probe) && !inodeIsError(probe);
    if (checked && owned)
        *owned = closureTrialType(((IExpNode*)probe)->vtype);
    if (checked && fits)
        *fits = iexpCheckedCoerceIn(pstate, expect, &probe) && errorsOwn() == before;
    errorTrialEnd(&trial);
    genericTrialPurge();
    narrowFnEnd(&nsave);
    return checked ? probe : NULL;
}

// Does this parameter type, as written, name one of the macro's type parameters?
static int macroTypeNamesParm(INode *type, Nodes *typeparms, int depth) {
    if (type == NULL || depth > 32)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    if (isNameUseNode(type)) {
        for (nodesFor(typeparms, cnt, nodesp))
            if (*nodesp == ((NameUseNode*)type)->dclnode)
                return 1;
        return 0;
    }
    switch (type->tag) {
    case FnCallTag: {
        FnCallNode *call = (FnCallNode*)type;
        if (macroTypeNamesParm(call->objfn, typeparms, depth + 1))
            return 1;
        if (call->args)
            for (nodesFor(call->args, cnt, nodesp))
                if (macroTypeNamesParm(*nodesp, typeparms, depth + 1))
                    return 1;
        return 0;
    }
    case RefTag:
    case BorrowTag:
    case AllocateTag:
    case ArrayRefTag:
    case ArrayBorrowTag:
    case VirtRefTag:
        return macroTypeNamesParm(((RefNode*)type)->vtexp, typeparms, depth + 1);
    case PtrTag:
    case DerefTag:
        return macroTypeNamesParm(((StarNode*)type)->vtexp, typeparms, depth + 1);
    case ArrayTag:
        return macroTypeNamesParm(arrayElemType(type), typeparms, depth + 1);
    case FnSigTag: {
        FnSigNode *sig = (FnSigNode*)type;
        for (nodesFor(sig->parms, cnt, nodesp))
            if (macroTypeNamesParm(((VarDclNode*)*nodesp)->vtype, typeparms, depth + 1))
                return 1;
        return macroTypeNamesParm(sig->rettype, typeparms, depth + 1);
    }
    case TTupleTag:
        for (nodesFor(((TupleNode*)type)->elems, cnt, nodesp))
            if (macroTypeNamesParm(*nodesp, typeparms, depth + 1))
                return 1;
        return 0;
    default:
        return 0;
    }
}

// The type arguments of a use that writes none, inferred from its plain
// arguments by the rules a generic function's are from a call's
// (genericInferMacroParms). Each plain parameter whose type names a type
// parameter has its binding checked now ('early'), typed by its argument, and
// that type matched against the parameter's as written. An unsuffixed number
// literal, or a 'null', for one waits, as a function's does, for what the
// others say, then takes its own type; its binding is checked with the block,
// typed by the parameter. A type parameter the plain arguments leave open is
// then said by the lazy arguments whose parameters' types name it, each a
// checked copy (macroArgTrial), by the same rule and agreeing with what the
// plain ones said -- but not by a copy whose type the trial alone has (a
// closure's). Answers NULL once reported: a binding that does not check (its
// own error), two arguments saying a type parameter differently, one only such
// a copy could say (ErrorMacroLazyInfer), or one nothing says.
static Nodes *macroInferTypeArgs(TypeCheckState *pstate, INode *use, MacroDclNode *macro, Nodes *args,
        Nodes *bindings, uint8_t *early, INode *selfparm) {
    uint32_t given = args ? args->used : 0;
    Nodes *parmtypes = newNodes(given ? given : 1);
    Nodes *typed = newNodes(given ? given : 1);
    int before = errors;
    for (uint32_t i = 0; i < given; ++i) {
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i);
        INode *parmtype = NULL;
        INode *argtyped = NULL;
        VarDclNode *var = bindings ? (VarDclNode*)nodesGet(bindings, i) : NULL;
        if (var && macroTypeNamesParm(parm->valtype, macro->typeparms, 0)) {
            parmtype = parm->valtype;
            INode *arg = nodesGet(args, i);
            if (litIsUntypedNumber(arg) || litIsUntypedNull(arg))
                argtyped = arg;
            else {
                var->vtype = unknownType;
                inodeTypeCheckAny(pstate, &nodesGet(bindings, i));
                early[i] = 1;
                argtyped = var->value;
            }
        }
        nodesAdd(&parmtypes, parmtype);
        nodesAdd(&typed, argtyped);
    }
    if (errors != before)
        return NULL;
    Nodes *typeargs = genericInferMacroParms(pstate, macro->typeparms, parmtypes, typed, NULL, use, macro->namesym);
    if (typeargs == NULL)
        return NULL;

    // The type parameters still open, and the lazy arguments that may say them
    Nodes *open = newNodes(typeargs->used);
    for (uint32_t t = 0; t < typeargs->used; ++t)
        if (nodesGet(typeargs, t) == NULL)
            nodesAdd(&open, nodesGet(macro->typeparms, t));
    if (open->used == 0)
        return typeargs;
    Nodes *lazytypes = newNodes(given ? given : 1);
    Nodes *lazytyped = newNodes(given ? given : 1);
    GenVarDclNode *ownedparm = NULL;
    int failed = 0;
    for (uint32_t i = 0; i < given; ++i) {
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i);
        INode *parmtype = NULL;
        INode *argtyped = NULL;
        if ((INode*)parm != selfparm && (parm->flags & FlagLazy) && parm->valtype && parm->valtype != unknownType
            && macroTypeNamesParm(parm->valtype, open, 0)) {
            INode *arg = nodesGet(args, i);
            int owned = 0;
            if (litIsUntypedNumber(arg) || litIsUntypedNull(arg))
                argtyped = arg;
            else if ((argtyped = macroArgTrial(pstate, arg, unknownType, NULL, &owned)) == NULL)
                failed = 1;
            if (owned) {
                // What the copy is, is the copy's: it says nothing
                if (ownedparm == NULL)
                    ownedparm = parm;
                argtyped = NULL;
            }
            if (argtyped)
                parmtype = parm->valtype;
        }
        nodesAdd(&lazytypes, parmtype);
        nodesAdd(&lazytyped, argtyped);
    }
    // A lazy argument that does not check on its own says nothing, and its own
    // error is the one to report. The use will not be expanded, so it is checked
    // where it stands.
    if (failed) {
        for (uint32_t i = 0; i < given; ++i) {
            GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i);
            if ((INode*)parm != selfparm && (parm->flags & FlagLazy) && parm->valtype
                && macroTypeNamesParm(parm->valtype, open, 0) && nodesGet(lazytyped, i) == NULL)
                inodeTypeCheckAny(pstate, &nodesGet(args, i));
        }
        if (errors != before)
            return NULL;
    }
    typeargs = genericInferMacroParms(pstate, macro->typeparms, lazytypes, lazytyped, typeargs, use, macro->namesym);
    if (typeargs == NULL)
        return NULL;

    // A type parameter nothing said
    for (uint32_t t = 0; t < typeargs->used; ++t) {
        if (nodesGet(typeargs, t) != NULL)
            continue;
        if (ownedparm) {
            char all[200] = "";
            size_t used = 0;
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(macro->typeparms, cnt, nodesp))
                if (used + 40 < sizeof(all))
                    used += snprintf(all + used, sizeof(all) - used, "%s%s", used ? ", " : "",
                        &((GenVarDclNode*)*nodesp)->namesym->namestr);
            Name *tname = ((GenVarDclNode*)nodesGet(macro->typeparms, t))->namesym;
            errorMsgNode(use, ErrorMacroLazyInfer,
                "The type parameter %s of the macro %s is left to the lazy argument for %s, whose type is a closure's: a lazy argument is checked as a copy, and a copy's closure is its own, so it fixes no type. Write the type arguments out, '%s[%s](...)', or make %s a plain parameter, whose argument's own type fixes it.",
                &tname->namestr, &macro->namesym->namestr, &ownedparm->namesym->namestr,
                &macro->namesym->namestr, all, &ownedparm->namesym->namestr);
            return NULL;
        }
        genericMacroMissingParms(typeargs, macro->typeparms, macro->namesym, use);
        return NULL;
    }
    return typeargs;
}

// The bindings checked early, typed by their arguments (macroInferTypeArgs),
// made the type their parameters have, the type parameters now known: the
// type itself, or a value that coerces to it. Reported, ErrorMacroArgType,
// where one does not.
static int macroEarlyBindingsFit(TypeCheckState *pstate, INode *use, MacroDclNode *macro, Nodes *typeargs,
        Nodes *bindings, uint8_t *early);

// A value parameter's type with the macro's type parameters substituted, and
// type checked, as the use's expansion will have it. NULL where it is no type
// (reported).
static INode *macroParmTypeAt(TypeCheckState *pstate, INode *use, MacroDclNode *macro, Nodes *typeargs, GenVarDclNode *parm) {
    CloneState cstate;
    clonePushState(&cstate, use, NULL, pstate->scope, typeargs ? macro->typeparms : NULL, typeargs);
    INode *type = cloneNode(&cstate, parm->valtype);
    clonePopState();
    if (!itypeTypeCheck(pstate, &type))
        return NULL;
    return type;
}

static int macroEarlyBindingsFit(TypeCheckState *pstate, INode *use, MacroDclNode *macro, Nodes *typeargs,
        Nodes *bindings, uint8_t *early) {
    int bad = 0;
    for (uint32_t i = 0; bindings && i < bindings->used; ++i) {
        if (!early[i])
            continue;
        VarDclNode *var = (VarDclNode*)nodesGet(bindings, i);
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i);
        INode *parmtype = macroParmTypeAt(pstate, use, macro, typeargs, parm);
        if (parmtype == NULL) {
            bad = 1;
            continue;
        }
        if (iexpMatches(&var->value, parmtype, Coercion) == EqMatch)
            continue;
        if (iexpCheckedCoerceIn(pstate, parmtype, &var->value))
            var->vtype = parmtype;
        else {
            macroArgTypeRefused(var->value, parm->namesym, 0, parmtype);
            bad = 1;
        }
    }
    return !bad;
}

// Does each 'lazy' argument fit its parameter's type? It is checked as a copy
// (macroArgTrial) before it is substituted, so a wrong type is refused at the
// use, naming the parameter, rather than wherever the body puts it, or nowhere
// if the body never does. 'lazy void' takes a block or statement that gives no
// value. An argument that does not check on its own is left to be checked
// where it is put, which reports it. Reported, the use an error node, where
// one does not fit.
static int macroLazyArgsFit(TypeCheckState *pstate, INode **nodep, MacroDclNode *macro, Nodes *typeargs, Nodes *args, INode *selfparm) {
    uint32_t given = args ? args->used : 0;
    int bad = 0;
    for (uint32_t i = 0; i < given; ++i) {
        GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i);
        if ((INode*)parm == selfparm || !(parm->flags & FlagLazy) || parm->valtype == NULL || parm->valtype == unknownType)
            continue;
        INode *parmtype = macroParmTypeAt(pstate, *nodep, macro, typeargs, parm);
        if (parmtype == NULL) {
            bad = 1;
            continue;
        }
        int fits = 1;
        INode *probe = macroArgTrial(pstate, nodesGet(args, i), parmtype, &fits, NULL);
        if (probe && !fits) {
            macroArgTypeRefused(probe, parm->namesym, 1, parmtype);
            bad = 1;
        }
    }
    if (bad)
        *nodep = newErrorNode(*nodep);
    return !bad;
}

// Expand a macro in place of the node that used it, substituting 'typeargs'
// for its type parameters, binding each plain value parameter's argument to a
// variable and substituting each 'lazy' one's, then type check what it
// expanded to. 'selfparm' is the parameter a macro method's receiver stands in
// for, or NULL.
static void macroExpand(TypeCheckState *pstate, INode **nodep, MacroDclNode *macro, Nodes *typeargs, Nodes *args, INode *selfparm) {
    if (!macroTypeArgsFit(nodep, macro, typeargs))
        return;
    uint32_t expected = macro->parms ? macro->parms->used : 0;
    uint32_t given = args ? args->used : 0;
    if (given != expected) {
        errorMsgNode(*nodep, ErrorArgCount, "Incorrect number of arguments vs. parameters expected");
        return;
    }
    // Arguments substitute for parameters by position, so a name has nothing to match
    if (namedValRefuseArgs(args, "a macro use")) {
        *nodep = newErrorNode(*nodep);
        return;
    }
    // A plain parameter is evaluated once: the expansion binds its argument
    // first, '{ let a: T = arg; body }', and the body's uses of the parameter
    // are uses of that variable (cloneNode). A 'lazy' one, and a macro
    // method's 'self', are substituted whole. One whose declaration was
    // refused (no type) is substituted, as before, so as not to report again.
    // 'bindings' holds, per value parameter, its variable or NULL.
    Nodes *bindings = NULL;
    if (given) {
        INode **nodesp;
        uint32_t cnt;
        uint32_t i = 0;
        for (nodesFor(macro->parms, cnt, nodesp)) {
            GenVarDclNode *parm = (GenVarDclNode*)*nodesp;
            INode *arg = nodesGet(args, i++);
            if ((INode*)parm == selfparm || (parm->flags & FlagLazy)
                || parm->valtype == NULL || parm->valtype == unknownType)
                continue;
            if (bindings == NULL) {
                bindings = newNodes(given);
                for (uint32_t j = 0; j < given; ++j)
                    nodesAdd(&bindings, NULL);
            }
            // Typed below, once the type parameters are hooked to their arguments.
            // Declared in the block the expansion becomes, one level in
            VarDclNode *var = newVarDclFull(parm->namesym, VarDclTag, NULL, (INode*)immPerm, arg);
            inodeLexCopy((INode*)var, arg);
            var->scope = (uint16_t)(pstate->scope + 1);
            var->flags |= FlagMacroBind;
            nodesGet(bindings, i - 1) = (INode*)var;
        }
    }

    // Type parameters not written are inferred from the plain arguments, their
    // bindings checked first; then each 'lazy' argument is checked against its
    // type, the type parameters known, before it is substituted
    uint8_t *early = memAllocBlk(given ? given : 1);
    memset(early, 0, given ? given : 1);
    if (macro->typeparms && typeargs == NULL) {
        typeargs = macroInferTypeArgs(pstate, *nodep, macro, args, bindings, early, selfparm);
        if (typeargs == NULL || !macroEarlyBindingsFit(pstate, *nodep, macro, typeargs, bindings, early)) {
            *nodep = newErrorNode(*nodep);
            return;
        }
    }
    if (!macroLazyArgsFit(pstate, nodep, macro, typeargs, args, selfparm))
        return;

    // Type arguments and value arguments substitute alike: one list of each,
    // the type parameters first. A plain parameter is hooked to its variable.
    Nodes *parms = NULL;
    if (typeargs || given) {
        uint32_t ntypes = typeargs ? typeargs->used : 0;
        parms = newNodes(ntypes + given);
        Nodes *allargs = newNodes(ntypes + given);
        INode **nodesp;
        uint32_t cnt;
        if (typeargs) {
            for (nodesFor(macro->typeparms, cnt, nodesp))
                nodesAdd(&parms, *nodesp);
            for (nodesFor(typeargs, cnt, nodesp))
                nodesAdd(&allargs, *nodesp);
        }
        if (given) {
            for (nodesFor(macro->parms, cnt, nodesp))
                nodesAdd(&parms, *nodesp);
            uint32_t i = 0;
            for (nodesFor(args, cnt, nodesp)) {
                INode *var = bindings ? nodesGet(bindings, i) : NULL;
                nodesAdd(&allargs, var ? var : *nodesp);
                ++i;
            }
        }
        args = allargs;
    }

    // A macro whose body names itself expands without end, and no mark can see
    // it: each expansion is a fresh clone, never the node expanded before.
    if (!genericInstantiateEnter(*nodep)) {
        *nodep = newErrorNode(*nodep);
        return;
    }

    // Replace the use with the body, substituting arguments for parameters
    INode *use = *nodep;
    CloneState cstate;
    clonePushState(&cstate, use, NULL, pstate->scope, parms, args);
    cstate.selfparm = selfparm;
    // 'srcFile()' and 'srcLine()' in the body answer where it is used
    cstate.srcsite = macroSrcSite(use);
    INode *body = cloneNode(&cstate, macro->body);
    // Each binding's type is its parameter's, the macro's type parameters
    // substituted, and the bindings go first, in the parameters' order: the
    // arguments are evaluated once each, left to right, before the body
    if (bindings) {
        BlockNode *blk = newBlockNode();
        inodeLexCopy((INode*)blk, use);
        blk->instnode = use;
        INode **nodesp;
        uint32_t cnt;
        uint32_t i = 0;
        for (nodesFor(bindings, cnt, nodesp)) {
            VarDclNode *var = (VarDclNode*)*nodesp;
            GenVarDclNode *parm = (GenVarDclNode*)nodesGet(macro->parms, i++);
            if (var == NULL)
                continue;
            // One checked already, for inference, keeps its type
            if (!early[i - 1])
                var->vtype = cloneNode(&cstate, parm->valtype);
            var->instnode = use;
            nodesAdd(&blk->stmts, (INode*)var);
        }
        nodesAdd(&blk->stmts, body);
        body = (INode*)blk;
    }
    *nodep = body;
    clonePopState();

    // Now type check the instantiated nodes
    inodeTypeCheckAny(pstate, nodep);
    genericInstantiateExit();
}

// The receiver a macro method named bare inside a method expands against: the
// enclosing method's own 'self', resolved. NULL, with a diagnostic, where no
// method encloses the use -- as for a bare field name, in nameUseTypeCheck.
static INode *macroSelfReceiver(TypeCheckState *pstate, INode *usenode, Name *macroname) {
    if (pstate->fn == NULL || !(pstate->fn->flags & FlagMethFld)) {
        errorMsgNode(usenode, ErrorUnkName,
            "%s is a macro method, and there is no self here to reach it through.", &macroname->namestr);
        return NULL;
    }
    NameUseNode *selfnode = newNameUseNode(selfName);
    copyNodeLex(selfnode, usenode);
    selfnode->dclnode = nodesGet(((FnSigNode*)pstate->fn->vtype)->parms, 0);
    selfnode->vtype = ((VarDclNode*)selfnode->dclnode)->vtype;
    return (INode*)selfnode;
}

// Expand a macro named where a value is expected
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **gennode) {
    // Through the alias where a fold is what bound the name here
    MacroDclNode *macrodcl = (MacroDclNode*)nameUseGetDcl(*gennode);

    // A macro method named bare inside a method means 'self.name', as a bare
    // field does. Rewritten to that call, which then expands as one.
    if (macrodcl->flags & FlagMethFld) {
        INode *self = macroSelfReceiver(pstate, (INode*)*gennode, macrodcl->namesym);
        if (self == NULL) {
            *((INode**)gennode) = newErrorNode((INode*)*gennode);
            return;
        }
        FnCallNode *call = newFnCallNode(self, 0);
        copyNodeLex(call, *gennode);
        call->methfld = (INode*)*gennode;
        *((INode**)gennode) = (INode*)call;
        macroMethodTypeCheck(pstate, (FnCallNode**)gennode, macrodcl, NULL);
        return;
    }

    uint32_t expected = macrodcl->parms ? macrodcl->parms->used : 0;
    if (expected > 0) {
        errorMsgNode((INode*)*gennode, ErrorNoArgs, "Generic or macro expects arguments to be provided");
        return;
    }
    macroExpand(pstate, (INode**)gennode, macrodcl, NULL, NULL, NULL);
}

// Expand a macro used by its name, with 'typeargs' for its type parameters
// and 'args' for its value parameters. A macro method called bare inside a
// method means 'self.name(args)', as a bare method call does.
static void macroUseExpand(TypeCheckState *pstate, FnCallNode **nodep, MacroDclNode *macrodcl, Nodes *typeargs, Nodes *args) {
    if (macrodcl->flags & FlagMethFld) {
        INode *self = macroSelfReceiver(pstate, (INode*)*nodep, macrodcl->namesym);
        if (self == NULL) {
            *((INode**)nodep) = newErrorNode((INode*)*nodep);
            return;
        }
        (*nodep)->methfld = (*nodep)->objfn;
        (*nodep)->objfn = self;
        (*nodep)->args = args;
        macroMethodTypeCheck(pstate, nodep, macrodcl, typeargs);
        return;
    }
    macroExpand(pstate, (INode**)nodep, macrodcl, typeargs, args, NULL);
}

// Expand a macro called by name, substituting the arguments for its parameters.
// The brackets of 'm[...]' fill the macro's first bracket list: its type
// parameters where it declares them, 'three[i64]', its value parameters
// where it does not, 'twice[x]'. Parentheses give value arguments alone.
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    // Through the alias where a fold is what bound the name here: the binding
    // carried the visibility, and the macro is what it stands for
    MacroDclNode *macrodcl = (MacroDclNode*)nameUseGetDcl((NameUseNode*)(*nodep)->objfn);
    if (((*nodep)->flags & FlagIndex) && macrodcl->typeparms)
        macroUseExpand(pstate, nodep, macrodcl, (*nodep)->args, NULL);
    else
        macroUseExpand(pstate, nodep, macrodcl, NULL, (*nodep)->args);
}

// Expand a macro called with its type arguments in brackets and its value
// arguments in parentheses, 'm[i32](x)': the call's callee is the bracketed use
void macroCallTypeArgsTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode *index = (FnCallNode*)(*nodep)->objfn;
    MacroDclNode *macrodcl = (MacroDclNode*)nameUseGetDcl((NameUseNode*)index->objfn);
    Nodes *typeargs = index->args ? index->args : newNodes(1);
    (*nodep)->objfn = index->objfn;
    macroUseExpand(pstate, nodep, macrodcl, typeargs, (*nodep)->args);
}

// A plain parameter is evaluated once, into a variable nothing may change
// (macroExpand), so a body that changes it -- assigns to it or a part of it,
// swaps it, borrows it to change it -- wanted its argument substituted whole.
// Reported where the body changes it, in each expansion.
void macroParmAssignRefused(INode *at, VarDclNode *var) {
    errorMsgNode(at, ErrorMacroParmAssign, MacroParmAssignMsg, &var->namesym->namestr, &var->namesym->namestr);
}

// An argument whose type does not fit its parameter's, 'parmtype': a 'lazy'
// one (macroLazyArgsFit), or a plain one as its binding is checked (varDclTypeCheck)
static void macroTypeSpell(char *buf, size_t size, INode *type) {
    buf[0] = '\0';
    if (itypeGetTypeDcl(type)->tag == VoidTag)
        snprintf(buf, size, "void");
    else
        itypeSpellCat(buf, size, type, 0);
}

void macroArgTypeRefused(INode *arg, Name *parm, int lazy, INode *parmtype) {
    char want[200];
    macroTypeSpell(want, sizeof(want), parmtype);
    INode *argtype = isExpNode(arg) ? ((IExpNode*)arg)->vtype : NULL;
    if (argtype && argtype != unknownType && argtype != errorType) {
        char given[200];
        macroTypeSpell(given, sizeof(given), argtype);
        errorMsgNode(arg, ErrorMacroArgType,
            "This argument's type is %s, and the macro's parameter %s is '%s: %s%s'.",
            given, &parm->namestr, &parm->namestr, lazy ? "lazy " : "", want);
    }
    else
        errorMsgNode(arg, ErrorMacroArgType,
            "This argument is not of the type the macro's parameter %s is, '%s: %s%s'.",
            &parm->namestr, &parm->namestr, lazy ? "lazy " : "", want);
}

// Expand a macro method called on a receiver: 'x.name(args)' expands the body
// with 'x' substituted for 'self' and the arguments for the parameters after it.
// The receiver is an expression like any other argument, substituted wherever
// 'self' appears and evaluated as many times as it appears.
void macroMethodTypeCheck(TypeCheckState *pstate, FnCallNode **nodep, MacroDclNode *macro, Nodes *typeargs) {
    FnCallNode *node = *nodep;
    if (node->args == NULL)
        node->args = newNodes(1);
    nodesInsert(&node->args, node->objfn, 0);
    macroExpand(pstate, (INode**)nodep, macro, typeargs, node->args, nodesGet(macro->parms, 0));
}
