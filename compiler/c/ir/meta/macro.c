/** Handling for macro declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

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

    inodeNameRes(pstate, (INode**)&gennode->body);

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
// Reported at the use, which becomes an error node, where they do not.
// Inferring a type parameter from the arguments is not built, so a macro that
// declares any is given all of them in brackets.
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
    if (typeargs == NULL) {
        errorMsgNode(*nodep, ErrorMacroTypeArgs,
            "%s declares type parameters, and inferring them from its arguments is not built yet: write them in brackets before the arguments, '%s[i32](...)'.",
            &macro->namesym->namestr, &macro->namesym->namestr);
        *nodep = newErrorNode(*nodep);
        return 0;
    }
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

// Expand a macro in place of the node that used it, substituting 'typeargs'
// for its type parameters and 'args' for its value parameters, then type
// check what it expanded to. 'selfparm' is the parameter a macro method's
// receiver stands in for, or NULL.
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

    // Type arguments and value arguments substitute alike: one list of each,
    // the type parameters first
    Nodes *parms = given ? macro->parms : NULL;
    if (typeargs) {
        Nodes *allparms = newNodes(typeargs->used + given);
        Nodes *allargs = newNodes(typeargs->used + given);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(macro->typeparms, cnt, nodesp))
            nodesAdd(&allparms, *nodesp);
        for (nodesFor(typeargs, cnt, nodesp))
            nodesAdd(&allargs, *nodesp);
        if (given) {
            for (nodesFor(macro->parms, cnt, nodesp))
                nodesAdd(&allparms, *nodesp);
            for (nodesFor(args, cnt, nodesp))
                nodesAdd(&allargs, *nodesp);
        }
        parms = allparms;
        args = allargs;
    }

    // A macro whose body names itself expands without end, and no mark can see
    // it: each expansion is a fresh clone, never the node expanded before.
    if (!genericInstantiateEnter(*nodep)) {
        *nodep = newErrorNode(*nodep);
        return;
    }

    // Replace the use with the body, substituting arguments for parameters
    CloneState cstate;
    clonePushState(&cstate, *nodep, NULL, pstate->scope, parms, args);
    cstate.selfparm = selfparm;
    // 'srcFile()' and 'srcLine()' in the body answer where it is used
    cstate.srcsite = macroSrcSite(*nodep);
    *nodep = cloneNode(&cstate, macro->body);
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
