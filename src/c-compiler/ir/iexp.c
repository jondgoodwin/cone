/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

// Macro that changes iexp pointer to type dcl pointer
#define iexpToTypeDcl(node) {\
    if (isExpNode(node) || (node)->tag == VarDclTag || (node)->tag == FnDclTag || (node)->tag == FieldDclTag) \
        node = ((IExpNode *)node)->vtype; \
    if (node->tag == TypeNameUseTag) \
        node = (INode*)((NameUseNode *)node)->dclnode; \
}

// Return value type
INode *iexpGetTypeDcl(INode *node) {
    iexpToTypeDcl(node);
    return node;
}

// Return type (or de-referenced type if ptr/ref)
INode *iexpGetDerefTypeDcl(INode *node) {
    iexpToTypeDcl(node);
    if (node->tag == RefTag) {
        node = ((RefNode*)node)->pvtype;
        if (node->tag == TypeNameUseTag)
            node = (INode*)((NameUseNode *)node)->dclnode;
    }
    else if (node->tag == PtrTag) {
        node = ((PtrNode*)node)->pvtype;
        if (node->tag == TypeNameUseTag)
            node = (INode*)((NameUseNode *)node)->dclnode;
    }
    return node;
}

// Handle type-checking for a block. 
// Mostly this is a pass-through to type-check the block's statements.
void blockTypeCheck2(TypeCheckState *pstate, BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        if (cnt > 1)
            // All stmts except last throw out their value
            inodeTypeCheck(pstate, nodesp);
        else
            // Value of block is value of last statement
            // Ensure its type matches expected type for block
            if (!iexpChkType(pstate, &blk->vtype, nodesp))
                errorMsgNode(*nodesp, ErrorInvType, "expression type does not match expected type");
    }
}

// Type check the if statement node
// - Every conditional expression must be a bool
// - Type of every branch's value must match expected type and each other
void ifTypeCheck2(TypeCheckState *pstate, IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        // Validate that conditional node is correct
        inodeTypeCheck(pstate, nodesp);
        if (*nodesp != voidType) {
            if (0 == iexpCoerces((INode*)boolType, nodesp))
                errorMsgNode(*nodesp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
        }
        else if (cnt > 2) {
            errorMsgNode(*(nodesp + 1), ErrorInvType, "match on everything should be last.");
        }

        // Validate that all branches have matching types
        ++nodesp; --cnt;
        if (!iexpChkType(pstate, &ifnode->vtype, nodesp))
            errorMsgNode(*nodesp, ErrorInvType, "expression type does not match expected type");
    }
}

// Bidirectional type checking between 'from' exp and 'to' type
// Evaluate whether the 'from' node will type check to 'to' type
// - If 'to' type is unspecified, infer it from 'from' type
// - If 'from' node is 'if', 'block', 'loop', set it to 'to' type
// - Otherwise, check that types match.
//   If match requires a coercion, a 'cast' node will be inject ahead of the 'from' node
// return 1 for success, 0 for fail
int iexpChkType(TypeCheckState *pstate, INode **totypep, INode **from) {
    // Both nodes need to be typed expression nodes
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 0;
    }

    // If 'to' type is unspecified, do type check on 'from' and then copy into 'to'
    IExpNode *fromnode = (IExpNode *)*from;
    if (*totypep == NULL) {
        inodeTypeCheck(pstate, from);
        *totypep = fromnode->vtype;
        return 1;
    }

    // If 'from' node is 'if', 'block', 'loop', set it to 'to' node's type
    // These 'from' nodes are just passing through a value, not calculating it
    // We want to push expected type as low down in nodes before forcing any coercion
    // This ensures multi-branch specialized values all upcast to same expected supertype
    if (fromnode->tag == IfTag) {
        fromnode->vtype = *totypep;
        ifTypeCheck2(pstate, (IfNode*)*from);
        return 1;
    }
    if (fromnode->tag == BlockTag) {
        fromnode->vtype = *totypep;
        blockTypeCheck2(pstate, (BlockNode*)*from);
        return 1;
    }
    if (fromnode->tag == LoopTag) {
        fromnode->vtype = *totypep;
        // ifTypeCheck2(pstate, from);
        return 1;
    }

    // Both 'from' and 'to' specify a type. Do they match?
    // (this may involve injecting a 'cast' node in front of 'from'
    inodeTypeCheck(pstate, from);
    if (*totypep == voidType)
        return 1;

    // Re-type a null literal node to match the expected ref/ptr type
    if (fromnode->tag == NullTag) {
        if (!refIsNullable(*totypep) && (*totypep)->tag != PtrTag)
            return 0;
        fromnode->vtype = *totypep;
        return 1;
    }

    INode *totypedcl = iexpGetTypeDcl(*totypep);
    INode *fromtypedcl = iexpGetTypeDcl(*from);

    // Are types equivalent, or is 'to' a subtype of fromtypedcl?
    int match = itypeMatches(totypedcl, fromtypedcl);
    if (match <= 1)
        return match; // return fail or non-changing matches. Fall through to perform any coercion

    // Add coercion node
    if (match == 2) {
        *from = (INode*)newCastNode(*from, totypedcl);
        return 1;
    }

    // If not an integer literal, don't convert
    if ((*from)->tag != ULitTag)
        return 0;
    // If it is an integer literal, convert it to correct type/value in place
    ULitNode *lit = (ULitNode*)(*from);
    lit->vtype = totypedcl;
    if (totypedcl->tag == FloatNbrTag) {
        lit->tag = FLitTag;
        ((FLitNode*)lit)->floatlit = (double)lit->uintlit;
    }
    return 1;
}

