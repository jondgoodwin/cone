/** The Data Flow analysis pass
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>

// Dispatch a node walk for the data flow pass
// - fstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void flowWalk(FlowState *fstate, INode **node) {
	switch ((*node)->tag) {
    case BlockTag:
        // blockFlow(fstate, (BlockNode *)*node); break;
    case VarDclTag:
        // varDclFlow(fstate, (VarDclNode *)*node); break;
    case FnDclTag:
       //  fnDclFlow(fstate, (FnDclNode *)*node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
        // nameUseFlow(fstate, (NameUseNode **)node); break;
    case ArrLitTag:
        // arrLitFlow(fstate, (ArrLitNode *)*node); break;
    case IfTag:
        // ifFlow(fstate, (IfNode *)*node); break;
    case WhileTag:
        // whileFlow(fstate, (WhileNode *)*node); break;
    case BreakTag:
    case ContinueTag:
        // breakFlow(fstate, *node); break;
    case ReturnTag:
        // returnFlow(fstate, (ReturnNode *)*node); break;
    case AssignTag:
        // assignFlow(fstate, (AssignNode *)*node); break;
    case VTupleTag:
        // vtupleFlow(fstate, (VTupleNode *)*node); break;
    case FnCallTag:
        // fnCallFlow(fstate, (FnCallNode *)*node); break;
    case SizeofTag:
        // sizeofFlow(fstate, (SizeofNode *)*node); break;
    case CastTag:
        // castFlow(fstate, (CastNode *)*node); break;
    case DerefTag:
        // derefFlow(fstate, (DerefNode *)*node); break;
    case AddrTag:
        // addrFlow(fstate, (AddrNode *)*node); break;
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
