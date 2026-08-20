/** Handling for function/method declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new function declaraction node
FnDclNode *newFnDclNode(Name *namesym, uint16_t flags, INode *type, INode *val) {
    FnDclNode *node;
    newNode(node, FnDclNode, FnDclTag);
    node->flags = flags;
    node->vtype = type;
    node->namesym = namesym;
    node->overloadsym = NULL;
    node->value = val;
    node->llvmvar = NULL;
    node->genname = namesym? &namesym->namestr : "";
    node->genericinfo = NULL;
    return node;
}

// Create a new overloaded function/method declaration node
FnOverloadDclNode *newFnOverloadDclNode(Name *namesym) {
    FnOverloadDclNode *node;
    newNode(node, FnOverloadDclNode, FnOverloadDclTag);
    node->namesym = namesym;
    node->overloads = newNodes(2);
    return node;
}

// Append a concrete declaration to an overload set's ordered candidates.
// The set is a method set when any of its candidates is a method, which is
// what lets an unqualified use be rewritten to 'self.name'.
void fnOverloadDclAdd(FnOverloadDclNode *ovlnode, FnDclNode *fnnode) {
    nodesAdd(&ovlnode->overloads, (INode*)fnnode);
    ovlnode->flags |= fnnode->flags & FlagMethFld;
}

// Return a clone of a function/method declaration
INode *cloneFnDclNode(CloneState *cstate, FnDclNode *oldfn) {
    uint32_t dclpos = cloneDclPush();
    FnDclNode *newnode = memAllocBlk(sizeof(FnDclNode));
    memcpy(newnode, oldfn, sizeof(FnDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    newnode->genericinfo = NULL;
    newnode->vtype = cloneNode(cstate, oldfn->vtype);
    newnode->value = cloneNode(cstate, oldfn->value);
    cloneDclPop(dclpos);
    return (INode*)newnode;
}

// Serialize a function node
void fnDclPrint(FnDclNode *node) {
    if (node->namesym)
        inodeFprint("fn %s", &node->namesym->namestr);
    else
        inodeFprint("fn");
    if (node->genericinfo)
        genericInfoPrint(node->genericinfo);
    if (node->overloadsym)
        inodeFprint(" overload %s ", &node->overloadsym->namestr);
    inodePrintNode(node->vtype);
    if (node->value) {
        inodeFprint(" {} ");
        if (node->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(node->value);
    }
}

// Serialize an overloaded function/method declaration node.
// Only each candidate's concrete name and signature are printed, as each
// candidate is separately printed by the module or type that owns it.
void fnOverloadDclPrint(FnOverloadDclNode *node) {
    inodeFprint("overload %s", &node->namesym->namestr);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->overloads, cnt, nodesp)) {
        FnDclNode *candidate = (FnDclNode *)*nodesp;
        inodeFprint(" %s ", candidate->namesym? &candidate->namesym->namestr : "");
        inodePrintNode(candidate->vtype);
    }
}

// Resolve all names in a function
void fnDclNameRes(AnalysisState *nstate, FnDclNode *fndclnode) {
    // Resolve generic parameters
    INode **nodesp;
    uint32_t cnt;
    if (fndclnode->genericinfo) {
        for (nodesFor(fndclnode->genericinfo->parms, cnt, nodesp))
            inodeNameRes(nstate, nodesp);
    }

    nametblHookPush();
    if (fndclnode->genericinfo) {
        // Hook generic parms so we can resolve them throughout type
        for (nodesFor(fndclnode->genericinfo->parms, cnt, nodesp))
            nametblHookNode(((VarDclNode *)*nodesp)->namesym, *nodesp);
    }
    inodeNameRes(nstate, &fndclnode->vtype);

    if (fndclnode->value) {

        uint16_t oldscope = nstate->scope;
        nstate->scope = 1;

        // Hook function's parameters into global fndclnode table
        // so that when we walk the function's logic, parameter names are resolved
        FnSigNode *fnsig = (FnSigNode*)fndclnode->vtype;
        for (nodesFor(fnsig->parms, cnt, nodesp))
            nametblHookNode(((VarDclNode *)*nodesp)->namesym, *nodesp);

        inodeNameRes(nstate, &fndclnode->value);

        nstate->scope = oldscope;
    }

    nametblHookPop();
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(INode *rettype, BlockNode *blk) {
    INode *laststmt;
    if (blk->stmts->used == 0)
        nodesAdd(&blk->stmts, (INode*)newReturnNodeExp((INode*)newNilLitNode()));
    laststmt = nodesLast(blk->stmts);
    if (rettype->tag == VoidTag) {
        if (laststmt->tag != ReturnTag)
            nodesAdd(&blk->stmts, (INode*)newReturnNodeExp((INode*)newNilLitNode()));
    }
    else {
        // Inject return in front of expression
        if (isExpOrMacroNode(laststmt)) {
            BreakRetNode *retnode = newReturnNodeExp(laststmt);
            nodesLast(blk->stmts) = (INode*)retnode;
        }
        else if (laststmt->tag != ReturnTag)
            errorMsgNode(laststmt, ErrorNoRet, "A return value is expected but this statement cannot give one.");
    }
}

// Type checking a function's logic does more than you might think:
// - Turn implicit returns into explicit returns
// - Perform type checking for all statements
// - Perform data flow analysis on variables and references
void fnDclTypeCheck(AnalysisState *pstate, FnDclNode *fnnode) {
    // Wait until a generic function is instantiated before type checking
    if (fnnode->genericinfo)
        return;

    // Data flow runs only on a function this pass left well typed. Remember the
    // error count on the way in, so that test is about this function alone: an
    // earlier failure elsewhere in the compile must not silence immutability,
    // move and lifetime checking for every function that follows it.
    int errorsOnEntry = errors;

    itypeTypeCheck(pstate, &fnnode->vtype);

    // A body is not checked against a signature that failed: every use of the
    // types that check was supposed to establish would report again, naming
    // nothing the author can act on. This is the same shape as the flow gate
    // below -- the count this call entered with, so that it is about this
    // declaration alone and not about whatever failed elsewhere.
    if (errors != errorsOnEntry)
        return;

    // No need to type check function body if no body or is a default method of a trait
    if (!fnnode->value 
        || ((fnnode->flags & FlagMethFld) && pstate->typenode->tag == StructTag && (pstate->typenode->flags & TraitType)))
        return;

    // Ensure self parameter on a method is (reference to) its enclosing type
    if (fnnode->flags & FlagMethFld) {
        INode *selfparm = nodesGet(((FnSigNode *)(fnnode->vtype))->parms, 0);
        if (iexpGetDerefTypeDcl(selfparm) != pstate->typenode)
            errorMsgNode((INode*)fnnode, ErrorInvType, "self parameter for a method must match, or be a reference to, its type");
    }

    // Syntactic sugar: Turn implicit returns into explicit returns
    fnImplicitReturn(((FnSigNode*)fnnode->vtype)->rettype, (BlockNode *)fnnode->value);

    // Type check/inference of the function's logic.
    //
    // Rule 8: this declaration may have been reached by demand, from the middle
    // of some other function's body, so the walk context describes somewhere
    // else. Saving and resetting both is what makes analyzing a declaration
    // independent of where it was analyzed from. Scope 1 is the signature's,
    // matching what fnDclNameRes sets, so the body's own block is scope 2.
    FnDclNode *svFn = pstate->fn;
    uint16_t svScope = pstate->scope;
    pstate->fn = fnnode;
    pstate->scope = 1;
    inodeTypeCheck(pstate, &fnnode->value, noCareType);
    pstate->scope = svScope;
    pstate->fn = svFn;

    // Immediately perform the data flow pass for this function
    // We run data flow separately as it requires type info which is inferred bottoms-up
    // Skip it when this function's own signature or body did not type check, as
    // flow analysis relies on the types that check was supposed to establish.
    if (errors != errorsOnEntry)
        return;
    FlowState fstate;
    fstate.fnsig = (FnSigNode *)fnnode->vtype;
    fstate.scope = 1;
    blockFlow(&fstate, (BlockNode **)&fnnode->value);
}

// Verify no two candidates of an overload set accept the same parameter signature.
// Candidates are not walked, as each is separately name resolved and type checked
// by the module or type that owns its concrete declaration.
void fnOverloadDclTypeCheck(AnalysisState *pstate, FnOverloadDclNode *node) {
    INode **nodesp;
    uint32_t cnt;
    uint32_t index = 0;
    for (nodesFor(node->overloads, cnt, nodesp)) {
        FnDclNode *candidate = (FnDclNode *)*nodesp;
        for (uint32_t prior = 0; prior < index; ++prior) {
            FnDclNode *earlier = (FnDclNode *)nodesGet(node->overloads, prior);
            if (fnSigParmsEqual((FnSigNode *)earlier->vtype, (FnSigNode *)candidate->vtype)) {
                errorMsgNode((INode*)candidate, ErrorDupOverload,
                    "%s accepts the same arguments as %s, so overload %s could never choose between them.",
                    &candidate->namesym->namestr, &earlier->namesym->namestr, &node->namesym->namestr);
                break;
            }
        }
        ++index;
    }
}
