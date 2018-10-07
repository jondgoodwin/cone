/** Handling for while nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef while_h
#define while_h

// While statement
typedef struct WhileNode {
	INodeHdr;
	INode *condexp;
	INode *blk;
} WhileNode;

WhileNode *newWhileNode();
void whilePrint(WhileNode *wnode);
void whilePass(PassState *pstate, WhileNode *wnode);

// Perform data flow analysis on an while statement
void whileFlow(FlowState *fstate, WhileNode **nodep);

#endif