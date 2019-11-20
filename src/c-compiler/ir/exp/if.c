/** Handling for if nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new If node
IfNode *newIfNode() {
    IfNode *ifnode;
    newNode(ifnode, IfNode, IfTag);
    ifnode->condblk = newNodes(4);
    ifnode->vtype = voidType;
    return ifnode;
}

// Serialize an if statement
void ifPrint(IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    int firstflag = 1;

    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (firstflag) {
            inodeFprint("if ");
            firstflag = 0;
            inodePrintNode(*nodesp);
        }
        else {
            inodePrintIndent();
            if (*nodesp == voidType)
                inodeFprint("else");
            else {
                inodeFprint("elif ");
                inodePrintNode(*nodesp);
            }
        }
        inodePrintNL();
        inodePrintNode(*(++nodesp));
        cnt--;
    }
}

// Recursively strip 'returns' out of all block-ends in 'if' (see returnPass)
void ifRemoveReturns(IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        INode **laststmt;
        cnt--; nodesp++;
        laststmt = &nodesLast(((BlockNode*)*nodesp)->stmts);
        if ((*laststmt)->tag == ReturnTag)
            *laststmt = ((ReturnNode*)*laststmt)->exp;
        if ((*laststmt)->tag == IfTag)
            ifRemoveReturns((IfNode*)*laststmt);
    }
}

// if node name resolution
void ifNameRes(NameResState *pstate, IfNode *ifnode) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        inodeNameRes(pstate, nodesp);
    }
}

// Detect when all closed variants are matched, and turn last match into 'else'
// We know the condition is an 'is' node
void ifExhaustCheck(IfNode *ifnode, CastNode *condition) {
    StructNode *strnode = (StructNode*)iexpGetDerefTypeDcl(condition->exp);

    // Exhaustiveness check only on closed variants against a base trait
    if (strnode->tag != StructTag || !(strnode->flags & TraitType) ||
        strnode->basetrait != NULL || 0 == (strnode->flags & (HasTagField | SameSize)))
        return;

    // We need to detect that all possible variants are accounted for
    uint32_t lowestcnt = ifnode->condblk->used;
    INode **varnodesp;
    uint32_t varcnt;
    for (nodesFor(strnode->derived, varcnt, varnodesp)) {
        // Look for 'is' check on same value against *varnodesp
        int found = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(ifnode->condblk, cnt, nodesp)) {
            if ((*nodesp)->tag == IsTag) {
                CastNode *isnode = (CastNode*)*nodesp;
                if (isnode->exp == condition->exp && itypeGetDerefTypeDcl(isnode->typ) == *varnodesp) {
                    found = 1;
                    if (cnt < lowestcnt)
                        lowestcnt = cnt;
                    break;
                }
            }
            ++nodesp; --cnt;
        }
        if (found == 0)
            return;
    }
    // Turn last 'is' condition into 'else'
    nodesGet(ifnode->condblk, ifnode->condblk->used - lowestcnt) = voidType;
}

// Type check the if statement node
// - Every conditional expression must be a bool
// - else can only be last
// Note: vtype is set to something other than voidType if all branches have the same type
// If they are different, bidirectional type inference will resolve this later
void ifTypeCheck(TypeCheckState *pstate, IfNode *ifnode) {
    INode *sametype = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        // Validate conditional node
        inodeTypeCheck(pstate, nodesp);
        // Detect when all closed variants are matched, and turn last match into 'else'
        if ((*nodesp)->tag == IsTag)
            ifExhaustCheck(ifnode, (CastNode*)*nodesp);

        if (*nodesp != voidType) {
            if (0 == iexpBiTypeInfer((INode**)&boolType, nodesp))
                errorMsgNode(*nodesp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
        }
        else {
            ifnode->flags |= IfHasElse;
            if (cnt > 2) {
                errorMsgNode(*(nodesp + 1), ErrorInvType, "match on everything should be last.");
            }
        }

        ++nodesp; --cnt;
        inodeTypeCheck(pstate, nodesp);
        iexpFindSameType(&sametype, *nodesp);
    }

    // Remember consistently-found type, so long as if-block included an 'else' clause
    // Without an 'else' clause, it won't have returned a value on every path
    if (ifnode->flags & IfHasElse)
        ifnode->vtype = sametype;
}

// Bidirectional type inference
void ifBiTypeInfer(INode **totypep, IfNode *ifnode) {

    // All paths must return a typed value. Absence of an 'else' clause makes that impossible
    if (!(ifnode->flags & IfHasElse)) {
        errorMsgNode((INode*)ifnode, ErrorInvType, "if requires an 'else' clause (or exhaustive matches) to return a value");
        return;
    }

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        ++nodesp; --cnt; // We don't need to look at conditionals, just blocks

        // Validate that all branches have matching types
        if (!iexpBiTypeInfer(totypep, nodesp))
            errorMsgNode(*nodesp, ErrorInvType, "expression type does not match expected type");
    }
    ifnode->vtype = *totypep;
}

// Perform data flow analysis on an if expression
void ifFlow(FlowState *fstate, IfNode **ifnodep) {
    IfNode *ifnode = *ifnodep;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (*nodesp != voidType)
            flowLoadValue(fstate, nodesp);
        nodesp++; cnt--;
        blockFlow(fstate, (BlockNode**)nodesp);
        flowAliasReset();
    }
}