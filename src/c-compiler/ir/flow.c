/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>
#include <memory.h>

// Dispatch a node walk for the data flow pass
// - fstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void flowWalk(FlowState *fstate, INode **node) {
	switch ((*node)->tag) {
    case BlockTag:
        blockFlow(fstate, (BlockNode **)node); break;
    case IfTag:
        ifFlow(fstate, (IfNode **)node); break;
    case WhileTag:
        whileFlow(fstate, (WhileNode **)node); break;
    case AssignTag:
        assignFlow(fstate, (AssignNode **)node); break;
    case AddrTag:
        addrFlow(fstate, (AddrNode **)node); break;
    case VarDclTag:
        varDclFlow(fstate, (VarDclNode **)node); break;
    case FnDclTag:
       //  fnDclFlow(fstate, (FnDclNode *)*node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
        // nameUseFlow(fstate, (NameUseNode **)node); break;
    case ArrLitTag:
        // arrLitFlow(fstate, (ArrLitNode *)*node); break;
    case BreakTag:
    case ContinueTag:
        // breakFlow(fstate, *node); break;
    case ReturnTag:
        // returnFlow(fstate, (ReturnNode *)*node); break;
    case VTupleTag:
        // vtupleFlow(fstate, (VTupleNode *)*node); break;
    case FnCallTag:
        // fnCallFlow(fstate, (FnCallNode *)*node); break;
    case ArrIndexTag:
        // fnCallFlow(fstate, (FnCallNode *)*node); break;
    case StrFieldTag:
        // fnCallFlow(fstate, (FnCallNode *)*node); break;
    case SizeofTag:
        // sizeofFlow(fstate, (SizeofNode *)*node); break;
    case CastTag:
        // castFlow(fstate, (CastNode *)*node); break;
    case DerefTag:
        // derefFlow(fstate, (DerefNode *)*node); break;
    case NotLogicTag:
        // logicNotFlow(fstate, (LogicNode *)*node); break;
    case OrLogicTag: case AndLogicTag:
        // logicFlow(fstate, (LogicNode *)*node); break;
    case FnSigTag:
        // fnSigFlow(fstate, (FnSigNode *)*node); break;
    case RefTag:
        // refFlow(fstate, (RefNode *)*node); break;
    case PtrTag:
        // ptrFlow(fstate, (PtrNode *)*node); break;
    case StructTag:
    case AllocTag:
        // structFlow(fstate, (StructNode *)*node); break;
    case ArrayTag:
        // arrayFlow(fstate, (ArrayNode *)*node); break;
    case TTupleTag:
        // ttupleFlow(fstate, (TTupleNode *)*node); break;

    case ULitTag:
    case FLitTag:
        // inodeFlow(fstate, &((ITypedNode*)*node)->vtype); break;

    case MbrNameUseTag:
    case StrLitTag:
    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
    case PermTag:
    case VoidTag:
        return;
	default:
		assert(0 && "**** ERROR **** Attempting to check an unknown node");
	}
}

// Perform data flow analysis on a node whose value we want to copy or move
// Does it have a valid value? Is it loadable (e.g., readable from a reference)?
// Is it copyable?  If only movable, can we deactivate its source?
void flowCopyValue(FlowState *fstate, INode **nodep) {
    // Handle specific nodes here - lvals (read check) + literals + fncall
    // fncall + literals? do not need copy check - it can return

    int copytrait = itypeCopyTrait(((ITypedNode *)*nodep)->vtype);
    if (copytrait != CopyBitwise)
        errorMsgNode(*nodep, WarnCopy, "No current support for move. Be sure this is safe!");
    // if CopyMethod - inject use of that method to create a safe clone that can be "moved"
    // if CopyMove - turn off access to the source (via static (local var) or dynamic mechanism)

    flowWalk(fstate, nodep); // << To do
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
