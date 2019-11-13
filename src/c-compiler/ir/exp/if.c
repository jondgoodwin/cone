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
    int16_t cnt;
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

// Type check the if statement node
// - Every conditional expression must be a bool
// - else can only be last
// Note: vtype is set to something other than voidType if all branches have the same type
// If they are different, bidirectional type inference will resolve this later
void ifTypeCheck(TypeCheckState *pstate, IfNode *ifnode) {
    INode *sametype = NULL;
    int hasElse = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {

        // Validate that conditional node is correct
        if (*nodesp != voidType) {
            if (0 == iexpTypeCheckAndMatch(pstate, (INode**)&boolType, nodesp))
                errorMsgNode(*nodesp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
        }
        else {
            hasElse = 1;
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
    if (hasElse)
        ifnode->vtype = sametype;
}

// Bidirectional type inference
void IfBiTypeInfer(INode **totypep, IfNode *ifnode) {
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