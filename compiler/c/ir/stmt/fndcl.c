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
    dclInfoInit(&node->dclinfo);
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
//
// The name's visibility is its candidates': the first candidate declares it,
// and every later one must agree. A private candidate may not join a pub name,
// since the name would make it reachable from outside its owner and a symbol
// that is private and reachable has no sound linkage; and a pub candidate may
// not join a private name, which would hide what it declares visible. A
// compiler-defined intrinsic is not a symbol at all: it counts as pub for the
// name it joins, which is what lets the core types hide '_neg' behind a pub
// '-', and it is exempt from agreeing.
void fnOverloadDclAdd(FnOverloadDclNode *ovlnode, FnDclNode *fnnode) {
    int intrinsic = fnnode->value && fnnode->value->tag == IntrinsicTag;
    int ispub = intrinsic || (fnnode->flags & FlagPub) != 0;
    if (ovlnode->overloads->used == 0) {
        if (ispub)
            ovlnode->flags |= FlagPub;
    }
    else if (!intrinsic && ispub != ((ovlnode->flags & FlagPub) != 0)) {
        errorMsgNode((INode*)fnnode, ErrorPrivOverload, ispub
            ? "%s is pub, so it may not join the private overload name %s; make both pub or neither."
            : "%s is private, so it may not join the pub overload name %s; make both pub or neither.",
            &fnnode->namesym->namestr, &ovlnode->namesym->namestr);
        return;
    }
    nodesAdd(&ovlnode->overloads, (INode*)fnnode);
    ovlnode->flags |= fnnode->flags & FlagMethFld;
}

// The copy of a function/method declaration, before its signature and body are
// copied: they still point at the original's. Split from filling it in so a
// generic type's clone can bind every member's copy before any body that may
// name one is copied (cloneStructNode).
FnDclNode *cloneFnDclShell(FnDclNode *oldfn) {
    FnDclNode *newnode = memAllocBlk(sizeof(FnDclNode));
    memcpy(newnode, oldfn, sizeof(FnDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    newnode->genericinfo = NULL;
    return newnode;
}

// Copy the original's signature and body into its shell
void cloneFnDclFill(CloneState *cstate, FnDclNode *newnode, FnDclNode *oldfn) {
    uint32_t dclpos = cloneDclPush();
    newnode->vtype = cloneNode(cstate, oldfn->vtype);
    newnode->value = cloneNode(cstate, oldfn->value);
    cloneDclPop(dclpos);
}

// Return a clone of a function/method declaration
INode *cloneFnDclNode(CloneState *cstate, FnDclNode *oldfn) {
    FnDclNode *newnode = cloneFnDclShell(oldfn);
    cloneFnDclFill(cstate, newnode, oldfn);
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
    dclInfoPrint((INode*)node);
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

// Whether an importer expands this function's body in its own object rather than
// calling a symbol: an inline or generic function, a trait's default (cloned
// into each implementer), a module trait's default (cloned into each conforming
// module), and any method of a generic type (cloned into each instance).
// 'typenode' is the type or module trait whose braces declare it, or NULL.
int fnDclIsExpanded(FnDclNode *fndclnode, INode *typenode) {
    if ((fndclnode->flags & FlagInline) || fndclnode->genericinfo)
        return 1;
    if (typenode && typenode->tag == ModTraitTag)
        return 1;
    if (typenode && typenode->tag == StructTag
        && (((StructNode*)typenode)->genericinfo || (typenode->flags & TraitType)))
        return 1;
    return 0;
}

// Resolve all names in a function
void fnDclNameRes(NameResState *nstate, FnDclNode *fndclnode) {
    INode **nodesp;
    uint32_t cnt;

    // What an expanded body names is marked by nameUseNameRes, so a library
    // compile can give it a symbol an importer links against. A function nested
    // in such a body is part of it, so it inherits the expander.
    INode *svexpander = nstate->expander;
    if (fnDclIsExpanded(fndclnode, nstate->typenode))
        nstate->expander = (INode*)fndclnode;

    nametblHookPush();
    // Resolve generic parameters inside the hooked context. Resolving one hooks
    // it, so doing it before the push would bind it in the enclosing scope and
    // the matching pop would never remove it.
    if (fndclnode->genericinfo) {
        for (nodesFor(fndclnode->genericinfo->parms, cnt, nodesp))
            inodeNameRes(nstate, nodesp);
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
    nstate->expander = svexpander;
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
void fnDclTypeCheck(TypeCheckState *pstate, FnDclNode *fnnode) {
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

    // No need to type check function body if no body or is a default method of a
    // trait, or a module trait's default: each is checked in the copy its
    // implementer or conforming module owns, where its names are that one's
    if (!fnnode->value
        || ((fnnode->flags & FlagMethFld) && pstate->typenode->tag == StructTag && (pstate->typenode->flags & TraitType))
        || (pstate->typenode && pstate->typenode->tag == ModTraitTag))
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
    // A module's 'init' starts with its module's uninitialized globals holding
    // nothing, as a local does, and must leave each one assigned
    ModuleNode *initmod = modInitOf(fnnode);
    uint16_t *saved = initmod ? modInitFlowBegin(initmod) : NULL;
    blockFlow(&fstate, (BlockNode **)&fnnode->value);
    if (initmod)
        modInitFlowEnd(initmod, saved);
}

// Verify no two candidates of an overload set accept the same parameter signature.
// Candidates are not walked, as each is separately name resolved and type checked
// by the module or type that owns its concrete declaration.
void fnOverloadDclTypeCheck(TypeCheckState *pstate, FnOverloadDclNode *node) {
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
