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
    node->flowresult = NULL;
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
void returnNameRes(NameResState *nstate, BreakRetNode *retnode) {
    inodeNameRes(nstate, &retnode->exp);
}

// A borrowed reference may not travel beyond the scope it was borrowed from.
// assignlvalrtype enforces that for a store into a global or through a
// reference, and the loan walk at a call that could store one (pwCallStores);
// this enforces it at the function's own return value.
//
// A borrow's lifetime is a scope number: 0 is global, 1 the caller band (what a
// borrowed parameter points at), and 2 or more a block of this function -- 2 its
// top block, where its parameters' own storage and its outermost locals live
// (iexpGetLvalInfo). Global and the caller band outlive the call; anything
// deeper is gone by the time the caller reads it.
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
    // An 'if' (a 'match' is one) returns whichever arm runs, and a block its last
    // value or a break's, so each value is checked where it is written. An arm
    // ending in a jump gives none; a 'return' there is checked as its own.
    if (exp->tag == IfTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((IfNode*)exp)->condblk, cnt, nodesp)) {
            ++nodesp; --cnt;
            if (!ifBlockJumps((BlockNode *)*nodesp))
                returnFlowEscape(*nodesp);
        }
        return;
    }
    if (exp->tag == BlockTag && !(exp->flags & FlagLoop)) {
        BlockNode *blk = (BlockNode *)exp;
        if (blk->stmts->used > 0) {
            INode *last = nodesLast(blk->stmts);
            returnFlowEscape(last->tag == BlockRetTag ? ((BreakRetNode *)last)->exp : last);
        }
        if (blk->breaks) {
            INode **nodesp;
            uint32_t cnt;
            for (nodesFor(blk->breaks, cnt, nodesp))
                returnFlowEscape(((BreakRetNode *)*nodesp)->exp);
        }
        return;
    }
    RefNode *reftype = (RefNode *)((IExpNode*)exp)->vtype;
    if (reftype == NULL)
        return;
    // Several values arriving as one expression -- a call returning a tuple --
    // have no element expressions to walk, so the lifetimes are read from the
    // elements of the type fnCallFinalizeArgs built for that call
    if (reftype->tag == TTupleTag) {
        INode **elemp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)reftype)->elems, cnt, elemp)) {
            RefNode *elemtype = (RefNode *)itypeGetTypeDcl(*elemp);
            if ((elemtype->tag == RefTag || elemtype->tag == ArrayRefTag || elemtype->tag == VirtRefTag)
                && elemtype->region == borrowRef && elemtype->scope > 1) {
                errorMsgNode(exp, ErrorEscape,
                    "Returned borrowed reference outlives the local value it points to");
                return;
            }
        }
        return;
    }
    if (reftype->tag != RefTag && reftype->tag != ArrayRefTag && reftype->tag != VirtRefTag)
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
void returnTypeCheck(TypeCheckState *tstate, BreakRetNode *retnode) {
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
    else {
        // What a function returning 'Never' hands to its 'return' gives no
        // value, so it is checked as a statement: an 'if' there needs no 'else'
        // to be well typed, only to pass the check below
        int never = itypeIsNever(fnsig->rettype);
        inodeTypeCheck(tstate, &retnode->exp, never ? noCareType : fnsig->rettype);
        // A call that does not return hands back nothing to coerce: control
        // leaves the function through it, whatever the function returns. This
        // is also how a block ends in one (blockTypeCheck), and generation ends
        // the path there (genlReturn).
        if (fnCallIsNever(retnode->exp))
            ;
        // A function returning 'Never' may leave only through such a call, or
        // an 'if' each of whose paths does
        else if (never) {
            if (!(retnode->exp->tag == IfTag && ifAllPathsJump((IfNode *)retnode->exp)))
                errorMsgNode((INode*)retnode, ErrorNeverReturns,
                    "This function returns Never, so it must not return: end it in a call that does not return, such as 'panic(...)'.");
        }
        else if (!iexpCheckedCoerce(fnsig->rettype, &retnode->exp)) {
            errorMsgNode((INode*)retnode, ErrorInvType, "Return expression type does not match return type on function");
            errorMsgNode((INode*)fnsig->rettype, ErrorInvType, "This is the declared function's return type");
        }
    }

    returnJoinFn(tstate, retnode);
}

// Point a type-checked return at the function block it leaves, and add it to
// that block's list of returns (generation needs this to help with inline
// functions)
void returnJoinFn(TypeCheckState *tstate, BreakRetNode *retnode) {
    BlockNode *fnblock = retnode->block = (BlockNode*)tstate->fn->value;
    if (tstate->fn->flags & FlagInline) {
        if (!fnblock->breaks)
            fnblock->breaks = newNodes(2);
        nodesAdd(&fnblock->breaks, (INode*)retnode);
    }
}
