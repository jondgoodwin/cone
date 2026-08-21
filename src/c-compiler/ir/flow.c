/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>

// Deactivate source of a moved value (or say move is illegal)
void flowHandleMove(INode *node) {
    switch (node->tag) {

    // For a variable, mark its value as moved
    case VarNameUseTag: {
        VarDclNode *vardclnode = (VarDclNode *)((NameUseNode*)node)->dclnode;
        vardclnode->flowtempflags |= VarMoved;
        if (vardclnode->scope == 0) {
            errorMsgNode(node, ErrorInvType, "May not move a value out of a global variable.");
        }
        break;
    }

    // Go inwards to find the variable to mark it as moved
    case FldAccessTag:
    case ArrIndexTag: 
        flowHandleMove(((FnCallNode*)node)->objfn);
        break;
    case DerefTag:
        flowHandleMove(((StarNode*)node)->vtexp);
        break;

    // For any other node, no source variable to mark as moved
    default:
        break;
    }
}

// If needed, inject an alias node for rc/own references, adjusting the count by amt.
// One value can become more than one holder at once: an array fill literal stores
// the reference it evaluates once into every one of its elements.
void flowInjectAliasAmt(INode **nodep, int16_t amt) {
    INode *vtype = ((IExpNode*)*nodep)->vtype;
    // No need for injected node if we are not dealing with rc references
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(vtype);
    if (reftype->tag != RefTag || !isRegion(reftype->region, rcName))
        return;

    // Inject alias count node
    AliasNode *aliasnode;
    newNode(aliasnode, AliasNode, AliasTag);
    aliasnode->exp = *nodep;
    aliasnode->vtype = vtype;
    aliasnode->aliasamt = amt;
    aliasnode->counts = NULL;
    *nodep = (INode*)aliasnode;
}

// If needed, inject an alias node for rc/own references
void flowInjectAliasNode(INode **nodep) {
    flowInjectAliasAmt(nodep, 1);
}

// Handle when we know we are either copying or moving a value
// (e.g., for assignment or function arguments).
// Does this expression still hold its value after it is read?
// An lvalue names storage that keeps it; anything else is a temporary.
int flowIsLvalRead(INode *node) {
    switch (node->tag) {
    case VarNameUseTag:
    case DerefTag:
    case ArrIndexTag:
    case FldAccessTag:
        return 1;
    default:
        return 0;
    }
}

void flowHandleMoveOrCopy(INode **nodep) {
    if (iexpIsMove(*nodep)) {
        // Moving needs to deactivate source variable use
        flowHandleMove(*nodep);
    }
    else {
        // A reference count is how many holders exist. Only an lvalue still
        // holds its reference afterwards, so only an lvalue adds a holder. A
        // temporary -- an allocation, a call's result, a literal -- hands over
        // the reference it was born holding, and counting that again would
        // count one holder twice.
        if (flowIsLvalRead(*nodep))
            flowInjectAliasNode(nodep);
    }
}


// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that any expression node holds an accessible, "readable" value
void flowLoadValue(FlowState *fstate, INode **nodep) {
    // Handle specific nodes here - lvals (read check) + literals + fncall
    // fncall + literals? do not need copy check - it can return
    switch ((*nodep)->tag) {
    case BlockTag:
        blockFlow(fstate, (BlockNode **)nodep); break;
    case IfTag:
        ifFlow(fstate, (IfNode **)nodep); break;
    case AssignTag:
        assignFlow(fstate, (AssignNode **)nodep); break;
    case FnCallTag:
        fnCallFlow(fstate, (FnCallNode**)nodep);
        break;
    case ArrayBorrowTag:
    case BorrowTag:
        borrowFlow(fstate, (RefNode **)nodep);
        break;
    case ArrayAllocTag:
    case AllocateTag:
        allocateFlow(fstate, (RefNode **)nodep);
        break;
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        uint32_t index = 0;
        for (nodesFor(((TupleNode *)*nodep)->elems, cnt, nodesp)) {
            flowLoadValue(fstate, nodesp);
        }
        break;
    }
    case VarNameUseTag:
        nameuseFlow(fstate, (NameUseNode**)nodep);
        break;
    case DerefTag:
        derefFlow(fstate, (StarNode**)nodep);
        break;
    case ArrIndexTag:
        fnCallArrIndexFlow(fstate, (FnCallNode**)nodep);
        break;
    case FldAccessTag:
        fnCallFldAccessFlow(fstate, (FnCallNode**)nodep);
        break;
    case CastTag: case IsTag:
        flowLoadValue(fstate, &((CastNode *)*nodep)->exp);
        break;
    case NotLogicTag:
        flowLoadValue(fstate, &((LogicNode *)*nodep)->lexp);
        break;
    case OrLogicTag: case AndLogicTag:
    {
        LogicNode *lnode = (LogicNode*)*nodep;
        flowLoadValue(fstate, &lnode->lexp);
        flowLoadValue(fstate, &lnode->rexp);
        break;
    }

    case TypeLitTag:
        typeLitFlow(fstate, (FnCallNode**)nodep);
        break;

    case ArrayLitTag:
        arrayLitFlow(fstate, (ArrayNode**)nodep);
        break;

    case SizeofTag:
    case NilLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
    case AbsenceTag:
    case UnknownTag:
        break;
    default:
        assert(0);
    }
}

