/** Handling for gennode declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new generic info block
GenericInfo *newGenericInfo() {
    GenericInfo *geninfo = (GenericInfo*)memAllocBlk(sizeof(GenericInfo));
    geninfo->parms = NULL;
    geninfo->memonodes = NULL;
    return geninfo;
}

// Serialize
void genericInfoPrint(GenericInfo *info) {
    INode **nodesp;
    uint32_t cnt;
    inodeFprint("[");
    for (nodesFor(info->parms, cnt, nodesp)) {
        inodePrintNode(*nodesp);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint("] ");
}

// Inference found an argtype that maps to a generic parmtype
// Capture it, and return 0 if it does not match what we already thought it was
int genericCaptureType(FnCallNode *gencall, Nodes *genparms, INode *parmtype, INode *argtype) {
    INode **genvarp;
    uint32_t genvarcnt;
    INode **genargp = &nodesGet(gencall->args, 0);
    for (nodesFor(genparms, genvarcnt, genvarp)) {
        Name *genvarname = ((GenVarDclNode *)(*genvarp))->namesym;
        // Found parameter with corresponding name? Capture/check type
        if (genvarname == ((NameUseNode*)parmtype)->namesym) {
            if (*genargp == NULL)
                *genargp = argtype;
            else if (!itypeIsSame(*genargp, argtype))
                return 0;
            break;
        }
        ++genargp;
    }
    return 1;
}

// Infer generic type parameters (inferredgencall) from the type literal arguments (srcgencallp)
int genericInferStructParms(TypeCheckState *pstate, Nodes *genparms, StructNode *genstruct, 
        FnCallNode *srcgencall, FnCallNode *inferredgencall) {

    // Reorder the literal's arguments to match the type's field order
    if (typeLitStructReorder(srcgencall, genstruct, (INode*)genstruct == pstate->typenode) == 0)
        return 0;

    // Iterate through arguments and expected parms
    int retcode = 1;
    uint32_t cnt;
    INode **argsp;
    INode **parmp = &nodelistGet(&genstruct->fields, 0);
    for (nodesFor(srcgencall->args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((FieldDclNode *)*argsp)->vtype;
        // If type of expected parm is a generic variable, capture type of corresponding argument
        if (parmtype->tag == GenVarUseTag
            && genericCaptureType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic type");
            retcode = 0;
        }
        ++parmp;
    }
    return retcode;
}

// Infer generic type parameters from the function call arguments
int genericInferFnParms(TypeCheckState *pstate, Nodes *genparms, FnSigNode *genfnsig, 
        FnCallNode *srcgencall, FnCallNode *inferredgencall) {

    if (srcgencall->args->used > genfnsig->parms->used) {
        errorMsgNode((INode*)srcgencall, ErrorFewArgs, "Too many arguments provided for generic function.");
        return 0;
    }

    // Iterate through arguments and expected parms
    int retcode = 1;
    INode **argsp;
    uint32_t cnt;
    INode **parmp = &nodesGet(genfnsig->parms, 0);
    for (nodesFor(srcgencall->args, cnt, argsp)) {
        INode *parmtype = ((VarDclNode *)(*parmp))->vtype;
        INode *argtype = ((IExpNode *)*argsp)->vtype;
        // If type of expected parm is a generic variable, capture type of corresponding argument
        if (parmtype->tag == GenVarUseTag
            && genericCaptureType(inferredgencall, genparms, parmtype, argtype) == 0) {
            errorMsgNode(*argsp, ErrorInvType, "Inconsistent type for generic function");
            retcode = 0;
        }
        ++parmp;
    }
    return retcode;
}

// Verify arguments are types, check if instantiated, instantiate if needed and return ptr to it
INode *genericMemoize(TypeCheckState *pstate, FnCallNode *srcgencall, INode *nodetoclone, 
        GenericInfo *genericinfo, Name *name) {

    // Verify expected number of generic paraameters
    uint32_t expected = genericinfo->parms ? genericinfo->parms->used : 0;
    if (srcgencall->args->used != expected) {
        errorMsgNode((INode*)srcgencall, ErrorManyArgs, "Incorrect number of arguments vs. parameters expected");
        return NULL;
    }

    // Verify all arguments are types
    INode **nodesp;
    uint32_t cnt;
    int badargs = 0;
    for (nodesFor(srcgencall->args, cnt, nodesp)) {
        if (!isTypeNode(*nodesp)) {
            errorMsgNode((INode*)*nodesp, ErrorManyArgs, "Expected a type for a generic parameter");
            badargs = 1;
        }
    }
    if (badargs)
        return NULL;

    if (!genericinfo->memonodes)
        genericinfo->memonodes = newNodes(2);

    // Check whether these types have already been instantiated for this generic
    // memonodes holds pairs of nodes: an FnCallNode and what it instantiated
    // A match is the first FnCallNode whose types match what we want
    for (nodesFor(genericinfo->memonodes, cnt, nodesp)) {
        FnCallNode *fncallprior = (FnCallNode *)*nodesp;
        nodesp++; cnt--; // skip to instance srcgencallp
        int match = 1;
        INode **priornodesp;
        uint32_t priorcnt;
        INode **nownodesp = &nodesGet(srcgencall->args, 0);
        for (nodesFor(fncallprior->args, priorcnt, priornodesp)) {
            if (!itypeIsSame(*priornodesp, *nownodesp)) {
                match = 0;
                break;
            }
            nownodesp++;
        }
        if (match) {
            // Return a namenode pointing to dcl instance
            NameUseNode *fnuse = newNameUseNode(name);
            fnuse->tag = isTypeNode(*nodesp) ? TypeNameUseTag : VarNameUseTag;
            fnuse->dclnode = *nodesp;
            return (INode *)fnuse;
        }
    }

    // No match found, instantiate the dcl generic
    CloneState cstate;
    clonePushState(&cstate, (INode*)srcgencall, NULL, pstate->scope, genericinfo->parms, srcgencall->args);
    INode *instance = cloneNode(&cstate, nodetoclone);
    clonePopState();

    // Type check the instanced declaration
    inodeTypeCheckAny(pstate, &instance);

    // Remember instantiation for the future
    nodesAdd(&genericinfo->memonodes, (INode*)srcgencall);
    nodesAdd(&genericinfo->memonodes, instance);

    // Return a namenode pointing to fndcl instance
    NameUseNode *fnuse = newNameUseNode(name);
    fnuse->tag = isTypeNode(instance) ? TypeNameUseTag : VarNameUseTag;
    fnuse->dclnode = instance;
    return (INode *)fnuse;
}

// Obtain GenericInfo from node, if it exists
GenericInfo *genericGetInfo(INode *node) {
    switch (node->tag) {
    case FnDclTag:
        return ((FnDclNode *)node)->genericinfo;
    case StructTag:
        return ((StructNode *)node)->genericinfo;
    default:
        return NULL;
    }
}

// Perform generic substitution, if this is a correctly set up generic "srcgencall"
// Return 1 if generic subsituted or error. Return 0 if not generic or it leaves behind a lit/srcgencall that needs processing.
int genericSubstitute(TypeCheckState *pstate, FnCallNode **srcgencallp) {
    // Return if not generic, otherwise gather data needed to substitute
    FnCallNode *srcgencall = *srcgencallp;
    if (srcgencall->objfn->tag != VarNameUseTag && srcgencall->objfn->tag != TypeNameUseTag)
        return 0;
    INode *nodetoclone = ((NameUseNode*)srcgencall->objfn)->dclnode;
    GenericInfo *genericinfo = genericGetInfo(nodetoclone);
    if (!genericinfo)
        return 0;
    Name *name = inodeGetName(nodetoclone);

    // Decide whether generic requires inference of type parameters
    // For now, we decide based on whether any parameter is a type
    int usesTypeArgs = 0;
    INode **argsp;
    uint32_t cnt;
    if (srcgencall->args) {
        for (nodesFor(srcgencall->args, cnt, argsp)) {
            if (isTypeNode(*argsp))
                usesTypeArgs = 1;
        }
    }

    // Since the arguments are types, no inference is needed
    // Replace gennnone with instantiated generic, substituting parameters
    // Then type check the substituted, instantiated srcgencallp
    if (usesTypeArgs) {
        *((INode**)srcgencallp) = genericMemoize(pstate, srcgencall, nodetoclone, genericinfo, name);
        inodeTypeCheckAny(pstate, (INode **)srcgencallp);
        return 1;
    }

    // Before we can get a generic instantiation, we have to first infer the type parameters from genfncall arguments.
    // We know arguments have been type checked
    // If successful, we are left with an instantiated function call/type that FnCall still needs to type-check.

    // Inject empty generic call as the place to fill in and hold the inferred type parameters
    uint32_t nparms = genericinfo->parms->used;
    FnCallNode *inferredgencall = newFnCallNode(srcgencall->objfn, nparms);
    srcgencall->objfn = (INode*)inferredgencall;
    while (nparms--)
        nodesAdd(&inferredgencall->args, (INode*)NULL);

    // Inference varies depending on the kind of generic
    switch (nodetoclone->tag) {
    case FnDclTag:
        // Infer based on function call arguments
        if (genericInferFnParms(pstate, genericinfo->parms,
            (FnSigNode*)itypeGetTypeDcl(((FnDclNode *)nodetoclone)->vtype), srcgencall, inferredgencall) == 0)
            return 1;
        break;
    case StructTag:
        // Infer based on type constructor arguments
        if (genericInferStructParms(pstate, genericinfo->parms, (StructNode*)itypeGetTypeDcl(nodetoclone), srcgencall, inferredgencall) == 0)
            return 1;
        break;
    default:
        assert(0 && "Illegal generic type.");
    }

    // Be sure all expected generic parameters were inferred
    for (nodesFor(inferredgencall->args, cnt, argsp)) {
        if (*argsp == NULL) {
            errorMsgNode((INode*)srcgencall, ErrorInvType, "Could not infer all of generic's type parameters.");
            return 1;
        }
    }

    // Now let's instantiate generic "call", substituting instantiated srcgencallp in objfn
    *((INode**)&srcgencall->objfn) = genericMemoize(pstate, (FnCallNode*)srcgencall->objfn, nodetoclone, genericinfo, name);

    return 0;
}
