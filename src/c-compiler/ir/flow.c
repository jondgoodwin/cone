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

// If needed, inject an alias node for rc/lex references
void flowInjectAliasNode(INode **nodep, int16_t rvalcount) {
    // No need for injected node if we are not dealing with rc/lex references and if alias calc = 0
    RefNode *reftype = (RefNode *)itypeGetTypeDcl(((ITypedNode*)*nodep)->vtype);
    if (reftype->tag != RefTag || !(reftype->alloc == (INode*)rcAlloc || reftype->alloc == (INode*)lexAlloc))
        return;
    int16_t count = flowAliasGet(0) + rvalcount;
    if (count == 0 || (reftype->alloc == (INode*)lexAlloc && count > 0))
        return;

    // Inject alias count node
    AliasNode *aliasnode;
    newNode(aliasnode, AliasNode, AliasTag);
    aliasnode->exp = *nodep;
    aliasnode->vtype = ((ITypedNode *)*nodep)->vtype;
    aliasnode->aliasamt = count;
    *nodep = (INode*)aliasnode;
}


// Perform data flow analysis on a node whose value we intend to load
// At minimum, we check that it is a valid, readable value
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
        flowInjectAliasNode(nodep, -1);
        break;
    case AddrTag:
        flowInjectAliasNode(nodep, -1);
        addrFlow(fstate, (AddrNode **)nodep);
        break;
    case VTupleTag:
    {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((VTupleNode *)*nodep)->values, cnt, nodesp))
            flowLoadValue(fstate, nodesp);
        break;
    }
    case VarNameUseTag:
    case DerefTag:
    case ArrIndexTag:
    case StrFieldTag:
        flowInjectAliasNode(nodep, 0);
        if (flowAliasGet(0) > 0) {
            flowHandleMove(*nodep);
        }
        break;
    case CastTag:
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

    case ULitTag:
    case FLitTag:
    case StrLitTag:
    case ArrLitTag:
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

// *********************
// Aliasing stack for data flow analysis
//
// As we traverse the IR nodes, this tracks expression "aliasing" in each block.
// Aliasing is when we copy a value. This matters with RC and LEX references.
// *********************

int16_t *gFlowAliasStackp = NULL;
size_t gFlowAliasStackSz = 0;
size_t gFlowAliasStackPos = 0;

// Ensure enough room for alias stack
void flowAliasRoom(size_t highpos) {
    if (highpos >= gFlowAliasStackSz) {
        if (gFlowAliasStackSz == 0) {
            gFlowAliasStackSz = 1024;
            gFlowAliasStackp = (int16_t*)memAllocBlk(gFlowAliasStackSz * sizeof(int16_t));
            memset(gFlowAliasStackp, 0, gFlowAliasStackSz * sizeof(int16_t));
            gFlowAliasStackPos = 0;
        }
        else {
            // Double table size, copying over old data
            int16_t *oldtable = gFlowAliasStackp;
            int oldsize = gFlowAliasStackSz;
            gFlowAliasStackSz <<= 1;
            gFlowAliasStackp = (int16_t*)memAllocBlk(gFlowAliasStackSz * sizeof(int16_t));
            memset(gFlowAliasStackp, 0, gFlowAliasStackSz * sizeof(int16_t));
            memcpy(gFlowAliasStackp, oldtable, oldsize * sizeof(int16_t));
        }
    }
}

// Initialize a function's alias stack
void flowAliasInit() {
    flowAliasRoom(3);
    gFlowAliasStackp[0] = 1;  // current frame's # of aliasing values
    gFlowAliasStackp[1] = 0;  // current frame's start aliasing count
    gFlowAliasStackp[2] = 0;  // Alias count of first value
}

// Start a new frame on alias stack
size_t flowAliasPushNew(int16_t init) {
    size_t svpos = gFlowAliasStackPos;
    int16_t oldstacksz = gFlowAliasStackp[svpos];
    flowAliasRoom(5 + oldstacksz);
    gFlowAliasStackPos += 2 + oldstacksz;
    gFlowAliasStackp[gFlowAliasStackPos] = 1;       // current frame's # of aliasing values
    gFlowAliasStackp[gFlowAliasStackPos+1] = init;  // current frame's start aliasing count
    gFlowAliasStackp[gFlowAliasStackPos + 2] = init; // First value's alias count
    return svpos;
}

// Restore previous stack
void flowAliasPop(size_t oldpos) {
    gFlowAliasStackPos = oldpos;
}

// Reset current frame (to one value initialized to init value)
void flowAliasReset() {
    gFlowAliasStackp[gFlowAliasStackPos] = 1;
    gFlowAliasStackp[gFlowAliasStackPos + 2] = gFlowAliasStackp[gFlowAliasStackPos + 1];
}

// Ensure frame has enough initialized alias counts for 'size' values
void flowAliasSize(int16_t size) {
    int16_t stacksz = gFlowAliasStackp[gFlowAliasStackPos];
    if (stacksz >= size)
        return;
    int16_t init = gFlowAliasStackp[gFlowAliasStackPos + 1];
    while (stacksz < size)
        gFlowAliasStackp[gFlowAliasStackPos + 2 + stacksz++] = init;
    gFlowAliasStackp[gFlowAliasStackPos] = size;
}

// Increment aliasing count at frame's position
void flowAliasIncr(int16_t pos) {
    ++gFlowAliasStackp[gFlowAliasStackPos + 2 + pos];
}

// Get aliasing count at frame's position
int16_t flowAliasGet(size_t pos) {
    return gFlowAliasStackp[gFlowAliasStackPos + 2 + pos];
}