// *********************
// Variable Info stack for data flow analysis
//
// As we traverse the IR nodes, this tracks what we know about a variable in each block:
// - Has it been initialized (and used)?
// - Has it been moved and has it not been moved?
// *********************

// An entry for a local declared name, in which we preserve its flow flags
typedef struct {
    VarDclNode *node;    // The variable declaration node
    int16_t flags;       // The preserved flow flags
} VarFlowInfo;

VarFlowInfo *gVarFlowStackp = NULL;
size_t gVarFlowStackSz = 0;
size_t gVarFlowStackPos = 0;

// Add a just declared variable to the data flow stack
void flowAddVar(VarDclNode *varnode) {
    // Ensure we have room for another variable
    if (gVarFlowStackPos >= gVarFlowStackSz) {
        if (gVarFlowStackSz == 0) {
            gVarFlowStackSz = 1024;
            gVarFlowStackp = (VarFlowInfo*)memAllocBlk(gVarFlowStackSz * sizeof(VarFlowInfo));
            memset(gVarFlowStackp, 0, gVarFlowStackSz * sizeof(VarFlowInfo));
            gVarFlowStackPos = 0;
        }
        else {
            // Double table size, copying over old data
            VarFlowInfo *oldtable = gVarFlowStackp;
            int oldsize = gVarFlowStackSz;
            gVarFlowStackSz <<= 1;
            gVarFlowStackp = (VarFlowInfo*)memAllocBlk(gVarFlowStackSz * sizeof(VarFlowInfo));
            memset(gVarFlowStackp, 0, gVarFlowStackSz * sizeof(VarFlowInfo));
            memcpy(gVarFlowStackp, oldtable, oldsize * sizeof(VarFlowInfo));
        }
    }
    VarFlowInfo *stackp = &gVarFlowStackp[gVarFlowStackPos++];
    stackp->node = varnode;
    stackp->flags = 0;
}

// Start a new scope
size_t flowScopePush() {
    return gVarFlowStackPos;
}

// Is this variable's value the one being handed to the caller, and therefore
// not to be released as the scope ends?
// 'retexp' is the value being returned, or NULL where nothing is: NULL means
// nothing is exempt, not that nothing is released.
// A multi-value return hands back a value tuple, whose elements are exempt one
// by one -- the same walk returnFlowEscape does for the borrow check.
static int flowIsScopeResult(INode *retexp, VarDclNode *varnode) {
    if (retexp == NULL)
        return 0;
    if (retexp->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)retexp)->elems, cnt, elemp)) {
            if (flowIsScopeResult(*elemp, varnode))
                return 1;
        }
        return 0;
    }
    return retexp->tag == VarNameUseTag && ((NameUseNode *)retexp)->namesym == varnode->namesym;
}

// Create de-alias list of all own/rc reference variables (except the retexp name(s))
// As a simple optimization: returns 0 if retexp name was not de-aliased
int flowScopeDealias(size_t startpos, Nodes **varlist, INode *retexp) {
    int doalias = 1;
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        INode *vartype = avar->node->vtype;
        RefNode *reftype = (RefNode*)vartype;
        if (reftype->tag == RefTag && (isRegion(reftype->region, soName) || isRegion(reftype->region, rcName))) {
            // Stopgap: a variable whose value was moved out no longer owns it, so
            // releasing it here would free the new owner's allocation a second
            // time. VarMoved is the state at scope exit rather than at each
            // program point, so a value moved on only one branch is skipped on
            // all of them -- that leaks rather than double-frees. Precise
            // deactivation belongs to the region redesign.
            if (avar->node->flowtempflags & VarMoved)
                continue;
            if (!flowIsScopeResult(retexp, avar->node)) {
                if (*varlist == NULL)
                    *varlist = newNodes(4);
                nodesAdd(varlist, (INode*)avar->node);
            }
            else
                doalias = 0;
        }
        else {
            // Add call to type's drop fn to dealias list, if there is one
            INode *dropfn = itypeGetDropFnDcl(vartype);
            if (dropfn != NULL) {
                FnCallNode *dropfncall = newFnCallLower(retexp, dropfn, 1);
                INode *dropnameuse = (INode*)newNameUseFromDclNode((INode*)avar->node, retexp);
                INode *borrow = newBorrowMutRef(dropnameuse, ((IExpNode*)avar->node)->vtype, (INode*)uniPerm);
                nodesAdd(&dropfncall->args, borrow);
                if (*varlist == NULL)
                    *varlist = newNodes(4);
                nodesAdd(varlist, (INode*)dropfncall);
            }
        }
    }
    return doalias;
}

// Back out of current scope
void flowScopePop(size_t startpos) {
    gVarFlowStackPos = startpos;
}
