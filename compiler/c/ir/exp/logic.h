/** Handling for logic nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef logic_h
#define logic_h

// Logic operator: not, or, and
typedef struct LogicNode {
    IExpNodeHdr;
    INode *lexp;
    INode *rexp;
} LogicNode;

LogicNode *newLogicNode(int16_t typ);

// Clone logic node
INode *cloneLogicNode(CloneState *cstate, LogicNode *node);

void logicPrint(LogicNode *node);

// Name resolution of not logic node
void logicNotNameRes(NameResState *pstate, LogicNode *node);

// Type check not logic node
void logicNotTypeCheck(TypeCheckState *pstate, LogicNode *node);

// Name resolution of logic node
void logicNameRes(NameResState *pstate, LogicNode *node);

// Type check logic node
void logicTypeCheck(TypeCheckState *pstate, LogicNode **nodep);

// '||' and '&&' as methods of the left operand's (checked) type. Answers 0 where the left is a
// bool, whose primitives the logic node is; 1 where the node was replaced by the expansion of
// a macro method, the call of a function method, or an error
int fnCallLogicMethod(TypeCheckState *pstate, LogicNode **nodep);

#endif
