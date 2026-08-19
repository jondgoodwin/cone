/** Handling for return nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new return statement retnode
BreakRetNode *newReturnNode() {
    BreakRetNode *node;
    newNode(node, BreakRetNode, ReturnTag);
    node->exp = NULL;
    node->block = NULL;
    node->dealias = NULL;
    return node;
}

// New return retnode with exp injected, and copy lex pos from it
BreakRetNode *newReturnNodeExp(INode *exp) {
    BreakRetNode *node = newReturnNode();
    node->exp = exp;
    inodeLexCopy((INode*)node, (INode*)exp);
    return node;
}

// Clone return
INode *cloneReturnNode(CloneState *cstate, BreakRetNode *node) {
    BreakRetNode *newnode;
    newnode = memAllocBlk(sizeof(BreakRetNode));
    memcpy(newnode, node, sizeof(BreakRetNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->block = (BlockNode *)cloneDclFix((INode*)node->block);
    return (INode *)newnode;
}

// Serialize a return statement
void returnPrint(BreakRetNode *node) {
    inodeFprint(node->tag == BlockRetTag? "blockret " : "return ");
    inodePrintNode(node->exp);
}

// Name resolution for return
void returnNameRes(AnalysisState *nstate, BreakRetNode *retnode) {
    inodeNameRes(nstate, &retnode->exp);
}

// A borrowed reference may not travel beyond the scope it was borrowed from.
// assignlvalrtype enforces that between two variables; this enforces it at the
// only other place a borrow can escape, the function's own return value.
//
// Borrow scopes count outward from the value borrowed from: 0 is a global, 1 is
// a parameter (parseFnSig stamps every parameter with scope 1), and 2 or more is
// a local of this function or of a block inside it. A global or a parameter
// outlives the call; anything deeper is gone by the time the caller reads it.
static void returnFlowEscape(INode *exp) {
    if (!isExpNode(exp))
        return;
    // A value tuple returns each of its elements, so each is checked in turn
    if (exp->tag == VTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)exp)->elems, cnt, elemp))
            returnFlowEscape(*elemp);
        return;
    }
    RefNode *reftype = (RefNode *)((IExpNode*)exp)->vtype;
    if (reftype == NULL || (reftype->tag != RefTag && reftype->tag != ArrayRefTag))
        return;
    if (reftype->region == borrowRef && reftype->scope > 1)
        errorMsgNode(exp, ErrorEscape,
            "Returned borrowed reference outlives the local value it points to");
}

// Perform data flow analysis on a return statement's value
void returnFlow(BreakRetNode *retnode) {
    if (retnode->exp)
        returnFlowEscape(retnode->exp);
}

// Type check for return statement
// Related analysis for return elsewhere:
// - Block ensures that return can only appear at end of block
// - NameDcl turns fn block's final expression into an implicit return
void returnTypeCheck(AnalysisState *tstate, BreakRetNode *retnode) {
    // If we are returning the value from an 'if', recursively strip out any of its path's redundant 'return's
    if (retnode->exp->tag == IfTag)
        ifRemoveReturns((IfNode*)(retnode->exp));

    // Ensure the vtype of the expression can be coerced to the function's declared return type
    // while processing the exp nodes
    FnSigNode *fnsig = (FnSigNode*)tstate->fn->vtype;
    if (fnsig->rettype->tag == TTupleTag && retnode->exp->tag == VTupleTag) {
        // Where return expression is an explicit value tuple,
        // we can safely perform implicit type coercions on individual tuple elements
        Nodes *retnodes = ((TupleNode*)retnode->exp)->elems;
        Nodes *rettypes = ((TupleNode*)fnsig->rettype)->elems;
        if (rettypes->used > retnodes->used) {
            errorMsgNode(retnode->exp, ErrorBadTerm, "Not enough return values");
            return;
        }
        uint32_t retcnt;
        INode **rettypesp;
        INode **retnodesp = &nodesGet(retnodes, 0);
        for (nodesFor(rettypes, retcnt, rettypesp)) {
            if (!iexpTypeCheckCoerce(tstate, *rettypesp, retnodesp++))
                errorMsgNode(*(retnodesp - 1), ErrorInvType, "Return value's type does not match fn return type");
        }
        // Establish the type of the tuple (from the expected return value types)
        ((TupleNode *)retnode->exp)->vtype = fnsig->rettype;
    }
    else if (!iexpTypeCheckCoerce(tstate, fnsig->rettype, &retnode->exp)) {
        errorMsgNode((INode*)retnode, ErrorInvType, "Return expression type does not match return type on function");
        errorMsgNode((INode*)fnsig->rettype, ErrorInvType, "This is the declared function's return type");
    }

    // Have return node point to function block that is "breaks" out of
    // Also, add return node to that block's list of returns (generation needs this to help with inline functions)
    BlockNode *fnblock = retnode->block = (BlockNode*)tstate->fn->value;
    if (tstate->fn->flags & FlagInline) {
        if (!fnblock->breaks)
            fnblock->breaks = newNodes(2);
        nodesAdd(&fnblock->breaks, (INode*)retnode);
    }
}
