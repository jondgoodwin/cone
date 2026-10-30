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

// Clone logic node
INode *cloneLogicNode(CloneState *cstate, LogicNode *node) {
    LogicNode *newnode;
    newnode = memAllocBlk(sizeof(LogicNode));
    memcpy(newnode, node, sizeof(LogicNode));
    newnode->lexp = cloneNode(cstate, node->lexp);
    newnode->rexp = cloneNode(cstate, node->rexp);
    return (INode *)newnode;
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

// Name resolution of not logic node
void logicNotNameRes(NameResState *pstate, LogicNode *node) {
    inodeNameRes(pstate, &node->lexp);
}

// Type check not logic node
void logicNotTypeCheck(TypeCheckState *pstate, LogicNode *node) {
    // '!' takes a condition: an Option or Result converts here, as in an 'if'
    if (0 == iexpTypeCheckCoerceCondition(pstate, &node->lexp))
        errorMsgNode(node->lexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
}

// Name resolution of logic node
void logicNameRes(NameResState *pstate, LogicNode *node) {
    inodeNameRes(pstate, &node->lexp);
    inodeNameRes(pstate, &node->rexp);
}

// An operand of '||' or '&&' that is an Option or Result is refused, on either side,
// until a later PR defines those operators on them: written now, 'opt || flag' would
// change meaning silently. They convert to bool in a condition and under '!' only.
// Answer whether it refused.
static int logicOptionOperand(INode *operand) {
    if (!iexpIsOptionOrResult(operand))
        return 0;
    StructNode *dcl = structVariantEnum(iexpGetDerefTypeDcl(operand));
    if (dcl == NULL)
        dcl = (StructNode*)iexpGetDerefTypeDcl(operand);
    int option = dcl->namesym == optionName;
    errorMsgNode(operand, ErrorOptionLogic,
        "%s cannot be an operand of '||' or '&&' yet: they are not defined on it. It converts to bool in a condition ('if', 'while') and under '!', but here the meaning is not yet decided. Test it with 'x is %s' (or 'x is %s').",
        option ? "An Option" : "A Result", option ? "Some" : "Ok", option ? "None" : "Err");
    return 1;
}

// Type check an operand of '||' or '&&' and coerce it to bool
static int logicOperandCoerce(TypeCheckState *pstate, INode **operand) {
    inodeTypeCheck(pstate, operand, (INode*)boolType);
    if (logicOptionOperand(*operand))
        return 1;
    return iexpCheckedCoerceIn(pstate, (INode*)boolType, operand);
}

// Analyze logic node
void logicTypeCheck(TypeCheckState *pstate, LogicNode *node) {
    if (0 == logicOperandCoerce(pstate, &node->lexp))
        errorMsgNode(node->lexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
    // The right operand runs only when the left is true (for '&&'), or false (for
    // '||'), and what that says of the names it tests is known there. What the right
    // operand changes may or may not happen, so afterwards either path may hold.
    NarrowEnv *left = narrowSave();
    narrowAssumeLocal(node->lexp, node->tag == AndLogicTag);
    if (0 == logicOperandCoerce(pstate, &node->rexp))
        errorMsgNode(node->rexp, ErrorInvType, "Conditional expression must be coercible to boolean value.");
    narrowRestore(narrowJoin(left, narrowSave()));
}
