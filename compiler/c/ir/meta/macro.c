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
// instance's types.
INode *cloneMacroDclNode(CloneState *cstate, MacroDclNode *node) {
    MacroDclNode *newnode = memAllocBlk(sizeof(MacroDclNode));
    memcpy(newnode, node, sizeof(MacroDclNode));
    newnode->parms = newNodes(node->parms->used);

    INode **nodesp;
    uint32_t cnt;
    nametblHookPush();
    for (nodesFor(node->parms, cnt, nodesp)) {
        GenVarDclNode *parm = newGVarDclNode(((GenVarDclNode*)*nodesp)->namesym);
        copyNodeLex(parm, *nodesp);
        nodesAdd(&newnode->parms, (INode*)parm);
        nametblHookNode(parm->namesym, (INode*)parm);
    }
    newnode->body = cloneNode(cstate, node->body);
    nametblHookPop();
    return (INode*)newnode;
}

// Serialize
void macroPrint(MacroDclNode *name) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("macro %s (", &name->namesym->namestr);
    for (nodesFor(name->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint(") ");
    inodePrintNode(name->body);
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

    // Hook gennode's parameters into global name table
    // so that when we walk the gennode's logic, parameter names are resolved.
    // Resolving a parameter hooks it, so this has to happen inside the push --
    // before it, the parameter would bind in the enclosing scope and the
    // matching pop would never remove it.
    nametblHookPush();
    for (nodesFor(gennode->parms, cnt, nodesp))
        inodeNameRes(pstate, nodesp);

    inodeNameRes(pstate, (INode**)&gennode->body);

    nametblHookPop();
    pstate->macromethod = svmacromethod;
    pstate->scope = oldscope;
}

// Type check a generic declaration
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *gennode) {
}

// Expand a macro in place of the node that used it, substituting 'args' for
// its parameters, then type check what it expanded to. 'selfparm' is the
// parameter a macro method's receiver stands in for, or NULL.
static void macroExpand(TypeCheckState *pstate, INode **nodep, MacroDclNode *macro, Nodes *args, INode *selfparm) {
    uint32_t expected = macro->parms ? macro->parms->used : 0;
    uint32_t given = args ? args->used : 0;
    if (given != expected) {
        errorMsgNode(*nodep, ErrorArgCount, "Incorrect number of arguments vs. parameters expected");
        return;
    }

    // A macro whose body names itself expands without end, and no mark can see
    // it: each expansion is a fresh clone, never the node expanded before.
    if (!genericInstantiateEnter(*nodep)) {
        *nodep = newErrorNode(*nodep);
        return;
    }

    // Replace the use with the body, substituting arguments for parameters
    CloneState cstate;
    clonePushState(&cstate, *nodep, NULL, pstate->scope, given ? macro->parms : NULL, args);
    cstate.selfparm = selfparm;
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
        macroMethodTypeCheck(pstate, (FnCallNode**)gennode, macrodcl);
        return;
    }

    uint32_t expected = macrodcl->parms ? macrodcl->parms->used : 0;
    if (expected > 0) {
        errorMsgNode((INode*)*gennode, ErrorNoArgs, "Generic or macro expects arguments to be provided");
        return;
    }
    macroExpand(pstate, (INode**)gennode, macrodcl, NULL, NULL);
}

// Expand a macro called by name, substituting the arguments for its parameters
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    // Through the alias where a fold is what bound the name here: the binding
    // carried the visibility, and the macro is what it stands for
    MacroDclNode *macrodcl = (MacroDclNode*)nameUseGetDcl((NameUseNode*)(*nodep)->objfn);

    // A macro method called bare inside a method means 'self.name(args)', as a
    // bare method call does
    if (macrodcl->flags & FlagMethFld) {
        INode *self = macroSelfReceiver(pstate, (INode*)*nodep, macrodcl->namesym);
        if (self == NULL) {
            *((INode**)nodep) = newErrorNode((INode*)*nodep);
            return;
        }
        (*nodep)->methfld = (*nodep)->objfn;
        (*nodep)->objfn = self;
        macroMethodTypeCheck(pstate, nodep, macrodcl);
        return;
    }

    macroExpand(pstate, (INode**)nodep, macrodcl, (*nodep)->args, NULL);
}

// Expand a macro method called on a receiver: 'x.name(args)' expands the body
// with 'x' substituted for 'self' and the arguments for the parameters after it.
// The receiver is an expression like any other argument, substituted wherever
// 'self' appears and evaluated as many times as it appears.
void macroMethodTypeCheck(TypeCheckState *pstate, FnCallNode **nodep, MacroDclNode *macro) {
    FnCallNode *node = *nodep;
    if (node->args == NULL)
        node->args = newNodes(1);
    nodesInsert(&node->args, node->objfn, 0);
    macroExpand(pstate, (INode**)nodep, macro, node->args, nodesGet(macro->parms, 0));
}
