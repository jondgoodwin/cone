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
GenericNode *newGenericNode(Name *namesym) {
    GenericNode *gennode;
    newNode(gennode, GenericNode, GenericTag);
    gennode->vtype = NULL;
    gennode->namesym = namesym;
    gennode->parms = newNodes(4);
    gennode->body = NULL;
    gennode->memonodes = newNodes(4);
    return gennode;
}

// Serialize
void genericPrint(GenericNode *name) {
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
void genericNameRes(NameResState *pstate, GenericNode *gennode) {
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
void genericTypeCheck(TypeCheckState *pstate, GenericNode *gennode) {
}

// Type check generic name use
void genericNameTypeCheck(TypeCheckState *pstate, NameUseNode **gennode) {
    // Obtain macro to expand
    GenericNode *macrodcl = (GenericNode*)(*gennode)->dclnode;
    uint32_t expected = macrodcl->parms ? macrodcl->parms->used : 0;
    if (expected > 0) {
        errorMsgNode((INode*)*gennode, ErrorManyArgs, "Macro expects arguments to be provided");
        return;
    }

    // Instantiate macro, replacing gennode
    CloneState cstate;
    clonePushState(&cstate, (INode*)*gennode, NULL, pstate->scope, NULL, NULL);
    *((INode**)gennode) = cloneNode(&cstate, macrodcl->body);
    clonePopState();

    // Now type check the instantiated nodes
    inodeTypeCheck(pstate, (INode**)gennode);
}

// Verify arguments are types, check if instantiated, instantiate if needed and return ptr to it
INode *genericMemoize(TypeCheckState *pstate, FnCallNode *fncall) {
    GenericNode *genericnode = (GenericNode*)((NameUseNode*)fncall->objfn)->dclnode;

    // Verify all arguments are types
    INode **nodesp;
    uint32_t cnt;
    int badargs = 0;
    for (nodesFor(fncall->args, cnt, nodesp)) {
        if (!isTypeNode(*nodesp)) {
            errorMsgNode((INode*)*nodesp, ErrorManyArgs, "Expected a type for a generic parameter");
            badargs = 1;
        }
    }
    if (badargs)
        return NULL;

    // Check whether these types have already been instantiated for this generic
    // memonodes holds pairs of nodes: an FnCallNode and what it instantiated
    // A match is the first FnCallNode whose types match what we want
    for (nodesFor(genericnode->memonodes, cnt, nodesp)) {
        FnCallNode *fncallprior = (FnCallNode *)*nodesp;
        nodesp++; cnt--; // skip to instance node
        int match = 1;
        INode **priornodesp;
        uint32_t priorcnt;
        INode **nownodesp = &nodesGet(fncall->args, 0);
        for (nodesFor(fncallprior->args, priorcnt, priornodesp)) {
            if (!itypeIsSame(*priornodesp, *nownodesp)) {
                match = 0;
                break;
            }
            nownodesp++;
        }
        if (match) {
            // Return a namenode pointing to dcl instance
            NameUseNode *fnuse = newNameUseNode(genericnode->namesym);
            fnuse->tag = isTypeNode(*nownodesp)? TypeNameUseTag : VarNameUseTag;
            fnuse->dclnode = *nownodesp;
            return (INode *)fnuse;
        }
    }

    // No match found, instantiate the dcl generic
    CloneState cstate;
    clonePushState(&cstate, (INode*)fncall, NULL, pstate->scope, genericnode->parms, fncall->args);
    INode *instance = cloneNode(&cstate, genericnode->body);
    clonePopState();

    // Type check the instanced declaration
    inodeTypeCheck(pstate, &instance);

    // Remember instantiation for the future
    nodesAdd(&genericnode->memonodes, (INode*)fncall);
    nodesAdd(&genericnode->memonodes, instance);

    // Return a namenode pointing to fndcl instance
    NameUseNode *fnuse = newNameUseNode(genericnode->namesym);
    fnuse->tag = isTypeNode(instance) ? TypeNameUseTag : VarNameUseTag;
    fnuse->dclnode = instance;
    return (INode *)fnuse;
}

// Instantiate a generic using passed arguments
void genericCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    GenericNode *genericnode = (GenericNode*)((NameUseNode*)(*nodep)->objfn)->dclnode;

    uint32_t expected = genericnode->parms ? genericnode->parms->used : 0;
    if ((*nodep)->args->used != expected) {
        errorMsgNode((INode*)*nodep, ErrorManyArgs, "Incorrect number of arguments vs. parameters expected");
        return;
    }

    // Replace gennode call with instantiated body, substituting parameters
    if (genericnode->flags & GenericMemoize)
        *((INode**)nodep) = genericMemoize(pstate, *nodep);
    else {
        CloneState cstate;
        clonePushState(&cstate, (INode*)*nodep, NULL, pstate->scope, genericnode->parms, (*nodep)->args);
        *((INode**)nodep) = cloneNode(&cstate, genericnode->body);
        clonePopState();
    }

    // Now type check the instantiated nodes
    inodeTypeCheck(pstate, (INode **)nodep);
}

