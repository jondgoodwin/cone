/** Expression nodes that return a typed value
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

// Return expression node's "declared" type
INode *iexpGetTypeDcl(INode *node) {
    if (isExpNode(node) || (node)->tag == VarDclTag || (node)->tag == FnDclTag || (node)->tag == FieldDclTag) {
        return itypeGetTypeDcl(((IExpNode *)node)->vtype);
    }
    else {
        //if (isTypeNode(node)) // caller should be using itypeGetTypeDcl()
        //    return node;
        assert(0 && "Cannot obtain a type from this non-expression node");
        return unknownType;
    }
}

// Return type (or de-referenced type if ptr/ref)
INode *iexpGetDerefTypeDcl(INode *node) {
    return itypeGetDerefTypeDcl(iexpGetTypeDcl(node));
}

// After multiple uses, answer (which starts off NULL)
// will be set to some type if all the nodes agree on it.
// Otherwise, it will be set to void.
void iexpFindSameType(INode **answer, INode *node) {
    INode *nodetype = node && isExpNode(node) ? ((IExpNode*)node)->vtype : unknownType;
    if (*answer == NULL)
        *answer = nodetype;
    else if (!itypeIsSame(*answer, nodetype))
        *answer = unknownType;
}

// Bidirectional type checking between 'from' exp and 'to' type
// Evaluate whether the 'from' node will type check to 'to' type
// - If 'to' type is unspecified, infer it from 'from' type
// - If 'from' node is 'if', 'block', 'loop', set it to 'to' type
// - Otherwise, check that types match.
//   If match requires a coercion, a 'cast' node will be inject ahead of the 'from' node
// return 1 for success, 0 for fail
//
// Note: We need this to be a distinct process from normal TypeCheck
// because how we must do type checking on function call arguments.
// 1) Every node should have TypeCheck done only once because it may mutate (lower)
// 2) Since methods may be overloaded, We non-destructively use type matches to help decide
// 3) Once the best match is decided, we need to perform this step because we need
//    to inject re-casts as low as possible (underneath control flow) on special-typed nodes
int iexpBiTypeInfer(INode **totypep, INode **from) {
    // From should be a typed expression node
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 1;
    }
    IExpNode *fromnode = (IExpNode *)*from;

    // We need special handling for control flow nodes (if, loop, block),
    // if they might have values of inconsistent types,
    // as they may be conduits for multiple branches of values, all of which need to type match.
    // They need to delegate downwards, until we hit a node with a known vtype.
    // We want to push expected type as low down in nodes before forcing any coercion
    // This ensures multi-branch specialized values all upcast to same expected supertype
    if (fromnode->vtype == unknownType) {
        switch (fromnode->tag) {
        case IfTag:
            ifBiTypeInfer(totypep, (IfNode*)*from);
            break;
        case BlockTag:
            blockBiTypeInfer(totypep, (BlockNode*)*from);
            break;
        case LoopTag:
            loopBiTypeInfer(totypep, (LoopNode*)*from);
            break;
        }
    }

    // If 'to' type is unspecified, just use 'from's type
    if (*totypep == NULL) {
        *totypep = fromnode->vtype;
        return 1;
    }

    // Both 'from' and 'to' specify a type. Do they match?
    // (this may involve injecting a 'cast' node in front of 'from'
    INode *totype = *totypep;
    if (totype == unknownType)
        return 1;

    // Re-type a null literal node to match the expected ref/ptr type
    if ((*from)->tag == NullTag) {
        if (!refIsNullable(totype) && totype->tag != PtrTag)
            return 0;
        ((IExpNode*)(*from))->vtype = totype;
        return 1;
    }

    INode *totypedcl = itypeGetTypeDcl(totype);
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

// Perform basic typecheck followed by bi-directional type checking that
// evaluates whether the 'from' node will type check to 'to' type
// - If 'to' type is unspecified, infer it from 'from' type
// - If 'from' node is 'if', 'block', 'loop', set it to 'to' type
// - Otherwise, check that types match.
//   If match requires a coercion, a 'cast' node will be inject ahead of the 'from' node
// return 1 for success, 0 for fail
int iexpTypeCheckAndMatch(TypeCheckState *pstate, INode **totypep, INode **from) {
    inodeTypeCheck(pstate, from);
    return iexpBiTypeInfer(totypep, from);
}

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from) {
    return itypeMatches(iexpGetTypeDcl(to), iexpGetTypeDcl(*from)) == 1;
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
    case FldAccessTag:
    {
        FnCallNode *fncall = (FnCallNode *)node;
        // Field access. Permission is the conjunction of structure and field permissions.
        return iexpGetPermFlags(fncall->objfn) & iexpGetPermFlags((INode*)fncall->methfld);
    }
    default:
        return 0;
    }
}
