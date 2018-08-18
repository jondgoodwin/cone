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
	ITypedNodeHdr;
	INode *lexp;
	INode *rexp;
} LogicNode;

LogicNode *newLogicNode(int16_t typ);
void logicPrint(LogicNode *node);
void logicPass(PassState *pstate, LogicNode *node);
void logicNotPass(PassState *pstate, LogicNode *node);

#endif