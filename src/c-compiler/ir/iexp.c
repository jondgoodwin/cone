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

// Type check node we expect to be an expression. Return 0 if not.
int iexpTypeCheckAny(TypeCheckState *pstate, INode **from) {
    inodeTypeCheckAny(pstate, from);
    // From should be a typed expression node
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 0;
    }
    return 1;
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
int iexpTypeExpect(INode **totypep, INode **from) {
    // From should be a typed expression node
    assert(isExpNode(*from));
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
    if (*totypep == unknownType) {
        *totypep = fromnode->vtype;
        return 1;
    }

    // Both 'from' and 'to' specify a type. Do they match?
    // (this may involve injecting a 'cast' node in front of 'from'
    INode *totype = *totypep;

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
    switch (itypeMatches(totypedcl, fromtypedcl)) {
    case NoMatch:
        return 0;
    case EqMatch:
        return 1;
    case CoerceMatch:
        *from = (INode*)newCastNode(*from, totypedcl);
        return 1;
    default: {
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
    }
}

// Perform basic typecheck followed by bi-directional type checking that
// evaluates whether the 'from' node will type check to 'to' type
// - If 'to' type is unknownType, infer it from 'from' type
// - If 'from' node is 'if', 'block', 'loop', set it to 'to' type
// - Otherwise, check that types match.
//   If match requires a coercion, a 'cast' node will be inject ahead of the 'from' node
// return 1 for success, 0 for fail
int iexpTypeCheckExpect(TypeCheckState *pstate, INode **totypep, INode **from) {
    inodeTypeCheck(pstate, from, *totypep);
    if (!isExpNode(*from)) {
        errorMsgNode(*from, ErrorNotTyped, "Expected a typed expression.");
        return 1; // pretend we match to not provoke additional errors
    }
    return iexpTypeExpect(totypep, from);
}

// Ensure it is a lval, return error and 0 if not.
int iexpIsLval(INode *lval) {
    switch (lval->tag) {
    case VarNameUseTag:
    case DerefTag:
        return 1;
    case ArrIndexTag:
        return iexpIsLval(((FnCallNode *)lval)->objfn);
    case FldAccessTag:
        return iexpIsLval(((FnCallNode *)lval)->objfn);
    default:
        errorMsgNode(lval, ErrorBadLval, "Expression must be lval");
        return 0;
    }
}

// Extract lval variable, scope and overall permission from lval
INode *iexpGetLvalInfo(INode *lval, INode **lvalperm, uint16_t *scope) {
    switch (lval->tag) {

        // A variable or named function node
    case VarNameUseTag:
    {
        INode *lvalvar = ((NameUseNode *)lval)->dclnode;
        if (lvalvar->tag == VarDclTag) {
            *lvalperm = ((VarDclNode *)lvalvar)->perm;
            *scope = ((VarDclNode *)lvalvar)->scope;
        }
        else {
            *lvalperm = (INode*)opaqPerm; // Function
            *scope = 0;
        }
        return lvalvar;
    }

    case DerefTag:
    {
        INode *lvalvar = iexpGetLvalInfo(((DerefNode *)lval)->exp, lvalperm, scope);
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)lval)->exp);
        if (vtype->tag == RefTag || vtype->tag == ArrayRefTag)
            *lvalperm = vtype->perm;
        else if (vtype->tag == PtrTag)
            *lvalperm = (INode*)mutPerm;
        return lvalvar;
    }

    // Array element (obj[2])
    case ArrIndexTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        // flowLoadValue(fstate, nodesFind(element->args, 0), 0);
        INode *lvalvar = iexpGetLvalInfo(element->objfn, lvalperm, scope);
        INode *objtype = iexpGetTypeDcl(element->objfn);
        if (objtype->tag == ArrayRefTag)
            *lvalperm = ((RefNode*)objtype)->perm;
        else if (objtype->tag == PtrTag)
            *lvalperm = (INode*)mutPerm;
        return lvalvar;
    }

    // Field access (obj.prop)
    case FldAccessTag:
    {
        FnCallNode *element = (FnCallNode *)lval;
        INode *lvalvar = iexpGetLvalInfo(element->objfn, lvalperm, scope);
        if (lvalvar == NULL)
            return NULL;
        RefNode *vtype = (RefNode*)iexpGetTypeDcl(((DerefNode *)lval)->exp);
        if (vtype->tag == VirtRefTag)
            *lvalperm = vtype->perm;
        else {
            PermNode *methperm = (PermNode *)((VarDclNode*)((NameUseNode *)element->methfld)->dclnode)->perm;
            // Downgrade overall static permission if field is immutable
            if (methperm == immPerm)
                *lvalperm = (INode*)constPerm;
        }
        return lvalvar;
    }

    // No other node is an lval
    default:
        return NULL;
    }
}

// Are types the same (no coercion)
int iexpSameType(INode *to, INode **from) {
    return itypeMatches(iexpGetTypeDcl(to), iexpGetTypeDcl(*from)) == EqMatch;
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
