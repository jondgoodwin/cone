/** Handling for function/method declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
FnDclNode *newFnDclNode(Name *namesym, uint16_t flags, INode *type, INode *val) {
    FnDclNode *name;
    newNode(name, FnDclNode, FnDclTag);
    name->flags = flags;
    name->vtype = type;
    name->owner = NULL;
    name->namesym = namesym;
    name->value = val;
    name->llvmvar = NULL;
    name->nextnode = NULL;
    return name;
}

// Serialize a function node
void fnDclPrint(FnDclNode *name) {
    if (name->namesym)
        inodeFprint("fn %s", &name->namesym->namestr);
    else
        inodeFprint("fn");
    inodePrintNode(name->vtype);
    if (name->value) {
        inodeFprint(" {} ");
        if (name->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(name->value);
    }
}

// Resolve all names in a function
void fnDclNameResolve(PassState *pstate, FnDclNode *name) {
    inodeWalk(pstate, &name->vtype);
    if (!name->value)
        return;

    int16_t oldscope = pstate->scope;
    pstate->scope = 1;

    // Hook function's parameters into global name table
    // so that when we walk the function's logic, parameter names are resolved
    FnSigNode *fnsig = (FnSigNode*)name->vtype;
    nametblHookPush();
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fnsig->parms, cnt, nodesp))
        nametblHookNode((INamedNode *)*nodesp);

    inodeWalk(pstate, &name->value);

    nametblHookPop();
    pstate->scope = oldscope;
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(INode *rettype, BlockNode *blk) {
    INode *laststmt;
    if (blk->stmts->used == 0)
        nodesAdd(&blk->stmts, (INode*)newReturnNode());
    laststmt = nodesLast(blk->stmts);
    if (rettype == voidType) {
        if (laststmt->tag != ReturnTag)
            nodesAdd(&blk->stmts, (INode*)newReturnNode());
    }
    else {
        // Inject return in front of expression
        if (isExpNode(laststmt)) {
            ReturnNode *retnode = newReturnNode();
            retnode->exp = laststmt;
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
void fnDclTypeCheck(PassState *pstate, FnDclNode *fnnode) {
    inodeWalk(pstate, &fnnode->vtype);
    if (!fnnode->value)
        return;

    // Syntactic sugar: Turn implicit returns into explicit returns
    fnImplicitReturn(((FnSigNode*)fnnode->vtype)->rettype, (BlockNode *)fnnode->value);

    // Type check/inference of the function's logic
    FnSigNode *oldfnsig = pstate->fnsig;
    pstate->fnsig = (FnSigNode*)fnnode->vtype;   // needed for return type check
    inodeWalk(pstate, &fnnode->value);
    pstate->fnsig = oldfnsig;

    // Immediately perform the data flow pass for this function
    // We run data flow separately as it requires type info which is inferred bottoms-up
    if (errors)
        return;
    flowAliasInit();
    FlowState fstate;
    fstate.fnsig = (FnSigNode *)fnnode->vtype;
    fstate.scope = 1;
    blockFlow(&fstate, (BlockNode **)&fnnode->value);
}

// Check the function declaration node
void fnDclPass(PassState *pstate, FnDclNode *name) {
    INode *vtype = iexpGetTypeDcl(name->vtype);

    // Process nodes in name's initial value/code block
    switch (pstate->pass) {
    case NameResolution:
        fnDclNameResolve(pstate, name);
        break;

    case TypeCheck:
        fnDclTypeCheck(pstate, name);
        break;
    }
}
