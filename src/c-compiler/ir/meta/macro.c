/** Handling for gennode declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new generic declaraction node
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
    for (nodesFor(gennode->parms, cnt, nodesp))
        inodeNameRes(pstate, nodesp);

    // Hook gennode's parameters into global name table
    // so that when we walk the gennode's logic, parameter names are resolved
    nametblHookPush();
    for (nodesFor(gennode->parms, cnt, nodesp))
        nametblHookNode(((VarDclNode *)*nodesp)->namesym, *nodesp);

    inodeNameRes(pstate, (INode**)&gennode->body);

    nametblHookPop();
    pstate->scope = oldscope;
}

// Type check a generic declaration
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *gennode) {
}

// Type check generic name use
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **gennode) {
    // Obtain macro to expand
    MacroDclNode *macrodcl = (MacroDclNode*)(*gennode)->dclnode;
    uint32_t expected = macrodcl->parms ? macrodcl->parms->used : 0;
    if (expected > 0) {
        errorMsgNode((INode*)*gennode, ErrorManyArgs, "Generic or macro expects arguments to be provided");
        return;
    }

    // Instantiate macro, replacing gennode
    CloneState cstate;
    clonePushState(&cstate, (INode*)*gennode, NULL, pstate->scope, NULL, NULL);
    *((INode**)gennode) = cloneNode(&cstate, macrodcl->body);
    clonePopState();

    // Now type check the instantiated nodes
    inodeTypeCheckAny(pstate, (INode**)gennode);
}

// Instantiate a generic using passed arguments
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    MacroDclNode *genericnode = (MacroDclNode*)((NameUseNode*)(*nodep)->objfn)->dclnode;

    uint32_t expected = genericnode->parms ? genericnode->parms->used : 0;
    if ((*nodep)->args->used != expected) {
        errorMsgNode((INode*)*nodep, ErrorManyArgs, "Incorrect number of arguments vs. parameters expected");
        return;
    }

    // Replace gennode call with instantiated body, substituting parameters
    CloneState cstate;
    clonePushState(&cstate, (INode*)*nodep, NULL, pstate->scope, genericnode->parms, (*nodep)->args);
    *((INode**)nodep) = cloneNode(&cstate, genericnode->body);
    clonePopState();

    // Now type check the instantiated nodes
    inodeTypeCheckAny(pstate, (INode **)nodep);
}

