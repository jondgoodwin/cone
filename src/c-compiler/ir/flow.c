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

// If needed, inject an alias node for rc/own references
void flowInjectAliasNode(INode **nodep) {
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
    aliasnode->aliasamt = 1;
    aliasnode->counts = NULL;
    *nodep = (INode*)aliasnode;
}

// Handle when we know we are either copying or moving a value
// (e.g., for assignment or function arguments).
void flowHandleMoveOrCopy(INode **nodep) {
    uint16_t moveflag = itypeGetTypeDcl(((IExpNode *)*nodep)->vtype)->flags & MoveType;
    if (iexpIsMove(*nodep)) {
        // Moving needs to deactivate source variable use
        flowHandleMove(*nodep);
    }
    else {
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

    case SizeofTag:
    case NilLitTag:
    case ULitTag:
    case FLitTag:
    case StringLitTag:
    case TypeLitTag:
    case ArrayLitTag:
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

// Create de-alias list of all own/rc reference variables (except single retexp name)
// As a simple optimization: returns 0 if retexp name was not de-aliased
int flowScopeDealias(size_t startpos, Nodes **varlist, INode *retexp) {
    int doalias = 1;
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        RefNode *reftype = (RefNode*)avar->node->vtype;
        if (reftype->tag == RefTag && (isRegion(reftype->region, soName) || isRegion(reftype->region, rcName))) {
            if (retexp && (retexp->tag != VarNameUseTag || ((NameUseNode *)retexp)->namesym != avar->node->namesym)) {
                if (*varlist == NULL)
                    *varlist = newNodes(4);
                nodesAdd(varlist, (INode*)avar->node);
            }
            else
                doalias = 0;
        }
    }
    return doalias;
}

// Back out of current scope
void flowScopePop(size_t startpos) {
    gVarFlowStackPos = startpos;
}
