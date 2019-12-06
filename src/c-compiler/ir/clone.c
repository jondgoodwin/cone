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
    case AllocateTag:
        node = cloneAllocateNode(cstate, (AllocateNode *)nodep); break;
    case AssignTag:
        node = cloneAssignNode(cstate, (AssignNode *)nodep); break;
    case BlockTag:
        node = cloneBlockNode(cstate, (BlockNode *)nodep); break;
    case BorrowTag:
        node = cloneBorrowNode(cstate, (BorrowNode *)nodep); break;
    case CastTag:
        node = cloneCastNode(cstate, (CastNode *)nodep); break;
    case DerefTag:
        node = cloneDerefNode(cstate, (DerefNode *)nodep); break;
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
    case LoopTag:
        node = cloneLoopNode(cstate, (LoopNode *)nodep); break;
    case NamedValTag:
        node = cloneNamedValNode(cstate, (NamedValNode *)nodep); break;
    case NameUseTag:
    case MacroNameTag:
    case VarNameUseTag:
    case MbrNameUseTag:
    case TypeNameUseTag: {
        node = cloneNameUseNode(cstate, (NameUseNode *)nodep);
        // For traits as mixins, repoint 'Self' to the struct node
        if (cstate->selftype && ((NameUseNode*)node)->namesym == selfTypeName)
            ((NameUseNode*)node)->dclnode = cstate->selftype;
        break;
    }
    case SizeofTag:
        node = cloneSizeofNode(cstate, (SizeofNode *)nodep); break;
    case VTupleTag:
        node = cloneVTupleNode(cstate, (VTupleNode *)nodep); break;
    case ULitTag:
        node = cloneULitNode((ULitNode *)nodep); break;
    case FLitTag:
        node = cloneFLitNode((FLitNode *)nodep); break;
    case StringLitTag:
        node = cloneSLitNode((SLitNode *)nodep); break;

    case BreakTag:
        node = cloneBreakNode(cstate, (BreakNode *)nodep); break;
    case ContinueTag:
        node = cloneContinueNode(cstate, (ContinueNode *)nodep); break;
    case FieldDclTag:
        node = cloneFieldDclNode(cstate, (FieldDclNode *)nodep); break;
    case FnDclTag:
        node = cloneFnDclNode(cstate, (FnDclNode *)nodep); break;
    case ReturnTag:
        node = cloneReturnNode(cstate, (ReturnNode *)nodep); break;
    case VarDclTag:
        node = cloneVarDclNode(cstate, (VarDclNode *)nodep); break;

    case StructTag:
        node = cloneStructNode(cstate, (StructNode *)nodep); break;
    case ArrayTag:
        node = cloneArrayNode(cstate, (ArrayNode *)nodep); break;
    case FnSigTag:
        node = cloneFnSigNode(cstate, (FnSigNode *)nodep); break;
    case PtrTag:
        node = clonePtrNode(cstate, (PtrNode *)nodep); break;
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        node = cloneRefNode(cstate, (RefNode *)nodep); break;
    case LifetimeTag:
        node = cloneLifetimeDclNode(cstate, (LifetimeNode *)nodep); break;

    case GenVarUseTag:
        node = cloneNode(cstate, ((GenVarDclNode*)nodep)->namesym->node);
        return node;

    case AbsenceTag:
    case UnknownTag:
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

CloneDclMap *cloneDclMap = NULL;
uint32_t cloneDclPos = 0;
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

// Return the new pointer to the dcl node, given the original pointer
INode *cloneDclFix(INode *orig) {
    for (uint32_t pos = 0; pos < cloneDclPos; ++pos) {
        CloneDclMap *map = &cloneDclMap[pos];
        if (map->original == orig)
            return map->clone;
    }
    return orig;
}