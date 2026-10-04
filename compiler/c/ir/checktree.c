/** IR well-formedness check, run after semantic analysis
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../shared/error.h"

// Verify that the IR carries no hole a later phase would read without asking.
//
// The invariant is narrow and mechanical: every expression node reachable from
// the program has a value type, and every block has a statement list. It is the
// invariant a phase breaks when it reports a bad program and returns before
// filling one of those in, and reading the hole afterwards is an access
// violation rather than a diagnostic. That defect was found nine times over;
// this walk is what notices the tenth without waiting for someone to write the
// scenario that dies on it.
//
// So it runs whether or not errors were reported. Error paths are exactly where
// the holes come from, which means a 'reject' scenario is the case that
// exercises this and a clean compile is the one that cannot.
//
// It deliberately does not descend into type declarations. A type may refer to
// itself through a reference, so the type graph has cycles, and following them
// would need visited-set bookkeeping to buy nothing: a type node's own vtype is
// not what any of these defects left empty.

static void checkNode(INode *node);

// Walk a node list, tolerating the null list itself so that the diagnostic for
// it is reported once, by its owner, rather than crashing the check
static void checkNodes(Nodes *nodes) {
    if (nodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp))
        checkNode(*nodesp);
}

static void checkNodeList(NodeList *list) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(list, cnt, nodesp))
        checkNode(*nodesp);
}

// A generic's instances: 'memonodes' holds pairs, the call that asked for each
// and the instance it made. An instance is not generic itself, so this does not
// return to the template it came from
static void checkInstances(GenericInfo *info, INode *template) {
    if (info->memonodes == NULL)
        return;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(info->memonodes, cnt, nodesp)) {
        nodesp++; cnt--;
        if (*nodesp != template)
            checkNode(*nodesp);
    }
}

static void checkNode(INode *node) {
    if (node == NULL)
        return;

    // The invariant itself. A node in the expression group carries a value type
    // by definition of the group, so a null here is a phase that returned early.
    if (isExpNode(node) && ((IExpNode*)node)->vtype == NULL) {
        // The tag is in the message because this diagnostic accuses the compiler
        // rather than the program, so what it has to identify is the node kind
        errorMsgNode(node, ErrorBadTree,
            "Compiler defect: an expression node (tag %d) was left with no value type.",
            (int)node->tag);
        return;
    }

    switch (node->tag) {
    case ProgramTag:
        checkNodes(((ProgramNode*)node)->modules); break;

    case ModuleTag:
        checkNodes(((ModuleNode*)node)->nodes); break;

    // A generic's template is never analyzed, only its instances, so its body
    // holds nodes no phase was meant to type (a value tuple, built with no value
    // type, among them). Its instances, which were analyzed, are walked instead
    case FnDclTag:
        if (((FnDclNode*)node)->genericinfo != NULL)
            checkInstances(((FnDclNode*)node)->genericinfo, node);
        else
            checkNode(((FnDclNode*)node)->value);
        break;

    case FnOverloadDclTag:
        checkNodes(((FnOverloadDclNode*)node)->overloads); break;

    case VarDclTag:
        checkNode(((VarDclNode*)node)->value); break;
    case ConstDclTag:
        checkNode(((ConstDclNode*)node)->value); break;
    case FieldDclTag:
        checkNode(((FieldDclNode*)node)->value); break;

    // A struct is a type, but it owns declarations whose bodies are code, so its
    // members are walked while the type graph below them is not. A generic
    // type's template, like a generic function's, gives way to its instances
    case StructTag:
        if (((StructNode*)node)->genericinfo != NULL) {
            checkInstances(((StructNode*)node)->genericinfo, node);
            break;
        }
        checkNodeList(&((StructNode*)node)->nodelist);
        checkNodeList(&((StructNode*)node)->fields);
        break;

    case BlockTag: {
        BlockNode *blk = (BlockNode*)node;
        if (blk->stmts == NULL) {
            errorMsgNode(node, ErrorBadTree,
                "Compiler defect: this block was left with no statement list.");
            break;
        }
        checkNodes(blk->stmts);
        break;
    }

    case IfTag:
        checkNodes(((IfNode*)node)->condblk); break;

    case BreakTag:
    case ContinueTag:
    case BlockRetTag:
    case ReturnTag:
        checkNode(((BreakRetNode*)node)->exp); break;

    case AssignTag:
        checkNode(((AssignNode*)node)->lval);
        checkNode(((AssignNode*)node)->rval);
        break;
    case SwapTag:
        checkNode(((SwapNode*)node)->lval);
        checkNode(((SwapNode*)node)->rval);
        break;

    case VTupleTag:
        checkNodes(((TupleNode*)node)->elems); break;

    case ArrayLitTag:
        checkNodes(((ArrayNode*)node)->dimens);
        checkNodes(((ArrayNode*)node)->elems);
        break;

    case FnCallTag:
    case ArrIndexTag:
    case FldAccessTag:
    case TypeLitTag: {
        FnCallNode *call = (FnCallNode*)node;
        checkNode(call->objfn);
        checkNodes(call->args);
        break;
    }

    case CastTag:
    case IsTag:
        checkNode(((CastNode*)node)->exp); break;

    case DerefTag:
        checkNode(((StarNode*)node)->vtexp); break;

    case BorrowTag:
    case ArrayBorrowTag:
    case AllocateTag:
        checkNode(((RefNode*)node)->vtexp); break;

    case NotLogicTag:
        checkNode(((LogicNode*)node)->lexp); break;
    case OrLogicTag:
    case AndLogicTag:
        checkNode(((LogicNode*)node)->lexp);
        checkNode(((LogicNode*)node)->rexp);
        break;

    case AwaitTag:
        checkNode(((AwaitNode*)node)->exp); break;
    case AwaitReplyTag:
    case SelfActorTag:
        break;

    case NamedValTag:
        checkNode(((NamedValNode*)node)->val); break;

    case OfEntryTag: case FillEntryTag: case PairEntryTag:
        if (((EntryNode*)node)->first)
            checkNode(((EntryNode*)node)->first);
        checkNode(((EntryNode*)node)->val);
        break;

    case RefCountTag:
        checkNode(((RefCountNode*)node)->exp); break;

    case HollowTag:
        if (((HollowNode*)node)->exp)
            checkNode(((HollowNode*)node)->exp);
        break;

    case DropFlagTag:
        checkNode(((DropFlagNode*)node)->release); break;

    case TempTag:
        checkNode(((TempNode*)node)->exp); break;

    // Everything else is a leaf here: a literal, a name use whose declaration is
    // reached through the owning module anyway, or a type node
    default:
        break;
    }
}

// Verify IR well-formedness across the whole program
void inodeCheckTree(INode *pgm) {
    checkNode(pgm);
}