// can from's value be coerced to to's value type?
// This might inject a 'cast' node in front of the 'from' node with non-matching numbers
int iexpCoerces(INode *to, INode **from) {

    // Re-type a null literal node to match the expected ref/ptr type
    if ((*from)->tag == NullTag) {
        if (!refIsNullable(to) && to->tag != PtrTag)
            return 0;
        ((NullNode*)(*from))->vtype = to;
        return 1;
    }

    // Convert to pointer from iexp to type dcl
    iexpToTypeDcl(to);

    // When coercing a block, do so on its last expression
    while ((*from)->tag == BlockTag) {
        ((IExpNode*)*from)->vtype = to;
        from = &nodesLast(((BlockNode*)*from)->stmts);
    }

    // Coercing an 'if' requires we do so on all its paths
    if ((*from)->tag == IfTag) {
        int16_t cnt;
        INode **nodesp;
        IfNode *ifnode = (IfNode*)*from;
        ifnode->vtype = to;
        if (nodesGet(ifnode->condblk, ifnode->condblk->used - 2) != voidType)
            errorMsgNode((INode*)ifnode, ErrorNoElse, "Missing else branch which needs to provide a value");
        for (nodesFor(ifnode->condblk, cnt, nodesp)) {
            cnt--; nodesp++;
            INode **lastnode = &nodesLast(((BlockNode*)*nodesp)->stmts);
            if (!iexpCoerces(to, lastnode)) {
                errorMsgNode(*lastnode, ErrorInvType, "expression type does not match expected type");
            }
        }
        return 1;
    }

    INode *fromtype = *from;
    iexpToTypeDcl(fromtype);

    // Are types equivalent, or is 'to' a subtype of fromtypedcl?
    int match = itypeMatches(to, fromtype);
    if (match <= 1)
        return match; // return fail or non-changing matches. Fall through to perform any coercion

    // Add coercion node
    if (match == 2) {
        *from = (INode*)newCastNode(*from, to);
        return 1;
    }

    // If not an integer literal, don't convert
    if ((*from)->tag != ULitTag)
        return 0;
    // If it is an integer literal, convert it to correct type/value in place
    ULitNode *lit = (ULitNode*)(*from);
    lit->vtype = to;
    if (to->tag == FloatNbrTag) {
        lit->tag = FLitTag;
        ((FLitNode*)lit)->floatlit = (double)lit->uintlit;
    }
    return 1;
}

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from) {
    iexpToTypeDcl(to);
    INode *fromtype = *from;
    iexpToTypeDcl(fromtype);
    return itypeMatches(to, fromtype) == 1;
}

// Retrieve the permission flags for the node
uint16_t iexpGetPermFlags(INode *node) {
    switch (node->tag) {
    case VarNameUseTag:
        return iexpGetPermFlags((INode*)((NameUseNode*)node)->dclnode);
    case VarDclTag:
        return permGetFlags(((VarDclNode*)node)->perm);
    case FnDclTag:
        return permGetFlags((INode*)opaqPerm);
    case DerefTag:
    {
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)node)->exp);
        if (vtype->tag == RefTag)
            return permGetFlags(vtype->perm);
        else if (vtype->tag == PtrTag)
            return 0xFFFF;  // <-- In a trust block?
        assert(0 && "Should be ref or ptr");
    }
    case ArrIndexTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        return iexpGetPermFlags(fncall->objfn);
    }
    case StrFieldTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        // Field access. Permission is the conjunction of structure and field permissions.
        return iexpGetPermFlags(fncall->objfn) & iexpGetPermFlags((INode*)fncall->methfld);
    }
    default:
        return 0;
    }
}
