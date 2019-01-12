/** Handling for logic nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new logic operator node
LogicNode *newLogicNode(int16_t typ) {
    LogicNode *node;
    newNode(node, LogicNode, typ);
    node->vtype = (INode*)boolType;
    return node;
}

// Serialize logic node
void logicPrint(LogicNode *node) {
    if (node->tag == NotLogicTag) {
        inodeFprint("!");
        inodePrintNode(node->lexp);
    }
    else {
        inodeFprint(node->tag == AndLogicTag? "(&&, " : "(||, ");
        inodePrintNode(node->lexp);
        inodeFprint(", ");
        inodePrintNode(node->rexp);
        inodeFprint(")");
    }
}

// Analyze not logic node
void logicNotPass(PassState *pstate, LogicNode *node) {
    inodeWalk(pstate, &node->lexp);
    if (pstate->pass == TypeCheck)
        if (0 == iexpCoerces((INode*)boolType, &node->lexp))
            errorMsgNode(node->lexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
}

// Analyze logic node
void logicPass(PassState *pstate, LogicNode *node) {
    inodeWalk(pstate, &node->lexp);
    inodeWalk(pstate, &node->rexp);

    if (pstate->pass == TypeCheck) {
        if (0 == iexpCoerces((INode*)boolType, &node->lexp))
            errorMsgNode(node->lexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
        if (0 == iexpCoerces((INode*)boolType, &node->rexp))
            errorMsgNode(node->rexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
    }
}
