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
    MacroDclNode *macro;
    newNode(macro, MacroDclNode, MacroDclTag);
    macro->vtype = NULL;
    macro->namesym = namesym;
    macro->parms = newNodes(4);
    macro->body = NULL;
    return macro;
}

// Serialize a macro node
void macroDclPrint(MacroDclNode *name) {
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
void macroDclNameRes(NameResState *pstate, MacroDclNode *macro) {
    uint16_t oldscope = pstate->scope;
    pstate->scope = 1;

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(macro->parms, cnt, nodesp))
        inodeNameRes(pstate, nodesp);

    // Hook macro's parameters into global name table
    // so that when we walk the macro's logic, parameter names are resolved
    nametblHookPush();
    for (nodesFor(macro->parms, cnt, nodesp))
        nametblHookNode(((VarDclNode *)*nodesp)->namesym, *nodesp);

    inodeNameRes(pstate, (INode**)&macro->body);

    nametblHookPop();
    pstate->scope = oldscope;
}

// Type check 
void macroDclTypeCheck(TypeCheckState *pstate, MacroDclNode *macro) {
}

// Type check macro name use
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro) {
    // Set up clone state
    CloneState cstate;
    cstate.instnode = (INode*)*macro;
    cstate.scope = pstate->scope;

    // Obtain macro to expand
    MacroDclNode *macrodcl = (MacroDclNode*)(*macro)->dclnode;

    // Instantiate macro, replacing macro name
    *((INode**)macro) = cloneNode(&cstate, macrodcl->body);

    // Now type check the instantiated nodes
    inodeTypeCheck(pstate, (INode**)macro);
}

// Instantiate a macro using passed arguments
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    MacroDclNode *macrodcl = (MacroDclNode*)((NameUseNode*)(*nodep)->objfn)->dclnode;
    if ((*nodep)->args->used != macrodcl->parms->used) {
        errorMsgNode((INode*)*nodep, ErrorManyArgs, "Incorrect number of arguments for macro call");
        return;
    }

    // Set up clone state
    CloneState cstate;
    cstate.instnode = (INode*)*nodep;
    cstate.scope = pstate->scope;

    // Hook in named arguments for parms, then instantiate macro , replacing fncall node
    nametblHookPush();
    INode **parmp = &nodesGet(macrodcl->parms, 0);
    INode **argsp;
    uint32_t cnt;
    for (nodesFor((*nodep)->args, cnt, argsp)) {
        nametblHookNode(((GenVarDclNode *)*parmp++)->namesym, (INode*)*argsp);
    }
    *((INode **)nodep) = cloneNode(&cstate, macrodcl->body);
    nametblHookPop();

    // Now type check the instantiated nodes
    inodeTypeCheck(pstate, (INode **)nodep);
}

