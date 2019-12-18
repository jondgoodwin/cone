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
    inodeTypeCheckAny(pstate, (INode**)gennode);
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
            fnuse->tag = isTypeNode(*nodesp)? TypeNameUseTag : VarNameUseTag;
            fnuse->dclnode = *nodesp;
            return (INode *)fnuse;
        }
    }

    // No match found, instantiate the dcl generic
    CloneState cstate;
    clonePushState(&cstate, (INode*)fncall, NULL, pstate->scope, genericnode->parms, fncall->args);
    INode *instance = cloneNode(&cstate, genericnode->body);
    clonePopState();

    // Type check the instanced declaration
    inodeTypeCheckAny(pstate, &instance);

    // Remember instantiation for the future
    nodesAdd(&genericnode->memonodes, (INode*)fncall);
    nodesAdd(&genericnode->memonodes, instance);

    // Return a namenode pointing to fndcl instance
    NameUseNode *fnuse = newNameUseNode(genericnode->namesym);
    fnuse->tag = isTypeNode(instance) ? TypeNameUseTag : VarNameUseTag;
    fnuse->dclnode = instance;
    return (INode *)fnuse;
}

// For a generic function, create a generic call node whose parameters are inferred from the function call arguments
int genericFnInferVars(GenericNode *genericnode, FnCallNode **nodep) {
    FnCallNode *node = *nodep;

    // Inject empty generic call node
    uint32_t nparms = genericnode->parms->used;
    FnCallNode *gencall = newFnCallNode((*nodep)->objfn, nparms);
    node->objfn = (INode*)gencall;
    while (nparms--)
        nodesAdd(&gencall->args, (INode*)NULL);
    
    FnSigNode *fnsig = (FnSigNode*)itypeGetTypeDcl(((FnDclNode *)genericnode->body)->vtype);
    if (node->args->used > fnsig->parms->used) {
        errorMsgNode((INode*)node, ErrorFewArgs, "Too many arguments provided for generic function.");
        return 0;
    }

    // Iterate through arguments and expected parms
    int badargs = 0;
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(fnsig->parms, 0);
    for (nodesFor(node->args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((IExpNode *)*argsp)->vtype;
        // If type of expected parm is a generic variable, capture type of corresponding argument
        if (parmtype->tag == GenVarUseTag) {
            INode **genvarp;
            uint32_t genvarcnt;
            INode **genargp = &nodesGet(gencall->args, 0);
            for (nodesFor(genericnode->parms, genvarcnt, genvarp)) {
                Name *genvarname = ((GenVarDclNode *)(*genvarp))->namesym;
                // Found parameter with corresponding name? Capture/check type
                if (genvarname == ((NameUseNode*)parmtype)->namesym) {
                    if (*genargp == NULL)
                        *genargp = argtype;
                    else if (!itypeIsSame(*genargp, argtype)) {
                        errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic function");
                        badargs = 1;
                    }
                    break;
                }
                ++genargp;
            }
        }
        ++parmp;
    }
    if (badargs)
        return 0;

    // Be sure all expected generic parameters were inferred
    for (nodesFor(gencall->args, cnt, argsp)) {
        if (*argsp == NULL) {
            errorMsgNode(*argsp, ErrorInvType, "Could not infer all of generic function's type parameters.");
            return 0;
        }
    }

    return 1;
}

// Instantiate a generic using passed arguments
void genericCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep) {
    FnCallNode **orignodep = nodep;
    GenericNode *genericnode = (GenericNode*)((NameUseNode*)(*nodep)->objfn)->dclnode;
    int isGeneric = genericnode->flags & GenericMemoize; // vs. macro

    // If first argument is an expression, assume we need to infer generic variables
    if (isGeneric && (*nodep)->args->used > 0 && isExpNode(nodesGet((*nodep)->args, 0))) {
        if (genericnode->body->tag == FnDclTag) {
            if (genericFnInferVars(genericnode, nodep) == 0)
                return;
            nodep = (FnCallNode **)&(*nodep)->objfn;
            genericnode = (GenericNode*)((NameUseNode*)(*nodep)->objfn)->dclnode;
        }
    }

    uint32_t expected = genericnode->parms ? genericnode->parms->used : 0;
    if ((*nodep)->args->used != expected) {
        errorMsgNode((INode*)*nodep, ErrorManyArgs, "Incorrect number of arguments vs. parameters expected");
        return;
    }

    // Replace gennode call with instantiated body, substituting parameters
    if (isGeneric)
        *((INode**)nodep) = genericMemoize(pstate, *nodep);
    else {
        CloneState cstate;
        clonePushState(&cstate, (INode*)*nodep, NULL, pstate->scope, genericnode->parms, (*nodep)->args);
        *((INode**)nodep) = cloneNode(&cstate, genericnode->body);
        clonePopState();
    }

    // Now type check the instantiated nodes
    inodeTypeCheckAny(pstate, (INode **)orignodep);
}

