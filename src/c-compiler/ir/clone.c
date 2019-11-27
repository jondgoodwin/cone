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
    case VarNameUseTag:
    case TypeNameUseTag:
        node = cloneNameUseNode(cstate, (NameUseNode *)nodep); break;
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

    default:
        assert(0 && "Do not know how to clone a node of this type");
    }
    node->instnode = cstate->instnode;
    return node;
}
