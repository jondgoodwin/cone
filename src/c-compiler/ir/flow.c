/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>

void flowHandleMove(INode *node) {
    int copytrait = itypeCopyTrait(((ITypedNode *)node)->vtype);
    if (copytrait != CopyBitwise)
        errorMsgNode(node, WarnCopy, "No current support for move. Be sure this is safe!");
    // if CopyMethod - inject use of that method to create a safe clone that can be "moved"
    // if CopyMove - turn off access to the source (via static (local var) or dynamic mechanism)
}

// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that it is a valid, readable value
// copyflag indicates whether value is to be copied or moved
// If copied, we may need to alias it. If moved, we may have to deactivate its source.
void flowLoadValue(FlowState *fstate, INode **nodep, int copyflag) {
    // Handle specific nodes here - lvals (read check) + literals + fncall
    // fncall + literals? do not need copy check - it can return
    switch ((*nodep)->tag) {
    case BlockTag:
        blockFlow(fstate, (BlockNode **)nodep, copyflag); break;
    case IfTag:
        ifFlow(fstate, (IfNode **)nodep, copyflag); break;
    case AssignTag:
        assignFlow(fstate, (AssignNode **)nodep); break;
    case FnCallTag:
        fnCallFlow(fstate, (FnCallNode**)nodep);
        break;
    case AddrTag:
        addrFlow(fstate, (AddrNode **)nodep); break;
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((VTupleNode *)*nodep)->values, cnt, nodesp))
            flowLoadValue(fstate, nodesp, copyflag);
        break;
    }
    case VarNameUseTag:
    case DerefTag:
    case ArrIndexTag:
    case StrFieldTag:
        if (copyflag) {
            flowHandleMove(*nodep);
        }
        break;
    case CastTag:
        flowLoadValue(fstate, &((CastNode *)*nodep)->exp, copyflag);
        break;
    case NotLogicTag:
        flowLoadValue(fstate, &((LogicNode *)*nodep)->lexp, 0);
        break;
    case OrLogicTag: case AndLogicTag:
    {
        LogicNode *lnode = (LogicNode*)*nodep;
        flowLoadValue(fstate, &lnode->lexp, 0);
        flowLoadValue(fstate, &lnode->rexp, 0);
        break;
    }

    case ULitTag:
    case FLitTag:
    case StrLitTag:
    case ArrLitTag:
        break;
    default:
        assert(0);
    }

}

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

// Back out of current scope
void flowScopePop(size_t startpos, Nodes **varlist) {
    size_t pos = gVarFlowStackPos;
    while (pos > startpos) {
        VarFlowInfo *avar = &gVarFlowStackp[--pos];
        RefNode *reftype = (RefNode*)avar->node->vtype;
        if (reftype->tag == RefTag && reftype->alloc != voidType) {
            if (*varlist == NULL)
                *varlist = newNodes(4);
            nodesAdd(varlist, (INode*)avar->node);
        }
    }

    gVarFlowStackPos = pos;
}
