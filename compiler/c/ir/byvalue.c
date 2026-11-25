/** The uses of an '@unsized' enum's value in a function's body
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <assert.h>

// A deref of a reference to an '@unsized' enum is typed without knowing whether
// its value or only its place is wanted, and values are consumed all over:
// assignments, swaps, tuple elements, block and 'if' values, discards. So the
// whole of a checked body is walked, saying of each expression what its parent
// does with it. A parent asks for a place only where this walk says so; every
// other expression is held to be a value used, so a form nothing here names is
// refused rather than let through.
//
// 'use' completes "its value can't be ...", and is NULL for a place. Each
// function answers whether it refused something in what it walked: a node whose
// part was refused says nothing more, and the first refusal in an expression
// ends the walk of its other parts.

static int byValueNode(INode *node, const char *use);

static int byValueList(Nodes *nodes, const char *use) {
    if (nodes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(nodes, cnt, nodesp))
        if (byValueNode(*nodesp, use))
            return 1;
    return 0;
}

// The targets of an assignment or a swap. A '_' takes a value and stores nothing
static int byValueTarget(INode *node, const char *use) {
    if (node->tag == VTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)node)->elems, cnt, nodesp))
            if (byValueTarget(*nodesp, use))
                return 1;
        return 0;
    }
    if (assignIsAnon(node))
        return 0;
    return byValueNode(node, use);
}

// Walk what 'node' is made of
static int byValueParts(INode *node) {
    switch (node->tag) {
    case BlockTag: {
        // Each statement is its own: one refused does not silence the next. A
        // block with a value may end in the expression that gives it
        int found = 0;
        int valued = itypeHeldUnsizedEnum(((IExpNode*)node)->vtype) != NULL;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((BlockNode*)node)->stmts, cnt, nodesp))
            found |= byValueNode(*nodesp, valued && cnt == 1 ? "given as a block's value" : "discarded");
        return found;
    }
    case IfTag: {
        // Pairs of a condition and the block it chooses
        int found = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((IfNode*)node)->condblk, cnt, nodesp))
            found |= byValueNode(*nodesp, "the value of an 'if' or a 'match'");
        return found;
    }
    case BlockRetTag:
        return byValueNode(((BreakRetNode*)node)->exp, "given as a block's value");
    case ReturnTag:
        return byValueNode(((BreakRetNode*)node)->exp, "returned");
    case BreakTag:
    case ContinueTag:
        return byValueNode(((BreakRetNode*)node)->exp, "given to 'break'");
    case VarDclTag:
        return byValueNode(((VarDclNode*)node)->value, "held in a variable");
    case ConstDclTag:
        return byValueNode(((ConstDclNode*)node)->value, "held in a constant");

    case AssignTag: {
        AssignNode *assign = (AssignNode*)node;
        int many = assign->lval->tag == VTupleTag;
        return byValueTarget(assign->lval, many ? "swapped or stored by a multiple assignment" : "assigned or stored")
            || byValueNode(assign->rval, assign->lval->tag != VTupleTag && assignIsAnon(assign->lval)
                ? "discarded" : "copied or assigned");
    }
    case SwapTag:
        return byValueTarget(((SwapNode*)node)->lval, "swapped")
            || byValueTarget(((SwapNode*)node)->rval, "swapped");

    case VTupleTag:
        return byValueList(((TupleNode*)node)->elems, "put in a tuple");
    case ArrayLitTag:
        return byValueList(((ArrayNode*)node)->dimens, "used as an array's size")
            || byValueList(((ArrayNode*)node)->elems, "put in an array");

    // The base of a field access is asked for as a place
    case FldAccessTag:
        return byValueNode(((FnCallNode*)node)->objfn, NULL);
    case ArrIndexTag:
        return byValueNode(((FnCallNode*)node)->objfn, "indexed")
            || byValueList(((FnCallNode*)node)->args, "used as an index");
    // A method's receiver is an argument by now, borrowed where the method
    // takes a reference
    case FnCallTag:
        return byValueNode(((FnCallNode*)node)->objfn, "called")
            || byValueList(((FnCallNode*)node)->args, "passed as an argument");
    case TypeLitTag:
        return byValueList(((FnCallNode*)node)->args, "put in a field");

    case BorrowTag:
    case ArrayBorrowTag:
        return byValueNode(((RefNode*)node)->vtexp, NULL);
    case AllocateTag:
        return byValueNode(((RefNode*)node)->vtexp, "allocated");
    case DerefTag:
        return byValueNode(((StarNode*)node)->vtexp, "dereferenced");
    case CastTag:
        return byValueNode(((CastNode*)node)->exp, "converted");
    case IsTag:
        return byValueNode(((CastNode*)node)->exp, "tested by value");
    case NotLogicTag:
        return byValueNode(((LogicNode*)node)->lexp, "tested by value");
    case OrLogicTag:
    case AndLogicTag:
        return byValueNode(((LogicNode*)node)->lexp, "tested by value")
            || byValueNode(((LogicNode*)node)->rexp, "tested by value");
    case NamedValTag:
        return byValueNode(((NamedValNode*)node)->val, "passed by name");
    case AwaitTag:
        return byValueNode(((AwaitNode*)node)->exp, "awaited");
    case YieldTag:
        return byValueNode(((YieldNode*)node)->exp, "yielded");
    case DestructTag:
        return byValueNode(((DestructNode*)node)->exp, "taken apart");
    case OfEntryTag: case FillEntryTag: case PairEntryTag: case EachEntryTag: case YieldEntryTag:
        return byValueNode(((EntryNode*)node)->first, "put in a collection")
            || byValueNode(((EntryNode*)node)->val, "put in a collection");

    // What data flow wraps a value in, which runs after this walk
    case RefCountTag:
        return byValueNode(((RefCountNode*)node)->exp, "used");
    case HollowTag:
        return byValueNode(((HollowNode*)node)->exp, "used");
    case DropFlagTag:
        return byValueNode(((DropFlagNode*)node)->release, "used");
    case TempTag:
        return byValueNode(((TempNode*)node)->exp, "used");

    // No parts that are values
    case NilLitTag: case NullLitTag: case ULitTag: case FLitTag: case StringLitTag:
    case SizeofTag: case AwaitReplyTag: case SelfActorTag: case ClosureTag:
    case AbsenceTag: case UnknownTag:
        return 0;

    default:
        // An expression this walk cannot see into would go unchecked
        if (isExpNode(node))
            errorUnreachable(node, "an expression the walk for '@unsized' values has no case for");
        return 0;
    }
}

static int byValueNode(INode *node, const char *use) {
    if (node == NULL)
        return 0;
    // A name use has no parts, and may name a type, which is no value
    if (!isNameUseNode(node) && byValueParts(node))
        return 1;
    if (use == NULL || !isExpNode(node))
        return 0;
    INode *enumdcl = itypeHeldUnsizedEnum(((IExpNode*)node)->vtype);
    if (enumdcl == NULL)
        return 0;
    itypeUnsizedEnumMsg(node, enumdcl, use);
    return 1;
}

// Refuse every use of an '@unsized' enum's value in this function. See byvalue.h.
void byValueCheck(FnDclNode *fn) {
    if (fn->value)
        byValueNode(fn->value, NULL);
}
