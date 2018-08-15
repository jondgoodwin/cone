/** AST handling for expression nodes that might do copy/move
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef copyexpr_h
#define copyexpr_h

// Variations on assignment
enum AssignType {
	NormalAssign
};

// Assignment node
typedef struct AssignNode {
	ITypedNodeHdr;
	INode *lval;
	INode *rval;
	int16_t assignType;
} AssignNode;

typedef struct AddrNode {
	ITypedNodeHdr;
	INode *exp;
} AddrNode;

AssignNode *newAssignNode(int16_t assigntype, INode *lval, INode *rval);
void assignPrint(AssignNode *node);
void assignPass(PassState *pstate, AssignNode *node);

AddrNode *newAddrNode();
void addrPrint(AddrNode *node);
void addrPass(PassState *pstate, AddrNode *node);

#endif