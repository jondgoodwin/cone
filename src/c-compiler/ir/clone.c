/** The cloning pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>

// Deep copy a node
INode *cloneNode(CloneState *cstate, INode *nodep) {
    if (nodep == NULL)
        return NULL;

    INode *node;
    switch (nodep->tag) {
    case AssignTag:
        node = cloneAssignNode(cstate, (AssignNode *)nodep); break;
    case SwapTag:
        node = cloneSwapNode(cstate, (SwapNode *)nodep); break;
    case BlockTag:
        node = cloneBlockNode(cstate, (BlockNode *)nodep); break;
    case CastTag:
        node = cloneCastNode(cstate, (CastNode *)nodep); break;
    case DerefTag:
        node = cloneStarNode(cstate, (StarNode *)nodep); break;
    case FnCallTag:
    case ArrIndexTag:
    case FldAccessTag:
    case TypeLitTag:
        node = cloneFnCallNode(cstate, (FnCallNode *)nodep); break;
    case IfTag:
        node = cloneIfNode(cstate, (IfNode *)nodep); break;
    case NotLogicTag:
    case OrLogicTag:
    case AndLogicTag:
        node = cloneLogicNode(cstate, (LogicNode *)nodep); break;
    case NamedValTag:
        node = cloneNamedValNode(cstate, (NamedValNode *)nodep); break;
    case NameUseTag:
    case MacroNameTag:
    case GenericNameTag:
    case VarNameUseTag:
    case MbrNameUseTag:
    case TypeNameUseTag: 
    {
        node = cloneNameUseNode(cstate, (NameUseNode *)nodep);
        // For traits as mixins, repoint 'Self' to the struct node
        if (cstate->selftype && ((NameUseNode*)node)->namesym == selfTypeName)
            ((NameUseNode*)node)->dclnode = cstate->selftype;
        break;
    }
    case SizeofTag:
        node = cloneSizeofNode(cstate, (SizeofNode *)nodep); break;
    case VTupleTag:
        node = cloneTupleNode(cstate, (TupleNode *)nodep); break;
    case NilLitTag:
        node = cloneNilLitNode(cstate, (NilLitNode *)nodep); break;
    case ULitTag:
        node = cloneULitNode(cstate, (ULitNode *)nodep); break;
    case FLitTag:
        node = cloneFLitNode(cstate, (FLitNode *)nodep); break;
    case StringLitTag:
        node = cloneSLitNode((SLitNode *)nodep); break;

    case BreakTag:
        node = cloneBreakNode(cstate, (BreakRetNode *)nodep); break;
    case ContinueTag:
        node = cloneContinueNode(cstate, (BreakRetNode *)nodep); break;
    case FieldDclTag:
        node = cloneFieldDclNode(cstate, (FieldDclNode *)nodep); break;
    case FnDclTag:
        node = cloneFnDclNode(cstate, (FnDclNode *)nodep); break;
    case ReturnTag:
        node = cloneReturnNode(cstate, (BreakRetNode *)nodep); break;
    case VarDclTag:
        node = cloneVarDclNode(cstate, (VarDclNode *)nodep); break;

    case StructTag:
        node = cloneStructNode(cstate, (StructNode *)nodep); break;
    case TTupleTag:
        node = cloneTupleNode(cstate, (TupleNode *)nodep); break;
    case ArrayLitTag:
    case ArrayTag:
        node = cloneArrayNode(cstate, (ArrayNode *)nodep); break;
    case FnSigTag:
        node = cloneFnSigNode(cstate, (FnSigNode *)nodep); break;
    case PtrTag:
        node = cloneStarNode(cstate, (StarNode *)nodep); break;
    case AllocateTag:
    case ArrayAllocTag:
    case BorrowTag:
    case ArrayBorrowTag:
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        node = cloneRefNode(cstate, (RefNode *)nodep); break;
    case LifetimeTag:
        node = cloneLifetimeDclNode(cstate, (LifetimeNode *)nodep); break;
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        node = cloneNbrNode(cstate, (NbrNode *)nodep);  break; // Don't clone for now

    case GenVarUseTag:
        node = cloneNode(cstate, ((GenVarDclNode*)nodep)->namesym->node);
        return node;

    case AbsenceTag:
    case UnknownTag:
    case BorrowRegTag:
        node = nodep; break; // Don't clone unclonable node

    case VoidTag:
        node = cloneVoidNode(cstate, (VoidTypeNode *)nodep); break;

    case EnumTag:
        node = nodep; break;

    default:
        assert(0 && "Do not know how to clone a node of this type");
    }
    node->instnode = cstate->instnode;
    return node;
}

// Set the state needed for deep cloning some node
void clonePushState(CloneState *cstate, INode *instnode, INode *selftype, uint32_t scope, Nodes *parms, Nodes *args) {
    cstate->instnode = instnode;
    cstate->selftype = selftype;
    cstate->scope = scope;
    nametblHookPush();

    if (parms) {
        // Hook in named arguments for parms
        INode **parmp = &nodesGet(parms, 0);
        INode **argsp;
        uint32_t cnt;
        for (nodesFor(args, cnt, argsp)) {
            nametblHookNode(((GenVarDclNode *)*parmp++)->namesym, (INode*)*argsp);
        }
    }
}

// Release the acquired state
void clonePopState() {
    nametblHookPop();
}

// -------
// cloneDclFix is needed by already-name-resolved nameuse nodes during cloning.
// It re-points the cloned nameuse node to the comparably cloned dcl node.
// This requires a LIFO stack that is built when cloning each named dcl node,
// capturing a mapping from some original dcl node to its cloned copy.
// Because blocks introduce local names, push and pop preserve and restores the stack's high-water point,
// minimizing linear search throughput.

// By being LIFO, this should be impervious to cloning within cloning.
// -------

// The clone dclnode map stack
CloneDclMap *cloneDclMap = NULL;

// High-water mark on clone dcl map stack, position of next entry
uint32_t cloneDclPos = 0;

// Current max number of entries in dclnode map stack, triggering growth if exceeded
uint32_t cloneDclSize = 0;

// Preserve high-water position in the dcl stack
uint32_t cloneDclPush() {
    return cloneDclPos;
}

// Restore high-water position in the dcl stack
void cloneDclPop(uint32_t pos) {
    cloneDclPos = pos;
}

// Remember a mapping of a declaration node between the original and a copy
void cloneDclSetMap(INode *orig, INode *clone) {
    if (cloneDclSize <= cloneDclPos) {
        if (cloneDclSize == 0) {
            cloneDclSize = 1024;
            cloneDclMap = memAllocBlk(cloneDclSize * sizeof(CloneDclMap));
        }
        else {
            cloneDclSize <<= 1;
            CloneDclMap *oldmap = cloneDclMap;
            cloneDclMap = memAllocBlk(cloneDclSize * sizeof(CloneDclMap));
            memcpy(cloneDclMap, oldmap, cloneDclPos * sizeof(CloneDclMap));
        }
    }
    CloneDclMap *map = &cloneDclMap[cloneDclPos++];
    map->original = orig;
    map->clone = clone;
}

// Return the new pointer to the cloned dcl node, given a pointer to the original dclnode
INode *cloneDclFix(INode *orig) {
    // Search for dcl node backwards in LIFO order
    for (uint32_t pos = cloneDclPos; pos > 0; ) {
        CloneDclMap *map = &cloneDclMap[--pos];
        // Found: return cloned copy of the dcl node
        if (map->original == orig)
            return map->clone;
    }
    return orig;
}
